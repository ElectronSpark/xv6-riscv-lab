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

static volatile int g_running = 1;

/* ── Window tracking ─────────────────────────────────────────────── */

static lv_obj_t *win_sysinfo;
static lv_obj_t *win_calc;
static lv_obj_t *win_settings;
static lv_obj_t *win_about;

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
        usleep(16000);  /* ~60 fps */
    }

    lv_deinit();
    return 0;
}
