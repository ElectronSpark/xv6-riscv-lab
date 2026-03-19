/*
 * lvgl_xv6.h — Minimal LVGL-style GUI framework for xv6.
 *
 * This is a self-contained, lightweight GUI toolkit inspired by LVGL
 * (Light and Versatile Graphics Library).  It provides:
 *   - Framebuffer display driver (via /dev/fb0)
 *   - Mouse input driver (via /dev/mouse)
 *   - Widget system: screen, label, button, container
 *   - Event/callback system
 *   - Basic drawing: rectangles, text, colors
 *
 * Designed to run on xv6 with the Bochs VGA framebuffer.
 */

#ifndef LVGL_XV6_H
#define LVGL_XV6_H

#include <stdint.h>
#include <stddef.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Color
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
    uint8_t alpha;
} lv_color_t;

static inline lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b)
{
    lv_color_t c = { .red = r, .green = g, .blue = b, .alpha = 255 };
    return c;
}

#define LV_COLOR_WHITE       lv_color_make(255, 255, 255)
#define LV_COLOR_BLACK       lv_color_make(0, 0, 0)
#define LV_COLOR_RED         lv_color_make(255, 0, 0)
#define LV_COLOR_GREEN       lv_color_make(0, 255, 0)
#define LV_COLOR_BLUE        lv_color_make(0, 0, 255)
#define LV_COLOR_GRAY        lv_color_make(128, 128, 128)
#define LV_COLOR_DARK_GRAY   lv_color_make(64, 64, 64)
#define LV_COLOR_LIGHT_GRAY  lv_color_make(192, 192, 192)
#define LV_COLOR_CYAN        lv_color_make(0, 200, 200)
#define LV_COLOR_YELLOW      lv_color_make(255, 255, 0)
#define LV_COLOR_ORANGE      lv_color_make(255, 165, 0)

/* ══════════════════════════════════════════════════════════════════════
 *  Geometry
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int16_t x;
    int16_t y;
} lv_point_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} lv_area_t;

/* ══════════════════════════════════════════════════════════════════════
 *  Display driver
 * ══════════════════════════════════════════════════════════════════════ */

/* Framebuffer info (queried from /dev/fb0 via ioctl) */
typedef struct {
    int         fb_fd;          /* file descriptor for /dev/fb0 */
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;          /* bytes per line */
    uint32_t    bpp;
    uint32_t   *framebuf;      /* local rendering buffer */
    uint32_t    fb_size;        /* total buffer size in bytes */
    int         gpu_accel;      /* non-zero if GPU ioctls available */
} lv_disp_t;

/* ══════════════════════════════════════════════════════════════════════
 *  GPU acceleration (ioctl commands for /dev/fb0)
 * ══════════════════════════════════════════════════════════════════════ */

#define FB_GPU_FILL_RECT     0x4610   /* fill rectangle with solid color */
#define FB_GPU_BLIT          0x4611   /* copy user buffer to screen rect */
#define FB_GPU_COPY_RECT     0x4612   /* screen-to-screen rectangle copy */

/* GPU fill: solid-color rectangle directly on framebuffer */
struct fb_gpu_fill {
    uint32_t x, y, w, h;
    uint32_t color;               /* ARGB8888 */
};

/* GPU blit: copy pixels from user buffer to screen rectangle */
struct fb_gpu_blit {
    uint32_t x, y, w, h;
    uint32_t src_pitch;           /* source row stride in bytes */
    uint64_t pixels;              /* user pointer to pixel data */
};

/* GPU copy: screen-to-screen rectangle copy */
struct fb_gpu_copy {
    uint32_t src_x, src_y;
    uint32_t dst_x, dst_y;
    uint32_t w, h;
};

/* ══════════════════════════════════════════════════════════════════════
 *  Input driver (mouse + keyboard)
 * ══════════════════════════════════════════════════════════════════════ */

/* Keyboard event (matches kernel struct kbd_event) */
typedef struct {
    uint8_t keycode;        /* ASCII code (0 if non-printable) */
    uint8_t scancode;       /* raw PS/2 scancode */
    uint8_t pressed;        /* 1 = press, 0 = release */
    uint8_t modifiers;      /* bit0=shift, bit1=ctrl, bit2=alt */
} lv_kbd_event_t;

#define LV_KBD_MOD_SHIFT  0x01
#define LV_KBD_MOD_CTRL   0x02
#define LV_KBD_MOD_ALT    0x04

/* Special (non-ASCII) key codes (0x80+) — matches kernel KBD_KEY_* */
#define LV_KEY_UP      0x80
#define LV_KEY_DOWN    0x81
#define LV_KEY_LEFT    0x82
#define LV_KEY_RIGHT   0x83
#define LV_KEY_HOME    0x84
#define LV_KEY_END     0x85
#define LV_KEY_PGUP    0x86
#define LV_KEY_PGDN    0x87
#define LV_KEY_INSERT  0x88
#define LV_KEY_DELETE  0x89

/* Keyboard callback: receives key events when a widget has focus */
typedef void (*lv_kbd_cb_t)(lv_kbd_event_t *ev, void *user_data);

typedef struct {
    int         mouse_fd;
    int16_t     x;              /* current absolute position */
    int16_t     y;
    uint8_t     buttons;        /* bit0=left, bit1=right */
    int         pressed;        /* left button currently pressed */
    int         kbd_fd;         /* file descriptor for /dev/kbd */
    lv_kbd_cb_t kbd_cb;         /* keyboard event callback */
    void       *kbd_cb_data;    /* user data for keyboard callback */
} lv_indev_t;

/* ══════════════════════════════════════════════════════════════════════
 *  Event system
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    LV_EVENT_CLICKED = 0,
    LV_EVENT_PRESSED,
    LV_EVENT_RELEASED,
    LV_EVENT_VALUE_CHANGED,
    LV_EVENT_CLOSE,
    LV_EVENT_MAX,
} lv_event_t;

struct lv_obj;
typedef void (*lv_event_cb_t)(struct lv_obj *obj, lv_event_t event);

/* ══════════════════════════════════════════════════════════════════════
 *  Object (widget) system
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    LV_OBJ_TYPE_BASE = 0,
    LV_OBJ_TYPE_LABEL,
    LV_OBJ_TYPE_BUTTON,
    LV_OBJ_TYPE_CONTAINER,
    LV_OBJ_TYPE_CHECKBOX,
    LV_OBJ_TYPE_SLIDER,
    LV_OBJ_TYPE_WINDOW,
    LV_OBJ_TYPE_TEXTBOX,
} lv_obj_type_t;

#define LV_OBJ_MAX_CHILDREN 32
#define LV_LABEL_MAX_TEXT   2048

typedef enum {
    LV_ALIGN_TOP_LEFT = 0,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_CENTER,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_RIGHT_MID,
} lv_align_t;

typedef struct lv_obj {
    lv_obj_type_t   type;
    struct lv_obj  *parent;
    struct lv_obj  *children[LV_OBJ_MAX_CHILDREN];
    int             child_count;

    /* Geometry (relative to parent) */
    int16_t         x;
    int16_t         y;
    int16_t         w;
    int16_t         h;

    /* Style */
    lv_color_t      bg_color;
    lv_color_t      border_color;
    lv_color_t      text_color;
    uint8_t         border_width;
    uint8_t         radius;
    uint8_t         padding;
    uint8_t         visible;
    uint8_t         scrollable;  /* 1 = scrollable container with scrollbar */
    int16_t         scroll_y;    /* vertical scroll offset (pixels) */

    /* Events */
    lv_event_cb_t   event_cb;
    void           *user_data;

    /* Type-specific data */
    union {
        struct {
            char    text[LV_LABEL_MAX_TEXT];
        } label;
        struct {
            char    text[64];
            uint8_t pressed;
        } btn;
        struct {
            char    text[64];
            uint8_t checked;
        } checkbox;
        struct {
            int16_t value;
            int16_t min_val;
            int16_t max_val;
        } slider;
        struct {
            char    title[64];
            uint8_t dragging;
            int16_t drag_ox;
            int16_t drag_oy;
        } win;
        struct {
            char    text[48];
            uint8_t cursor;
            uint8_t max_len;
            uint8_t focused;
        } textbox;
    } spec;
} lv_obj_t;

/* ══════════════════════════════════════════════════════════════════════
 *  API — Initialization
 * ══════════════════════════════════════════════════════════════════════ */

/* Initialize LVGL framework (opens /dev/fb0, /dev/mouse) */
int       lv_init(void);

/* Shut down and free resources */
void      lv_deinit(void);

/* Get display driver */
lv_disp_t *lv_disp_get(void);

/* Get input driver */
lv_indev_t *lv_indev_get(void);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Object system
 * ══════════════════════════════════════════════════════════════════════ */

/* Get the active screen (root object) */
lv_obj_t *lv_scr_act(void);

/* Create a base object (container) */
lv_obj_t *lv_obj_create(lv_obj_t *parent);

/* Clean (remove all children) */
void      lv_obj_clean(lv_obj_t *obj);

/* Delete an object and its children */
void      lv_obj_del(lv_obj_t *obj);

/* Move an object to the front (render last / on top) */
void      lv_obj_move_to_front(lv_obj_t *obj);

/* Position and size */
void      lv_obj_set_pos(lv_obj_t *obj, int16_t x, int16_t y);
void      lv_obj_set_size(lv_obj_t *obj, int16_t w, int16_t h);
void      lv_obj_set_width(lv_obj_t *obj, int16_t w);
void      lv_obj_set_height(lv_obj_t *obj, int16_t h);
void      lv_obj_align(lv_obj_t *obj, lv_align_t align, int16_t x_ofs, int16_t y_ofs);

/* Style */
void      lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color);
void      lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color);
void      lv_obj_set_style_border_width(lv_obj_t *obj, uint8_t width);
void      lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color);
void      lv_obj_set_style_radius(lv_obj_t *obj, uint8_t radius);
void      lv_obj_set_style_pad_all(lv_obj_t *obj, uint8_t pad);

/* Events */
void      lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_t event,
                               void *user_data);

/* Visibility */
void      lv_obj_set_hidden(lv_obj_t *obj, int hidden);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Label widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_label_create(lv_obj_t *parent);
void      lv_label_set_text(lv_obj_t *label, const char *text);
void      lv_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...);
const char *lv_label_get_text(lv_obj_t *label);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Button widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_btn_create(lv_obj_t *parent);
void      lv_btn_set_text(lv_obj_t *btn, const char *text);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Checkbox widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_checkbox_create(lv_obj_t *parent);
void      lv_checkbox_set_text(lv_obj_t *cb, const char *text);
int       lv_checkbox_is_checked(lv_obj_t *cb);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Slider widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_slider_create(lv_obj_t *parent);
void      lv_slider_set_range(lv_obj_t *slider, int16_t min_val, int16_t max_val);
void      lv_slider_set_value(lv_obj_t *slider, int16_t value);
int16_t   lv_slider_get_value(lv_obj_t *slider);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Window widget
 * ══════════════════════════════════════════════════════════════════════ */

#define LV_WIN_TITLE_H   28
#define LV_WIN_CLOSE_W   28

lv_obj_t *lv_win_create(lv_obj_t *parent);
void      lv_win_set_title(lv_obj_t *win, const char *title);
lv_obj_t *lv_win_get_content(lv_obj_t *win);
void      lv_win_close(lv_obj_t *win);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Textbox widget
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *lv_textbox_create(lv_obj_t *parent);
void      lv_textbox_set_text(lv_obj_t *tb, const char *text);
const char *lv_textbox_get_text(lv_obj_t *tb);
void      lv_textbox_set_max_length(lv_obj_t *tb, int max_len);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Scrollable containers
 * ══════════════════════════════════════════════════════════════════════ */

#define LV_SCROLLBAR_W  10

void      lv_obj_set_scrollable(lv_obj_t *obj, int enable);
void      lv_obj_scroll_by(lv_obj_t *obj, int16_t dy);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Drawing / rendering
 * ══════════════════════════════════════════════════════════════════════ */

/* Main loop tick — process input, redraw dirty widgets, flush display.
 * Call this in a loop. Returns non-zero if the user wants to quit. */
int       lv_timer_handler(void);

/* Get monotonic frame counter (incremented each lv_timer_handler call) */
uint32_t  lv_get_frame_count(void);

/* Force full screen redraw */
void      lv_refr_now(void);

/* Mark an object as needing a screen update */
void      lv_obj_invalidate(lv_obj_t *obj);

/* Force a full-screen blit on the next frame (e.g. after login/logout) */
void      lv_force_refresh(void);

/* ══════════════════════════════════════════════════════════════════════
 *  API — Font (built-in 8x16 bitmap font)
 * ══════════════════════════════════════════════════════════════════════ */

#define LV_FONT_WIDTH   8
#define LV_FONT_HEIGHT  16

/* Draw a single character at pixel position (x,y) */
void      lv_font_draw_char(uint32_t *buf, int buf_w, int buf_h,
                             int x, int y, char ch, lv_color_t color);

/* Draw a string at pixel position (x,y) */
void      lv_font_draw_string(uint32_t *buf, int buf_w, int buf_h,
                               int x, int y, const char *str, lv_color_t color);

/* Measure text dimensions */
void      lv_font_measure(const char *text, int *out_w, int *out_h);

/* ══════════════════════════════════════════════════════════════════════
 *  API — GPU acceleration
 * ══════════════════════════════════════════════════════════════════════ */

/* Check if GPU acceleration is available */
int       lv_gpu_available(void);

/* Register a keyboard event callback (called each frame for pending keys) */
void      lv_indev_set_kbd_callback(lv_kbd_cb_t cb, void *user_data);

/* Fill a screen rectangle with a solid color (bypasses local buffer) */
int       lv_gpu_fill_rect(int x, int y, int w, int h, lv_color_t color);

/* Blit pixels from a user buffer to a screen rectangle */
int       lv_gpu_blit(int x, int y, int w, int h,
                       const uint32_t *pixels, int src_pitch);

/* Copy a screen rectangle to another position */
int       lv_gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h);

/* Flush only a dirty region from the local framebuffer to the screen */
int       lv_gpu_flush_area(int x, int y, int w, int h);

#endif /* LVGL_XV6_H */
