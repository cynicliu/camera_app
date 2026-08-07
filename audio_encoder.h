#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_ENCODER_AAC,
    AUDIO_ENCODER_MP3,
} audio_encoder_codec_t;

typedef struct audio_encoder audio_encoder_t;
typedef void (*audio_packet_callback_t)(const uint8_t *data, size_t size,
                                        uint64_t pts_us, void *userdata);

int audio_encoder_create(audio_encoder_t **encoder, audio_encoder_codec_t codec,
                         int sample_rate, int channels, int bit_rate,
                         audio_packet_callback_t callback, void *userdata);
int audio_encoder_push_s16(audio_encoder_t *encoder, const int16_t *samples,
                           size_t samples_per_channel, uint64_t pts_us);
int audio_encoder_flush(audio_encoder_t *encoder);
void audio_encoder_destroy(audio_encoder_t *encoder);
size_t audio_encoder_frame_samples(const audio_encoder_t *encoder);

#endif
