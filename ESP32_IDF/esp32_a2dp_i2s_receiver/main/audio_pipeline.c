#include "audio_pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* User-confirmed I2S wiring. */
#define I2S_BCLK_GPIO              GPIO_NUM_14
#define I2S_WS_GPIO                GPIO_NUM_15
#define I2S_DATA_GPIO              GPIO_NUM_22

/* User-confirmed decoded PCM format. */
#define PCM_RATE_HZ                44100U
#define PCM_CHANNELS               2U
#define PCM_SAMPLE_BYTES           2U
#define PCM_FRAME_BYTES            (PCM_CHANNELS * PCM_SAMPLE_BYTES)

/*
 * 32 KiB holds about 186 ms of 44.1-kHz/16-bit/stereo PCM.
 * Playback starts or restarts after about 116 ms has accumulated.
 */
#define PCM_RING_BYTES             (32U * 1024U)
#define PCM_PREFILL_BYTES          (20U * 1024U)

/* 2048 bytes = 512 stereo frames = about 11.6 ms. */
#define I2S_WRITE_BYTES            2048U

#define AUDIO_EVENT_STREAMING      BIT0
#define AUDIO_EVENT_I2S_READY      BIT1
#define AUDIO_EVENT_I2S_FAILED     BIT2

static const char *TAG = "AUDIO_PIPELINE";

static uint8_t *s_pcm_ring;
static uint32_t s_pcm_write_seq;
static uint32_t s_pcm_read_seq;
static EventGroupHandle_t s_audio_events;
static TaskHandle_t s_i2s_task;
static i2s_chan_handle_t s_i2s_tx;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_pipeline_stats_t s_stats;
static esp_err_t s_i2s_init_result;

static esp_err_t init_i2s(void);

static void stats_update_buffer_locked(size_t buffered_bytes)
{
    if (buffered_bytes < s_stats.min_buffered_bytes) {
        s_stats.min_buffered_bytes = buffered_bytes;
    }
    if (buffered_bytes > s_stats.max_buffered_bytes) {
        s_stats.max_buffered_bytes = buffered_bytes;
    }
}


static void stats_note_buffer_level(size_t buffered_bytes)
{
    portENTER_CRITICAL(&s_stats_lock);
    stats_update_buffer_locked(buffered_bytes);
    portEXIT_CRITICAL(&s_stats_lock);
}


static bool is_streaming(void)
{
    return (xEventGroupGetBits(s_audio_events) & AUDIO_EVENT_STREAMING) != 0;
}


static size_t ring_bytes_waiting(void)
{
    if (s_pcm_ring == NULL) {
        return 0;
    }

    uint32_t write_seq = __atomic_load_n(&s_pcm_write_seq, __ATOMIC_ACQUIRE);
    uint32_t read_seq = __atomic_load_n(&s_pcm_read_seq, __ATOMIC_ACQUIRE);
    return (size_t)(write_seq - read_seq);
}


static void stats_increment(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stats_lock);
    ++(*counter);
    portEXIT_CRITICAL(&s_stats_lock);
}


static void stats_record_received(uint32_t length)
{
    portENTER_CRITICAL(&s_stats_lock);
    ++s_stats.received_packets;
    s_stats.received_bytes += length;
    portEXIT_CRITICAL(&s_stats_lock);
}


static void discard_all_buffered_audio(void)
{
    uint32_t write_seq = __atomic_load_n(&s_pcm_write_seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_pcm_read_seq, write_seq, __ATOMIC_RELEASE);
}


static bool write_all_to_i2s(const uint8_t *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_i2s_tx,
            data + offset,
            length - offset,
            &written,
            portMAX_DELAY
        );

        if (err != ESP_OK || written == 0) {
            stats_increment(&s_stats.i2s_errors);
            return false;
        }

        offset += written;
    }

    return true;
}


static bool wait_for_prefill(void)
{
    stats_increment(&s_stats.prefill_waits);

    while (is_streaming() && ring_bytes_waiting() < PCM_PREFILL_BYTES) {
        stats_note_buffer_level(ring_bytes_waiting());
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    stats_note_buffer_level(ring_bytes_waiting());
    return is_streaming();
}


static void i2s_feeder_task(void *argument)
{
    (void)argument;

    /* Allocate the I2S interrupt on core 1, away from the core-0 BT stack. */
    s_i2s_init_result = init_i2s();
    if (s_i2s_init_result != ESP_OK) {
        xEventGroupSetBits(s_audio_events, AUDIO_EVENT_I2S_FAILED);
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(s_audio_events, AUDIO_EVENT_I2S_READY);

    for (;;) {
        xEventGroupWaitBits(
            s_audio_events,
            AUDIO_EVENT_STREAMING,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        /* Never play PCM left over from an earlier stream. */
        discard_all_buffered_audio();

        if (!wait_for_prefill()) {
            continue;
        }

        stats_note_buffer_level(ring_bytes_waiting());

        while (is_streaming()) {
            uint32_t read_seq = __atomic_load_n(&s_pcm_read_seq, __ATOMIC_RELAXED);
            uint32_t write_seq = __atomic_load_n(&s_pcm_write_seq, __ATOMIC_ACQUIRE);
            size_t available = (size_t)(write_seq - read_seq);

            if (available == 0) {
                stats_increment(&s_stats.underruns);
                stats_note_buffer_level(ring_bytes_waiting());

                /*
                 * The DMA auto-clear option outputs zeros. Rebuild the jitter
                 * buffer instead of repeatedly starting on individual packets.
                 */
                if (!wait_for_prefill()) {
                    break;
                }
                continue;
            }

            size_t offset = read_seq & (PCM_RING_BYTES - 1U);
            size_t length = available < I2S_WRITE_BYTES ? available : I2S_WRITE_BYTES;
            size_t contiguous = PCM_RING_BYTES - offset;
            if (length > contiguous) {
                length = contiguous;
            }
            length -= length % PCM_FRAME_BYTES;

            bool success = write_all_to_i2s(s_pcm_ring + offset, length);
            __atomic_store_n(&s_pcm_read_seq, read_seq + length, __ATOMIC_RELEASE);
            stats_note_buffer_level(ring_bytes_waiting());

            if (!success) {
                break;
            }
        }

        discard_all_buffered_audio();
    }
}


static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = 256;
    channel_config.auto_clear = true;

    esp_err_t err = i2s_new_channel(&channel_config, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCM_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx, &standard_config);

    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    ESP_LOGI(
        TAG,
        "I2S ready: 44100 Hz, 16-bit stereo Philips; BCLK=%d WS=%d DATA=%d",
        I2S_BCLK_GPIO,
        I2S_WS_GPIO,
        I2S_DATA_GPIO
    );

    return ESP_OK;
}


esp_err_t audio_pipeline_init(void)
{
    if (s_pcm_ring != NULL || s_audio_events != NULL || s_i2s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pcm_ring = malloc(PCM_RING_BYTES);
    if (s_pcm_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.min_buffered_bytes = PCM_RING_BYTES;
    portEXIT_CRITICAL(&s_stats_lock);

    s_audio_events = xEventGroupCreate();
    if (s_audio_events == NULL) {
        free(s_pcm_ring);
        s_pcm_ring = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        i2s_feeder_task,
        "i2s_feeder",
        4096,
        NULL,
        configMAX_PRIORITIES - 3,
        &s_i2s_task,
        1
    );

    if (created != pdPASS) {
        vEventGroupDelete(s_audio_events);
        free(s_pcm_ring);
        s_audio_events = NULL;
        s_pcm_ring = NULL;
        return ESP_ERR_NO_MEM;
    }

    EventBits_t init_bits = xEventGroupWaitBits(
        s_audio_events,
        AUDIO_EVENT_I2S_READY | AUDIO_EVENT_I2S_FAILED,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );
    if ((init_bits & AUDIO_EVENT_I2S_FAILED) != 0) {
        s_i2s_task = NULL;
        vEventGroupDelete(s_audio_events);
        free(s_pcm_ring);
        s_audio_events = NULL;
        s_pcm_ring = NULL;
        return s_i2s_init_result;
    }

    return ESP_OK;
}


void audio_pipeline_set_streaming(bool streaming)
{
    if (s_audio_events == NULL || s_i2s_task == NULL) {
        return;
    }

    if (streaming) {
        portENTER_CRITICAL(&s_stats_lock);
        ++s_stats.stream_starts;
        s_stats.min_buffered_bytes = PCM_RING_BYTES;
        s_stats.max_buffered_bytes = 0;
        portEXIT_CRITICAL(&s_stats_lock);
        xEventGroupSetBits(s_audio_events, AUDIO_EVENT_STREAMING);
    } else {
        xEventGroupClearBits(s_audio_events, AUDIO_EVENT_STREAMING);
    }

    /* Wake the feeder if it is waiting for a streaming-state transition. */
    xTaskNotifyGive(s_i2s_task);
}


void audio_pipeline_push(const uint8_t *data, uint32_t length)
{
    if (data == NULL || length == 0 || s_pcm_ring == NULL || s_audio_events == NULL) {
        return;
    }

    if (!is_streaming()) {
        stats_increment(&s_stats.dropped_while_stopped);
        return;
    }

    /* One signed-16-bit left sample plus one right sample per frame. */
    if ((length % PCM_FRAME_BYTES) != 0) {
        stats_increment(&s_stats.bad_packets);
        return;
    }

    stats_record_received(length);

    uint32_t write_seq = __atomic_load_n(&s_pcm_write_seq, __ATOMIC_RELAXED);
    uint32_t read_seq = __atomic_load_n(&s_pcm_read_seq, __ATOMIC_ACQUIRE);
    size_t buffered = (size_t)(write_seq - read_seq);
    if (length > PCM_RING_BYTES - buffered) {
        stats_increment(&s_stats.overruns);
        return;
    }

    size_t offset = write_seq & (PCM_RING_BYTES - 1U);
    size_t first = PCM_RING_BYTES - offset;
    if (first > length) {
        first = length;
    }
    memcpy(s_pcm_ring + offset, data, first);
    memcpy(s_pcm_ring, data + first, length - first);
    __atomic_store_n(&s_pcm_write_seq, write_seq + length, __ATOMIC_RELEASE);
}


void audio_pipeline_get_stats(audio_pipeline_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    memcpy(stats, &s_stats, sizeof(*stats));
    portEXIT_CRITICAL(&s_stats_lock);

    stats->buffered_bytes = ring_bytes_waiting();
    if (stats->min_buffered_bytes > stats->buffered_bytes) {
        stats->min_buffered_bytes = stats->buffered_bytes;
    }
}
