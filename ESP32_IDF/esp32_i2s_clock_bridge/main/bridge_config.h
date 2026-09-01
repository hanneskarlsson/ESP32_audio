#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#define BRIDGE_SAMPLE_RATE_HZ        48000U
#define BRIDGE_CHANNEL_COUNT             2U
#define BRIDGE_SAMPLE_BITS              16U
#define BRIDGE_INPUT_SLOT_BITS           24U
#define BRIDGE_OUTPUT_SLOT_BITS          32U
#define BRIDGE_OUTPUT_MCLK_HZ      12288000U

#define BRIDGE_FIFO_CAPACITY_FRAMES   16384U
#define BRIDGE_FIFO_TARGET_FRAMES      4096U
#define BRIDGE_FIFO_PREFILL_FRAMES     4096U
#define BRIDGE_MAX_CORRECTION_PPM       1500

/* TinySine/simulator input: ESP32 I2S0 slave receiver. */
#define BRIDGE_RX_BCLK_GPIO       GPIO_NUM_26
#define BRIDGE_RX_WS_GPIO         GPIO_NUM_25
#define BRIDGE_RX_DATA_GPIO       GPIO_NUM_33

/* ADAU1701 output: ESP32 I2S1 master transmitter. */
#define BRIDGE_TX_MCLK_GPIO        GPIO_NUM_1
#define BRIDGE_TX_BCLK_GPIO       GPIO_NUM_14
#define BRIDGE_TX_WS_GPIO         GPIO_NUM_15
#define BRIDGE_TX_DATA_GPIO       GPIO_NUM_22

typedef struct {
    uint64_t input_frames;
    uint64_t output_frames;
    uint32_t rx_dma_errors;
    uint32_t tx_dma_errors;
    uint32_t fifo_underruns;
    uint32_t fifo_overruns;
    uint32_t stream_restarts;
    int32_t correction_ppm;
    size_t fifo_fill_frames;
    size_t fifo_capacity_frames;
} bridge_stats_t;

void bridge_log_configuration(void);
