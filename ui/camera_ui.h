#ifndef CAMERA_UI_H
#define CAMERA_UI_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

typedef struct {
    void (*set_running)(void *userdata, bool running);
    void (*set_audio_enabled)(void *userdata, bool enabled);
    void (*request_exit)(void *userdata);
    void *userdata;
} camera_ui_callbacks_t;

typedef struct {
    camera_ui_callbacks_t callbacks;
    lv_obj_t *play_label;
    lv_obj_t *audio_label;
    lv_obj_t *status_label;
    bool running;
    bool audio_enabled;
} camera_ui_t;

void camera_ui_create(camera_ui_t *ui, const camera_ui_callbacks_t *callbacks,
                      bool running, bool audio_enabled);
void camera_ui_set_running(camera_ui_t *ui, bool running);
void camera_ui_set_audio_enabled(camera_ui_t *ui, bool enabled);

#endif
