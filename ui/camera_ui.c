#include "camera_ui.h"

#include <stdint.h>
#include <string.h>

static void button_event(lv_event_t *event) {
    camera_ui_t *ui = lv_event_get_user_data(event);
    intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
        return;

    if (action == 0) {
        camera_ui_set_running(ui, !ui->running);
        if (ui->callbacks.set_running)
            ui->callbacks.set_running(ui->callbacks.userdata, ui->running);
    } else if (action == 1) {
        camera_ui_set_audio_enabled(ui, !ui->audio_enabled);
        if (ui->callbacks.set_audio_enabled)
            ui->callbacks.set_audio_enabled(ui->callbacks.userdata,
                                            ui->audio_enabled);
    } else if (ui->callbacks.request_exit) {
        ui->callbacks.request_exit(ui->callbacks.userdata);
    }
}

static lv_obj_t *make_button(camera_ui_t *ui, lv_obj_t *parent,
                             const char *symbol, lv_color_t color,
                             intptr_t action, int size, int radius) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, radius, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xffa126),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, button_event, LV_EVENT_CLICKED, ui);
    return label;
}

void camera_ui_create(camera_ui_t *ui, const camera_ui_callbacks_t *callbacks,
                      bool running, bool audio_enabled) {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *accent;
    lv_obj_t *status;
    lv_obj_t *dot;
    memset(ui, 0, sizeof(*ui));
    ui->callbacks = *callbacks;
    ui->running = running;
    ui->audio_enabled = audio_enabled;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x202124), 0);
    lv_obj_set_style_pad_left(screen, 18, 0);
    lv_obj_set_style_pad_right(screen, 18, 0);
    lv_obj_set_style_pad_top(screen, 14, 0);
    lv_obj_set_style_pad_bottom(screen, 10, 0);
    lv_obj_set_style_pad_column(screen, 18, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    accent = lv_obj_create(screen);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 720, 3);
    lv_obj_set_pos(accent, -18, -14);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0xff8800), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_add_flag(accent, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE);

    status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_height(status, 72);
    lv_obj_set_flex_grow(status, 1);
    dot = lv_obj_create(status);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xff8800), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 4, 0);
    ui->status_label = lv_label_create(status);
    lv_label_set_text(ui->status_label, running ? "LIVE" : "PAUSED");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(ui->status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ui->status_label, LV_ALIGN_LEFT_MID, 24, 0);

    ui->play_label = make_button(ui, screen,
                                 running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                                 lv_color_hex(0xff8800), 0, 72,
                                 LV_RADIUS_CIRCLE);
    ui->audio_label = make_button(ui, screen,
                                  audio_enabled ? LV_SYMBOL_VOLUME_MID
                                                : LV_SYMBOL_MUTE,
                                  lv_color_hex(0x3c4043), 1, 58, 5);
    make_button(ui, screen, LV_SYMBOL_CLOSE, lv_color_hex(0x3c4043), 2, 58, 5);
}

void camera_ui_set_running(camera_ui_t *ui, bool running) {
    ui->running = running;
    lv_label_set_text(ui->play_label,
                      running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(ui->status_label, running ? "LIVE" : "PAUSED");
}

void camera_ui_set_audio_enabled(camera_ui_t *ui, bool enabled) {
    ui->audio_enabled = enabled;
    lv_label_set_text(ui->audio_label,
                      enabled ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE);
}
