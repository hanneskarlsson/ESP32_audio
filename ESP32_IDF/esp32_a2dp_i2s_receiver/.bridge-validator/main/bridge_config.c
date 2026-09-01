#include "bridge_config.h"

#include "esp_log.h"

static const char *TAG = "BRIDGE_CONFIG";

void bridge_log_configuration(void)
{
    ESP_LOGI(TAG, "Nominal PCM: %u Hz, %u-bit stereo", BRIDGE_SAMPLE_RATE_HZ, BRIDGE_SAMPLE_BITS);
    ESP_LOGI(
        TAG,
        "RX I2S0 slave: BCLK=%d WS=%d DATA=%d, %u-bit slots",
        BRIDGE_RX_BCLK_GPIO,
        BRIDGE_RX_WS_GPIO,
        BRIDGE_RX_DATA_GPIO,
        BRIDGE_INPUT_SLOT_BITS
    );
    ESP_LOGI(
        TAG,
        "Future TX I2S1: MCLK=%d BCLK=%d WS=%d DATA=%d, MCLK=%u Hz",
        BRIDGE_TX_MCLK_GPIO,
        BRIDGE_TX_BCLK_GPIO,
        BRIDGE_TX_WS_GPIO,
        BRIDGE_TX_DATA_GPIO,
        BRIDGE_OUTPUT_MCLK_HZ
    );
}
