// ESP32 clock timer - BT module to ADAU1701

#include "AudioTools.h"

I2SStream i2s_in;
I2SStream i2s_out;

StreamCopy copier(i2s_out, i2s_in);

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // Configure input stream: Tinysine -> ESP32 
  auto cfg_in = i2s_in.defaultConfig(RX_MODE);
  cfg_in.sample_rate = 48000; 
  cfg_in.bits_per_sample = 16;
  cfg_in.channels = 2;
  cfg_in.i2s_format = I2S_MSB_FORMAT;
  cfg_in.is_master = true;

  // Input pis configuration
  cfg_in.pin_bck = 14; // BCLK
  cfg_in.pin_ws = 15; // LRCK
  cfg_in.pin_data = 34; // SD

  i2s_in.begin(cfg_in);

  // Configure output stream
  auto cfg_out = i2s_out.defaultConfig(TX_MODE);
  cfg_out.sample_rate = 48000;
  cfg_out.bits_per_sample = 16; 
  cfg_out.channels = 2; 
  cfg_out.i2s_format = I2S_MSB_FORMAT;

  cfg_out.is_master = false; // ESP32 is slave
  cfg_out.use_apll = false; // Use incoming clocks instead => ADAU1701 is master

  // output pins config
  cfg_out.pin_bck = 26; // BCLK
  cfg_out.pin_ws = 25;  // LRCK
  cfg_out.pin_data = 22; // SD

  i2s_out.begin(cfg_out);

  Serial.println("Pass through initialized");

}

void loop() {
  copier.copy();

}
// ADAU1701 header J4 -> ESP32: 
//    MP4/MP5 (BCLK & LRCK outputs)
// MP4 (BLCK) -> ESP32 pin 26 
// MP5 (LRCK) -> ESP32 pin 25
// Pin 12 (SDATA_IN) -> ESP32 pin 22 
