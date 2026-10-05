#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <WebSocketsClient.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include "board.h"
#include "pager.h"
#include "nostr_crypto.h"
#include "ca_bundle.h"
#include "web_pager.h"
#include "dm_relay.h"
#include "version.h"
#include "contacts.h"
#include "reset_control.h"
#include "public_relays.h"
#include "history_store.h"
#include "tls_memory.h"
#include "dns_health.h"
#include "inbox_requests.h"
#include "message_expiry.h"
#include "pager_sync.h"

namespace pager {
std::vector<Message> messages;
String status="Starting...", publicKey, webAddress, webIpAddress, webPin, webFingerprint;
bool canSend=false;
bool inboxReceivedOnly=true;
}
namespace {
constexpr size_t HistoryLimit=60;
Preferences preferences;
Preferences resetStore;
struct ResetStorage {
    Preferences tls;
    bool openTls(){return tls.begin("pager-tls",false);}
    bool formatFiles(){return LittleFS.format();}
    bool clearTls(){return tls.clear();}
    bool clearConfiguration(){return preferences.clear();}
    bool finishWipe(){return resetStore.remove("wipe");}
    void closeTls(){tls.end();}
};
TaskHandle_t relayHandle=nullptr,uiHandle=nullptr;
void relayTask(void *);
void uiTask(void *);
WebServer server(80);
DNSServer dns;
WebSocketsClient socket;
String wifiSsid,wifiPassword,secret,relayUrl="wss://relay.damus.io",apSsid,apPassword;
keys::Relay relayAddress;
bool portalActive=false,relayStarted=false,relayConnected=false,fsReady=false,historyDirty=false;
uint32_t connectAt=0,saveAt=0,rebootAt=0,lastPublish=0,bootHold=0,wifiLostAt=0;
uint32_t oldestFetched=UINT32_MAX;
String pendingEvent,pendingId,pendingWireId,pendingPeer,relayAuthId;
bool pendingModern=false;
bool legacyDenied=false;
bool pendingPublic=false;
std::vector<String> extraRelays,backupInboxes;
bool portalRequested=false,resumeRequested=false;
String portalRequestMessage;
String shownDmStatus;
String inboxState="Waiting for relay connection";
String publicInboxState="Waiting for connection",privateInboxState="Waiting for connection";
uint32_t inboxEvents=0,inboxSkipped=0;
uint32_t receivedNotes=0,receivedLegacy=0,receivedModern=0,acceptedMessages=0,repeatedMessages=0;
uint32_t primaryInboxEvents=0;
historyStore::Filter historyFilter;
String lastInboxSkip;
String relayLastError;
uint32_t relayFailures=0;
uint32_t primaryRetryAt=0;
uint8_t primaryAttempts=0;
void sendFrame(const String &wire) { socket.sendTXT(wire.c_str()); }
void publish();
void subscribe(bool older=false);
void deliveryAck(const String &id,bool accepted,const String &reason);
void receiveMessage(JsonObjectConst event);
String randomHex(size_t bytes) {
    uint8_t b[16];esp_fill_random(b,std::min(bytes,sizeof(b)));return keys::hex(b,std::min(bytes,sizeof(b))).c_str();
}
String escape(const String &s) {
    String out;out.reserve(s.length());
    for(char c:s) switch(c) {
        case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;
        case '\"':out+="&quot;";break;case '\'':out+="&#39;";break;default:out+=c;
    }
    return out;
}
void setStatus(const String &s) {pagerSync::Lock guard;pager::status=s;}
bool saveHistory() {
    if(!fsReady)return false;
    File file=LittleFS.open("/history.tmp","w");
    if(!file) {setStatus("History write failed");saveAt=millis()+5000;return false;}
    bool ok=true;
    {
        DynamicJsonDocument header(24576);header["owner"]=pager::publicKey;header["historyFilter"]=1;header["before"]=historyFilter.before;
        JsonArray deleted=header.createNestedArray("deleted");
        for(const auto &marker:historyFilter.deleted){JsonObject row=deleted.createNestedObject();row["id"]=marker.id;row["timestamp"]=marker.timestamp;}
        ok=!header.overflowed() && serializeJson(header,file)>0 && file.print('\n')==1;
    }
    for(const auto &m:pager::messages) {
        if(!ok)break;
        DynamicJsonDocument doc(24576);
        doc["owner"]=pager::publicKey;doc["event"]=m.event;if(m.delivery=="pending")doc["pending"]=m.pendingWire;doc["read"]=m.read;doc["delivery"]=m.delivery;
        if(doc.overflowed() || !serializeJson(doc,file) || file.print('\n')!=1){ok=false;break;}
    }
    file.flush();file.close();
    if(ok && LittleFS.rename("/history.tmp","/history.jsonl")){historyDirty=false;return true;}
    setStatus("History save failed");saveAt=millis()+5000;return false;
}
bool tagged(JsonObjectConst e,const String &key) {
    for(JsonArrayConst tag:e["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="p" && tag[1]==key)return true;
    return false;
}
bool messageFrom(JsonObjectConst event,Message &m,String *error=nullptr) {
    auto reject=[&](const char *reason){if(error)*error=reason;return false;};
    if(event["kind"]==1059) {
        if(!pager::canSend)return reject("Private key required to decrypt modern DMs");
        DynamicJsonDocument rumor(8192);if(!crypto::unwrap(event,secret,pager::publicKey,rumor,error,&m.expiresAt))return false;
        JsonObjectConst inner=rumor.as<JsonObjectConst>();
        m.id=inner["id"].as<String>();m.timestamp=inner["created_at"]|0U;m.kind=14;
        m.outgoing=inner["pubkey"]==pager::publicKey;m.peer=inner["pubkey"].as<String>();
        if(m.outgoing)for(JsonArrayConst tag:inner["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="p" && tag[1]!=pager::publicKey){m.peer=tag[1].as<String>();break;}
        m.text=inner["content"].as<String>();m.delivery=m.outgoing?"accepted":"";serializeJson(event,m.event);return true;
    }
    int kind=event["kind"]|-1;
    if(kind!=1 && kind!=4)return reject("Unsupported message kind");
    String sender=event["pubkey"]|"";
    bool outgoing=sender==pager::publicKey;
    if(!tagged(event,pager::publicKey) && !outgoing)return reject("Message does not tag this recipient");
    if(kind==4 && !pager::canSend)return reject("Private key required to decrypt legacy DMs");
    if(!crypto::verify(event))return reject("Message signature or ID invalid");
    m.id=event["id"].as<String>();m.timestamp=event["created_at"]|0U;m.kind=kind;
    m.outgoing=outgoing;m.peer=sender;m.delivery=outgoing?"accepted":"";
    if(outgoing) {
        for(JsonArrayConst tag:event["tags"].as<JsonArrayConst>())
            if(tag.size()>=2 && tag[0]=="p") {m.peer=tag[1].as<String>();break;}
    }
    if(kind==4) {
        if(!crypto::decrypt(secret,m.peer,event["content"].as<String>(),m.text))return reject("Legacy NIP-04 decryption failed");
    } else {
        String content=event["content"].as<String>();
        m.text=content.length()>512?content.substring(0,512)+"...":content;
    }
    serializeJson(event,m.event);return true;
}
bool addMessage(Message m,bool notify) {
    for(auto &old:pager::messages)if(old.id==m.id)return false;
    if(pager::messages.size()>=HistoryLimit) {
        // Preserve a pending outgoing message while evicting the oldest confirmed item.
        auto it=std::find_if(pager::messages.rbegin(),pager::messages.rend(),[](const Message &v){return v.delivery!="pending";});
        if(it==pager::messages.rend())return false;
        if(m.timestamp<=it->timestamp)return false;
        pager::messages.erase(std::next(it).base());
    }
    pager::messages.push_back(std::move(m));
    std::sort(pager::messages.begin(),pager::messages.end(),[](const Message &a,const Message &b){return a.timestamp>b.timestamp;});
    historyDirty=true;saveAt=millis()+2000;ui::changed(notify);return true;
}
void loadHistory() {
    if(!fsReady)return;
    File file=LittleFS.open("/history.jsonl","r");
    while(file && file.available() && pager::messages.size()<HistoryLimit) {
        String line=file.readStringUntil('\n');
        if(line.length()>24576)continue;
        DynamicJsonDocument row(32768),event(16384);
        if(deserializeJson(row,line)||row["owner"]!=pager::publicKey)continue;
        if(row["historyFilter"]==1) {
            historyFilter.before=row["before"]|0U;
            for(JsonObjectConst marker:row["deleted"].as<JsonArrayConst>()) {
                String id=marker["id"]|"";uint8_t bytes[32];
                if(keys::unhex(id.c_str(),bytes,32) && historyFilter.deleted.size()<historyStore::MarkerLimit)historyFilter.remember(id.c_str(),marker["timestamp"]|0U);
            }
            continue;
        }
        if(deserializeJson(event,row["event"].as<String>()))continue;
        Message m;
        if(messageFrom(event.as<JsonObjectConst>(),m)) {
            if(historyFilter.hides(m.id.c_str(),m.timestamp))continue;
            m.read=row["read"]|false;m.delivery=row["delivery"]|"";
            if(m.delivery=="pending" && pendingId.isEmpty()) {
                m.pendingWire=row["pending"]|"";
                String candidate=m.pendingWire.isEmpty()?m.event:m.pendingWire;
                DynamicJsonDocument wire(16384);
                bool valid=m.outgoing && !(m.kind==14 && m.pendingWire.isEmpty()) &&
                    !deserializeJson(wire,candidate) && crypto::verify(wire.as<JsonObjectConst>());
                if(valid && m.kind==14)valid=wire["kind"]==1059 && tagged(wire.as<JsonObjectConst>(),m.peer);
                if(valid && m.kind!=14)valid=wire["id"]==m.id && wire["pubkey"]==pager::publicKey;
                if(valid) {
                    pendingId=m.id;pendingModern=m.kind==14;pendingPublic=m.kind==1;pendingPeer=m.peer;
                    pendingEvent=candidate;pendingWireId=wire["id"].as<String>();
                } else {m.delivery="rejected";m.pendingWire="";}
            }
            addMessage(std::move(m),false);
        }
    }
    if(file)file.close();historyDirty=false;
}
void subscribe(bool older) {
    if(!relayConnected)return;
    uint32_t until=oldestFetched;
    if(older && until==UINT32_MAX) {
        for(auto &m:pager::messages)until=std::min(until,m.timestamp);
        if(until==UINT32_MAX)until=time(nullptr);
    }
    auto transmit=[&](bool privateInbox) {
        DynamicJsonDocument doc(2048);
        const char *id=privateInbox?(older?"older-dm":"pager-dm"):(older?"older":"pager");
        inboxRequests::build(doc,id,pager::publicKey,privateInbox,true,20,older,until,uint32_t(time(nullptr)),!legacyDenied);
        String wire;serializeJson(doc,wire);socket.sendTXT(wire);
    };
    transmit(false);if(pager::canSend)transmit(true);
    if(!older){inboxState="Subscription requested / waiting for relay";publicInboxState="Requested / waiting for relay";privateInboxState=publicInboxState;}
    setStatus(older?"Loading older messages...":"Relay connected / syncing...");
}
void deliveryAck(const String &id,bool accepted,const String &reason) {
    if(pendingId.isEmpty() || id!=pendingWireId)return;
    if(!accepted && reason.startsWith("auth-required:")){setStatus("Relay requires authentication / retrying");return;}
    for(auto &m:pager::messages)if(m.id==pendingId){m.delivery=accepted?"accepted":"rejected";m.pendingWire="";}
    setStatus(accepted?"Accepted by relay":"Relay rejected: "+reason.substring(0,80));
    pendingId="";pendingEvent="";pendingWireId="";pendingPeer="";pendingModern=false;
    historyDirty=true;saveAt=millis();ui::changed();
    publicRelays::clear(id);pendingPublic=false;
}
void publish() {
    if(pendingEvent.isEmpty())return;
    if(pendingPublic) {
        bool backups=publicRelays::publish(pendingEvent,pendingWireId);
        if(relayConnected)sendFrame("[\"EVENT\","+pendingEvent+"]");
        setStatus(backups?"Public message queued across relays":relayConnected?"Message sent / waiting for relay":"Queued locally / waiting for relay");
        lastPublish=millis();return;
    }
    if(!relayConnected && !pendingModern)return;
    if(pendingModern) {
        dmRelay::deliver(pendingPeer,pendingEvent,pendingWireId);
        if(relayConnected)for(const auto &m:pager::messages)if(m.id==pendingId){sendFrame("[\"EVENT\","+m.event+"]");break;}
        setStatus(dmRelay::status());
    } else {sendFrame("[\"EVENT\","+pendingEvent+"]");setStatus("Message sent / waiting for relay");}
    lastPublish=millis();
}
void receiveMessage(JsonObjectConst event) {
    ++inboxEvents;Message m;
    int kind=event["kind"]|-1;
    if(kind==1)++receivedNotes;else if(kind==4)++receivedLegacy;else if(kind==1059)++receivedModern;
    auto skip=[&](const char *reason){++inboxSkipped;lastInboxSkip=reason;Serial.printf("[Inbox] skipped kind=%d: %s\n",kind,reason);};
    if(uint32_t(event["created_at"]|0U)>uint32_t(time(nullptr))+600){skip("Event timestamp is in the future");return;}
    if(measureJson(event)>(event["kind"]==1059?12288:3072)){skip("Event exceeds receive size limit");return;}
    String error;bool valid=false;
    pagerSync::blocking([&]{valid=messageFrom(event,m,&error);});
    if(!valid){skip(error.c_str());return;}
    if(m.timestamp>uint32_t(time(nullptr))+600){skip("Message timestamp is in the future");return;}
    if(historyFilter.hides(m.id.c_str(),m.timestamp)){skip("Message was deleted locally");return;}
    oldestFetched=std::min(oldestFetched,uint32_t(event["created_at"]|0U));
    for(const auto &old:pager::messages)if(old.id==m.id){++repeatedMessages;return;}
    bool notify=!m.outgoing && m.timestamp>=uint32_t(time(nullptr))-60;
    bool outgoing=m.outgoing;
    if(addMessage(std::move(m),notify)){++acceptedMessages;Serial.printf("[Inbox] received kind=%d / %s\n",kind,outgoing?"sent copy":"incoming");}
    else skip("Message is older than the full local cache");
}
void relayEvent(WStype_t type,uint8_t *payload,size_t length) {
    pagerSync::Lock guard;
    if(type==WStype_CONNECTED) {relayConnected=true;relayLastError="";relayAuthId="";legacyDenied=false;Serial.printf("[Relay] Connected to %s:%u\n",relayAddress.host.c_str(),relayAddress.port);subscribe();dmRelay::connected();contacts::query(sendFrame);publish();return;}
    if(type==WStype_DISCONNECTED || type==WStype_ERROR) {
        if(type==WStype_DISCONNECTED && !relayConnected && !payload && !relayLastError.isEmpty())return;
        relayLastError=payload && length?String(reinterpret_cast<const char *>(payload),std::min(length,size_t(192))):String("Connection closed without a reason");
        ++relayFailures;
        primaryAttempts=std::min(unsigned(primaryAttempts)+1,6U);
        uint32_t delay=(8000U<<(primaryAttempts-1))+(millis()%1000);primaryRetryAt=millis()+delay;socket.setReconnectInterval(delay);
        Serial.printf("[Relay] %s:%u: %s\n",relayAddress.host.c_str(),relayAddress.port,relayLastError.c_str());
        if(type==WStype_DISCONNECTED || !socket.isConnected()) {
            dmRelay::disconnected();relayConnected=false;inboxState="Relay disconnected";publicInboxState="Disconnected";privateInboxState="Disconnected";
            setStatus("Relay offline / retrying...");
        } else setStatus("Relay protocol error: "+relayLastError.substring(0,80));
        return;
    }
    if(type!=WStype_TEXT || length>16384)return;
    DynamicJsonDocument doc(24576);
    if(deserializeJson(doc,payload,length) || !doc.is<JsonArray>())return;
    dmRelay::event(doc.as<JsonArrayConst>());
    contacts::event(doc.as<JsonArrayConst>());
    String typeName=doc[0]|"";
    if(typeName=="EVENT" && (doc[1]=="pager" || doc[1]=="pager-dm" || doc[1]=="older" || doc[1]=="older-dm")) {
        ++primaryInboxEvents;
        receiveMessage(doc[2].as<JsonObjectConst>());
    } else if(typeName=="EOSE") {
        if(doc[1]=="older" || doc[1]=="older-dm")socket.sendTXT("[\"CLOSE\",\""+doc[1].as<String>()+"\"]");
        if(doc[1]=="pager" || doc[1]=="pager-dm") {
            if(doc[1]=="pager")publicInboxState="Subscribed / live mentions";
            else privateInboxState="Subscribed / live DMs";
            primaryAttempts=0;primaryRetryAt=0;socket.setReconnectInterval(8000);
            bool complete=publicInboxState=="Subscribed / live mentions" && (!pager::canSend || privateInboxState=="Subscribed / live DMs");
            inboxState=complete?"Subscribed / live inbox":"Partial subscription / check public and DM status";
            setStatus(complete?"Online / "+String(pager::messages.size())+" messages":"Relay online / inbox partly subscribed");
        }
    } else if(typeName=="OK" && doc[2].is<bool>()) {
        String id=doc[1]|"",reason=doc[3]|"";bool accepted=doc[2];
        if(!relayAuthId.isEmpty() && id==relayAuthId) {
            relayAuthId="";if(accepted){legacyDenied=false;subscribe();dmRelay::connected();publish();}else setStatus("Relay authentication rejected");
        } else if(pendingPublic && !accepted && id==pendingWireId && reason.startsWith("auth-required:"))setStatus("Relay requires authentication / retrying");
        else if(pendingPublic && !accepted && id==pendingWireId && publicRelays::publish(pendingEvent,pendingWireId))publicRelays::primaryRejected(id,reason);
        else if(!pendingModern)deliveryAck(id,accepted,reason);
    } else if(typeName=="NOTICE" || typeName=="CLOSED") {
        String reason=doc[typeName=="NOTICE"?1:2]|"";setStatus("Relay: "+reason.substring(0,80));
        if(typeName=="CLOSED" && doc[1]=="pager-dm" && !legacyDenied && reason.startsWith("auth-required")) {
            // Keep receiving modern DMs while the legacy kind-4 filter waits for authentication.
            legacyDenied=true;subscribe();return;
        }
        if(typeName=="CLOSED" && (doc[1]=="pager" || doc[1]=="pager-dm")) {
            String failure="Refused: "+reason.substring(0,96);
            if(doc[1]=="pager")publicInboxState=failure;else privateInboxState=failure;
            inboxState="Partial subscription / check public and DM status";
        }
    } else if(typeName=="AUTH") {
        String wire;if(dmRelay::authenticate(doc[1]|"",relayUrl,wire,relayAuthId))sendFrame(wire);
        else setStatus("Relay authentication needs a private key");
    }
}
void startRelay() {
    if(!keys::relay(relayUrl.c_str(),relayAddress)){setStatus("Invalid relay URL / open Setup");return;}
    socket.onEvent(relayEvent);
    socket.beginSslWithBundle(relayAddress.host.c_str(),relayAddress.port,relayAddress.path.c_str(),caBundle,"");
    socket.setReconnectInterval(8000);socket.enableHeartbeat(20000,5000,2);
    relayStarted=true;inboxState="Connecting";setStatus("Connecting to relay...");
}
String setupPage(const String &token) {
    String page=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Nostr Pager Setup</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{background:radial-gradient(ellipse at top right,#19372f55,transparent 55%),#0c111b;color:#eaf0fa;font:16px system-ui;margin:0;padding:28px 18px 40px}main{max-width:500px;margin:auto}.brand{display:flex;align-items:center;gap:14px;margin-bottom:32px}.brand svg{width:48px;height:48px;flex-shrink:0}.wordmark{font-weight:750;letter-spacing:.08em}.tagline{color:#92a2bb;font-size:12px;margin-top:4px}.eyebrow{color:#68e0c4;font-size:11px;font-weight:700;letter-spacing:.12em}h1{font-size:34px;letter-spacing:-.04em;margin:10px 0}p{color:#92a2bb;line-height:1.6}form{background:#151e2d;border:1px solid #27344a;border-radius:20px;padding:22px;margin:24px 0}label{font-weight:600;font-size:13px}input,select,button{width:100%;padding:13px;margin:8px 0 18px;border:1px solid #34445b;border-radius:10px;background:#0f1826;color:#eaf0fa;font:inherit}input:focus,select:focus{outline:2px solid #68e0c4;outline-offset:2px}button{background:#68e0c4;color:#0c111b;border:0;font-weight:750;margin:22px 0 0;cursor:pointer}button:hover{background:#8bedd5}.error{color:#ff9caa;background:#35202d;border:1px solid #623447;border-radius:12px;padding:14px}small{display:block;color:#92a2bb;font-size:12px;line-height:1.6;margin:-8px 0 18px}footer{font-size:12px;border-top:1px solid #27344a;padding-top:16px}footer strong{color:#b7a1ff}</style></head><body><main><div class="brand"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img" aria-labelledby="title">  <title id="title">Nostr Pager logo</title>  <rect width="64" height="64" rx="18" fill="#19372f"/>  <rect x="12" y="15" width="40" height="35" rx="8" fill="#0d191a" stroke="#68e0c4" stroke-width="2"/>  <rect x="19" y="23" width="26" height="14" rx="3" fill="#68e0c4"/>  <path d="M24 28h15m-15 4h9" stroke="#19372f" stroke-width="2" stroke-linecap="round"/>  <path d="M19 43h9m6 0h9" stroke="#68e0c4" stroke-width="2" stroke-linecap="round"/>  <path d="M43 9h8l5 5" fill="none" stroke="#b7a1ff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/></svg><div><div class="wordmark">NOSTR PAGER</div><div class="tagline">Your private signal.</div></div></div><div class="eyebrow">DEVICE SETUP</div><h1>Connect your pager</h1><p>Choose your Wi-Fi network and Nostr identity.</p>)HTML";
    page+="<form method='post' action='/save'><input type='hidden' name='csrf' value='"+token+"'>";
    page+="<label>Wi-Fi network (SSID)</label><input name='ssid' maxlength='32' required value='"+escape(wifiSsid)+"'>";
    page+="<label>Wi-Fi password</label><input type='password' name='password' maxlength='64' autocomplete='new-password'><small>Leave blank to keep the saved password for the same SSID. Use Open network below to clear it.</small>";
    page+="<label><input style='width:auto' type='checkbox' name='open'> Open network</label><br>";
    page+="<label>Nostr key</label><input type='password' name='key' maxlength='512' autocomplete='new-password' placeholder='npub, nsec, nprofile, hex, nostr:...'><small>Leave blank to keep the saved identity. Public keys watch public mentions. Private keys also receive and send encrypted DMs. Keys are stored locally in flash.</small>";
    page+="<details><summary>Advanced identity and relay settings</summary><p>Keep these settings unless you need a raw private hex key or different relays.</p>";
    page+="<label>For a raw hex key</label><select name='keytype'><option value='public'>Public key</option><option value='private'>Private key</option></select>";
    page+="<label>Relay URL</label><input name='relay' maxlength='200' required value='"+escape(relayUrl)+"'><small>Use wss://. TLS certificates are verified. Choose a relay accepting kind-1059 private messages and kind-10050 inbox announcements. The pager publishes this relay as its DM inbox.</small>";
    page+="<label>Additional public relays</label><textarea name='publicRelays' rows='4' maxlength='1212' style='width:100%;background:#0f1826;color:#eaf0fa;padding:13px;border-radius:10px'>"+escape(pager::publicRelayList())+"</textarea><small>One wss:// URL per line, up to six. Receives public mentions and retries public sending across relays.</small>";
    page+="<label>Backup DM inbox relays</label><textarea name='dmRelays' rows='3' maxlength='1212' style='width:100%;background:#0f1826;color:#eaf0fa;padding:13px;border-radius:10px'>"+escape(pager::dmRelayList())+"</textarea><small>One wss:// URL per line. Private-key mode announces these as inboxes and monitors them for encrypted messages.</small>";
    page+="</details>";
    page+="<p class='tagline'>Firmware "+escape(PAGER_VERSION)+"</p>";
    page+="<button>Save and connect &rarr;</button></form>";
    page+="<details><summary>Reboot and reset options</summary><p><a style='display:inline-block;background:#f4c780;color:#0c111b;border-radius:10px;padding:13px;margin-right:8px' href='/reset/confirm?mode=restart'>Reboot</a><a style='display:inline-block;background:#a92d43;color:#fff;border-radius:10px;padding:13px' href='/reset/confirm?mode=erase'>Delete data and reset</a></p></details>";
    page+="<footer><strong>A key of your own.</strong><p>Use a dedicated Nostr identity: setup uses PIN-protected HTTPS and the device stores credentials in unencrypted flash. The temporary Wi-Fi network is password protected and closes after connecting.</p></footer></main></body></html>";
    return page;
}
void httpPage() {
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Security-Policy","default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'");
    // Use the address of the interface this client reached us on: LAN IP for LAN clients, 192.168.4.1 only on the setup network.
    IPAddress local=server.client().localIP();
    bool lan=WiFi.status()==WL_CONNECTED && local==WiFi.localIP();
    String url="https://"+(local==IPAddress()?WiFi.softAPIP():local).toString()+"/";
    String name=lan?"https://pager.local/":url;
    server.send(200,"text/html","<meta name='viewport' content='width=device-width,initial-scale=1'><body style='background:#0c111b;color:#eaf0fa;font:18px system-ui;padding:32px'><h1>Nostr Pager</h1><p>Open this address in your browser:</p><p><a style='color:#68e0c4' href='"+name+"'>"+name+"</a></p>"+(lan?"<p>Or by IP: <a style='color:#68e0c4' href='"+url+"'>"+url+"</a></p>":"")+"<p>Accept the device's self-signed certificate, then enter the six-digit PIN shown on its display. Setup and messages require HTTPS and this PIN.</p></body>");
}
bool applyConfig(const pager::Configuration &config, String &error) {
    pagerSync::Lock guard;
    String ssid=config.ssid,password=config.password,url=config.relay,input=config.key;
    ssid.trim();url.trim();input.trim();
    if(ssid.isEmpty() || ssid.length()>32 || password.length()>64 || input.length()>512){error="Invalid Wi-Fi or key length.";return false;}
    if(config.open)password="";
    else if(password.isEmpty() && ssid==wifiSsid)password=wifiPassword;
    else if(password.isEmpty()){error="Enter a password or select Open network.";return false;}
    if(!password.isEmpty() && password.length()<8){error="Wi-Fi passwords need at least 8 characters.";return false;}
    keys::Relay r;if(!keys::relay(url.c_str(),r)){error="Enter a valid wss:// relay URL.";return false;}
    String pub=pager::publicKey,newSecret=secret;
    if(!input.isEmpty()) {
        keys::Key key;std::string keyError;
        if(!keys::decode(input.c_str(),config.rawPrivate,key,keyError)||!crypto::publicKey(key,pub)) {
            error=keyError.empty()?"Invalid secp256k1 key.":String(keyError.c_str());return false;
        }
        newSecret=key.secret?String(key.hex.c_str()):String("");
        std::fill(key.hex.begin(),key.hex.end(),'0');
    }
    if(pub.isEmpty()){error="Enter a Nostr public or private key.";return false;}
    std::vector<String> publicUrls,inboxes;
    if(!publicRelays::parse(config.publicRelays,publicUrls,error) || !publicRelays::parse(config.dmRelays,inboxes,error))return false;
    DynamicJsonDocument stored(4096);
    stored["ssid"]=ssid;stored["password"]=password;stored["pubkey"]=pub;stored["secret"]=newSecret;stored["relay"]=url;
    JsonArray publicList=stored.createNestedArray("publicRelays");for(const auto &endpoint:publicUrls)publicList.add(endpoint);
    JsonArray inboxList=stored.createNestedArray("dmRelays");for(const auto &endpoint:inboxes)inboxList.add(endpoint);
    String json;serializeJson(stored,json);
    if(preferences.putString("config",json)!=json.length()){error="Could not save configuration.";return false;}
    rebootAt=millis()+1500;
    return true;
}
void startPortal(const String &message) {
    if(!portalActive) {
        WiFi.mode(WIFI_AP_STA);
        apSsid="Nostr-Pager-"+String(uint32_t(ESP.getEfuseMac())&0xffff,HEX);
        apPassword=randomHex(6);
        if(!WiFi.softAP(apSsid.c_str(),apPassword.c_str())){setStatus("Setup Wi-Fi failed");return;}
        dns.start(53,"*",WiFi.softAPIP());
        server.on("/",HTTP_GET,[]{httpPage();});
        server.onNotFound([]{server.sendHeader("Location","http://"+server.client().localIP().toString()+"/",true);server.send(302,"text/plain","");});
        server.begin();portalActive=true;webPager::ensureStarted();
    }
    setStatus("Configuration mode");ui::portal(apSsid,apPassword,message);
}
void stopPortal() {
    server.stop();dns.stop();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);portalActive=false;
}
void doReconnect() {
    socket.disconnect();relayStarted=false;relayConnected=false;relayAuthId="";inboxState="Reconnect requested";
    publicInboxState="Reconnect requested";privateInboxState="Reconnect requested";
    publicRelays::reconnect();dmRelay::reconnect();lastPublish=millis()-30001;primaryAttempts=0;primaryRetryAt=0;relayLastError="";
    setStatus(WiFi.status()==WL_CONNECTED?"Reconnecting relays...":"Reconnect queued / waiting for Wi-Fi");
}
}
namespace pager {
String configurationPage(const String &token) { return setupPage(token); }
bool wifiConnected() { return WiFi.status()==WL_CONNECTED; }
bool configured() { return !wifiSsid.isEmpty() && !publicKey.isEmpty(); }
bool applyConfiguration(const Configuration &config, String &error) { return applyConfig(config,error); }
String publicRelayList(){String result;for(const auto &url:extraRelays){if(!result.isEmpty())result+='\n';result+=url;}return result;}
String dmRelayList(){String result;for(const auto &url:backupInboxes){if(!result.isEmpty())result+='\n';result+=url;}return result;}
void reconnectRelays() {pagerSync::post(doReconnect);}
bool saveRelayLists(const String &publicList,const String &dmList,String &error) {
    pagerSync::Lock guard;
    std::vector<String> publicUrls,inboxes;
    if(!publicRelays::parse(publicList,publicUrls,error) || !publicRelays::parse(dmList,inboxes,error))return false;
    DynamicJsonDocument doc(4096);
    if(deserializeJson(doc,preferences.getString("config","{}")) || doc["pubkey"]!=publicKey){error="Configure the board identity first.";return false;}
    JsonArray pubRows=doc["publicRelays"].to<JsonArray>();for(const auto &url:publicUrls)pubRows.add(url);
    JsonArray dmRows=doc["dmRelays"].to<JsonArray>();for(const auto &url:inboxes)dmRows.add(url);
    String json;serializeJson(doc,json);
    if(doc.overflowed() || preferences.putString("config",json)!=json.length()){error="Could not save relay lists.";return false;}
    extraRelays=std::move(publicUrls);backupInboxes=std::move(inboxes);
    pagerSync::post([]{
        dmRelay::begin(secret,publicKey,relayUrl,sendFrame,deliveryAck,backupInboxes,extraRelays);
        publicRelays::begin(publicKey,relayUrl,extraRelays,receiveMessage,deliveryAck,dmRelay::authenticate,canSend);
        publicRelays::inboxes(dmRelay::inboxRelays());doReconnect();
    });
    return true;
}
void relayRows(JsonArray rows) {
    JsonObject primary=rows.createNestedObject();primary["url"]=relayUrl;primary["role"]=canSend?"Primary inbox + public":"Primary public";
    primary["publicSubscribed"]=publicInboxState=="Subscribed / live mentions";primary["dmSubscribed"]=privateInboxState=="Subscribed / live DMs";
    primary["connected"]=relayConnected;primary["subscribed"]=inboxState=="Subscribed / live inbox";
    primary["state"]=inboxState;primary["error"]=relayLastError;primary["events"]=primaryInboxEvents;primary["failures"]=relayFailures;
    primary["retry"]=primaryRetryAt && int32_t(primaryRetryAt-millis())>0?(primaryRetryAt-millis()+999)/1000:0;
    primary["publication"]=pendingPublic?"Awaiting public acknowledgement":"No pending public send";
    for(const auto &entry:publicRelays::statuses()) {
        JsonObject row=rows.createNestedObject();row["url"]=entry.url;row["role"]=entry.dmInbox?(entry.publicRelay?"Public + backup DM inbox":"Backup DM inbox"):"Public";
        row["connected"]=entry.connected;row["subscribed"]=entry.subscribed;row["publicSubscribed"]=entry.publicSubscribed;row["dmSubscribed"]=entry.dmSubscribed;row["state"]=entry.state;row["error"]=entry.error;
        row["events"]=entry.events;row["failures"]=entry.failures;row["publication"]=entry.publicRelay?entry.publication:String("N/A (DM inbox only)");
        row["retry"]=entry.retryAt && int32_t(entry.retryAt-millis())>0?(entry.retryAt-millis()+999)/1000:0;
    }
}
bool restoreDefaultRelays(String &error) {
    return saveRelayLists(publicRelays::defaults(),"wss://relay.damus.io\nwss://nos.lol",error);
}
bool deleteMessages(const String &id,bool all,String &error) {
    pagerSync::Lock guard;
    if(!fsReady){error="History storage unavailable.";return false;}
    bool wasDirty=historyDirty;uint32_t oldSaveAt=saveAt;
    bool cancelPending=all || id==pendingId;
    uint32_t cutoff=uint32_t(time(nullptr));
    if(cutoff<1700000000){cutoff=historyFilter.before;for(const auto &message:messages)cutoff=std::max(cutoff,message.timestamp);}
    std::vector<String> removed;for(const auto &message:messages)if(all || message.id==id)removed.push_back(message.id);
    auto result=historyStore::remove(messages,historyFilter,id.c_str(),all,cutoff,saveHistory);
    if(result!=historyStore::Result::Ok) {
        historyDirty=wasDirty;saveAt=oldSaveAt;
        error=result==historyStore::Result::NotFound?"Message no longer cached":result==historyStore::Result::MarkersFull?"Deletion marker limit reached. Use Delete all to clear older history.":"Could not persist deletion. Messages were kept.";
        return false;
    }
    pagerSync::post([removed]{for(const auto &removedId:removed)publicRelays::cancel(removedId);});
    if(cancelPending && !pendingId.isEmpty()) {
        String wireId=pendingWireId;
        pagerSync::post([wireId]{dmRelay::cancel(wireId);publicRelays::cancel(wireId);});
        pendingId="";pendingWireId="";pendingEvent="";pendingPeer="";pendingModern=false;pendingPublic=false;
    }
    setStatus(all?"Local message history deleted":"Message deleted locally");ui::changed();return true;
}
void restartSoon(const String &reason) { pagerSync::Lock guard; setStatus(reason); rebootAt=millis()+1500; }
bool reset(bool eraseData,String &error) {
    pagerSync::Lock guard;
    // Deletion is performed during the next boot, before loading credentials
    // or starting HTTPS. A durable flag makes an interrupted wipe retryable.
    if(eraseData && resetStore.putBool("wipe",true)!=1){error="Could not schedule data deletion.";return false;}
    restartSoon(eraseData?"Deleting saved data / restarting...":"Restarting...");return true;
}
String messagingDiagnostics() {
    pagerSync::Lock guard;
    String result="Wi-Fi: ";result+=WiFi.status()==WL_CONNECTED?"connected":"offline";
    result+="\n"+dnsHealth::diagnostics();
    result+="\nIdentity: "+String(canSend?"private key / sending and DM decryption enabled":"public key / mentions only");
    result+="\nHistory storage: "+String(fsReady?"ready":"unavailable / sending blocked");
    result+="\nClock: "+String(time(nullptr)>=1700000000?"synchronized":"waiting for NTP / sending blocked");
    result+="\nRelay: "+relayUrl+String(relayConnected?" (connected)":" (offline)");
    result+="\nRelay failures this boot: "+String(relayFailures);
    if(!relayLastError.isEmpty())result+="\nRelay connection error: "+relayLastError;
    result+="\nInbox: "+inboxState;
    result+="\nPublic mention subscription: "+publicInboxState;
    result+="\nPrivate DM subscription: "+String(canSend?privateInboxState:String("disabled / private key required"));
    result+="\nInbox events this boot: "+String(inboxEvents)+" / skipped: "+String(inboxSkipped);
    result+="\nReceived kinds: public="+String(receivedNotes)+" / legacy DM="+String(receivedLegacy)+" / modern DM="+String(receivedModern);
    result+="\nAdded to inbox: "+String(acceptedMessages)+" / already cached: "+String(repeatedMessages);
    if(inboxSkipped)result+="\nLast skipped event: "+lastInboxSkip;
    result+="\nOutbox: "+String(pendingId.isEmpty()?"empty":pendingModern?"pending modern DM":"pending relay acknowledgement");
    result+="\nYour public key: "+String(keys::npub(publicKey.c_str()).c_str());
    return result;
}
void resume() {
    pagerSync::Lock guard;
    if(wifiSsid.isEmpty() || publicKey.isEmpty())return;
    resumeRequested=true;
}
void configure() {
    pagerSync::Lock guard;
    portalRequested=true;portalRequestMessage="Enter configuration in your phone browser.\nHold BOOT for 3 seconds to reopen setup.";
}
void markRead(const String &id) {
    pagerSync::Lock guard;
    for(auto &m:messages)if(m.id==id)m.read=true;
    historyDirty=true;saveAt=millis()+1000;
}
void older() {
    pagerSync::post([]{
        if(messages.size()>=HistoryLimit){setStatus("History cache full (60 messages)");return;}
        if(!relayConnected && publicRelays::statuses().empty()){setStatus("Connect a relay to fetch history");return;}
        if(relayConnected)subscribe(true);
        publicRelays::older(oldestFetched==UINT32_MAX?uint32_t(time(nullptr)):oldestFetched);
    });
}
bool send(const String &recipient,const String &text,bool publicNote,const String &replyId,String &error) {
    String peer;bool legacy=false;uint32_t floorTime=0;
    {
        pagerSync::Lock guard;
        if(!canSend){error="A private key is required.";return false;}
        if(!fsReady){error="History storage unavailable.";return false;}
        if(!pendingId.isEmpty()){error="Wait for the pending relay reply.";return false;}
        if(time(nullptr)<1700000000){error="Waiting for internet time.";return false;}
        if(text.isEmpty() || text.length()>512){error="Use 1-512 bytes of text.";return false;}
        if(!contacts::resolve(recipient,peer,error))return false;
        if(!publicNote && !replyId.isEmpty())for(const auto &message:messages)if(message.id==replyId && message.kind==4)legacy=true;
        floorTime=historyFilter.before+1;
    }
    // Signing and encryption run without the shared lock so the display and web pages stay responsive.
    DynamicJsonDocument doc(8192);JsonObject event=doc.to<JsonObject>();
    event["pubkey"]=publicKey;event["created_at"]=std::max(uint32_t(time(nullptr)),floorTime);event["kind"]=publicNote?1:legacy?4:14;
    String content=text;
    if(legacy && !crypto::encrypt(secret,peer,text,content)){error="Encryption failed.";return false;}
    event["content"]=content;JsonArray tags=event.createNestedArray("tags");
    JsonArray p=tags.createNestedArray();p.add("p");p.add(peer);
    if(!legacy && !replyId.isEmpty()) {JsonArray e=tags.createNestedArray();e.add("e");e.add(replyId);if(publicNote){e.add("");e.add("reply");}}
    Message m;m.peer=peer;m.text=text;m.kind=publicNote?1:legacy?4:14;
    m.timestamp=event["created_at"];m.outgoing=true;m.read=true;m.delivery="pending";
    if(!publicNote && !legacy) {
        if(!crypto::identify(event) || !crypto::wrap(event,secret,peer,m.pendingWire) || !crypto::wrap(event,secret,publicKey,m.event)) {
            error="Modern DM encryption failed.";return false;
        }
    } else {
        if(doc.overflowed() || !crypto::sign(event,secret)){error="Signing failed.";return false;}
        serializeJson(event,m.event);m.pendingWire=m.event;
    }
    m.id=event["id"].as<String>();
    String pendingText=m.pendingWire;
    DynamicJsonDocument wire(16384);if(deserializeJson(wire,pendingText)){error="Invalid outgoing message.";return false;}
    String wireId=wire["id"].as<String>();
    {
        pagerSync::Lock guard;
        if(!pendingId.isEmpty()){error="Wait for the pending relay reply.";return false;}
        pendingId=m.id;pendingModern=m.kind==14;pendingPublic=m.kind==1;pendingPeer=peer;pendingEvent=pendingText;pendingWireId=wireId;
        addMessage(std::move(m),false);
        if(!saveHistory()) {
            messages.erase(std::remove_if(messages.begin(),messages.end(),[](const Message &v){return v.id==pendingId;}),messages.end());
            pendingId="";pendingEvent="";pendingWireId="";pendingModern=false;error="Could not persist message.";ui::changed();return false;
        }
    }
    pagerSync::post([]{publish();if(!relayConnected)setStatus("Queued locally / waiting for relay");});
    return true;
}
}
bool persistContacts(const String &json) {
    if(!fsReady)return false;
    File file=LittleFS.open("/contacts.tmp","w");if(!file)return false;
    bool ok=file.print(json)==json.length();file.flush();file.close();
    if(ok)ok=LittleFS.rename("/contacts.tmp","/contacts.json");
    if(!ok)LittleFS.remove("/contacts.tmp");return ok;
}
void contactsChanged(){ui::changed();}
void setup() {
    pagerSync::begin();
    Serial.begin(115200);Serial.println("Nostr Pager firmware " PAGER_VERSION);pinMode(0,INPUT_PULLUP);
    if(!board::begin()){Serial.println("Display init failed: check PSRAM and board variant.");while(true)delay(1000);}
    heap_caps_malloc_extmem_enable(4096);
    if(!tlsMemory::begin()) {
        Serial.println("TLS PSRAM allocator unavailable: startup stopped.");
        while(true)delay(1000);
    }
    Serial.println("TLS allocator: PSRAM preferred / internal fallback");
    preferences.begin("nostr-pager",false);
    resetStore.begin("pager-reset",false);
    if(resetStore.getBool("wipe",false)) {
        Serial.println("Confirmed factory reset: deleting device data");
        // begin sets the partition label even when an old filesystem cannot mount.
        LittleFS.begin(false,"/littlefs",10,"littlefs");LittleFS.end();
        ResetStorage storage;bool erased=resetControl::eraseStoredData(storage);
        if(!erased) {
            pager::status="Data deletion failed / restart to retry";ui::begin();
            Serial.println("Factory reset failed; pending wipe will retry after restart.");
            while(true){ui::loop();if(rebootAt && int32_t(millis()-rebootAt)>=0)ESP.restart();delay(20);}
        }
        Serial.println("Factory reset complete");
    }
    DynamicJsonDocument doc(4096);
    if(!deserializeJson(doc,preferences.getString("config","{}"))) {
        wifiSsid=doc["ssid"]|"";wifiPassword=doc["password"]|"";
        pager::publicKey=doc["pubkey"]|"";secret=doc["secret"]|"";relayUrl=doc["relay"]|"wss://relay.damus.io";
    }
    pager::canSend=!secret.isEmpty();
    String publicList=publicRelays::defaults(),dmList="wss://relay.damus.io\nwss://nos.lol";
    if(doc["publicRelays"].is<JsonArray>()){publicList="";for(JsonVariantConst url:doc["publicRelays"].as<JsonArrayConst>()){if(!publicList.isEmpty())publicList+='\n';publicList+=url.as<String>();}}
    // A list still equal to the previous three defaults was never customised: move it to the current defaults.
    if(publicList=="wss://relay.damus.io\nwss://nos.lol\nwss://relay.primal.net")publicList=publicRelays::defaults();
    if(doc["dmRelays"].is<JsonArray>()){dmList="";for(JsonVariantConst url:doc["dmRelays"].as<JsonArrayConst>()){if(!dmList.isEmpty())dmList+='\n';dmList+=url.as<String>();}}
    String relayError;publicRelays::parse(publicList,extraRelays,relayError);publicRelays::parse(dmList,backupInboxes,relayError);
    dmRelay::begin(secret,pager::publicKey,relayUrl,sendFrame,deliveryAck,backupInboxes,extraRelays);
    publicRelays::begin(pager::publicKey,relayUrl,extraRelays,receiveMessage,deliveryAck,dmRelay::authenticate,pager::canSend);
    publicRelays::inboxes(dmRelay::inboxRelays());
    fsReady=LittleFS.begin(false,"/littlefs",10,"littlefs");
    if(!fsReady && !preferences.getBool("fs-init",false)) {
        fsReady=LittleFS.format() && LittleFS.begin(false,"/littlefs",10,"littlefs");
        if(fsReady)preferences.putBool("fs-init",true);
    } else if(fsReady)preferences.putBool("fs-init",true);
    String savedContacts="[]";
    if(fsReady && LittleFS.exists("/contacts.json")) {
        File file=LittleFS.open("/contacts.json","r");
        savedContacts=file && file.size()<=16384?file.readString():String("invalid");file.close();
    }
    contacts::begin(savedContacts,persistContacts,contactsChanged);
    loadHistory();
    WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);
    webPager::begin();ui::begin();
    configTime(0,0,"pool.ntp.org","time.cloudflare.com");
    if(wifiSsid.isEmpty() || pager::publicKey.isEmpty())startPortal("Configuration needed.\nConnect to the network above to start.");
    else {WiFi.begin(wifiSsid.c_str(),wifiPassword.c_str());connectAt=millis();setStatus("Connecting to Wi-Fi...");}
    // Relay and UI tasks run Schnorr/NIP-44 crypto, which needs more than 24 KB of stack (the loop task uses 32 KB).
    // Display first (higher priority), relay below the HTTPS server (priority 5) so web pages always win.
    xTaskCreatePinnedToCore(uiTask,"ui",32768,nullptr,4,&uiHandle,1);
    xTaskCreatePinnedToCore(relayTask,"relay",40960,nullptr,2,&relayHandle,0);
}
namespace {
// Relay task: owns every WebSocket and relay module. It holds the shared-state lock only for
// short non-blocking sections; network I/O and signature checks run with it released.
void relayStep() {
    pagerSync::drain();
    // Sockets are scarce (~10 in lwIP): HTTPS server 6, primary + DM discovery 2, setup portal 2 (then no extra relay), else one extra relay.
    publicRelays::setBudget(portalActive?0:1);
    if(dnsHealth::loop())doReconnect();
    // Setup adds an access point; it must not pause the saved station's inbox.
    if(WiFi.status()==WL_CONNECTED && !pager::publicKey.isEmpty()) {
        if(time(nullptr)<1700000000)setStatus("Wi-Fi connected / syncing time...");
        else if(!relayStarted)startRelay();
        if(relayStarted){
            pagerSync::blocking([]{socket.loop();});
            if(relayConnected && contacts::refreshPending()) {
                contacts::query(sendFrame);dmRelay::refreshContacts();contacts::refreshed();
            }
            String dmStatus=dmRelay::status();
            if(dmStatus!=shownDmStatus) {
                shownDmStatus=dmStatus;
                if(pendingModern || dmStatus.indexOf("relay list")>=0 || dmStatus.indexOf("DM inbox")>=0)setStatus(dmStatus);
            }
        }
        bool networkReady=time(nullptr)>=1700000000;
        dmRelay::loop(networkReady);publicRelays::inboxes(dmRelay::inboxRelays());publicRelays::loop(networkReady);
        bool connectionStatus=pager::status=="Relay offline / retrying..." || pager::status=="Connecting to relay..." || pager::status=="Reconnecting relays..." ||
            pager::status=="Online / backup DM inbox" || pager::status=="Public online / primary DM inbox offline" || pager::status=="Online / public backup relay";
        if(!relayConnected && pendingId.isEmpty() && connectionStatus) {
            bool backupOnline=false;
            for(const auto &row:publicRelays::statuses())if(row.connected && row.subscribed) {
                setStatus(row.dmInbox?"Online / backup DM inbox":pager::canSend?"Public online / primary DM inbox offline":"Online / public backup relay");backupOnline=true;break;
            }
            if(!backupOnline)setStatus("Relay offline / retrying...");
        }
        if(networkReady && !pendingId.isEmpty() && millis()-lastPublish>30000)publish();
    } else {
        dmRelay::loop(false);publicRelays::loop(false);
        if(relayStarted){socket.disconnect();relayStarted=false;relayConnected=false;setStatus("Wi-Fi offline / reconnecting...");}
    }
}
void relayTask(void *) {
    for(;;) {
        {pagerSync::Lock guard;relayStep();}
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
void uiTask(void *) {
    for(;;) {
        // Never wait long for the relay: skip a frame and retry instead of stalling touch input.
        {pagerSync::Lock guard(40);if(guard)ui::loop();}
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
}
void loop() {
    static uint32_t memoryAt=0;
    if(!memoryAt || millis()-memoryAt>=60000) {
        memoryAt=millis();auto memory=tlsMemory::stats();
        Serial.printf("TLS memory: internal free=%u largest=%u; PSRAM free=%u largest=%u; fallbacks=%u failures=%u\n",
            unsigned(memory.internalFree),unsigned(memory.internalLargest),unsigned(memory.psramFree),
            unsigned(memory.psramLargest),unsigned(memory.fallbacks),unsigned(memory.failures));
        Serial.printf("Stack free (words): worker=%u relay=%u ui=%u\n",unsigned(uxTaskGetStackHighWaterMark(nullptr)),
            relayHandle?unsigned(uxTaskGetStackHighWaterMark(relayHandle)):0U,uiHandle?unsigned(uxTaskGetStackHighWaterMark(uiHandle)):0U);
    }
    // Web worker: jobs and snapshots; takes the shared lock only briefly per operation.
    webPager::loop();
    bool startSetup=false,resumeNow=false;String setupMessage;
    {
        pagerSync::Lock guard;
        if(messageExpiry::prune(pager::messages,uint32_t(time(nullptr)))){historyDirty=true;saveAt=millis();ui::changed();}
        if(portalRequested){portalRequested=false;startSetup=true;setupMessage=portalRequestMessage;}
        resumeNow=resumeRequested;resumeRequested=false;
        if(WiFi.status()==WL_CONNECTED && !pager::publicKey.isEmpty())wifiLostAt=0;
        else {
            if(!wifiLostAt)wifiLostAt=millis();
            if(!portalActive && (!connectAt || millis()-connectAt>20000) && millis()-wifiLostAt>20000){startSetup=true;setupMessage="Wi-Fi connection failed.\nCheck network name and password.";}
        }
        // A local physical gesture always provides recovery from incorrect settings.
        if(digitalRead(0)==LOW) {
            if(!bootHold)bootHold=millis();
            else if(millis()-bootHold>3000 && !portalActive){startSetup=true;setupMessage="Setup requested with BOOT button.";}
        } else bootHold=0;
        if(historyDirty && int32_t(millis()-saveAt)>=0)saveHistory();
        if(rebootAt && int32_t(millis()-rebootAt)>=0)ESP.restart();
    }
    // Slow Wi-Fi/access-point calls run without the shared lock so the display and relay keep going.
    if(startSetup)startPortal(setupMessage);
    if(resumeNow) {
        if(portalActive)stopPortal();
        if(WiFi.status()!=WL_CONNECTED){WiFi.begin(wifiSsid.c_str(),wifiPassword.c_str());connectAt=millis();pagerSync::Lock guard;wifiLostAt=0;setStatus("Connecting to Wi-Fi...");}
        else {pagerSync::Lock guard;setStatus(relayConnected?"Online / "+String(pager::messages.size())+" messages":"Connecting to relay...");}
        ui::inbox();
    }
    if(portalActive){dns.processNextRequest();server.handleClient();}
    delay(5);
}
