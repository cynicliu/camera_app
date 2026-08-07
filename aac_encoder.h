#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct aac_encoder aac_encoder_t;
typedef void (*aac_packet_callback_t)(const uint8_t *data, size_t size,
                                      uint64_t pts_us, void *userdata);

int aac_encoder_create(aac_encoder_t **encoder, int sample_rate, int channels,
                       int bit_rate, aac_packet_callback_t callback,
                       void *userdata);
int aac_encoder_push_s16(aac_encoder_t *encoder, const int16_t *samples,
                         size_t samples_per_channel, uint64_t pts_us);
int aac_encoder_flush(aac_encoder_t *encoder);
void aac_encoder_destroy(aac_encoder_t *encoder);

#endif
