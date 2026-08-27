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
#include "freertos/ringbuf.h"
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

static const char *TAG = "AUDIO_PIPELINE";

static RingbufHandle_t s_pcm_ring;
static EventGroupHandle_t s_audio_events;
static TaskHandle_t s_i2s_task;
static i2s_chan_handle_t s_i2s_tx;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_pipeline_stats_t s_stats;


static bool is_streaming(void)
{
    return (xEventGroupGetBits(s_audio_events) & AUDIO_EVENT_STREAMING) != 0;
}


static size_t ring_bytes_waiting(void)
{
    UBaseType_t waiting = 0;

    if (s_pcm_ring == NULL) {
        return 0;
    }

    vRingbufferGetInfo(s_pcm_ring, NULL, NULL, NULL, NULL, &waiting);
    return (size_t)waiting;
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
    for (;;) {
        size_t length = 0;
        void *data = xRingbufferReceiveUpTo(s_pcm_ring, &length, 0, I2S_WRITE_BYTES);

        if (data == NULL) {
            return;
        }

        vRingbufferReturnItem(s_pcm_ring, data);
    }
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
    /* Remove packet notifications accumulated during the previous run. */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    while (is_streaming() && ring_bytes_waiting() < PCM_PREFILL_BYTES) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    return is_streaming();
}


static void i2s_feeder_task(void *argument)
{
    (void)argument;

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

        while (is_streaming()) {
            size_t length = 0;
            uint8_t *data = xRingbufferReceiveUpTo(
                s_pcm_ring,
                &length,
                pdMS_TO_TICKS(25),
                I2S_WRITE_BYTES
            );

            if (data == NULL) {
                stats_increment(&s_stats.underruns);

                /*
                 * The DMA auto-clear option outputs zeros. Rebuild the jitter
                 * buffer instead of repeatedly starting on individual packets.
                 */
                if (!wait_for_prefill()) {
                    break;
                }
                continue;
            }

            if (!is_streaming()) {
                vRingbufferReturnItem(s_pcm_ring, data);
                break;
            }

            bool success = write_all_to_i2s(data, length);
            vRingbufferReturnItem(s_pcm_ring, data);

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

    /* The audio PLL gives a more accurate 44.1-kHz clock on ESP32. */
    standard_config.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

    err = i2s_channel_init_std_mode(s_i2s_tx, &standard_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APLL I2S setup failed (%s); trying default clock", esp_err_to_name(err));
        standard_config.clk_cfg = (i2s_std_clk_config_t)
            I2S_STD_CLK_DEFAULT_CONFIG(PCM_RATE_HZ);
        err = i2s_channel_init_std_mode(s_i2s_tx, &standard_config);
    }

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

    s_pcm_ring = xRingbufferCreate(PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_pcm_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_audio_events = xEventGroupCreate();
    if (s_audio_events == NULL) {
        vRingbufferDelete(s_pcm_ring);
        s_pcm_ring = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_i2s();
    if (err != ESP_OK) {
        vEventGroupDelete(s_audio_events);
        vRingbufferDelete(s_pcm_ring);
        s_audio_events = NULL;
        s_pcm_ring = NULL;
        return err;
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
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        vEventGroupDelete(s_audio_events);
        vRingbufferDelete(s_pcm_ring);
        s_i2s_tx = NULL;
        s_audio_events = NULL;
        s_pcm_ring = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


void audio_pipeline_set_streaming(bool streaming)
{
    if (s_audio_events == NULL || s_i2s_task == NULL) {
        return;
    }

    if (streaming) {
        xEventGroupSetBits(s_audio_events, AUDIO_EVENT_STREAMING);
    } else {
        xEventGroupClearBits(s_audio_events, AUDIO_EVENT_STREAMING);
    }

    /* Wake the feeder if it is waiting for prefill or a state transition. */
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

    /* Zero timeout: the Bluetooth stack must never wait for the I2S side. */
    if (xRingbufferSend(s_pcm_ring, data, length, 0) != pdTRUE) {
        stats_increment(&s_stats.overruns);
        return;
    }

    xTaskNotifyGive(s_i2s_task);
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
}
