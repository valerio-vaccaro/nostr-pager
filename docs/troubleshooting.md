# Troubleshooting

[Project README](../README.md) · [Setup](setup.md)

## Build and upload

| Symptom | Check |
| --- | --- |
| `.venv/bin/pio` missing | Create `.venv` and install `requirements.txt` as described in Setup |
| Package download/DNS failure | A fresh machine needs internet; an empty local tool cache cannot compile offline |
| Board ID unknown | Run from the repository root and verify `boards/JC3248W535C_I_Y.json` exists |
| No serial port | Use a data-capable USB cable, try another USB port, and run `pio device list` |
| Port busy or permission denied | Close the serial monitor/other serial programs and verify your account can access the device |
| Upload cannot connect | Enter the bootloader with BOOT + RESET, then retry with the actual port |
| Upload unstable | Try overriding `upload_speed` with a lower value in `platformio.ini` |

## Display and touch

A serial message beginning **Display init failed** means startup stopped before networking. Check the board variant and OPI PSRAM settings. A lit blank screen can also indicate incorrect GPIOs or initialization; inspect the [hardware reference](hardware.md).

For missing or incorrect touch input, verify I²C address `0x3b`, SDA 4 and SCL 8, and compare raw coordinates with the 320×480 portrait dimensions. There is no automatic rotation or calibration. Unsupported glyphs can appear missing even when display/touch transport works.

The current interface uses a compact header, larger message previews and a **Web access** button. If the older address panel is still in the inbox, build and upload the updated firmware. Removing animations and redundant redraws reduces rendering work, but synchronous DNS/TCP/TLS connection attempts can still pause display input. Use browser Settings diagnostics to identify a failing endpoint.

## HTTPS TLS allocation failure

Repeated `mbedtls_ssl_setup returned -0x7F00`, `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` and `esp_tls_create_server_session failed` mean there was insufficient allocatable memory for a new browser TLS session.

Update the firmware: TLS now prefers the board's PSRAM instead of the SDK's internal-only heap, leaving internal RAM for tasks, Wi-Fi and DMA. Existing credentials, messages and certificates are preserved. Check Settings → **TLS memory** or the serial `TLS memory:` line for free bytes, largest contiguous blocks and failed allocations. If failures continue, capture these figures with the TLS error while the browser and relays are connected. See [allocator implementation and testing](development.md#tls-allocation-failures).

## Portal and connectivity

| Display/browser state | Action |
| --- | --- |
| Portal does not open | Stay connected to the pager's Wi-Fi and open `https://192.168.4.1/` in a full browser; HTTP only supplies instructions |
| Phone switches to mobile data | Choose to remain on the network without internet; retry the local URL |
| `pager.local` cannot be resolved | Open **Web access** on the display and use its numeric HTTPS IP URL from the same LAN; guest networks and multicast filtering can prevent mDNS discovery |
| Expired browser session / HTTP 401 | Open `/login`, enter the current display PIN, then reload the form |
| Browser certificate warning | Compare the SHA-256 fingerprint with Web details before accepting the self-signed certificate |
| HTTP 429 after PIN failures | Wait one minute, then read the current six-digit PIN on the display |
| Connection interrupted / Retrying | The browser retries automatically, with one request at a time and a ten-second request timeout. Verify the current address and Wi-Fi connection if it persists |
| Showing cached data while messaging is busy | HTTPS is serving the last snapshot while relay work is delayed. New message/status changes appear once the worker resumes; do not resubmit queued actions |
| Changed LAN address | Use the current HTTPS address on the display; renew the certificate if its IP entry is stale |
| Wi-Fi connection failed | Check SSID/password; use a 2.4 GHz network compatible with the ESP32-S3 |
| Wi-Fi connected / syncing time | Ensure the network permits DNS and NTP; relay startup waits for usable UTC time |
| Relay offline / retrying | Read web Settings → Messaging connection → Relay connection error, or the serial `[Relay]` log. A TLS code identifies a TLS failure; an HTTP status identifies a rejected WebSocket handshake. Generic DNS/TCP/TLS failures require checking the configured hostname, internet connection and port |
| Relay requires authentication | Configure a private key for NIP-42 authentication, or use a relay that permits public-key access |

Edit additional public and backup DM inbox lists on **Relays** without restarting or ending the browser session. Changing the primary relay, Wi-Fi or identity uses **Settings → Wi-Fi and Nostr setup** and restarts after saving. On the display, **Back** resumes saved settings without saving browser form edits.

## Messaging

| Symptom | Explanation / action |
| --- | --- |
| No DMs with an `npub` identity | Public-key mode cannot decrypt DMs; configure a private key for that identity |
| Another client shows a DM but the pager does not | Legacy kind 4 and text-only, two-person NIP-17 DMs are supported. Check the sender uses the pager's configured inbox relay and the same recipient identity; check Settings → DM relay publication results |
| Connected but inbox empty, without an error | Reload web Settings → Messaging connection after a test. Check the actual inbox subscription status and event counts; a metadata relay response alone does not confirm the inbox is subscribed |
| Public subscription works but DMs do not | Check the separate Private DM subscription status and its refusal reason. Configure the recipient identity's private key and check that the sender uses a listed DM inbox relay |
| Public notes missing | Incoming public notes must tag the configured public key; this is not a general public feed |
| New/Reply disabled | The configured identity is public-only |
| Enter a valid public recipient key | Use public hex, `npub`, `nprofile`, a supported `nostr:` link, or an unambiguous saved contact name. Unknown names and private keys are rejected; the contact picker avoids ambiguous names |
| Use 1–512 bytes of text | Shorten the message; UTF-8 characters may occupy multiple bytes |
| Wait for the pending relay reply | One outgoing event is already pending; wait or configure a compatible relay |
| Relay rejected | Read the displayed reason; relay policies can refuse publication |
| History cache full | The device has reached 60 entries; further older-history requests are blocked |

Malformed, incorrectly signed, unrelated, oversized or undecryptable events are skipped rather than displayed. A public/legacy event over 3072 serialized bytes, a modern wrapper over 12288 bytes, or a relay text frame over 16384 bytes is outside the application's receive limits. Valid events more than ten minutes in the future are also skipped.

Messaging connection now breaks received events down into public notes, legacy kind-4 DMs and modern kind-1059 gift wraps, and counts added messages separately from duplicates. **Last skipped event** and `[Inbox] skipped kind=...` serial logs identify the failed stage: private key missing, wrong recipient, envelope/seal signature, NIP-44 or NIP-04 decryption, rumor ID/sender, unsupported group/file message, text length, deletion filter or full cache. Message plaintext and keys are not logged. Zero received DM events points to transport, subscription, identity or sender relay routing; a rising skipped count identifies a receive/validation problem. A successful kind-10050 metadata readback alone does not establish that a DM was delivered to that inbox.

## DNS failures across relays

`hostByName(): DNS Failed for ...` means the hostname could not be resolved before a TCP/TLS connection was attempted. If several unrelated relay names fail while `TLS memory:` reports free memory and zero failed allocations, check the station's network and DNS servers. Wi-Fi association alone does not establish internet access.

The firmware logs `[DNS] station=... gateway=...; main=... backup=... fallback=...` and includes these fields in Settings → Messaging connection. It keeps valid DHCP-provided main/backup DNS addresses and fills missing slots with `1.1.1.1`/`8.8.8.8`. Addresses pointing to the pager itself, loopback or the captive access point are repaired. Three DNS failures involving at least two hosts within a minute trigger a public-resolver attempt and reset relay backoff without clearing the outbox or browser sessions. Further resolver changes are at least a minute apart. A successful hostname resolution resets the failure burst; a single invalid relay hostname does not trigger global DNS changes. A new DHCP-provided primary resolver is honored.

Update and upload, then capture the `[DNS]` lines and Settings diagnostics if failures continue. From another device on the same Wi-Fi, try `nslookup relay.damus.io` and check internet access. Guest-network sign-in, a missing upstream route, blocked DNS traffic or a firewall can prevent both router and public resolvers from working; firmware fallback cannot supply internet access in those cases. Do not replace relay hostnames with hardcoded IP addresses or disable certificate verification.

## Storage

**History storage unavailable**, **History write failed**, **History save failed**, or **Could not persist message** indicate a filesystem problem. Sending requires successful persistence before publication. A normal restart does not deliberately erase initialized storage.

Setup has red **Reset** and **Delete data + reset** controls, each requiring a second confirmation. Reset preserves data; deletion clears Wi-Fi credentials, the Nostr identity, messages/outbox, contacts and the HTTPS certificate. Preserve independently held keys and needed data before confirming deletion. Firmware remains installed; first-time setup opens afterwards. A deletion failure is shown on the display; restart to retry the durable pending wipe. Do not use filesystem upload as a routine recovery step: it can replace the history partition.

## DM inbox relay warning

If a client says DM inbox relays were not found, follow [Missing DM inbox list](relays.md#missing-dm-inbox-list).
