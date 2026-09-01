#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "bridge_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define RX_FRAMES_PER_BLOCK       480U
#define PCM_BYTES_PER_FRAME       (BRIDGE_CHANNEL_COUNT * sizeof(int16_t))
#define DMA_BYTES_PER_FRAME       (BRIDGE_CHANNEL_COUNT * sizeof(int32_t))
#define EXPECTED_PCM_BYTES_PER_SECOND (BRIDGE_SAMPLE_RATE_HZ * PCM_BYTES_PER_FRAME)
#define EXPECTED_DMA_BYTES_PER_SECOND (BRIDGE_SAMPLE_RATE_HZ * DMA_BYTES_PER_FRAME)
#define REPORT_INTERVAL_US        1000000LL
#define RX_TIMEOUT_MS             1000U

typedef struct {
    uint64_t bytes;
    uint64_t dma_bytes;
    uint64_t frames;
    uint64_t sum_squares;
    int64_t sample_sum;
    uint32_t positive_crossings;
    uint32_t stereo_mismatches;
    uint32_t clipped_samples;
    uint32_t short_reads;
    uint32_t read_errors;
    uint32_t timeouts;
    uint16_t peak;
} interval_stats_t;

static const char *TAG = "I2S_RX_TEST";
static i2s_chan_handle_t s_rx_channel;
static int32_t s_rx_samples[RX_FRAMES_PER_BLOCK * BRIDGE_CHANNEL_COUNT];

static esp_err_t init_i2s_receiver(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = 240;

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

    /* TinySine documents 48 bit clocks per stereo frame: two 24-bit slots. */
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_24BIT;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_channel, &config),
        TAG,
        "could not configure I2S0 slave RX"
    );
    return i2s_channel_enable(s_rx_channel);
}

static uint16_t magnitude_i16(int16_t sample)
{
    return sample == INT16_MIN ? 32768U : (uint16_t)(sample < 0 ? -sample : sample);
}

static void analyze_frames(
    interval_stats_t *stats,
    const int32_t *samples,
    size_t frame_count,
    int16_t *previous_left
)
{
    for (size_t frame = 0; frame < frame_count; ++frame) {
        /* ESP32 aligns each received 24-bit sample in bits 31:8. */
        int16_t left = (int16_t)(samples[2U * frame] >> 16);
        int16_t right = (int16_t)(samples[2U * frame + 1U] >> 16);
        uint16_t magnitude = magnitude_i16(left);

        stats->sample_sum += left;
        stats->sum_squares += (uint64_t)((int32_t)left * (int32_t)left);
        if (magnitude > stats->peak) {
            stats->peak = magnitude;
        }
        if (left == INT16_MIN || left == INT16_MAX) {
            ++stats->clipped_samples;
        }
        if (left != right) {
            ++stats->stereo_mismatches;
        }
        if (*previous_left <= 0 && left > 0) {
            ++stats->positive_crossings;
        }
        *previous_left = left;
    }

    stats->frames += frame_count;
    stats->bytes += frame_count * PCM_BYTES_PER_FRAME;
    stats->dma_bytes += frame_count * DMA_BYTES_PER_FRAME;
}

static void report_and_reset(interval_stats_t *stats, int64_t elapsed_us)
{
    double seconds = (double)elapsed_us / 1000000.0;
    double bytes_per_second = seconds > 0.0 ? (double)stats->bytes / seconds : 0.0;
    double dma_bytes_per_second = seconds > 0.0 ? (double)stats->dma_bytes / seconds : 0.0;
    double frames_per_second = seconds > 0.0 ? (double)stats->frames / seconds : 0.0;
    double tone_hz = seconds > 0.0 ? (double)stats->positive_crossings / seconds : 0.0;
    double rms = stats->frames > 0
        ? sqrt((double)stats->sum_squares / (double)stats->frames)
        : 0.0;
    double dc = stats->frames > 0
        ? (double)stats->sample_sum / (double)stats->frames
        : 0.0;
    double rate_error_percent =
        100.0 * (bytes_per_second - EXPECTED_PCM_BYTES_PER_SECOND) /
        EXPECTED_PCM_BYTES_PER_SECOND;

    ESP_LOGI(
        TAG,
        "pcm=%.0f B/s dma=%.0f B/s frames=%.1f/s err=%+.3f%% tone=%.1f Hz "
        "rms=%.1f peak=%u dc=%.1f "
        "stereo_bad=%" PRIu32 " clip=%" PRIu32 " short=%" PRIu32
        " read_err=%" PRIu32 " timeout=%" PRIu32,
        bytes_per_second,
        dma_bytes_per_second,
        frames_per_second,
        rate_error_percent,
        tone_hz,
        rms,
        stats->peak,
        dc,
        stats->stereo_mismatches,
        stats->clipped_samples,
        stats->short_reads,
        stats->read_errors,
        stats->timeouts
    );

    *stats = (interval_stats_t){0};
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 TinySine-compatible I2S input validator");
    bridge_log_configuration();
    ESP_ERROR_CHECK(init_i2s_receiver());
    ESP_LOGI(TAG, "RX enabled; waiting for BCLK/LRCLK/data from the simulator");

    interval_stats_t stats = {0};
    int16_t previous_left = 0;
    bool preview_printed = false;
    int64_t interval_start = esp_timer_get_time();

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t error = i2s_channel_read(
            s_rx_channel,
            s_rx_samples,
            sizeof(s_rx_samples),
            &bytes_read,
            pdMS_TO_TICKS(RX_TIMEOUT_MS)
        );

        if (error == ESP_ERR_TIMEOUT) {
            ++stats.timeouts;
        } else if (error != ESP_OK) {
            ++stats.read_errors;
            ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(error));
        } else {
            if ((bytes_read % DMA_BYTES_PER_FRAME) != 0 || bytes_read != sizeof(s_rx_samples)) {
                ++stats.short_reads;
            }

            size_t frame_count = bytes_read / DMA_BYTES_PER_FRAME;
            analyze_frames(&stats, s_rx_samples, frame_count, &previous_left);

            if (!preview_printed && frame_count >= 8U) {
                ESP_LOGI(
                    TAG,
                    "first L samples: %d %d %d %d %d %d %d %d",
                    (int16_t)(s_rx_samples[0] >> 16), (int16_t)(s_rx_samples[2] >> 16),
                    (int16_t)(s_rx_samples[4] >> 16), (int16_t)(s_rx_samples[6] >> 16),
                    (int16_t)(s_rx_samples[8] >> 16), (int16_t)(s_rx_samples[10] >> 16),
                    (int16_t)(s_rx_samples[12] >> 16), (int16_t)(s_rx_samples[14] >> 16)
                );
                preview_printed = true;
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - interval_start >= REPORT_INTERVAL_US) {
            report_and_reset(&stats, now - interval_start);
            interval_start = now;
        }
    }
}
