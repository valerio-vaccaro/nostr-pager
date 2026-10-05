# Setup and flashing

[Project README](../README.md) · [Troubleshooting](troubleshooting.md)

## Prerequisites

Use the JC3248W535C_I_Y board, a data-capable USB cable, and a computer with Python 3 and virtual-environment support. The commands below use a Linux shell and must run from the repository root. Native tests additionally require GCC and G++.

The configured hardware has 16 MB flash and 8 MB OPI PSRAM. Check [hardware](hardware.md) before using a different variant.

## Install and compile

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pio --version
.venv/bin/pio run
```

`requirements.txt` pins PlatformIO. `platformio.ini` selects the `JC3248W535C_I_Y` environment and custom board definition. Platform/tool downloads are cached in `.build-cache/platformio`; build products go into `.pio/build/JC3248W535C_I_Y/`.

The current workspace already has `.venv` and cached tool packages. A fresh checkout needs internet for installation. The libraries in `lib/` need no separate downloads.

A successful build produces `firmware.bin`, `firmware.elf`, `bootloader.bin`, and `partitions.bin`. Use PlatformIO's upload command to flash the required images at their correct offsets.

## Upload and monitor

```sh
.venv/bin/pio device list
.venv/bin/pio run -t upload --upload-port /dev/ttyACM0
.venv/bin/pio device monitor --port /dev/ttyACM0 --baud 115200
```

Replace the port with the detected device; it may be `/dev/ttyUSB0`. Exit the serial monitor before another upload. If the board will not enter its bootloader, hold BOOT, press and release RESET, then release BOOT and retry uploading.

No `uploadfs` step is needed. On first boot, the application formats an uninitialized LittleFS partition. Once initialized, a mount failure is not automatically repaired by formatting.

Flashing a different partition layout can invalidate existing firmware or data. Preserve any needed device data before changing it. The reserved second application partition does not imply that this firmware has an OTA-update feature.

## Configure from a phone

1. Power or reset the board. Without a saved identity/network, the screen enters configuration mode.
2. Join the displayed `Nostr-Pager-xxxx` network using its displayed password. The password changes when a new setup session starts.
3. Stay on the pager's network if your phone reports no internet. Open **https://192.168.4.1/** in a full browser. The HTTP captive page provides these connection instructions.
4. Tap **Web details** on the display, compare the certificate SHA-256 fingerprint with the browser certificate, then accept the self-signed certificate.
5. Enter the six-digit PIN shown on the display. Choose **Settings → Wi-Fi and Nostr setup**.
6. Complete the form and select **Save and connect**.
7. Wait for restart, Wi-Fi connection, UTC time synchronization and relay connection. Tap **Web access** in the inbox for the HTTPS address; open **Setup** on the display to read the new PIN.

| Field | Meaning |
| --- | --- |
| Wi-Fi SSID | Network name, up to 32 bytes; surrounding whitespace is trimmed |
| Wi-Fi password | Up to 64 bytes; nonempty passwords must have at least 8 bytes |
| Open network | Explicitly clears the Wi-Fi password |
| Nostr key | Public or private identity in a supported format, up to 512 bytes |
| Raw hex key type | Select public/private for a hex identity; Bech32 prefixes identify their own type |
| Relay URL | One `wss://` relay URL, up to 200 bytes |
| Additional public relays | Up to six `wss://` URLs, one per line |
| Backup DM inbox relays | Up to six `wss://` URLs, one per line; private-key mode announces and monitors them |

A blank password preserves the saved password only when the SSID is unchanged. For a new SSID, enter a password or select **Open network**. A blank Nostr-key field preserves the saved identity; first setup requires a key. Saved passwords and keys are never prefilled into the page.

The primary default is `wss://relay.damus.io`; [Relay connections](relays.md) covers the additional public and backup DM endpoints. For modern DMs, inboxes must accept kind 1059 and kind 10050.

## Reconfigure and recover

Tap **Setup** or hold BOOT for three seconds during normal operation. Setup keeps relay operation active while connected to the saved Wi-Fi network. **Back** closes the portal and returns to the inbox, reconnecting with saved settings if needed; it is disabled before an identity has been configured.

Saving restarts the device after approximately 1.5 seconds. Wi-Fi failure lasting approximately 20 seconds brings back the configuration network. If a network later recovers while the portal is open, use **Back** or save settings to resume normal operation.

Browser login and certificate regeneration are described in [Browser interface](web-interface.md).
