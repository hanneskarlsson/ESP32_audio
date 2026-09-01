# ESP32 I2S clock bridge — input validation stage

This ESP-IDF 5.4 firmware currently validates the TinySine-compatible input.
The DSP-facing I2S transmitter remains disabled until input capture passes.

## Wiring from the simulator ESP32

| Simulator | Bridge/validator |
|---|---|
| GPIO14 BCLK | GPIO26 RX BCLK |
| GPIO15 LRCLK/WS | GPIO25 RX WS |
| GPIO22 SD/data | GPIO33 RX data |
| GND | GND |

Power both boards from their own USB ports and connect their grounds. Do not
connect the 3.3 V rails together.

## Expected diagnostics

- approximately 192000 PCM bytes/second
- approximately 384000 raw DMA bytes/second (24-bit samples use 32-bit words on ESP32)
- approximately 48000 stereo frames/second
- approximately 500 Hz from positive-going zero crossings
- RMS approximately 5792 for the simulator's 25% amplitude sine
- peak approximately 8192
- DC near zero
- zero stereo mismatches, clipping, short reads, and read errors

If every report shows zero bytes and a rising timeout count, BCLK/LRCLK is
missing or the boards are wired to the wrong GPIOs.

## Future output defaults

The provisional DSP-facing configuration remains 48 kHz with 12.288 MHz MCLK,
but it is not enabled in this validation firmware.
