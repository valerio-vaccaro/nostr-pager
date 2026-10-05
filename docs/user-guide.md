# Using the pager

[Project README](../README.md) · [Setup](setup.md)

## Choose an identity mode

| Configured identity | Receive | Send |
| --- | --- | --- |
| Public key | Public kind-1 notes tagging your key | Read only; New and Reply are disabled |
| Private key | Tagged public notes, modern NIP-17 and legacy kind-4 DMs | Encrypted DMs and public mentions |

Accepted formats are 64-character hex, `npub`, `nsec`, `nprofile`, and `nostr:` links containing those formats. Hex identity input requires selecting its type. Recipient input accepts public formats only. `nprofile` relay hints are ignored; the configured relay is used.

Uppercase Bech32 is accepted, mixed case is rejected, and checksums are verified. NIP-05 names, note/event identifiers and encrypted `ncryptsec` keys are not identity inputs.

## Read messages

The compact header shows firmware version and connection status; the larger card gives most of the inbox to message text.

The inbox shows one message at a time, newest first, with a saved contact name or peer-key prefix, DM/public indicator, preview and UTC timestamp. Use **Prev** and **Next** to browse; the counter shows your position in the saved cache. Tap the card to open its body. Opening an entry marks it read locally; it does not send a read receipt.

The reader also has **Prev** and **Next**, so you can move directly between messages. Buttons are disabled at the ends of the cache. Incoming messages do not move you away from the message you are browsing. Long message bodies can still be scrolled to read their full text. Browser Inbox and message pages provide the same button navigation; browser messages retain the separate **Mark read** action.

The reader preserves its scroll position when arrivals update the navigation counter. Unchanged message data does not rebuild the screen. Display buttons change state immediately, without transition animations. Relay handshakes still run on the display's loop task, so an unavailable endpoint can temporarily delay touch processing.

Tap **Contacts** to manage the [address book](contacts.md). Save an npub once, let the pager fetch its Nostr name, then select it through **Contacts** in the composer or type its saved short name.

Tap **Web access** for the browser URL, IP fallback, certificate fingerprint and **Reconnect**. The login PIN is shown only in **Setup**. **Older**, **New** and **Setup** remain along the bottom of the inbox.

The unread count excludes outgoing messages. Newly received incoming events dated within the preceding minute request a brief visual alert. Historical events can appear unread without a recent-message alert. There is no sound or vibration.

Use **Inbox** to return. Use **Older** to request earlier relay history while connected and below the 60-message cache limit. Messages absent from the relay cannot be recovered by this action.

## Delete local messages

The reader has a red **Delete** button; the inbox has **Delete all**. Both open a second confirmation with **Cancel**. The browser offers the same controls. Deletion preserves Wi-Fi settings, your key, contacts and certificates, and stops future retries for a deleted pending send. It does not remove already published copies from Nostr relays.

Deletion markers are committed in the same atomic history-file replacement as the remaining messages. Failed writes restore the messages and markers in RAM and keep the pending send. Deleted messages stay suppressed after restart and relay resynchronization. Delete all suppresses messages dated at or before the deletion time, plus any deleted future-dated cached entries. New messages arriving with newer timestamps remain visible. Without a synchronized clock, the newest cached timestamp supplies the cutoff.

Up to 128 individual deletion markers are retained. If full, another single-message deletion is refused rather than silently losing suppression; Delete all advances the cutoff and releases older markers. This is logical deletion, not forensic flash erasure.

## Compose or reply

1. Tap **New**, or open a message and tap **Reply**.
2. Enter a recipient public key. Replies prefill the original message's peer.
3. Tap the message field and type with the on-screen keyboard. Tap either text field to change keyboard focus.
4. Check **Public mention** only when you want a publicly readable note.
5. Tap **Send**, or **Cancel** to discard the draft.

New messages default to encrypted DMs. Replies inherit the selected message's type, so a reply to a public mention starts in public mode. Public replies also reference the original event ID. The keyboard's ready action moves focus to the message field; publication uses the separate **Send** button.

Outgoing text is limited to 512 UTF-8 bytes, which can be fewer than 512 characters. Incoming DMs above that plaintext limit are rejected. Public previews/bodies are truncated after 512 bytes. The included font does not cover all emoji or CJK characters.

Drafts remain available while incoming events update the inbox, but are not saved across reset, cancellation or a transition to setup.

## Understand sending status

| Status | Meaning / next step |
| --- | --- |
| `pending` | Signed event saved locally; waiting for relay acknowledgment |
| `accepted` | Relay acknowledged acceptance; recipient delivery/read is not confirmed |
| `rejected` | Relay refused the event; inspect the status reason and compose again if appropriate |
| Queued locally / waiting for relay | Offline send is saved and will be published after relay reconnection |

Only one outgoing event can be pending. Pending events survive restart and are retried after connection and approximately every 30 seconds while connected. Confirmed deletion of that pending message stops further retries; it cannot recall already published copies. Use **Relays → Reconnect relays** or edit the relay lists when an endpoint remains unavailable.

A private key, usable UTC time and working history storage are required to send. [Troubleshooting](troubleshooting.md) explains the corresponding errors.

## History and identity changes

History and read state survive restart. At most 60 entries are retained; newer arrivals can evict the oldest confirmed entry, while a pending entry is protected. Once full, **Older** stops fetching.

Stored records belong to the configured public key. Switching identity does not present the previous identity's records, and subsequent history saves can replace that cache. There is no archive/export or multi-account feature. Public notes sent from another client are not fetched through an authors subscription; the outgoing authors subscription covers kind-4 DMs only.

## Private-message compatibility

New private messages use modern NIP-17. Legacy kind-4 messages remain readable, and Reply uses their original format. Modern text chats are limited to two people; group messages and attachments are not displayed. Private-key mode publishes the configured inbox relay automatically. If another client reports missing DM inbox relays, see [Relay connections](relays.md#missing-dm-inbox-list).
