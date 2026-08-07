#ifndef ONVIF_SERVER_H
#define ONVIF_SERVER_H

#include <stdbool.h>

typedef struct onvif_server onvif_server_t;

typedef struct {
    int http_port;
    int rtsp_port;
    const char *rtsp_path;
    const char *device_name;
    const char *device_scope;
    const char *interface_name;
    const char *username;
    const char *password;
    const char *video_codec;
    const char *audio_codec;
    int width;
    int height;
    int frame_rate;
    int video_bitrate_kbps;
    int audio_sample_rate;
    int audio_bitrate;
} onvif_server_config_t;

int onvif_server_start(onvif_server_t **result,
                       const onvif_server_config_t *config);
void onvif_server_stop(onvif_server_t *server);

#endif
