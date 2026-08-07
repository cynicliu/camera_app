#ifndef MEDIA_CALLBACKS_H
#define MEDIA_CALLBACKS_H

#include <stdbool.h>
#include <pthread.h>

#include "rk_mpi_venc.h"

typedef void (*media_venc_callback_t)(const VENC_STREAM_S *stream, void *userdata);

typedef struct {
    VENC_CHN venc_chn;
    media_venc_callback_t venc_callback;
    void *userdata;
    pthread_t venc_thread;
    volatile bool stop;
    bool venc_started;
} media_callbacks_t;

int media_callbacks_start(media_callbacks_t *ctx, VENC_CHN venc_chn,
                          media_venc_callback_t venc_callback,
                          void *userdata);
void media_callbacks_stop(media_callbacks_t *ctx);

#endif
