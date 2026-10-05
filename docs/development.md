# Development and validation

[Project README](../README.md) · [Architecture](architecture.md)

## Source map

| File | Responsibility |
| --- | --- |
| [platformio.ini](../platformio.ini) | Build environment, memory configuration and build flags |
| [boards/JC3248W535C_I_Y.json](../boards/JC3248W535C_I_Y.json) | Custom PlatformIO board definition |
| [src/main.cpp](../src/main.cpp) | Startup, portal, configuration, relay messages and history |
| [src/board.cpp](../src/board.cpp) | QSPI framebuffer transport and touch input |
| [src/ui.cpp](../src/ui.cpp) | Inbox, reader, composer and setup screens |
| [src/web_pager.cpp](../src/web_pager.cpp) | HTTPS service, sessions, certificate lifecycle and queued pager operations |
| [src/tls_memory.cpp](../src/tls_memory.cpp) | Shared HTTPS/relay TLS allocator and heap diagnostics |
| [src/dns_health.cpp](../src/dns_health.cpp) | Station DNS diagnostics, DHCP-preserving fallback and retry recovery |
| [include/inbox_requests.h](../include/inbox_requests.h) | Shared, independently testable public/private inbox request filters |
| [include/web_auth.h](../include/web_auth.h) | Testable PIN/session validation |
| [include/web_bridge.h](../include/web_bridge.h) | Immutable snapshots and bounded asynchronous browser-operation lifetime |
| [src/brand.cpp](../src/brand.cpp) | Scalable LVGL pager logo |
| [src/key_codec.cpp](../src/key_codec.cpp) | Key/URI decoding and relay URL validation |
| [src/nostr_crypto.cpp](../src/nostr_crypto.cpp) | Event hashing, signatures, NIP-04 and NIP-59 envelopes |
| [src/nip44.cpp](../src/nip44.cpp) | Bounded NIP-44 v2 encryption/decryption |
| [src/dm_relay.cpp](../src/dm_relay.cpp) | Relay-list announcements, recipient routing and NIP-42 |
| [src/contacts.cpp](../src/contacts.cpp) | Durable contacts, short-name resolution and verified profile discovery |
| [include/pager.h](../include/pager.h) | Shared message model and pager/UI interfaces |
| [include/lv_conf.h](../include/lv_conf.h) | LVGL memory, color, tick and font settings |
| [scripts/test_native.py](../scripts/test_native.py) | Host protocol-test build and execution |
| [scripts/preview_ui.py](../scripts/preview_ui.py) | Actual LVGL screen rendering with sample data |
| [scripts/make_ca_bundle.py](../scripts/make_ca_bundle.py) | Trust-root header generation |
| [scripts/package_firmware.py](../scripts/package_firmware.py) | Export flash images and a DIYFlasher manifest |

Dependencies live in `lib/`; retain their license notices and the documented local patches when updating them.

The vendored `WebSocketsClient::connectFailedCb()` emits `WStype_ERROR` with separate ESP32 DNS, TCP/setup and TLS failure reasons. Optional weak `websocketsDNSFailed`/`websocketsDNSResolved` hooks feed the station DNS recovery policy. This classification relies on the pinned Arduino 2.0.14 `WiFiClientSecure::connect(host,...)` behavior: a DNS failure leaves the newly constructed client's `lastError()` at zero, while TCP/TLS failures update it. Recheck that behavior when upgrading the SDK. ESP32 TLS handshakes are bounded to ten seconds. The pager preserves disconnect/HTTP-handshake reasons in browser Settings and serial logs. Retain these patches when updating arduinoWebSockets; upstream debug-only output does not provide them to the application.

## Compile and run host checks

From the repository root:

```sh
.venv/bin/pio run
python3 scripts/test_native.py
python3 scripts/test_web.py
python3 scripts/test_modern.py
python3 scripts/test_tls_memory.py
python3 scripts/test_dns.py
```

The native script builds uBitcoin and the key parser with GCC/G++, writes objects and the executable to `.build-cache/native`, then runs assertions. It uses the system Python, so it does not require PlatformIO or an attached board.

`scripts/test_dns.py` compiles the production DNS health module against a simulated station and ESP-NETIF API. It checks preservation of working DHCP DNS, filling missing slots, rejecting self/loopback resolver addresses, cross-host failure detection, cooldown/rollover, public-resolver rotation, successful-lookup reset, DHCP renewal and API failures. Firmware changes use `esp_netif_set_dns_info()` on the station interface; they do not call `WiFi.config()` or change IP/DHCP mode. Verify physical recovery and upstream resolver reachability separately with the `[DNS]` serial log.

`scripts/test_web.py` also exercises the production cross-core bridge with multiple reader threads and a simulated blocked messaging worker. It checks that cached reads and operation polling continue, snapshots stay coherent and immutable, another session cannot read a result, browser disconnects do not destroy queued jobs, pending capacity is bounded, and only completed jobs can be expired/replaced across timer rollover. The browser test checks stale-snapshot notices. Core affinity and real HTTPS responsiveness still require the physical ESP32-S3: make relays temporarily unavailable, navigate several protected pages, submit one action and check its queued/completed page. The startup log should show HTTPS on core 0 and the pager/relay worker on core 1.

Coverage includes official NIP-19 public/private/profile fixtures, key/URI parsing, checksum mutations, invalid scalar ranges, relay URLs, all 15 BIP-340 vectors with 32-byte messages, exact signatures for four signing fixtures, rejection of invalid compressed public keys, and a raw ECDH fixture. Auxiliary randomness is replaced with published fixture data only in the native test executable.

Modern tests also exercise the production inbox-request builder used by primary and rotating connections: independent public/private subscriptions, recipient/sent-copy filters, a 200-envelope three-day modern replay window and older-history boundaries. Relay tests check that a rejected DM subscription preserves public state, wrong-channel events are ignored, and authentication retries both requests. Crypto tests check diagnostic stage reporting for wrong identities, wrong decryption keys and invalid signatures while preserving the verification rules.

These checks do not exercise the LVGL interface, physical transport, portal, LittleFS, TLS handshake, live relay behavior or ESP32 AES implementation. A firmware build establishes compilation/linking, not hardware behavior.

## TLS allocation failures

`mbedtls_ssl_setup returned -0x7F00` means `MBEDTLS_ERR_SSL_ALLOC_FAILED`. The Arduino SDK used by this build selects `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`: its default TLS allocator requires internal RAM, regardless of the general `heap_caps_malloc_extmem_enable()` threshold. Concurrent HTTPS sessions and relay connections can exhaust or fragment that heap even with free PSRAM.

Startup installs the supported `mbedtls_platform_set_calloc_free()` hook before certificates, Wi-Fi and TLS connections. TLS allocations prefer zeroed 8-bit PSRAM and fall back to zeroed internal RAM; both use `heap_caps_free()`. This applies to the precompiled mbedTLS library used by both the HTTPS server and relay clients. The SDK also supports external TLS allocation directly; see [Espressif's allocator implementation](https://github.com/espressif/esp-idf/blob/v4.4.6/components/mbedtls/port/esp_mem.c). Changing a build macro alone would not rebuild this precompiled SDK.

Settings shows free heap and largest free block for internal RAM and PSRAM, plus fallback and failed-allocation counters. Serial logs print these every minute. `scripts/test_tls_memory.py` compiles the production allocator against simulated heaps and checks preference, zeroing, fallback, overflow rejection and failure statistics. After flashing, validate simultaneous relay reception and browser navigation on the actual board, including repeated page reloads. A build or host test cannot establish the available heap during real TLS handshakes. Configuration, keys and certificates need no erase or regeneration for this change.

The initial implementation passed both checks. Its firmware used approximately 1.35 MB of the 6 MB application slot and 49.8 KB of static RAM; these figures exclude dynamic allocations and PSRAM usage.

## Firmware version labels

The PlatformIO pre-build hook [scripts/build_version.py](../scripts/build_version.py) generates `.build-cache/build_version.h` from a tag pointing at the latest commit. Untagged commits use their short commit ID; repositories without commits use `dev`. `NOSTR_PAGER_VERSION` can supply an explicit label. The value appears on the display, web pages, status API and startup serial log.

`python3 scripts/test_version.py` tests this against isolated temporary repositories, including tags, commits, overrides and the no-commit fallback. It never modifies the project repository.

The [firmware action](../.github/workflows/firmware.yml) builds the default board and uploads `dist/`. `python3 scripts/package_firmware.py` creates the same export locally after `pio run`. The manifest is an array containing the board, version and four images with flash offsets `0x0`, `0x8000`, `0xe000` and `0x10000`. Its URLs are relative to the manifest, so keep the JSON and `<version>_<board>` folder together when adding them to DIYFlasher. The website also needs a Nostr Pager entry to load this new manifest.

## Render the interface locally

Install GCC/G++ and Pillow, then run from the repository root:

```sh
.venv/bin/pip install Pillow
.venv/bin/python scripts/preview_ui.py
```

If Pillow is already installed for the system Python, `python3 scripts/preview_ui.py` also works. The script compiles the production `src/ui.cpp`, `src/brand.cpp` and vendored LVGL, with a small host Arduino shim and synthetic pager data. It caches objects in `.build-cache/preview` and writes PNGs plus [a contact sheet](../assets/previews/overview.png) under `assets/previews/`. It checks inbox/reader navigation, cache boundaries, selection across new arrivals and eviction, draft preservation, setup-only PIN visibility and reset confirmations. It also asserts zero display flushes for unchanged message data, correct selection when an arrival races a tap, and reader scroll preservation across arrivals.

The preview fixture contains sample credentials; do not replace them with a live device PIN or Wi-Fi password when producing documentation images. Live browser pages require a reachable pager and are a separate capture step.

The preview exercises opening a message, replying, entering a keyboard character, opening setup/web details and displaying public-key mode. It asserts that the keyboard stays within the display. It uses no Wi-Fi, keys or relay connections. Preview rendering does not establish physical touch accuracy or live message delivery.

## Modern DM checks

`python3 scripts/test_modern.py` requires GCC/G++, OpenSSL development headers/library and system Python. It compiles the production crypto and relay modules with host Arduino/WebSocket adapters. Coverage includes a published NIP-44 encryption vector, MAC/version tampering, padding boundaries, UTF-8 validation, gift-wrap sender/recipient checks, preservation of existing relay-list entries, NIP-42 authentication, recipient relay failover, missing-list retry and reconnect. It uses synthetic identities only.

Additional relay tests cover primary-offline metadata publication and recipient delivery, public/backup-inbox role separation, canonical URL deduplication, per-endpoint failure isolation/backoff, authentication, public publication failover, reconnect retention and cancellation. `scripts/test_web.py` also exercises the production deletion policy with persistence failures, marker exhaustion, a monotonic all-history cutoff and future-dated messages. LVGL previews exercise confirmation/cancellation for single/all message deletion and the display Reconnect action.

Recipient-routing regressions also verify immediate progress after EOSE supplies a usable signed inbox list, fallback progress without EOSE, and rejection of a delivery acknowledgement arriving on a discovery connection.

For independent interoperability checks, install pinned test dependencies and rerun with the oracle enabled:

```sh
npm install --prefix scripts/native_fixtures --ignore-scripts
NOSTR_ORACLE_MODULES="$PWD/scripts/native_fixtures/node_modules" python3 scripts/test_modern.py
```

The oracle uses Node/OpenSSL and noble secp256k1 to generate incoming fixtures and decrypt firmware-created wrappers. It checks rejection of forged sender identities, group messages and incorrect rumor IDs. It also generates signed seals with valid, expired or malformed expiration tags, unsupported tags, missing/mismatched seal IDs, invalid signatures and tampered signed tags. These check expiration compatibility and precise diagnostics without weakening authentication. Browser tests exercise removal of expired inbox/reader text; a production cache-policy test checks exact expiration boundaries. These are host protocol checks, not evidence of live relay availability or physical delivery.

## Browser and access-control checks

`python3 scripts/test_web.py` requires GCC/G++ and Node.js. It checks PIN length/digits, wrong-PIN rejection, global cooldown, token/CSRF comparison, idle/absolute session expiry, logout, replacement, reboot isolation and 32-bit clock wrap. Client checks cover literal rendering of hostile HTML, encoded message IDs, UTF-8 byte limits, expired login, connection errors and hidden tabs. They also check unchanged-card reuse, overlapping-poll prevention, timeout recovery and unread counts after expiration. These host checks do not exercise live ESP-IDF HTTPS handshakes or physical certificate renewal.

After editing `assets/web/head.html` or `assets/web/app.js`, run:

```sh
python3 scripts/embed_web.py
python3 scripts/test_web.py
.venv/bin/pio run
```

`include/web_assets.h` is committed so ordinary offline builds need no asset-generation step. For a changed logo, update the inline SVG in `assets/web/head.html` and the setup-page SVG too.

## Update trust roots

The committed `include/ca_bundle.h` allows building without a certificate-generation dependency. To refresh it from a trusted PEM certificate collection:

```sh
.venv/bin/pip install cryptography
.venv/bin/python scripts/make_ca_bundle.py /etc/ssl/certs/ca-certificates.crt
.venv/bin/pio run
```

Run from the repository root: the script writes `include/ca_bundle.h` relative to the working directory. Review the PEM source and generated change before distributing firmware. The generated bundle stores subjects/public keys sorted in ESP32's bundle format. Installing `cryptography` is optional for normal compilation.

## Hardware acceptance checklist

Use a dedicated test identity and another client supporting NIP-17 DMs; also retain a legacy client for kind-4 compatibility. Record board variant, firmware revision, relay, phone/browser and observed results.

- [ ] Boot reaches configuration with a readable display and correct touch coordinates.
- [ ] Join the displayed access point; automatic portal and direct URL both work.
- [ ] HTTPS setup requires the display PIN; missing/wrong PIN cannot access message/configuration data.
- [ ] Verify the browser certificate fingerprint matches Web details; test session expiry and logout.
- [ ] Generate a new certificate; verify a new fingerprint/PIN after restart and rejection of old sessions.
- [ ] Browser inbox, reader, reply, composition, older history and read flags agree with the display.
- [ ] Invalid key/checksum and invalid relay URL produce actionable form errors.
- [ ] Save valid settings; the device restarts and reaches Online.
- [ ] Public-key configuration receives a tagged public note and disables sending.
- [ ] Private-key configuration publishes signed kind-10050/10002 metadata; another client discovers the inbox.
- [ ] Send/receive modern DMs through advertised inbox relays, including NIP-42 authentication.
- [ ] Private-key configuration receives a legacy DM whose displayed text matches the sender's text.
- [ ] Reply with the keyboard; the other client decrypts the reply and the relay acknowledgment updates its state.
- [ ] Compose a new DM and a public mention; confirm the intended privacy mode for each.
- [ ] Restart; history, read flags and a pending outgoing event restore correctly.
- [ ] Disconnect/reconnect networking; the same pending event retries without creating a different event ID.
- [ ] Fetch older history and exercise the 60-message limit.
- [ ] Test incorrect Wi-Fi credentials, Setup/Back and the three-second BOOT recovery gesture.

Physical acceptance checks have not yet been completed in this workspace.
