# ESP32 TinySine AudioB simulator

This ESP-IDF 5.4 project generates a continuous stereo 500 Hz tone while
reproducing the documented TinySine AudioB I2S master timing:

- 48 kHz sample rate
- signed 16-bit stereo PCM in the upper 16 bits of each slot
- Philips/I2S one-bit data delay
- true 24-bit slots, producing 48 BCLK periods per stereo frame
- 2.304 MHz BCLK
- 3.3 V ESP32 logic

## Default wiring

| Signal | Simulator ESP32 |
|---|---:|
| BCLK | GPIO14 |
| LRCLK/WS | GPIO15 |
| SD/data | GPIO22 |
| Ground | GND |

Connect ground between the two ESP32 boards. Do not connect their 3.3 V rails
if they are powered independently by USB.

## Build and flash

```sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The real module should still be measured before treating the 24-bit slot
assumption as final. TinySine documents 2.304 MHz BCLK and 48 kHz LRCLK, which
implies 48 bit clocks per stereo frame.
