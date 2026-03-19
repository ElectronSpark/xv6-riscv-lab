/*
 * lv_core.c — LVGL xv6 core: display driver, input driver, object system.
 *
 * This implements the full widget lifecycle:
 *   1. lv_init()          — open /dev/fb0 and /dev/mouse
 *   2. Create widgets      — lv_btn_create(), lv_label_create(), etc.
 *   3. lv_timer_handler()  — poll input, hit-test, dispatch events, redraw
 *   4. lv_deinit()        — cleanup
 */

#include "../lvgl_xv6.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ── fbdev ioctl structures (must match kernel/inc/dev/fb.h) ─────── */

#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t bits_per_pixel;
    uint32_t pitch;
};

struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t line_length;
};

/* ── Mouse event (must match kernel/inc/dev/ps2mouse.h) ──────────── */

struct mouse_event {
    int16_t  dx;
    int16_t  dy;
    uint8_t  buttons;
    uint8_t  flags;     /* bit 0: 1 = absolute coordinates */
    int8_t   dz;        /* scroll wheel delta */
    uint8_t  pad[1];
};

#define MOUSE_EVENT_F_ABSOLUTE  0x01

/* ══════════════════════════════════════════════════════════════════════
 *  Global state
 * ══════════════════════════════════════════════════════════════════════ */

static lv_disp_t  g_disp;
static lv_indev_t g_indev;
static lv_obj_t   g_screen;      /* root screen object */
static int        g_initialized;

/* Object pool — simple flat allocator */
#define LV_OBJ_POOL_SIZE 256
static lv_obj_t   g_obj_pool[LV_OBJ_POOL_SIZE];
static uint8_t    g_obj_used[LV_OBJ_POOL_SIZE];
static lv_obj_t  *g_pressed_obj = NULL;
static lv_obj_t  *g_focused_textbox = NULL;
static uint32_t   g_frame_count = 0;
static int8_t     g_scroll_dz = 0;  /* accumulated scroll wheel delta */

/* Dirty region tracking for partial flush during drag */
static int     g_partial_ok = 0;      /* 1 = partial flush is sufficient */
static int16_t g_dirty_x1, g_dirty_y1, g_dirty_x2, g_dirty_y2;

/* GPU-accelerated drag state — wireframe ghost while dragging */
static int     g_dragging_gpu = 0;    /* 1 = GPU wireframe drag active */
static int16_t g_ghost_x, g_ghost_y; /* current ghost outline position */
static int16_t g_ghost_w, g_ghost_h; /* ghost outline size */

/* Shadow buffer for GPU smart flush — compares current vs previous frame */
static uint32_t *g_shadow = NULL;
static int       g_force_full = 1;    /* force full-screen blit (first frame) */

/* Clip rectangle — when active, draw_pixel/draw_rect_fill skip pixels outside */
int     g_clip_active = 0;
int     g_clip_x1, g_clip_y1, g_clip_x2, g_clip_y2;

static void dirty_reset(void) {
    g_partial_ok = 0;
    g_dirty_x1 = g_dirty_y1 = 32767;
    g_dirty_x2 = g_dirty_y2 = -1;
}

static void dirty_expand(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (x < g_dirty_x1) g_dirty_x1 = x;
    if (y < g_dirty_y1) g_dirty_y1 = y;
    int16_t x2 = (int16_t)(x + w);
    int16_t y2 = (int16_t)(y + h);
    if (x2 > g_dirty_x2) g_dirty_x2 = x2;
    if (y2 > g_dirty_y2) g_dirty_y2 = y2;
}

/* Mouse cursor (simple 12×12 arrow drawn on top of everything) */
static const uint8_t cursor_bmp[12][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,1,1,0,0,0,0,0,0},
    {1,2,1,2,2,1,0,0,0,0,0,0},
    {1,1,0,1,2,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
};

/* ══════════════════════════════════════════════════════════════════════
 *  Object pool management
 * ══════════════════════════════════════════════════════════════════════ */

static lv_obj_t *obj_alloc(void)
{
    for (int i = 0; i < LV_OBJ_POOL_SIZE; i++) {
        if (!g_obj_used[i]) {
            g_obj_used[i] = 1;
            memset(&g_obj_pool[i], 0, sizeof(lv_obj_t));
            return &g_obj_pool[i];
        }
    }
    return NULL;
}

static void obj_free(lv_obj_t *obj)
{
    if (!obj) return;
    if (obj == g_pressed_obj)
        g_pressed_obj = NULL;
    if (obj == g_focused_textbox)
        g_focused_textbox = NULL;
    int idx = (int)(obj - g_obj_pool);
    if (idx >= 0 && idx < LV_OBJ_POOL_SIZE)
        g_obj_used[idx] = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Display driver
 * ══════════════════════════════════════════════════════════════════════ */

static int disp_init(void)
{
    g_disp.fb_fd = open("/dev/fb0", O_RDWR);
    if (g_disp.fb_fd < 0)
        return -1;

    struct fb_var_screeninfo vinfo;
    if (ioctl(g_disp.fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(g_disp.fb_fd);
        return -1;
    }

    g_disp.width  = vinfo.xres;
    g_disp.height = vinfo.yres;
    g_disp.bpp    = vinfo.bits_per_pixel;
    g_disp.pitch  = vinfo.pitch;
    g_disp.fb_size = g_disp.pitch * g_disp.height;

    /* Allocate local rendering buffer */
    g_disp.framebuf = (uint32_t *)malloc(g_disp.fb_size);
    if (!g_disp.framebuf) {
        close(g_disp.fb_fd);
        return -1;
    }
    memset(g_disp.framebuf, 0, g_disp.fb_size);

    /* Probe GPU acceleration: try a 1x1 fill rect ioctl */
    struct fb_gpu_fill probe = { .x = 0, .y = 0, .w = 0, .h = 0, .color = 0 };
    g_disp.gpu_accel = (ioctl(g_disp.fb_fd, FB_GPU_FILL_RECT, &probe) == 0);

    /* Allocate shadow buffer for GPU smart flush (partial blit) */
    if (g_disp.gpu_accel) {
        g_shadow = (uint32_t *)malloc(g_disp.fb_size);
        if (g_shadow)
            memset(g_shadow, 0, g_disp.fb_size);
    }
    g_force_full = 1;

    return 0;
}

static void disp_flush(void)
{
    if (g_disp.gpu_accel) {
        /* Use GPU blit to send the entire framebuffer */
        struct fb_gpu_blit cmd;
        cmd.x = 0;
        cmd.y = 0;
        cmd.w = g_disp.width;
        cmd.h = g_disp.height;
        cmd.src_pitch = g_disp.width * 4;
        cmd.pixels = (uint64_t)(uintptr_t)g_disp.framebuf;
        ioctl(g_disp.fb_fd, FB_GPU_BLIT, &cmd);
    } else {
        write(g_disp.fb_fd, g_disp.framebuf, g_disp.fb_size);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  GPU acceleration API
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t color_to_pixel(lv_color_t c)
{
    return ((uint32_t)c.alpha << 24) | ((uint32_t)c.red << 16) |
           ((uint32_t)c.green << 8)  | (uint32_t)c.blue;
}

int lv_gpu_available(void)
{
    return g_disp.gpu_accel;
}

int lv_gpu_fill_rect(int x, int y, int w, int h, lv_color_t color)
{
    if (!g_disp.gpu_accel)
        return -1;
    struct fb_gpu_fill cmd;
    cmd.x = (uint32_t)x;
    cmd.y = (uint32_t)y;
    cmd.w = (uint32_t)w;
    cmd.h = (uint32_t)h;
    cmd.color = color_to_pixel(color);
    return ioctl(g_disp.fb_fd, FB_GPU_FILL_RECT, &cmd);
}

int lv_gpu_blit(int x, int y, int w, int h,
                const uint32_t *pixels, int src_pitch)
{
    if (!g_disp.gpu_accel)
        return -1;
    struct fb_gpu_blit cmd;
    cmd.x = (uint32_t)x;
    cmd.y = (uint32_t)y;
    cmd.w = (uint32_t)w;
    cmd.h = (uint32_t)h;
    cmd.src_pitch = (uint32_t)src_pitch;
    cmd.pixels = (uint64_t)(uintptr_t)pixels;
    return ioctl(g_disp.fb_fd, FB_GPU_BLIT, &cmd);
}

int lv_gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{
    if (!g_disp.gpu_accel)
        return -1;
    struct fb_gpu_copy cmd;
    cmd.src_x = (uint32_t)sx;
    cmd.src_y = (uint32_t)sy;
    cmd.dst_x = (uint32_t)dx;
    cmd.dst_y = (uint32_t)dy;
    cmd.w = (uint32_t)w;
    cmd.h = (uint32_t)h;
    return ioctl(g_disp.fb_fd, FB_GPU_COPY_RECT, &cmd);
}

int lv_gpu_flush_area(int x, int y, int w, int h)
{
    if (!g_disp.gpu_accel)
        return -1;
    /* Blit the dirty region from the local framebuffer to screen */
    uint32_t *src = g_disp.framebuf + (uint32_t)y * g_disp.width + (uint32_t)x;
    struct fb_gpu_blit cmd;
    cmd.x = (uint32_t)x;
    cmd.y = (uint32_t)y;
    cmd.w = (uint32_t)w;
    cmd.h = (uint32_t)h;
    cmd.src_pitch = g_disp.width * 4;
    cmd.pixels = (uint64_t)(uintptr_t)src;
    return ioctl(g_disp.fb_fd, FB_GPU_BLIT, &cmd);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Input driver
 * ══════════════════════════════════════════════════════════════════════ */

static int indev_init(void)
{
    g_indev.mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    if (g_indev.mouse_fd < 0)
        g_indev.mouse_fd = -1;
    g_indev.kbd_fd = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
    if (g_indev.kbd_fd < 0)
        g_indev.kbd_fd = -1;
    g_indev.kbd_cb = NULL;
    g_indev.kbd_cb_data = NULL;
    /* Start cursor in center of screen */
    g_indev.x = (int16_t)(g_disp.width / 2);
    g_indev.y = (int16_t)(g_disp.height / 2);
    g_indev.buttons = 0;
    g_indev.pressed = 0;
    return 0;
}

static void indev_read(void)
{
    if (g_indev.mouse_fd < 0)
        return;

    g_scroll_dz = 0;  /* reset per-frame accumulator */

    struct mouse_event ev;
    /* Read all pending events */
    while (read(g_indev.mouse_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.flags & MOUSE_EVENT_F_ABSOLUTE) {
            /* VMware vmmouse: absolute coords in 0-65535 range */
            g_indev.x = (int16_t)((uint32_t)ev.dx * g_disp.width / 65536);
            g_indev.y = (int16_t)((uint32_t)ev.dy * g_disp.height / 65536);
        } else {
            /* PS/2 relative */
            g_indev.x += ev.dx;
            g_indev.y += ev.dy;
        }

        /* Clamp to screen bounds */
        if (g_indev.x < 0) g_indev.x = 0;
        if (g_indev.y < 0) g_indev.y = 0;
        if (g_indev.x >= (int16_t)g_disp.width)
            g_indev.x = (int16_t)(g_disp.width - 1);
        if (g_indev.y >= (int16_t)g_disp.height)
            g_indev.y = (int16_t)(g_disp.height - 1);

        g_indev.buttons = ev.buttons;

        /* Accumulate scroll wheel delta */
        g_scroll_dz += ev.dz;
    }
}

static void textbox_handle_key(lv_obj_t *tb, lv_kbd_event_t *ev)
{
    uint8_t key = ev->keycode;
    char *text = tb->spec.textbox.text;
    int len = (int)strlen(text);
    int cur = tb->spec.textbox.cursor;
    int maxl = tb->spec.textbox.max_len;
    if (maxl <= 0) maxl = (int)sizeof(tb->spec.textbox.text);

    if (key == 0x08) { /* Backspace */
        if (cur > 0) {
            memmove(&text[cur - 1], &text[cur], (size_t)(len - cur + 1));
            tb->spec.textbox.cursor = (uint8_t)(cur - 1);
        }
    } else if (key == 0x82) { /* Left */
        if (cur > 0) tb->spec.textbox.cursor--;
    } else if (key == 0x83) { /* Right */
        if (cur < len) tb->spec.textbox.cursor++;
    } else if (key == 0x84) { /* Home */
        tb->spec.textbox.cursor = 0;
    } else if (key == 0x85) { /* End */
        tb->spec.textbox.cursor = (uint8_t)len;
    } else if (key == 0x89) { /* Delete */
        if (cur < len)
            memmove(&text[cur], &text[cur + 1], (size_t)(len - cur));
    } else if (key == 0x0A || key == 0x0D) { /* Enter */
        tb->spec.textbox.focused = 0;
        g_focused_textbox = NULL;
    } else if (key == 0x1B) { /* Escape */
        tb->spec.textbox.focused = 0;
        g_focused_textbox = NULL;
    } else if (key >= 0x20 && key < 0x7F) { /* Printable */
        if (len < maxl - 1) {
            memmove(&text[cur + 1], &text[cur], (size_t)(len - cur + 1));
            text[cur] = (char)key;
            tb->spec.textbox.cursor = (uint8_t)(cur + 1);
        }
    }
}

static void indev_read_kbd(void)
{
    if (g_indev.kbd_fd < 0)
        return;

    lv_kbd_event_t ev;
    while (read(g_indev.kbd_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (g_focused_textbox) {
            if (ev.pressed)
                textbox_handle_key(g_focused_textbox, &ev);
        } else if (g_indev.kbd_cb) {
            g_indev.kbd_cb(&ev, g_indev.kbd_cb_data);
        }
    }
}

void lv_indev_set_kbd_callback(lv_kbd_cb_t cb, void *user_data)
{
    g_indev.kbd_cb = cb;
    g_indev.kbd_cb_data = user_data;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Drawing primitives
 * ══════════════════════════════════════════════════════════════════════ */

static inline void draw_pixel(int x, int y, lv_color_t c)
{
    if (x < 0 || x >= (int)g_disp.width || y < 0 || y >= (int)g_disp.height)
        return;
    if (g_clip_active && (x < g_clip_x1 || x >= g_clip_x2 ||
                          y < g_clip_y1 || y >= g_clip_y2))
        return;
    g_disp.framebuf[y * g_disp.width + x] = color_to_pixel(c);
}

static void draw_rect_fill(int x, int y, int w, int h, lv_color_t color)
{
    uint32_t pixel = color_to_pixel(color);

    /* Apply clip rect early for fast rejection */
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)g_disp.width) x2 = (int)g_disp.width;
    if (y2 > (int)g_disp.height) y2 = (int)g_disp.height;
    if (g_clip_active) {
        if (x1 < g_clip_x1) x1 = g_clip_x1;
        if (y1 < g_clip_y1) y1 = g_clip_y1;
        if (x2 > g_clip_x2) x2 = g_clip_x2;
        if (y2 > g_clip_y2) y2 = g_clip_y2;
    }
    if (x1 >= x2 || y1 >= y2)
        return;

    for (int row = y1; row < y2; row++) {
        uint32_t *dst = g_disp.framebuf + row * g_disp.width + x1;
        for (int col = x1; col < x2; col++)
            *dst++ = pixel;
    }
}

static void draw_rect_border(int x, int y, int w, int h, int bw, lv_color_t color)
{
    /* Top and bottom */
    draw_rect_fill(x, y, w, bw, color);
    draw_rect_fill(x, y + h - bw, w, bw, color);
    /* Left and right */
    draw_rect_fill(x, y + bw, bw, h - 2 * bw, color);
    draw_rect_fill(x + w - bw, y + bw, bw, h - 2 * bw, color);
}

static lv_color_t color_darken(lv_color_t c, int amount)
{
    lv_color_t d;
    d.red   = (c.red   > amount) ? c.red   - amount : 0;
    d.green = (c.green > amount) ? c.green - amount : 0;
    d.blue  = (c.blue  > amount) ? c.blue  - amount : 0;
    d.alpha = c.alpha;
    return d;
}

static lv_color_t color_lighten(lv_color_t c, int amount)
{
    lv_color_t d;
    d.red   = (c.red   + amount > 255) ? 255 : c.red   + amount;
    d.green = (c.green + amount > 255) ? 255 : c.green + amount;
    d.blue  = (c.blue  + amount > 255) ? 255 : c.blue  + amount;
    d.alpha = c.alpha;
    return d;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Widget absolute position (walk up parent chain)
 * ══════════════════════════════════════════════════════════════════════ */

static void obj_get_abs_pos(lv_obj_t *obj, int *ax, int *ay)
{
    *ax = obj->x;
    *ay = obj->y;
    lv_obj_t *p = obj->parent;
    while (p) {
        *ax += p->x + p->padding;
        *ay += p->y + p->padding - p->scroll_y;
        p = p->parent;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scrollbar helpers
 * ══════════════════════════════════════════════════════════════════════ */

/* Scrollbar drag state */
static lv_obj_t *g_scroll_target = NULL;

/* Compute bounding bottom of all children (content extent) */
static int16_t obj_content_bottom(lv_obj_t *obj)
{
    int16_t max_y = 0;
    for (int i = 0; i < obj->child_count; i++) {
        int16_t bottom = (int16_t)(obj->children[i]->y + obj->children[i]->h +
                                   obj->padding);
        if (bottom > max_y) max_y = bottom;
    }
    return max_y;
}

static void render_scrollbar(int ax, int ay, int w, int h,
                              int16_t scroll_y, int16_t content_h)
{
    int sb_x = ax + w - LV_SCROLLBAR_W;
    /* Track */
    draw_rect_fill(sb_x, ay, LV_SCROLLBAR_W, h, lv_color_make(35, 35, 40));
    draw_rect_fill(sb_x, ay, 1, h, lv_color_make(55, 55, 60));
    /* Thumb */
    int thumb_h = h * h / content_h;
    if (thumb_h < 20) thumb_h = 20;
    if (thumb_h > h)  thumb_h = h;
    int max_scroll = content_h - h;
    int thumb_y = 0;
    if (max_scroll > 0)
        thumb_y = (int)scroll_y * (h - thumb_h) / max_scroll;
    draw_rect_fill(sb_x + 2, ay + thumb_y + 1,
                   LV_SCROLLBAR_W - 4, thumb_h - 2,
                   lv_color_make(100, 100, 110));
}

/* Find scrollable container whose scrollbar area is under (mx, my) */
static lv_obj_t *find_scrollbar_hit(lv_obj_t *obj, int mx, int my)
{
    if (!obj || !obj->visible) return NULL;
    for (int i = obj->child_count - 1; i >= 0; i--) {
        lv_obj_t *r = find_scrollbar_hit(obj->children[i], mx, my);
        if (r) return r;
    }
    if (!obj->scrollable) return NULL;
    int16_t content_h = obj_content_bottom(obj);
    if (content_h <= obj->h) return NULL;
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);
    int sb_x = ax + obj->w - LV_SCROLLBAR_W;
    if (mx >= sb_x && mx < ax + obj->w && my >= ay && my < ay + obj->h)
        return obj;
    return NULL;
}

/* Set scroll position from mouse Y on scrollbar track */
static void scroll_to_mouse(lv_obj_t *obj, int mouse_y)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);
    int16_t content_h = obj_content_bottom(obj);
    int16_t visible_h = obj->h;
    if (content_h <= visible_h) return;
    int max_scroll = content_h - visible_h;
    int thumb_h = visible_h * visible_h / content_h;
    if (thumb_h < 20) thumb_h = 20;
    int track_range = visible_h - thumb_h;
    if (track_range <= 0) return;
    int rel_y = mouse_y - ay - thumb_h / 2;
    if (rel_y < 0) rel_y = 0;
    if (rel_y > track_range) rel_y = track_range;
    obj->scroll_y = (int16_t)(rel_y * max_scroll / track_range);
}

/* Find scrollable container whose area is under (mx, my) — for scroll wheel */
static lv_obj_t *find_scrollable_at(lv_obj_t *obj, int mx, int my)
{
    if (!obj || !obj->visible) return NULL;
    for (int i = obj->child_count - 1; i >= 0; i--) {
        lv_obj_t *r = find_scrollable_at(obj->children[i], mx, my);
        if (r) return r;
    }
    if (!obj->scrollable) return NULL;
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);
    if (mx >= ax && mx < ax + obj->w && my >= ay && my < ay + obj->h)
        return obj;
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Widget rendering
 * ══════════════════════════════════════════════════════════════════════ */

static void render_obj(lv_obj_t *obj);

static void render_label(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    lv_font_draw_string(g_disp.framebuf, (int)g_disp.width, (int)g_disp.height,
                         ax, ay, obj->spec.label.text, obj->text_color);
}

static void render_button(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    lv_color_t bg = obj->bg_color;
    if (obj->spec.btn.pressed)
        bg = color_darken(bg, 50);

    /* Button body */
    draw_rect_fill(ax, ay, obj->w, obj->h, bg);

    /* Border (3D effect) */
    lv_color_t hi = color_lighten(bg, 60);
    lv_color_t lo = color_darken(bg, 60);
    /* Top edge highlight */
    draw_rect_fill(ax, ay, obj->w, 2, obj->spec.btn.pressed ? lo : hi);
    /* Left edge highlight */
    draw_rect_fill(ax, ay, 2, obj->h, obj->spec.btn.pressed ? lo : hi);
    /* Bottom edge shadow */
    draw_rect_fill(ax, ay + obj->h - 2, obj->w, 2, obj->spec.btn.pressed ? hi : lo);
    /* Right edge shadow */
    draw_rect_fill(ax + obj->w - 2, ay, 2, obj->h, obj->spec.btn.pressed ? hi : lo);

    /* Button text (centered) */
    if (obj->spec.btn.text[0]) {
        int tw, th;
        lv_font_measure(obj->spec.btn.text, &tw, &th);
        int tx = ax + (obj->w - tw) / 2;
        int ty = ay + (obj->h - th) / 2;
        lv_font_draw_string(g_disp.framebuf, (int)g_disp.width,
                             (int)g_disp.height, tx, ty,
                             obj->spec.btn.text, obj->text_color);
    }
}

static void render_checkbox(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    /* Checkbox box (16x16) */
    int box_size = 16;
    draw_rect_fill(ax, ay + 2, box_size, box_size, LV_COLOR_WHITE);
    draw_rect_border(ax, ay + 2, box_size, box_size, 1, LV_COLOR_DARK_GRAY);

    /* Checkmark */
    if (obj->spec.checkbox.checked) {
        lv_color_t ck = lv_color_make(0, 120, 215);
        draw_rect_fill(ax + 3, ay + 5, box_size - 6, box_size - 6, ck);
    }

    /* Label */
    if (obj->spec.checkbox.text[0]) {
        lv_font_draw_string(g_disp.framebuf, (int)g_disp.width,
                             (int)g_disp.height,
                             ax + box_size + 6, ay + 2,
                             obj->spec.checkbox.text, obj->text_color);
    }
}

static void render_slider(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    /* Track */
    int track_h = 6;
    int track_y = ay + (obj->h - track_h) / 2;
    draw_rect_fill(ax, track_y, obj->w, track_h, LV_COLOR_LIGHT_GRAY);
    draw_rect_border(ax, track_y, obj->w, track_h, 1, LV_COLOR_GRAY);

    /* Filled portion */
    int range = obj->spec.slider.max_val - obj->spec.slider.min_val;
    int fill_w = 0;
    if (range > 0)
        fill_w = (obj->spec.slider.value - obj->spec.slider.min_val) * obj->w / range;
    if (fill_w > 0) {
        lv_color_t fill_color = lv_color_make(0, 120, 215);
        draw_rect_fill(ax, track_y, fill_w, track_h, fill_color);
    }

    /* Knob */
    int knob_w = 14, knob_h = 20;
    int knob_x = ax + fill_w - knob_w / 2;
    int knob_y = ay + (obj->h - knob_h) / 2;
    draw_rect_fill(knob_x, knob_y, knob_w, knob_h, LV_COLOR_WHITE);
    draw_rect_border(knob_x, knob_y, knob_w, knob_h, 1, LV_COLOR_GRAY);
}

static void render_base(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    /* Background */
    draw_rect_fill(ax, ay, obj->w, obj->h, obj->bg_color);

    /* Border */
    if (obj->border_width > 0)
        draw_rect_border(ax, ay, obj->w, obj->h,
                         obj->border_width, obj->border_color);
}

static void render_textbox(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);
    dirty_expand((int16_t)ax, (int16_t)ay, obj->w, obj->h);

    /* Background */
    draw_rect_fill(ax, ay, obj->w, obj->h, lv_color_make(20, 22, 30));

    /* Border — highlight when focused */
    lv_color_t bcolor = obj->spec.textbox.focused
        ? lv_color_make(60, 140, 220)
        : lv_color_make(80, 80, 100);
    draw_rect_border(ax, ay, obj->w, obj->h, 1, bcolor);

    /* Text */
    int tx = ax + 4;
    int ty = ay + (obj->h - LV_FONT_HEIGHT) / 2;
    if (obj->spec.textbox.text[0])
        lv_font_draw_string(g_disp.framebuf, (int)g_disp.width,
                             (int)g_disp.height, tx, ty,
                             obj->spec.textbox.text, obj->text_color);

    /* Cursor when focused */
    if (obj->spec.textbox.focused) {
        int cx = tx + obj->spec.textbox.cursor * LV_FONT_WIDTH;
        draw_rect_fill(cx, ay + 2, 2, obj->h - 4,
                       lv_color_make(220, 220, 255));
    }
}

static void render_window(lv_obj_t *obj)
{
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    /* Drop shadow */
    draw_rect_fill(ax + 3, ay + 3, obj->w, obj->h,
                   lv_color_make(10, 10, 10));

    /* Window background */
    draw_rect_fill(ax, ay, obj->w, obj->h, obj->bg_color);

    /* Title bar */
    draw_rect_fill(ax, ay, obj->w, LV_WIN_TITLE_H,
                   lv_color_make(0, 80, 160));

    /* Title text */
    if (obj->spec.win.title[0])
        lv_font_draw_string(g_disp.framebuf, (int)g_disp.width,
                             (int)g_disp.height,
                             ax + 8, ay + 6,
                             obj->spec.win.title, LV_COLOR_WHITE);

    /* Close button [X] */
    int cx = ax + obj->w - LV_WIN_CLOSE_W;
    draw_rect_fill(cx, ay, LV_WIN_CLOSE_W, LV_WIN_TITLE_H,
                   lv_color_make(200, 50, 50));
    lv_font_draw_string(g_disp.framebuf, (int)g_disp.width,
                         (int)g_disp.height,
                         cx + 10, ay + 6, "X", LV_COLOR_WHITE);

    /* Border */
    draw_rect_border(ax, ay, obj->w, obj->h, 1,
                     lv_color_make(100, 100, 110));
}

static void render_obj(lv_obj_t *obj)
{
    if (!obj || !obj->visible)
        return;

    switch (obj->type) {
    case LV_OBJ_TYPE_LABEL:
        render_label(obj);
        break;
    case LV_OBJ_TYPE_BUTTON:
        render_button(obj);
        break;
    case LV_OBJ_TYPE_CHECKBOX:
        render_checkbox(obj);
        break;
    case LV_OBJ_TYPE_SLIDER:
        render_slider(obj);
        break;
    case LV_OBJ_TYPE_WINDOW:
        render_window(obj);
        break;
    case LV_OBJ_TYPE_TEXTBOX:
        render_textbox(obj);
        break;
    case LV_OBJ_TYPE_BASE:
    case LV_OBJ_TYPE_CONTAINER:
    default:
        render_base(obj);
        break;
    }

    /* Check if this is a scrollable container with overflow */
    int need_scroll = 0;
    int16_t content_h = 0;
    int saved_clip_active = 0;
    int saved_cx1 = 0, saved_cy1 = 0, saved_cx2 = 0, saved_cy2 = 0;

    if (obj->scrollable &&
        (obj->type == LV_OBJ_TYPE_CONTAINER || obj->type == LV_OBJ_TYPE_BASE)) {
        content_h = obj_content_bottom(obj);
        if (content_h > obj->h) {
            need_scroll = 1;

            /* Clamp scroll_y */
            int16_t max_scroll = (int16_t)(content_h - obj->h);
            if (obj->scroll_y > max_scroll) obj->scroll_y = max_scroll;
            if (obj->scroll_y < 0) obj->scroll_y = 0;

            /* Save clip state */
            saved_clip_active = g_clip_active;
            saved_cx1 = g_clip_x1; saved_cy1 = g_clip_y1;
            saved_cx2 = g_clip_x2; saved_cy2 = g_clip_y2;

            /* Set clip to container bounds (minus scrollbar width) */
            int ax, ay;
            obj_get_abs_pos(obj, &ax, &ay);
            int nx1 = ax, ny1 = ay;
            int nx2 = ax + obj->w - LV_SCROLLBAR_W, ny2 = ay + obj->h;
            if (saved_clip_active) {
                if (nx1 < saved_cx1) nx1 = saved_cx1;
                if (ny1 < saved_cy1) ny1 = saved_cy1;
                if (nx2 > saved_cx2) nx2 = saved_cx2;
                if (ny2 > saved_cy2) ny2 = saved_cy2;
            }
            g_clip_active = 1;
            g_clip_x1 = nx1; g_clip_y1 = ny1;
            g_clip_x2 = nx2; g_clip_y2 = ny2;
        }
    }

    /* Render children */
    for (int i = 0; i < obj->child_count; i++)
        render_obj(obj->children[i]);

    /* Draw scrollbar and restore clip */
    if (need_scroll) {
        g_clip_active = saved_clip_active;
        g_clip_x1 = saved_cx1; g_clip_y1 = saved_cy1;
        g_clip_x2 = saved_cx2; g_clip_y2 = saved_cy2;

        int ax, ay;
        obj_get_abs_pos(obj, &ax, &ay);
        render_scrollbar(ax, ay, obj->w, obj->h, obj->scroll_y, content_h);
    }
}

static void draw_cursor(void)
{
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 12; col++) {
            uint8_t v = cursor_bmp[row][col];
            if (v == 1)
                draw_pixel(g_indev.x + col, g_indev.y + row, LV_COLOR_BLACK);
            else if (v == 2)
                draw_pixel(g_indev.x + col, g_indev.y + row, LV_COLOR_WHITE);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Hit testing
 * ══════════════════════════════════════════════════════════════════════ */

static lv_obj_t *hit_test(lv_obj_t *obj, int mx, int my)
{
    if (!obj || !obj->visible)
        return NULL;

    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);

    /* Check children first (front-to-back: last child rendered last) */
    for (int i = obj->child_count - 1; i >= 0; i--) {
        lv_obj_t *hit = hit_test(obj->children[i], mx, my);
        if (hit) return hit;
    }

    /* Check self */
    if (mx >= ax && mx < ax + obj->w && my >= ay && my < ay + obj->h) {
        /* Only interactive types are clickable */
        if (obj->type == LV_OBJ_TYPE_BUTTON ||
            obj->type == LV_OBJ_TYPE_CHECKBOX ||
            obj->type == LV_OBJ_TYPE_SLIDER ||
            obj->type == LV_OBJ_TYPE_WINDOW ||
            obj->type == LV_OBJ_TYPE_TEXTBOX ||
            obj->event_cb != NULL)
            return obj;
    }

    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Initialization / API
 * ══════════════════════════════════════════════════════════════════════ */

int lv_init(void)
{
    if (g_initialized)
        return 0;

    memset(g_obj_used, 0, sizeof(g_obj_used));

    if (disp_init() < 0)
        return -1;

    indev_init();

    /* Initialize root screen object */
    memset(&g_screen, 0, sizeof(g_screen));
    g_screen.type = LV_OBJ_TYPE_BASE;
    g_screen.w = (int16_t)g_disp.width;
    g_screen.h = (int16_t)g_disp.height;
    g_screen.bg_color = lv_color_make(45, 45, 48);  /* dark theme background */
    g_screen.text_color = LV_COLOR_WHITE;
    g_screen.visible = 1;

    g_initialized = 1;
    return 0;
}

void lv_deinit(void)
{
    if (!g_initialized)
        return;
    if (g_shadow)
        free(g_shadow);
    g_shadow = NULL;
    if (g_disp.framebuf)
        free(g_disp.framebuf);
    if (g_disp.fb_fd >= 0)
        close(g_disp.fb_fd);
    if (g_indev.mouse_fd >= 0)
        close(g_indev.mouse_fd);
    g_initialized = 0;
    g_force_full = 1;
}

lv_disp_t  *lv_disp_get(void)  { return &g_disp; }
lv_indev_t *lv_indev_get(void) { return &g_indev; }
lv_obj_t   *lv_scr_act(void)   { return &g_screen; }

/* ══════════════════════════════════════════════════════════════════════
 *  Object system implementation
 * ══════════════════════════════════════════════════════════════════════ */

static lv_obj_t *obj_create_typed(lv_obj_t *parent, lv_obj_type_t type)
{
    lv_obj_t *obj = obj_alloc();
    if (!obj) return NULL;

    obj->type = type;
    obj->parent = parent;
    obj->visible = 1;
    obj->bg_color = lv_color_make(60, 60, 64);
    obj->border_color = LV_COLOR_GRAY;
    obj->text_color = LV_COLOR_WHITE;
    obj->border_width = 0;
    obj->radius = 0;
    obj->padding = 4;

    if (parent && parent->child_count < LV_OBJ_MAX_CHILDREN) {
        parent->children[parent->child_count++] = obj;
    }

    return obj;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *obj = obj_create_typed(parent, LV_OBJ_TYPE_CONTAINER);
    if (obj) {
        obj->w = 100;
        obj->h = 50;
    }
    return obj;
}

void lv_obj_clean(lv_obj_t *obj)
{
    if (!obj) return;
    for (int i = 0; i < obj->child_count; i++) {
        lv_obj_clean(obj->children[i]);
        obj_free(obj->children[i]);
        obj->children[i] = NULL;
    }
    obj->child_count = 0;
}

void lv_obj_del(lv_obj_t *obj)
{
    if (!obj) return;

    /* Remove from parent */
    if (obj->parent) {
        lv_obj_t *p = obj->parent;
        for (int i = 0; i < p->child_count; i++) {
            if (p->children[i] == obj) {
                for (int j = i; j < p->child_count - 1; j++)
                    p->children[j] = p->children[j + 1];
                p->child_count--;
                break;
            }
        }
    }

    lv_obj_clean(obj);
    obj_free(obj);
}

void lv_obj_move_to_front(lv_obj_t *obj)
{
    if (!obj || !obj->parent) return;
    lv_obj_t *p = obj->parent;
    int idx = -1;
    for (int i = 0; i < p->child_count; i++) {
        if (p->children[i] == obj) { idx = i; break; }
    }
    if (idx < 0 || idx == p->child_count - 1) return;
    for (int i = idx; i < p->child_count - 1; i++)
        p->children[i] = p->children[i + 1];
    p->children[p->child_count - 1] = obj;
}

void lv_obj_set_pos(lv_obj_t *obj, int16_t x, int16_t y)
{
    if (obj) { obj->x = x; obj->y = y; }
}

void lv_obj_set_size(lv_obj_t *obj, int16_t w, int16_t h)
{
    if (!obj) return;
    obj->w = w;
    obj->h = h;
    if (obj->type == LV_OBJ_TYPE_WINDOW && obj->child_count > 0) {
        obj->children[0]->w = (int16_t)(w - 2);
        obj->children[0]->h = (int16_t)(h - LV_WIN_TITLE_H - 1);
    }
}

void lv_obj_set_width(lv_obj_t *obj, int16_t w)
{
    if (obj) obj->w = w;
}

void lv_obj_set_height(lv_obj_t *obj, int16_t h)
{
    if (obj) obj->h = h;
}

void lv_obj_align(lv_obj_t *obj, lv_align_t align, int16_t x_ofs, int16_t y_ofs)
{
    if (!obj || !obj->parent) return;
    lv_obj_t *p = obj->parent;
    int pw = p->w - 2 * p->padding;
    int ph = p->h - 2 * p->padding;

    switch (align) {
    case LV_ALIGN_TOP_LEFT:
        obj->x = x_ofs; obj->y = y_ofs; break;
    case LV_ALIGN_TOP_MID:
        obj->x = (int16_t)((pw - obj->w) / 2 + x_ofs); obj->y = y_ofs; break;
    case LV_ALIGN_TOP_RIGHT:
        obj->x = (int16_t)(pw - obj->w + x_ofs); obj->y = y_ofs; break;
    case LV_ALIGN_CENTER:
        obj->x = (int16_t)((pw - obj->w) / 2 + x_ofs);
        obj->y = (int16_t)((ph - obj->h) / 2 + y_ofs); break;
    case LV_ALIGN_BOTTOM_LEFT:
        obj->x = x_ofs; obj->y = (int16_t)(ph - obj->h + y_ofs); break;
    case LV_ALIGN_BOTTOM_MID:
        obj->x = (int16_t)((pw - obj->w) / 2 + x_ofs);
        obj->y = (int16_t)(ph - obj->h + y_ofs); break;
    case LV_ALIGN_BOTTOM_RIGHT:
        obj->x = (int16_t)(pw - obj->w + x_ofs);
        obj->y = (int16_t)(ph - obj->h + y_ofs); break;
    case LV_ALIGN_LEFT_MID:
        obj->x = x_ofs;
        obj->y = (int16_t)((ph - obj->h) / 2 + y_ofs); break;
    case LV_ALIGN_RIGHT_MID:
        obj->x = (int16_t)(pw - obj->w + x_ofs);
        obj->y = (int16_t)((ph - obj->h) / 2 + y_ofs); break;
    }
}

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color)
    { if (obj) obj->bg_color = color; }
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color)
    { if (obj) obj->border_color = color; }
void lv_obj_set_style_border_width(lv_obj_t *obj, uint8_t width)
    { if (obj) obj->border_width = width; }
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color)
    { if (obj) obj->text_color = color; }
void lv_obj_set_style_radius(lv_obj_t *obj, uint8_t radius)
    { if (obj) obj->radius = radius; }
void lv_obj_set_style_pad_all(lv_obj_t *obj, uint8_t pad)
    { if (obj) obj->padding = pad; }

void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_t event,
                           void *user_data)
{
    if (obj) {
        obj->event_cb = cb;
        obj->user_data = user_data;
    }
    (void)event;
}

void lv_obj_set_hidden(lv_obj_t *obj, int hidden)
{
    if (obj) obj->visible = !hidden;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Label widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *lbl = obj_create_typed(parent, LV_OBJ_TYPE_LABEL);
    if (lbl) {
        lbl->spec.label.text[0] = '\0';
        lbl->w = 0;
        lbl->h = LV_FONT_HEIGHT;
    }
    return lbl;
}

void lv_label_set_text(lv_obj_t *label, const char *text)
{
    if (!label || label->type != LV_OBJ_TYPE_LABEL)
        return;
    strncpy(label->spec.label.text, text, LV_LABEL_MAX_TEXT - 1);
    label->spec.label.text[LV_LABEL_MAX_TEXT - 1] = '\0';

    int tw, th;
    lv_font_measure(label->spec.label.text, &tw, &th);
    label->w = (int16_t)tw;
    label->h = (int16_t)th;
}

void lv_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
    if (!label || label->type != LV_OBJ_TYPE_LABEL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(label->spec.label.text, LV_LABEL_MAX_TEXT, fmt, ap);
    va_end(ap);

    int tw, th;
    lv_font_measure(label->spec.label.text, &tw, &th);
    label->w = (int16_t)tw;
    label->h = (int16_t)th;
}

const char *lv_label_get_text(lv_obj_t *label)
{
    if (!label || label->type != LV_OBJ_TYPE_LABEL)
        return "";
    return label->spec.label.text;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Button widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_btn_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *btn = obj_create_typed(parent, LV_OBJ_TYPE_BUTTON);
    if (btn) {
        btn->w = 120;
        btn->h = 40;
        btn->bg_color = lv_color_make(0, 120, 215);
        btn->text_color = LV_COLOR_WHITE;
        btn->spec.btn.text[0] = '\0';
        btn->spec.btn.pressed = 0;
    }
    return btn;
}

void lv_btn_set_text(lv_obj_t *btn, const char *text)
{
    if (!btn || btn->type != LV_OBJ_TYPE_BUTTON)
        return;
    strncpy(btn->spec.btn.text, text, sizeof(btn->spec.btn.text) - 1);
    btn->spec.btn.text[sizeof(btn->spec.btn.text) - 1] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════
 *  Checkbox widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_checkbox_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *cb = obj_create_typed(parent, LV_OBJ_TYPE_CHECKBOX);
    if (cb) {
        cb->w = 200;
        cb->h = 22;
        cb->spec.checkbox.text[0] = '\0';
        cb->spec.checkbox.checked = 0;
    }
    return cb;
}

void lv_checkbox_set_text(lv_obj_t *cb, const char *text)
{
    if (!cb || cb->type != LV_OBJ_TYPE_CHECKBOX)
        return;
    strncpy(cb->spec.checkbox.text, text, sizeof(cb->spec.checkbox.text) - 1);
    cb->spec.checkbox.text[sizeof(cb->spec.checkbox.text) - 1] = '\0';
}

int lv_checkbox_is_checked(lv_obj_t *cb)
{
    if (!cb || cb->type != LV_OBJ_TYPE_CHECKBOX)
        return 0;
    return cb->spec.checkbox.checked;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Slider widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_slider_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *sl = obj_create_typed(parent, LV_OBJ_TYPE_SLIDER);
    if (sl) {
        sl->w = 200;
        sl->h = 30;
        sl->spec.slider.min_val = 0;
        sl->spec.slider.max_val = 100;
        sl->spec.slider.value = 50;
    }
    return sl;
}

void lv_slider_set_range(lv_obj_t *slider, int16_t min_val, int16_t max_val)
{
    if (!slider || slider->type != LV_OBJ_TYPE_SLIDER)
        return;
    slider->spec.slider.min_val = min_val;
    slider->spec.slider.max_val = max_val;
}

void lv_slider_set_value(lv_obj_t *slider, int16_t value)
{
    if (!slider || slider->type != LV_OBJ_TYPE_SLIDER)
        return;
    if (value < slider->spec.slider.min_val)
        value = slider->spec.slider.min_val;
    if (value > slider->spec.slider.max_val)
        value = slider->spec.slider.max_val;
    slider->spec.slider.value = value;
}

int16_t lv_slider_get_value(lv_obj_t *slider)
{
    if (!slider || slider->type != LV_OBJ_TYPE_SLIDER)
        return 0;
    return slider->spec.slider.value;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Textbox widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_textbox_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *tb = obj_create_typed(parent, LV_OBJ_TYPE_TEXTBOX);
    if (tb) {
        tb->w = 160;
        tb->h = 22;
        tb->text_color = LV_COLOR_WHITE;
        tb->spec.textbox.text[0] = '\0';
        tb->spec.textbox.cursor = 0;
        tb->spec.textbox.max_len = (uint8_t)(sizeof(tb->spec.textbox.text) - 1);
        tb->spec.textbox.focused = 0;
    }
    return tb;
}

void lv_textbox_set_text(lv_obj_t *tb, const char *text)
{
    if (!tb || tb->type != LV_OBJ_TYPE_TEXTBOX)
        return;
    strncpy(tb->spec.textbox.text, text, sizeof(tb->spec.textbox.text) - 1);
    tb->spec.textbox.text[sizeof(tb->spec.textbox.text) - 1] = '\0';
    int len = (int)strlen(tb->spec.textbox.text);
    if (tb->spec.textbox.cursor > len)
        tb->spec.textbox.cursor = (uint8_t)len;
}

const char *lv_textbox_get_text(lv_obj_t *tb)
{
    if (!tb || tb->type != LV_OBJ_TYPE_TEXTBOX)
        return "";
    return tb->spec.textbox.text;
}

void lv_textbox_set_max_length(lv_obj_t *tb, int max_len)
{
    if (!tb || tb->type != LV_OBJ_TYPE_TEXTBOX)
        return;
    if (max_len > (int)sizeof(tb->spec.textbox.text))
        max_len = (int)sizeof(tb->spec.textbox.text);
    tb->spec.textbox.max_len = (uint8_t)max_len;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Window widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_win_create(lv_obj_t *parent)
{
    if (!parent) parent = &g_screen;
    lv_obj_t *win = obj_create_typed(parent, LV_OBJ_TYPE_WINDOW);
    if (!win) return NULL;

    win->w = 400;
    win->h = 300;
    win->bg_color = lv_color_make(50, 50, 55);
    win->border_color = lv_color_make(80, 80, 90);
    win->border_width = 1;
    win->padding = 0;
    win->spec.win.title[0] = '\0';
    win->spec.win.dragging = 0;

    /* Create internal content panel (always children[0]) */
    lv_obj_t *content = obj_create_typed(win, LV_OBJ_TYPE_CONTAINER);
    if (content) {
        content->x = 1;
        content->y = LV_WIN_TITLE_H;
        content->w = (int16_t)(win->w - 2);
        content->h = (int16_t)(win->h - LV_WIN_TITLE_H - 1);
        content->bg_color = lv_color_make(50, 50, 55);
        content->border_width = 0;
        content->padding = 8;
    }

    return win;
}

void lv_win_set_title(lv_obj_t *win, const char *title)
{
    if (!win || win->type != LV_OBJ_TYPE_WINDOW) return;
    strncpy(win->spec.win.title, title, sizeof(win->spec.win.title) - 1);
    win->spec.win.title[sizeof(win->spec.win.title) - 1] = '\0';
}

lv_obj_t *lv_win_get_content(lv_obj_t *win)
{
    if (!win || win->type != LV_OBJ_TYPE_WINDOW || win->child_count < 1)
        return NULL;
    return win->children[0];
}

void lv_win_close(lv_obj_t *win)
{
    if (!win || win->type != LV_OBJ_TYPE_WINDOW) return;
    lv_obj_del(win);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scrollable container API
 * ══════════════════════════════════════════════════════════════════════ */

void lv_obj_set_scrollable(lv_obj_t *obj, int enable)
{
    if (obj) obj->scrollable = enable ? 1 : 0;
}

void lv_obj_scroll_by(lv_obj_t *obj, int16_t dy)
{
    if (!obj) return;
    int16_t new_y = (int16_t)(obj->scroll_y + dy);
    if (new_y < 0) new_y = 0;
    int16_t content_h = obj_content_bottom(obj);
    int16_t max_scroll = (int16_t)(content_h - obj->h);
    if (max_scroll < 0) max_scroll = 0;
    if (new_y > max_scroll) new_y = max_scroll;
    obj->scroll_y = new_y;
}

uint32_t lv_get_frame_count(void)
{
    return g_frame_count;
}

void lv_obj_invalidate(lv_obj_t *obj)
{
    if (!obj) return;
    int ax, ay;
    obj_get_abs_pos(obj, &ax, &ay);
    dirty_expand((int16_t)ax, (int16_t)ay, obj->w, obj->h);
}

void lv_force_refresh(void)
{
    g_force_full = 1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Main loop — input processing + rendering
 * ══════════════════════════════════════════════════════════════════════ */

int lv_timer_handler(void)
{
    if (!g_initialized) return -1;

    g_frame_count++;

    /* 1. Read input */
    int prev_btn = g_indev.buttons & 1;
    int16_t prev_x = g_indev.x, prev_y = g_indev.y;
    indev_read();
    indev_read_kbd();
    int cur_btn = g_indev.buttons & 1;

    /* 2. Hit test & dispatch events */

    /* Restore window visibility before hit-test if GPU drag is ending,
     * so the close button can be detected */
    if (!cur_btn && prev_btn && g_dragging_gpu && g_pressed_obj) {
        g_pressed_obj->visible = 1;
        g_dragging_gpu = 0;
        g_force_full = 1; /* shadow is stale after ghost outlines */
    }

    lv_obj_t *hovered = hit_test(&g_screen, g_indev.x, g_indev.y);

    /* Button press */
    if (cur_btn && !prev_btn) {
        /* Defocus textbox if clicking elsewhere */
        if (g_focused_textbox && hovered != g_focused_textbox) {
            g_focused_textbox->spec.textbox.focused = 0;
            g_focused_textbox = NULL;
        }

        /* Check for scrollbar click before normal hit test */
        lv_obj_t *sb_hit = find_scrollbar_hit(&g_screen,
                                               g_indev.x, g_indev.y);
        if (sb_hit) {
            g_scroll_target = sb_hit;
            scroll_to_mouse(sb_hit, g_indev.y);
        } else if (hovered) {
            g_pressed_obj = hovered;

            /* Textbox: set focus and cursor position */
            if (hovered->type == LV_OBJ_TYPE_TEXTBOX) {
                hovered->spec.textbox.focused = 1;
                g_focused_textbox = hovered;
                int _ax, _ay;
                obj_get_abs_pos(hovered, &_ax, &_ay);
                int cx = (g_indev.x - _ax - 4) / LV_FONT_WIDTH;
                int tlen = (int)strlen(hovered->spec.textbox.text);
                if (cx < 0) cx = 0;
                if (cx > tlen) cx = tlen;
                hovered->spec.textbox.cursor = (uint8_t)cx;
            }

            /* Window: bring to front and start drag on title bar */
            if (hovered->type == LV_OBJ_TYPE_WINDOW) {
                lv_obj_move_to_front(hovered);
                int ax, ay;
                obj_get_abs_pos(hovered, &ax, &ay);
                int ry = g_indev.y - ay;
                int rx = g_indev.x - ax;
                if (ry < LV_WIN_TITLE_H &&
                    rx < hovered->w - LV_WIN_CLOSE_W) {
                    hovered->spec.win.dragging = 1;
                    hovered->spec.win.drag_ox =
                        (int16_t)(g_indev.x - hovered->x);
                    hovered->spec.win.drag_oy =
                        (int16_t)(g_indev.y - hovered->y);

                    /* Start GPU wireframe drag: hide window,
                     * render background once, then only draw outlines */
                    if (g_disp.gpu_accel) {
                        g_dragging_gpu = 1;
                        g_ghost_x = hovered->x;
                        g_ghost_y = hovered->y;
                        g_ghost_w = hovered->w;
                        g_ghost_h = hovered->h;
                        hovered->visible = 0; /* hide during drag */
                    }
                }
            } else {
                /* Bring parent window to front */
                lv_obj_t *pw = hovered->parent;
                while (pw && pw->type != LV_OBJ_TYPE_WINDOW)
                    pw = pw->parent;
                if (pw)
                    lv_obj_move_to_front(pw);
            }

            if (hovered->type == LV_OBJ_TYPE_BUTTON)
                hovered->spec.btn.pressed = 1;
            if (hovered->event_cb)
                hovered->event_cb(hovered, LV_EVENT_PRESSED);
        }
    }

    /* Button release */
    if (!cur_btn && prev_btn) {
        if (g_scroll_target) {
            g_scroll_target = NULL;
        } else if (g_pressed_obj) {
            if (g_pressed_obj->type == LV_OBJ_TYPE_WINDOW) {
                /* Window-specific release */
                g_pressed_obj->spec.win.dragging = 0;

                if (g_pressed_obj == hovered) {
                    int ax, ay;
                    obj_get_abs_pos(g_pressed_obj, &ax, &ay);
                    int ry = g_indev.y - ay;
                    int rx = g_indev.x - ax;
                    if (ry < LV_WIN_TITLE_H &&
                        rx >= g_pressed_obj->w - LV_WIN_CLOSE_W) {
                        if (g_pressed_obj->event_cb)
                            g_pressed_obj->event_cb(g_pressed_obj,
                                                    LV_EVENT_CLOSE);
                    }
                }
            } else {
                if (g_pressed_obj->type == LV_OBJ_TYPE_BUTTON)
                    g_pressed_obj->spec.btn.pressed = 0;

                /* Click = press + release on same object */
                if (g_pressed_obj == hovered) {
                    if (g_pressed_obj->type == LV_OBJ_TYPE_CHECKBOX)
                        g_pressed_obj->spec.checkbox.checked ^= 1;
                    if (g_pressed_obj->event_cb)
                        g_pressed_obj->event_cb(g_pressed_obj,
                                                LV_EVENT_CLICKED);
                }

                if (g_pressed_obj && g_pressed_obj->event_cb)
                    g_pressed_obj->event_cb(g_pressed_obj,
                                            LV_EVENT_RELEASED);
            }

            g_pressed_obj = NULL;
        }
    }

    /* Slider dragging */
    if (cur_btn && g_pressed_obj &&
        g_pressed_obj->type == LV_OBJ_TYPE_SLIDER) {
        int ax, ay;
        obj_get_abs_pos(g_pressed_obj, &ax, &ay);
        int rel_x = g_indev.x - ax;
        int range = g_pressed_obj->spec.slider.max_val -
                    g_pressed_obj->spec.slider.min_val;
        int16_t new_val = g_pressed_obj->spec.slider.min_val +
                          (int16_t)(rel_x * range / g_pressed_obj->w);
        if (new_val < g_pressed_obj->spec.slider.min_val)
            new_val = g_pressed_obj->spec.slider.min_val;
        if (new_val > g_pressed_obj->spec.slider.max_val)
            new_val = g_pressed_obj->spec.slider.max_val;
        if (new_val != g_pressed_obj->spec.slider.value) {
            g_pressed_obj->spec.slider.value = new_val;
            if (g_pressed_obj->event_cb)
                g_pressed_obj->event_cb(g_pressed_obj,
                                        LV_EVENT_VALUE_CHANGED);
        }
    }

    /* Window dragging — GPU wireframe path */
    if (cur_btn && g_pressed_obj &&
        g_pressed_obj->type == LV_OBJ_TYPE_WINDOW &&
        g_pressed_obj->spec.win.dragging) {

        /* Update window position */
        g_pressed_obj->x =
            (int16_t)(g_indev.x - g_pressed_obj->spec.win.drag_ox);
        g_pressed_obj->y =
            (int16_t)(g_indev.y - g_pressed_obj->spec.win.drag_oy);

        if (g_dragging_gpu) {
            g_partial_ok = 1;  /* skip full render */
        }
    }

    /* Scrollbar dragging (continuous while button held) */
    if (cur_btn && g_scroll_target) {
        scroll_to_mouse(g_scroll_target, g_indev.y);
    }

    /* Mouse scroll wheel */
    if (g_scroll_dz != 0) {
        lv_obj_t *sc = find_scrollable_at(&g_screen, g_indev.x, g_indev.y);
        if (sc) {
            /* Each scroll tick = 30 pixels */
            lv_obj_scroll_by(sc, (int16_t)(g_scroll_dz * 30));
        }
    }

    /* Track cursor movement as dirty for partial flush */
    if (prev_x != g_indev.x || prev_y != g_indev.y) {
        dirty_expand(prev_x, prev_y, 13, 13);    /* old cursor */
        dirty_expand(g_indev.x, g_indev.y, 13, 13); /* new cursor */
    }

    /* 3. Render */
    lv_refr_now();

    return 0;
}

/* Helper: clamp rect to screen, return 0 if empty */
static int clamp_rect(int *x1, int *y1, int *x2, int *y2)
{
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > (int)g_disp.width)  *x2 = (int)g_disp.width;
    if (*y2 > (int)g_disp.height) *y2 = (int)g_disp.height;
    return (*x1 < *x2 && *y1 < *y2);
}

/* Draw a wireframe rectangle on the LFB using GPU fill_rect */
static void gpu_draw_outline(int x, int y, int w, int h, lv_color_t color, int thick)
{
    /* Top */
    lv_gpu_fill_rect(x, y, w, thick, color);
    /* Bottom */
    lv_gpu_fill_rect(x, y + h - thick, w, thick, color);
    /* Left */
    lv_gpu_fill_rect(x, y + thick, thick, h - 2 * thick, color);
    /* Right */
    lv_gpu_fill_rect(x + w - thick, y + thick, thick, h - 2 * thick, color);
}

/* Erase old ghost outline by re-blitting that area from local framebuffer */
static void gpu_erase_outline(int x, int y, int w, int h, int thick)
{
    /* Top */
    lv_gpu_flush_area(x, y, w, thick);
    /* Bottom */
    lv_gpu_flush_area(x, y + h - thick, w, thick);
    /* Left */
    lv_gpu_flush_area(x, y + thick, thick, h - 2 * thick);
    /* Right */
    lv_gpu_flush_area(x + w - thick, y + thick, thick, h - 2 * thick);
}

/* ══════════════════════════════════════════════════════════════════════
 *  GPU Smart Flush — shadow-buffer comparison for minimal screen updates
 *
 *  Compares the current local framebuffer against a shadow copy of the
 *  previous frame.  Only changed scanline bands are GPU-blitted to the
 *  screen, dramatically reducing LFB write bandwidth.
 * ══════════════════════════════════════════════════════════════════════ */

static void gpu_smart_flush(void)
{
    uint32_t w = g_disp.width;
    uint32_t h = g_disp.height;
    uint32_t row_bytes = w * 4;
    uint32_t y = 0;

    while (y < h) {
        /* Skip unchanged rows (fast — memcmp bails on first byte) */
        while (y < h &&
               memcmp(g_disp.framebuf + y * w,
                      g_shadow + y * w, row_bytes) == 0)
            y++;
        if (y >= h) break;

        /* Start of a changed band — accumulate consecutive changed rows,
         * allowing small gaps (up to 8 unchanged rows) to merge bands
         * and avoid excessive per-band ioctl overhead. */
        uint32_t band_y = y;
        int bx1 = (int)w, bx2 = -1;
        uint32_t gap = 0;

        while (y < h && gap < 8) {
            uint32_t *cr = g_disp.framebuf + y * w;
            uint32_t *sr = g_shadow + y * w;
            if (memcmp(cr, sr, row_bytes) == 0) {
                gap++;
                y++;
                continue;
            }
            gap = 0;
            /* Tighten horizontal bounds */
            for (int x = 0; x < bx1; x++)
                if (cr[x] != sr[x]) { bx1 = x; break; }
            for (int x = (int)w - 1; x > bx2; x--)
                if (cr[x] != sr[x]) { bx2 = x; break; }
            y++;
        }

        /* Trim trailing unchanged rows from band */
        uint32_t band_end = y - gap;

        if (bx1 <= bx2 && band_end > band_y) {
            int bw = bx2 - bx1 + 1;
            int bh = (int)(band_end - band_y);

            /* GPU blit this band from local buffer to screen */
            lv_gpu_flush_area(bx1, (int)band_y, bw, bh);

            /* Update shadow buffer for the blitted region */
            for (uint32_t sy = band_y; sy < band_end; sy++)
                memcpy(g_shadow + sy * w + bx1,
                       g_disp.framebuf + sy * w + bx1,
                       (uint32_t)bw * 4);
        }
    }
}

void lv_refr_now(void)
{
    if (!g_initialized) return;

    uint32_t bg_pixel = color_to_pixel(g_screen.bg_color);

    if (g_dragging_gpu && g_partial_ok) {
        /* ═══ GPU wireframe drag path ═════════════════════════════ *
         * Window is hidden. The LFB has the background rendered.    *
         * We just erase the old ghost outline and draw a new one.   */
        int new_x = g_pressed_obj->x;
        int new_y = g_pressed_obj->y;
        int new_w = g_pressed_obj->w;
        int new_h = g_pressed_obj->h;
        int thick = 2;

        lv_color_t outline_color = lv_color_make(0, 120, 215);

        /* Erase old ghost (restore LFB from local framebuffer) */
        if (g_ghost_x != new_x || g_ghost_y != new_y) {
            gpu_erase_outline(g_ghost_x, g_ghost_y,
                              g_ghost_w, g_ghost_h, thick);
        }

        /* Draw new ghost outline directly on LFB */
        gpu_draw_outline(new_x, new_y, new_w, new_h, outline_color, thick);

        /* Update ghost position */
        g_ghost_x = new_x;
        g_ghost_y = new_y;

        /* Cursor: blit from local fb (cursor was drawn in last full render) */
        /* Just draw cursor on LFB area */
        {
            int cx = g_indev.x, cy = g_indev.y;
            int cw = 13, ch = 13;
            int cx2 = cx + cw, cy2 = cy + ch;
            if (clamp_rect(&cx, &cy, &cx2, &cy2))
                lv_gpu_flush_area(cx, cy, cx2 - cx, cy2 - cy);
        }

        dirty_reset();
        return;
    }

    /* ── Render scene to local buffer ─────────────────────────────── */
    uint32_t npixels = g_disp.width * g_disp.height;
    for (uint32_t i = 0; i < npixels; i++)
        g_disp.framebuf[i] = bg_pixel;

    for (int i = 0; i < g_screen.child_count; i++)
        render_obj(g_screen.children[i]);

    draw_cursor();

    /* ── Flush to screen ──────────────────────────────────────────
     * GPU smart flush: compare local buffer against shadow buffer,
     * find changed scanline bands, and blit only those regions.
     * Falls back to full-screen blit on first frame or if shadow
     * buffer is unavailable.
     * ────────────────────────────────────────────────────────────── */
    if (g_shadow && g_disp.gpu_accel && !g_force_full) {
        gpu_smart_flush();
    } else {
        /* Full-screen blit (first frame or no shadow) */
        disp_flush();
        if (g_shadow)
            memcpy(g_shadow, g_disp.framebuf, g_disp.fb_size);
        g_force_full = 0;
    }

    /* Reset dirty tracking for next frame */
    dirty_reset();
}
