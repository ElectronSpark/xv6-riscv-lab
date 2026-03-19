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

static volatile int g_running = 1;

/* ── Window tracking ─────────────────────────────────────────────── */

static lv_obj_t *win_sysinfo;
static lv_obj_t *win_calc;
static lv_obj_t *win_settings;
static lv_obj_t *win_about;

/* ── Terminal instances ──────────────────────────────────────────── */

#define TERM_COLS      80
#define TERM_ROWS      24
#define TERM_ESC_MAX   32
#define TERM_MAX       4       /* max simultaneous terminals */

typedef struct {
    int         active;            /* slot in use */
    lv_obj_t   *win;              /* window widget */
    lv_obj_t   *label;            /* text label */
    lv_obj_t   *tb_btn;           /* taskbar button for this terminal */
    int         master_fd;        /* PTY master fd */
    int         shell_pid;        /* child shell PID */
    char        buf[TERM_ROWS][TERM_COLS];
    int         row, col;
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

/* ── Forward declarations ────────────────────────────────────────── */

static void create_sysinfo(void);
static void create_calc(void);
static void create_settings(void);
static void create_about(void);
static void create_terminal(void);
static void term_poll_all(void);
static term_instance_t *term_find_by_win(lv_obj_t *win);
static void term_focus(int idx);
static void on_terminal_pressed(lv_obj_t *obj, lv_event_t e);
static void tb_relayout_app_btns(void);
static void tb_app_btn_cb(lv_obj_t *obj, lv_event_t e);
static void term_kbd_cb(lv_kbd_event_t *ev, void *user_data);

/* ══════════════════════════════════════════════════════════════════════
 *  Window close callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void on_sysinfo_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    lv_obj_del(obj);
    win_sysinfo = NULL;
}

static void on_calc_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
    lv_obj_del(obj);
    win_calc = NULL;
    calc_display_lbl = NULL;
}

static void on_settings_close(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_CLOSE) return;
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
    lv_obj_del(obj);
    win_about = NULL;
}

static void on_terminal_pressed(lv_obj_t *obj, lv_event_t e)
{
    if (e != LV_EVENT_PRESSED) return;
    term_instance_t *t = term_find_by_win(obj);
    if (!t) return;
    int idx = (int)(t - g_terms);
    if (g_term_focus != idx)
        term_focus(idx);
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
        waitpid(t->shell_pid, NULL, 0);
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

static void btn_quit_cb(lv_obj_t *obj, lv_event_t e)
{
    (void)obj;
    if (e != LV_EVENT_CLICKED) return;
    g_running = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  System Info Window
 * ══════════════════════════════════════════════════════════════════════ */

static void create_sysinfo(void)
{
    lv_disp_t *d = lv_disp_get();

    win_sysinfo = lv_win_create(lv_scr_act());
    lv_win_set_title(win_sysinfo, "System Information");
    lv_obj_set_pos(win_sysinfo, 80, 40);
    lv_obj_set_size(win_sysinfo, 340, 320);
    lv_obj_add_event_cb(win_sysinfo, on_sysinfo_close, LV_EVENT_CLOSE, NULL);

    lv_obj_t *c = lv_win_get_content(win_sysinfo);
    int y = 0;
    lv_obj_t *l;

    l = lv_label_create(c);
    lv_label_set_text(l, "Operating System");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 0, y); y += 22;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Name:      xv6");
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Arch:      x86_64");
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text_fmt(l, "  Display:   %ux%ux%u",
                          d->width, d->height, d->bpp);
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Libc:      musl");
    lv_obj_set_pos(l, 0, y); y += 26;

    l = lv_label_create(c);
    lv_label_set_text(l, "Hardware");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 0, y); y += 22;

    l = lv_label_create(c);
    lv_label_set_text(l, "  CPU:       QEMU x86_64");
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Memory:    4 GB");
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Video:     Bochs VGA");
    lv_obj_set_pos(l, 0, y); y += 18;

    l = lv_label_create(c);
    lv_label_set_text(l, "  Input:     PS/2 Mouse");
    lv_obj_set_pos(l, 0, y); y += 26;

    l = lv_label_create(c);
    lv_label_set_text(l, "GUI Framework");
    lv_obj_set_style_text_color(l, lv_color_make(0, 160, 255));
    lv_obj_set_pos(l, 0, y); y += 22;

    l = lv_label_create(c);
    lv_label_set_text(l, "  LVGL on xv6 v1.0");
    lv_obj_set_pos(l, 0, y);
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

static void create_settings(void)
{
    win_settings = lv_win_create(lv_scr_act());
    lv_win_set_title(win_settings, "Settings");
    lv_obj_set_pos(win_settings, 200, 100);
    lv_obj_set_size(win_settings, 360, 300);
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
    for (int r = 0; r < TERM_ROWS; r++)
        memset(t->buf[r], ' ', TERM_COLS);
    t->row = t->col = 0;
    t->dirty = 1;
}

static void ti_scroll_up(term_instance_t *t)
{
    for (int r = 0; r < TERM_ROWS - 1; r++)
        memcpy(t->buf[r], t->buf[r + 1], TERM_COLS);
    memset(t->buf[TERM_ROWS - 1], ' ', TERM_COLS);
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
        }
        break;
    default:
        if ((unsigned char)c >= 32) {
            t->buf[t->row][t->col] = c;
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
              for (int r = t->row + 1; r < TERM_ROWS; r++)
                  memset(t->buf[r], ' ', TERM_COLS);
          } else if (mode == 1) {
              for (int r = 0; r < t->row; r++)
                  memset(t->buf[r], ' ', TERM_COLS);
              memset(t->buf[t->row], ' ', t->col + 1);
          }
        } break;
    case 'K':
        { int mode = (nparam > 0) ? params[0] : 0;
          if (mode == 0)
              memset(&t->buf[t->row][t->col], ' ', TERM_COLS - t->col);
          else if (mode == 1)
              memset(t->buf[t->row], ' ', t->col + 1);
          else if (mode == 2)
              memset(t->buf[t->row], ' ', TERM_COLS);
        } break;
    case 'm':
        break;
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
          if (rem > 0)
              memmove(&t->buf[t->row][t->col],
                      &t->buf[t->row][t->col + n], rem);
          memset(&t->buf[t->row][TERM_COLS - n], ' ', n);
        } break;
    case '@':
        { int n = (nparam > 0 && params[0] > 0) ? params[0] : 1;
          int rem = TERM_COLS - t->col - n;
          if (rem > 0)
              memmove(&t->buf[t->row][t->col + n],
                      &t->buf[t->row][t->col], rem);
          memset(&t->buf[t->row][t->col], ' ', n);
        } break;
    default:
        break;
    }
    t->dirty = 1;
}

static void ti_feed(term_instance_t *t, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char c = data[i];

        if (t->esc_state == 1) {
            if (c == '[') {
                t->esc_state = 2;
                t->esc_idx = 0;
            } else {
                t->esc_state = 0;
            }
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

static void ti_refresh_label(term_instance_t *t)
{
    if (!t->label || !t->dirty) return;
    t->dirty = 0;

    /* Temporarily insert cursor character */
    int cr = t->row, cc = t->col;
    if (cr >= TERM_ROWS) cr = TERM_ROWS - 1;
    if (cc >= TERM_COLS) cc = TERM_COLS - 1;
    char saved = t->buf[cr][cc];
    int is_focused = (g_term_focus >= 0 && &g_terms[g_term_focus] == t);
    if (is_focused)
        t->buf[cr][cc] = '_';

    char display[TERM_ROWS * (TERM_COLS + 1) + 1];
    int pos = 0;
    for (int r = 0; r < TERM_ROWS; r++) {
        int len = TERM_COLS;
        while (len > 0 && t->buf[r][len - 1] == ' ') len--;
        memcpy(&display[pos], t->buf[r], len);
        pos += len;
        if (r < TERM_ROWS - 1)
            display[pos++] = '\n';
    }
    display[pos] = '\0';
    lv_label_set_text(t->label, display);

    /* Restore original character */
    t->buf[cr][cc] = saved;
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
    for (int i = 0; i < TERM_MAX; i++) {
        if (!g_terms[i].active || !g_terms[i].tb_btn) continue;
        lv_obj_set_pos(g_terms[i].tb_btn, x, 0);
        x += g_tb_app_btn_w + 4;
    }
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

    write(t->master_fd, &c, 1);
}

static void term_poll_all(void)
{
    for (int i = 0; i < TERM_MAX; i++) {
        term_instance_t *t = &g_terms[i];
        if (!t->active || t->master_fd < 0) continue;

        char buf[512];
        for (;;) {
            int n = read(t->master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            ti_feed(t, buf, n);
        }
        ti_refresh_label(t);
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
    lv_obj_add_event_cb(t->win, on_terminal_close, LV_EVENT_CLOSE, NULL);
    lv_obj_add_event_cb(t->win, on_terminal_pressed, LV_EVENT_PRESSED, NULL);

    lv_obj_t *c = lv_win_get_content(t->win);
    lv_obj_set_style_bg_color(c, lv_color_make(0, 0, 0));

    t->label = lv_label_create(c);
    lv_obj_set_pos(t->label, 0, 0);
    lv_obj_set_style_text_color(t->label, lv_color_make(192, 192, 192));
    lv_label_set_text(t->label, "");

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

int main(void)
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    if (lv_init() < 0)
        return 1;

    lv_obj_t *scr = lv_scr_act();
    lv_disp_t *d = lv_disp_get();
    int16_t scr_w = (int16_t)d->width;
    int16_t scr_h = (int16_t)d->height;

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

    /* Taskbar buttons */
    int16_t bx = 4;
    lv_obj_t *b;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "Info");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 60, 28);
    lv_obj_add_event_cb(b, btn_sysinfo_cb, LV_EVENT_CLICKED, NULL);
    bx += 66;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "Calc");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 60, 28);
    lv_obj_add_event_cb(b, btn_calc_cb, LV_EVENT_CLICKED, NULL);
    bx += 66;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "Config");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 70, 28);
    lv_obj_add_event_cb(b, btn_settings_cb, LV_EVENT_CLICKED, NULL);
    bx += 76;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "About");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 65, 28);
    lv_obj_add_event_cb(b, btn_about_cb, LV_EVENT_CLICKED, NULL);
    bx += 71;

    b = lv_btn_create(taskbar);
    lv_btn_set_text(b, "Term");
    lv_obj_set_pos(b, bx, 0);
    lv_obj_set_size(b, 60, 28);
    lv_obj_set_style_bg_color(b, lv_color_make(40, 100, 50));
    lv_obj_add_event_cb(b, btn_terminal_cb, LV_EVENT_CLICKED, NULL);
    bx += 66;

    /* Separator: open-app buttons start here */
    g_tb_app_x0 = (int16_t)(bx + 10);

    /* Quit button (red, right side) */
    lv_obj_t *quit = lv_btn_create(taskbar);
    lv_btn_set_text(quit, "Quit");
    lv_obj_set_pos(quit, (int16_t)(scr_w - 70), 0);
    lv_obj_set_size(quit, 60, 28);
    lv_obj_set_style_bg_color(quit, lv_color_make(180, 40, 40));
    lv_obj_add_event_cb(quit, btn_quit_cb, LV_EVENT_CLICKED, NULL);

    /* Taskbar title */
    lv_obj_t *tb_title = lv_label_create(taskbar);
    lv_label_set_text(tb_title, "xv6 Desktop v1.0");
    lv_obj_set_style_text_color(tb_title, lv_color_make(140, 140, 150));
    lv_obj_set_pos(tb_title, (int16_t)(scr_w / 2 - 68), 6);

    /* ── Main loop ─────────────────────────────────────────────── */

    while (g_running) {
        lv_timer_handler();
        term_poll_all();
        usleep(16000);  /* ~60 fps */
    }

    lv_deinit();
    return 0;
}
