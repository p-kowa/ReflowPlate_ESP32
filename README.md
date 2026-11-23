# ReflowPlate ESP32

PlatformIO project for an ESP32-based reflow plate with an LVGL touchscreen and CST820 touch controller. The interface is designed for a 320 x 240 pixel landscape display.

## Project Status

The firmware reads the plate temperature with a MAX6675 and supports fixed-setpoint heating. Reflow profiles and real-load commissioning are not complete.

## Features

- LVGL touchscreen interface with temperature display and fixed-setpoint heating
- Wi-Fi connection and setup access point when no Wi-Fi credentials are configured
- Web interface and HTTP endpoints for status, Wi-Fi, and MQTT configuration
- German, English, and Polish interface languages
- Wi-Fi and MQTT settings persisted in ESP32 NVS

## Hardware and Dependencies

- ESP32 Dev Module (`esp32dev`)
- TFT display supported by the project's TFT_eSPI configuration
- CST820 touch controller
- PlatformIO with Espressif32 platform package 7.1.3
- LVGL and TFT_eSPI are included in `lib/`; PlatformIO resolves other required libraries.

The touch and display pins and the display configuration must match your hardware. TFT_eSPI settings are in `lib/TFT_eSPI/User_Setup.h`.

## Heater commissioning

- GPIO26 drives an active-low actuator input: `HIGH` is off, `LOW` is on. The external driver must hold the heater off while the ESP32 is unpowered or resetting; the plate also needs an independent thermal cutoff.
- **Set** starts regulation at the selected temperature. Three values come from a table of support points (see MQTT Commands), linearly interpolated by the target and taken from the nearest point outside the table: `off` (switch-off point below the target), `hyst` (re-heat distance below that) and `lag`. Defaults: 80 C: 9/1/28, 150 C: 5/1/28, 220 C: 1/1/28. The heater stays on until the reading reaches the switch-off point, or earlier once the reading plus `lag` seconds of its current rise reaches the target (`lag` 0 disables this). After that, the heater turns back on once the reading falls `hyst` below the switch-off point (at least 20 seconds after it turned off), and turns off once the reading is rising and above that re-heat point (after at least 15 seconds), or reaches the switch-off point (at most 120 seconds).
- Regulation decisions and the rise/drop checks use the mean of the last 5 seconds of readings (sampled every 0.5 seconds). "Rising" means that mean is at least 0.1 C above the mean 5 seconds earlier, which in turn is at least 0.1 C above the one 10 seconds earlier. The overtemperature and sensor checks use each raw reading.
- The session ends with an error on a missing/stale reading, overtemperature, reaching the heating time limit, less than 1 C rise above the lowest reading per 30 seconds of heating (from 45 seconds; when re-heating 0.5 C, from 75 seconds), or a reading 2 C below its maximum while heating (from 30 seconds). It does not resume automatically. These are software checks based on one sensor, not independent protection.
- Tap the gear button at the top right of the display to choose the heating time limit (the house button returns to the main page): tap 15, 30 or 45 minutes, or press and hold **Unlimited** to disable the limit. The web page offers 15, 30 and 45 minutes as well; **Unlimited** can only be chosen on the device. The limit applies immediately, including to a running session; choosing a limit shorter than the elapsed time stops it with an error. It is not saved: every restart returns to 15 minutes. Unlimited heating removes the time-based cutoff, so the plate needs supervision and independent overtemperature protection.
- The Wi-Fi button at the top right opens the system page. Press and hold to toggle the password for the web page, web control and firmware updates via `/update` (saved; also available as a checkbox on the web page) or to reset Wi-Fi and restart into the setup access point. Without the password, anyone on the network can control the plate and flash firmware. ArduinoOTA (`espota`) always keeps the password.
- Web control uses username `ota` and the configured OTA password. Web start is unavailable without an OTA password. Use only a trusted local network.
- Before connecting heater power, verify the actuator input stays off at power-on and reset, and returns to off on **Stop**, sensor disconnection and OTA. The 260 C firmware cutoff and the cutoff at 15 C above the target are not substitutes for independent protection.

## Status LED

The active-low RGB LED uses GPIO4 red, GPIO16 green and GPIO17 blue. Blue means cold or waiting; green means regulation is running below the target, including pauses between pulses; red means the running controller has reached its target. Red and blue mean regulation is stopped and the plate is at least 40 C, not necessarily that the temperature is currently falling. Red and green indicate a sensor, watchdog or regulation fault. The LED is not an independent safety indicator.

## MQTT Monitoring

Configure the address, port, optional credentials and base topic of an existing MQTT broker on the setup page. The broker must be reachable by both the ESP32 and the monitoring computer. The default status topic is `reflowplate/status`.

The device publishes approximately once per second with QoS 0, without retained messages or buffering during disconnection. JSON fields are `uptime_ms`, `actual` (C, or `null` for an invalid or stale reading), `target` (C), `running`, `heater` (output active), and `state` (`cold`, `heating`, `target`, `cooling`, or `error`), `reason` (why the state is `error`: `sensor`, `overtemp`, `limit`, `drop`, `progress` or `watchdog`; otherwise `null`; also shown on the display), and `pulse_ms` (length of the current or last pulse), `smooth` (5 s mean used by the regulation), `trend` (change of that mean over the last 5 s), and `limit_min` (heating time limit in minutes, `0` = unlimited), `touch_err` (failed touch controller reads since boot) and `touch_rej` (touch samples discarded as interference since boot), `touch_x`/`touch_y` (last accepted touch point in screen pixels, `-1` before the first touch); `limit_min` and the touch fields are not written to the CSV. The `heater` field describes the GPIO output, not proof that the relay or heater actually switched.

Install the Python client and run the monitor, replacing the broker address:

```powershell
python -m pip install "paho-mqtt>=2,<3"
python tools/mqtt_monitor.py 192.168.10.10 --csv reflow-log.csv
```

Use `--user USERNAME` to prompt for the broker password and `--topic` if the base topic was changed. Stop with Ctrl+C. MQTT uses unencrypted TCP in this configuration; use a trusted network. The monitor timestamps received data locally, and missing records during disconnections are not recovered. It refuses to append to a CSV file with a different column layout.

### MQTT Commands

The device listens on `<base>/set` (default `reflowplate/set`) for plain-text commands: `stop`, a target temperature such as `50` (same limits as the touch screen), or `pulse N` for a single test pulse of 1-15 seconds. Tuning table (saved in NVS, up to 8 points, 0-230 C):

- `set T off N` / `set T hyst N` / `set T lag N`: set a value at point `T`; a new point starts with the interpolated values. Ranges: `off` 0-30 C, `hyst` 0.25-10 C, `lag` 0-60 s; decimals allowed (e.g. `set 160 lag 31.5`).
- `del T`: remove point `T` (the last point cannot be removed).
- `table`: publish the table to `<base>/table`; also published after every change. `table reset`: restore the defaults.

The status shows the values in use for the current target as `off` (switch-off temperature), `hyst` and `lag`. To watch the table: `python tools/mqtt_monitor.py 192.168.10.10 --topic reflowplate/table`. A test pulse is rejected while regulation is running and until the reading has not risen for 40 seconds. Retained messages are ignored, and invalid or rejected commands are dropped silently; check `reflowplate/status`. Any client that can reach the broker can heat the plate, so only use a broker on a trusted network and only send heating commands while someone supervises the plate.

```powershell
python tools/mqtt_command.py 192.168.10.10 "pulse 5"
python tools/mqtt_command.py 192.168.10.10 50
python tools/mqtt_command.py 192.168.10.10 stop
```

## Build

Open the project in VS Code with the PlatformIO extension and select one of these environments:

| Environment | Interface language |
| --- | --- |
| `esp32_ReflowPlate_Dev_DE` | German |
| `esp32_ReflowPlate_Dev_EN` | English |
| `esp32_ReflowPlate_Dev_PL` | Polish |

Build the project with **PlatformIO: Build**, then upload it to the ESP32.

## Wi-Fi Setup

Default SSID and password values are in `include/Config.h`. Enter your own credentials before flashing, and do not commit real credentials to a public repository. If no SSID is configured, the firmware starts a setup access point. Its network name, temporary password, and setup URL appear on the device display and in the serial console. Join the displayed network and open the setup URL to configure Wi-Fi again after a reset.

Wi-Fi and MQTT settings can also be saved through the web interface.

## Wi-Fi Firmware Updates (OTA)

### Browser upload (no incoming PC connection)

1. Set `OTA_PASSWORD` in local `include/Config.h` and install this firmware once via USB-TTL using `esp32_ReflowPlate_Dev_DE` (or via ArduinoOTA from a machine that permits the TCP callback). Keep the heater disconnected.
2. Once the device joins Wi-Fi, open `http://<device-ip>/update`. Sign in as `ota` with the password configured on the device. Use only a trusted local network: HTTP does not encrypt the password or firmware.
3. Select an existing PlatformIO `firmware.bin` from `.pio/build/<environment>/` and click **Aktualisieren**. Do not select `bootloader.bin` or `partitions.bin`. A successful upload is followed by an automatic reboot. The page uploads an existing file; it does not start a build.

### PlatformIO upload (ArduinoOTA)

1. In your local `include/Config.h`, add `#define OTA_PASSWORD "your-own-long-random-password"` with a unique password. An empty password disables OTA. Use OTA only on a trusted local network.
2. With the heater disconnected, upload **once over USB-TTL** using `esp32_ReflowPlate_Dev_DE`. This installs the new `min_spiffs.csv` partition table with two 1,920 KiB firmware slots; OTA cannot migrate the old `huge_app.csv` layout. A normal serial upload preserves NVS; do not erase the flash.
3. Reboot normally and wait for the ESP32 to join Wi-Fi. Its display shows the IP address, and the serial monitor reports `OTA: reflowplate.local (...)`.
4. For subsequent uploads, run the following in a PlatformIO PowerShell terminal. Enter the same OTA password when prompted. If mDNS does not work, append `--upload-port 192.168.1.50` to `pio run`, substituting the IP shown on the display.

```powershell
$otaPassword = Read-Host 'OTA password' -AsSecureString
try {
	$env:PLATFORMIO_UPLOAD_FLAGS = '--auth=' + [pscredential]::new('ota', $otaPassword).GetNetworkCredential().Password
	pio run -e esp32_ReflowPlate_OTA_DE -t upload
} finally {
	Remove-Item Env:PLATFORMIO_UPLOAD_FLAGS -ErrorAction SilentlyContinue
}
```

The uploader's diagnostic output can include the password; redact it before sharing logs. If the device answers the OTA invitation but the transfer times out, check that the PC firewall permits the uploader's incoming TCP connection from the ESP32. Keep the USB-TTL adapter as a recovery option if Wi-Fi is unavailable.

## Licenses

The original project code is licensed under the MIT License (see `LICENSE`). Bundled libraries include their own license files, including those in `lib/lvgl/` and `lib/TFT_eSPI/`; their terms apply separately.