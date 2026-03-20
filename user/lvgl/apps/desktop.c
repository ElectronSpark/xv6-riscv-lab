/*
 * desktop.c — xv6 Desktop Environment
 *
 * A graphical desktop with taskbar, draggable windows, and built-in
 * applications: System Info, Calculator, Settings, About.
 *
 * Usage:  $ desktop
 * Exit:   Click the power button on the taskbar, or Ctrl+C
 */

#include "../lvgl_xv6.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/time.h>

/* ── xv6-specific syscall numbers (not exposed by musl) ──────────── */
#define XV6_SYS_memstat   90
#define XV6_SYS_kstats    160
#define XV6_SYS_poweroff  166
#define XV6_SYS_reboot    873

/* ── Kernel statistics structures (must match kernel layout) ─────── */
#define GUI_NCPU 8

struct gui_cpu_stat {
    uint32_t nr_running;
    uint32_t idle;
    uint64_t load_avg;
    uint64_t cpu_util;
    uint64_t cpu_load;
    uint64_t busy_ticks;
    uint64_t total_ticks;
    uint64_t util_1s;
};

struct gui_kstats {
    uint64_t uptime_ms;
    int      ncpus;
    uint64_t timebase_freq;
    uint64_t timestamp;
    uint64_t load_avg_1s;
    uint64_t load_avg_5s;
    uint64_t load_avg_16s;
    struct gui_cpu_stat cpu[GUI_NCPU];
    uint64_t bio_reads;
    uint64_t bio_writes;
    uint64_t bio_read_bytes;
    uint64_t bio_write_bytes;
    uint64_t net_tx_packets;
    uint64_t net_tx_bytes;
    uint64_t net_rx_packets;
    uint64_t net_rx_bytes;
    uint64_t _pad[69];  /* remaining kernel fields we don't access */
};

struct gui_netconf_req {
    int          mode;
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int dns;
    char         hostname[32];
};

#define SIOCNETCONF_GET 0x89F1
#define SIOCNETCONF     0x89F0

/* ── xv6 raw syscall helper (bypasses musl for xv6-specific calls) ─ */
static long xv6_syscall0(long num) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static long xv6_syscall1(long num, long a1) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static int gui_kstats_get(struct gui_kstats *ks) {
    return (int)xv6_syscall1(XV6_SYS_kstats, (long)ks);
}

static uint64_t gui_memstat_free(void) {
    return (uint64_t)xv6_syscall1(XV6_SYS_memstat, 0x10);
}

static uint64_t gui_memstat_used(void) {
    return (uint64_t)xv6_syscall1(XV6_SYS_memstat, 0x20);
}

/* ── Helper utilities ────────────────────────────────────────────── */
static int gui_read_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
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

static void format_size(uint64_t bytes, char *out, int outsz) {
    if (bytes >= (uint64_t)1073741824)
        snprintf(out, outsz, "%lu GB", (unsigned long)(bytes / 1073741824));
    else if (bytes >= 1048576)
        snprintf(out, outsz, "%lu MB", (unsigned long)(bytes / 1048576));
    else if (bytes >= 1024)
        snprintf(out, outsz, "%lu KB", (unsigned long)(bytes / 1024));
    else
        snprintf(out, outsz, "%lu B", (unsigned long)bytes);
}

static void format_uptime(uint64_t ms, char *out, int outsz) {
    unsigned long s = (unsigned long)(ms / 1000);
    unsigned long m = s / 60; s %= 60;
    unsigned long h = m / 60; m %= 60;
    unsigned long d = h / 24; h %= 24;
    if (d > 0)
        snprintf(out, outsz, "%lud %luh %lum", d, h, m);
    else if (h > 0)
        snprintf(out, outsz, "%luh %lum %lus", h, m, s);
    else
        snprintf(out, outsz, "%lum %lus", m, s);
}

static void format_ipv4(unsigned int ip, char *out, int outsz) {
    unsigned char *b = (unsigned char *)&ip;
    snprintf(out, outsz, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

static int parse_proc_field_u64(const char *buf, const char *key,
                                uint64_t *val)
{
    const char *p = buf;
    int klen = (int)strlen(key);
    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            const char *q = p + klen;
            while (*q == ':' || *q == '\t' || *q == ' ') q++;
            uint64_t v = 0;
            while (*q >= '0' && *q <= '9')
                v = v * 10 + (uint64_t)(*q++ - '0');
            *val = v;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

static int parse_proc_field_str(const char *buf, const char *key,
                                char *out, int outsz)
{
    const char *p = buf;
    int klen = (int)strlen(key);
    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            const char *q = p + klen;
            while (*q == ':' || *q == '\t' || *q == ' ') q++;
            int i = 0;
            while (*q && *q != '\n' && i < outsz - 1)
                out[i++] = *q++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

static volatile int g_running = 1;

/* ── Window tracking ─────────────────────────────────────────────── */

static lv_obj_t *win_sysinfo;
static lv_obj_t *win_calc;
static lv_obj_t *win_settings;
static lv_obj_t *win_about;
static lv_obj_t *win_procmgr;
static lv_obj_t *win_sysmon;
static lv_obj_t *win_filemgr;
static lv_obj_t *win_netinfo;
static lv_obj_t *win_power;
static lv_obj_t *win_usermgr;
static lv_obj_t *win_netcfg;
static lv_obj_t *win_demo3d;

/* Resolution confirmation dialog */
static lv_obj_t *win_res_confirm;
static lv_obj_t *res_confirm_lbl;
static volatile int res_confirm_keep;
static volatile int res_confirm_revert;
static uint32_t    res_old_w, res_old_h;
static int         res_countdown;       /* seconds remaining */
static int         res_countdown_ctr;   /* frame counter (60fps) */

/* Updatable labels for live-data windows */
static lv_obj_t *sysinfo_data_lbl;
static lv_obj_t *procmgr_list_lbl;
static lv_obj_t *sysmon_info_lbl;
static lv_obj_t *filemgr_path_lbl;
static lv_obj_t *filemgr_list_lbl;
static lv_obj_t *netinfo_lbl;
static lv_obj_t *power_info_lbl;
static lv_obj_t *usermgr_info_lbl;
static lv_obj_t *netcfg_status_lbl;
static lv_obj_t *netcfg_ip_tb;
static lv_obj_t *netcfg_mask_tb;
static lv_obj_t *netcfg_gw_tb;
static lv_obj_t *netcfg_dns_tb;
static lv_obj_t *netcfg_mode_cb;

/* Clock label on taskbar */
static lv_obj_t *g_clock_lbl;
static int g_clock_refresh_ctr;

static int netcfg_mode = 0; /* 0=DHCP, 1=Static */

/* File manager state */
static char filemgr_cwd[256] = "/";
static lv_obj_t *filemgr_scroll;  /* scrollable sub-container */

#define FILEMGR_MAX_ENTRIES 64
static struct {
    char name[64];
    int  is_dir;
} filemgr_entries[FILEMGR_MAX_ENTRIES];
static int filemgr_entry_count;
static int filemgr_selected = -1;          /* selected entry index */
static uint32_t filemgr_last_click_frame;  /* frame of last click for double-click */
static int filemgr_last_click_line = -1;   /* entry clicked last time */

/* System monitor auto-refresh */
static int sysmon_refresh_ctr;

/* ── Terminal instances ──────────────────────────────────────────── */

#define TERM_COLS      80
#define TERM_ROWS      24
#define TERM_ESC_MAX   32
#define TERM_MAX       4       /* max simultaneous terminals */

/* Per-cell attribute bits */
#define TATTR_REVERSE  0x01
#define TATTR_BOLD     0x02

typedef struct {
    int         active;            /* slot in use */
    lv_obj_t   *win;              /* window widget */
    lv_obj_t   *label;            /* text label */
    lv_obj_t   *tb_btn;           /* taskbar button for this terminal */
    int         master_fd;        /* PTY master fd */
    int         shell_pid;        /* child shell PID */
    char        buf[TERM_ROWS][TERM_COLS];
    uint8_t     attr[TERM_ROWS][TERM_COLS];  /* per-cell SGR attributes */
    uint8_t     cur_attr;                     /* current SGR state */
    int         row, col;
    int         saved_row, saved_col;           /* ESC 7/8 saved cursor */
    int         esc_state;        /* 0=normal, 1=ESC, 2=CSI */
    char        esc_buf[TERM_ESC_MAX];
    int         esc_idx;
    int         dirty;
    int         id;               /* terminal number (1-based) */
} term_instance_t;

static term_instance_t g_terms[TERM_MAX];
static int             g_term_focus = -1;   /* index of focused terminal, -1 = none */
static int             g_term_next_id = 1;  /* monotonically increasing ID */

/* ── Taskbar globals (for open-app buttons) ──────────────────────── */
static lv_obj_t *g_taskbar;                 /* taskbar container */
static int16_t   g_tb_app_x0;              /* x start of open-app area */
static int16_t   g_tb_app_btn_w = 80;      /* width of each app button */

/* Menu popup (shown/hidden on Menu button click) */
static lv_obj_t *g_menu_popup = NULL;

/* Dynamic taskbar buttons for open windows */
#define DYN_TB_MAX 16
static struct {
    lv_obj_t  *btn;         /* taskbar button widget */
    lv_obj_t **win_ptr;     /* pointer to corresponding window variable */
} g_dyn_tb[DYN_TB_MAX];

/* ── 3D Demo state ───────────────────────────────────────────────── */

#define D3D_W   280
#define D3D_H   220
static uint32_t *d3d_buf;     /* pixel buffer D3D_W * D3D_H */
static int       d3d_angle_y; /* Y-axis rotation (0..255 = full circle) */
static int       d3d_angle_x; /* X-axis rotation (0..255 = full circle) */
static int       d3d_angle_z; /* Z-axis rotation (0..255 = full circle) */

/* ── Calculator state ────────────────────────────────────────────── */

static int32_t    calc_display_val;
static int32_t    calc_accumulator;
static char       calc_pending_op;
static int        calc_new_input;
static lv_obj_t  *calc_display_lbl;

/* ── Settings state ──────────────────────────────────────────────── */

static lv_obj_t *settings_r_slider;
static lv_obj_t *settings_g_slider;
static lv_obj_t *settings_b_slider;
static lv_obj_t *settings_r_lbl;
static lv_obj_t *settings_g_lbl;
static lv_obj_t *settings_b_lbl;
static lv_obj_t *settings_preview;

static void sighandler(int sig) { (void)sig; g_running = 0; }

/* Generic flag-setter callback: sets *(int*)user_data = 1 on click */
static void flag_btn_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    volatile int *flag = (volatile int *)obj->user_data;
    if (flag) *flag = 1;
}

/* ── Forward declarations ────────────────────────────────────────── */

static void create_sysinfo(void);
static void create_calc(void);
static void create_settings(void);
static void create_about(void);
static void create_terminal(void);
static void create_procmgr(void);
static void create_sysmon(void);
static void create_filemgr(void);
static void create_netinfo(void);
static void create_power(void);
static void create_usermgr(void);
static void create_netcfg(void);
static void create_demo3d(void);
static void term_poll_all(void);
static term_instance_t *term_find_by_win(lv_obj_t *win);
static void term_focus(int idx);
static void on_terminal_event(lv_obj_t *obj, lv_event_t e);
static void on_terminal_close(lv_obj_t *obj, lv_event_t e);
static void tb_relayout_app_btns(void);
static void tb_app_btn_cb(lv_obj_t *obj, lv_event_t e);
static void tb_add_dyn_btn(lv_obj_t **win_ptr, const char *label);
static void tb_remove_dyn_btn(lv_obj_t **win_ptr);
static void tb_dyn_btn_cb(lv_obj_t *obj, lv_event_t e);
static void menu_btn_cb(lv_obj_t *obj, lv_event_t e);
static void term_kbd_cb(lv_kbd_event_t *ev, void *user_data);
static void sysinfo_refresh(void);
static void procmgr_refresh(void);
static void sysmon_refresh(void);
static void filemgr_refresh(void);
static void netinfo_refresh(void);
static void power_refresh(void);
static void usermgr_refresh(void);

/* ══════════════════════════════════════════════════════════════════════
 *  Window close callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void on_sysinfo_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_sysinfo);
    lv_obj_del(obj);
    win_sysinfo = NULL;
    sysinfo_data_lbl = NULL;
}

static void on_calc_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_calc);
    lv_obj_del(obj);
    win_calc = NULL;
    calc_display_lbl = NULL;
}

static void on_settings_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_settings);
    lv_obj_del(obj);
    win_settings = NULL;
    settings_r_slider = NULL;
    settings_g_slider = NULL;
    settings_b_slider = NULL;
    settings_r_lbl = NULL;
    settings_g_lbl = NULL;
    settings_b_lbl = NULL;
    settings_preview = NULL;
}

static void on_about_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_about);
    lv_obj_del(obj);
    win_about = NULL;
}

static void on_procmgr_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_procmgr);
    lv_obj_del(obj);
    win_procmgr = NULL;
    procmgr_list_lbl = NULL;
}

static void on_sysmon_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_sysmon);
    lv_obj_del(obj);
    win_sysmon = NULL;
    sysmon_info_lbl = NULL;
}

static void on_filemgr_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_filemgr);
    lv_obj_del(obj);
    win_filemgr = NULL;
    filemgr_path_lbl = NULL;
    filemgr_list_lbl = NULL;
    filemgr_scroll = NULL;
    filemgr_entry_count = 0;
    filemgr_selected = -1;
    filemgr_last_click_line = -1;
}

static void on_netinfo_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_netinfo);
    lv_obj_del(obj);
    win_netinfo = NULL;
    netinfo_lbl = NULL;
}

static void on_power_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_power);
    lv_obj_del(obj);
    win_power = NULL;
    power_info_lbl = NULL;
}

static void on_usermgr_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_usermgr);
    lv_obj_del(obj);
    win_usermgr = NULL;
    usermgr_info_lbl = NULL;
}

static void on_netcfg_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_netcfg);
    lv_obj_del(obj);
    win_netcfg = NULL;
    netcfg_status_lbl = NULL;
    netcfg_ip_tb = NULL;
    netcfg_mask_tb = NULL;
    netcfg_gw_tb = NULL;
    netcfg_dns_tb = NULL;
    netcfg_mode_cb = NULL;
}

static void on_demo3d_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    tb_remove_dyn_btn(&win_demo3d);
    lv_obj_del(obj);
    win_demo3d = NULL;
    if (d3d_buf) { free(d3d_buf); d3d_buf = NULL; }
}

static void on_terminal_event(lv_obj_t *obj, lv_event_t e)
{
    if (e == LV_EVENT_CLOSE) {
        on_terminal_close(obj, e);
        return;
    }
    if (e == LV_EVENT_PRESSED) {
        term_instance_t *t = term_find_by_win(obj);
        if (!t) return;
        int idx = (int)(t - g_terms);
        if (g_term_focus != idx)
            term_focus(idx);
    }
}

static void on_terminal_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    term_instance_t *t = term_find_by_win(obj);
    if (!t) return;

    /* If this was the focused terminal, unfocus */
    int idx = (int)(t - g_terms);
    if (g_term_focus == idx) {
        g_term_focus = -1;
        lv_indev_set_kbd_callback(NULL, NULL);
    }

    /* Kill shell and close PTY */
    if (t->shell_pid > 0) {
        kill(t->shell_pid, SIGKILL);
        waitpid(t->shell_pid, NULL, WNOHANG);
    }
    if (t->master_fd >= 0)
        close(t->master_fd);

    /* Remove taskbar button */
    if (t->tb_btn) {
        lv_obj_del(t->tb_btn);
        t->tb_btn = NULL;
    }

    lv_obj_del(obj);
    memset(t, 0, sizeof(*t));
    t->master_fd = -1;
    t->shell_pid = -1;

    tb_relayout_app_btns();
}

/* ══════════════════════════════════════════════════════════════════════
 *  Taskbar button callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void btn_sysinfo_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_sysinfo) { on_sysinfo_close(win_sysinfo, LV_EVENT_CLOSE); }
    else create_sysinfo();
}

static void btn_calc_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_calc) { on_calc_close(win_calc, LV_EVENT_CLOSE); }
    else create_calc();
}

static void btn_settings_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_settings) { on_settings_close(win_settings, LV_EVENT_CLOSE); }
    else create_settings();
}

static void btn_about_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_about) { on_about_close(win_about, LV_EVENT_CLOSE); }
    else create_about();
}

static void btn_terminal_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    create_terminal();
}

static void btn_logout_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    g_running = 0;
}

static void btn_procmgr_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_procmgr) { on_procmgr_close(win_procmgr, LV_EVENT_CLOSE); }
    else create_procmgr();
}

static void btn_sysmon_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_sysmon) { on_sysmon_close(win_sysmon, LV_EVENT_CLOSE); }
    else create_sysmon();
}

static void btn_filemgr_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_filemgr) { on_filemgr_close(win_filemgr, LV_EVENT_CLOSE); }
    else create_filemgr();
}

static void btn_netinfo_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_netinfo) { on_netinfo_close(win_netinfo, LV_EVENT_CLOSE); }
    else create_netinfo();
}

static void btn_power_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_power) { on_power_close(win_power, LV_EVENT_CLOSE); }
    else create_power();
}

static void btn_usermgr_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_usermgr) { on_usermgr_close(win_usermgr, LV_EVENT_CLOSE); }
    else create_usermgr();
}

static void btn_netcfg_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_netcfg) { on_netcfg_close(win_netcfg, LV_EVENT_CLOSE); }
    else create_netcfg();
}

static void btn_demo3d_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_demo3d) { on_demo3d_close(win_demo3d, LV_EVENT_CLOSE); }
    else create_demo3d();
}

/* ══════════════════════════════════════════════════════════════════════
 *  System Info Window (live kernel data via uname, kstats, memstat)
 * ══════════════════════════════════════════════════════════════════════ */

static void sysinfo_refresh(void)
{
    if (!sysinfo_data_lbl) return;

    static char info[1024];
    int pos = 0;
    lv_disp_t *d = lv_disp_get();

    /* uname() for OS info */
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    pos += snprintf(info + pos, sizeof(info) - pos,
        "Operating System\n"
        "  Name:    %s\n"
        "  Release: %s\n"
        "  Version: %s\n"
        "  Machine: %s\n"
        "  Host:    %s\n\n",
        uts.sysname, uts.release, uts.version,
        uts.machine, uts.nodename);

    /* kstats() for uptime, CPUs, load */
    struct gui_kstats ks;
    memset(&ks, 0, sizeof(ks));
    gui_kstats_get(&ks);

    char uptime_str[64];
    format_uptime(ks.uptime_ms, uptime_str, sizeof(uptime_str));

    pos += snprintf(info + pos, sizeof(info) - pos,
        "System\n"
        "  Uptime:  %s\n"
        "  CPUs:    %d\n"
        "  Load:    %lu.%02lu  %lu.%02lu  %lu.%02lu\n\n",
        uptime_str, ks.ncpus,
        (unsigned long)(ks.load_avg_1s >> 11),
        (unsigned long)((ks.load_avg_1s & 0x7FF) * 100 / 2048),
        (unsigned long)(ks.load_avg_5s >> 11),
        (unsigned long)((ks.load_avg_5s & 0x7FF) * 100 / 2048),
        (unsigned long)(ks.load_avg_16s >> 11),
        (unsigned long)((ks.load_avg_16s & 0x7FF) * 100 / 2048));

    /* memstat() for memory info */
    uint64_t mem_free = gui_memstat_free();
    uint64_t mem_used = gui_memstat_used();
    char free_str[32], used_str[32], total_str[32];
    format_size(mem_free, free_str, sizeof(free_str));
    format_size(mem_used, used_str, sizeof(used_str));
    format_size(mem_free + mem_used, total_str, sizeof(total_str));

    pos += snprintf(info + pos, sizeof(info) - pos,
        "Memory\n"
        "  Total:   %s\n"
        "  Used:    %s\n"
        "  Free:    %s\n\n",
        total_str, used_str, free_str);

    /* Display info */
    pos += snprintf(info + pos, sizeof(info) - pos,
        "Display\n"
        "  Resolution: %ux%ux%u\n"
        "  GPU accel:  %s\n"
        "  Libc:       musl",
        d->width, d->height, d->bpp,
        d->gpu_accel ? "yes" : "no");

    lv_label_set_text(sysinfo_data_lbl, info);
}

static void sysinfo_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    sysinfo_refresh();
}

static void create_sysinfo(void)
{
    win_sysinfo = lv_win_create(lv_scr_act());
    lv_win_set_title(win_sysinfo, "System Information");
    lv_obj_set_pos(win_sysinfo, 80, 40);
    lv_obj_set_size(win_sysinfo, 360, 400);
    lv_obj_add_event_cb(win_sysinfo, on_sysinfo_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_sysinfo);

    /* Toolbar at top */
    lv_obj_t *rb = lv_btn_create(c);
    lv_btn_set_text(rb, "Refresh");
    lv_obj_set_pos(rb, 0, 0);
    lv_obj_set_size(rb, 80, 28);
    lv_obj_set_style_bg_color(rb, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(rb, sysinfo_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable content area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 342, 323);
    lv_obj_set_style_bg_color(sc, lv_color_make(50, 50, 55));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    sysinfo_data_lbl = lv_label_create(sc);
    lv_obj_set_pos(sysinfo_data_lbl, 0, 0);
    lv_obj_set_style_text_color(sysinfo_data_lbl,
                                lv_color_make(200, 200, 210));
    lv_label_set_text(sysinfo_data_lbl, "Loading...");

    sysinfo_refresh();
    tb_add_dyn_btn(&win_sysinfo, "Info");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Calculator
 * ══════════════════════════════════════════════════════════════════════ */

static void calc_update_display(void)
{
    if (calc_display_lbl)
        lv_label_set_text_fmt(calc_display_lbl, "%d", (int)calc_display_val);
}

static int32_t calc_apply_op(int32_t a, char op, int32_t b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b != 0 ? a / b : 0;
    default:  return b;
    }
}

static void calc_digit_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    int digit = (int)(intptr_t)obj->user_data;

    if (calc_new_input) {
        calc_display_val = digit;
        calc_new_input = 0;
    } else {
        if (calc_display_val > -100000000 && calc_display_val < 100000000)
            calc_display_val = calc_display_val * 10 +
                (calc_display_val >= 0 ? digit : -digit);
    }
    calc_update_display();
}

static void calc_op_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    char op = (char)(intptr_t)obj->user_data;

    if (!calc_new_input) {
        calc_accumulator = calc_apply_op(calc_accumulator, calc_pending_op,
                                          calc_display_val);
        calc_display_val = calc_accumulator;
        calc_update_display();
    }
    calc_pending_op = op;
    calc_new_input = 1;
}

static void calc_eq_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (calc_pending_op) {
        calc_accumulator = calc_apply_op(calc_accumulator, calc_pending_op,
                                          calc_display_val);
        calc_display_val = calc_accumulator;
        calc_pending_op = 0;
        calc_new_input = 1;
        calc_update_display();
    }
}

static void calc_clear_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    calc_display_val = 0;
    calc_accumulator = 0;
    calc_pending_op = 0;
    calc_new_input = 1;
    calc_update_display();
}

static lv_obj_t *make_calc_btn(lv_obj_t *parent, const char *text,
                                int16_t x, int16_t y, int16_t w, int16_t h,
                                lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_btn_set_text(b, text);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    return b;
}

static void create_calc(void)
{
    calc_display_val = 0;
    calc_accumulator = 0;
    calc_pending_op = 0;
    calc_new_input = 1;

    win_calc = lv_win_create(lv_scr_act());
    lv_win_set_title(win_calc, "Calculator");
    lv_obj_set_pos(win_calc, 460, 40);
    lv_obj_set_size(win_calc, 232, 270);
    lv_obj_add_event_cb(win_calc, on_calc_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_calc);

    /* Display */
    lv_obj_t *disp_bg = lv_obj_create(c);
    lv_obj_set_pos(disp_bg, 0, 0);
    lv_obj_set_size(disp_bg, 200, 32);
    lv_obj_set_style_bg_color(disp_bg, lv_color_make(30, 30, 35));
    lv_obj_set_style_border_width(disp_bg, 1);
    lv_obj_set_style_border_color(disp_bg, LV_COLOR_GRAY);

    calc_display_lbl = lv_label_create(disp_bg);
    lv_label_set_text(calc_display_lbl, "0");
    lv_obj_set_pos(calc_display_lbl, 4, 8);

    /* Button grid: 4 columns x 4 rows */
    int16_t bw = 46, bh = 32, gap = 4;
    int16_t x0 = 0, y0 = 40;
    lv_obj_t *b;

    /* Row 1: 7 8 9 / */
    make_calc_btn(c, "7", x0, y0, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)7);
    make_calc_btn(c, "8", (int16_t)(x0+bw+gap), y0, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)8);
    make_calc_btn(c, "9", (int16_t)(x0+2*(bw+gap)), y0, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)9);
    b = make_calc_btn(c, "/", (int16_t)(x0+3*(bw+gap)), y0, bw, bh,
                  calc_op_cb, (void*)(intptr_t)'/');
    lv_obj_set_style_bg_color(b, lv_color_make(80, 80, 90));

    /* Row 2: 4 5 6 * */
    int16_t y1 = (int16_t)(y0 + bh + gap);
    make_calc_btn(c, "4", x0, y1, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)4);
    make_calc_btn(c, "5", (int16_t)(x0+bw+gap), y1, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)5);
    make_calc_btn(c, "6", (int16_t)(x0+2*(bw+gap)), y1, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)6);
    b = make_calc_btn(c, "*", (int16_t)(x0+3*(bw+gap)), y1, bw, bh,
                  calc_op_cb, (void*)(intptr_t)'*');
    lv_obj_set_style_bg_color(b, lv_color_make(80, 80, 90));

    /* Row 3: 1 2 3 - */
    int16_t y2 = (int16_t)(y0 + 2*(bh + gap));
    make_calc_btn(c, "1", x0, y2, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)1);
    make_calc_btn(c, "2", (int16_t)(x0+bw+gap), y2, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)2);
    make_calc_btn(c, "3", (int16_t)(x0+2*(bw+gap)), y2, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)3);
    b = make_calc_btn(c, "-", (int16_t)(x0+3*(bw+gap)), y2, bw, bh,
                  calc_op_cb, (void*)(intptr_t)'-');
    lv_obj_set_style_bg_color(b, lv_color_make(80, 80, 90));

    /* Row 4: C 0 = + */
    int16_t y3 = (int16_t)(y0 + 3*(bh + gap));
    b = make_calc_btn(c, "C", x0, y3, bw, bh, calc_clear_cb, NULL);
    lv_obj_set_style_bg_color(b, lv_color_make(200, 80, 50));

    make_calc_btn(c, "0", (int16_t)(x0+bw+gap), y3, bw, bh,
                  calc_digit_cb, (void*)(intptr_t)0);

    b = make_calc_btn(c, "=", (int16_t)(x0+2*(bw+gap)), y3, bw, bh,
                  calc_eq_cb, NULL);
    lv_obj_set_style_bg_color(b, lv_color_make(0, 150, 80));

    b = make_calc_btn(c, "+", (int16_t)(x0+3*(bw+gap)), y3, bw, bh,
                  calc_op_cb, (void*)(intptr_t)'+');
    lv_obj_set_style_bg_color(b, lv_color_make(80, 80, 90));
    tb_add_dyn_btn(&win_calc, "Calc");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Settings Window
 * ══════════════════════════════════════════════════════════════════════ */

static void settings_color_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_VALUE_CHANGED) return;
    if (!settings_r_slider || !settings_g_slider || !settings_b_slider)
        return;

    int r = lv_slider_get_value(settings_r_slider);
    int g = lv_slider_get_value(settings_g_slider);
    int b = lv_slider_get_value(settings_b_slider);

    if (settings_r_lbl) lv_label_set_text_fmt(settings_r_lbl, "R: %d", r);
    if (settings_g_lbl) lv_label_set_text_fmt(settings_g_lbl, "G: %d", g);
    if (settings_b_lbl) lv_label_set_text_fmt(settings_b_lbl, "B: %d", b);

    lv_color_t col = lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
    if (settings_preview)
        lv_obj_set_style_bg_color(settings_preview, col);

    /* Update desktop background */
    lv_obj_set_style_bg_color(lv_scr_act(), col);
}

static void settings_reset_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (settings_r_slider) lv_slider_set_value(settings_r_slider, 26);
    if (settings_g_slider) lv_slider_set_value(settings_g_slider, 58);
    if (settings_b_slider) lv_slider_set_value(settings_b_slider, 92);
    settings_color_cb(NULL, LV_EVENT_VALUE_CHANGED);
}

/* ── Resolution change ───────────────────────────────────────────── */
#define FBIOPUT_VSCREENINFO  0x4601
struct resolution_opt { uint32_t w; uint32_t h; };
static const struct resolution_opt res_options[] = {
    {  640,  480 },
    {  800,  600 },
    { 1024,  768 },
    { 1280,  720 },
    { 1280, 1024 },
    { 1440,  900 },
    { 1600, 1200 },
    { 1920, 1080 },
};
#define NUM_RES_OPTIONS (int)(sizeof(res_options)/sizeof(res_options[0]))

static lv_obj_t *settings_res_lbl;

static void settings_res_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)obj->user_data;
    if (idx < 0 || idx >= NUM_RES_OPTIONS) return;

    uint32_t nw = res_options[idx].w;
    uint32_t nh = res_options[idx].h;

    /* Check if this is already the current resolution */
    lv_disp_t *d = lv_disp_get();
    if (d->width == nw && d->height == nh)
        return;

    /* Apply via ioctl */
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;

    struct { uint32_t xres; uint32_t yres; uint32_t bpp; uint32_t pitch; } req;
    req.xres = nw;
    req.yres = nh;
    req.bpp = 32;
    req.pitch = 0;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &req) < 0) {
        close(fd);
        if (settings_res_lbl)
            lv_label_set_text(settings_res_lbl, "Failed to set resolution");
        return;
    }
    close(fd);

    /* Re-exec desktop to reinitialize GUI at new resolution */
    lv_deinit();
    /* Pass old resolution so new instance can show confirmation / revert */
    char pending_arg[40];
    snprintf(pending_arg, sizeof(pending_arg), "--pending-res=%dx%d",
             (int)d->width, (int)d->height);
    char *argv[] = { "desktop", "--skip-login", pending_arg, NULL };
    char *envp[] = { "TERM=dumb", "HOME=/", "PATH=/bin:/usr/bin", NULL };
    execve("/bin/desktop", argv, envp);
    _exit(1);
}

/* ── Resolution confirmation dialog ────────────────────────────────── */

static void res_keep_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    res_confirm_keep = 1;
}

static void res_revert_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    res_confirm_revert = 1;
}

static void revert_resolution(void)
{
    /* Re-apply old resolution via ioctl */
    int fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        struct { uint32_t xres; uint32_t yres; uint32_t bpp; uint32_t pitch; } req;
        req.xres = res_old_w;
        req.yres = res_old_h;
        req.bpp  = 32;
        req.pitch = 0;
        ioctl(fd, FBIOPUT_VSCREENINFO, &req);
        close(fd);
    }
    /* Re-exec without --pending-res to accept old resolution */
    lv_deinit();
    char *argv[] = { "desktop", "--skip-login", NULL };
    char *envp[] = { "TERM=dumb", "HOME=/", "PATH=/bin:/usr/bin", NULL };
    execve("/bin/desktop", argv, envp);
    _exit(1);
}

static void create_res_confirm(void)
{
    lv_disp_t *d = lv_disp_get();
    int16_t dw = (int16_t)(300);
    int16_t dh = (int16_t)(140);
    int16_t dx = (int16_t)((d->width  - dw) / 2);
    int16_t dy = (int16_t)((d->height - dh) / 2);

    win_res_confirm = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(win_res_confirm, dx, dy);
    lv_obj_set_size(win_res_confirm, dw, dh);
    lv_obj_set_style_bg_color(win_res_confirm, lv_color_make(30, 30, 40));
    lv_obj_set_style_border_width(win_res_confirm, 2);
    lv_obj_set_style_border_color(win_res_confirm, lv_color_make(80, 140, 220));
    lv_obj_set_style_pad_all(win_res_confirm, 12);

    lv_obj_t *title = lv_label_create(win_res_confirm);
    lv_label_set_text(title, "Keep this resolution?");
    lv_obj_set_style_text_color(title, lv_color_make(220, 220, 230));
    lv_obj_set_pos(title, 40, 8);

    res_confirm_lbl = lv_label_create(win_res_confirm);
    lv_obj_set_style_text_color(res_confirm_lbl, lv_color_make(180, 180, 190));
    lv_obj_set_pos(res_confirm_lbl, 30, 36);

    char buf[80];
    snprintf(buf, sizeof(buf), "Reverting in %d seconds...", 25);
    lv_label_set_text(res_confirm_lbl, buf);

    lv_obj_t *keep_btn = lv_btn_create(win_res_confirm);
    lv_btn_set_text(keep_btn, "Keep Changes");
    lv_obj_set_pos(keep_btn, 16, 75);
    lv_obj_set_size(keep_btn, 120, 32);
    lv_obj_set_style_bg_color(keep_btn, lv_color_make(40, 120, 60));
    lv_obj_add_event_cb(keep_btn, res_keep_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rev_btn = lv_btn_create(win_res_confirm);
    lv_btn_set_text(rev_btn, "Revert");
    lv_obj_set_pos(rev_btn, 160, 75);
    lv_obj_set_size(rev_btn, 100, 32);
    lv_obj_set_style_bg_color(rev_btn, lv_color_make(160, 50, 50));
    lv_obj_add_event_cb(rev_btn, res_revert_cb, LV_EVENT_CLICKED, NULL);

    res_confirm_keep   = 0;
    res_confirm_revert = 0;
    res_countdown      = 25;
    res_countdown_ctr  = 0;
}

static void create_settings(void)
{
    win_settings = lv_win_create(lv_scr_act());
    lv_win_set_title(win_settings, "Settings");
    lv_obj_set_pos(win_settings, 200, 100);
    lv_obj_set_size(win_settings, 360, 480);
    lv_obj_add_event_cb(win_settings, on_settings_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_settings);
    int y = 0;
    lv_obj_t *l;

    l = lv_label_create(c);
    lv_label_set_text(l, "Desktop Background Color");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 0, y); y += 28;

    /* Red slider */
    settings_r_lbl = lv_label_create(c);
    lv_label_set_text(settings_r_lbl, "R: 26");
    lv_obj_set_pos(settings_r_lbl, 0, y);
    settings_r_slider = lv_slider_create(c);
    lv_obj_set_pos(settings_r_slider, 60, y);
    lv_obj_set_size(settings_r_slider, 240, 24);
    lv_slider_set_range(settings_r_slider, 0, 255);
    lv_slider_set_value(settings_r_slider, 26);
    lv_obj_add_event_cb(settings_r_slider, settings_color_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);
    y += 34;

    /* Green slider */
    settings_g_lbl = lv_label_create(c);
    lv_label_set_text(settings_g_lbl, "G: 58");
    lv_obj_set_pos(settings_g_lbl, 0, y);
    settings_g_slider = lv_slider_create(c);
    lv_obj_set_pos(settings_g_slider, 60, y);
    lv_obj_set_size(settings_g_slider, 240, 24);
    lv_slider_set_range(settings_g_slider, 0, 255);
    lv_slider_set_value(settings_g_slider, 58);
    lv_obj_add_event_cb(settings_g_slider, settings_color_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);
    y += 34;

    /* Blue slider */
    settings_b_lbl = lv_label_create(c);
    lv_label_set_text(settings_b_lbl, "B: 92");
    lv_obj_set_pos(settings_b_lbl, 0, y);
    settings_b_slider = lv_slider_create(c);
    lv_obj_set_pos(settings_b_slider, 60, y);
    lv_obj_set_size(settings_b_slider, 240, 24);
    lv_slider_set_range(settings_b_slider, 0, 255);
    lv_slider_set_value(settings_b_slider, 92);
    lv_obj_add_event_cb(settings_b_slider, settings_color_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);
    y += 40;

    /* Color preview */
    l = lv_label_create(c);
    lv_label_set_text(l, "Preview:");
    lv_obj_set_pos(l, 0, y + 4);

    settings_preview = lv_obj_create(c);
    lv_obj_set_pos(settings_preview, 80, y);
    lv_obj_set_size(settings_preview, 220, 30);
    lv_obj_set_style_bg_color(settings_preview, lv_color_make(26, 58, 92));
    lv_obj_set_style_border_width(settings_preview, 1);
    lv_obj_set_style_border_color(settings_preview, LV_COLOR_GRAY);
    y += 44;

    /* Reset button */
    lv_obj_t *reset = lv_btn_create(c);
    lv_btn_set_text(reset, "Reset Default");
    lv_obj_set_pos(reset, 0, y);
    lv_obj_set_size(reset, 140, 32);
    lv_obj_set_style_bg_color(reset, lv_color_make(100, 100, 110));
    lv_obj_add_event_cb(reset, settings_reset_cb, LV_EVENT_CLICKED, NULL);
    y += 44;

    /* ── Display Resolution ───────────────────────────────────── */
    l = lv_label_create(c);
    lv_label_set_text(l, "Display Resolution");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 0, y); y += 24;

    settings_res_lbl = lv_label_create(c);
    {
        lv_disp_t *d = lv_disp_get();
        char rbuf[48];
        snprintf(rbuf, sizeof(rbuf), "Current: %dx%d", d->width, d->height);
        lv_label_set_text(settings_res_lbl, rbuf);
    }
    lv_obj_set_pos(settings_res_lbl, 0, y); y += 24;

    /* Resolution option buttons — 2 columns */
    {
        int col0_x = 0, col1_x = 170;
        int bw = 150, bh = 28;
        lv_disp_t *d = lv_disp_get();
        for (int i = 0; i < NUM_RES_OPTIONS; i++) {
            int col = i % 2;
            int row = i / 2;
            int bx = col ? col1_x : col0_x;
            int by = y + row * (bh + 4);
            char txt[20];
            snprintf(txt, sizeof(txt), "%dx%d",
                     res_options[i].w, res_options[i].h);
            lv_obj_t *rb = lv_btn_create(c);
            lv_btn_set_text(rb, txt);
            lv_obj_set_pos(rb, (int16_t)bx, (int16_t)by);
            lv_obj_set_size(rb, (int16_t)bw, (int16_t)bh);
            lv_obj_add_event_cb(rb, settings_res_cb, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);
            /* Highlight current resolution */
            if (d->width == res_options[i].w &&
                d->height == res_options[i].h)
                lv_obj_set_style_bg_color(rb, lv_color_make(40, 120, 200));
            else
                lv_obj_set_style_bg_color(rb, lv_color_make(80, 80, 90));
        }
        y += ((NUM_RES_OPTIONS + 1) / 2) * (bh + 4);
    }

    tb_add_dyn_btn(&win_settings, "Cfg");
}

/* ══════════════════════════════════════════════════════════════════════
 *  About Window
 * ══════════════════════════════════════════════════════════════════════ */

static void about_ok_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (win_about) { on_about_close(win_about, LV_EVENT_CLOSE); }
}

static void create_about(void)
{
    win_about = lv_win_create(lv_scr_act());
    lv_win_set_title(win_about, "About");
    lv_obj_set_pos(win_about, 320, 200);
    lv_obj_set_size(win_about, 300, 220);
    lv_obj_add_event_cb(win_about, on_about_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_about);
    lv_obj_t *l;
    int y = 10;

    l = lv_label_create(c);
    lv_label_set_text(l, "xv6 Desktop");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 80, y); y += 24;

    l = lv_label_create(c);
    lv_label_set_text(l, "Version 1.0");
    lv_obj_set_pos(l, 96, y); y += 30;

    l = lv_label_create(c);
    lv_label_set_text(l, "A lightweight desktop");
    lv_obj_set_pos(l, 48, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "environment for xv6 using");
    lv_obj_set_pos(l, 32, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "the LVGL GUI framework.");
    lv_obj_set_pos(l, 36, y); y += 30;

    lv_obj_t *ok = lv_btn_create(c);
    lv_btn_set_text(ok, "OK");
    lv_obj_set_pos(ok, 100, y);
    lv_obj_set_size(ok, 80, 30);
    lv_obj_add_event_cb(ok, about_ok_cb, LV_EVENT_CLICKED, NULL);
    tb_add_dyn_btn(&win_about, "About");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Process Manager (reads /proc/<pid>/status for each process)
 * ══════════════════════════════════════════════════════════════════════ */

static void procmgr_refresh(void)
{
    if (!procmgr_list_lbl) return;

    static char display[2048];
    int pos = 0;

    pos += snprintf(display + pos, sizeof(display) - pos,
        "%-6s %-16s %-8s %8s\n"
        "--------------------------------------\n",
        "PID", "Name", "State", "VmSize");

    DIR *dp = opendir("/proc");
    if (!dp) {
        lv_label_set_text(procmgr_list_lbl, "Cannot open /proc");
        return;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;

        int pid = atoi(de->d_name);
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        if (gui_read_file(path, buf, sizeof(buf)) <= 0)
            continue;

        char name[17] = "?", state[8] = "?";
        uint64_t vm_kb = 0;
        parse_proc_field_str(buf, "Name", name, sizeof(name));
        parse_proc_field_str(buf, "State", state, sizeof(state));
        parse_proc_field_u64(buf, "VmSize", &vm_kb);

        if ((int)(sizeof(display) - pos) < 60) break;
        pos += snprintf(display + pos, sizeof(display) - pos,
            "%-6d %-16s %-8s %6lu KB\n",
            pid, name, state, (unsigned long)vm_kb);
        count++;
    }
    closedir(dp);

    if (count == 0)
        pos += snprintf(display + pos, sizeof(display) - pos,
                        "(no processes found)");

    lv_label_set_text(procmgr_list_lbl, display);
}

static void procmgr_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    procmgr_refresh();
}

static void create_procmgr(void)
{
    win_procmgr = lv_win_create(lv_scr_act());
    lv_win_set_title(win_procmgr, "Process Manager");
    lv_obj_set_pos(win_procmgr, 100, 30);
    lv_obj_set_size(win_procmgr, 400, 440);
    lv_obj_add_event_cb(win_procmgr, on_procmgr_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_procmgr);
    lv_obj_set_style_bg_color(c, lv_color_make(15, 15, 20));

    /* Toolbar at top */
    lv_obj_t *rb = lv_btn_create(c);
    lv_btn_set_text(rb, "Refresh");
    lv_obj_set_pos(rb, 0, 0);
    lv_obj_set_size(rb, 80, 28);
    lv_obj_set_style_bg_color(rb, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(rb, procmgr_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable content area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 382, 363);
    lv_obj_set_style_bg_color(sc, lv_color_make(15, 15, 20));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    procmgr_list_lbl = lv_label_create(sc);
    lv_obj_set_pos(procmgr_list_lbl, 0, 0);
    lv_obj_set_style_text_color(procmgr_list_lbl,
                                lv_color_make(180, 220, 180));
    lv_label_set_text(procmgr_list_lbl, "Loading...");

    procmgr_refresh();
    tb_add_dyn_btn(&win_procmgr, "Procs");
}

/* ══════════════════════════════════════════════════════════════════════
 *  System Monitor (CPU, memory, disk I/O, network via kstats/memstat)
 * ══════════════════════════════════════════════════════════════════════ */

static void sysmon_refresh(void)
{
    if (!sysmon_info_lbl) return;

    static char display[2048];
    int pos = 0;

    struct gui_kstats ks;
    memset(&ks, 0, sizeof(ks));
    gui_kstats_get(&ks);

    char uptime_str[64];
    format_uptime(ks.uptime_ms, uptime_str, sizeof(uptime_str));
    pos += snprintf(display + pos, sizeof(display) - pos,
        "Uptime: %s   CPUs: %d\n"
        "Load:   %lu.%02lu  %lu.%02lu  %lu.%02lu\n\n",
        uptime_str, ks.ncpus,
        (unsigned long)(ks.load_avg_1s >> 11),
        (unsigned long)((ks.load_avg_1s & 0x7FF) * 100 / 2048),
        (unsigned long)(ks.load_avg_5s >> 11),
        (unsigned long)((ks.load_avg_5s & 0x7FF) * 100 / 2048),
        (unsigned long)(ks.load_avg_16s >> 11),
        (unsigned long)((ks.load_avg_16s & 0x7FF) * 100 / 2048));

    uint64_t mem_free = gui_memstat_free();
    uint64_t mem_used = gui_memstat_used();
    uint64_t mem_total = mem_free + mem_used;
    int mem_pct = mem_total ? (int)(mem_used * 100 / mem_total) : 0;
    char bar[21];
    int fill = mem_pct / 5;
    for (int i = 0; i < 20; i++) bar[i] = (i < fill) ? '#' : '.';
    bar[20] = '\0';

    char free_s[32], used_s[32], total_s[32];
    format_size(mem_free, free_s, sizeof(free_s));
    format_size(mem_used, used_s, sizeof(used_s));
    format_size(mem_total, total_s, sizeof(total_s));

    pos += snprintf(display + pos, sizeof(display) - pos,
        "Memory  [%s] %d%%\n"
        "  Total: %s  Used: %s  Free: %s\n\n",
        bar, mem_pct, total_s, used_s, free_s);

    pos += snprintf(display + pos, sizeof(display) - pos, "CPU Usage\n");
    for (int i = 0; i < ks.ncpus && i < GUI_NCPU; i++) {
        uint64_t busy = ks.cpu[i].busy_ticks;
        uint64_t total = ks.cpu[i].total_ticks;
        int pct = total ? (int)(busy * 100 / total) : 0;
        char cbar[11];
        int cf = pct / 10;
        for (int j = 0; j < 10; j++) cbar[j] = (j < cf) ? '#' : '.';
        cbar[10] = '\0';
        pos += snprintf(display + pos, sizeof(display) - pos,
            "  CPU%d [%s] %3d%%  %s\n",
            i, cbar, pct, ks.cpu[i].idle ? "idle" : "busy");
    }

    char dr[32], dw[32];
    format_size(ks.bio_read_bytes, dr, sizeof(dr));
    format_size(ks.bio_write_bytes, dw, sizeof(dw));
    pos += snprintf(display + pos, sizeof(display) - pos,
        "\nDisk I/O\n"
        "  Reads:  %lu ops / %s\n"
        "  Writes: %lu ops / %s\n",
        (unsigned long)ks.bio_reads, dr,
        (unsigned long)ks.bio_writes, dw);

    char ntx[32], nrx[32];
    format_size(ks.net_tx_bytes, ntx, sizeof(ntx));
    format_size(ks.net_rx_bytes, nrx, sizeof(nrx));
    pos += snprintf(display + pos, sizeof(display) - pos,
        "\nNetwork\n"
        "  TX: %lu pkts / %s\n"
        "  RX: %lu pkts / %s",
        (unsigned long)ks.net_tx_packets, ntx,
        (unsigned long)ks.net_rx_packets, nrx);

    lv_label_set_text(sysmon_info_lbl, display);
}

static void sysmon_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    sysmon_refresh();
}

static void create_sysmon(void)
{
    win_sysmon = lv_win_create(lv_scr_act());
    lv_win_set_title(win_sysmon, "System Monitor");
    lv_obj_set_pos(win_sysmon, 140, 20);
    lv_obj_set_size(win_sysmon, 420, 480);
    lv_obj_add_event_cb(win_sysmon, on_sysmon_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_sysmon);
    lv_obj_set_style_bg_color(c, lv_color_make(10, 10, 15));

    /* Toolbar at top */
    lv_obj_t *rb = lv_btn_create(c);
    lv_btn_set_text(rb, "Refresh");
    lv_obj_set_pos(rb, 0, 0);
    lv_obj_set_size(rb, 80, 28);
    lv_obj_set_style_bg_color(rb, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(rb, sysmon_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable content area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 402, 403);
    lv_obj_set_style_bg_color(sc, lv_color_make(10, 10, 15));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    sysmon_info_lbl = lv_label_create(sc);
    lv_obj_set_pos(sysmon_info_lbl, 0, 0);
    lv_obj_set_style_text_color(sysmon_info_lbl,
                                lv_color_make(100, 220, 100));
    lv_label_set_text(sysmon_info_lbl, "Loading...");

    sysmon_refresh_ctr = 0;
    sysmon_refresh();
    tb_add_dyn_btn(&win_sysmon, "Mon");
}

/* ══════════════════════════════════════════════════════════════════════
 *  File Manager (directory browsing via opendir/readdir/stat)
 * ══════════════════════════════════════════════════════════════════════ */

static void filemgr_update_display(void);

static void filemgr_refresh(void)
{
    if (!filemgr_list_lbl || !filemgr_path_lbl) return;

    lv_label_set_text(filemgr_path_lbl, filemgr_cwd);

    filemgr_entry_count = 0;

    DIR *dp = opendir(filemgr_cwd);
    if (!dp) {
        lv_label_set_text(filemgr_list_lbl, "Cannot open directory");
        return;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (count >= FILEMGR_MAX_ENTRIES) break;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 filemgr_cwd,
                 (filemgr_cwd[strlen(filemgr_cwd) - 1] == '/') ? "" : "/",
                 de->d_name);

        struct stat st;
        int is_dir = 0;
        if (stat(fullpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        } else {
            is_dir = (de->d_type == DT_DIR);
        }

        /* Cache entry for click navigation */
        strncpy(filemgr_entries[count].name, de->d_name,
                sizeof(filemgr_entries[count].name) - 1);
        filemgr_entries[count].name[sizeof(filemgr_entries[count].name) - 1] = '\0';
        filemgr_entries[count].is_dir = is_dir;

        count++;
    }
    closedir(dp);
    filemgr_entry_count = count;
    filemgr_selected = -1;

    /* Build display text via shared helper */
    filemgr_update_display();

    /* Reset scroll to top on directory change */
    if (filemgr_scroll)
        filemgr_scroll->scroll_y = 0;
}

/* Compute absolute Y of an object by walking parent chain */
static int obj_abs_y(lv_obj_t *obj)
{
    int ay = obj->y;
    lv_obj_t *p = obj->parent;
    while (p) {
        ay += p->y + p->padding - p->scroll_y;
        p = p->parent;
    }
    return ay;
}

static void filemgr_navigate_to(int line);

static void filemgr_list_click_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (!filemgr_scroll || filemgr_entry_count <= 0) return;

    /* Get mouse Y position */
    lv_indev_t *indev = lv_indev_get();
    int mouse_y = indev->y;

    /* Compute absolute Y of the list label */
    int label_abs_y = obj_abs_y(filemgr_list_lbl);

    /* Which line was clicked? */
    int rel_y = mouse_y - label_abs_y;
    if (rel_y < 0) return;
    int line = rel_y / LV_FONT_HEIGHT;
    if (line < 0 || line >= filemgr_entry_count) return;

    uint32_t now = lv_get_frame_count();

    /* Double-click detection: same entry clicked within 15 frames (~500ms at 30fps) */
    if (line == filemgr_last_click_line &&
        (now - filemgr_last_click_frame) < 15) {
        /* Double-click: navigate into directory or view file */
        filemgr_navigate_to(line);
        filemgr_last_click_line = -1;
        return;
    }

    /* Single click: select this entry */
    filemgr_selected = line;
    filemgr_last_click_line = line;
    filemgr_last_click_frame = now;
    filemgr_update_display();
}

/* Navigate into directory at given entry index */
static void filemgr_navigate_to(int line)
{
    if (line < 0 || line >= filemgr_entry_count) return;

    if (filemgr_entries[line].is_dir) {
        const char *name = filemgr_entries[line].name;
        if (strcmp(name, ".") == 0) {
            filemgr_refresh();
            return;
        }
        if (strcmp(name, "..") == 0) {
            /* Go up */
            int len = (int)strlen(filemgr_cwd);
            if (len > 1 && filemgr_cwd[len - 1] == '/')
                filemgr_cwd[--len] = '\0';
            char *last = strrchr(filemgr_cwd, '/');
            if (last && last != filemgr_cwd)
                *last = '\0';
            else
                strcpy(filemgr_cwd, "/");
        } else {
            int len = (int)strlen(filemgr_cwd);
            if (len > 1)
                snprintf(filemgr_cwd + len, sizeof(filemgr_cwd) - len,
                         "/%s", name);
            else
                snprintf(filemgr_cwd + len, sizeof(filemgr_cwd) - len,
                         "%s", name);
        }
        filemgr_selected = -1;
        filemgr_refresh();
    }
    /* Files: no navigation action */
}

/* Rebuild display text with selection highlight (> prefix) */
static void filemgr_update_display(void)
{
    if (!filemgr_list_lbl) return;

    static char display[2048];
    int pos = 0;

    for (int i = 0; i < filemgr_entry_count; i++) {
        if ((int)(sizeof(display) - pos) < 50) break;
        const char *sel = (i == filemgr_selected) ? "> " : "  ";
        if (filemgr_entries[i].is_dir) {
            pos += snprintf(display + pos, sizeof(display) - pos,
                "%s[DIR]  %s\n", sel, filemgr_entries[i].name);
        } else {
            pos += snprintf(display + pos, sizeof(display) - pos,
                "%s[FILE] %s\n", sel, filemgr_entries[i].name);
        }
    }

    if (filemgr_entry_count == 0)
        snprintf(display, sizeof(display), "(empty directory)");

    lv_label_set_text(filemgr_list_lbl, display);
}

static void filemgr_up_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    int len = (int)strlen(filemgr_cwd);
    if (len <= 1) return;

    if (filemgr_cwd[len - 1] == '/' && len > 1) {
        filemgr_cwd[len - 1] = '\0';
        len--;
    }
    char *last = strrchr(filemgr_cwd, '/');
    if (last && last != filemgr_cwd)
        *(last) = '\0';
    else
        strcpy(filemgr_cwd, "/");

    filemgr_refresh();
}

static void filemgr_root_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    strcpy(filemgr_cwd, "/");
    filemgr_refresh();
}

static void filemgr_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    filemgr_refresh();
}

static void create_filemgr(void)
{
    win_filemgr = lv_win_create(lv_scr_act());
    lv_win_set_title(win_filemgr, "File Manager");
    lv_obj_set_pos(win_filemgr, 180, 50);
    lv_obj_set_size(win_filemgr, 400, 440);
    lv_obj_add_event_cb(win_filemgr, on_filemgr_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_filemgr);
    lv_obj_set_style_bg_color(c, lv_color_make(20, 20, 28));

    /* Path label at top */
    filemgr_path_lbl = lv_label_create(c);
    lv_obj_set_pos(filemgr_path_lbl, 0, 0);
    lv_obj_set_style_text_color(filemgr_path_lbl,
                                lv_color_make(100, 180, 255));
    lv_label_set_text(filemgr_path_lbl, filemgr_cwd);

    /* Navigation buttons */
    lv_obj_t *b;

    b = lv_btn_create(c);
    lv_btn_set_text(b, "Up");
    lv_obj_set_pos(b, 0, 20);
    lv_obj_set_size(b, 50, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(b, filemgr_up_cb, LV_EVENT_CLICKED, NULL);

    b = lv_btn_create(c);
    lv_btn_set_text(b, "/");
    lv_obj_set_pos(b, 56, 20);
    lv_obj_set_size(b, 35, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(b, filemgr_root_cb, LV_EVENT_CLICKED, NULL);

    b = lv_btn_create(c);
    lv_btn_set_text(b, "Refresh");
    lv_obj_set_pos(b, 97, 20);
    lv_obj_set_size(b, 80, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(b, filemgr_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable file listing area */
    lv_obj_t *sc = lv_obj_create(c);
    filemgr_scroll = sc;
    lv_obj_set_pos(sc, 0, 52);
    lv_obj_set_size(sc, 382, 343);
    lv_obj_set_style_bg_color(sc, lv_color_make(20, 20, 28));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);
    lv_obj_add_event_cb(sc, filemgr_list_click_cb, LV_EVENT_CLICKED, NULL);

    filemgr_list_lbl = lv_label_create(sc);
    lv_obj_set_pos(filemgr_list_lbl, 0, 0);
    lv_obj_set_style_text_color(filemgr_list_lbl,
                                lv_color_make(200, 200, 200));
    lv_label_set_text(filemgr_list_lbl, "Loading...");

    filemgr_refresh();
    tb_add_dyn_btn(&win_filemgr, "Files");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Network Information (ioctl SIOCNETCONF_GET + kstats network stats)
 * ══════════════════════════════════════════════════════════════════════ */

static void netinfo_refresh(void)
{
    if (!netinfo_lbl) return;

    static char display[1024];
    int pos = 0;

    struct gui_netconf_req nc;
    memset(&nc, 0, sizeof(nc));

    int nfd = open("/dev/netconf", O_RDONLY);
    if (nfd >= 0) {
        if (ioctl(nfd, SIOCNETCONF_GET, &nc) == 0) {
            char ip[20], mask[20], gw[20], dns[20];
            format_ipv4(nc.ip, ip, sizeof(ip));
            format_ipv4(nc.netmask, mask, sizeof(mask));
            format_ipv4(nc.gateway, gw, sizeof(gw));
            format_ipv4(nc.dns, dns, sizeof(dns));

            pos += snprintf(display + pos, sizeof(display) - pos,
                "Network Configuration\n"
                "  Mode:    %s\n"
                "  IP:      %s\n"
                "  Netmask: %s\n"
                "  Gateway: %s\n"
                "  DNS:     %s\n"
                "  Host:    %s\n\n",
                nc.mode == 0 ? "DHCP" : "Static",
                ip, mask, gw, dns,
                nc.hostname[0] ? nc.hostname : "(none)");
        } else {
            pos += snprintf(display + pos, sizeof(display) - pos,
                "Network Configuration\n"
                "  (ioctl failed)\n\n");
        }
        close(nfd);
    } else {
        pos += snprintf(display + pos, sizeof(display) - pos,
            "Network Configuration\n"
            "  (no /dev/netconf)\n\n");
    }

    struct gui_kstats ks;
    memset(&ks, 0, sizeof(ks));
    gui_kstats_get(&ks);

    char ntx[32], nrx[32];
    format_size(ks.net_tx_bytes, ntx, sizeof(ntx));
    format_size(ks.net_rx_bytes, nrx, sizeof(nrx));

    pos += snprintf(display + pos, sizeof(display) - pos,
        "Traffic Statistics\n"
        "  TX: %lu packets / %s\n"
        "  RX: %lu packets / %s\n\n",
        (unsigned long)ks.net_tx_packets, ntx,
        (unsigned long)ks.net_rx_packets, nrx);

    char dr[32], dw_str[32];
    format_size(ks.bio_read_bytes, dr, sizeof(dr));
    format_size(ks.bio_write_bytes, dw_str, sizeof(dw_str));

    pos += snprintf(display + pos, sizeof(display) - pos,
        "Disk I/O Statistics\n"
        "  Reads:  %lu ops / %s\n"
        "  Writes: %lu ops / %s",
        (unsigned long)ks.bio_reads, dr,
        (unsigned long)ks.bio_writes, dw_str);

    lv_label_set_text(netinfo_lbl, display);
}

static void netinfo_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    netinfo_refresh();
}

static void create_netinfo(void)
{
    win_netinfo = lv_win_create(lv_scr_act());
    lv_win_set_title(win_netinfo, "Network Info");
    lv_obj_set_pos(win_netinfo, 220, 80);
    lv_obj_set_size(win_netinfo, 350, 380);
    lv_obj_add_event_cb(win_netinfo, on_netinfo_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_netinfo);
    lv_obj_set_style_bg_color(c, lv_color_make(15, 15, 25));

    /* Toolbar at top */
    lv_obj_t *rb = lv_btn_create(c);
    lv_btn_set_text(rb, "Refresh");
    lv_obj_set_pos(rb, 0, 0);
    lv_obj_set_size(rb, 80, 28);
    lv_obj_set_style_bg_color(rb, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(rb, netinfo_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable content area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 332, 303);
    lv_obj_set_style_bg_color(sc, lv_color_make(15, 15, 25));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    netinfo_lbl = lv_label_create(sc);
    lv_obj_set_pos(netinfo_lbl, 0, 0);
    lv_obj_set_style_text_color(netinfo_lbl,
                                lv_color_make(180, 200, 255));
    lv_label_set_text(netinfo_lbl, "Loading...");

    netinfo_refresh();
    tb_add_dyn_btn(&win_netinfo, "Net");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Power Management Window
 * ══════════════════════════════════════════════════════════════════════ */

static void power_refresh(void)
{
    if (!power_info_lbl) return;

    static char display[512];
    int pos = 0;

    struct gui_kstats ks;
    memset(&ks, 0, sizeof(ks));
    gui_kstats_get(&ks);

    char upstr[64];
    format_uptime(ks.uptime_ms, upstr, sizeof(upstr));

    uint64_t mem_free = gui_memstat_free();
    uint64_t mem_used = gui_memstat_used();
    char mf[32], mu[32];
    format_size(mem_free, mf, sizeof(mf));
    format_size(mem_used, mu, sizeof(mu));

    pos += snprintf(display + pos, sizeof(display) - pos,
        "System Status\n"
        "  Uptime:   %s\n"
        "  CPUs:     %d\n"
        "  Mem Used: %s\n"
        "  Mem Free: %s\n\n"
        "Power Actions\n"
        "  Use buttons below to\n"
        "  shutdown or reboot.\n",
        upstr, ks.ncpus, mu, mf);

    lv_label_set_text(power_info_lbl, display);
}

static void power_shutdown_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    xv6_syscall0(XV6_SYS_poweroff);
}

static void power_reboot_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    xv6_syscall0(XV6_SYS_reboot);
}

static void power_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    power_refresh();
}

static void create_power(void)
{
    win_power = lv_win_create(lv_scr_act());
    lv_win_set_title(win_power, "Power Management");
    lv_obj_set_pos(win_power, 250, 120);
    lv_obj_set_size(win_power, 320, 340);
    lv_obj_add_event_cb(win_power, on_power_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_power);
    lv_obj_set_style_bg_color(c, lv_color_make(20, 15, 25));

    /* Toolbar */
    lv_obj_t *b;
    int16_t bx = 0;

    b = lv_btn_create(c);
    lv_btn_set_text(b, "Refresh");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 80, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(b, power_refresh_cb, LV_EVENT_CLICKED, NULL);
    bx += 86;

    b = lv_btn_create(c);
    lv_btn_set_text(b, "Shutdown");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 90, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(180, 40, 40));
    lv_obj_add_event_cb(b, power_shutdown_cb, LV_EVENT_CLICKED, NULL);
    bx += 96;

    b = lv_btn_create(c);
    lv_btn_set_text(b, "Reboot");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 80, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(160, 120, 40));
    lv_obj_add_event_cb(b, power_reboot_cb, LV_EVENT_CLICKED, NULL);

    /* Info area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 302, 263);
    lv_obj_set_style_bg_color(sc, lv_color_make(20, 15, 25));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    power_info_lbl = lv_label_create(sc);
    lv_obj_set_pos(power_info_lbl, 0, 0);
    lv_obj_set_style_text_color(power_info_lbl,
                                lv_color_make(200, 200, 220));
    lv_label_set_text(power_info_lbl, "Loading...");

    power_refresh();
    tb_add_dyn_btn(&win_power, "Pwr");
}

/* ══════════════════════════════════════════════════════════════════════
 *  User Management Window
 * ══════════════════════════════════════════════════════════════════════ */

static void usermgr_refresh(void)
{
    if (!usermgr_info_lbl) return;

    static char display[1024];
    int pos = 0;

    int uid  = getuid();
    int euid = geteuid();
    int gid  = getgid();
    int egid = getegid();
    int pid  = getpid();
    int ppid = getppid();

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    pos += snprintf(display + pos, sizeof(display) - pos,
        "Current User\n"
        "  UID:      %d\n"
        "  EUID:     %d\n"
        "  GID:      %d\n"
        "  EGID:     %d\n"
        "  PID:      %d\n"
        "  PPID:     %d\n\n",
        uid, euid, gid, egid, pid, ppid);

    pos += snprintf(display + pos, sizeof(display) - pos,
        "User Name\n"
        "  %s\n\n",
        (uid == 0) ? "root (superuser)" : "user");

    pos += snprintf(display + pos, sizeof(display) - pos,
        "System Info\n"
        "  Hostname: %s\n"
        "  OS:       %s %s\n"
        "  Arch:     %s\n\n",
        uts.nodename, uts.sysname, uts.release, uts.machine);

    /* Check logged-in processes by scanning /proc */
    pos += snprintf(display + pos, sizeof(display) - pos,
        "Active Processes\n");

    DIR *dp = opendir("/proc");
    if (dp) {
        struct dirent *de;
        int nprocs = 0;
        while ((de = readdir(dp)) != NULL) {
            /* Only count numeric directories (PIDs) */
            if (de->d_name[0] >= '1' && de->d_name[0] <= '9')
                nprocs++;
        }
        closedir(dp);
        pos += snprintf(display + pos, sizeof(display) - pos,
            "  Total processes: %d\n", nprocs);
    } else {
        pos += snprintf(display + pos, sizeof(display) - pos,
            "  (cannot read /proc)\n");
    }

    /* Check /etc/passwd for user accounts */
    pos += snprintf(display + pos, sizeof(display) - pos,
        "\nUser Accounts (/etc/passwd)\n");
    char passbuf[512];
    if (gui_read_file("/etc/passwd", passbuf, sizeof(passbuf)) > 0) {
        /* Show each line */
        char *line = passbuf;
        while (*line) {
            char *eol = strchr(line, '\n');
            if (eol) *eol = '\0';
            if (*line) {
                /* Extract username (first field before ':') */
                char *colon = strchr(line, ':');
                if (colon) *colon = '\0';
                pos += snprintf(display + pos, sizeof(display) - pos,
                    "  %s\n", line);
                if (colon) *colon = ':';
            }
            if (!eol) break;
            line = eol + 1;
        }
    } else {
        pos += snprintf(display + pos, sizeof(display) - pos,
            "  root (default)\n");
    }

    lv_label_set_text(usermgr_info_lbl, display);
}

static void usermgr_refresh_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    usermgr_refresh();
}

static void create_usermgr(void)
{
    win_usermgr = lv_win_create(lv_scr_act());
    lv_win_set_title(win_usermgr, "User Management");
    lv_obj_set_pos(win_usermgr, 150, 60);
    lv_obj_set_size(win_usermgr, 340, 400);
    lv_obj_add_event_cb(win_usermgr, on_usermgr_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_usermgr);
    lv_obj_set_style_bg_color(c, lv_color_make(18, 20, 28));

    /* Toolbar */
    lv_obj_t *rb = lv_btn_create(c);
    lv_btn_set_text(rb, "Refresh");
    lv_obj_set_pos(rb, 0, 0);
    lv_obj_set_size(rb, 80, 28);
    lv_obj_set_style_bg_color(rb, lv_color_make(50, 80, 120));
    lv_obj_add_event_cb(rb, usermgr_refresh_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable content area */
    lv_obj_t *sc = lv_obj_create(c);
    lv_obj_set_pos(sc, 0, 32);
    lv_obj_set_size(sc, 322, 323);
    lv_obj_set_style_bg_color(sc, lv_color_make(18, 20, 28));
    lv_obj_set_style_border_width(sc, 0);
    lv_obj_set_style_pad_all(sc, 2);
    lv_obj_set_scrollable(sc, 1);

    usermgr_info_lbl = lv_label_create(sc);
    lv_obj_set_pos(usermgr_info_lbl, 0, 0);
    lv_obj_set_style_text_color(usermgr_info_lbl,
                                lv_color_make(200, 210, 200));
    lv_label_set_text(usermgr_info_lbl, "Loading...");

    usermgr_refresh();
    tb_add_dyn_btn(&win_usermgr, "User");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Internet Configuration Window
 * ══════════════════════════════════════════════════════════════════════ */

static int parse_ipv4(const char *str, unsigned char out[4])
{
    int a, b, c, d;
    if (sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return 0;
    if (a < 0 || a > 255 || b < 0 || b > 255 ||
        c < 0 || c > 255 || d < 0 || d > 255)
        return 0;
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return 1;
}

static void netcfg_load_current(void)
{
    struct gui_netconf_req nc;
    memset(&nc, 0, sizeof(nc));
    int nfd = open("/dev/netconf", O_RDONLY);
    if (nfd >= 0) {
        if (ioctl(nfd, SIOCNETCONF_GET, &nc) == 0) {
            unsigned char *p;
            char buf[20];
            p = (unsigned char *)&nc.ip;
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
            if (netcfg_ip_tb) lv_textbox_set_text(netcfg_ip_tb, buf);

            p = (unsigned char *)&nc.netmask;
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
            if (netcfg_mask_tb) lv_textbox_set_text(netcfg_mask_tb, buf);

            p = (unsigned char *)&nc.gateway;
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
            if (netcfg_gw_tb) lv_textbox_set_text(netcfg_gw_tb, buf);

            p = (unsigned char *)&nc.dns;
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
            if (netcfg_dns_tb) lv_textbox_set_text(netcfg_dns_tb, buf);

            netcfg_mode = nc.mode;
        }
        close(nfd);
    }
}

static void netcfg_dhcp_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    netcfg_mode = obj->spec.checkbox.checked ? 1 : 0;
    if (netcfg_status_lbl)
        lv_label_set_text(netcfg_status_lbl,
                          netcfg_mode == 0 ? "Mode: DHCP" : "Mode: Static");
}

static void netcfg_apply_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;

    struct gui_netconf_req req;
    memset(&req, 0, sizeof(req));
    req.mode = netcfg_mode;

    unsigned char ip[4];
    if (netcfg_ip_tb && parse_ipv4(lv_textbox_get_text(netcfg_ip_tb), ip))
        memcpy(&req.ip, ip, 4);
    if (netcfg_mask_tb && parse_ipv4(lv_textbox_get_text(netcfg_mask_tb), ip))
        memcpy(&req.netmask, ip, 4);
    if (netcfg_gw_tb && parse_ipv4(lv_textbox_get_text(netcfg_gw_tb), ip))
        memcpy(&req.gateway, ip, 4);
    if (netcfg_dns_tb && parse_ipv4(lv_textbox_get_text(netcfg_dns_tb), ip))
        memcpy(&req.dns, ip, 4);
    strncpy(req.hostname, "xv6", sizeof(req.hostname) - 1);

    int nfd = open("/dev/netconf", O_RDWR);
    if (nfd >= 0) {
        if (ioctl(nfd, SIOCNETCONF, &req) == 0) {
            if (netcfg_status_lbl)
                lv_label_set_text(netcfg_status_lbl, "Applied OK!");
        } else {
            if (netcfg_status_lbl)
                lv_label_set_text(netcfg_status_lbl, "Apply FAILED");
        }
        close(nfd);
    } else {
        if (netcfg_status_lbl)
            lv_label_set_text(netcfg_status_lbl, "No /dev/netconf");
    }
}

static void create_netcfg(void)
{
    win_netcfg = lv_win_create(lv_scr_act());
    lv_win_set_title(win_netcfg, "Internet Config");
    lv_obj_set_pos(win_netcfg, 180, 40);
    lv_obj_set_size(win_netcfg, 420, 300);
    lv_obj_add_event_cb(win_netcfg, on_netcfg_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_netcfg);
    lv_obj_set_style_bg_color(c, lv_color_make(15, 18, 28));

    int16_t y = 0;
    lv_obj_t *b, *l, *tb;

    /* Mode checkbox */
    netcfg_mode_cb = lv_checkbox_create(c);
    lv_checkbox_set_text(netcfg_mode_cb, "Static IP");
    lv_obj_set_pos(netcfg_mode_cb, 0, y);
    netcfg_mode_cb->spec.checkbox.checked = (netcfg_mode == 1) ? 1 : 0;
    lv_obj_add_event_cb(netcfg_mode_cb, netcfg_dhcp_cb, LV_EVENT_CLICKED, NULL);

    /* Status label */
    netcfg_status_lbl = lv_label_create(c);
    lv_obj_set_pos(netcfg_status_lbl, 160, y + 2);
    lv_obj_set_style_text_color(netcfg_status_lbl,
                                lv_color_make(100, 200, 100));
    lv_label_set_text(netcfg_status_lbl,
                      netcfg_mode == 0 ? "Mode: DHCP" : "Mode: Static");
    y += 24;

    /* Apply button */
    b = lv_btn_create(c);
    lv_btn_set_text(b, "Apply");
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_size(b, 80, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(40, 120, 60));
    lv_obj_add_event_cb(b, netcfg_apply_cb, LV_EVENT_CLICKED, NULL);
    y += 36;

    /* IP fields with textboxes */
    static const char *field_names[] = {
        "IP Addr:", "Netmask:", "Gateway:", "DNS:"
    };
    lv_obj_t **field_tbs[] = {
        &netcfg_ip_tb, &netcfg_mask_tb, &netcfg_gw_tb, &netcfg_dns_tb
    };
    static const char *defaults[] = {
        "10.0.2.15", "255.255.255.0", "10.0.2.2", "8.8.8.8"
    };

    for (int f = 0; f < 4; f++) {
        l = lv_label_create(c);
        lv_label_set_text(l, field_names[f]);
        lv_obj_set_pos(l, 0, (int16_t)(y + 3));
        lv_obj_set_style_text_color(l, lv_color_make(180, 180, 200));

        tb = lv_textbox_create(c);
        lv_obj_set_pos(tb, 80, y);
        lv_obj_set_size(tb, 140, 22);
        lv_textbox_set_text(tb, defaults[f]);
        lv_textbox_set_max_length(tb, 16);
        *field_tbs[f] = tb;
        y += 28;
    }

    /* Load current config from kernel (overwrites defaults) */
    netcfg_load_current();
    tb_add_dyn_btn(&win_netcfg, "Inet");
}

/* ══════════════════════════════════════════════════════════════════════
 *  3D Demo — Solid lit icosahedron with filled faces
 *  Uses fixed-point (16.16) arithmetic — no SSE/float needed.
 * ══════════════════════════════════════════════════════════════════════ */

#define FP_SHIFT  16
#define FP_ONE    (1 << FP_SHIFT)
#define FP_HALF   (1 << (FP_SHIFT - 1))
#define FP_MUL(a, b)  ((int)(((long long)(a) * (b)) >> FP_SHIFT))
#define FP_DIV(a, b)  ((int)(((long long)(a) << FP_SHIFT) / (b)))
#define FP_FROM_INT(x) ((x) << FP_SHIFT)

/* Fixed-point sine table: 256 entries for 0..2π (sin * 32768) */
static const int fp_sin_tab[256] = {
        0,  1608,  3212,  4808,  6393,  7962,  9512, 11039,
    12540, 14010, 15447, 16846, 18205, 19520, 20788, 22006,
    23170, 24279, 25330, 26320, 27246, 28106, 28899, 29622,
    30274, 30853, 31357, 31786, 32138, 32413, 32610, 32729,
    32768, 32729, 32610, 32413, 32138, 31786, 31357, 30853,
    30274, 29622, 28899, 28106, 27246, 26320, 25330, 24279,
    23170, 22006, 20788, 19520, 18205, 16846, 15447, 14010,
    12540, 11039,  9512,  7962,  6393,  4808,  3212,  1608,
        0, -1608, -3212, -4808, -6393, -7962, -9512,-11039,
   -12540,-14010,-15447,-16846,-18205,-19520,-20788,-22006,
   -23170,-24279,-25330,-26320,-27246,-28106,-28899,-29622,
   -30274,-30853,-31357,-31786,-32138,-32413,-32610,-32729,
   -32768,-32729,-32610,-32413,-32138,-31786,-31357,-30853,
   -30274,-29622,-28899,-28106,-27246,-26320,-25330,-24279,
   -23170,-22006,-20788,-19520,-18205,-16846,-15447,-14010,
   -12540,-11039, -9512, -7962, -6393, -4808, -3212, -1608,
        0,  1608,  3212,  4808,  6393,  7962,  9512, 11039,
    12540, 14010, 15447, 16846, 18205, 19520, 20788, 22006,
    23170, 24279, 25330, 26320, 27246, 28106, 28899, 29622,
    30274, 30853, 31357, 31786, 32138, 32413, 32610, 32729,
    32768, 32729, 32610, 32413, 32138, 31786, 31357, 30853,
    30274, 29622, 28899, 28106, 27246, 26320, 25330, 24279,
    23170, 22006, 20788, 19520, 18205, 16846, 15447, 14010,
    12540, 11039,  9512,  7962,  6393,  4808,  3212,  1608,
        0, -1608, -3212, -4808, -6393, -7962, -9512,-11039,
   -12540,-14010,-15447,-16846,-18205,-19520,-20788,-22006,
   -23170,-24279,-25330,-26320,-27246,-28106,-28899,-29622,
   -30274,-30853,-31357,-31786,-32138,-32413,-32610,-32729,
   -32768,-32729,-32610,-32413,-32138,-31786,-31357,-30853,
   -30274,-29622,-28899,-28106,-27246,-26320,-25330,-24279,
   -23170,-22006,-20788,-19520,-18205,-16846,-15447,-14010,
   -12540,-11039, -9512, -7962, -6393, -4808, -3212, -1608,
};

/* Table period = 128 entries (two full cycles in 256).  cos = sin + 32. */
static int fp_sin(int angle) { return fp_sin_tab[angle & 255] * 2; }
static int fp_cos(int angle) { return fp_sin_tab[(angle + 32) & 255] * 2; }

/* ── Triangle rasterizer (scanline fill) ─────────────────────────── */

static void d3d_fill_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                          uint32_t color)
{
    /* Sort vertices by Y (top to bottom: y0 <= y1 <= y2) */
    if (y0 > y1) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y0 > y2) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }

    int total_h = y2 - y0;
    if (total_h == 0) return;

    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= D3D_H) continue;
        int second = (y > y1) || (y1 == y0);
        int seg_h = second ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0) continue;

        int a = ((y - y0) << 16) / total_h;
        int b = second ? (((y - y1) << 16) / seg_h)
                       : (((y - y0) << 16) / seg_h);

        int xa = x0 + (((x2 - x0) * (long long)a) >> 16);
        int xb = second ? x1 + (((x2 - x1) * (long long)b) >> 16)
                        : x0 + (((x1 - x0) * (long long)b) >> 16);

        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb >= D3D_W) xb = D3D_W - 1;

        uint32_t *row = d3d_buf + y * D3D_W;
        for (int x = xa; x <= xb; x++)
            row[x] = color;
    }
}

/* ── Edge drawer (Bresenham) for face outlines ───────────────────── */

static void d3d_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < D3D_W && y0 >= 0 && y0 < D3D_H)
            d3d_buf[y0 * D3D_W + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ── 3D rotation (Y → X → Z) returning rotated coordinates ──────── */

static void d3d_rotate3(int vx, int vy, int vz, int ay, int ax, int az,
                         int *rx, int *ry, int *rz)
{
    /* Rotate around Y axis */
    int cy = fp_cos(ay), sny = fp_sin(ay);
    int x1 = FP_MUL(vx, cy) + FP_MUL(vz, sny);
    int z1 = FP_MUL(-vx, sny) + FP_MUL(vz, cy);

    /* Rotate around X axis */
    int cx = fp_cos(ax), snx = fp_sin(ax);
    int y1 = FP_MUL(vy, cx) - FP_MUL(z1, snx);
    int z2 = FP_MUL(vy, snx) + FP_MUL(z1, cx);

    /* Rotate around Z axis */
    int cz = fp_cos(az), snz = fp_sin(az);
    *rx = FP_MUL(x1, cz) - FP_MUL(y1, snz);
    *ry = FP_MUL(x1, snz) + FP_MUL(y1, cz);
    *rz = z2;
}

/* ── Perspective projection from rotated 3D → screen 2D ──────────── */

static void d3d_project3d(int rx, int ry, int rz, int *sx, int *sy)
{
    /* Camera at z=-8; large offset reduces perspective distortion */
    int d = FP_FROM_INT(8) + rz;
    if (d < FP_ONE) d = FP_ONE;
    int scale = FP_DIV(FP_FROM_INT(240), d);
    *sx = D3D_W / 2 + (FP_MUL(rx, scale) >> FP_SHIFT);
    *sy = D3D_H / 2 - (FP_MUL(ry, scale) >> FP_SHIFT);
}

/* ── Icosahedron model (12 vertices, 20 triangular faces) ────────── */

#define ICO_1  FP_ONE                          /* 1.0 */
#define ICO_T  ((FP_ONE * 1618) / 1000)        /* φ ≈ 1.618 */

/* Vertices: (±1, ±φ, 0), (0, ±1, ±φ), (±φ, 0, ±1) scaled by 1.3 */
static const int ico_vx[12] = {
    -ICO_1,  ICO_1, -ICO_1,  ICO_1,
     0,       0,      0,      0,
     ICO_T,  ICO_T, -ICO_T, -ICO_T
};
static const int ico_vy[12] = {
     ICO_T,  ICO_T, -ICO_T, -ICO_T,
    -ICO_1,  ICO_1, -ICO_1,  ICO_1,
     0,       0,      0,      0
};
static const int ico_vz[12] = {
     0,  0,  0,  0,
     ICO_T,  ICO_T, -ICO_T, -ICO_T,
    -ICO_1,  ICO_1, -ICO_1,  ICO_1
};

/* 20 faces (CCW winding from outside) */
static const int ico_faces[20][3] = {
    {0,11,5},  {0,5,1},   {0,1,7},   {0,7,10},  {0,10,11},
    {1,5,9},   {5,11,4},  {11,10,2}, {10,7,6},  {7,1,8},
    {3,9,4},   {3,4,2},   {3,2,6},   {3,6,8},   {3,8,9},
    {4,9,5},   {2,4,11},  {6,2,10},  {8,6,7},   {9,8,1}
};

/* Base colors per face group (XRGB) — warm/cool bands for gem look */
static const uint32_t ico_base[20] = {
    0xE07050, 0xE87858, 0xD06848, 0xE07050, 0xE87858,   /* top cap: coral */
    0xE0B060, 0xD8A858, 0xE8B868, 0xE0B060, 0xD8A858,   /* upper: gold */
    0x5098D8, 0x4890D0, 0x58A0E0, 0x5098D8, 0x4890D0,   /* lower: blue */
    0x50C898, 0x48C090, 0x58D0A0, 0x50C898, 0x48C090    /* bottom cap: teal */
};

/* Light direction in view space (from upper-right-front toward scene) */
#define LIGHT_X  ((FP_ONE * 3) / 10)     /* 0.3 */
#define LIGHT_Y  ((FP_ONE * 5) / 10)     /* 0.5 */
#define LIGHT_Z  ((FP_ONE * (-8)) / 10)  /* -0.8 (into screen toward model) */

/* ── Render one frame ──────────────────────────────────────────────── */

static void d3d_render_frame(void)
{
    /* Dark gradient background (deep blue) */
    for (int y = 0; y < D3D_H; y++) {
        int b = 12 + y * 16 / D3D_H;
        int g = b / 3;
        int r = b / 4;
        uint32_t bg = 0xFF000000u | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | (uint32_t)b;
        for (int x = 0; x < D3D_W; x++)
            d3d_buf[y * D3D_W + x] = bg;
    }

    /* Subtle starfield */
    for (int i = 0; i < 50; i++) {
        int px = (i * 7919 + 1283) % D3D_W;
        int py = (i * 6271 + 947) % D3D_H;
        uint8_t bri = (uint8_t)(50 + (i * 37) % 80);
        d3d_buf[py * D3D_W + px] =
            0xFF000000u | ((uint32_t)bri << 16) |
            ((uint32_t)bri << 8) | bri;
    }

    int ay = d3d_angle_y, ax = d3d_angle_x, az = d3d_angle_z;

    /* ── Rotate all 12 vertices ──────────────────────────────────── */
    int rvx[12], rvy[12], rvz[12], scx[12], scy[12];
    for (int i = 0; i < 12; i++) {
        d3d_rotate3(ico_vx[i], ico_vy[i], ico_vz[i],
                    ay, ax, az, &rvx[i], &rvy[i], &rvz[i]);
        d3d_project3d(rvx[i], rvy[i], rvz[i], &scx[i], &scy[i]);
    }

    /* ── Per-face: backface cull, compute depth & lighting ───────── */
    struct { int idx; int depth; int raw_dot; } vis[20];
    int nvis = 0;

    for (int f = 0; f < 20; f++) {
        int i0 = ico_faces[f][0];
        int i1 = ico_faces[f][1];
        int i2 = ico_faces[f][2];

        /* Face normal in rotated 3D space (cross product of edges) */
        int e1x = rvx[i1] - rvx[i0], e1y = rvy[i1] - rvy[i0],
            e1z = rvz[i1] - rvz[i0];
        int e2x = rvx[i2] - rvx[i0], e2y = rvy[i2] - rvy[i0],
            e2z = rvz[i2] - rvz[i0];
        int nx = FP_MUL(e1y, e2z) - FP_MUL(e1z, e2y);
        int ny = FP_MUL(e1z, e2x) - FP_MUL(e1x, e2z);
        int nz = FP_MUL(e1x, e2y) - FP_MUL(e1y, e2x);

        /* Backface cull: camera looks in +Z, front faces have nz < 0 */
        if (nz >= 0) continue;

        /* Depth for painter's sort (average Z of rotated vertices) */
        int avg_z = (rvz[i0] + rvz[i1] + rvz[i2]) / 3;

        /* Diffuse lighting: dot(face_normal, light_direction) */
        int dot = FP_MUL(nx, LIGHT_X) + FP_MUL(ny, LIGHT_Y) +
                  FP_MUL(nz, LIGHT_Z);
        if (dot < 0) dot = 0;

        vis[nvis].idx = f;
        vis[nvis].depth = avg_z;
        vis[nvis].raw_dot = dot;
        nvis++;
    }

    /* Normalize shading: find max dot across visible faces */
    int max_dot = 1;
    for (int i = 0; i < nvis; i++)
        if (vis[i].raw_dot > max_dot) max_dot = vis[i].raw_dot;

    /* ── Painter's sort: draw far faces first (highest avg_z) ────── */
    for (int i = 0; i < nvis - 1; i++)
        for (int j = i + 1; j < nvis; j++)
            if (vis[j].depth > vis[i].depth) {
                int ti = vis[i].idx; vis[i].idx = vis[j].idx; vis[j].idx = ti;
                int td = vis[i].depth;
                vis[i].depth = vis[j].depth; vis[j].depth = td;
                int tr = vis[i].raw_dot;
                vis[i].raw_dot = vis[j].raw_dot; vis[j].raw_dot = tr;
            }

    /* ── Draw filled, shaded faces with edge highlights ──────────── */
    for (int fi = 0; fi < nvis; fi++) {
        int f = vis[fi].idx;
        int i0 = ico_faces[f][0];
        int i1 = ico_faces[f][1];
        int i2 = ico_faces[f][2];

        /* Compute shade (ambient + normalized diffuse) */
        int shade = 50 + (vis[fi].raw_dot * 200) / max_dot;
        if (shade > 250) shade = 250;

        /* Apply shading to face base color */
        uint32_t base = ico_base[f];
        int cr = (int)((base >> 16) & 0xFF) * shade / 255;
        int cg = (int)((base >> 8)  & 0xFF) * shade / 255;
        int cb = (int)( base        & 0xFF) * shade / 255;
        uint32_t fill = 0xFF000000u | ((uint32_t)cr << 16) |
                        ((uint32_t)cg << 8) | (uint32_t)cb;

        /* Fill the triangle */
        d3d_fill_tri(scx[i0], scy[i0], scx[i1], scy[i1],
                     scx[i2], scy[i2], fill);

        /* Bright edge highlight (faceted gem look) */
        int er = cr + (255 - cr) / 3;
        int eg = cg + (255 - cg) / 3;
        int eb = cb + (255 - cb) / 3;
        uint32_t edge = 0xFF000000u | ((uint32_t)er << 16) |
                        ((uint32_t)eg << 8) | (uint32_t)eb;
        d3d_line(scx[i0], scy[i0], scx[i1], scy[i1], edge);
        d3d_line(scx[i1], scy[i1], scx[i2], scy[i2], edge);
        d3d_line(scx[i2], scy[i2], scx[i0], scy[i0], edge);
    }

    /* Advance rotation angles (128 = full revolution) */
    d3d_angle_y += 1;
    d3d_angle_x += 1;
    d3d_angle_z += 0;
}

/* LVGL render callback: blit d3d_buf into local framebuffer during z-order pass */
static void d3d_render_cb(lv_obj_t *obj, int abs_x, int abs_y,
                           uint32_t *fb, int fb_w)
{
    (void)obj;
    if (!d3d_buf) return;
    for (int y = 0; y < D3D_H; y++) {
        int dy = abs_y + y;
        if (dy < 0) continue;
        for (int x = 0; x < D3D_W; x++) {
            int dx = abs_x + x;
            if (dx < 0) continue;
            fb[dy * fb_w + dx] = d3d_buf[y * D3D_W + x];
        }
    }
}

static void create_demo3d(void)
{
    win_demo3d = lv_win_create(lv_scr_act());
    lv_win_set_title(win_demo3d, "3D Demo");
    lv_obj_set_pos(win_demo3d, 120, 50);
    lv_obj_set_size(win_demo3d, (int16_t)(D3D_W + 4), (int16_t)(D3D_H + 28));
    lv_obj_add_event_cb(win_demo3d, on_demo3d_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_demo3d);
    lv_obj_set_style_bg_color(c, lv_color_make(0, 0, 0));
    c->render_cb = d3d_render_cb;

    /* Allocate pixel buffer */
    if (!d3d_buf)
        d3d_buf = (uint32_t *)malloc(D3D_W * D3D_H * 4);
    if (d3d_buf)
        memset(d3d_buf, 0, D3D_W * D3D_H * 4);
    d3d_angle_y = 0;
    d3d_angle_x = 0;
    d3d_angle_z = 0;

    tb_add_dyn_btn(&win_demo3d, "3D");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Terminal Emulator (multi-instance)
 * ══════════════════════════════════════════════════════════════════════ */

static term_instance_t *term_find_by_win(lv_obj_t *win)
{
    for (int i = 0; i < TERM_MAX; i++)
        if (g_terms[i].active && g_terms[i].win == win)
            return &g_terms[i];
    return NULL;
}

static term_instance_t *term_alloc(void)
{
    for (int i = 0; i < TERM_MAX; i++)
        if (!g_terms[i].active)
            return &g_terms[i];
    return NULL;
}

static void ti_clear(term_instance_t *t)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(t->buf[r], ' ', TERM_COLS);
        memset(t->attr[r], 0, TERM_COLS);
    }
    t->row = t->col = 0;
    t->dirty = 1;
}

static void ti_scroll_up(term_instance_t *t)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(t->buf[r], t->buf[r + 1], TERM_COLS);
        memcpy(t->attr[r], t->attr[r + 1], TERM_COLS);
    }
    memset(t->buf[TERM_ROWS - 1], ' ', TERM_COLS);
    memset(t->attr[TERM_ROWS - 1], 0, TERM_COLS);
    t->dirty = 1;
}

static void ti_putchar(term_instance_t *t, char c)
{
    switch (c) {
    case '\r':
        t->col = 0;
        break;
    case '\n':
        t->col = 0;
        t->row++;
        if (t->row >= TERM_ROWS) {
            ti_scroll_up(t);
            t->row = TERM_ROWS - 1;
        }
        break;
    case '\b':
        if (t->col > 0) t->col--;
        break;
    case '\t':
        t->col = (t->col + 8) & ~7;
        if (t->col >= TERM_COLS) t->col = TERM_COLS - 1;
        break;
    case '\033':
        t->esc_state = 1;
        t->esc_idx = 0;
        break;
    case '\a':
        break;
    case 0x7f:
        if (t->col > 0) {
            t->col--;
            t->buf[t->row][t->col] = ' ';
            t->attr[t->row][t->col] = 0;
        }
        break;
    default:
        if ((unsigned char)c >= 32) {
            t->buf[t->row][t->col] = c;
            t->attr[t->row][t->col] = t->cur_attr;
            t->col++;
            if (t->col >= TERM_COLS) {
                t->col = 0;
                t->row++;
                if (t->row >= TERM_ROWS) {
                    ti_scroll_up(t);
                    t->row = TERM_ROWS - 1;
                }
            }
        }
        break;
    }
    t->dirty = 1;
}

static void ti_process_csi(term_instance_t *t)
{
    int params[4] = {0, 0, 0, 0};
    int nparam = 0;
    char final_ch = t->esc_buf[t->esc_idx - 1];

    int val = 0;
    int has_val = 0;
    for (int i = 0; i < t->esc_idx - 1; i++) {
        char ch = t->esc_buf[i];
        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            has_val = 1;
        } else if (ch == ';') {
            if (nparam < 4) params[nparam++] = val;
            val = 0; has_val = 0;
        }
    }
    if (has_val && nparam < 4) params[nparam++] = val;

    switch (final_ch) {
    case 'H': case 'f':
        t->row = (nparam > 0 && params[0] > 0) ? params[0] - 1 : 0;
        t->col = (nparam > 1 && params[1] > 0) ? params[1] - 1 : 0;
        if (t->row >= TERM_ROWS) t->row = TERM_ROWS - 1;
        if (t->col >= TERM_COLS) t->col = TERM_COLS - 1;
        break;
    case 'A':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          t->row -= n; if (t->row < 0) t->row = 0;
        } break;
    case 'B':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          t->row += n; if (t->row >= TERM_ROWS) t->row = TERM_ROWS - 1;
        } break;
    case 'C':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          t->col += n; if (t->col >= TERM_COLS) t->col = TERM_COLS - 1;
        } break;
    case 'D':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          t->col -= n; if (t->col < 0) t->col = 0;
        } break;
    case 'J':
        { int mode = (nparam > 0) ? params[0] : 0;
          if (mode == 2 || mode == 3) {
              ti_clear(t);
          } else if (mode == 0) {
              memset(&t->buf[t->row][t->col], ' ', TERM_COLS - t->col);
              memset(&t->attr[t->row][t->col], 0, TERM_COLS - t->col);
              for (int r = t->row + 1; r < TERM_ROWS; r++) {
                  memset(t->buf[r], ' ', TERM_COLS);
                  memset(t->attr[r], 0, TERM_COLS);
              }
          } else if (mode == 1) {
              for (int r = 0; r < t->row; r++) {
                  memset(t->buf[r], ' ', TERM_COLS);
                  memset(t->attr[r], 0, TERM_COLS);
              }
              memset(t->buf[t->row], ' ', t->col + 1);
              memset(t->attr[t->row], 0, t->col + 1);
          }
        } break;
    case 'K':
        { int mode = (nparam > 0) ? params[0] : 0;
          if (mode == 0) {
              memset(&t->buf[t->row][t->col], ' ', TERM_COLS - t->col);
              memset(&t->attr[t->row][t->col], 0, TERM_COLS - t->col);
          } else if (mode == 1) {
              memset(t->buf[t->row], ' ', t->col + 1);
              memset(t->attr[t->row], 0, t->col + 1);
          } else if (mode == 2) {
              memset(t->buf[t->row], ' ', TERM_COLS);
              memset(t->attr[t->row], 0, TERM_COLS);
          }
        } break;
    case 'm':
        { /* SGR — Select Graphic Rendition */
          if (nparam == 0) {
              /* ESC[m with no params = reset */
              t->cur_attr = 0;
          }
          for (int p = 0; p < nparam; p++) {
              switch (params[p]) {
              case 0:  t->cur_attr = 0; break;                    /* reset */
              case 1:  t->cur_attr |= TATTR_BOLD; break;          /* bold */
              case 7:  t->cur_attr |= TATTR_REVERSE; break;       /* reverse */
              case 22: t->cur_attr &= ~TATTR_BOLD; break;         /* bold off */
              case 27: t->cur_attr &= ~TATTR_REVERSE; break;      /* reverse off */
              default: break;  /* ignore colors and other attrs */
              }
          }
        } break;
    case 'G':
        t->col = (nparam > 0 && params[0] > 0) ? params[0] - 1 : 0;
        if (t->col >= TERM_COLS) t->col = TERM_COLS - 1;
        break;
    case 'd':
        t->row = (nparam > 0 && params[0] > 0) ? params[0] - 1 : 0;
        if (t->row >= TERM_ROWS) t->row = TERM_ROWS - 1;
        break;
    case 'P':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          int rem = TERM_COLS - t->col - n;
          if (rem > 0) {
              memmove(&t->buf[t->row][t->col],
                      &t->buf[t->row][t->col + n], rem);
              memmove(&t->attr[t->row][t->col],
                      &t->attr[t->row][t->col + n], rem);
          }
          memset(&t->buf[t->row][TERM_COLS - n], ' ', n);
          memset(&t->attr[t->row][TERM_COLS - n], 0, n);
        } break;
    case '@':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          int rem = TERM_COLS - t->col - n;
          if (rem > 0) {
              memmove(&t->buf[t->row][t->col + n],
                      &t->buf[t->row][t->col], rem);
              memmove(&t->attr[t->row][t->col + n],
                      &t->attr[t->row][t->col], rem);
          }
          memset(&t->buf[t->row][t->col], ' ', n);
          memset(&t->attr[t->row][t->col], 0, n);
        } break;
    case 'h': case 'l':
        /* Private mode set/reset: ESC [ ? Pn h/l */
        /* ?1049h/l = alternate/normal screen buffer → clear screen */
        if (t->esc_idx > 0 && t->esc_buf[0] == '?' && nparam > 0 && params[0] == 1049) {
            ti_clear(t);
        }
        /* All other private modes (mouse, bracketed paste) silently ignored */
        break;
    case 'r':
        /* DECSTBM: Set Top and Bottom Margins (scroll region) — ignore for now */
        break;
    case 'L':
        /* Insert N lines at cursor, scrolling down */
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          for (int rr = TERM_ROWS - 1; rr >= t->row + n; rr--) {
              memcpy(t->buf[rr], t->buf[rr - n], TERM_COLS);
              memcpy(t->attr[rr], t->attr[rr - n], TERM_COLS);
          }
          for (int rr = t->row; rr < t->row + n && rr < TERM_ROWS; rr++) {
              memset(t->buf[rr], ' ', TERM_COLS);
              memset(t->attr[rr], 0, TERM_COLS);
          }
        } break;
    case 'M':
        /* Delete N lines at cursor, scrolling up */
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          for (int rr = t->row; rr < TERM_ROWS - n; rr++) {
              memcpy(t->buf[rr], t->buf[rr + n], TERM_COLS);
              memcpy(t->attr[rr], t->attr[rr + n], TERM_COLS);
          }
          for (int rr = TERM_ROWS - n; rr < TERM_ROWS; rr++) {
              memset(t->buf[rr], ' ', TERM_COLS);
              memset(t->attr[rr], 0, TERM_COLS);
          }
        } break;
    case 'X':
        /* Erase N characters (fill with spaces, don't move cursor) */
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          int end = t->col + n;
          if (end > TERM_COLS) end = TERM_COLS;
          memset(&t->buf[t->row][t->col], ' ', end - t->col);
          memset(&t->attr[t->row][t->col], 0, end - t->col);
        } break;
    case 'S':
        /* Scroll up N lines */
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          for (int s = 0; s < n; s++) ti_scroll_up(t);
        } break;
    case 'T':
        /* Scroll down N lines */
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          for (int s = 0; s < n; s++) {
              for (int rr = TERM_ROWS - 1; rr > 0; rr--) {
                  memcpy(t->buf[rr], t->buf[rr-1], TERM_COLS);
                  memcpy(t->attr[rr], t->attr[rr-1], TERM_COLS);
              }
              memset(t->buf[0], ' ', TERM_COLS);
              memset(t->attr[0], 0, TERM_COLS);
          }
        } break;
    default:
        /* Log unhandled CSI sequences */
        { static int unk_csi = 0;
          if (unk_csi < 20) {
              unk_csi++;
              char seq[64];
              int slen = t->esc_idx < 60 ? t->esc_idx : 60;
              memcpy(seq, t->esc_buf, slen);
              seq[slen] = '\0';
              fprintf(stderr, "[term-csi?] unknown: ESC[%s (final='%c')\n", seq, final_ch);
          }
        }
        break;
    }
    t->dirty = 1;
}

static int g_hexdump_done = 0;

static void ti_feed(term_instance_t *t, const char *data, int len)
{
    /* One-time hex dump of first large data burst (likely links rendering) */
    if (!g_hexdump_done && len > 100) {
        g_hexdump_done = 1;
        int dlen = len < 256 ? len : 256;
        fprintf(stderr, "[term-hex] %d bytes (showing %d):\n", len, dlen);
        for (int hi = 0; hi < dlen; hi += 16) {
            fprintf(stderr, "  %04x: ", hi);
            for (int hj = 0; hj < 16 && hi+hj < dlen; hj++)
                fprintf(stderr, "%02x ", (unsigned char)data[hi+hj]);
            fprintf(stderr, " |");
            for (int hj = 0; hj < 16 && hi+hj < dlen; hj++) {
                unsigned char hc = data[hi+hj];
                fprintf(stderr, "%c", (hc >= 32 && hc < 127) ? hc : '.');
            }
            fprintf(stderr, "|\n");
        }
    }
    for (int i = 0; i < len; i++) {
        char c = data[i];

        if (t->esc_state == 1) {
            if (c == '[') {
                t->esc_state = 2;
                t->esc_idx = 0;
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                /* Charset designation: ESC ( X, ESC ) X — eat next byte */
                t->esc_state = 3;
            } else if (c == 'M') {
                /* ESC M = Reverse Index: move cursor up, scroll down if at top */
                if (t->row > 0) {
                    t->row--;
                } else {
                    /* Scroll down: shift all rows down by 1 */
                    for (int r = TERM_ROWS - 1; r > 0; r--) {
                        memcpy(t->buf[r], t->buf[r-1], TERM_COLS);
                        memcpy(t->attr[r], t->attr[r-1], TERM_COLS);
                    }
                    memset(t->buf[0], ' ', TERM_COLS);
                    memset(t->attr[0], 0, TERM_COLS);
                    t->dirty = 1;
                }
                t->esc_state = 0;
            } else if (c == '7') {
                /* ESC 7 = Save cursor position */
                t->saved_row = t->row;
                t->saved_col = t->col;
                t->esc_state = 0;
            } else if (c == '8') {
                /* ESC 8 = Restore cursor position */
                t->row = t->saved_row;
                t->col = t->saved_col;
                t->esc_state = 0;
            } else {
                /* Other single-char sequences — ignore */
                t->esc_state = 0;
            }
            continue;
        }

        if (t->esc_state == 3) {
            /* Eat the charset designator byte (e.g. '0', 'B') */
            t->esc_state = 0;
            continue;
        }

        if (t->esc_state == 2) {
            if (t->esc_idx < TERM_ESC_MAX)
                t->esc_buf[t->esc_idx++] = c;
            if (c >= 0x40 && c <= 0x7E) {
                ti_process_csi(t);
                t->esc_state = 0;
            } else if (t->esc_idx >= TERM_ESC_MAX) {
                t->esc_state = 0;
            }
            continue;
        }

        ti_putchar(t, c);
    }
}

/*
 * Custom render callback: draw the terminal buffer cell-by-cell directly
 * into the framebuffer, with proper reverse-video support.
 */
static int g_term_render_dbg_count = 0;
static int g_term_bufdump_done = 0;

static void term_render_cb(struct lv_obj *obj, int ax, int ay,
                           uint32_t *fb, int fb_w)
{
    term_instance_t *t = (term_instance_t *)obj->user_data;
    if (!t || !t->active) return;

    lv_disp_t *disp = lv_disp_get();
    int fb_h = disp ? (int)disp->height : 768;

    if (g_term_render_dbg_count < 4) {
        g_term_render_dbg_count++;
        fprintf(stderr, "[term-render] ax=%d ay=%d obj_w=%d obj_h=%d fb_w=%d fb_h=%d buf00='%c' row=%d col=%d\n",
                ax, ay, obj->w, obj->h, fb_w, fb_h,
                t->buf[0][0] >= 32 ? t->buf[0][0] : '?', t->row, t->col);
    }

    lv_color_t fg_normal = lv_color_make(192, 192, 192);
    lv_color_t bg_normal = lv_color_make(0, 0, 0);
    lv_color_t fg_reverse = lv_color_make(0, 0, 0);
    lv_color_t bg_reverse = lv_color_make(192, 192, 192);

    int is_focused = (g_term_focus >= 0 && &g_terms[g_term_focus] == t);

    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int px = ax + c * LV_FONT_WIDTH;
            int py = ay + r * LV_FONT_HEIGHT;
            char ch = t->buf[r][c];
            uint8_t at = t->attr[r][c];
            int rev = !!(at & TATTR_REVERSE);

            /* Cursor: show as reverse block on focused terminal */
            if (is_focused && r == t->row && c == t->col)
                rev = !rev;

            lv_color_t fg = rev ? fg_reverse : fg_normal;
            lv_color_t bg = rev ? bg_reverse : bg_normal;

            /* Draw cell background if reversed */
            if (rev) {
                uint32_t pixel = ((uint32_t)bg.alpha << 24) |
                                 ((uint32_t)bg.red << 16) |
                                 ((uint32_t)bg.green << 8) |
                                 (uint32_t)bg.blue;
                for (int yr = 0; yr < LV_FONT_HEIGHT; yr++) {
                    int yy = py + yr;
                    if (yy < 0 || yy >= fb_h) continue;
                    for (int xr = 0; xr < LV_FONT_WIDTH; xr++) {
                        int xx = px + xr;
                        if (xx < 0 || xx >= fb_w) continue;
                        fb[yy * fb_w + xx] = pixel;
                    }
                }
            }

            /* Draw character glyph */
            if (ch >= 32 && ch <= 126)
                lv_font_draw_char(fb, fb_w, fb_h, px, py, ch, fg);
        }
    }

    /* One-time diagnostic: verify pixels are actually drawn */
    if (!g_term_bufdump_done && t->buf[0][0] != ' ') {
        g_term_bufdump_done = 1;

        /* Draw a bright GREEN 8x8 block at top-left of terminal area.
         * If user can see this green block, the framebuffer pipeline works. */
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                int gpx = ax + gx, gpy = ay + gy;
                if (gpx >= 0 && gpx < fb_w && gpy >= 0 && gpy < fb_h)
                    fb[gpy * fb_w + gpx] = 0xFF00FF00; /* bright green */
            }
        }

        /* Multi-point pixel sampling of the first non-space char */
        int sr = 0, sc = -1;
        for (int c = 0; c < TERM_COLS; c++) {
            if (t->buf[sr][c] != ' ') { sc = c; break; }
        }
        if (sc >= 0) {
            char ch = t->buf[sr][sc];
            int cpx = ax + sc * LV_FONT_WIDTH;
            int cpy = ay + sr * LV_FONT_HEIGHT;
            fprintf(stderr, "[term-pixel-multi] char='%c' at cell(%d,%d) px=(%d,%d):\n",
                    ch, sr, sc, cpx, cpy);
            /* Sample 8 positions across the glyph */
            for (int col = 0; col < 8; col++) {
                int spx = cpx + col;
                int spy = cpy + 6; /* row 6 in glyph — most chars have pixels here */
                if (spx >= 0 && spx < fb_w && spy >= 0 && spy < fb_h) {
                    uint32_t pval = fb[spy * fb_w + spx];
                    fprintf(stderr, "  col%d: (%d,%d)=0x%08x%s\n",
                            col, spx, spy, pval,
                            pval != 0xFF000000 ? " <-- NON-BLACK" : "");
                }
            }
            /* Count non-black pixels in the first 3 char cells */
            int nonblack = 0;
            for (int r = 0; r < LV_FONT_HEIGHT; r++) {
                for (int c2 = 0; c2 < 3 * LV_FONT_WIDTH; c2++) {
                    int px2 = cpx + c2, py2 = cpy + r;
                    if (px2 >= 0 && px2 < fb_w && py2 >= 0 && py2 < fb_h) {
                        if (fb[py2 * fb_w + px2] != 0xFF000000)
                            nonblack++;
                    }
                }
            }
            fprintf(stderr, "[term-pixel-count] %d non-black pixels in first 3 chars\n",
                    nonblack);
        }

        /* Report clip state — if clipped, font rendering may be hidden */
        extern int g_clip_active, g_clip_x1, g_clip_y1, g_clip_x2, g_clip_y2;
        fprintf(stderr, "[term-clip] g_clip_active=%d",  g_clip_active);
        if (g_clip_active)
            fprintf(stderr, " rect=(%d,%d)-(%d,%d)", g_clip_x1, g_clip_y1,
                    g_clip_x2, g_clip_y2);
        fprintf(stderr, "\n");
    }
}

static void ti_refresh_dirty(term_instance_t *t)
{
    if (!t->dirty) return;
    /* The actual rendering is done by term_render_cb during the
     * LVGL render pass.  We just need to make sure the widget is
     * marked dirty so it gets redrawn. */
    t->dirty = 0;
}

/* Focus a terminal — route keyboard to it */
static void term_focus(int idx)
{
    if (idx < 0 || idx >= TERM_MAX || !g_terms[idx].active) {
        g_term_focus = -1;
        lv_indev_set_kbd_callback(NULL, NULL);
        return;
    }
    g_term_focus = idx;
    /* Bring the window to front */
    lv_obj_move_to_front(g_terms[idx].win);
    /* Mark focused terminal's dirty so cursor updates */
    g_terms[idx].dirty = 1;
}

/* Relayout all open-app buttons in the taskbar */
static void tb_relayout_app_btns(void)
{
    if (!g_taskbar) return;
    int16_t x = g_tb_app_x0;
    /* Dynamic (non-terminal) buttons first */
    for (int i = 0; i < DYN_TB_MAX; i++) {
        if (!g_dyn_tb[i].btn) continue;
        lv_obj_set_pos(g_dyn_tb[i].btn, x, 0);
        x += g_tb_app_btn_w + 4;
    }
    /* Terminal buttons */
    for (int i = 0; i < TERM_MAX; i++) {
        if (!g_terms[i].active || !g_terms[i].tb_btn) continue;
        lv_obj_set_pos(g_terms[i].tb_btn, x, 0);
        x += g_tb_app_btn_w + 4;
    }
}

/* Add a dynamic taskbar button when a window is opened */
static void tb_add_dyn_btn(lv_obj_t **win_ptr, const char *label)
{
    if (!g_taskbar) return;
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < DYN_TB_MAX; i++) {
        if (!g_dyn_tb[i].btn) { slot = i; break; }
    }
    if (slot < 0) return;

    lv_obj_t *b = lv_btn_create(g_taskbar);
    lv_btn_set_text(b, label);
    lv_obj_set_size(b, g_tb_app_btn_w, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(50, 55, 65));
    lv_obj_add_event_cb(b, tb_dyn_btn_cb, LV_EVENT_CLICKED, NULL);
    g_dyn_tb[slot].btn = b;
    g_dyn_tb[slot].win_ptr = win_ptr;
    tb_relayout_app_btns();
}

/* Remove a dynamic taskbar button when a window is closed */
static void tb_remove_dyn_btn(lv_obj_t **win_ptr)
{
    for (int i = 0; i < DYN_TB_MAX; i++) {
        if (g_dyn_tb[i].win_ptr == win_ptr && g_dyn_tb[i].btn) {
            lv_obj_del(g_dyn_tb[i].btn);
            g_dyn_tb[i].btn = NULL;
            g_dyn_tb[i].win_ptr = NULL;
            tb_relayout_app_btns();
            return;
        }
    }
}

/* Dynamic taskbar button click: bring corresponding window to front */
static void tb_dyn_btn_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    for (int i = 0; i < DYN_TB_MAX; i++) {
        if (g_dyn_tb[i].btn == obj && g_dyn_tb[i].win_ptr) {
            lv_obj_t *w = *g_dyn_tb[i].win_ptr;
            if (w) lv_obj_move_to_front(w);
            return;
        }
    }
}

/* Menu button callback: toggle popup visibility */
static void menu_btn_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    if (!g_menu_popup) return;
    g_menu_popup->visible = !g_menu_popup->visible;
    if (g_menu_popup->visible)
        lv_obj_move_to_front(g_menu_popup);
}

/* Taskbar app-button click: focus the corresponding terminal */
static void tb_app_btn_cb(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLICKED) return;
    for (int i = 0; i < TERM_MAX; i++) {
        if (g_terms[i].active && g_terms[i].tb_btn == obj) {
            term_focus(i);
            lv_indev_set_kbd_callback(term_kbd_cb, NULL);
            return;
        }
    }
}

static void term_kbd_cb(lv_kbd_event_t *ev, void *user_data)
{
    (void)user_data;
    if (!ev->pressed || g_term_focus < 0) return;
    term_instance_t *t = &g_terms[g_term_focus];
    if (!t->active || t->master_fd < 0) return;

    uint8_t key = ev->keycode;

    if (key >= LV_KEY_UP && key <= LV_KEY_DELETE) {
        const char *seq = NULL;
        switch (key) {
        case LV_KEY_UP:     seq = "\033[A"; break;
        case LV_KEY_DOWN:   seq = "\033[B"; break;
        case LV_KEY_RIGHT:  seq = "\033[C"; break;
        case LV_KEY_LEFT:   seq = "\033[D"; break;
        case LV_KEY_HOME:   seq = "\033[H"; break;
        case LV_KEY_END:    seq = "\033[F"; break;
        case LV_KEY_DELETE: seq = "\033[3~"; break;
        case LV_KEY_PGUP:   seq = "\033[5~"; break;
        case LV_KEY_PGDN:   seq = "\033[6~"; break;
        default: break;
        }
        if (seq) write(t->master_fd, seq, strlen(seq));
        return;
    }

    if (key == 0) return;

    char c = (char)key;
    if (c == '\n') c = '\r';

    static int kbd_dbg = 0;
    ssize_t wr = write(t->master_fd, &c, 1);
    if (kbd_dbg < 10) {
        kbd_dbg++;
        fprintf(stderr, "[term-kbd] key=0x%02x wr=%d fd=%d\n", (unsigned char)c, (int)wr, t->master_fd);
    }
}

static void term_poll_all(void)
{
    for (int i = 0; i < TERM_MAX; i++) {
        term_instance_t *t = &g_terms[i];
        if (!t->active || t->master_fd < 0) continue;

        char buf[512];
        int total = 0;
        for (;;) {
            int n = read(t->master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            ti_feed(t, buf, n);
            total += n;
        }
        if (total > 0) {
            static int poll_dbg = 0;
            if (poll_dbg < 20) {
                poll_dbg++;
                fprintf(stderr, "[term-poll] term %d: read %d bytes, buf[0][0]='%c'(0x%02x) row=%d col=%d esc=%d\n",
                        i, total, t->buf[0][0] >= 32 ? t->buf[0][0] : '?',
                        (unsigned char)t->buf[0][0], t->row, t->col, t->esc_state);
            }
            /* Dump buffer after large read (links rendering) — separate counter
             * so it fires even if the render_cb dump already triggered. */
            {
                static int links_dump_count = 0;
                if (total > 500 && links_dump_count < 3) {
                    links_dump_count++;
                    fprintf(stderr, "[term-links-dump] after %d bytes, rows 0-%d:\n",
                            total, TERM_ROWS - 1);
                    for (int dr = 0; dr < TERM_ROWS; dr++) {
                        char line[TERM_COLS + 1];
                        memcpy(line, t->buf[dr], TERM_COLS);
                        line[TERM_COLS] = '\0';
                        for (int e = TERM_COLS - 1; e >= 0 && line[e] == ' '; e--)
                            line[e] = '\0';
                        if (line[0] != '\0')
                            fprintf(stderr, "  row%02d: \"%s\"\n", dr, line);
                    }
                    /* Show attr map for rows with content */
                    for (int dr = 0; dr < 3; dr++) {
                        fprintf(stderr, "  attr%02d:", dr);
                        for (int c = 0; c < TERM_COLS; c++)
                            fprintf(stderr, "%x", t->attr[dr][c]);
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
        ti_refresh_dirty(t);
    }
}

static void create_terminal(void)
{
    term_instance_t *t = term_alloc();
    if (!t) return;  /* all slots full */

    /* Open PTY master */
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) return;

    unsigned int idx = 0;
    if (ioctl(master, TIOCGPTN, &idx) < 0) {
        close(master);
        return;
    }

    char pts_path[32];
    snprintf(pts_path, sizeof(pts_path), "/dev/pts/%u", idx);

    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);

    int pid = fork();
    if (pid < 0) {
        close(master);
        return;
    }

    if (pid == 0) {
        close(master);
        setsid();

        int slave = open(pts_path, O_RDWR);
        if (slave < 0) _exit(1);

        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = TERM_ROWS;
        ws.ws_col = TERM_COLS;
        ioctl(slave, TIOCSWINSZ, &ws);

        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);

        char *argv[] = { "sh", NULL };
        char *envp[] = { "TERM=dumb", "HOME=/", "PATH=/bin:/usr/bin", NULL };
        execve("/bin/sh", argv, envp);
        _exit(1);
    }

    /* Parent: initialize instance */
    int slot = (int)(t - g_terms);
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->master_fd = master;
    t->shell_pid = pid;
    t->id = g_term_next_id++;
    ti_clear(t);

    /* Position windows in a cascade pattern */
    int16_t win_w = (int16_t)(TERM_COLS * LV_FONT_WIDTH + 16);
    int16_t win_h = (int16_t)(TERM_ROWS * LV_FONT_HEIGHT + LV_WIN_TITLE_H + 8);
    int16_t off = (int16_t)((slot % 4) * 30);

    char title[32];
    snprintf(title, sizeof(title), "Terminal %d", t->id);

    t->win = lv_win_create(lv_scr_act());
    lv_win_set_title(t->win, title);
    lv_obj_set_pos(t->win, (int16_t)(40 + off), (int16_t)(20 + off));
    lv_obj_set_size(t->win, win_w, win_h);
    lv_obj_add_event_cb(t->win, on_terminal_event, 0, NULL);

    lv_obj_t *c = lv_win_get_content(t->win);
    lv_obj_set_style_bg_color(c, lv_color_make(0, 0, 0));
    c->padding = 0;  /* no padding — terminal fills entire content area */
    c->render_cb = term_render_cb;
    c->user_data = t;
    t->label = NULL; /* rendering handled by term_render_cb */
    fprintf(stderr, "[term-create] slot=%d win=%p content=%p render_cb=%p user_data=%p\n",
            slot, (void*)t->win, (void*)c, (void*)(uintptr_t)c->render_cb, c->user_data);

    /* Focus this new terminal */
    term_focus(slot);
    lv_indev_set_kbd_callback(term_kbd_cb, NULL);

    /* Add taskbar button for this terminal */
    if (g_taskbar) {
        t->tb_btn = lv_btn_create(g_taskbar);
        lv_btn_set_text(t->tb_btn, title);
        lv_obj_set_size(t->tb_btn, g_tb_app_btn_w, 28);
        lv_obj_set_style_bg_color(t->tb_btn, lv_color_make(50, 80, 120));
        lv_obj_add_event_cb(t->tb_btn, tb_app_btn_cb, LV_EVENT_CLICKED, NULL);
        tb_relayout_app_btns();
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Main — Desktop setup
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    int skip_login = 0;
    int pending_res = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skip-login") == 0)
            skip_login = 1;
        else if (strncmp(argv[i], "--pending-res=", 14) == 0) {
            /* Parse old resolution: --pending-res=WxH */
            unsigned int pw = 0, ph = 0;
            const char *p = argv[i] + 14;
            while (*p >= '0' && *p <= '9') { pw = pw * 10 + (*p - '0'); p++; }
            if (*p == 'x') p++;
            while (*p >= '0' && *p <= '9') { ph = ph * 10 + (*p - '0'); p++; }
            if (pw > 0 && ph > 0) {
                res_old_w = pw;
                res_old_h = ph;
                pending_res = 1;
            }
        }
    }

    if (lv_init() < 0)
        return 1;

    lv_obj_t *scr = lv_scr_act();
    lv_disp_t *d = lv_disp_get();
    int16_t scr_w = (int16_t)d->width;
    int16_t scr_h = (int16_t)d->height;

    /* ── Login screen ──────────────────────────────────────────── */
    if (!skip_login)
    {
        lv_obj_set_style_bg_color(scr, lv_color_make(18, 32, 52));
        lv_refr_now();

        /* Login panel (centered box) */
        int16_t pw = 320, ph = 260;
        int16_t px = (int16_t)((scr_w - pw) / 2);
        int16_t py = (int16_t)((scr_h - ph) / 2);

        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, px, py);
        lv_obj_set_size(panel, pw, ph);
        lv_obj_set_style_bg_color(panel, lv_color_make(30, 38, 52));
        lv_obj_set_style_border_width(panel, 1);
        lv_obj_set_style_border_color(panel, lv_color_make(60, 70, 90));
        lv_obj_set_style_pad_all(panel, 20);

        /* Title */
        lv_obj_t *title = lv_label_create(panel);
        lv_label_set_text(title, "xv6 Login");
        lv_obj_set_style_text_color(title, lv_color_make(140, 180, 230));
        lv_obj_set_pos(title, 95, 10);

        /* Username label + textbox */
        lv_obj_t *ul = lv_label_create(panel);
        lv_label_set_text(ul, "Username:");
        lv_obj_set_style_text_color(ul, lv_color_make(180, 180, 190));
        lv_obj_set_pos(ul, 20, 50);

        lv_obj_t *user_tb = lv_textbox_create(panel);
        lv_obj_set_pos(user_tb, 20, 70);
        lv_obj_set_size(user_tb, 240, 24);
        lv_textbox_set_text(user_tb, "root");
        lv_textbox_set_max_length(user_tb, 32);

        /* Password label + textbox */
        lv_obj_t *pl = lv_label_create(panel);
        lv_label_set_text(pl, "Password:");
        lv_obj_set_style_text_color(pl, lv_color_make(180, 180, 190));
        lv_obj_set_pos(pl, 20, 106);

        lv_obj_t *pass_tb = lv_textbox_create(panel);
        lv_obj_set_pos(pass_tb, 20, 126);
        lv_obj_set_size(pass_tb, 240, 24);
        lv_textbox_set_text(pass_tb, "");
        lv_textbox_set_max_length(pass_tb, 64);

        /* Log In button */
        volatile int login_clicked = 0;
        lv_obj_t *login_btn = lv_btn_create(panel);
        lv_btn_set_text(login_btn, "Log In");
        lv_obj_set_pos(login_btn, 90, 170);
        lv_obj_set_size(login_btn, 100, 34);
        lv_obj_set_style_bg_color(login_btn, lv_color_make(40, 90, 160));
        lv_obj_add_event_cb(login_btn, flag_btn_cb, LV_EVENT_CLICKED,
                            (void *)&login_clicked);

        /* Status label for errors */
        lv_obj_t *status_lbl = lv_label_create(panel);
        lv_label_set_text(status_lbl, "");
        lv_obj_set_style_text_color(status_lbl, lv_color_make(220, 80, 80));
        lv_obj_set_pos(status_lbl, 20, 215);

        lv_refr_now();

        /* Login event loop */
        while (g_running) {
            lv_timer_handler();

            /* Check if login button was clicked */
            if (login_clicked) {
                login_clicked = 0;

                const char *uname = lv_textbox_get_text(user_tb);
                const char *pwd   = lv_textbox_get_text(pass_tb);

                /* Validate: check /etc/passwd for the user */
                int valid = 0;
                char line[256];
                FILE *fp = fopen("/etc/passwd", "r");
                if (fp) {
                    while (fgets(line, sizeof(line), fp)) {
                        /* Format: user:password:uid:gid:... */
                        char *colon1 = strchr(line, ':');
                        if (!colon1) continue;
                        *colon1 = '\0';
                        if (strcmp(line, uname) != 0) continue;
                        /* Found user — check password */
                        char *colon2 = strchr(colon1 + 1, ':');
                        if (colon2) *colon2 = '\0';
                        const char *stored_pw = colon1 + 1;
                        /* Empty stored password or "x" means no password */
                        if (stored_pw[0] == '\0' || strcmp(stored_pw, "x") == 0) {
                            valid = 1;
                        } else if (strcmp(stored_pw, pwd) == 0) {
                            valid = 1;
                        }
                        break;
                    }
                    fclose(fp);
                } else {
                    /* No /etc/passwd — allow root with empty password */
                    if (strcmp(uname, "root") == 0 &&
                        (pwd[0] == '\0'))
                        valid = 1;
                }

                if (valid) {
                    break;  /* proceed to desktop */
                } else {
                    lv_label_set_text(status_lbl,
                                      "Invalid username or password");
                }
            }

            usleep(16000);
        }

        /* Remove all login screen objects */
        lv_obj_del(panel);
    }

    if (!g_running) {
        lv_deinit();
        return 0;
    }

    /* Desktop background color (dark blue) */
    lv_obj_set_style_bg_color(scr, lv_color_make(26, 58, 92));

    /* Immediately render one frame to prove rendering works */
    lv_refr_now();

    /* ── Desktop label ─────────────────────────────────────────── */
    lv_obj_t *desk_label = lv_label_create(scr);
    lv_label_set_text(desk_label, "xv6 Desktop");
    lv_obj_set_style_text_color(desk_label, lv_color_make(60, 90, 130));
    lv_obj_set_pos(desk_label, (int16_t)(scr_w / 2 - 48), (int16_t)(scr_h / 2 - 40));

    /* ── Taskbar ───────────────────────────────────────────────── */
    int16_t tb_h = 36;
    lv_obj_t *taskbar = lv_obj_create(scr);
    g_taskbar = taskbar;
    lv_obj_set_pos(taskbar, 0, (int16_t)(scr_h - tb_h));
    lv_obj_set_size(taskbar, scr_w, tb_h);
    lv_obj_set_style_bg_color(taskbar, lv_color_make(30, 30, 33));
    lv_obj_set_style_border_width(taskbar, 0);
    lv_obj_set_style_pad_all(taskbar, 3);

    /* Menu button on taskbar */
    int16_t bx = 4;
    lv_obj_t *b;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "Menu");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 55, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(60, 80, 120));
    lv_obj_add_event_cb(b, menu_btn_cb, LV_EVENT_CLICKED, NULL);
    bx += 62;

    /* Dynamic app buttons start here */
    g_tb_app_x0 = bx;

    /* ── Menu popup (above taskbar) ────────────────────────────── */
    {
        int16_t mcols = 2, mrows = 7;
        int16_t mbw = 88, mbh = 26, mgap = 4, mpad = 6;
        int16_t mw = (int16_t)(mcols * (mbw + mgap) + mpad * 2);
        int16_t mh = (int16_t)(mrows * (mbh + mgap) + mpad * 2);

        g_menu_popup = lv_obj_create(scr);
        lv_obj_set_pos(g_menu_popup, 0, (int16_t)(scr_h - tb_h - mh));
        lv_obj_set_size(g_menu_popup, mw, mh);
        lv_obj_set_style_bg_color(g_menu_popup, lv_color_make(35, 35, 40));
        lv_obj_set_style_border_width(g_menu_popup, 1);
        lv_obj_set_style_border_color(g_menu_popup, lv_color_make(80, 80, 90));
        lv_obj_set_style_pad_all(g_menu_popup, mpad);
        lv_obj_set_hidden(g_menu_popup, 1);

        /* Helper macro for menu item placement */
        int16_t mi = 0;
        #define MENU_ITEM(label, cb) do { \
            int16_t col = mi % mcols, row = mi / mcols; \
            lv_obj_t *mb = lv_btn_create(g_menu_popup); \
            lv_btn_set_text(mb, label); \
            lv_obj_set_pos(mb, (int16_t)(col * (mbw + mgap)), \
                               (int16_t)(row * (mbh + mgap))); \
            lv_obj_set_size(mb, mbw, mbh); \
            lv_obj_set_style_bg_color(mb, lv_color_make(55, 55, 65)); \
            lv_obj_add_event_cb(mb, cb, LV_EVENT_CLICKED, NULL); \
            mi++; \
        } while(0)

        MENU_ITEM("Info",     btn_sysinfo_cb);
        MENU_ITEM("Procs",    btn_procmgr_cb);
        MENU_ITEM("Monitor",  btn_sysmon_cb);
        MENU_ITEM("Files",    btn_filemgr_cb);
        MENU_ITEM("Network",  btn_netinfo_cb);
        MENU_ITEM("Calc",     btn_calc_cb);
        MENU_ITEM("Settings", btn_settings_cb);
        MENU_ITEM("About",    btn_about_cb);
        MENU_ITEM("Terminal", btn_terminal_cb);
        MENU_ITEM("Power",    btn_power_cb);
        MENU_ITEM("Users",    btn_usermgr_cb);
        MENU_ITEM("Internet", btn_netcfg_cb);
        MENU_ITEM("3D Demo",  btn_demo3d_cb);
        MENU_ITEM("Log Out",  btn_logout_cb);

        #undef MENU_ITEM
    }

    /* Taskbar title */
    lv_obj_t *tb_title = lv_label_create(taskbar);
    lv_label_set_text(tb_title, "xv6");
    lv_obj_set_style_text_color(tb_title, lv_color_make(140, 140, 150));
    lv_obj_set_pos(tb_title, (int16_t)(scr_w - 210), 6);

    /* Clock label (right side of taskbar) */
    g_clock_lbl = lv_label_create(taskbar);
    lv_label_set_text(g_clock_lbl, "00:00:00");
    lv_obj_set_style_text_color(g_clock_lbl, lv_color_make(100, 200, 255));
    lv_obj_set_pos(g_clock_lbl, (int16_t)(scr_w - 140), 6);
    g_clock_refresh_ctr = 0;

    /* Show resolution confirmation dialog if we just changed resolution */
    if (pending_res)
        create_res_confirm();

    /* ── Main loop ─────────────────────────────────────────────── */

    while (g_running) {
        /* Pre-render 3D scene into buffer (LVGL's render_cb will blit it) */
        if (win_demo3d && d3d_buf)
            d3d_render_frame();

        lv_timer_handler();
        term_poll_all();
        { static int __hb = 0;
          if (++__hb % 120 == 0)
              fprintf(stderr, "[desktop-hb] frame %d, terms active: t0=%d t1=%d\n",
                      __hb, g_terms[0].active, g_terms[1].active);
        }

        /* Update dynamic taskbar button colors based on window state */
        {
            /* Find topmost window by scanning screen children from back */
            lv_obj_t *scr_root = lv_scr_act();
            lv_obj_t *topwin = NULL;
            for (int i = scr_root->child_count - 1; i >= 0; i--) {
                if (scr_root->children[i]->type == LV_OBJ_TYPE_WINDOW &&
                    scr_root->children[i]->visible) {
                    topwin = scr_root->children[i];
                    break;
                }
            }
            lv_color_t c_fg = lv_color_make(60, 140, 220);
            lv_color_t c_bg = lv_color_make(50, 55, 65);
            /* Dynamic app buttons */
            for (int i = 0; i < DYN_TB_MAX; i++) {
                if (!g_dyn_tb[i].btn || !g_dyn_tb[i].win_ptr) continue;
                lv_obj_t *w = *g_dyn_tb[i].win_ptr;
                g_dyn_tb[i].btn->bg_color = (w && w == topwin) ? c_fg : c_bg;
            }
            /* Terminal buttons */
            for (int t = 0; t < TERM_MAX; t++) {
                if (!g_terms[t].active || !g_terms[t].tb_btn) continue;
                g_terms[t].tb_btn->bg_color =
                    (g_terms[t].win == topwin) ? c_fg : c_bg;
            }
        }

        /* Resolution confirmation countdown */
        if (win_res_confirm) {
            if (res_confirm_keep) {
                /* User accepted — dismiss dialog */
                lv_obj_del(win_res_confirm);
                win_res_confirm = NULL;
                res_confirm_lbl = NULL;
            } else if (res_confirm_revert || res_countdown <= 0) {
                /* User clicked revert or timeout — revert resolution */
                revert_resolution();  /* does not return */
            } else {
                /* Tick countdown (~60 fps) */
                if (++res_countdown_ctr >= 60) {
                    res_countdown_ctr = 0;
                    res_countdown--;
                    if (res_confirm_lbl) {
                        char cbuf[60];
                        snprintf(cbuf, sizeof(cbuf),
                                 "Reverting in %d seconds...", res_countdown);
                        lv_label_set_text(res_confirm_lbl, cbuf);
                    }
                }
            }
        }

        /* Auto-refresh system monitor (~1 Hz) */
        if (win_sysmon && ++sysmon_refresh_ctr >= 60) {
            sysmon_refresh_ctr = 0;
            sysmon_refresh();
        }

        /* Update clock (~1 Hz) */
        if (++g_clock_refresh_ctr >= 60) {
            g_clock_refresh_ctr = 0;
            if (g_clock_lbl) {
                struct timeval tv;
                if (gettimeofday(&tv, NULL) == 0) {
                    /* Convert to HH:MM:SS (UTC) */
                    unsigned long total_secs = (unsigned long)tv.tv_sec;
                    unsigned int h = (unsigned int)((total_secs / 3600) % 24);
                    unsigned int m = (unsigned int)((total_secs / 60) % 60);
                    unsigned int s = (unsigned int)(total_secs % 60);
                    char tbuf[12];
                    snprintf(tbuf, sizeof(tbuf), "%02u:%02u:%02u", h, m, s);
                    lv_label_set_text(g_clock_lbl, tbuf);
                }
            }
        }

        usleep(16000);  /* ~60 fps */
    }

    /* Clean up any open terminals */
    for (int i = 0; i < TERM_MAX; i++) {
        if (g_terms[i].active) {
            if (g_terms[i].shell_pid > 0) {
                kill(g_terms[i].shell_pid, SIGKILL);
                waitpid(g_terms[i].shell_pid, NULL, 0);
            }
            if (g_terms[i].master_fd >= 0)
                close(g_terms[i].master_fd);
        }
    }

    /* Clear framebuffer to black before exiting */
    {
        lv_disp_t *d = lv_disp_get();
        if (d->framebuf && d->fb_size > 0) {
            memset(d->framebuf, 0, d->fb_size);
            /* Flush the black screen */
            if (d->gpu_accel) {
                struct fb_gpu_blit cmd;
                cmd.x = 0; cmd.y = 0;
                cmd.w = d->width; cmd.h = d->height;
                cmd.src_pitch = d->width * 4;
                cmd.pixels = (uint64_t)(uintptr_t)d->framebuf;
                ioctl(d->fb_fd, FB_GPU_BLIT, &cmd);
            } else {
                lseek(d->fb_fd, 0, SEEK_SET);
                write(d->fb_fd, d->framebuf, d->fb_size);
            }
        }
    }

    lv_deinit();

    /* Re-exec the desktop to restart the session */
    char *re_argv[] = { "desktop", NULL };
    char *re_envp[] = { "TERM=dumb", "HOME=/", "PATH=/bin:/usr/bin", NULL };
    execve("/bin/desktop", re_argv, re_envp);

    /* If execve fails, just exit */
    return 0;
}
