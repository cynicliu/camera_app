#include <string.h>

#include "media_callbacks.h"

static void *dispatch_venc(void *userdata) {
    media_callbacks_t *ctx = userdata;
    VENC_PACK_S pack;
    VENC_STREAM_S stream;
    memset(&pack, 0, sizeof(pack));
    memset(&stream, 0, sizeof(stream));
    stream.pstPack = &pack;
    while (!ctx->stop) {
        if (RK_MPI_VENC_GetStream(ctx->venc_chn, &stream, 200) != RK_SUCCESS)
            continue;
        if (ctx->venc_callback)
            ctx->venc_callback(&stream, ctx->userdata);
        RK_MPI_VENC_ReleaseStream(ctx->venc_chn, &stream);
    }
    return NULL;
}

int media_callbacks_start(media_callbacks_t *ctx, VENC_CHN venc_chn,
                          media_venc_callback_t venc_callback,
                          void *userdata) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->venc_chn = venc_chn;
    ctx->venc_callback = venc_callback;
    ctx->userdata = userdata;
    if (pthread_create(&ctx->venc_thread, NULL, dispatch_venc, ctx) != 0)
        return -1;
    ctx->venc_started = true;
    return 0;
}

void media_callbacks_stop(media_callbacks_t *ctx) {
    ctx->stop = true;
    if (ctx->venc_started) pthread_join(ctx->venc_thread, NULL);
    ctx->venc_started = false;
}
