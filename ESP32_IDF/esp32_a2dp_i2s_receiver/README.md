# ESP32 A2DP Bluetooth receiver with buffered I2S output

Standalone project for an original ESP32/ESP32-WROOM-32, targeting ESP-IDF
5.0 through 5.4. ESP-IDF 5.4.x is recommended. It uses the ESP-IDF internal
A2DP/SBC decoder and the IDF 5.x standard-mode I2S driver.

This revision includes the explicit clock-configuration compound literal
required by ESP-IDF 5.4.4.

It receives A2DP/SBC audio, uses the ESP32 Bluetooth stack's decoded PCM
callback, copies that PCM into a 32-KiB jitter buffer, and feeds I2S DMA from a
dedicated high-priority task.

## Fixed configuration

- Bluetooth name: `ESP32-I2S-Receiver`
- PCM: 44.1 kHz, signed 16-bit, stereo interleaved
- I2S mode: Philips/I2S, ESP32 is clock master
- BCLK: GPIO 14
- WS/LRCK: GPIO 15
- DATA: GPIO 22
- MCLK: not used
- Initial/recovery prefill: 20 KiB, about 116 ms
- Application PCM buffer: 32 KiB, about 186 ms

Connect ESP32 ground to the I2S DAC ground.

GPIO 15 is an ESP32 boot-strapping pin. It works as an I2S output after boot,
but an external circuit that drives or strongly pulls GPIO 15 during reset can
prevent the ESP32 from booting. Your DAC input should normally be high
impedance.

## Build and flash

Open an ESP-IDF 5.0-5.4 terminal, change into this directory, then run:

```sh
idf.py set-target esp32
idf.py build
idf.py -p YOUR_SERIAL_PORT flash monitor
```

Examples of serial ports:

- Linux: `/dev/ttyUSB0`
- macOS: `/dev/cu.usbserial-...`
- Windows: `COM5`

Exit the serial monitor with `Ctrl+]`.

Because this archive does not contain an existing `sdkconfig`, the first build
will apply `sdkconfig.defaults` automatically.

If the directory was previously built with another ESP-IDF release, run
`idf.py fullclean` before rebuilding.

ESP-IDF 5.5 introduced a different, undecoded A2DP audio-buffer interface.
This project deliberately stops at configure time on 5.5 or newer instead of
silently treating encoded SBC frames as PCM.

## Use

1. Power the ESP32 and DAC.
2. Open Bluetooth settings on the phone or computer.
3. Pair with `ESP32-I2S-Receiver`.
4. Start audio playback.
5. If a legacy PIN is requested, use `1234`.

During playback the monitor prints one line per second. At 44.1-kHz/16-bit/
stereo, `rx` should average close to `176400 B/s`.

- Increasing `under`: the I2S side ran out of buffered PCM.
- Increasing `over`: the Bluetooth side filled the entire application buffer.
- Increasing `bad`: incoming PCM packets were not aligned to four-byte stereo
  frames.
- Increasing `i2s_err`: the I2S driver returned a write error.

## Important sample-rate behavior

This build intentionally accepts only a negotiated SBC rate of 44.1 kHz. If
the Bluetooth source negotiates 48 kHz, the serial monitor reports the mismatch
and the pipeline stays silent rather than playing at the wrong speed.

## Project files

- `CMakeLists.txt`: top-level ESP-IDF project definition
- `sdkconfig.defaults`: enables Classic Bluetooth, Bluedroid, and A2DP
- `main/CMakeLists.txt`: component definition
- `main/main.c`: NVS, Bluetooth, pairing, A2DP, and diagnostics
- `main/audio_pipeline.c`: ring buffer, feeder task, and I2S DMA
- `main/audio_pipeline.h`: audio pipeline interface
