# Relay connections and recovery

[Project README](../README.md) · [Browser interface](web-interface.md)

Open **Relays** in the authenticated browser interface. **Settings** also shows the connection and publication tables. Rows show the endpoint, its role, connection/subscription state, retry countdown, received-event count, public publication result and most recent error. Event counts include duplicate frames and events later rejected by signature/decryption checks; the inbox itself deduplicates public/legacy event IDs and modern rumor IDs.

The table reports public and DM subscriptions separately. Public and private requests use different subscription IDs, so a DM refusal does not close public mentions on that connection. Modern inbox startup requests up to 200 gift wraps over the preceding three days; their envelope timestamps are randomized, so requesting only the latest 20 can omit a newly sent message. The local cache remains limited to 60 messages, sorted by the decrypted message timestamp. Older-history requests use their existing page limits. A busy inbox can still exceed these bounds; this is not unlimited archival retrieval.

## Lists and defaults

| List | Default | Purpose |
| --- | --- | --- |
| Primary relay | `wss://relay.damus.io` | Persistent public mentions and private inbox connection |
| Additional public relays | `wss://relay.damus.io`, `wss://nos.lol`, `wss://relay.primal.net`, `wss://relay.nostr.band`, `wss://offchain.pub`, `wss://relay.snort.social` (up to 6; saved lists are not changed) | More sources for public mentions and alternatives for public publication |
| Backup DM inboxes | `wss://relay.damus.io`, `wss://nos.lol` | Private-key mode advertises these inboxes and monitors them for encrypted messages |

The providers publish relay information at [Damus](https://relay.damus.io/), [nos.lol](https://nos.lol/) and [Primal](https://relay.primal.net/). Defaults do not guarantee availability, permission to publish, or delivery. Public-key mode cannot decrypt DMs or publish relay-list metadata.

Use one `wss://` URL per line, at most six per editable list and 200 bytes per URL. Host case, default port 443 and a root trailing slash are normalized; duplicate endpoints and the primary endpoint are not connected twice. An empty field clears its configured list. Valid inboxes discovered in this identity's signed profile can still be monitored, even after clearing the configured backup field.

Saving on **Relays** preserves Wi-Fi credentials, identity, messages and browser sessions. Configuration is persisted before it takes effect; a failed write keeps the running lists. Existing configured boards receive the new defaults when their saved configuration has no relay-list fields. Explicitly saved empty arrays remain empty after restart.

## Scheduling and failures

The primary socket stays connected. One additional socket rotates among public and backup DM inbox endpoints, including supported inboxes discovered in this identity's signed kind-10050 metadata. Private mode advertises at most seven inboxes: the primary plus up to six additional endpoints, prioritizing configured backups. Advertised inboxes are monitored; unsupported or excess discovered endpoints are omitted from the rewritten announcement. Public and DM roles share a connection when they refer to the same URL.

A healthy additional connection stays active for about 30 seconds. Failed connections and unconfirmed subscriptions move to another endpoint. Each endpoint has its own exponential retry delay, starting around eight seconds and capped around 256 seconds, with a small jitter. A valid subscription response resets its consecutive-failure count. TLS certificate checks remain enabled; individual TLS handshakes are limited to ten seconds. DNS/TCP/TLS work is synchronous on core 1 and can delay touch input and fresh state snapshots. The HTTPS task on core 0 can still serve cached pages and operation status.

The rotating connection is paused while the setup network is open to conserve sockets. Messages may arrive when an endpoint is revisited rather than immediately. Each reconnect requests up to 20 public mentions, 60 legacy DMs and 200 modern envelopes within three days. Relay retention and these limits can still leave gaps.

## Sending and reconnecting

Public notes first enter the durable local outbox, then publish to the primary and configured public alternatives. Acceptance from any relay completes the local pending state. A rejection from one endpoint does not override another endpoint's chance to accept; rejection becomes final only when the primary and all public alternatives explicitly refuse. Missing acknowledgements leave the durable event pending for retry. After acceptance, the most recent note continues background publication to the remaining public alternatives, until replaced by a new public note or its ten-minute replication window ends.

Modern DMs use the recipient's verified signed inbox list, rather than arbitrary public fallbacks. Discovery, own relay-list publication and recipient delivery continue when the primary connection is down. The primary still receives the sender's encrypted history copy when available. Legacy outgoing kind-4 replies use the primary relay.

Recipient routing starts as soon as discovery has a usable signed list and an end-of-stored-events response (EOSE). If EOSE is absent, the seven-second lookup timeout still allows progress. Only an acknowledgement from the recipient delivery connection completes a modern DM; the primary/history and discovery connections cannot acknowledge that recipient wrapper. Acceptance still does not establish recipient reading.

**Reconnect relays** immediately clears connection backoff and requests fresh connections and subscriptions. It preserves the identity, history, queued message and browser session. The display has the same action in **Web access**. A bad relay URL, network restriction or denied subscription still needs correction; reconnect does not bypass TLS or relay policy.

The second table separates acknowledgement of kind-10050 publication from **verified readback**. A relay saying `OK` alone is not proof that senders can fetch the list, nor proof that it accepts every DM wrapper. Refresh the sending client's profile/relay cache after publishing new inbox metadata.

## Missing DM inbox list

Modern clients need the recipient's signed kind-10050 inbox list. Configure a private key, choose an inbox relay that accepts kinds 10050 and 1059, then check **Settings → DM relay publication** for verified readback. Refresh the sending client's profile/relay cache after publication. A missing-list warning may refer to the other recipient; check which identity the sender could not resolve. If no recipient list is found, the pager keeps its encrypted outbox pending and retries discovery.
