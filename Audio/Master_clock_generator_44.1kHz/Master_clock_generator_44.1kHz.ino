#include <Arduino.h>
#include <ESP_I2S.h>

#define I2S_MCLK 3

I2SClass I2S;

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("ADAU1701 MCLK generator");
    Serial.println("Target MCLK: 12.288 MHz");

    /*
     * We only care about MCLK here.
     *
     * BCLK / LRCLK / DATA are assigned to unused GPIOs
     * just so the I2S peripheral can be initialized.
     */
    I2S.setPins(
        14,          // BCLK, leave physically unconnected
        15,          // LRCLK, leave physically unconnected
        22,          // DATA OUT, leave physically unconnected
        -1,          // DATA IN unused
        I2S_MCLK     // MCLK = GPIO3
    );

    bool ok = I2S.begin(
        I2S_MODE_STD,
        48000,
        I2S_DATA_BIT_WIDTH_32BIT,
        I2S_SLOT_MODE_STEREO,
        -1
    );

    Serial.printf(
        "I2S.begin(): %s\n",
        ok ? "OK" : "FAILED"
    );

    if (!ok) {
        while (true) {
            delay(1000);
        }
    }

    Serial.println("MCLK should now be running on GPIO3.");
}

void loop()
{
    /*
     * Keep the I2S peripheral active.
     *
     * Feed silence continuously.
     * BCLK/LRCLK/DATA are not connected to the DSP.
     */
    static int32_t silence[256] = {0};

    I2S.write(
        (uint8_t *)silence,
        sizeof(silence)
    );
}