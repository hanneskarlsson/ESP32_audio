#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_pipeline.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define BLUETOOTH_DEVICE_NAME "ESP32-I2S-Receiver"

static const char *TAG = "A2DP_RECEIVER";
static bool s_audio_config_supported;


static const char *connection_state_name(esp_a2d_connection_state_t state)
{
    switch (state) {
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            return "disconnected";
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            return "connecting";
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            return "connected";
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            return "disconnecting";
        default:
            return "unknown";
    }
}


static const char *audio_state_name(esp_a2d_audio_state_t state)
{
    switch (state) {
        case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
            return "remote suspend";
        case ESP_A2D_AUDIO_STATE_STOPPED:
            return "stopped";
        case ESP_A2D_AUDIO_STATE_STARTED:
            return "started";
        default:
            return "unknown";
    }
}


static uint32_t sbc_sample_rate(const esp_a2d_mcc_t *mcc)
{
    if (mcc == NULL || mcc->type != ESP_A2D_MCT_SBC) {
        return 0;
    }

    uint8_t octet0 = mcc->cie.sbc[0];

    if ((octet0 & 0x80U) != 0) {
        return 16000;
    }
    if ((octet0 & 0x40U) != 0) {
        return 32000;
    }
    if ((octet0 & 0x20U) != 0) {
        return 44100;
    }
    if ((octet0 & 0x10U) != 0) {
        return 48000;
    }

    return 0;
}


static void a2dp_event_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
        case ESP_A2D_PROF_STATE_EVT:
            if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
                ESP_LOGI(TAG, "A2DP sink initialized; device is discoverable");
                ESP_ERROR_CHECK(
                    esp_bt_gap_set_scan_mode(
                        ESP_BT_CONNECTABLE,
                        ESP_BT_GENERAL_DISCOVERABLE
                    )
                );
            }
            break;

        case ESP_A2D_CONNECTION_STATE_EVT: {
            const uint8_t *address = param->conn_stat.remote_bda;
            ESP_LOGI(
                TAG,
                "A2DP %s: %02x:%02x:%02x:%02x:%02x:%02x",
                connection_state_name(param->conn_stat.state),
                address[0], address[1], address[2],
                address[3], address[4], address[5]
            );

            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_audio_config_supported = false;
                audio_pipeline_set_streaming(false);
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            }
            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "A2DP audio %s", audio_state_name(param->audio_stat.state));

            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED &&
                !s_audio_config_supported) {
                ESP_LOGE(
                    TAG,
                    "Stream started without a supported 44.1-kHz SBC configuration"
                );
            }

            audio_pipeline_set_streaming(
                param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED &&
                s_audio_config_supported
            );
            break;

        case ESP_A2D_AUDIO_CFG_EVT: {
            uint32_t rate = sbc_sample_rate(&param->audio_cfg.mcc);
            s_audio_config_supported = (rate == 44100U);
            ESP_LOGI(TAG, "Negotiated SBC sample rate: %" PRIu32 " Hz", rate);

            if (!s_audio_config_supported) {
                ESP_LOGE(
                    TAG,
                    "This build requires 44100-Hz PCM, but the source negotiated %" PRIu32 " Hz",
                    rate
                );
                audio_pipeline_set_streaming(false);
            }
            break;
        }

        default:
            break;
    }
}


static void a2dp_pcm_callback(const uint8_t *data, uint32_t length)
{
    audio_pipeline_push(data, length);
}


static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Bluetooth authentication successful: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Bluetooth authentication failed: status=%d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT: {
            esp_bt_pin_code_t pin_code = {0};

            if (param->pin_req.min_16_digit) {
                ESP_LOGI(TAG, "Replying to 16-digit legacy PIN request");
                esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
            } else {
                ESP_LOGI(TAG, "Replying to legacy PIN request with 1234");
                pin_code[0] = '1';
                pin_code[1] = '2';
                pin_code[2] = '3';
                pin_code[3] = '4';
                esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
            }
            break;
        }

#if defined(CONFIG_BT_SSP_ENABLED) && CONFIG_BT_SSP_ENABLED
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "Confirming SSP numeric value: %" PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP passkey: %06" PRIu32, param->key_notif.passkey);
            break;
#endif

        default:
            break;
    }
}


static void diagnostics_task(void *argument)
{
    (void)argument;

    uint32_t previous_received_bytes = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        audio_pipeline_stats_t stats;
        audio_pipeline_get_stats(&stats);

        uint32_t bytes_per_second = stats.received_bytes - previous_received_bytes;
        previous_received_bytes = stats.received_bytes;

        ESP_LOGI(
            TAG,
            "PCM rx=%" PRIu32 " B/s fill=%u over=%" PRIu32
            " under=%" PRIu32 " bad=%" PRIu32 " i2s_err=%" PRIu32,
            bytes_per_second,
            (unsigned)stats.buffered_bytes,
            stats.overruns,
            stats.underruns,
            stats.bad_packets,
            stats.i2s_errors
        );
    }
}


static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}


static void initialize_bluetooth(void)
{
    /* This firmware uses Classic Bluetooth only. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_dev_set_device_name(BLUETOOTH_DEVICE_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));

#if defined(CONFIG_BT_SSP_ENABLED) && CONFIG_BT_SSP_ENABLED
    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(
        esp_bt_gap_set_security_param(
            ESP_BT_SP_IOCAP_MODE,
            &io_capability,
            sizeof(io_capability)
        )
    );
#endif

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 0, pin_code));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_event_callback));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_pcm_callback));
}


void app_main(void)
{
    initialize_nvs();

    ESP_ERROR_CHECK(audio_pipeline_init());
    initialize_bluetooth();

    BaseType_t created = xTaskCreate(
        diagnostics_task,
        "audio_diagnostics",
        3072,
        NULL,
        1,
        NULL
    );
    configASSERT(created == pdPASS);

    ESP_LOGI(TAG, "Ready. Pair with Bluetooth device '%s'", BLUETOOTH_DEVICE_NAME);
}
