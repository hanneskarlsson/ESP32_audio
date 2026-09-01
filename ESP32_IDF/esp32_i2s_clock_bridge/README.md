# ESP32 asynchronous I2S clock bridge

This ESP-IDF 5.4 project receives TinySine-compatible I2S on I2S0 as a clock
slave, buffers the stereo PCM, and sends it to an ADAU1701 on I2S1 as clock
master. The DSP side runs at 48 kHz with a 12.288 MHz (256 x fs) MCLK.

The input and output clocks are independent. A small linear asynchronous sample
rate correction continuously follows FIFO fill, preventing the FIFO from slowly
emptying or overflowing because of normal oscillator error. Correction is
limited to +/-1500 ppm and does not change the DSP-facing 48 kHz clocks.

## Simulator/TinySine to ESP32 B

| Source | ESP32 B |
|---|---|
| BCLK | GPIO26 RX BCLK |
| LRCLK/WS | GPIO25 RX WS |
| SD/data | GPIO33 RX data |
| GND | GND |

The simulator uses GPIO14 for BCLK, GPIO15 for LRCLK, and GPIO22 for data.

## ESP32 B to ADAU1701 DSP

| ESP32 B | Sure ADAU1701 board |
|---|---|
| GPIO1 MCLK | J4 pin 2 MCLK |
| GPIO14 BCLK | J4 pin 16 MP5/BCLK_IN |
| GPIO15 LRCLK/WS | J4 pin 18 MP4/LRCLK_IN |
| GPIO22 data | J4 pin 12 MP0/SDATA_IN0 |
| GND | Any J4 odd-numbered GND pin |

Power the boards normally and connect their grounds; do not connect their 3.3 V
rails. Keep the clock wires short.

On the original ESP32, hardware MCLK is limited to GPIO0, GPIO1, or GPIO3.
GPIO0 is not used because the attached DSP clock net held the ESP32 in download
mode. GPIO3 is not used because the onboard USB-UART transmitter actively drives
it. GPIO1 is electrically safe because the USB-UART side is an input, but GPIO1
normally carries the serial-console TX signal. Once firmware enables MCLK, the
onboard serial monitor will stop receiving valid logs. Flashing remains available
after reset, before the application changes the pin function.

## Runtime telemetry

Bridge diagnostics are transmitted separately at 115200 baud so GPIO1 can remain
dedicated to MCLK:

| ESP32 B | ESP32 A |
|---|---|
| GPIO21 UART2 TX | GPIO32 UART1 RX |

This is a one-way connection. The existing common ground between the ESP32s is
also the UART reference. The simulator firmware relays each `BRIDGE` line to its
normal USB serial monitor. Do not connect GPIO3 to MCLK; it is actively driven by
the onboard USB-UART transmitter.

The ADAU1701 requires MCLK during initialization. For this firmware revision,
boot ESP32 B first and then reset or power-cycle the DSP after the log says
`DSP I2S output started`. Automatic DSP reset sequencing can be added later with
one separate, evidence-based change.

## DSP configuration

The SigmaStudio project must use a 48 kHz program and configure:

- MP0 as `SDATA_IN0`
- MP4 as `LRCLK_IN`
- MP5 as `BCLK_IN`
- serial input format as Philips I2S
- input clocks as slave/external

The bridge transmits signed 16-bit stereo PCM in the high 16 bits of two 32-bit
slots. This produces 3.072 MHz BCLK, 48 kHz LRCLK, and 12.288 MHz MCLK.

## Diagnostics

Once per second, ESP32 B sends a `BRIDGE` telemetry line reporting:

- RX and TX logical PCM bytes/second (approximately 192000 B/s at 48 kHz)
- current and observed FIFO fill
- asynchronous correction in ppm
- underrun and overrun totals plus interval deltas
- discarded frames, RX/TX errors, timeouts, and short transfers

A successful build only validates compilation. The bridge must still be flashed
and measured for at least 30 seconds after warm-up before it can be considered
stable.
