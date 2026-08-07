#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp3_enc_types.h"

#include "aac_encoder.h"
#include "audio_encoder.h"

struct audio_encoder {
    audio_encoder_codec_t codec;
    aac_encoder_t *aac;
    mp3_enc *mp3;
    int16_t *mp3_samples;
    size_t mp3_sample_count;
    size_t frame_samples;
    uint64_t buffered_pts_us;
    audio_packet_callback_t callback;
    void *userdata;
};

static int encode_mp3_frame(audio_encoder_t *encoder, uint64_t pts_us) {
    unsigned char *output = NULL;
    int output_size;

    memcpy(encoder->mp3->config.in_buf, encoder->mp3_samples,
           encoder->frame_samples * sizeof(*encoder->mp3_samples));
    output_size = L3_compress(encoder->mp3, 0, &output);
    if (output_size <= 0 || !output) {
        fprintf(stderr, "Rockchip MP3 encoder failed\n");
        return -1;
    }
    encoder->callback(output, output_size, pts_us, encoder->userdata);
    return 0;
}

int audio_encoder_create(audio_encoder_t **result, audio_encoder_codec_t codec,
                         int sample_rate, int channels, int bit_rate,
                         audio_packet_callback_t callback, void *userdata) {
    audio_encoder_t *encoder;
    if (!result || !callback || channels != 1) return -1;
    encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return -1;
    encoder->codec = codec;
    encoder->callback = callback;
    encoder->userdata = userdata;

    if (codec == AUDIO_ENCODER_AAC) {
        if (aac_encoder_create(&encoder->aac, sample_rate, channels, bit_rate,
                               callback, userdata) != 0)
            goto fail;
        encoder->frame_samples = 1024;
    } else if (codec == AUDIO_ENCODER_MP3) {
        encoder->mp3 = Mp3EncodeVariableInit(sample_rate, channels,
                                              bit_rate / 1000);
        if (!encoder->mp3 || encoder->mp3->frame_size <= 0) {
            fprintf(stderr, "Rockchip MP3 encoder initialization failed\n");
            goto fail;
        }
        encoder->frame_samples = encoder->mp3->frame_size;
        encoder->mp3_samples = calloc(encoder->frame_samples,
                                      sizeof(*encoder->mp3_samples));
        if (!encoder->mp3_samples) goto fail;
    } else {
        goto fail;
    }
    *result = encoder;
    return 0;

fail:
    audio_encoder_destroy(encoder);
    return -1;
}

int audio_encoder_push_s16(audio_encoder_t *encoder, const int16_t *samples,
                           size_t sample_count, uint64_t pts_us) {
    size_t offset = 0;
    if (!encoder || !samples) return -1;
    if (encoder->codec == AUDIO_ENCODER_AAC)
        return aac_encoder_push_s16(encoder->aac, samples, sample_count, pts_us);

    while (offset < sample_count) {
        size_t available = encoder->frame_samples - encoder->mp3_sample_count;
        size_t count = sample_count - offset < available
            ? sample_count - offset : available;
        if (!encoder->mp3_sample_count) encoder->buffered_pts_us = pts_us;
        memcpy(encoder->mp3_samples + encoder->mp3_sample_count,
               samples + offset, count * sizeof(*samples));
        encoder->mp3_sample_count += count;
        offset += count;
        if (encoder->mp3_sample_count == encoder->frame_samples) {
            if (encode_mp3_frame(encoder, encoder->buffered_pts_us) != 0)
                return -1;
            encoder->mp3_sample_count = 0;
        }
    }
    return 0;
}

int audio_encoder_flush(audio_encoder_t *encoder) {
    if (!encoder) return 0;
    if (encoder->codec == AUDIO_ENCODER_AAC)
        return aac_encoder_flush(encoder->aac);
    if (encoder->mp3_sample_count) {
        memset(encoder->mp3_samples + encoder->mp3_sample_count, 0,
               (encoder->frame_samples - encoder->mp3_sample_count) *
               sizeof(*encoder->mp3_samples));
        if (encode_mp3_frame(encoder, encoder->buffered_pts_us) != 0)
            return -1;
        encoder->mp3_sample_count = 0;
    }
    return 0;
}

void audio_encoder_destroy(audio_encoder_t *encoder) {
    if (!encoder) return;
    if (encoder->aac) aac_encoder_destroy(encoder->aac);
    if (encoder->mp3) Mp3EncodeDeinit(encoder->mp3);
    free(encoder->mp3_samples);
    free(encoder);
}

size_t audio_encoder_frame_samples(const audio_encoder_t *encoder) {
    return encoder ? encoder->frame_samples : 0;
}
