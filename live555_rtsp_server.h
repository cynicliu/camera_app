#ifndef LIVE555_RTSP_SERVER_H
#define LIVE555_RTSP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct live555_rtsp_server live555_rtsp_server_t;
typedef enum {
    LIVE555_VIDEO_H264,
    LIVE555_VIDEO_H265,
} live555_video_codec_t;
typedef enum {
    LIVE555_AUDIO_AAC,
    LIVE555_AUDIO_MP3,
} live555_audio_codec_t;

int live555_rtsp_server_start(live555_rtsp_server_t **server, int port,
                              const char *path, live555_video_codec_t video_codec,
                              live555_audio_codec_t audio_codec,
                              unsigned audio_sample_rate,
                              unsigned audio_channels,
                              const char *audio_config);
void live555_rtsp_server_stop(live555_rtsp_server_t *server);
int live555_rtsp_server_push_video(live555_rtsp_server_t *server,
                                   const void *data, size_t size,
                                   uint64_t pts_us);
int live555_rtsp_server_push_audio(live555_rtsp_server_t *server,
                                   const void *data, size_t size,
                                   uint64_t pts_us);

#ifdef __cplusplus
}
#endif

#endif
