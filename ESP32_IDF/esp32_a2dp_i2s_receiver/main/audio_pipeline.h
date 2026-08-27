#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t received_packets;
    uint32_t received_bytes;
    uint32_t dropped_while_stopped;
    uint32_t bad_packets;
    uint32_t overruns;
    uint32_t underruns;
    uint32_t i2s_errors;
    size_t buffered_bytes;
} audio_pipeline_stats_t;

/*
 * Initialize Philips-I2S output and the Bluetooth-to-I2S buffering task.
 * This function must be called once before registering the A2DP PCM callback.
 */
esp_err_t audio_pipeline_init(void);

/*
 * Enable or disable acceptance and playback of A2DP PCM. Call with true for
 * ESP_A2D_AUDIO_STATE_STARTED and false for every other audio state.
 */
void audio_pipeline_set_streaming(bool streaming);

/*
 * Copy one decoded A2DP PCM packet into the nonblocking application buffer.
 * Expected format: signed 16-bit, stereo interleaved PCM.
 */
void audio_pipeline_push(const uint8_t *data, uint32_t length);

/* Obtain a consistent snapshot for low-priority diagnostics. */
void audio_pipeline_get_stats(audio_pipeline_stats_t *stats);

#ifdef __cplusplus
}
#endif
