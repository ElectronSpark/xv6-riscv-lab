#include <types.h>
#include <param.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <dev/cdev.h>
#include <mm/vm.h>
#include <vfs/fcntl.h>
#include <vfs/vfs_types.h>
#include "lock/spinlock.h"
#include "proc/sched.h"
#include "printf.h"
#include "uabi/poll.h"

#define OSS_SOUND_MAJOR 14

#define OSS_MINOR_MIXER   0
#define OSS_MINOR_DSP     3
#define OSS_MINOR_AUDIO   4
#define OSS_MINOR_SNDSTAT 6
#define OSS_MINOR_MIXER0  16
#define OSS_MINOR_DSP0    19
#define OSS_MINOR_AUDIO0  20

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

_Static_assert(sizeof(audio_buf_info_t) == 16, "audio_buf_info size");
_Static_assert(sizeof(count_info_t) == 12, "count_info size");
_Static_assert(sizeof(audio_errinfo_t) == 104, "audio_errinfo size");
_Static_assert(sizeof(oss_sysinfo_t) == 1248, "oss_sysinfo size");
_Static_assert(sizeof(oss_audioinfo_t) == 1180, "oss_audioinfo size");
_Static_assert(sizeof(oss_count_t) == 144, "oss_count size");

static int oss_format = OSS_DEFAULT_FORMAT;
static int oss_rate = OSS_DEFAULT_RATE;
static int oss_channels = OSS_DEFAULT_CHANNELS;
static int oss_trigger = PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT;
static int oss_volume = SOUND_MIXER_VALUE(100, 100);
static uint64 oss_play_written_bytes;
static uint64 oss_play_consumed_bytes;
static uint64 oss_play_last_ms;
static uint oss_rec_bytes;
static spinlock_t oss_lock;

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

static int oss_normalize_format(int fmt) {
    if (fmt == 0)
        return oss_format;
    if (fmt == AFMT_U8 || fmt == AFMT_S16_LE || fmt == AFMT_S24_LE ||
        fmt == AFMT_S32_LE)
        return fmt;
    if (fmt & AFMT_S16_LE)
        return AFMT_S16_LE;
    if (fmt & AFMT_U8)
        return AFMT_U8;
    if (fmt & AFMT_S24_LE)
        return AFMT_S24_LE;
    if (fmt & AFMT_S32_LE)
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

static void oss_reset_playback_locked(void) {
    oss_play_written_bytes = 0;
    oss_play_consumed_bytes = 0;
    oss_play_last_ms = sched_timer_now_ms();
}

static int oss_is_pcm_device(cdev_t *cdev) {
    if (cdev == NULL)
        return 0;
    int minor = cdev->dev.minor;
    return minor == OSS_MINOR_DSP || minor == OSS_MINOR_DSP0 ||
           minor == OSS_MINOR_AUDIO || minor == OSS_MINOR_AUDIO0;
}

static void oss_drain_playback(void) {
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
    oss_put_string(ai->name, sizeof(ai->name), "xv6 OSS virtual PCM");
    ai->caps = PCM_CAP_INPUT | PCM_CAP_OUTPUT | PCM_CAP_DUPLEX |
               PCM_CAP_TRIGGER | PCM_CAP_MULTI | PCM_CAP_VIRTUAL;
    ai->iformats = OSS_FORMATS;
    ai->oformats = OSS_FORMATS;
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
    (void)cdev;
    return 0;
}

static int oss_release(cdev_t *cdev) {
    (void)cdev;
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

static int oss_write_file(cdev_t *cdev, struct vfs_file *file, bool user,
                          const void *buf, size_t count) {
    (void)user;
    if (buf == NULL)
        return -EINVAL;
    if (!oss_is_pcm_device(cdev))
        return (int)count;

    bool nonblock = file != NULL && (file->f_flags & O_NONBLOCK);
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
    return (int)done;
}

static int oss_write(cdev_t *cdev, bool user, const void *buf, size_t count) {
    return oss_write_file(cdev, NULL, user, buf, count);
}

static int sndstat_read(cdev_t *cdev, bool user, void *buf, size_t count) {
    (void)cdev;
    static const char text[] =
        "OSS 4.0 compatible virtual audio\n"
        "Audio devices:\n"
        "0: xv6 OSS virtual PCM /dev/dsp\n"
        "Mixers:\n"
        "0: xv6 virtual mixer /dev/mixer\n";
    size_t len = sizeof(text) - 1;
    if (count < len)
        len = count;
    if (!user) {
        memcpy(buf, text, len);
        return len;
    }
    return either_copyout(1, (uint64)buf, (void *)text, len) < 0 ? -EFAULT : (int)len;
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
        return 0;

    case SNDCTL_DSP_GETFMTS:
        return oss_put_int(uarg, OSS_FORMATS);

    case SNDCTL_DSP_SETFMT:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_format = oss_normalize_format(value);
        oss_reset_playback_locked();
        value = oss_format;
        spin_unlock(&oss_lock);
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
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_CHANNELS:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_channels = value <= 1 ? 1 : 2;
        oss_reset_playback_locked();
        value = oss_channels;
        spin_unlock(&oss_lock);
        return oss_put_int(uarg, value);

    case SNDCTL_DSP_STEREO:
        if (oss_get_int(uarg, &value) < 0)
            return -EFAULT;
        spin_lock(&oss_lock);
        oss_channels = value ? 2 : 1;
        oss_reset_playback_locked();
        value = oss_channels == 2;
        spin_unlock(&oss_lock);
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
                PCM_CAP_TRIGGER | PCM_CAP_MULTI | PCM_CAP_VIRTUAL;
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
        uint64 played = oss_play_consumed_bytes;
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
        uint64 played = oss_play_consumed_bytes;
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

    return -ENOTTY;
}

static int oss_poll(cdev_t *cdev, short events) {
    if (!oss_is_pcm_device(cdev))
        return events;

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
    }
    return revents;
}

#define OSS_CDEV(_name, _minor, _readable, _writable, _read, _write) \
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
        }, \
    }

static cdev_t mixer_cdev =
    OSS_CDEV("mixer", OSS_MINOR_MIXER, 1, 1, oss_read, oss_write);
static cdev_t mixer0_cdev =
    OSS_CDEV("mixer0", OSS_MINOR_MIXER0, 1, 1, oss_read, oss_write);
static cdev_t dsp_cdev =
    OSS_CDEV("dsp", OSS_MINOR_DSP, 1, 1, oss_read, oss_write);
static cdev_t dsp0_cdev =
    OSS_CDEV("dsp0", OSS_MINOR_DSP0, 1, 1, oss_read, oss_write);
static cdev_t audio_cdev =
    OSS_CDEV("audio", OSS_MINOR_AUDIO, 1, 1, oss_read, oss_write);
static cdev_t audio0_cdev =
    OSS_CDEV("audio0", OSS_MINOR_AUDIO0, 1, 1, oss_read, oss_write);
static cdev_t sndstat_cdev =
    OSS_CDEV("sndstat", OSS_MINOR_SNDSTAT, 1, 0, sndstat_read, NULL);

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

    printf("audio: registered OSS virtual PCM /dev/dsp and /dev/mixer\n");
}
