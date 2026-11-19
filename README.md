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

## Licenses

The original project code is licensed under the MIT License (see `LICENSE`). Bundled libraries include their own license files, including those in `lib/lvgl/` and `lib/TFT_eSPI/`; their terms apply separately.