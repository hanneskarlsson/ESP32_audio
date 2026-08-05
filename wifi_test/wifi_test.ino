#include "WiFi.h"

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Set Wi-Fi to station mode and disconnect from AP if previously connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("Starting Wi-Fi Scan...");
}

void loop() {
  int n = WiFi.scanNetworks();
  Serial.println("Scan complete.");
  
  if (n == 0) {
    Serial.println("No networks found.");
  } else {
    Serial.print(n);
    Serial.println(" networks found:");
    for (int i = 0; i < n; ++i) {
      // Print SSID, Signal Strength (RSSI), and Channel
      Serial.printf("%2d: %-32.32s (%d dBm) Ch:%d\n", 
                    i + 1, 
                    WiFi.SSID(i).c_str(), 
                    WiFi.RSSI(i), 
                    WiFi.channel(i));
      delay(10);
    }
  }
  Serial.println("------------------------------------");
  delay(10000); // Rescan every 10 seconds
}