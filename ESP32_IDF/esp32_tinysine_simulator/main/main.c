#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Documented TinySine AudioB I2S master output. */
#define SAMPLE_RATE_HZ       48000U
#define TONE_FREQUENCY_HZ      500U
#define CHANNEL_COUNT             2U
#define SAMPLE_BITS              16U
#define SLOT_BITS                24U
#define FRAMES_PER_BLOCK         480U
#define TONE_PERIOD_FRAMES       (SAMPLE_RATE_HZ / TONE_FREQUENCY_HZ)

/* Change only these three definitions when rewiring the simulator board. */
#define I2S_BCLK_GPIO         GPIO_NUM_14
#define I2S_WS_GPIO           GPIO_NUM_15
#define I2S_DATA_GPIO         GPIO_NUM_22

#define BRIDGE_TELEMETRY_UART    UART_NUM_1
#define BRIDGE_TELEMETRY_RX_GPIO GPIO_NUM_32
#define BRIDGE_TELEMETRY_BAUD     115200

static const char *TAG = "TINYSINE_SIM";
static i2s_chan_handle_t s_tx_channel;
static int16_t s_tone[TONE_PERIOD_FRAMES * CHANNEL_COUNT];

/* ESP32 DMA represents each 24-bit sample as a 32-bit word, high bits valid. */
static int32_t s_tx_block[FRAMES_PER_BLOCK * CHANNEL_COUNT];

static esp_err_t init_bridge_telemetry_receiver(void)
{
    uart_config_t config = {
        .baud_rate = BRIDGE_TELEMETRY_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        uart_driver_install(BRIDGE_TELEMETRY_UART, 2048, 0, 0, NULL, 0),
        TAG,
        "could not install telemetry receiver"
    );
    ESP_RETURN_ON_ERROR(
        uart_param_config(BRIDGE_TELEMETRY_UART, &config),
        TAG,
        "could not configure telemetry receiver"
    );
    return uart_set_pin(
        BRIDGE_TELEMETRY_UART,
        UART_PIN_NO_CHANGE,
        BRIDGE_TELEMETRY_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );
}

static void bridge_telemetry_relay_task(void *argument)
{
    (void)argument;
    uint8_t buffer[256];

    for (;;) {
        int bytes_read = uart_read_bytes(
            BRIDGE_TELEMETRY_UART,
            buffer,
            sizeof(buffer),
            pdMS_TO_TICKS(100)
        );
        if (bytes_read > 0) {
            fwrite(buffer, 1, (size_t)bytes_read, stdout);
            fflush(stdout);
        }
    }
}

static void make_500_hz_tone(void)
{
    const float amplitude = 0.25f * INT16_MAX;

    for (uint32_t frame = 0; frame < TONE_PERIOD_FRAMES; ++frame) {
        float phase = 2.0f * (float)M_PI * (float)frame / (float)TONE_PERIOD_FRAMES;
        int16_t sample = (int16_t)lrintf(amplitude * sinf(phase));
        s_tone[2U * frame] = sample;
        s_tone[2U * frame + 1U] = sample;
    }

    for (uint32_t frame = 0; frame < FRAMES_PER_BLOCK; ++frame) {
        uint32_t source = frame % TONE_PERIOD_FRAMES;
        int32_t left = (int32_t)s_tone[2U * source] * 65536;
        int32_t right = (int32_t)s_tone[2U * source + 1U] * 65536;
        s_tx_block[2U * frame] = left;
        s_tx_block[2U * frame + 1U] = right;
    }
}

static esp_err_t init_i2s_transmitter(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = 240;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, &s_tx_channel, NULL),
        TAG,
        "could not allocate I2S0 TX"
    );

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_24BIT,
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

    /* Two true 24-bit slots produce 48 BCLK periods per stereo frame. */
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_channel, &config),
        TAG,
        "could not configure TinySine-compatible I2S"
    );
    return i2s_channel_enable(s_tx_channel);
}

void app_main(void)
{
    make_500_hz_tone();
    ESP_ERROR_CHECK(init_i2s_transmitter());
    ESP_ERROR_CHECK(init_bridge_telemetry_receiver());

    BaseType_t relay_created = xTaskCreate(
        bridge_telemetry_relay_task,
        "bridge_telemetry",
        3072,
        NULL,
        2,
        NULL
    );
    ESP_ERROR_CHECK(relay_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "TinySine simulator running");
    ESP_LOGI(TAG, "PCM: 48000 Hz, signed 16-bit stereo, 500-Hz tone");
    ESP_LOGI(TAG, "I2S: Philips one-bit delay, true 24-bit slots, BCLK=2.304 MHz");
    ESP_LOGI(TAG, "Pins: BCLK=%d LRCLK=%d SD=%d", I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DATA_GPIO);
    ESP_LOGI(TAG, "Bridge telemetry relay: GPIO32 RX at 115200 baud");

    uint64_t frames_sent = 0;
    TickType_t next_report = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    for (;;) {
        size_t bytes_written = 0;
        esp_err_t error = i2s_channel_write(
            s_tx_channel,
            s_tx_block,
            sizeof(s_tx_block),
            &bytes_written,
            portMAX_DELAY
        );
        if (error != ESP_OK || bytes_written != sizeof(s_tx_block)) {
            ESP_LOGE(
                TAG,
                "I2S write failed: %s, wrote %u/%u bytes",
                esp_err_to_name(error),
                (unsigned)bytes_written,
                (unsigned)sizeof(s_tx_block)
            );
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        frames_sent += FRAMES_PER_BLOCK;
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_report) >= 0) {
            ESP_LOGI(TAG, "continuous output; frames_sent=%llu", frames_sent);
            next_report += pdMS_TO_TICKS(5000);
        }
    }
}
