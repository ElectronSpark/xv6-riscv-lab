/**
 * @file top.c
 * @brief System monitor — per-process CPU%, disk/network throughput.
 *
 * Usage: top [-a] [-n N] [-d SECS]
 *   -a        show all processes (including kernel threads)
 *   -n N      refresh N times then exit (default: 1)
 *   -d SECS   delay between refreshes in seconds (default: 2)
 *
 * On the first invocation (or with -n 1), absolute cumulative values are
 * shown.  With -n >1 the display switches to per-interval deltas/rates
 * starting from the second iteration.
 */
#include "kernel/inc/types.h"
#include "kernel/inc/kstats.h"
#include "user/user.h"

/* ------------------------------------------------------------------ */
/*  linux_dirent64 — needed for getdents()                            */
/* ------------------------------------------------------------------ */
struct linux_dirent64 {
	uint64 d_ino;
	int64  d_off;
	uint16 d_reclen;
	uint8  d_type;
	char   d_name[];
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static int strncmp_local(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		if (a[i] != b[i])
			return (unsigned char)a[i] - (unsigned char)b[i];
		if (a[i] == '\0')
			return 0;
	}
	return 0;
}

static int is_numeric(const char *s)
{
	if (*s == '\0')
		return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

static int parse_u64_field(const char *line, const char *key, uint64 *val)
{
	int klen = strlen(key);
	if (strncmp_local(line, key, klen) != 0)
		return 0;
	const char *p = line + klen;
	while (*p == ':' || *p == '\t' || *p == ' ')
		p++;
	uint64 v = 0;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*val = v;
	return 1;
}

/* ------------------------------------------------------------------ */
/*  Per-process snapshot                                              */
/* ------------------------------------------------------------------ */

#define MAX_PROCS 128

struct proc_snap {
	int    pid;
	int    ppid;
	char   name[17];
	char   state[8];
	uint64 vm_kb;
	uint64 cputime_raw;      /* from /proc/<pid>/status CpuTime (raw ticks) */
	/* from /proc/<pid>/resources */
	uint64 fs_bytes_read;
	uint64 fs_bytes_written;
	uint64 net_bytes_sent;
	uint64 net_bytes_recv;
	uint64 bio_reads;
	uint64 bio_writes;
};

static struct proc_snap snap[2][MAX_PROCS]; /* two snapshot buffers */
static int snap_count[2];                   /* number of procs in each */

static int read_file_into(const char *path, char *buf, int bufsz)
{
	int fd = open(path, 0);
	if (fd < 0)
		return -1;
	int total = 0, n;
	while (total < bufsz - 1 &&
	       (n = read(fd, buf + total, bufsz - 1 - total)) > 0)
		total += n;
	buf[total] = '\0';
	close(fd);
	return total;
}

static int parse_status(int pid, struct proc_snap *ps)
{
	char path[64], buf[512];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if (read_file_into(path, buf, sizeof(buf)) <= 0)
		return -1;

	ps->pid = pid;
	ps->ppid = 0;
	ps->name[0] = '\0';
	ps->state[0] = '\0';
	ps->vm_kb = 0;
	ps->cputime_raw = 0;

	char *p = buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		uint64 tmp;
		if (strncmp_local(p, "Name:\t", 6) == 0) {
			char *src = p + 6;
			int i = 0;
			while (*src && i < 16)
				ps->name[i++] = *src++;
			ps->name[i] = '\0';
		} else if (strncmp_local(p, "State:\t", 7) == 0) {
			char *src = p + 7;
			int i = 0;
			while (*src && i < 7)
				ps->state[i++] = *src++;
			ps->state[i] = '\0';
		} else if (parse_u64_field(p, "Pid", &tmp)) {
			ps->pid = (int)tmp;
		} else if (parse_u64_field(p, "PPid", &tmp)) {
			ps->ppid = (int)tmp;
		} else if (parse_u64_field(p, "VmSize", &tmp)) {
			ps->vm_kb = tmp;
		} else if (parse_u64_field(p, "CpuTime", &tmp)) {
			ps->cputime_raw = tmp;
		}
		if (nl)
			p = nl + 1;
		else
			break;
	}
	return 0;
}

static void parse_resources(int pid, struct proc_snap *ps)
{
	char path[64], buf[2048];
	snprintf(path, sizeof(path), "/proc/%d/resources", pid);
	if (read_file_into(path, buf, sizeof(buf)) <= 0)
		return;

	char *p = buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		parse_u64_field(p, "fs_bytes_read", &ps->fs_bytes_read);
		parse_u64_field(p, "fs_bytes_written", &ps->fs_bytes_written);
		parse_u64_field(p, "net_bytes_sent", &ps->net_bytes_sent);
		parse_u64_field(p, "net_bytes_recv", &ps->net_bytes_recv);
		parse_u64_field(p, "bio_reads", &ps->bio_reads);
		parse_u64_field(p, "bio_writes", &ps->bio_writes);
		if (nl)
			p = nl + 1;
		else
			break;
	}
}

static void scan_procs(int slot)
{
	int count = 0;
	int fd = open("/proc", 0);
	if (fd < 0) {
		printf("top: cannot open /proc\n");
		snap_count[slot] = 0;
		return;
	}
	char dirent_buf[1024];
	int nread;
	while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
		int pos = 0;
		while (pos < nread) {
			struct linux_dirent64 *de =
			    (struct linux_dirent64 *)(dirent_buf + pos);
			if (de->d_ino != 0 && is_numeric(de->d_name)) {
				int pid = atoi(de->d_name);
				if (count < MAX_PROCS) {
					struct proc_snap *ps = &snap[slot][count];
					memset(ps, 0, sizeof(*ps));
					if (parse_status(pid, ps) == 0) {
						parse_resources(pid, ps);
						count++;
					}
				}
			}
			pos += de->d_reclen;
		}
	}
	close(fd);
	snap_count[slot] = count;
}

/* Find a process by PID in the given snapshot slot. Returns NULL if gone. */
static struct proc_snap *find_by_pid(int slot, int pid)
{
	for (int i = 0; i < snap_count[slot]; i++)
		if (snap[slot][i].pid == pid)
			return &snap[slot][i];
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Formatting helpers                                                */
/* ------------------------------------------------------------------ */

static void fmt_bytes(char *buf, int bufsz, uint64 bytes)
{
	if (bytes < 1024)
		snprintf(buf, bufsz, "%d B", (int)bytes);
	else if (bytes < 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d KB", (int)(bytes / 1024),
		         (int)((bytes % 1024) * 10 / 1024));
	else if (bytes < (uint64)1024 * 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d MB",
		         (int)(bytes / (1024 * 1024)),
		         (int)((bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
	else
		snprintf(buf, bufsz, "%d.%d GB",
		         (int)(bytes / (1024ULL * 1024 * 1024)),
		         (int)((bytes % (1024ULL * 1024 * 1024)) * 10 /
		               (1024ULL * 1024 * 1024)));
}

static void fmt_rate(char *buf, int bufsz, uint64 delta_bytes, int secs)
{
	if (secs <= 0)
		secs = 1;
	uint64 rate = delta_bytes / (uint64)secs;
	if (rate < 1024)
		snprintf(buf, bufsz, "%d B/s", (int)rate);
	else if (rate < 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d KB/s", (int)(rate / 1024),
		         (int)((rate % 1024) * 10 / 1024));
	else
		snprintf(buf, bufsz, "%d.%d MB/s",
		         (int)(rate / (1024 * 1024)),
		         (int)((rate % (1024 * 1024)) * 10 / (1024 * 1024)));
}

/* ------------------------------------------------------------------ */
/*  Display                                                           */
/* ------------------------------------------------------------------ */

static void print_header(struct kstats *cur, struct kstats *prev,
                         int interval_secs)
{
	uint64 up_s = cur->uptime_ms / 1000;
	/* Load average 5s (FSHIFT=11 fixed-point → integer.fraction) */
	int load_int  = (int)(cur->load_avg_5s >> 11);
	int load_frac = (int)((cur->load_avg_5s & 0x7ff) * 100 / 2048);

	printf("Uptime: %d:%02d:%02d   CPUs: %d   Load(5s): %d.%02d\n",
	       (int)(up_s / 3600), (int)((up_s / 60) % 60),
	       (int)(up_s % 60), cur->ncpus, load_int, load_frac);

	/* Per-CPU line with 1s EWMA utilization from kernel. */
	printf("CPU  ");
	int total_run = 0;
	for (int i = 0; i < cur->ncpus && i < KSTATS_MAX_CPUS; i++) {
		struct cpu_stat *cs = &cur->cpu[i];
		total_run += cs->nr_running;
		/* util_1s is FSHIFT=11 fixed-point where FIXED_1=100% */
		int util_pct = (int)(cs->util_1s * 100 / 2048);
		if (util_pct > 100) util_pct = 100;
		printf(" [%d:%s r=%d u=%d%%]", i,
		       cs->idle ? "idle" : "busy", (int)cs->nr_running,
		       util_pct);
	}
	printf("  runnable=%d\n", total_run);

	/* Disk */
	char rbuf[32], wbuf[32];
	if (prev) {
		fmt_rate(rbuf, sizeof(rbuf),
		         cur->bio_read_bytes - prev->bio_read_bytes,
		         interval_secs);
		fmt_rate(wbuf, sizeof(wbuf),
		         cur->bio_write_bytes - prev->bio_write_bytes,
		         interval_secs);
	} else {
		fmt_bytes(rbuf, sizeof(rbuf), cur->bio_read_bytes);
		fmt_bytes(wbuf, sizeof(wbuf), cur->bio_write_bytes);
	}
	printf("Disk   rd=%d wr=%d  %s / %s\n",
	       (int)cur->bio_reads, (int)cur->bio_writes, rbuf, wbuf);

	/* Net */
	char txb[32], rxb[32];
	if (prev) {
		fmt_rate(txb, sizeof(txb),
		         cur->net_tx_bytes - prev->net_tx_bytes,
		         interval_secs);
		fmt_rate(rxb, sizeof(rxb),
		         cur->net_rx_bytes - prev->net_rx_bytes,
		         interval_secs);
	} else {
		fmt_bytes(txb, sizeof(txb), cur->net_tx_bytes);
		fmt_bytes(rxb, sizeof(rxb), cur->net_rx_bytes);
	}
	printf("Net    tx=%d rx=%d  %s / %s\n",
	       (int)cur->net_tx_packets, (int)cur->net_rx_packets,
	       txb, rxb);
}

/**
 * Print the process table.
 *
 * @param cur_slot  index into snap[] for the current snapshot
 * @param prev_slot index into snap[] for the prev snapshot (-1 if none)
 * @param show_all  include kernel threads (VmSize==0)
 * @param interval_ms  milliseconds between snapshots (for rate calc)
 */
static void print_procs(int cur_slot, int prev_slot, int show_all,
                        uint64 interval_ms, uint64 interval_raw,
                        uint64 timebase_freq)
{
	int n = snap_count[cur_slot];
	int user_count = 0, kern_count = 0;
	for (int i = 0; i < n; i++) {
		if (snap[cur_slot][i].vm_kb > 0)
			user_count++;
		else
			kern_count++;
	}
	printf("Procs: %d user, %d kernel, %d total\n\n",
	       user_count, kern_count, n);

	int have_prev = (prev_slot >= 0);

	/* Header line */
	if (have_prev)
		printf("%-6s %-16s %-5s %5s %8s %6s %9s %9s %9s %9s\n",
		       "PID", "NAME", "STATE", "VM",
		       "CPU(ms)", "CPU%", "FS_RD/s", "FS_WR/s", "NTX/s", "NRX/s");
	else
		printf("%-6s %-16s %-5s %5s %8s %6s %9s %9s %9s %9s\n",
		       "PID", "NAME", "STATE", "VM",
		       "CPU(ms)", "CPU%", "FS_RD", "FS_WR", "NET_TX", "NET_RX");

	for (int i = 0; i < n; i++) {
		struct proc_snap *cur = &snap[cur_slot][i];

		if (!show_all && cur->vm_kb == 0)
			continue;

		/* CPU% = delta(cputime_raw) / interval_raw * 100
		 * Both numerator and denominator use r_time() ticks,
		 * avoiding drift between r_time() and jiffies clocks.
		 * pct_x10 = tenths of percent (1000 = 100.0%). */
		int cpu_pct_x10 = 0;
		if (have_prev && interval_raw > 0) {
			struct proc_snap *old = find_by_pid(prev_slot,
			                                    cur->pid);
			if (old) {
				uint64 dt_raw = cur->cputime_raw -
				               old->cputime_raw;
				cpu_pct_x10 = (int)(dt_raw * 1000 /
				              interval_raw);
			}
		}

		/* Convert raw ticks to ms for display */
		uint64 cputime_ms = 0;
		if (timebase_freq > 0)
			cputime_ms = cur->cputime_raw * 1000 / timebase_freq;

		if (have_prev) {
			/* Delta mode — compute I/O rates */
			struct proc_snap *old = find_by_pid(prev_slot,
			                                    cur->pid);
			uint64 dt_frd = 0, dt_fwr = 0;
			uint64 dt_ntx = 0, dt_nrx = 0;
			if (old) {
				dt_frd = cur->fs_bytes_read -
				         old->fs_bytes_read;
				dt_fwr = cur->fs_bytes_written -
				         old->fs_bytes_written;
				dt_ntx = cur->net_bytes_sent -
				         old->net_bytes_sent;
				dt_nrx = cur->net_bytes_recv -
				         old->net_bytes_recv;
			} else {
				dt_frd = cur->fs_bytes_read;
				dt_fwr = cur->fs_bytes_written;
				dt_ntx = cur->net_bytes_sent;
				dt_nrx = cur->net_bytes_recv;
			}

			int interval_secs = (int)(interval_ms / 1000);
			if (interval_secs < 1)
				interval_secs = 1;

			char frd[16], fwr[16], ntx[16], nrx[16];
			fmt_rate(frd, sizeof(frd), dt_frd, interval_secs);
			fmt_rate(fwr, sizeof(fwr), dt_fwr, interval_secs);
			fmt_rate(ntx, sizeof(ntx), dt_ntx, interval_secs);
			fmt_rate(nrx, sizeof(nrx), dt_nrx, interval_secs);

			printf("%-6d %-16s %-5s %5d %8d %3d.%d %9s %9s %9s %9s\n",
			       cur->pid, cur->name, cur->state,
			       (int)cur->vm_kb, (int)cputime_ms,
			       cpu_pct_x10 / 10, cpu_pct_x10 % 10,
			       frd, fwr, ntx, nrx);
		} else {
			/* Absolute mode — show cumulative I/O values */
			char frd[16], fwr[16], ntx[16], nrx[16];
			fmt_bytes(frd, sizeof(frd), cur->fs_bytes_read);
			fmt_bytes(fwr, sizeof(fwr), cur->fs_bytes_written);
			fmt_bytes(ntx, sizeof(ntx), cur->net_bytes_sent);
			fmt_bytes(nrx, sizeof(nrx), cur->net_bytes_recv);

			printf("%-6d %-16s %-5s %5d %8d %3d.%d %9s %9s %9s %9s\n",
			       cur->pid, cur->name, cur->state,
			       (int)cur->vm_kb, (int)cputime_ms,
			       cpu_pct_x10 / 10, cpu_pct_x10 % 10,
			       frd, fwr, ntx, nrx);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("Usage: top [-a] [-n N] [-d SECS]\n");
	printf("  -a        show all (including kernel threads)\n");
	printf("  -n N      refresh N times then exit (default: 1)\n");
	printf("  -d SECS   delay between refreshes (default: 2)\n");
}

int main(int argc, char *argv[])
{
	int show_all = 0;
	int iterations = 1;
	int delay_secs = 2;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-a") == 0) {
			show_all = 1;
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			iterations = atoi(argv[++i]);
			if (iterations < 1)
				iterations = 1;
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			delay_secs = atoi(argv[++i]);
			if (delay_secs < 1)
				delay_secs = 1;
		} else if (strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		} else {
			printf("top: unknown option '%s'\n", argv[i]);
			usage();
			return 1;
		}
	}

	struct kstats ks[2]; /* two kstats buffers for delta */
	int cur = 0;         /* current slot index (alternates 0/1) */

	for (int iter = 0; iter < iterations; iter++) {
		int prev = -1;

		if (iter == 0) {
			/* First iteration: take two snapshots with a 1s
			 * sleep to compute meaningful CPU% deltas
			 * (Linux approach). */
			if (kstats(&ks[0]) < 0) {
				printf("top: kstats failed\n");
				return 1;
			}
			scan_procs(0);
			sleep(1000); /* 1 second baseline */
			if (kstats(&ks[1]) < 0) {
				printf("top: kstats failed\n");
				return 1;
			}
			scan_procs(1);
			prev = 0;
			cur = 1;
		} else {
			/* Subsequent: we already have the previous in cur */
			prev = cur;
			cur = 1 - cur; /* flip to other slot */
			if (kstats(&ks[cur]) < 0) {
				printf("top: kstats failed\n");
				return 1;
			}
			scan_procs(cur);
		}

		if (iter > 0)
			printf("\n--- refresh %d/%d ---\n\n", iter + 1,
			       iterations);

		uint64 interval_ms = 0;
		uint64 interval_raw = 0;
		if (prev >= 0) {
			interval_ms = ks[cur].uptime_ms - ks[prev].uptime_ms;
			interval_raw = ks[cur].timestamp - ks[prev].timestamp;
		} else {
			interval_ms = ks[cur].uptime_ms;
			interval_raw = ks[cur].timestamp;
		}

		print_header(&ks[cur],
		             (prev >= 0) ? &ks[prev] : 0,
		             (int)(interval_ms / 1000));
		printf("\n");
		print_procs(cur, prev, show_all, interval_ms, interval_raw,
		            ks[cur].timebase_freq);

		if (iter + 1 < iterations)
			sleep(delay_secs * 1000); /* sleep(ms) */
	}

	return 0;
}
