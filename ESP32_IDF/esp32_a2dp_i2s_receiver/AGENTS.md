# Project goal

Produce uninterrupted Bluetooth A2DP audio on the ESP32-WROOM-32 I2S pins.
The primary failure to eliminate is clicking/Morse-code audio caused by
repeated PCM starvation.

# Fixed configuration

- ESP-IDF 5.4.4
- Target: ESP32-WROOM-32
- PCM: 44100 Hz, signed 16-bit, stereo interleaved
- I2S: Philips format, ESP32 clock master
- BCLK: GPIO14
- WS/LRCK: GPIO15
- DATA: GPIO22
- The phone supplies a continuous 500-Hz Bluetooth test tone.

# Build and hardware

- Build with: idf.py build
- Serial port: /dev/ttyUSB0
- Flash with: idf.py -p /dev/ttyUSB0 flash
- Only one process may open the serial port.
- Ask the user to reconnect or restart phone playback when required.

# Acceptance criteria

After a 10-second prefill/warm-up period, observe at least 30 seconds of audio:

- underrun delta = 0
- overrun delta = 0
- bad packets = 0
- I2S errors = 0
- no A2DP sequence-number errors
- average PCM input is close to 176400 bytes/second
- ring-buffer fill does not repeatedly reach zero
- verified I2S or analog capture contains no dropouts

# Working rules

- Change one variable at a time.
- Build after every source change.
- Flash and collect a fresh labeled log after every experiment.
- Preserve logs under test-logs/.
- Do not claim success based only on compilation.
- Do not endlessly increase buffering to hide missing Bluetooth packets.
- If transport sequence errors persist, investigate RF conditions, antenna
  placement, power integrity, Bluetooth-source behavior and logging overhead.