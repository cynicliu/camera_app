#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#include "aac_encoder.h"

#define PTS_QUEUE_SIZE 64

struct aac_encoder {
    AVCodecContext *context;
    AVFrame *frame;
    AVPacket *packet;
    float *samples;
    size_t sample_count;
    size_t frame_samples;
    uint64_t buffered_pts_us;
    uint64_t pts_queue[PTS_QUEUE_SIZE];
    unsigned pts_read;
    unsigned pts_write;
    int64_t sample_pts;
    int flushed;
    aac_packet_callback_t callback;
    void *userdata;
};

static void log_av_error(const char *operation, int error) {
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, message, sizeof(message));
    fprintf(stderr, "%s failed: %s\n", operation, message);
}

static int supports_fltp(const AVCodec *codec) {
    const enum AVSampleFormat *format = codec->sample_fmts;
    if (!format) return 0;
    while (*format != AV_SAMPLE_FMT_NONE) {
        if (*format++ == AV_SAMPLE_FMT_FLTP) return 1;
    }
    return 0;
}

static void enqueue_pts(aac_encoder_t *encoder, uint64_t pts_us) {
    unsigned next = (encoder->pts_write + 1) % PTS_QUEUE_SIZE;
    if (next == encoder->pts_read)
        encoder->pts_read = (encoder->pts_read + 1) % PTS_QUEUE_SIZE;
    encoder->pts_queue[encoder->pts_write] = pts_us;
    encoder->pts_write = next;
}

static uint64_t dequeue_pts(aac_encoder_t *encoder) {
    uint64_t pts_us = 0;
    if (encoder->pts_read != encoder->pts_write) {
        pts_us = encoder->pts_queue[encoder->pts_read];
        encoder->pts_read = (encoder->pts_read + 1) % PTS_QUEUE_SIZE;
    }
    return pts_us;
}

static int receive_packets(aac_encoder_t *encoder) {
    int ret;
    while ((ret = avcodec_receive_packet(encoder->context,
                                          encoder->packet)) == 0) {
        encoder->callback(encoder->packet->data, encoder->packet->size,
                          dequeue_pts(encoder), encoder->userdata);
        av_packet_unref(encoder->packet);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        log_av_error("avcodec_receive_packet", ret);
        return -1;
    }
    return 0;
}

static int encode_frame(aac_encoder_t *encoder, uint64_t pts_us) {
    int ret = av_frame_make_writable(encoder->frame);
    if (ret < 0) {
        log_av_error("av_frame_make_writable", ret);
        return -1;
    }
    memcpy(encoder->frame->data[0], encoder->samples,
           encoder->frame_samples * sizeof(float));
    encoder->frame->pts = encoder->sample_pts;
    encoder->sample_pts += encoder->frame_samples;
    ret = avcodec_send_frame(encoder->context, encoder->frame);
    if (ret < 0) {
        log_av_error("avcodec_send_frame", ret);
        return -1;
    }
    enqueue_pts(encoder, pts_us);
    return receive_packets(encoder);
}

int aac_encoder_create(aac_encoder_t **result, int sample_rate, int channels,
                       int bit_rate, aac_packet_callback_t callback,
                       void *userdata) {
    const AVCodec *codec;
    aac_encoder_t *encoder;
    int ret;

    if (!result || sample_rate <= 0 || channels != 1 || bit_rate <= 0 ||
        !callback)
        return -1;
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec || !supports_fltp(codec)) {
        fprintf(stderr, "FFmpeg AAC encoder with FLTP input is unavailable\n");
        return -1;
    }
    encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return -1;
    encoder->context = avcodec_alloc_context3(codec);
    encoder->frame = av_frame_alloc();
    encoder->packet = av_packet_alloc();
    if (!encoder->context || !encoder->frame || !encoder->packet) goto fail;

    encoder->context->bit_rate = bit_rate;
    encoder->context->sample_rate = sample_rate;
    encoder->context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    encoder->context->profile = FF_PROFILE_AAC_LOW;
    encoder->context->channel_layout = AV_CH_LAYOUT_MONO;
    encoder->context->channels = channels;
    encoder->context->time_base = (AVRational){1, sample_rate};
    encoder->context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = avcodec_open2(encoder->context, codec, NULL);
    if (ret < 0) {
        log_av_error("avcodec_open2(AAC)", ret);
        goto fail;
    }

    encoder->frame_samples = encoder->context->frame_size;
    if (!encoder->frame_samples) {
        fprintf(stderr, "FFmpeg AAC encoder returned an invalid frame size\n");
        goto fail;
    }
    encoder->samples = calloc(encoder->frame_samples, sizeof(float));
    if (!encoder->samples) goto fail;
    encoder->frame->nb_samples = encoder->frame_samples;
    encoder->frame->format = encoder->context->sample_fmt;
    encoder->frame->channel_layout = encoder->context->channel_layout;
    ret = av_frame_get_buffer(encoder->frame, 0);
    if (ret < 0) {
        log_av_error("av_frame_get_buffer", ret);
        goto fail;
    }
    encoder->callback = callback;
    encoder->userdata = userdata;
    *result = encoder;
    return 0;

fail:
    aac_encoder_destroy(encoder);
    return -1;
}

int aac_encoder_push_s16(aac_encoder_t *encoder, const int16_t *samples,
                         size_t sample_count, uint64_t pts_us) {
    size_t offset = 0;
    if (!encoder || !samples || encoder->flushed) return -1;
    while (offset < sample_count) {
        size_t available = encoder->frame_samples - encoder->sample_count;
        size_t count = sample_count - offset < available
            ? sample_count - offset : available;
        size_t i;
        if (encoder->sample_count == 0) encoder->buffered_pts_us = pts_us;
        for (i = 0; i < count; ++i)
            encoder->samples[encoder->sample_count + i] =
                samples[offset + i] / 32768.0f;
        encoder->sample_count += count;
        offset += count;
        if (encoder->sample_count == encoder->frame_samples) {
            if (encode_frame(encoder, encoder->buffered_pts_us) != 0) return -1;
            encoder->sample_count = 0;
        }
    }
    return 0;
}

int aac_encoder_flush(aac_encoder_t *encoder) {
    int ret;
    if (!encoder || encoder->flushed) return 0;
    if (encoder->sample_count) {
        memset(encoder->samples + encoder->sample_count, 0,
               (encoder->frame_samples - encoder->sample_count) * sizeof(float));
        if (encode_frame(encoder, encoder->buffered_pts_us) != 0) return -1;
        encoder->sample_count = 0;
    }
    ret = avcodec_send_frame(encoder->context, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        log_av_error("avcodec_send_frame(flush)", ret);
        return -1;
    }
    encoder->flushed = 1;
    return receive_packets(encoder);
}

void aac_encoder_destroy(aac_encoder_t *encoder) {
    if (!encoder) return;
    av_packet_free(&encoder->packet);
    av_frame_free(&encoder->frame);
    avcodec_free_context(&encoder->context);
    free(encoder->samples);
    free(encoder);
}
