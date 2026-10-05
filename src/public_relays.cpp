#include "public_relays.h"
#include "key_codec.h"
#include "nostr_crypto.h"
#include "ca_bundle.h"
#include "inbox_requests.h"
#include "pager_sync.h"
#include <WebSocketsClient.h>
#include <memory>
#include <algorithm>
#include <cctype>
namespace {
// Several relays stay connected at once, bounded by the socket budget; extra relays rotate in
// when a connected one has been up for 30 s and another is waiting.
// lwIP allows ~10 sockets in total and the HTTPS server, setup servers, primary and DM-discovery
// relays already use most of them, so main.cpp sets this budget dynamically.
size_t budget=2;
struct Slot {
    std::unique_ptr<WebSocketsClient> ws;
    bool online=false;
    String authId;
    bool legacyDenied=false;
    uint32_t connectAt=0,connectedAt=0;
};
std::vector<publicRelays::Status> rows;
std::vector<Slot> slots;
std::vector<bool> olderPending;
String own,pendingWire,pendingId,primaryRejection;
String primaryUrl;
std::vector<String> monitoredInboxes;
publicRelays::Receiver receiveEvent=nullptr;
publicRelays::Ack acknowledgement=nullptr;
publicRelays::Auth authenticate=nullptr;
size_t cursor=0;
bool stopping=false;
bool privateMode=false,ackDelivered=false;
uint32_t lastPublish=0,olderUntil=0;
uint32_t queuedAt=0;
String canonical(const keys::Relay &relay) {
    std::string host=relay.host;
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c){return std::tolower(c);});
    return String("wss://")+host.c_str()+(relay.port==443?String(""):String(":")+String(relay.port))+
        (relay.path=="/"?String(""):String(relay.path.c_str()));
}
void send(size_t i,const String &wire){if(slots[i].ws)slots[i].ws->sendTXT(wire.c_str());}
void resetSubscriptions(size_t i){rows[i].connected=false;rows[i].subscribed=false;rows[i].publicSubscribed=false;rows[i].dmSubscribed=false;slots[i].online=false;}
void subscribe(size_t i,bool older=false) {
    auto request=[&](bool dmInbox) {
        DynamicJsonDocument doc(1536);
        const char *id=dmInbox?(older?"dm-older":"dm-inbox"):(older?"public-older":"public");
        inboxRequests::build(doc,id,own,dmInbox,false,dmInbox?60:20,older,olderUntil,uint32_t(time(nullptr)),!slots[i].legacyDenied);
        String wire;serializeJson(doc,wire);send(i,wire);
    };
    if(rows[i].publicRelay)request(false);
    if(rows[i].dmInbox && privateMode)request(true);
    if(!older){rows[i].subscribed=false;rows[i].publicSubscribed=false;rows[i].dmSubscribed=false;rows[i].state="Connected / subscribing";}
}
void sendPending(size_t i) {
    if(!slots[i].online || pendingId.isEmpty() || !rows[i].publicRelay || rows[i].rejected || rows[i].publication=="Accepted")return;
    send(i,"[\"EVENT\","+pendingWire+"]");lastPublish=millis();rows[i].publication="Awaiting acknowledgement";
}
void checkRejected() {
    if(ackDelivered || pendingId.isEmpty() || primaryRejection.isEmpty() || rows.empty())return;
    for(const auto &row:rows)if(row.publicRelay && !row.rejected)return;
    String id=pendingId,reason="All public relays rejected: "+primaryRejection;
    pendingWire="";pendingId="";if(acknowledgement)acknowledgement(id,false,reason);
}
void failed(size_t i,const String &reason) {
    auto &row=rows[i];resetSubscriptions(i);
    row.error=reason;row.state="Backoff / retry queued";++row.failures;
    row.attempts=std::min(unsigned(row.attempts)+1,6U);
    row.retryAt=millis()+(8000U<<(row.attempts-1))+(millis()%1000);
}
void subscribed(size_t i,bool dmInbox) {
    auto &row=rows[i];
    if(dmInbox)row.dmSubscribed=true;else row.publicSubscribed=true;
    row.subscribed=(!row.publicRelay || row.publicSubscribed) && (!row.dmInbox || !privateMode || row.dmSubscribed);
    if(row.subscribed)row.error="";
    row.attempts=0;row.retryAt=0;
    row.state=row.subscribed?(row.dmInbox?(row.publicRelay?"Live mentions + DM inbox":"Live DM inbox"):"Live public mentions"):
        "Partly subscribed / public and DM requests separate";
}
void event(size_t i,WStype_t type,uint8_t *payload,size_t length) {
    pagerSync::Lock guard;
    if(i>=rows.size() || i>=slots.size())return;
    auto &row=rows[i];auto &slot=slots[i];
    if(type==WStype_CONNECTED) {
        slot.online=true;row.connected=true;row.error="";slot.authId="";slot.legacyDenied=false;slot.connectedAt=millis();subscribe(i);
        if(olderPending[i]){subscribe(i,true);olderPending[i]=false;}
        sendPending(i);return;
    }
    if(type==WStype_DISCONNECTED || type==WStype_ERROR) {
        resetSubscriptions(i);
        if(!stopping)failed(i,payload && length?String(reinterpret_cast<char *>(payload),std::min(length,size_t(192))):String("Connection closed without a reason"));
        return;
    }
    if(type!=WStype_TEXT || length>16384)return;
    DynamicJsonDocument doc(24576);if(deserializeJson(doc,payload,length) || !doc.is<JsonArray>())return;
    String kind=doc[0]|"";
    if(kind=="EVENT" && (doc[1]=="public" || doc[1]=="public-older" || doc[1]=="dm-inbox" || doc[1]=="dm-older")) {
        JsonObjectConst message=doc[2].as<JsonObjectConst>();
        int eventKind=message["kind"]|-1;
        bool dmRequest=doc[1]=="dm-inbox" || doc[1]=="dm-older";
        if((eventKind==1 && dmRequest) || (eventKind!=1 && !dmRequest))return;
        if(eventKind==1 && !row.publicRelay)return;
        if(eventKind!=1 && (!(eventKind==4 || eventKind==1059) || !row.dmInbox || !privateMode))return;
        bool tagged=false;for(JsonArrayConst tag:message["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="p" && tag[1]==own)tagged=true;
        if(!tagged)return;
        ++row.events;subscribed(i,dmRequest);if(receiveEvent)receiveEvent(message);
    } else if(kind=="EOSE") {
        if(doc[1]=="public" && row.publicRelay)subscribed(i,false);
        else if(doc[1]=="dm-inbox" && row.dmInbox && privateMode)subscribed(i,true);
        else if(doc[1]=="public-older" || doc[1]=="dm-older")send(i,"[\"CLOSE\",\""+doc[1].as<String>()+"\"]");
    } else if(kind=="AUTH") {
        String wire;
        if(authenticate && authenticate(doc[1]|"",row.url,wire,slot.authId))send(i,wire);
        else row.error="Relay authentication requires a private key";
    } else if(kind=="OK" && doc[2].is<bool>()) {
        String id=doc[1]|"",reason=doc[3]|"";bool accepted=doc[2];
        if(!slot.authId.isEmpty() && id==slot.authId) {
            slot.authId="";
            if(accepted){slot.legacyDenied=false;slot.connectedAt=millis();subscribe(i);if(olderPending[i]){subscribe(i,true);olderPending[i]=false;}sendPending(i);}
            else row.error="Authentication rejected: "+reason.substring(0,128);
        } else if(!pendingId.isEmpty() && id==pendingId) {
            if(accepted){row.publication="Accepted";if(!ackDelivered && acknowledgement){ackDelivered=true;acknowledgement(id,true,"");}}
            else if(reason.startsWith("auth-required:"))row.publication="Authentication required / retrying";
            else {row.rejected=true;row.publication="Rejected: "+reason.substring(0,128);checkRejected();}
        }
    } else if(kind=="CLOSED" && (doc[1]=="public" || doc[1]=="public-older" || doc[1]=="dm-inbox" || doc[1]=="dm-older")) {
        row.error=String(doc[2]|"").substring(0,192);
        if(doc[1]=="dm-inbox" && row.dmInbox && privateMode && !slot.legacyDenied && row.error.startsWith("auth-required")) {
            slot.legacyDenied=true;row.error="";subscribe(i);return; // modern DMs only until authenticated
        }
        if(doc[1]=="public" || doc[1]=="dm-inbox") {
            if(doc[1]=="public")row.publicSubscribed=false;else row.dmSubscribed=false;
            row.subscribed=false;row.state=doc[1]=="public"?"Public subscription refused":"DM subscription refused";
        } else olderPending[i]=true;
    } else if(kind=="NOTICE")row.error=String(doc[1]|"").substring(0,192);
}
void stop(size_t i) {
    if(i>=slots.size() || !slots[i].ws)return;
    stopping=true;slots[i].ws->disconnect();stopping=false;
    slots[i].ws.reset();
    if(i<rows.size()){resetSubscriptions(i);rows[i].state=rows[i].retryAt?"Backoff / retry queued":"Queued for next scan";}
    slots[i].online=false;
}
void stopAll(){for(size_t i=0;i<slots.size();++i)stop(i);}
size_t activeCount(){size_t n=0;for(const auto &slot:slots)if(slot.ws)++n;return n;}
void connect(size_t i) {
    keys::Relay relay;
    if(!keys::relay(rows[i].url.c_str(),relay))return;
    auto &slot=slots[i];
    slot.ws.reset(new WebSocketsClient());
    slot.ws->onEvent([i](WStype_t type,uint8_t *payload,size_t length){event(i,type,payload,length);});
    slot.ws->beginSslWithBundle(relay.host.c_str(),relay.port,relay.path.c_str(),caBundle,"");
    slot.ws->setReconnectInterval(8000);slot.ws->enableHeartbeat(20000,5000,2);
    slot.online=false;slot.authId="";slot.connectAt=millis();rows[i].retryAt=0;rows[i].state="Connecting";
}
bool available(size_t i){return !slots[i].ws && (!rows[i].retryAt || int32_t(millis()-rows[i].retryAt)>=0);}
void startAvailable() {
    if(activeCount()>=budget)return;
    // Handshakes block the shared relay task; never start one while another is still connecting.
    for(const auto &slot:slots)if(slot.ws && !slot.online)return;
    for(size_t k=0;k<rows.size();++k) {
        size_t index=(cursor+k)%rows.size();
        if(available(index)){connect(index);cursor=index+1;return;} // one handshake per pass
    }
}
bool othersWaiting(){for(size_t i=0;i<rows.size();++i)if(available(i))return true;return false;}
}
namespace publicRelays {
String defaults(){return "wss://relay.damus.io\nwss://nos.lol\nwss://relay.primal.net\nwss://relay.nostr.band\nwss://offchain.pub\nwss://relay.snort.social";}
bool parse(const String &input,std::vector<String> &urls,String &error) {
    std::vector<String> parsed;
    if(input.length()>Limit*202){error="Public relay list is too long.";return false;}
    std::string text=input.c_str();size_t start=0;
    while(start<=text.size()) {
        size_t end=text.find('\n',start);String value=text.substr(start,end==std::string::npos?end:end-start).c_str();value.trim();
        if(!value.isEmpty()) {
            keys::Relay relay;if(!keys::relay(value.c_str(),relay)){error="Use one valid wss:// relay URL per line.";return false;}
            String url=canonical(relay);
            if(std::find(parsed.begin(),parsed.end(),url)==parsed.end())parsed.push_back(url);
            if(parsed.size()>Limit){error="Use at most 6 public relays.";return false;}
        }
        if(end==std::string::npos)break;start=end+1;
    }
    urls=std::move(parsed);return true;
}
void begin(const String &pub,const String &inbox,const std::vector<String> &urls,Receiver receive,Ack ack,Auth auth,bool canDecrypt) {
    stopAll();rows.clear();slots.clear();own=pub;receiveEvent=receive;acknowledgement=ack;authenticate=auth;
    pendingWire="";pendingId="";primaryRejection="";cursor=0;
    privateMode=canDecrypt;monitoredInboxes.clear();
    keys::Relay primary;primaryUrl=keys::relay(inbox.c_str(),primary)?canonical(primary):inbox;
    for(const auto &url:urls)if(url!=primaryUrl && rows.size()<Limit) {
        bool exists=false;for(const auto &row:rows)if(row.url==url)exists=true;
        if(!exists){Status row;row.url=url;rows.push_back(row);}
    }
    slots.resize(rows.size());
    olderPending.assign(rows.size(),false);
}
void inboxes(const std::vector<String> &urls) {
    std::vector<String> valid;
    if(privateMode)for(const auto &url:urls) {
        keys::Relay relay;if(!keys::relay(url.c_str(),relay))continue;String normal=canonical(relay);
        if(normal!=primaryUrl && std::find(valid.begin(),valid.end(),normal)==valid.end() && valid.size()<Limit)valid.push_back(normal);
    }
    if(valid==monitoredInboxes)return;
    stopAll();
    std::vector<Status> updated;
    for(auto row:rows)if(row.publicRelay){row.dmInbox=std::find(valid.begin(),valid.end(),row.url)!=valid.end();updated.push_back(row);}
    for(const auto &url:valid) {
        bool exists=false;for(const auto &row:updated)if(row.url==url)exists=true;
        if(!exists){Status row;row.url=url;row.publicRelay=false;row.dmInbox=true;updated.push_back(row);}
    }
    rows=std::move(updated);monitoredInboxes=std::move(valid);cursor=0;
    slots.clear();slots.resize(rows.size());
    olderPending.assign(rows.size(),false);
}
void loop(bool ready) {
    if(ackDelivered && !pendingId.isEmpty() && millis()-queuedAt>=600000) {
        for(auto &row:rows)if(row.publicRelay && (row.publication=="Queued for publication" || row.publication=="Awaiting acknowledgement"))row.publication="Replication window expired";
        pendingId="";pendingWire="";
    }
    if(!ready || own.isEmpty()){stopAll();return;}
    if(rows.empty())return;
    while(activeCount()>budget && !slots.empty()) {
        size_t victim=slots.size();
        for(size_t i=0;i<slots.size();++i)if(slots[i].ws)victim=i;
        if(victim==slots.size())break;
        stop(victim);
    }
    startAvailable();
    for(size_t i=0;i<rows.size();++i) {
        if(!slots[i].ws)continue;
        pagerSync::blocking([i]{if(i<slots.size() && slots[i].ws)slots[i].ws->loop();});
        if(!slots[i].ws)continue;
        auto &slot=slots[i];
        if(!slot.online && rows[i].retryAt)stop(i);
        else if(slot.online && !rows[i].subscribed && millis()-slot.connectedAt>=15000){failed(i,rows[i].error.isEmpty()?String("Inbox subscription not confirmed"):rows[i].error);stop(i);}
        else if(slot.online && millis()-slot.connectedAt>=30000 && othersWaiting())stop(i);
        else if(!slot.online && millis()-slot.connectAt>=15000){failed(i,"Connection timeout");stop(i);}
    }
    if(!pendingId.isEmpty() && millis()-lastPublish>=30000)for(size_t i=0;i<rows.size();++i)sendPending(i);
}
void setBudget(size_t sockets){budget=sockets;}
void reconnect(){stopAll();cursor=0;for(auto &row:rows){row.state="Queued";row.error="";row.retryAt=0;row.attempts=0;}}
void older(uint32_t until) {
    olderUntil=until;olderPending.assign(rows.size(),true);
    for(size_t i=0;i<rows.size();++i)if(slots[i].online){subscribe(i,true);olderPending[i]=false;}
}
bool publish(const String &wire,const String &id) {
    bool writable=false;for(const auto &row:rows)if(row.publicRelay)writable=true;
    if(!writable)return false;
    if(id==pendingId)return true;
    DynamicJsonDocument doc(8192);
    if(id.isEmpty() || deserializeJson(doc,wire) || doc["kind"]!=1 || doc["pubkey"]!=own || doc["id"]!=id || !crypto::verify(doc.as<JsonObjectConst>()))return false;
    pendingWire=wire;pendingId=id;primaryRejection="";
    ackDelivered=false;queuedAt=millis();
    for(auto &row:rows){row.rejected=false;row.publication="Queued for publication";}
    for(size_t i=0;i<rows.size();++i)sendPending(i);return true;
}
void primaryRejected(const String &id,const String &reason) {if(id==pendingId){primaryRejection=reason.isEmpty()?String("Inbox relay rejected"):reason;checkRejected();}}
void clear(const String &id) {if(id!=pendingId)return;ackDelivered=true;primaryRejection="";}
void cancel(const String &id) {if(id!=pendingId)return;pendingWire="";pendingId="";primaryRejection="";}
const std::vector<Status> &statuses(){return rows;}
}
