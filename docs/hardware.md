# Hardware reference

[Project README](../README.md)

## Board configuration

The target is `JC3248W535C_I_Y`, defined in [boards/JC3248W535C_I_Y.json](../boards/JC3248W535C_I_Y.json).

| Setting | Value |
| --- | --- |
| MCU / CPU clock | ESP32-S3 / 240 MHz |
| Flash / flash clock | 16 MB / 80 MHz |
| PSRAM | 8 MB OPI, `qio_opi` Arduino memory configuration |
| Display | AXS15231B, 320 × 480, fixed portrait orientation |
| Display interface | QSPI at 32 MHz |
| Touch interface | I²C at 400 kHz, address `0x3b` |
| Serial monitor / upload | 115200 / 921600 baud |

These settings describe the intended variant and have passed compilation. They have not been confirmed on physical hardware in this workspace.

## GPIO mapping

| Signal | GPIO |
| --- | --- |
| LCD chip select | 45 |
| LCD clock | 47 |
| LCD D0 | 21 |
| LCD D1 | 48 |
| LCD D2 | 40 |
| LCD D3 | 39 |
| Backlight | 1 |
| Touch SDA / SCL | 4 / 8 |
| Touch interrupt | 3 |
| BOOT / setup gesture | 0 |

Pin constants are in [include/board.h](../include/board.h). The touch interrupt is configured as an input, but touch samples are polled. No buzzer or vibration pin is configured.

## Display and touch implementation

[src/board.cpp](../src/board.cpp) registers LVGL display and pointer-input callbacks. LVGL updates a PSRAM RGB565 framebuffer; the driver transmits the full frame after the final flush of a redraw, using an internal DMA staging buffer. RGB565 bytes are swapped for transmission.

The panel initialization table and its source notice are in [include/panel_init.h](../include/panel_init.h). Touch coordinates are read from the AXS15231B and checked against the portrait dimensions. There is no calibration menu or rotation setting.

If using a variant, verify controller, flash/PSRAM type, GPIOs, initialization sequence and touch axes before editing these files. A successful compilation does not establish that a different panel is compatible.

## Flash partitions

The layout is defined in [partitions.csv](../partitions.csv).

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | `0x5000` | Wi-Fi/identity configuration and filesystem initialization marker |
| `otadata` | `0xe000` | `0x2000` | OTA selection metadata reservation |
| `app0` | `0x10000` | `0x600000` | Primary application slot |
| `app1` | `0x610000` | `0x600000` | Second application slot reservation |
| `littlefs` | `0xc10000` | `0x3e0000` | Message-history files |
| `coredump` | `0xff0000` | `0x10000` | Core-dump reservation |

OTA updating and a core-dump viewing interface are not implemented by the application. Changing this layout can make existing data unusable.
