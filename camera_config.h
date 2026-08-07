#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include <stdbool.h>

#define CAMERA_CONFIG_PATH_MAX 256
#define CAMERA_CONFIG_URL_MAX 512
#define CAMERA_CONFIG_RTSP_PATH_MAX 128

typedef struct {
    char iq_dir[CAMERA_CONFIG_PATH_MAX];
    char rtmp_url[CAMERA_CONFIG_URL_MAX];
    bool rtsp_enabled;
    int rtsp_port;
    char rtsp_path[CAMERA_CONFIG_RTSP_PATH_MAX];
    char frame_source[8];
    char video_codec[8];
    char audio_codec[8];
    bool lvgl;
} camera_config_t;

void camera_config_defaults(camera_config_t *config);
int camera_config_load(camera_config_t *config, const char *path);

#endif
