// ESP32 Input Diagnostic Test - Tinysine BT to Serial Monitor
#define I2S_USE_CALLBACKS false

#include "AudioTools.h"
#include <math.h>

I2SStream i2s_in;

unsigned long lastPrint = 0;
long sampleCount = 0;
double sumSquares = 0;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  auto cfg_in = i2s_in.defaultConfig(RX_MODE);
  cfg_in.port_no = 0;                
  cfg_in.sample_rate = 48000; 
  cfg_in.bits_per_sample = 16;
  cfg_in.channels = 2;
  cfg_in.i2s_format = I2S_PHILIPS_FORMAT; 
  cfg_in.is_master = true;   
        
  cfg_in.pin_bck = 14;  // BCLK
  cfg_in.pin_ws = 15;   // LRCK
  cfg_in.pin_data = 34; // SD / Data In

  i2s_in.begin(cfg_in);

  Serial.println("\n=================================");
  Serial.println("ESP32 BT Audio Input Test Started");
  Serial.println("Play music on your phone now...");
  Serial.println("=================================\n");
}

void loop() {
  int16_t sampleBuffer[512];
  
  // Read raw audio samples from the Tinysine module
  size_t bytesRead = i2s_in.readBytes((uint8_t*)sampleBuffer, sizeof(sampleBuffer));
  int samplesRead = bytesRead / sizeof(int16_t);

  for (int i = 0; i < samplesRead; i++) {
    double sample = sampleBuffer[i];
    sumSquares += sample * sample;
    sampleCount++;
  }

  // Print volume level every 500 ms
  if (millis() - lastPrint > 500) {
    if (sampleCount > 0) {
      double rms = sqrt(sumSquares / sampleCount);
      
      // Print a simple visual bar graph in the terminal
      int barLength = map((int)rms, 0, 10000, 0, 40);
      barLength = constrain(barLength, 0, 40);

      Serial.printf("RMS Level: %5.0f | ", rms);
      for (int b = 0; b < barLength; b++) Serial.print("=");
      Serial.println();
    }
    
    // Reset accumulators
    sumSquares = 0;
    sampleCount = 0;
    lastPrint = millis();
  }
}