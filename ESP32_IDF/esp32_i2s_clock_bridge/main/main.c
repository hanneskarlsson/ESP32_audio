#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bridge_config.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FRAMES_PER_BLOCK                240U
#define CHANNELS_PER_FRAME              BRIDGE_CHANNEL_COUNT
#define PCM_BYTES_PER_FRAME             (CHANNELS_PER_FRAME * sizeof(int16_t))
#define RX_DMA_BYTES_PER_FRAME          (CHANNELS_PER_FRAME * sizeof(int32_t))
#define TX_DMA_BYTES_PER_FRAME          (CHANNELS_PER_FRAME * sizeof(int32_t))
#define RX_TIMEOUT_MS                   1000U
#define TX_TIMEOUT_MS                   1000U
#define REPORT_INTERVAL_MS              1000U
#define TELEMETRY_UART                  UART_NUM_2
#define TELEMETRY_TX_GPIO               GPIO_NUM_21
#define TELEMETRY_BAUD_RATE             115200
#define TELEMETRY_LINE_MAX              384U
#define Q32_ONE                         (UINT64_C(1) << 32)
#define FIFO_INDEX_MASK                 (BRIDGE_FIFO_CAPACITY_FRAMES - 1U)

_Static_assert(
    (BRIDGE_FIFO_CAPACITY_FRAMES & FIFO_INDEX_MASK) == 0U,
    "FIFO capacity must be a power of two"
);
_Static_assert(
    BRIDGE_OUTPUT_MCLK_HZ == BRIDGE_SAMPLE_RATE_HZ * 256U,
    "ADAU1701 MCLK must be 256 times the sample rate"
);

typedef struct {
    int16_t left;
    int16_t right;
} stereo_frame_t;

static const char *TAG = "I2S_BRIDGE";

static i2s_chan_handle_t s_rx_channel;
static i2s_chan_handle_t s_tx_channel;

static int32_t s_rx_dma_samples[FRAMES_PER_BLOCK * CHANNELS_PER_FRAME];
static stereo_frame_t s_rx_pcm_frames[FRAMES_PER_BLOCK];
static int32_t s_tx_dma_samples[FRAMES_PER_BLOCK * CHANNELS_PER_FRAME];

/* One producer (RX task), one consumer (TX task): lock-free SPSC FIFO. */
static stereo_frame_t s_fifo[BRIDGE_FIFO_CAPACITY_FRAMES];
static uint32_t s_fifo_read_index;
static uint32_t s_fifo_write_index;

static uint32_t s_input_frames;
static uint32_t s_output_frames;
static uint32_t s_rx_errors;
static uint32_t s_rx_timeouts;
static uint32_t s_rx_short_reads;
static uint32_t s_tx_errors;
static uint32_t s_tx_short_writes;
static uint32_t s_fifo_underruns;
static uint32_t s_fifo_overruns;
static uint32_t s_dropped_input_frames;
static uint32_t s_stream_restarts;
static int32_t s_correction_ppm;
static uint32_t s_min_fill_seen;
static uint32_t s_max_fill_seen;
static bool s_tx_started;

static esp_err_t init_telemetry_uart(void)
{
    uart_config_t config = {
        .baud_rate = TELEMETRY_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        uart_driver_install(TELEMETRY_UART, 256, 2048, 0, NULL, 0),
        TAG,
        "could not install telemetry UART"
    );
    ESP_RETURN_ON_ERROR(
        uart_param_config(TELEMETRY_UART, &config),
        TAG,
        "could not configure telemetry UART"
    );
    return uart_set_pin(
        TELEMETRY_UART,
        TELEMETRY_TX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );
}

static uint32_t counter_load(const uint32_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

static void counter_add(uint32_t *counter, uint32_t value)
{
    __atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static uint32_t fifo_fill_frames(void)
{
    uint32_t read_index = __atomic_load_n(&s_fifo_read_index, __ATOMIC_ACQUIRE);
    uint32_t write_index = __atomic_load_n(&s_fifo_write_index, __ATOMIC_ACQUIRE);
    return (write_index - read_index) & FIFO_INDEX_MASK;
}

static bool fifo_push_block(const stereo_frame_t *frames, uint32_t frame_count)
{
    uint32_t write_index = __atomic_load_n(&s_fifo_write_index, __ATOMIC_RELAXED);
    uint32_t read_index = __atomic_load_n(&s_fifo_read_index, __ATOMIC_ACQUIRE);
    uint32_t fill = (write_index - read_index) & FIFO_INDEX_MASK;
    uint32_t free_frames = FIFO_INDEX_MASK - fill;

    if (frame_count > free_frames) {
        counter_add(&s_fifo_overruns, 1U);
        counter_add(&s_dropped_input_frames, frame_count);
        return false;
    }

    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        s_fifo[write_index] = frames[frame];
        write_index = (write_index + 1U) & FIFO_INDEX_MASK;
    }
    __atomic_store_n(&s_fifo_write_index, write_index, __ATOMIC_RELEASE);
    return true;
}

static bool fifo_pop(stereo_frame_t *frame)
{
    uint32_t read_index = __atomic_load_n(&s_fifo_read_index, __ATOMIC_RELAXED);
    uint32_t write_index = __atomic_load_n(&s_fifo_write_index, __ATOMIC_ACQUIRE);

    if (read_index == write_index) {
        return false;
    }

    *frame = s_fifo[read_index];
    read_index = (read_index + 1U) & FIFO_INDEX_MASK;
    __atomic_store_n(&s_fifo_read_index, read_index, __ATOMIC_RELEASE);
    return true;
}

static esp_err_t init_i2s_receiver(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = FRAMES_PER_BLOCK;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, NULL, &s_rx_channel),
        TAG,
        "could not allocate I2S0 RX"
    );

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BRIDGE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_24BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BRIDGE_RX_BCLK_GPIO,
            .ws = BRIDGE_RX_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BRIDGE_RX_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* TinySine-compatible input uses two physical 24-bit slots. */
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_24BIT;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_channel, &config),
        TAG,
        "could not configure I2S0 slave RX"
    );
    return i2s_channel_enable(s_rx_channel);
}

static esp_err_t init_i2s_transmitter(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = FRAMES_PER_BLOCK;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, &s_tx_channel, NULL),
        TAG,
        "could not allocate I2S1 TX"
    );

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BRIDGE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = BRIDGE_TX_MCLK_GPIO,
            .bclk = BRIDGE_TX_BCLK_GPIO,
            .ws = BRIDGE_TX_WS_GPIO,
            .dout = BRIDGE_TX_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    config.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    return i2s_channel_init_std_mode(s_tx_channel, &config);
}

static void rx_task(void *argument)
{
    (void)argument;

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t error = i2s_channel_read(
            s_rx_channel,
            s_rx_dma_samples,
            sizeof(s_rx_dma_samples),
            &bytes_read,
            pdMS_TO_TICKS(RX_TIMEOUT_MS)
        );

        if (error == ESP_ERR_TIMEOUT) {
            counter_add(&s_rx_timeouts, 1U);
            continue;
        }
        if (error != ESP_OK) {
            counter_add(&s_rx_errors, 1U);
            ESP_LOGW(TAG, "I2S RX error: %s", esp_err_to_name(error));
            continue;
        }
        if (bytes_read != sizeof(s_rx_dma_samples) ||
            (bytes_read % RX_DMA_BYTES_PER_FRAME) != 0U) {
            counter_add(&s_rx_short_reads, 1U);
        }

        uint32_t frame_count = bytes_read / RX_DMA_BYTES_PER_FRAME;
        for (uint32_t frame = 0; frame < frame_count; ++frame) {
            /* The 16-bit PCM payload occupies bits 31:16 of each 24-bit slot. */
            s_rx_pcm_frames[frame].left =
                (int16_t)(s_rx_dma_samples[2U * frame] >> 16);
            s_rx_pcm_frames[frame].right =
                (int16_t)(s_rx_dma_samples[2U * frame + 1U] >> 16);
        }

        counter_add(&s_input_frames, frame_count);
        fifo_push_block(s_rx_pcm_frames, frame_count);
    }
}

static int16_t interpolate_sample(int16_t first, int16_t second, uint32_t fraction_q32)
{
    int64_t difference = (int32_t)second - (int32_t)first;
    int64_t interpolated = (int32_t)first +
        ((difference * (int64_t)fraction_q32) >> 32);
    return (int16_t)interpolated;
}

static int32_t update_correction_ppm(uint32_t fill)
{
    int32_t error_frames = (int32_t)fill - (int32_t)BRIDGE_FIFO_TARGET_FRAMES;
    int32_t target_ppm = error_frames;

    if (target_ppm > BRIDGE_MAX_CORRECTION_PPM) {
        target_ppm = BRIDGE_MAX_CORRECTION_PPM;
    } else if (target_ppm < -BRIDGE_MAX_CORRECTION_PPM) {
        target_ppm = -BRIDGE_MAX_CORRECTION_PPM;
    }

    int32_t correction = __atomic_load_n(&s_correction_ppm, __ATOMIC_RELAXED);
    correction += (target_ppm - correction) / 16;
    __atomic_store_n(&s_correction_ppm, correction, __ATOMIC_RELAXED);
    return correction;
}

static void record_fifo_extrema(uint32_t fill)
{
    uint32_t minimum = __atomic_load_n(&s_min_fill_seen, __ATOMIC_RELAXED);
    uint32_t maximum = __atomic_load_n(&s_max_fill_seen, __ATOMIC_RELAXED);

    if (fill < minimum) {
        __atomic_store_n(&s_min_fill_seen, fill, __ATOMIC_RELAXED);
    }
    if (fill > maximum) {
        __atomic_store_n(&s_max_fill_seen, fill, __ATOMIC_RELAXED);
    }
}

static void tx_task(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG, "MCLK configured; waiting for %u input frames", BRIDGE_FIFO_PREFILL_FRAMES);
    while (fifo_fill_frames() < BRIDGE_FIFO_PREFILL_FRAMES) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_channel));
    __atomic_store_n(&s_tx_started, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "DSP I2S output started; reset or power-cycle the DSP now");

    bool streaming = false;
    stereo_frame_t current = {0};
    stereo_frame_t next = {0};
    uint64_t phase_q32 = 0;
    __atomic_store_n(&s_min_fill_seen, fifo_fill_frames(), __ATOMIC_RELAXED);
    __atomic_store_n(&s_max_fill_seen, fifo_fill_frames(), __ATOMIC_RELAXED);

    for (;;) {
        uint32_t fill = fifo_fill_frames();

        if (!streaming && fill >= BRIDGE_FIFO_PREFILL_FRAMES &&
            fifo_pop(&current) && fifo_pop(&next)) {
            streaming = true;
            phase_q32 = 0;
            counter_add(&s_stream_restarts, 1U);
            ESP_LOGI(
                TAG,
                "audio forwarding active at FIFO fill=%" PRIu32,
                fifo_fill_frames()
            );
        }

        int32_t correction_ppm = streaming ? update_correction_ppm(fill) : 0;
        int64_t step_q32_signed = (int64_t)Q32_ONE +
            ((int64_t)correction_ppm * (int64_t)Q32_ONE) / 1000000LL;
        uint64_t step_q32 = (uint64_t)step_q32_signed;

        for (uint32_t output_frame = 0; output_frame < FRAMES_PER_BLOCK; ++output_frame) {
            int16_t left = 0;
            int16_t right = 0;

            if (streaming) {
                uint32_t fraction_q32 = (uint32_t)phase_q32;
                left = interpolate_sample(current.left, next.left, fraction_q32);
                right = interpolate_sample(current.right, next.right, fraction_q32);

                phase_q32 += step_q32;
                while (phase_q32 >= Q32_ONE) {
                    stereo_frame_t following;
                    if (!fifo_pop(&following)) {
                        streaming = false;
                        phase_q32 = 0;
                        counter_add(&s_fifo_underruns, 1U);
                        ESP_LOGW(TAG, "FIFO underrun; outputting silence until refill");
                        break;
                    }
                    current = next;
                    next = following;
                    phase_q32 -= Q32_ONE;
                }
            }

            /* Send signed 16-bit PCM in the most significant bits of 32-bit slots. */
            s_tx_dma_samples[2U * output_frame] = (int32_t)left * 65536;
            s_tx_dma_samples[2U * output_frame + 1U] = (int32_t)right * 65536;
        }

        size_t bytes_written = 0;
        esp_err_t error = i2s_channel_write(
            s_tx_channel,
            s_tx_dma_samples,
            sizeof(s_tx_dma_samples),
            &bytes_written,
            pdMS_TO_TICKS(TX_TIMEOUT_MS)
        );
        if (error != ESP_OK) {
            counter_add(&s_tx_errors, 1U);
            ESP_LOGW(TAG, "I2S TX error: %s", esp_err_to_name(error));
        } else {
            if (bytes_written != sizeof(s_tx_dma_samples)) {
                counter_add(&s_tx_short_writes, 1U);
            }
            counter_add(&s_output_frames, bytes_written / TX_DMA_BYTES_PER_FRAME);
        }

        record_fifo_extrema(fifo_fill_frames());
    }
}

static void log_bridge_stats(int64_t elapsed_us, uint32_t *previous_input, uint32_t *previous_output,
    uint32_t *previous_underruns, uint32_t *previous_overruns)
{
    uint32_t input = counter_load(&s_input_frames);
    uint32_t output = counter_load(&s_output_frames);
    uint32_t underruns = counter_load(&s_fifo_underruns);
    uint32_t overruns = counter_load(&s_fifo_overruns);
    double seconds = (double)elapsed_us / 1000000.0;
    double rx_pcm_rate = seconds > 0.0
        ? (double)(input - *previous_input) * PCM_BYTES_PER_FRAME / seconds
        : 0.0;
    double tx_pcm_rate = seconds > 0.0
        ? (double)(output - *previous_output) * PCM_BYTES_PER_FRAME / seconds
        : 0.0;

    char telemetry[TELEMETRY_LINE_MAX];
    int length = snprintf(
        telemetry,
        sizeof(telemetry),
        "BRIDGE rx=%.0fB/s tx=%.0fB/s fifo=%" PRIu32 "/%u min=%" PRIu32
        " max=%" PRIu32 " corr=%" PRId32 "ppm under=%" PRIu32 "(+%" PRIu32
        ") over=%" PRIu32 "(+%" PRIu32 ") drop=%" PRIu32
        " rxerr=%" PRIu32 " timeout=%" PRIu32 " short_rx=%" PRIu32
        " txerr=%" PRIu32 " short_tx=%" PRIu32 " restart=%" PRIu32 "\r\n",
        rx_pcm_rate,
        tx_pcm_rate,
        fifo_fill_frames(),
        FIFO_INDEX_MASK,
        counter_load(&s_min_fill_seen),
        counter_load(&s_max_fill_seen),
        __atomic_load_n(&s_correction_ppm, __ATOMIC_RELAXED),
        underruns,
        underruns - *previous_underruns,
        overruns,
        overruns - *previous_overruns,
        counter_load(&s_dropped_input_frames),
        counter_load(&s_rx_errors),
        counter_load(&s_rx_timeouts),
        counter_load(&s_rx_short_reads),
        counter_load(&s_tx_errors),
        counter_load(&s_tx_short_writes),
        counter_load(&s_stream_restarts)
    );
    if (length > 0) {
        size_t bytes_to_send = (size_t)length;
        if (bytes_to_send >= sizeof(telemetry)) {
            bytes_to_send = sizeof(telemetry) - 1U;
        }
        uart_write_bytes(TELEMETRY_UART, telemetry, bytes_to_send);
    }

    *previous_input = input;
    *previous_output = output;
    *previous_underruns = underruns;
    *previous_overruns = overruns;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 asynchronous I2S clock bridge");
    bridge_log_configuration();

    ESP_ERROR_CHECK(init_telemetry_uart());
    /* TX initialization starts the 12.288 MHz MCLK before audio forwarding. */
    ESP_ERROR_CHECK(init_i2s_transmitter());
    ESP_ERROR_CHECK(init_i2s_receiver());

    BaseType_t rx_created = xTaskCreatePinnedToCore(
        rx_task, "bridge_rx", 4096, NULL, 20, NULL, 0
    );
    BaseType_t tx_created = xTaskCreatePinnedToCore(
        tx_task, "bridge_tx", 4096, NULL, 19, NULL, 1
    );
    ESP_ERROR_CHECK(rx_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(tx_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    uint32_t previous_input = 0;
    uint32_t previous_output = 0;
    uint32_t previous_underruns = 0;
    uint32_t previous_overruns = 0;
    int64_t previous_report_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));
        int64_t now_us = esp_timer_get_time();
        log_bridge_stats(
            now_us - previous_report_us,
            &previous_input,
            &previous_output,
            &previous_underruns,
            &previous_overruns
        );
        previous_report_us = now_us;

        if (!__atomic_load_n(&s_tx_started, __ATOMIC_ACQUIRE) &&
            counter_load(&s_rx_timeouts) > 0U) {
            ESP_LOGW(TAG, "TX is waiting: no complete TinySine-compatible input blocks received");
        }
    }
}
