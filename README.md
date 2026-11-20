# ReflowPlate ESP32

PlatformIO project for an ESP32-based reflow plate with an LVGL touchscreen and CST820 touch controller. The interface is designed for a 320 x 240 pixel landscape display.

## Project Status

The current firmware is a UI and networking prototype. Temperature, target temperature, and profile progress are simulated; real temperature sensing and heater control are not implemented. Do not use the interface to control a real heating plate.

## Features

- LVGL touchscreen interface with temperature display and a demo heating cycle
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

## Build

Open the project in VS Code with the PlatformIO extension and select one of these environments:

| Environment | Interface language |
| --- | --- |
| `esp32_ReflowPlate_Dev_DE` | German |
| `esp32_ReflowPlate_Dev_EN` | English |
| `esp32_ReflowPlate_Dev_PL` | Polish |

Build the project with **PlatformIO: Build**, then upload it to the ESP32.

## Wi-Fi Setup

Default SSID and password values are in `include/Config.h`. Enter your own credentials before flashing, and do not commit real credentials to a public repository. If no SSID is configured, the firmware starts a setup access point. Its network name, temporary password, and setup URL are printed to the serial console.

Wi-Fi and MQTT settings can also be saved through the web interface.

## Wi-Fi Firmware Updates (OTA)

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