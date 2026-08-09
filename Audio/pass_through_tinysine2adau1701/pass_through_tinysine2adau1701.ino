// ESP32 clock timer - BT module to ADAU1701

#define I2S_USE_CALLBACKS false // Disable I2S driver callbacks to clean up IRAM warning

#include "AudioTools.h"

#define AMP_REMOTE_PIN 4

I2SStream i2s_in;
I2SStream i2s_out;

StreamCopy copier(i2s_out, i2s_in);

void setup() {
  Serial.begin(115200);
  // Set to Info to see if copier is moving bytes
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  pinMode(AMP_REMOTE_PIN, OUTPUT);
  digitalWrite(AMP_REMOTE_PIN, LOW); // Start with amp OFF to prevent startup pops

  auto cfg_in = i2s_in.defaultConfig(RX_MODE);
  cfg_in.port_no = 0;                 // Use I2S Port 0
  cfg_in.sample_rate = 48000; 
  cfg_in.bits_per_sample = 16;
  cfg_in.channels = 2;
  cfg_in.i2s_format = I2S_MSB_FORMAT;
  cfg_in.is_master = true;

  // Input pins configuration
  cfg_in.pin_bck = 14; // BCLK
  cfg_in.pin_ws = 15;  // LRCK
  cfg_in.pin_data = 34; // SD

  i2s_in.begin(cfg_in);

  // ----------------------------------------------------------------
  // Configure output stream: ESP32 -> ADAU1701
  // ----------------------------------------------------------------
  auto cfg_out = i2s_out.defaultConfig(TX_MODE);
  cfg_out.port_no = 1;                // Use I2S Port 1
  cfg_out.sample_rate = 48000;
  cfg_out.bits_per_sample = 16; 
  cfg_out.channels = 2; 
  cfg_out.i2s_format = I2S_PHILIPS_FORMAT;

  cfg_out.is_master = false; // ESP32 is slave
  cfg_out.use_apll = false;  // Use incoming clocks instead => ADAU1701 is master

  // Output pins config
  cfg_out.pin_bck = 26; // BCLK
  cfg_out.pin_ws = 25;  // LRCK
  cfg_out.pin_data = 22; // SD

  i2s_out.begin(cfg_out);

  Serial.println("Pass through initialized");

  // ----------------------------------------------------------------
  // Enable Amplifier
  // ----------------------------------------------------------------
  delay(2000); // Wait 2 seconds for Bluetooth & DSP clocks to stabilize
  digitalWrite(AMP_REMOTE_PIN, HIGH); // Drive GPIO 4 HIGH for the entire session
  Serial.println("Amplifier turned ON");
}

void loop() {
  copier.copy();
}