/*
 * lvgl_demo.c — LVGL Demo Application for xv6
 *
 * Demonstrates: labels, buttons with click events, checkbox, slider,
 * styled containers, and mouse interaction.
 *
 * Usage:  $ lvgl_demo
 * Exit:   Ctrl+C (or press the "Quit" button)
 */

#include "../lvgl_xv6.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int g_running = 1;
static lv_obj_t *counter_label;
static int click_count = 0;
static lv_obj_t *slider_val_label;
static lv_obj_t *status_label;

static void sighandler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── Event callbacks ─────────────────────────────────────────────── */

static void click_btn_cb(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    click_count++;
    lv_label_set_text_fmt(counter_label, "Clicks: %d", click_count);
}

static void reset_btn_cb(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    click_count = 0;
    lv_label_set_text(counter_label, "Clicks: 0");
}

static void quit_btn_cb(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    g_running = 0;
}

static void slider_cb(lv_obj_t *obj, lv_event_t event)
{
    if (event != LV_EVENT_VALUE_CHANGED) return;
    int16_t val = lv_slider_get_value(obj);
    lv_label_set_text_fmt(slider_val_label, "Slider value: %d%%", val);
}

static void checkbox_cb(lv_obj_t *obj, lv_event_t event)
{
    if (event != LV_EVENT_CLICKED) return;
    int checked = lv_checkbox_is_checked(obj);
    lv_label_set_text(status_label, checked ? "Option enabled" : "Option disabled");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void)
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    if (lv_init() < 0) {
        fprintf(stderr, "lvgl_demo: lv_init() failed\n");
        return 1;
    }

    lv_obj_t *scr = lv_scr_act();

    /* == Title bar ================================================ */
    lv_obj_t *titlebar = lv_obj_create(scr);
    lv_obj_set_pos(titlebar, 0, 0);
    lv_obj_set_size(titlebar, (int16_t)lv_disp_get()->width, 40);
    lv_obj_set_style_bg_color(titlebar, lv_color_make(30, 30, 33));
    lv_obj_set_style_border_width(titlebar, 0);

    lv_obj_t *title_lbl = lv_label_create(titlebar);
    lv_label_set_text(title_lbl, "LVGL Demo — xv6 GUI Framework");
    lv_obj_set_style_text_color(title_lbl, lv_color_make(200, 200, 200));
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    /* == Panel 1: Click counter =================================== */
    lv_obj_t *panel1 = lv_obj_create(scr);
    lv_obj_set_pos(panel1, 30, 60);
    lv_obj_set_size(panel1, 300, 200);
    lv_obj_set_style_bg_color(panel1, lv_color_make(50, 50, 55));
    lv_obj_set_style_border_width(panel1, 1);
    lv_obj_set_style_border_color(panel1, lv_color_make(80, 80, 90));
    lv_obj_set_style_pad_all(panel1, 12);

    lv_obj_t *p1_title = lv_label_create(panel1);
    lv_label_set_text(p1_title, "Click Counter");
    lv_obj_set_style_text_color(p1_title, lv_color_make(0, 160, 255));
    lv_obj_set_pos(p1_title, 0, 0);

    counter_label = lv_label_create(panel1);
    lv_label_set_text(counter_label, "Clicks: 0");
    lv_obj_set_pos(counter_label, 0, 30);

    lv_obj_t *click_btn = lv_btn_create(panel1);
    lv_btn_set_text(click_btn, "Click Me!");
    lv_obj_set_pos(click_btn, 0, 60);
    lv_obj_set_size(click_btn, 130, 36);
    lv_obj_add_event_cb(click_btn, click_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_btn = lv_btn_create(panel1);
    lv_btn_set_text(reset_btn, "Reset");
    lv_obj_set_pos(reset_btn, 145, 60);
    lv_obj_set_size(reset_btn, 100, 36);
    lv_obj_set_style_bg_color(reset_btn, lv_color_make(100, 100, 110));
    lv_obj_add_event_cb(reset_btn, reset_btn_cb, LV_EVENT_CLICKED, NULL);

    /* == Panel 2: Slider ========================================== */
    lv_obj_t *panel2 = lv_obj_create(scr);
    lv_obj_set_pos(panel2, 30, 280);
    lv_obj_set_size(panel2, 300, 160);
    lv_obj_set_style_bg_color(panel2, lv_color_make(50, 50, 55));
    lv_obj_set_style_border_width(panel2, 1);
    lv_obj_set_style_border_color(panel2, lv_color_make(80, 80, 90));
    lv_obj_set_style_pad_all(panel2, 12);

    lv_obj_t *p2_title = lv_label_create(panel2);
    lv_label_set_text(p2_title, "Slider Control");
    lv_obj_set_style_text_color(p2_title, lv_color_make(0, 160, 255));
    lv_obj_set_pos(p2_title, 0, 0);

    slider_val_label = lv_label_create(panel2);
    lv_label_set_text(slider_val_label, "Slider value: 50%");
    lv_obj_set_pos(slider_val_label, 0, 30);

    lv_obj_t *slider = lv_slider_create(panel2);
    lv_obj_set_pos(slider, 0, 65);
    lv_obj_set_size(slider, 260, 30);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* == Panel 3: Settings ======================================== */
    lv_obj_t *panel3 = lv_obj_create(scr);
    lv_obj_set_pos(panel3, 360, 60);
    lv_obj_set_size(panel3, 300, 200);
    lv_obj_set_style_bg_color(panel3, lv_color_make(50, 50, 55));
    lv_obj_set_style_border_width(panel3, 1);
    lv_obj_set_style_border_color(panel3, lv_color_make(80, 80, 90));
    lv_obj_set_style_pad_all(panel3, 12);

    lv_obj_t *p3_title = lv_label_create(panel3);
    lv_label_set_text(p3_title, "Settings");
    lv_obj_set_style_text_color(p3_title, lv_color_make(0, 160, 255));
    lv_obj_set_pos(p3_title, 0, 0);

    lv_obj_t *cb1 = lv_checkbox_create(panel3);
    lv_checkbox_set_text(cb1, "Enable option");
    lv_obj_set_pos(cb1, 0, 35);
    lv_obj_add_event_cb(cb1, checkbox_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cb2 = lv_checkbox_create(panel3);
    lv_checkbox_set_text(cb2, "Verbose mode");
    lv_obj_set_pos(cb2, 0, 65);

    lv_obj_t *cb3 = lv_checkbox_create(panel3);
    lv_checkbox_set_text(cb3, "Auto refresh");
    lv_obj_set_pos(cb3, 0, 95);

    status_label = lv_label_create(panel3);
    lv_label_set_text(status_label, "Option disabled");
    lv_obj_set_style_text_color(status_label, LV_COLOR_LIGHT_GRAY);
    lv_obj_set_pos(status_label, 0, 135);

    /* == Quit button ============================================== */
    lv_obj_t *quit_btn = lv_btn_create(scr);
    lv_btn_set_text(quit_btn, "Quit");
    lv_obj_set_pos(quit_btn, 360, 280);
    lv_obj_set_size(quit_btn, 100, 36);
    lv_obj_set_style_bg_color(quit_btn, lv_color_make(200, 50, 50));
    lv_obj_add_event_cb(quit_btn, quit_btn_cb, LV_EVENT_CLICKED, NULL);

    /* == Status bar =============================================== */
    lv_obj_t *statusbar = lv_obj_create(scr);
    lv_obj_set_pos(statusbar, 0,
                   (int16_t)(lv_disp_get()->height - 28));
    lv_obj_set_size(statusbar, (int16_t)lv_disp_get()->width, 28);
    lv_obj_set_style_bg_color(statusbar, lv_color_make(0, 120, 215));
    lv_obj_set_style_border_width(statusbar, 0);

    lv_obj_t *sb_label = lv_label_create(statusbar);
    lv_label_set_text(sb_label,
        "LVGL on xv6  |  Use mouse to interact  |  Ctrl+C to exit");
    lv_obj_set_style_text_color(sb_label, LV_COLOR_WHITE);
    lv_obj_align(sb_label, LV_ALIGN_LEFT_MID, 8, 0);

    /* == Main loop ================================================ */
    fprintf(stderr, "lvgl_demo: running (Ctrl+C to quit)\n");

    while (g_running) {
        lv_timer_handler();
        usleep(16000);  /* ~60 fps */
    }

    fprintf(stderr, "lvgl_demo: exiting\n");
    lv_deinit();
    return 0;
}
