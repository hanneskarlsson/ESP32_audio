#include "driver/i2s.h"

#define I2S_NUM         I2S_NUM_0
#define I2S_MCLK_PIN    0   // GPIO 0 for Master Clock
#define I2S_BCLK_PIN    26  // Bit Clock
#define I2S_LRCK_PIN    25  // Frame Clock (LRCK)
#define I2S_DOUT_PIN    22  // Data Out

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Initializing I2S Master Clock Generator...");

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 48000,                            // 48 kHz standard rate for ADAU1701
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = true,                                // ENABLES THE HIGH-ACCURACY AUDIO PLL
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_MCLK_PIN,                     // Hardware MCLK Output
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  // Install and start I2S driver
  if (i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL) == ESP_OK &&
      i2s_set_pin(I2S_NUM, &pin_config) == ESP_OK) {
    Serial.println("SUCCESS: Hardware I2S & MCLK initialized!");
    Serial.println("Generating 12.288 MHz clock on GPIO 0.");
  } else {
    Serial.println("ERROR: Failed to initialize I2S hardware!");
  }
}

void loop() {
  // The I2S hardware peripheral runs continuously in the background on silicon
  delay(5000);
  Serial.println("I2S clock engine active...");
}