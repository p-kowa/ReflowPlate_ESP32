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
- **Set** starts regulation at the selected temperature. The output turns off at the target, restarts 2 C below it with at least 5 seconds off between activations, and stops after 15 minutes. A missing/stale reading or overtemperature ends the session; it does not resume automatically.
- Web control uses username `ota` and the configured OTA password. Web start is unavailable without an OTA password. Use only a trusted local network.
- Before connecting heater power, verify the actuator input stays off at power-on and reset, and returns to off on **Stop**, sensor disconnection and OTA. The 260 C firmware cutoff and the cutoff at 15 C above the target are not substitutes for independent protection.

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