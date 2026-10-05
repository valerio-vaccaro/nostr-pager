# Address book

[Project README](../README.md) · [Browser interface](web-interface.md)

Save a public key once and choose the contact when writing a message. Up to 24
contacts are stored in `/contacts.json` on the LittleFS flash partition. Contacts
and cached profile names survive power-off and firmware uploads using the same
partition layout. The display and authenticated webpages share this address book.

## Display

Tap **Contacts** in the inbox, then **Add**. Enter an `npub`, public hex key,
`nprofile`, or supported `nostr:` link. Private keys are rejected. Add an optional
local nickname (up to 32 UTF-8 bytes), then **Save**. Leave it blank to use the
name fetched from Nostr. Tap the pencil to edit a nickname or delete a contact.
**Names** queues profile refresh when the relay connection is available.

Contacts appear in a compact scrolling table with Name, Key and Edit columns.
Tap the name/key to compose, or the pencil to open the editor and see the full key.

Tap a contact to start a message. **Contacts** in the composer opens the picker
while preserving the draft, reply reference and privacy mode; **Back** returns
without selecting. The chosen contact's short name is shown above the recipient
key. You can also type a saved short name directly in the recipient field.
**Save contact** in the message reader pre-fills the contact editor.

## Browser

Unlock with the display PIN and open **Contacts**. Add public keys, update local
nicknames, remove contacts, or **Refresh Nostr names**. Reload the page to see
names fetched asynchronously. **Message** starts a composer for a contact.
The compact table has Name, Public key and Actions columns. Expand the shortened
npub to read/copy the full key. **Edit** opens the nickname form; **Delete** removes
the contact. **Add contact** stays collapsed until opened.
The compose form's **Saved contact** selector works without JavaScript; with
JavaScript, typing a recipient clears the selection. **Save contact** in a message
pre-fills the add form. Contact changes use the existing HTTPS session and CSRF
checks. Public-key identity mode can manage contacts and fetch names; sending
still requires a private identity key.

## Profile names and identity

The pager requests kind-0 metadata over the configured relay and, in private
mode, DM discovery connections. Names are accepted only from profiles with valid
event IDs/signatures belonging to saved keys. `display_name` is preferred, then
`name`; names are limited to 64 UTF-8 bytes without control characters. Older
metadata is ignored; timestamp ties use the lowest event ID, following
[NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md).

Local nicknames override profile names. Only saved contact labels resolve: this
is not a global username search. ASCII matching ignores case and surrounding
spaces; non-ASCII matching is exact. Duplicate labels cause typed-name sending
to fail; choose a contact or use its npub instead. Pickers address the public key
directly, so duplicate names remain usable. Check the key when adding a contact;
a profile name does not independently establish a person's identity.

Missing profiles use a short key label until metadata arrives. Cached names work
offline. Profile pictures, NIP-05/DNS resolution and follow-list synchronization
are not implemented. Contact saves use a temporary file followed by rename;
write failure retains the previous address book. Contacts are unencrypted in
flash, like message history.

## Validation

`python3 scripts/test_modern.py` tests persistence across simulated reboot,
write-failure rollback, signed profile author/freshness checks, ambiguous names,
key validation, Unicode nicknames and capacity. Browser selection and untrusted
name rendering are checked by `python3 scripts/test_web.py`.
`python3 scripts/preview_ui.py` renders the LVGL contact screens and checks draft
preservation. Physical flash and live profile discovery require board validation.
