# Browser interface

[Project README](../README.md) · [Setup](setup.md)

## Connect and unlock

Use **https://pager.local/** from a device on the pager's Wi-Fi or the same LAN.
The firmware advertises the hostname `pager` and an HTTPS service on port 443 via
mDNS. The display also shows the numeric IP URL, usable when the browser/network
does not support mDNS or blocks local multicast. Web Settings lists this IP fallback.

Certificates include `pager.local` as a DNS subject alternative name. Older
certificates without the hostname are replaced on the first boot of this update;
compare the new fingerprint in Web details before accepting the certificate.

During first setup, join the Wi-Fi network shown on the board and use its large displayed password. Open `https://192.168.4.1/` in a full browser. The HTTP captive portal only provides connection instructions; it never accepts credentials or serves messages.

After connecting to your Wi-Fi, tap **Web access** in the display's inbox and open the shown HTTPS address from a browser on that same network. Compare the browser certificate's SHA-256 fingerprint with the full fingerprint on that screen, then accept the self-signed certificate. Browser controls vary; some phones' captive-portal mini browsers cannot accept it, so use a full browser.

Open **Setup** on the board to read the six-digit PIN; it is not shown in the inbox or Web details. Leading zeroes count. The Wi-Fi password and the PIN are separate: joining Wi-Fi alone does not unlock the pages. The login page, its animation script and HTTP connection instructions are public. Protected routes return HTTP 401 without an authenticated session.

## Messages

- **Inbox** displays cached messages, unread state, DM/NOTE badges, timestamps and outgoing delivery state. It refreshes every five seconds while the tab is visible.
- Open a card to view the complete message. **Mark read** updates the board's read flag. **Reply** opens the composer with recipient and privacy mode filled from the message.
- **New** opens a recipient/message form. Choose a saved contact or type its short name. Public keys also accept hex, npub, nprofile and supported nostr links. Use **Encrypted DM** or explicitly select **Public mention**. See [Address book](contacts.md) for managing contacts and fetched Nostr names.
- The counter measures UTF-8 bytes; messages may contain at most 512 bytes. Your Nostr identity needs a private key to send.
- **Fetch older messages** requests relay history subject to the same 60-message cache limit.

Browser and display actions use the same history, outbox and relay connection. A relay acceptance is not a recipient read receipt. If an input fails, use the browser's Back button to retain the draft. The composer does not poll or reload its fields.

Only one inbox refresh request runs at a time. A request that stalls for ten seconds is aborted; the next poll retries. Unchanged message cards stay in place instead of being recreated every second. Expired messages are removed and the unread count is corrected by the local expiry check while the inbox is visible. Returning to a hidden tab requests fresh state.

Pages run on core 0 while relay work runs on core 1. Page navigation uses the latest cached data, so it remains available during a slow relay connection. The inbox indicates when that data is over five seconds old; new arrivals and relay status appear after messaging work resumes. Browser actions show an **Action queued** page that checks completion automatically. Use **Continue browsing** to leave it; the action remains queued even if you close the tab. Do not resubmit a pending action. Up to four operations are tracked; a full queue reports that nothing was submitted. Result pages belong to the submitting login session, and older completed results may expire. Wi-Fi and memory remain shared between cores.

## Device settings and certificates

**Settings → Wi-Fi and Nostr setup** opens the protected configuration form. Saving restarts the device; use the new display PIN/address afterwards. Opening Setup on the display keeps messaging active while the saved Wi-Fi connection is available, so you can read the PIN and use browser messaging without leaving Setup.

**Settings → Messaging connection** reports the Wi-Fi connection, identity mode, storage, clock, configured relay, actual inbox subscription status and outbox state. It includes the relay failure count and most recent connection error, including TLS errors or the HTTP handshake refusal when supplied by the networking library. It also counts received inbox events and skipped events for the current boot, with the last skip reason. Reload Settings after sending a test message. Zero inbox events means the relay has not supplied matching messages; skipped events indicate an event validation, size or decryption problem. The public key shown here can be compared with the recipient used by the sender; private keys are never included.

The browser inbox displays one message card with **Previous**, **Next** and a position counter. The open-message page has the same navigation and a **Back to inbox** link that restores that message's position. The automatic inbox refresh preserves the selected message when new messages arrive. Empty inboxes and cache boundaries disable the corresponding buttons.

**Relays** provides editable public/backup DM lists and a **Reconnect relays** button. Both Relays and Settings show compact connection and DM publication tables, including per-endpoint errors and readback verification. Saving lists or reconnecting preserves the browser session and pending outbox. See [Relay connections](relays.md).

**Delete message** in the reader and **Delete all messages** in the inbox require a separate confirmation page. The final operation is an authenticated, CSRF-protected POST; GET only presents the confirmation. Deletion affects local history and pending retries, preserves other configuration and hides deleted messages after relay refresh. See [Delete local messages](user-guide.md#delete-local-messages).

The board generates an ECDSA P-256 TLS key and self-signed certificate locally on first use, then persists them. **Settings** shows the SHA-256 fingerprint and offers **Download certificate** and **Generate new certificate**. The latter creates a fresh TLS identity after restarting the board. Existing browser sessions are invalid, the PIN changes, and the browser will require accepting the new certificate. No Nostr identity or message history is changed by certificate renewal.

Certificates include the setup IP and the IP present when generated. When the LAN IP changes, use the new display address and regenerate if you want the certificate's IP entry updated. Because initial configuration can precede internet time synchronization, validity is fixed to 2020–2049. The certificate is not automatically trusted by a public CA; fingerprint comparison establishes that you reached your board.

## Reset controls

The display's Setup screen and the authenticated web setup page have two red
controls. **Reset** opens a second confirmation and then restarts, preserving
settings, messages, contacts and the HTTPS certificate. **Delete data and reset**
also requires a second confirmation; it deletes Wi-Fi credentials, the stored
Nostr identity, message history/outbox, contacts and the HTTPS key/certificate,
then returns to first-time setup. Keep an independent private-key backup before
confirming deletion. **Cancel** performs no reset or deletion.

Web confirmation pages require login. The final operation requires POST, a valid
session CSRF token, a valid reset mode and explicit confirmation. Neither merely
opening a confirmation page nor an unauthenticated request schedules a reset.
Every reset invalidates browser sessions and generates a new setup PIN. Deletion
also generates a new certificate, requiring another fingerprint check.

Deletion formats the LittleFS partition and clears the two application NVS
namespaces. A durable wipe flag is cleared only after deletion succeeds, allowing
an interrupted operation to retry at boot. This is a logical factory reset, not
a forensic secure-erasure guarantee. Firmware remains installed. If deletion
fails, the board displays an error; restart it to retry.

## Session expiry

Cookies are Secure, HttpOnly, SameSite=Strict session cookies with random 128-bit tokens. Up to four browser sessions are supported. A session expires after 30 minutes without requests or eight hours total. Inbox polling counts as activity while visible; hidden tabs stop polling. **Lock** logs out immediately. Reboot and certificate renewal remove every session and change the PIN. Five failed PIN attempts cause a global one-minute cooldown.

Up to three HTTPS client sockets are supported simultaneously to limit ESP32 memory use. Close idle tabs if access stalls. Live browser handshakes, certificate regeneration and end-to-end messaging still require physical-board acceptance tests.
