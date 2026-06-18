#include <types.h>
#include <param.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <mm/vm.h>
#include <vfs/fcntl.h>
#include <vfs/file.h>
#include <vfs/vfs_types.h>
#include <cmdline.h>
#include "kqueue.h"
#include "kqueue_types.h"
#include "lock/spinlock.h"
#include "proc/sched.h"
#include "printf.h"
#include "uabi/poll.h"
#include "dev/virtio.h"

#define OSS_SOUND_MAJOR 14
#define ALSA_SOUND_MAJOR 116

#define OSS_MINOR_MIXER   0
#define OSS_MINOR_DSP     3
#define OSS_MINOR_AUDIO   4
#define OSS_MINOR_SNDSTAT 6
#define OSS_MINOR_MIXER0  16
#define OSS_MINOR_DSP0    19
#define OSS_MINOR_AUDIO0  20

/*
 * xv6's device table reserves minor 0 for dynamic allocation.  Publish
 * Linux-compatible /dev/snd names, but keep internal minors non-zero.
 */
#define ALSA_MINOR_CONTROL0 1
#define ALSA_MINOR_PCM0P    16

#define OSS_VERSION 0x040090

#define OSS_GETVERSION          0x80044d76
#define SNDCTL_SYSINFO          0x84e05801
#define SNDCTL_AUDIOINFO        0xc49c5807
#define SNDCTL_ENGINEINFO       0xc49c580c

#define SNDCTL_DSP_RESET        0x5000
#define SNDCTL_DSP_SYNC         0x5001
#define SNDCTL_DSP_SPEED        0xc0045002
#define SNDCTL_DSP_STEREO       0xc0045003
#define SNDCTL_DSP_GETBLKSIZE   0xc0045004
#define SNDCTL_DSP_SETFMT       0xc0045005
#define SNDCTL_DSP_CHANNELS     0xc0045006
#define SNDCTL_DSP_POST         0x5008
#define SNDCTL_DSP_SUBDIVIDE    0xc0045009
#define SNDCTL_DSP_SETFRAGMENT  0xc004500a
#define SNDCTL_DSP_GETFMTS      0x8004500b
#define SNDCTL_DSP_GETOSPACE    0x8010500c
#define SNDCTL_DSP_GETISPACE    0x8010500d
#define SNDCTL_DSP_GETCAPS      0x8004500f
#define SNDCTL_DSP_GETTRIGGER   0x80045010
#define SNDCTL_DSP_SETTRIGGER   0x40045010
#define SNDCTL_DSP_GETIPTR      0x800c5011
#define SNDCTL_DSP_GETOPTR      0x800c5012
#define SNDCTL_DSP_SETDUPLEX    0x5016
#define SNDCTL_DSP_PROFILE      0x40045017
#define SNDCTL_DSP_GETODELAY    0x80045017
#define SNDCTL_DSP_GETPLAYVOL   0x80045018
#define SNDCTL_DSP_SETPLAYVOL   0xc0045018
#define SNDCTL_DSP_GETERROR     0x80685019
#define SNDCTL_DSP_COOKEDMODE   0x4004501e
#define SNDCTL_DSP_SILENCE      0x501f
#define SNDCTL_DSP_SKIP         0x5020
#define SNDCTL_DSP_HALT_INPUT   0x5021
#define SNDCTL_DSP_HALT_OUTPUT  0x5022
#define SNDCTL_DSP_LOW_WATER    0x40045022
#define SNDCTL_DSP_CURRENT_IPTR 0x80905023
#define SNDCTL_DSP_CURRENT_OPTR 0x80905024
#define SNDCTL_DSP_GET_CHNORDER 0x8008502a
#define SNDCTL_DSP_SET_CHNORDER 0xc008502a
#define SNDCTL_DSP_POLICY       0x4004502d
#define SNDCTL_DSP_GETCHANNELMASK 0xc0045040
#define SNDCTL_DSP_BIND_CHANNEL   0xc0045041

#define SOUND_MIXER_READ_VOLUME     0x80044d00
#define SOUND_MIXER_READ_PCM        0x80044d04
#define SOUND_MIXER_READ_RECSRC     0x80044dff
#define SOUND_MIXER_READ_DEVMASK    0x80044dfe
#define SOUND_MIXER_READ_RECMASK    0x80044dfd
#define SOUND_MIXER_READ_STEREODEVS 0x80044dfb
#define SOUND_MIXER_READ_CAPS       0x80044dfc
#define SOUND_MIXER_WRITE_VOLUME    0xc0044d00
#define SOUND_MIXER_WRITE_PCM       0xc0044d04
#define SOUND_MIXER_WRITE_RECSRC    0xc0044dff

#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010
#define AFMT_S32_LE  0x00001000
#define AFMT_S24_LE  0x00008000

#define OSS_FORMATS (AFMT_U8 | AFMT_S16_LE | AFMT_S24_LE | AFMT_S32_LE)
#define OSS_DEFAULT_FORMAT AFMT_S16_LE
#define OSS_DEFAULT_RATE 48000
#define OSS_DEFAULT_CHANNELS 2
#define OSS_BLOCK_SIZE 4096
#define OSS_FRAGMENTS 16
#define OSS_BUFFER_BYTES (OSS_BLOCK_SIZE * OSS_FRAGMENTS)
#define OSS_POLL_LOW_WATER (OSS_BLOCK_SIZE / 2)
#define OSS_NOTIFY_INTERVAL_MS 5
#define OSS_MAX_PCM_FILES 32

#ifndef INT_MAX
#define INT_MAX 0x7fffffff
#endif

#define PCM_CAP_DUPLEX   0x00000100
#define PCM_CAP_TRIGGER  0x00001000
#define PCM_CAP_MULTI    0x00004000
#define PCM_CAP_INPUT    0x00010000
#define PCM_CAP_OUTPUT   0x00020000
#define PCM_CAP_VIRTUAL  0x00040000
#define PCM_ENABLE_INPUT  0x00000001
#define PCM_ENABLE_OUTPUT 0x00000002

#define CHNORDER_NORMAL 0x0000000087654321ULL

#define SOUND_MASK_VOLUME (1 << 0)
#define SOUND_MASK_PCM    (1 << 4)
#define SOUND_MIXER_VALUE(left, right) (((right) << 8) | (left))

#define SNDRV_CTL_VERSION 0x00020009
#define SNDRV_PCM_VERSION 0x00020011

#define SNDRV_CTL_IOCTL_PVERSION         0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO        0x81785501
#define SNDRV_CTL_IOCTL_ELEM_LIST        0xc0505510
#define SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS 0xc0045516
#define SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE 0xc0045520
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE  0x80045530
#define SNDRV_CTL_IOCTL_PCM_INFO         0xc1205531
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE 0x40045532
#define SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE 0xc0045540
#define SNDRV_CTL_IOCTL_POWER       0xc00455d0
#define SNDRV_CTL_IOCTL_POWER_STATE 0x800455d1

#define SNDRV_PCM_IOCTL_PVERSION      0x80044100
#define SNDRV_PCM_IOCTL_INFO          0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP        0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP       0x40044103
#define SNDRV_PCM_IOCTL_USER_PVERSION 0x40044104
#define SNDRV_PCM_IOCTL_HW_REFINE     0xc2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS     0xc2604111
#define SNDRV_PCM_IOCTL_HW_FREE       0x4112
#define SNDRV_PCM_IOCTL_SW_PARAMS     0xc0884113
#define SNDRV_PCM_IOCTL_STATUS        0x80984120
#define SNDRV_PCM_IOCTL_DELAY         0x80084121
#define SNDRV_PCM_IOCTL_HWSYNC        0x4122
#define SNDRV_PCM_IOCTL_SYNC_PTR      0xc0884123
#define SNDRV_PCM_IOCTL_STATUS_EXT    0xc0984124
#define SNDRV_PCM_IOCTL_CHANNEL_INFO  0x80184132
#define SNDRV_PCM_IOCTL_PREPARE       0x4140
#define SNDRV_PCM_IOCTL_START         0x4142
#define SNDRV_PCM_IOCTL_DROP          0x4143
#define SNDRV_PCM_IOCTL_DRAIN         0x4144
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES 0x40184150

#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE  1

#define SNDRV_PCM_STATE_OPEN     0
#define SNDRV_PCM_STATE_SETUP    1
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING  3

#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3
#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_SUBFORMAT_STD 0

#define SNDRV_PCM_HW_PARAM_ACCESS       0
#define SNDRV_PCM_HW_PARAM_FORMAT       1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT    2
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19

#define SNDRV_PCM_INFO_INTERLEAVED 0x00000100
#define SNDRV_PCM_INFO_BLOCK_TRANSFER 0x00010000

typedef struct audio_buf_info {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
} audio_buf_info_t;

typedef struct count_info {
    uint bytes;
    int blocks;
    int ptr;
} count_info_t;

typedef struct audio_errinfo {
    int play_underruns;
    int rec_overruns;
    uint play_ptradjust;
    uint rec_ptradjust;
    int play_errorcount;
    int rec_errorcount;
    int play_lasterror;
    int rec_lasterror;
    int play_errorparm;
    int rec_errorparm;
    int filler[16];
} audio_errinfo_t;

typedef struct oss_sysinfo {
    char product[32];
    char version[32];
    int versionnum;
    char options[128];
    int numaudios;
    int openedaudio[8];
    int numsynths;
    int nummidis;
    int numtimers;
    int nummixers;
    int openedmidi[8];
    int numcards;
    int numaudioengines;
    char license[16];
    int filler[236];
} oss_sysinfo_t;

typedef struct oss_audioinfo {
    int dev;
    char name[64];
    int busy;
    int pid;
    int caps;
    int iformats, oformats;
    int magic;
    char cmd[64];
    int card_number;
    int port_number;
    int mixer_dev;
    int legacy_device;
    int enabled;
    int flags;
    int min_rate, max_rate;
    int min_channels, max_channels;
    int binding;
    int rate_source;
    char handle[32];
    uint nrates, rates[20];
    char song_name[64];
    char label[16];
    int latency;
    char devnode[32];
    int next_play_engine;
    int next_rec_engine;
    int filler[184];
} oss_audioinfo_t;

typedef struct oss_count {
    int samples;
    int fifo_samples;
    int filler[34];
} oss_count_t;

typedef struct alsa_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
} alsa_ctl_card_info_t;

typedef struct alsa_ctl_elem_id {
    uint numid;
    int iface;
    uint device;
    uint subdevice;
    unsigned char name[44];
    uint index;
} alsa_ctl_elem_id_t;

typedef struct alsa_ctl_elem_list {
    uint offset;
    uint space;
    uint used;
    uint count;
    alsa_ctl_elem_id_t *pids;
    unsigned char reserved[50];
} alsa_ctl_elem_list_t;

typedef union alsa_pcm_sync_id {
    unsigned char id[16];
    uint16 id16[8];
    uint id32[4];
} alsa_pcm_sync_id_t;

typedef struct alsa_pcm_info {
    uint device;
    uint subdevice;
    int stream;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned char subname[32];
    int dev_class;
    int dev_subclass;
    uint subdevices_count;
    uint subdevices_avail;
    alsa_pcm_sync_id_t sync;
    unsigned char reserved[64];
} alsa_pcm_info_t;

typedef struct alsa_mask {
    uint bits[8];
} alsa_mask_t;

typedef struct alsa_interval {
    uint min, max;
    uint openmin : 1, openmax : 1, integer : 1, empty : 1;
} alsa_interval_t;

typedef struct alsa_pcm_hw_params {
    uint flags;
    alsa_mask_t masks[3];
    alsa_mask_t mres[5];
    alsa_interval_t intervals[12];
    alsa_interval_t ires[9];
    uint rmask;
    uint cmask;
    uint info;
    uint msbits;
    uint rate_num;
    uint rate_den;
    uint64 fifo_size;
    unsigned char reserved[64];
} alsa_pcm_hw_params_t;

typedef struct alsa_pcm_sw_params {
    int tstamp_mode;
    uint period_step;
    uint sleep_min;
    uint64 avail_min;
    uint64 xfer_align;
    uint64 start_threshold;
    uint64 stop_threshold;
    uint64 silence_threshold;
    uint64 silence_size;
    uint64 boundary;
    uint proto;
    uint tstamp_type;
    unsigned char reserved[56];
} alsa_pcm_sw_params_t;

typedef struct alsa_timespec {
    int64 tv_sec;
    int64 tv_nsec;
} alsa_timespec_t;

typedef struct alsa_pcm_status {
    int state;
    int pad1;
    alsa_timespec_t trigger_tstamp;
    alsa_timespec_t tstamp;
    uint64 appl_ptr;
    uint64 hw_ptr;
    int64 delay;
    uint64 avail;
    uint64 avail_max;
    uint64 overrange;
    int suspended_state;
    uint audio_tstamp_data;
    alsa_timespec_t audio_tstamp;
    alsa_timespec_t driver_tstamp;
    uint audio_tstamp_accuracy;
    unsigned char reserved[20];
} alsa_pcm_status_t;

typedef struct alsa_pcm_mmap_status {
    int state;
    int pad1;
    uint64 hw_ptr;
    alsa_timespec_t tstamp;
    int suspended_state;
    alsa_timespec_t audio_tstamp;
} alsa_pcm_mmap_status_t;

typedef struct alsa_pcm_mmap_control {
    uint64 appl_ptr;
    uint64 avail_min;
} alsa_pcm_mmap_control_t;

typedef struct alsa_pcm_sync_ptr {
    uint flags;
    union {
        alsa_pcm_mmap_status_t status;
        unsigned char reserved[64];
    } s;
    union {
        alsa_pcm_mmap_control_t control;
        unsigned char reserved[64];
    } c;
} alsa_pcm_sync_ptr_t;

typedef struct alsa_xferi {
    int64 result;
    void *buf;
    uint64 frames;
} alsa_xferi_t;

_Static_assert(sizeof(audio_buf_info_t) == 16, "audio_buf_info size");
_Static_assert(sizeof(count_info_t) == 12, "count_info size");
_Static_assert(sizeof(audio_errinfo_t) == 104, "audio_errinfo size");
_Static_assert(sizeof(oss_sysinfo_t) == 1248, "oss_sysinfo size");
_Static_assert(sizeof(oss_audioinfo_t) == 1180, "oss_audioinfo size");
_Static_assert(sizeof(oss_count_t) == 144, "oss_count size");
_Static_assert(sizeof(alsa_ctl_card_info_t) == 376, "alsa card info size");
_Static_assert(sizeof(alsa_ctl_elem_list_t) == 80, "alsa elem list size");
_Static_assert(sizeof(alsa_pcm_info_t) == 288, "alsa pcm info size");
_Static_assert(sizeof(alsa_pcm_hw_params_t) == 608, "alsa hw params size");
_Static_assert(sizeof(alsa_pcm_sw_params_t) == 136, "alsa sw params size");
_Static_assert(sizeof(alsa_pcm_status_t) == 152, "alsa status size");
_Static_assert(sizeof(alsa_pcm_sync_ptr_t) == 136, "alsa sync ptr size");
_Static_assert(sizeof(alsa_xferi_t) == 24, "alsa xferi size");

static int oss_format = OSS_DEFAULT_FORMAT;
static int oss_rate = OSS_DEFAULT_RATE;
static int oss_channels = OSS_DEFAULT_CHANNELS;
static int oss_trigger = PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT;
static int oss_volume = SOUND_MIXER_VALUE(100, 100);
static uint64 oss_play_written_bytes;
static uint64 oss_play_consumed_bytes;
static uint64 oss_play_last_ms;
static uint oss_rec_bytes;
static int alsa_pcm_state = SNDRV_PCM_STATE_OPEN;
static uint64 alsa_appl_ptr;
static uint64 alsa_hw_ptr;
static struct vfs_file *oss_pcm_files[OSS_MAX_PCM_FILES];
static int oss_notify_armed;
static spinlock_t oss_lock;

static struct vfs_file_ops oss_pcm_file_ops;
static struct vfs_file_ops alsa_pcm_file_ops;
static struct vfs_file_ops oss_sndstat_file_ops;
static void oss_reset_playback_locked(void);
static void oss_schedule_notify(void);
static void oss_notify_writable_callback(void *arg);

static int oss_copyout(uint64 uaddr, const void *src, size_t len) {
    if (uaddr == 0)
        return -EINVAL;
    return either_copyout(1, uaddr, (void *)src, len) < 0 ? -EFAULT : 0;
}

static int oss_copyin(void *dst, uint64 uaddr, size_t len) {
    if (uaddr == 0)
        return -EINVAL;
    return either_copyin(dst, 1, uaddr, len) < 0 ? -EFAULT : 0;
}

static int oss_get_int(uint64 uaddr, int *value) {
    return oss_copyin(value, uaddr, sizeof(*value));
}

static int oss_put_int(uint64 uaddr, int value) {
    return oss_copyout(uaddr, &value, sizeof(value));
}

static void oss_put_string(char *dst, size_t len, const char *src) {
    if (len == 0)
        return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static void alsa_put_ustr(unsigned char *dst, size_t len, const char *src) {
    oss_put_string((char *)dst, len, src);
}

static int alsa_ioctl_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        char value[8];
        cached = cmdline_get_param("alsa_ioctl_trace", value,
                                   sizeof(value)) == 0 &&
                 value[0] != '0';
    }
    return cached;
}

static int oss_normalize_format(int fmt) {
    int available = virtio_snd_available() ?
        virtio_snd_supported_oss_formats() : OSS_FORMATS;

    if (fmt == 0)
        return oss_format;
    if ((fmt == AFMT_U8 || fmt == AFMT_S16_LE || fmt == AFMT_S24_LE ||
         fmt == AFMT_S32_LE) && (fmt & available))
        return fmt;
    if ((fmt & AFMT_S16_LE) && (available & AFMT_S16_LE))
        return AFMT_S16_LE;
    if ((fmt & AFMT_U8) && (available & AFMT_U8))
        return AFMT_U8;
    if ((fmt & AFMT_S24_LE) && (available & AFMT_S24_LE))
        return AFMT_S24_LE;
    if ((fmt & AFMT_S32_LE) && (available & AFMT_S32_LE))
        return AFMT_S32_LE;
    if (available & AFMT_S16_LE)
        return AFMT_S16_LE;
    if (available & AFMT_U8)
        return AFMT_U8;
    if (available & AFMT_S32_LE)
        return AFMT_S32_LE;
    return OSS_DEFAULT_FORMAT;
}

static uint64 oss_bytes_per_second_locked(void) {
    uint64 bytes_per_sample;

    switch (oss_format) {
    case AFMT_U8:
        bytes_per_sample = 1;
        break;
    case AFMT_S24_LE:
        bytes_per_sample = 3;
        break;
    case AFMT_S32_LE:
        bytes_per_sample = 4;
        break;
    case AFMT_S16_LE:
    default:
        bytes_per_sample = 2;
        break;
    }

    return (uint64)oss_rate * (uint64)oss_channels * bytes_per_sample;
}

static void oss_playback_update_locked(void) {
    uint64 now = sched_timer_now_ms();
    if (oss_play_last_ms == 0) {
        oss_play_last_ms = now;
        return;
    }

    uint64 elapsed_ms = now - oss_play_last_ms;
    if (elapsed_ms == 0)
        return;
    oss_play_last_ms = now;

    uint64 pending = oss_play_written_bytes - oss_play_consumed_bytes;
    if (pending == 0)
        return;

    uint64 bytes_per_sec = oss_bytes_per_second_locked();
    uint64 consumed = (bytes_per_sec * elapsed_ms) / 1000;
    if (consumed == 0)
        consumed = 1;
    if (consumed > pending)
        consumed = pending;
    oss_play_consumed_bytes += consumed;
}

static uint64 oss_pending_locked(void) {
    if (virtio_snd_available())
        return virtio_snd_pending_bytes();
    oss_playback_update_locked();
    return oss_play_written_bytes - oss_play_consumed_bytes;
}

static uint64 oss_free_locked(void) {
    uint64 pending = oss_pending_locked();
    if (pending >= OSS_BUFFER_BYTES)
        return 0;
    return OSS_BUFFER_BYTES - pending;
}

static int oss_playback_writable_locked(void) {
    uint64 free_bytes = oss_free_locked();
    return free_bytes >= OSS_POLL_LOW_WATER || free_bytes == OSS_BUFFER_BYTES;
}

static int oss_track_pcm_file(struct vfs_file *file) {
    int slot = -1;

    spin_lock(&oss_lock);
    oss_reset_playback_locked();
    for (int i = 0; i < OSS_MAX_PCM_FILES; i++) {
        if (oss_pcm_files[i] == NULL) {
            oss_pcm_files[i] = file;
            slot = i;
            break;
        }
    }
    spin_unlock(&oss_lock);
    if (virtio_snd_available())
        virtio_snd_reset();
    if (slot < 0)
        return -EMFILE;
    file->private_data = (void *)(uint64)(slot + 1);
    return 0;
}

static int oss_any_pcm_file_locked(void) {
    for (int i = 0; i < OSS_MAX_PCM_FILES; i++) {
        if (oss_pcm_files[i] != NULL)
            return 1;
    }
    return 0;
}

static void oss_collect_pcm_files(struct vfs_file **files, int *count) {
    int n = 0;

    spin_lock(&oss_lock);
    for (int i = 0; i < OSS_MAX_PCM_FILES; i++) {
        struct vfs_file *file = oss_pcm_files[i];
        if (file != NULL && n < OSS_MAX_PCM_FILES) {
            file = vfs_fdup(file);
            if (file != NULL)
                files[n++] = file;
        }
    }
    spin_unlock(&oss_lock);
    *count = n;
}

static void oss_notify_pcm_writers(void) {
    struct vfs_file *files[OSS_MAX_PCM_FILES];
    int count = 0;

    oss_collect_pcm_files(files, &count);
    for (int i = 0; i < count; i++) {
        vfs_file_knote_notify(files[i], EVFILT_WRITE, 0);
        vfs_fput(files[i]);
    }
}

static void oss_schedule_notify(void) {
    int arm = 0;

    spin_lock(&oss_lock);
    if (!oss_notify_armed && oss_any_pcm_file_locked()) {
        oss_notify_armed = 1;
        arm = 1;
    }
    spin_unlock(&oss_lock);

    if (arm && sched_timer_add(oss_notify_writable_callback, NULL,
                               OSS_NOTIFY_INTERVAL_MS) != 0) {
        spin_lock(&oss_lock);
        oss_notify_armed = 0;
        spin_unlock(&oss_lock);
    }
}

static void oss_notify_writable_callback(void *arg) {
    (void)arg;
    int writable;
    int should_rearm = 0;

    spin_lock(&oss_lock);
    oss_notify_armed = 0;
    writable = oss_playback_writable_locked();
    if (!writable && oss_any_pcm_file_locked())
        should_rearm = 1;
    spin_unlock(&oss_lock);

    if (writable)
        oss_notify_pcm_writers();
    else if (should_rearm)
        oss_schedule_notify();
}

static void oss_reset_playback_locked(void) {
    oss_play_written_bytes = 0;
    oss_play_consumed_bytes = 0;
    oss_play_last_ms = sched_timer_now_ms();
    alsa_appl_ptr = 0;
    alsa_hw_ptr = 0;
}

static int oss_is_pcm_device(cdev_t *cdev) {
    if (cdev == NULL)
        return 0;
    int minor = cdev->dev.minor;
    return minor == OSS_MINOR_DSP || minor == OSS_MINOR_DSP0 ||
           minor == OSS_MINOR_AUDIO || minor == OSS_MINOR_AUDIO0;
}

static void oss_drain_playback(void) {
    if (virtio_snd_available()) {
        virtio_snd_drain();
        return;
    }

    for (;;) {
        spin_lock(&oss_lock);
        uint64 pending = oss_pending_locked();
        if (pending == 0) {
            spin_unlock(&oss_lock);
            return;
        }

        uint64 bytes_per_ms = oss_bytes_per_second_locked() / 1000;
        if (bytes_per_ms == 0)
            bytes_per_ms = 1;
        uint64 sleep_for = (pending + bytes_per_ms - 1) / bytes_per_ms;
        if (sleep_for > 20)
            sleep_for = 20;
        spin_unlock(&oss_lock);
        sleep_ms(sleep_for ? sleep_for : 1);
    }
}

static void oss_fill_bufinfo(audio_buf_info_t *info) {
    info->fragments = OSS_FRAGMENTS;
    info->fragstotal = OSS_FRAGMENTS;
    info->fragsize = OSS_BLOCK_SIZE;
    info->bytes = OSS_FRAGMENTS * OSS_BLOCK_SIZE;
}

static void oss_fill_audioinfo(oss_audioinfo_t *ai) {
    int requested = ai->dev;
    memset(ai, 0, sizeof(*ai));
    if (requested < 0)
        requested = 0;

    ai->dev = 0;
    oss_put_string(ai->name, sizeof(ai->name),
                   virtio_snd_available() ?
                   "xv6 OSS PCM (virtio-sound)" :
                   "xv6 OSS virtual PCM");
    ai->caps = PCM_CAP_INPUT | PCM_CAP_OUTPUT | PCM_CAP_DUPLEX |
               PCM_CAP_TRIGGER | PCM_CAP_MULTI;
    if (!virtio_snd_available())
        ai->caps |= PCM_CAP_VIRTUAL;
    ai->iformats = OSS_FORMATS;
    ai->oformats = virtio_snd_available() ?
                   virtio_snd_supported_oss_formats() : OSS_FORMATS;
    ai->mixer_dev = 0;
    ai->legacy_device = OSS_MINOR_DSP;
    ai->enabled = 1;
    ai->min_rate = 8000;
    ai->max_rate = 192000;
    ai->min_channels = 1;
    ai->max_channels = 2;
    oss_put_string(ai->handle, sizeof(ai->handle), "xv6/pcm0");
    ai->nrates = 7;
    ai->rates[0] = 8000;
    ai->rates[1] = 16000;
    ai->rates[2] = 22050;
    ai->rates[3] = 32000;
    ai->rates[4] = 44100;
    ai->rates[5] = 48000;
    ai->rates[6] = 96000;
    oss_put_string(ai->label, sizeof(ai->label), "xv6pcm");
    ai->latency = 0;
    oss_put_string(ai->devnode, sizeof(ai->devnode), "/dev/dsp");
    ai->next_play_engine = -1;
    ai->next_rec_engine = -1;
    (void)requested;
}

static int oss_open(cdev_t *cdev) {
    if (oss_is_pcm_device(cdev)) {
        spin_lock(&oss_lock);
        oss_reset_playback_locked();
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
    }
    return 0;
}

static int oss_release(cdev_t *cdev) {
    (void)cdev;
    return 0;
}

static int oss_pcm_open_file(cdev_t *cdev, struct vfs_file *file) {
    if (!oss_is_pcm_device(cdev))
        return -EINVAL;
    int ret = oss_track_pcm_file(file);
    if (ret < 0)
        return ret;
    file->ops = &oss_pcm_file_ops;
    return 0;
}

static int oss_pcm_file_release(struct vfs_inode *inode,
                                struct vfs_file *file) {
    (void)inode;
    int slot = (int)(uint64)file->private_data - 1;

    if (slot >= 0 && slot < OSS_MAX_PCM_FILES) {
        spin_lock(&oss_lock);
        if (oss_pcm_files[slot] == file)
            oss_pcm_files[slot] = NULL;
        spin_unlock(&oss_lock);
    }
    return 0;
}

static int oss_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    if (buf == NULL)
        return -EINVAL;

    static const char zeros[128] = {0};
    if (!user) {
        memset(buf, 0, count);
        oss_rec_bytes += count;
        return count;
    }

    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        if (either_copyout(1, (uint64)buf + done, (void *)zeros, chunk) < 0)
            return done ? (int)done : -EFAULT;
        done += chunk;
    }
    oss_rec_bytes += count;
    return count;
}

static int oss_pcm_write_data(struct vfs_file *file, bool user,
                              const void *buf, size_t count) {
    if (buf == NULL)
        return -EINVAL;

    bool nonblock = file != NULL && (file->f_flags & O_NONBLOCK);
    if (virtio_snd_available()) {
        int ret = virtio_snd_write(user ? 1 : 0, buf, count, oss_format,
                                   oss_rate, oss_channels,
                                   nonblock ? 1 : 0);
        if (ret != -ENODEV)
            return ret;
    }

    size_t done = 0;
    while (done < count) {
        spin_lock(&oss_lock);
        uint64 free_bytes = oss_free_locked();
        if (free_bytes == 0) {
            spin_unlock(&oss_lock);
            if (nonblock)
                return done ? (int)done : -EAGAIN;
            sleep_ms(1);
            continue;
        }

        size_t chunk = count - done;
        if ((uint64)chunk > free_bytes)
            chunk = (size_t)free_bytes;
        oss_play_written_bytes += chunk;
        done += chunk;
        spin_unlock(&oss_lock);
        if (nonblock)
            break;
    }
    oss_schedule_notify();
    return (int)done;
}

static int oss_write_file(cdev_t *cdev, struct vfs_file *file, bool user,
                          const void *buf, size_t count) {
    if (!oss_is_pcm_device(cdev))
        return (int)count;
    return oss_pcm_write_data(file, user, buf, count);
}

static int oss_write(cdev_t *cdev, bool user, const void *buf, size_t count) {
    if (oss_is_pcm_device(cdev))
        return oss_pcm_write_data(NULL, user, buf, count);
    return oss_write_file(cdev, NULL, user, buf, count);
}

static ssize_t oss_pcm_file_read(struct vfs_file *file, char *buf,
                                 size_t count, bool user) {
    (void)file;
    return oss_read(NULL, user, buf, count);
}

static ssize_t oss_pcm_file_write(struct vfs_file *file, const char *buf,
                                  size_t count, bool user) {
    return oss_pcm_write_data(file, user, buf, count);
}

static int sndstat_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    static const char timer_text[] =
        "OSS 4.0 compatible virtual audio\n"
        "Audio devices:\n"
        "0: xv6 OSS virtual PCM /dev/dsp\n"
        "Mixers:\n"
        "0: xv6 virtual mixer /dev/mixer\n";
    static const char virtio_text[] =
        "OSS 4.0 compatible audio\n"
        "Audio devices:\n"
        "0: xv6 OSS PCM /dev/dsp (virtio-sound)\n"
        "Mixers:\n"
        "0: xv6 virtual mixer /dev/mixer\n";
    const char *text = virtio_snd_available() ? virtio_text : timer_text;
    size_t len = sizeof(text) - 1;
    len = strlen(text);
    if (count < len)
        len = count;
    if (!user) {
        memcpy(buf, text, len);
        return len;
    }
    return either_copyout(1, (uint64)buf, (void *)text, len) < 0 ? -EFAULT : (int)len;
}

static ssize_t oss_sndstat_file_read(struct vfs_file *file, char *buf,
                                     size_t count, bool user) {
    static const char timer_text[] =
        "OSS 4.0 compatible virtual audio\n"
        "Audio devices:\n"
        "0: xv6 OSS virtual PCM /dev/dsp\n"
        "Mixers:\n"
        "0: xv6 virtual mixer /dev/mixer\n";
    static const char virtio_text[] =
        "OSS 4.0 compatible audio\n"
        "Audio devices:\n"
        "0: xv6 OSS PCM /dev/dsp (virtio-sound)\n"
        "Mixers:\n"
        "0: xv6 virtual mixer /dev/mixer\n";
    const char *text = virtio_snd_available() ? virtio_text : timer_text;
    size_t len = strlen(text);

    if (file->f_pos >= (loff_t)len)
        return 0;
    size_t off = (size_t)file->f_pos;
    size_t n = len - off;
    if (n > count)
        n = count;

    int ret;
    if (!user) {
        memcpy(buf, text + off, n);
        ret = (int)n;
    } else {
        ret = either_copyout(1, (uint64)buf, (void *)(text + off), n) < 0 ?
              -EFAULT : (int)n;
    }
    if (ret > 0)
        file->f_pos += ret;
    return ret;
}

static int oss_sndstat_open_file(cdev_t *cdev, struct vfs_file *file) {
    if (cdev == NULL || file == NULL)
        return -EINVAL;
    file->ops = &oss_sndstat_file_ops;
    file->private_data = NULL;
    file->f_pos = 0;
    return 0;
}

static int oss_ioctl(cdev_t *cdev, uint64 cmd, void *arg) {
    uint64 uarg = (uint64)arg;
    int value;

    (void)cdev;

    switch ((uint)cmd) {
    case OSS_GETVERSION:
        return oss_put_int(uarg, OSS_VERSION);

    case SNDCTL_SYSINFO: {
        oss_sysinfo_t si;
        memset(&si, -1, sizeof(si));
        memset(&si, 0, sizeof(si));
        oss_put_string(si.product, sizeof(si.product), "OSS/xv6");
        oss_put_string(si.version, sizeof(si.version), "4.0-xv6");
        si.versionnum = OSS_VERSION;
        si.numaudios = 1;
        si.nummixers = 1;
        si.numcards = 1;
        si.numaudioengines = 1;
        oss_put_string(si.license, sizeof(si.license), "BSD");
        return oss_copyout(uarg, &si, sizeof(si));
    }

    case SNDCTL_AUDIOINFO:
    case SNDCTL_ENGINEINFO: {
        oss_audioinfo_t ai;
        if (oss_copyin(&ai, uarg, sizeof(ai)) < 0)
            memset(&ai, 0, sizeof(ai));
        oss_fill_audioinfo(&ai);
        return oss_copyout(uarg, &ai, sizeof(ai));
    }

    case SNDCTL_DSP_RESET:
        spin_lock(&oss_lock);
        oss_reset_playback_locked();
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return 0;

    case SNDCTL_DSP_SYNC:
        oss_drain_playback();
        return 0;

    case SNDCTL_DSP_POST:
    case SNDCTL_DSP_SETDUPLEX:
    case SNDCTL_DSP_SILENCE:
    case SNDCTL_DSP_SKIP:
    case SNDCTL_DSP_HALT_INPUT:
        return 0;

    case SNDCTL_DSP_HALT_OUTPUT:
        spin_lock(&oss_lock);
        oss_reset_playback_locked();
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return 0;

    case SNDCTL_DSP_GETFMTS:
        return oss_put_int(uarg, virtio_snd_available() ?
                           virtio_snd_supported_oss_formats() :
                           OSS_FORMATS);

    case SNDCTL_DSP_SETFMT:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_format = oss_normalize_format(value);
        oss_reset_playback_locked();
        value = oss_format;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_SPEED:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        if (value < 8000)
            value = 8000;
        if (value > 192000)
            value = 192000;
        spin_lock(&oss_lock);
        oss_rate = value;
        oss_reset_playback_locked();
        value = oss_rate;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_CHANNELS:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_channels = value <= 1 ? 1 : 2;
        oss_reset_playback_locked();
        value = oss_channels;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_STEREO:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_channels = value ? 2 : 1;
        oss_reset_playback_locked();
        value = oss_channels == 2;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_GETBLKSIZE:
        return oss_put_int(uarg, OSS_BLOCK_SIZE);

    case SNDCTL_DSP_GETOSPACE: {
        audio_buf_info_t info;
        oss_fill_bufinfo(&info);
        spin_lock(&oss_lock);
        info.bytes = (int)oss_free_locked();
        info.fragments = info.bytes / info.fragsize;
        spin_unlock(&oss_lock);
        return oss_copyout(uarg, &info, sizeof(info));
    }

    case SNDCTL_DSP_GETISPACE: {
        audio_buf_info_t info;
        oss_fill_bufinfo(&info);
        return oss_copyout(uarg, &info, sizeof(info));
    }

    case SNDCTL_DSP_GETCAPS:
        value = PCM_CAP_INPUT | PCM_CAP_OUTPUT | PCM_CAP_DUPLEX |
                PCM_CAP_TRIGGER | PCM_CAP_MULTI;
        if (!virtio_snd_available())
            value |= PCM_CAP_VIRTUAL;
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_GETTRIGGER:
        return oss_put_int(uarg, oss_trigger);

    case SNDCTL_DSP_SETTRIGGER:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        oss_trigger = value & (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT);
        return 0;

    case SNDCTL_DSP_GETIPTR: {
        count_info_t ci = { .bytes = oss_rec_bytes,
                            .blocks = (int)(oss_rec_bytes / OSS_BLOCK_SIZE),
                            .ptr = (int)(oss_rec_bytes % OSS_BLOCK_SIZE) };
        return oss_copyout(uarg, &ci, sizeof(ci));
    }

    case SNDCTL_DSP_GETOPTR: {
        spin_lock(&oss_lock);
        oss_pending_locked();
        uint64 played = virtio_snd_available() ?
                        virtio_snd_played_bytes() :
                        oss_play_consumed_bytes;
        count_info_t ci = { .bytes = (uint)played,
                            .blocks = (int)(played / OSS_BLOCK_SIZE),
                            .ptr = (int)(played % OSS_BLOCK_SIZE) };
        spin_unlock(&oss_lock);
        return oss_copyout(uarg, &ci, sizeof(ci));
    }

    case SNDCTL_DSP_GETODELAY:
        spin_lock(&oss_lock);
        value = (int)oss_pending_locked();
        spin_unlock(&oss_lock);
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_GETERROR: {
        audio_errinfo_t ei;
        memset(&ei, 0, sizeof(ei));
        return oss_copyout(uarg, &ei, sizeof(ei));
    }

    case SNDCTL_DSP_GETPLAYVOL:
        return oss_put_int(uarg, oss_volume);

    case SNDCTL_DSP_SETPLAYVOL:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        oss_volume = value;
        return oss_put_int(uarg, oss_volume);

    case SNDCTL_DSP_SUBDIVIDE:
    case SNDCTL_DSP_SETFRAGMENT:
    case SNDCTL_DSP_PROFILE:
    case SNDCTL_DSP_COOKEDMODE:
    case SNDCTL_DSP_LOW_WATER:
    case SNDCTL_DSP_POLICY:
        return 0;

    case SNDCTL_DSP_GET_CHNORDER: {
        uint64 order = CHNORDER_NORMAL;
        return oss_copyout(uarg, &order, sizeof(order));
    }

    case SNDCTL_DSP_SET_CHNORDER:
        return 0;

    case SNDCTL_DSP_GETCHANNELMASK:
        value = 0x1;
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_BIND_CHANNEL:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        if (value == 0)
            value = 0x1;
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_CURRENT_IPTR:
    case SNDCTL_DSP_CURRENT_OPTR: {
        spin_lock(&oss_lock);
        oss_pending_locked();
        uint64 played = virtio_snd_available() ?
                        virtio_snd_played_bytes() :
                        oss_play_consumed_bytes;
        uint64 bytes_per_sample = oss_bytes_per_second_locked() /
                                  ((uint64)oss_rate * (uint64)oss_channels);
        if (bytes_per_sample == 0)
            bytes_per_sample = 1;
        oss_count_t oc;
        memset(&oc, 0, sizeof(oc));
        oc.samples = (int)(played / bytes_per_sample);
        oc.fifo_samples = (int)(oss_pending_locked() / bytes_per_sample);
        spin_unlock(&oss_lock);
        return oss_copyout(uarg, &oc, sizeof(oc));
    }

    case SOUND_MIXER_READ_VOLUME:
    case SOUND_MIXER_READ_PCM:
        return oss_put_int(uarg, oss_volume);

    case SOUND_MIXER_WRITE_VOLUME:
    case SOUND_MIXER_WRITE_PCM:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        oss_volume = value;
        return oss_put_int(uarg, oss_volume);

    case SOUND_MIXER_READ_RECSRC:
    case SOUND_MIXER_WRITE_RECSRC:
        return oss_put_int(uarg, SOUND_MASK_PCM);

    case SOUND_MIXER_READ_DEVMASK:
    case SOUND_MIXER_READ_RECMASK:
    case SOUND_MIXER_READ_STEREODEVS:
        return oss_put_int(uarg, SOUND_MASK_VOLUME | SOUND_MASK_PCM);

    case SOUND_MIXER_READ_CAPS:
        return oss_put_int(uarg, 0);
    }

    if (alsa_ioctl_trace_enabled())
        printf("alsa-ioctl: oss unknown cmd=0x%lx\n", cmd);
    return -ENOTTY;
}

static void alsa_mask_only(alsa_mask_t *mask, uint bit)
{
    memset(mask, 0, sizeof(*mask));
    if (bit < 256)
        mask->bits[bit / 32] = 1U << (bit % 32);
}

static int alsa_mask_empty(const alsa_mask_t *mask)
{
    for (int i = 0; i < 8; i++) {
        if (mask->bits[i] != 0)
            return 0;
    }
    return 1;
}

static int alsa_mask_has(const alsa_mask_t *mask, uint bit)
{
    if (bit >= 256)
        return 0;
    return (mask->bits[bit / 32] & (1U << (bit % 32))) != 0;
}

static int alsa_mask_constrain(alsa_mask_t *mask, uint bit)
{
    if (!alsa_mask_empty(mask) && !alsa_mask_has(mask, bit))
        return -EINVAL;
    alsa_mask_only(mask, bit);
    return 0;
}

static void alsa_interval_set(alsa_interval_t *ival, uint min, uint max)
{
    memset(ival, 0, sizeof(*ival));
    ival->min = min;
    ival->max = max;
    ival->integer = 1;
}

static int alsa_interval_constrain(alsa_interval_t *ival, uint min, uint max)
{
    if (ival->empty)
        return -EINVAL;

    uint req_min = ival->min;
    uint req_max = ival->max;

    if (req_min == 0 && req_max == 0) {
        alsa_interval_set(ival, min, max);
        return 0;
    }

    if (ival->openmin && req_min < ~0U)
        req_min++;
    if (ival->openmax && req_max > 0)
        req_max--;
    if (req_max == 0 || req_max > max)
        req_max = max;
    if (req_min < min)
        req_min = min;
    if (req_max < req_min)
        return -EINVAL;

    alsa_interval_set(ival, req_min, req_max);
    return 0;
}

static alsa_interval_t *alsa_hw_interval(alsa_pcm_hw_params_t *params, uint hw)
{
    return &params->intervals[hw - SNDRV_PCM_HW_PARAM_SAMPLE_BITS];
}

static void alsa_fill_card_info(alsa_ctl_card_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->card = 0;
    alsa_put_ustr(info->id, sizeof(info->id), "XV6SND");
    alsa_put_ustr(info->driver, sizeof(info->driver), "xv6");
    alsa_put_ustr(info->name, sizeof(info->name), "xv6 Audio");
    alsa_put_ustr(info->longname, sizeof(info->longname),
                  virtio_snd_available() ?
                  "xv6 virtio-sound OSS/ALSA compatibility PCM" :
                  "xv6 virtual OSS/ALSA compatibility PCM");
    alsa_put_ustr(info->mixername, sizeof(info->mixername), "xv6 Mixer");
    alsa_put_ustr(info->components, sizeof(info->components), "VIRTIO");
}

static void alsa_fill_pcm_info(alsa_pcm_info_t *info, int from_control)
{
    uint device = from_control ? info->device : 0;
    uint subdevice = from_control ? info->subdevice : 0;
    int stream = from_control ? info->stream : SNDRV_PCM_STREAM_PLAYBACK;

    memset(info, 0, sizeof(*info));
    info->device = device;
    info->subdevice = subdevice;
    info->stream = stream;
    info->card = 0;
    alsa_put_ustr(info->id, sizeof(info->id), "xv6pcm0");
    alsa_put_ustr(info->name, sizeof(info->name),
                  virtio_snd_available() ?
                  "xv6 virtio-sound PCM" : "xv6 virtual PCM");
    alsa_put_ustr(info->subname, sizeof(info->subname), "subdevice #0");
    info->subdevices_count = 1;
    info->subdevices_avail = stream == SNDRV_PCM_STREAM_PLAYBACK ? 1 : 0;
    memcpy(info->sync.id, "xv6-alsa-pcm0", 13);
}

static int alsa_refine_hw_params(alsa_pcm_hw_params_t *params)
{
    if (alsa_mask_constrain(&params->masks[SNDRV_PCM_HW_PARAM_ACCESS],
                            SNDRV_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        alsa_mask_constrain(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT],
                            SNDRV_PCM_FORMAT_S16_LE) < 0 ||
        alsa_mask_constrain(&params->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT],
                            SNDRV_PCM_SUBFORMAT_STD) < 0)
        return -EINVAL;

    if (alsa_interval_constrain(alsa_hw_interval(params,
                                                 SNDRV_PCM_HW_PARAM_SAMPLE_BITS),
                                16, 16) < 0 ||
        alsa_interval_constrain(alsa_hw_interval(params,
                                                 SNDRV_PCM_HW_PARAM_CHANNELS),
                                1, 2) < 0 ||
        alsa_interval_constrain(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_RATE),
                                8000, 96000) < 0 ||
        alsa_interval_constrain(alsa_hw_interval(params,
                                                 SNDRV_PCM_HW_PARAM_PERIOD_TIME),
                                1000, 1000000) < 0 ||
        alsa_interval_constrain(alsa_hw_interval(params,
                                                 SNDRV_PCM_HW_PARAM_TICK_TIME),
                                0, 0) < 0)
        return -EINVAL;

    uint channels = alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->min;
    uint frame_bits_min = channels <= 1 ? 16 : 32;
    uint frame_bits_max = channels >= 2 ? 32 : 16;
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_FRAME_BITS),
                      frame_bits_min, frame_bits_max);

    uint rate = alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min;
    uint frame_bytes = channels * 2;
    uint max_buffer_frames = frame_bytes ? OSS_BUFFER_BYTES / frame_bytes : 128;
    if (max_buffer_frames < 128)
        max_buffer_frames = 128;

    uint requested_time = alsa_hw_interval(params,
                                           SNDRV_PCM_HW_PARAM_BUFFER_TIME)->min;
    uint requested_frames = alsa_hw_interval(params,
                                             SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    uint buffer_frames = requested_frames;
    if (buffer_frames == 0 && requested_time != 0)
        buffer_frames = (uint)(((uint64)rate * requested_time) / 1000000ULL);
    if (buffer_frames == 0)
        buffer_frames = rate / 10;
    if (buffer_frames < 128)
        buffer_frames = 128;
    if (buffer_frames > max_buffer_frames)
        buffer_frames = max_buffer_frames;

    uint period_frames = buffer_frames / 4;
    if (period_frames < 64)
        period_frames = 64;
    if (period_frames > buffer_frames)
        period_frames = buffer_frames;
    uint periods = period_frames ? buffer_frames / period_frames : 1;
    if (periods == 0)
        periods = 1;

    uint buffer_time = rate ? (uint)(((uint64)buffer_frames * 1000000ULL) /
                                    rate) : 0;
    uint period_time = rate ? (uint)(((uint64)period_frames * 1000000ULL) /
                                    rate) : 0;
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE),
                      buffer_frames, buffer_frames);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES),
                      buffer_frames * frame_bytes, buffer_frames * frame_bytes);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_TIME),
                      buffer_time, buffer_time);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
                      period_frames, period_frames);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES),
                      period_frames * frame_bytes, period_frames * frame_bytes);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_TIME),
                      period_time, period_time);
    alsa_interval_set(alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_PERIODS),
                      periods, periods);

    params->info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER;
    params->msbits = 16;
    params->rate_num = alsa_hw_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min;
    params->rate_den = 1;
    params->fifo_size = OSS_BUFFER_BYTES / 4;
    return 0;
}

static uint64 alsa_frames_from_bytes(uint64 bytes)
{
    uint64 frame_bytes = (uint64)oss_channels * 2;
    return frame_bytes ? bytes / frame_bytes : 0;
}

static void alsa_refresh_hw_ptr_locked(void)
{
    oss_pending_locked();
    uint64 played = virtio_snd_available() ?
                    virtio_snd_played_bytes() : oss_play_consumed_bytes;
    alsa_hw_ptr = alsa_frames_from_bytes(played);
}

static void alsa_fill_status(alsa_pcm_status_t *status)
{
    memset(status, 0, sizeof(*status));
    spin_lock(&oss_lock);
    alsa_refresh_hw_ptr_locked();
    status->state = alsa_pcm_state;
    status->appl_ptr = alsa_appl_ptr;
    status->hw_ptr = alsa_hw_ptr;
    status->delay = (int64)(alsa_appl_ptr - alsa_hw_ptr);
    status->avail = OSS_BUFFER_BYTES / ((uint64)oss_channels * 2);
    status->avail_max = status->avail;
    spin_unlock(&oss_lock);
}

static int alsa_ctl_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev;
    uint64 uarg = (uint64)arg;
    int value;

    switch ((uint)cmd) {
    case SNDRV_CTL_IOCTL_PVERSION:
        return oss_put_int(uarg, SNDRV_CTL_VERSION);
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        alsa_ctl_card_info_t info;
        alsa_fill_card_info(&info);
        return oss_copyout(uarg, &info, sizeof(info));
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE:
        if (oss_get_int(uarg, &value) < 0)
            value = -1;
        value = value < 0 ? 0 : -1;
        return oss_put_int(uarg, value);
    case SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE:
        return oss_put_int(uarg, -1);
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE:
        return 0;
    case SNDRV_CTL_IOCTL_POWER:
    case SNDRV_CTL_IOCTL_POWER_STATE:
        return oss_put_int(uarg, 0);
    case SNDRV_CTL_IOCTL_PCM_INFO: {
        alsa_pcm_info_t info;
        if (oss_copyin(&info, uarg, sizeof(info)) < 0)
            return -EFAULT;
        if (info.device != 0 || info.subdevice != 0 ||
            info.stream != SNDRV_PCM_STREAM_PLAYBACK)
            return -ENODEV;
        alsa_fill_pcm_info(&info, 1);
        return oss_copyout(uarg, &info, sizeof(info));
    }
    case SNDRV_CTL_IOCTL_ELEM_LIST: {
        alsa_ctl_elem_list_t list;
        if (oss_copyin(&list, uarg, sizeof(list)) < 0)
            memset(&list, 0, sizeof(list));
        list.count = 0;
        list.used = 0;
        return oss_copyout(uarg, &list, sizeof(list));
    }
    case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS:
        return oss_put_int(uarg, 0);
    }

    if (alsa_ioctl_trace_enabled())
        printf("alsa-ioctl: ctl unknown cmd=0x%lx\n", cmd);
    return -ENOTTY;
}

static int alsa_pcm_open_file(cdev_t *cdev, struct vfs_file *file)
{
    if (cdev == NULL || cdev->dev.minor != ALSA_MINOR_PCM0P)
        return -EINVAL;
    int ret = oss_track_pcm_file(file);
    if (ret < 0)
        return ret;
    spin_lock(&oss_lock);
    alsa_pcm_state = SNDRV_PCM_STATE_OPEN;
    spin_unlock(&oss_lock);
    file->ops = &alsa_pcm_file_ops;
    return 0;
}

static int alsa_pcm_ioctl_common(struct vfs_file *file, uint64 cmd, void *arg)
{
    uint64 uarg = (uint64)arg;

    switch ((uint)cmd) {
    case SNDRV_PCM_IOCTL_PVERSION:
        return oss_put_int(uarg, SNDRV_PCM_VERSION);
    case SNDRV_PCM_IOCTL_TSTAMP:
    case SNDRV_PCM_IOCTL_TTSTAMP:
    case SNDRV_PCM_IOCTL_USER_PVERSION:
        return 0;
    case SNDRV_PCM_IOCTL_INFO: {
        alsa_pcm_info_t info;
        alsa_fill_pcm_info(&info, 0);
        return oss_copyout(uarg, &info, sizeof(info));
    }
    case SNDRV_PCM_IOCTL_HW_REFINE:
    case SNDRV_PCM_IOCTL_HW_PARAMS: {
        alsa_pcm_hw_params_t params;
        if (oss_copyin(&params, uarg, sizeof(params)) < 0)
            memset(&params, 0, sizeof(params));
        if (alsa_refine_hw_params(&params) < 0)
            return -EINVAL;
        if ((uint)cmd == SNDRV_PCM_IOCTL_HW_PARAMS) {
            uint rate = alsa_hw_interval(&params, SNDRV_PCM_HW_PARAM_RATE)->min;
            uint channels = alsa_hw_interval(&params,
                                             SNDRV_PCM_HW_PARAM_CHANNELS)->min;
            if (rate < 8000 || rate > 96000)
                rate = OSS_DEFAULT_RATE;
            if (channels < 1 || channels > 2)
                channels = OSS_DEFAULT_CHANNELS;
            spin_lock(&oss_lock);
            oss_format = AFMT_S16_LE;
            oss_rate = (int)rate;
            oss_channels = (int)channels;
            oss_reset_playback_locked();
            alsa_pcm_state = SNDRV_PCM_STATE_SETUP;
            spin_unlock(&oss_lock);
            if (virtio_snd_available())
                virtio_snd_reset();
        }
        return oss_copyout(uarg, &params, sizeof(params));
    }
    case SNDRV_PCM_IOCTL_SW_PARAMS: {
        alsa_pcm_sw_params_t params;
        if (oss_copyin(&params, uarg, sizeof(params)) < 0)
            memset(&params, 0, sizeof(params));
        if (params.avail_min == 0)
            params.avail_min = OSS_BLOCK_SIZE / 4;
        if (params.boundary == 0)
            params.boundary = 0x40000000ULL;
        return oss_copyout(uarg, &params, sizeof(params));
    }
    case SNDRV_PCM_IOCTL_HW_FREE:
        spin_lock(&oss_lock);
        alsa_pcm_state = SNDRV_PCM_STATE_OPEN;
        spin_unlock(&oss_lock);
        return 0;
    case SNDRV_PCM_IOCTL_HWSYNC:
        return 0;
    case SNDRV_PCM_IOCTL_PREPARE:
        spin_lock(&oss_lock);
        oss_reset_playback_locked();
        alsa_pcm_state = SNDRV_PCM_STATE_PREPARED;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return 0;
    case SNDRV_PCM_IOCTL_START:
        spin_lock(&oss_lock);
        alsa_pcm_state = SNDRV_PCM_STATE_RUNNING;
        spin_unlock(&oss_lock);
        return 0;
    case SNDRV_PCM_IOCTL_DROP:
        spin_lock(&oss_lock);
        oss_reset_playback_locked();
        alsa_pcm_state = SNDRV_PCM_STATE_SETUP;
        spin_unlock(&oss_lock);
        if (virtio_snd_available())
            virtio_snd_reset();
        return 0;
    case SNDRV_PCM_IOCTL_DRAIN:
        oss_drain_playback();
        return 0;
    case SNDRV_PCM_IOCTL_DELAY: {
        spin_lock(&oss_lock);
        alsa_refresh_hw_ptr_locked();
        int64 delay = (int64)(alsa_appl_ptr - alsa_hw_ptr);
        spin_unlock(&oss_lock);
        return oss_copyout(uarg, &delay, sizeof(delay));
    }
    case SNDRV_PCM_IOCTL_STATUS: {
        alsa_pcm_status_t status;
        alsa_fill_status(&status);
        return oss_copyout(uarg, &status, sizeof(status));
    }
    case SNDRV_PCM_IOCTL_STATUS_EXT: {
        alsa_pcm_status_t status;
        if (oss_copyin(&status, uarg, sizeof(status)) < 0)
            memset(&status, 0, sizeof(status));
        alsa_fill_status(&status);
        return oss_copyout(uarg, &status, sizeof(status));
    }
    case SNDRV_PCM_IOCTL_CHANNEL_INFO:
        return -ENOTTY;
    case SNDRV_PCM_IOCTL_SYNC_PTR: {
        alsa_pcm_sync_ptr_t sync;
        if (oss_copyin(&sync, uarg, sizeof(sync)) < 0)
            memset(&sync, 0, sizeof(sync));
        spin_lock(&oss_lock);
        alsa_refresh_hw_ptr_locked();
        sync.s.status.state = alsa_pcm_state;
        sync.s.status.hw_ptr = alsa_hw_ptr;
        sync.c.control.appl_ptr = alsa_appl_ptr;
        if (sync.c.control.avail_min == 0)
            sync.c.control.avail_min = OSS_BLOCK_SIZE / 4;
        spin_unlock(&oss_lock);
        return oss_copyout(uarg, &sync, sizeof(sync));
    }
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES: {
        alsa_xferi_t xfer;
        if (oss_copyin(&xfer, uarg, sizeof(xfer)) < 0)
            return -EFAULT;
        uint64 frame_bytes = (uint64)oss_channels * 2;
        if (frame_bytes == 0)
            frame_bytes = 4;
        uint64 bytes = xfer.frames * frame_bytes;
        if (bytes > INT_MAX)
            bytes = INT_MAX;
        int ret = oss_pcm_write_data(file, 1, xfer.buf, (size_t)bytes);
        if (ret < 0) {
            xfer.result = ret;
        } else {
            xfer.result = ret / (int64)frame_bytes;
            spin_lock(&oss_lock);
            alsa_appl_ptr += (uint64)xfer.result;
            alsa_pcm_state = SNDRV_PCM_STATE_RUNNING;
            spin_unlock(&oss_lock);
        }
        int copy_ret = oss_copyout(uarg, &xfer, sizeof(xfer));
        return copy_ret < 0 ? copy_ret : (ret < 0 ? ret : 0);
    }
    }

    if (alsa_ioctl_trace_enabled())
        printf("alsa-ioctl: pcm unknown cmd=0x%lx\n", cmd);
    return -ENOTTY;
}

static int alsa_pcm_cdev_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev;
    return alsa_pcm_ioctl_common(NULL, cmd, arg);
}

static ssize_t alsa_pcm_file_read(struct vfs_file *file, char *buf,
                                  size_t count, bool user)
{
    (void)file;
    return oss_read(NULL, user, buf, count);
}

static ssize_t alsa_pcm_file_write(struct vfs_file *file, const char *buf,
                                   size_t count, bool user)
{
    int ret = oss_pcm_write_data(file, user, buf, count);
    if (ret > 0) {
        spin_lock(&oss_lock);
        alsa_appl_ptr += alsa_frames_from_bytes((uint64)ret);
        alsa_pcm_state = SNDRV_PCM_STATE_RUNNING;
        spin_unlock(&oss_lock);
    }
    return ret;
}

static int alsa_pcm_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    return alsa_pcm_ioctl_common(file, cmd, arg);
}

static int oss_pcm_poll_events(short events) {
    short revents = 0;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND))
        revents |= events & (POLLIN | POLLRDNORM | POLLRDBAND);
    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        /*
         * The playback FIFO drains against monotonic time, not a hardware
         * interrupt.  Report writable only after enough room has drained for
         * a useful nonblocking write; epoll performs short internal rescans,
         * so waiters do not need a synthetic always-writable edge here.
         */
        spin_lock(&oss_lock);
        int writable = oss_playback_writable_locked();
        spin_unlock(&oss_lock);
        if (writable)
            revents |= events & (POLLOUT | POLLWRNORM | POLLWRBAND);
        else
            oss_schedule_notify();
    }
    return revents;
}

static int oss_poll(cdev_t *cdev, short events) {
    if (!oss_is_pcm_device(cdev))
        return events;
    return oss_pcm_poll_events(events);
}

static int oss_pcm_file_poll(struct vfs_file *file, short events) {
    (void)file;
    return oss_pcm_poll_events(events);
}

static int oss_pcm_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg) {
    (void)file;
    return oss_ioctl(NULL, cmd, arg);
}

static struct vfs_file_ops oss_pcm_file_ops = {
    .read = oss_pcm_file_read,
    .write = oss_pcm_file_write,
    .release = oss_pcm_file_release,
    .poll = oss_pcm_file_poll,
    .ioctl = oss_pcm_file_ioctl,
};

static struct vfs_file_ops alsa_pcm_file_ops = {
    .read = alsa_pcm_file_read,
    .write = alsa_pcm_file_write,
    .release = oss_pcm_file_release,
    .poll = oss_pcm_file_poll,
    .ioctl = alsa_pcm_file_ioctl,
};

static struct vfs_file_ops oss_sndstat_file_ops = {
    .read = oss_sndstat_file_read,
};

#define OSS_CDEV(_name, _minor, _readable, _writable, _read, _write, _open_file) \
    { \
        .dev = { \
            .major = OSS_SOUND_MAJOR, \
            .minor = (_minor), \
            .devname = (_name), \
            .devmode = S_IFCHR | 0666, \
        }, \
        .readable = (_readable), \
        .writable = (_writable), \
        .ops = { \
            .read = (_read), \
            .write = (_write), \
            .write_file = oss_write_file, \
            .open = oss_open, \
            .release = oss_release, \
            .ioctl = oss_ioctl, \
            .poll = oss_poll, \
            .open_file = (_open_file), \
        }, \
    }

static cdev_t mixer_cdev =
    OSS_CDEV("mixer", OSS_MINOR_MIXER, 1, 1, oss_read, oss_write, NULL);
static cdev_t mixer0_cdev =
    OSS_CDEV("mixer0", OSS_MINOR_MIXER0, 1, 1, oss_read, oss_write, NULL);
static cdev_t dsp_cdev =
    OSS_CDEV("dsp", OSS_MINOR_DSP, 1, 1, oss_read, oss_write,
             oss_pcm_open_file);
static cdev_t dsp0_cdev =
    OSS_CDEV("dsp0", OSS_MINOR_DSP0, 1, 1, oss_read, oss_write,
             oss_pcm_open_file);
static cdev_t audio_cdev =
    OSS_CDEV("audio", OSS_MINOR_AUDIO, 1, 1, oss_read, oss_write,
             oss_pcm_open_file);
static cdev_t audio0_cdev =
    OSS_CDEV("audio0", OSS_MINOR_AUDIO0, 1, 1, oss_read, oss_write,
             oss_pcm_open_file);
static cdev_t sndstat_cdev =
    OSS_CDEV("sndstat", OSS_MINOR_SNDSTAT, 1, 0, sndstat_read, NULL,
             oss_sndstat_open_file);

#define ALSA_CDEV(_name, _minor, _ioctl, _open_file) \
    { \
        .dev = { \
            .major = ALSA_SOUND_MAJOR, \
            .minor = (_minor), \
            .devname = (_name), \
            .devmode = S_IFCHR | 0666, \
        }, \
        .readable = 1, \
        .writable = 1, \
        .ops = { \
            .read = oss_read, \
            .write = oss_write, \
            .write_file = oss_write_file, \
            .open = oss_open, \
            .release = oss_release, \
            .ioctl = (_ioctl), \
            .poll = oss_poll, \
            .open_file = (_open_file), \
        }, \
    }

static cdev_t alsa_control0_cdev =
    ALSA_CDEV("snd/controlC0", ALSA_MINOR_CONTROL0, alsa_ctl_ioctl, NULL);
static cdev_t alsa_pcm0p_cdev =
    ALSA_CDEV("snd/pcmC0D0p", ALSA_MINOR_PCM0P, alsa_pcm_cdev_ioctl,
              alsa_pcm_open_file);

void ossaudiodevinit(void) {
    spin_init(&oss_lock, "ossaudio");
    oss_play_last_ms = sched_timer_now_ms();

    int ret = cdev_register(&mixer_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/mixer: %d", ret);
    ret = cdev_register(&mixer0_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/mixer0: %d", ret);
    ret = cdev_register(&dsp_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/dsp: %d", ret);
    ret = cdev_register(&dsp0_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/dsp0: %d", ret);
    ret = cdev_register(&audio_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/audio: %d", ret);
    ret = cdev_register(&audio0_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/audio0: %d", ret);
    ret = cdev_register(&sndstat_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/sndstat: %d", ret);
    ret = cdev_register(&alsa_control0_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/snd/controlC0: %d",
           ret);
    ret = cdev_register(&alsa_pcm0p_cdev);
    assert(ret == 0, "ossaudio: failed to register /dev/snd/pcmC0D0p: %d",
           ret);

    printf("audio: registered OSS /dev/dsp and ALSA /dev/snd/pcmC0D0p\n");
}
