#include "dm_relay.h"
#include "nostr_crypto.h"
#include "ca_bundle.h"
#include "contacts.h"
#include "pager_sync.h"
#include <WebSocketsClient.h>
#include <vector>
#include <algorithm>
namespace {
WebSocketsClient auxiliary;
String secret,pub,configured,auxUrl,ownDmTags="[]",ownRelayTags="[]",announcementDm,announcementRelay;
String announcementDmId,announcementRelayId,authId,peer,delivery,deliveryId;
dmRelay::FrameSender mainSend=nullptr;
dmRelay::DeliveryAck deliveryAck=nullptr;
uint32_t ownDmTime=0,ownRelayTime=0,peerTime=0,announceAt=0,lookupAt=0,retryAt=0,publishedAt=0;
bool mainOnline=false,auxOnline=false,active=false,indexMode=true,ownReady=false,ownEose=false,peerEose=false;
bool announcementsSent=false;
uint32_t auxConnectAt=0,mainPublishAt=0,auxPublishAt=0;
unsigned shareIndex=0;
struct Publication {
    String url,result="not acknowledged";bool accepted=false,verified=false;
    explicit Publication(const String &endpoint):url(endpoint){}
};
std::vector<Publication> publications;
unsigned indexAttempt=0,target=0;
std::vector<String> peerRelays;
std::vector<String> savedInboxes;
std::vector<String> savedPublics;
String state="DM relay discovery not started";
const char *Indexes[]={"wss://relay.damus.io","wss://nos.lol","wss://purplepag.es"};
constexpr unsigned IndexCount=sizeof(Indexes)/sizeof(Indexes[0]);
Publication &publication(const String &url) {
    for(auto &entry:publications)if(entry.url==url)return entry;
    publications.emplace_back(url);return publications.back();
}
void sendAux(const String &frame) {auxiliary.sendTXT(frame.c_str());}
void request(dmRelay::FrameSender send,const String &id,const String &author,bool own) {
    DynamicJsonDocument doc(1024);JsonArray req=doc.to<JsonArray>();req.add("REQ");req.add(id);
    JsonObject filter=req.createNestedObject();filter.createNestedArray("authors").add(author);
    filter.createNestedArray("kinds").add(10050);filter["limit"]=1;
    if(own) {
        JsonObject general=req.createNestedObject();general.createNestedArray("authors").add(author);
        general.createNestedArray("kinds").add(10002);general["limit"]=1;
    }
    String wire;serializeJson(req,wire);send(wire);
}
void closeRequest(dmRelay::FrameSender send,const char *id) {send(String("[\"CLOSE\",\"")+id+"\"]");}
void publish(dmRelay::FrameSender send,const String &event) {send("[\"EVENT\","+event+"]");}
void checkPublished(dmRelay::FrameSender send) {
    DynamicJsonDocument doc(512);JsonArray frame=doc.to<JsonArray>();frame.add("REQ");frame.add("dm-check");
    JsonObject filter=frame.createNestedObject();filter.createNestedArray("ids").add(announcementDmId);
    filter["limit"]=1;String wire;serializeJson(frame,wire);send(wire);
}
void publishLists(dmRelay::FrameSender send,const String &url) {
    publish(send,announcementDm);publish(send,announcementRelay);checkPublished(send);
    publication(url);
}
String makeAnnouncement(int kind,const String &savedTags,uint32_t newest,String &id) {
    DynamicJsonDocument doc(16384),tags(8192);deserializeJson(tags,savedTags);
    JsonObject event=doc.to<JsonObject>();event["pubkey"]=pub;event["kind"]=kind;
    event["created_at"]=std::max(uint32_t(time(nullptr)),newest+1);event["content"]="";
    JsonArray output=event.createNestedArray("tags");bool present=false;
    if(kind==10050) {
        for(const auto &url:dmRelay::inboxRelays()){JsonArray tag=output.createNestedArray();tag.add("relay");tag.add(url);}
        if(doc.overflowed() || !crypto::sign(event,secret))return "";
        id=event["id"].as<String>();String wire;serializeJson(event,wire);return wire;
    }
    for(JsonArrayConst tag:tags.as<JsonArrayConst>()) {
        if(tag.size()<2)continue;
        if(kind==10050 && tag[0]=="relay" && tag[1]==configured)present=true;
        if(kind==10002 && tag[0]=="r" && tag[1]==configured) {
            if(!present){JsonArray both=output.createNestedArray();both.add("r");both.add(configured);present=true;}
            continue;
        }
        output.add(tag);
    }
    if(!present){JsonArray tag=output.createNestedArray();tag.add(kind==10050?"relay":"r");tag.add(configured);}
    if(kind==10050)for(const auto &url:savedInboxes) {
        bool found=false;for(JsonArrayConst tag:output)if(tag.size()>=2 && tag[0]=="relay" && tag[1]==url)found=true;
        if(!found){JsonArray tag=output.createNestedArray();tag.add("relay");tag.add(url);}
    }
    if(kind==10002)for(const auto &url:savedPublics) {
        bool found=false;
        for(JsonArray tag:output)if(tag.size()>=2 && tag[0]=="r" && tag[1]==url){if(tag.size()>2)tag.remove(2);found=true;}
        if(!found){JsonArray tag=output.createNestedArray();tag.add("r");tag.add(url);}
    }
    if(doc.overflowed() || !crypto::sign(event,secret))return "";
    id=event["id"].as<String>();String wire;serializeJson(event,wire);return wire;
}
void announce() {
    if((!mainOnline && !(auxOnline && indexMode)) || !ownReady || announcementsSent)return;
    announcementDm=makeAnnouncement(10050,ownDmTags,ownDmTime,announcementDmId);
    announcementRelay=makeAnnouncement(10002,ownRelayTags,ownRelayTime,announcementRelayId);
    if(announcementDm.isEmpty() || announcementRelay.isEmpty()){state="Could not sign DM relay announcement";return;}
    if(mainOnline){publishLists(mainSend,configured);mainPublishAt=millis();}
    if(auxOnline && indexMode){publishLists(sendAux,auxUrl);auxPublishAt=millis();}
    announcementsSent=true;state="Publishing DM inbox relay list...";
}
void queryOnConnect() {
    if(indexMode) {
        contacts::query(sendAux);
        request(sendAux,"dm-own",pub,true);
        if(!deliveryId.isEmpty())request(sendAux,"dm-peer",peer,false);
        if(announcementsSent){publishLists(sendAux,auxUrl);auxPublishAt=millis();}
    } else if(!deliveryId.isEmpty()){publish(sendAux,delivery);publishedAt=millis();state="Modern DM sent / waiting for relay";}
}
void process(JsonArrayConst frame,bool fromAux) {
    String type=frame[0]|"";
    if(type=="EVENT" && frame[1]=="dm-check") {
        JsonObjectConst event=frame[2].as<JsonObjectConst>();
        if(event["id"]==announcementDmId && event["pubkey"]==pub && event["kind"]==10050 && crypto::verify(event)) {
            auto &entry=publication(fromAux?auxUrl:configured);entry.verified=true;entry.result="verified by readback";
            state="DM inbox relay list published and verified";
            closeRequest(fromAux?sendAux:mainSend,"dm-check");
        }
        return;
    }
    if(type=="EVENT" && (frame[1]=="dm-own" || frame[1]=="dm-peer")) {
        JsonObjectConst event=frame[2].as<JsonObjectConst>();int kind=event["kind"]|0;
        uint32_t timestamp=event["created_at"]|0U;
        if((kind!=10050 && kind!=10002) || measureJson(event)>8192 || timestamp>uint32_t(time(nullptr))+600 || !crypto::verify(event))return;
        if(frame[1]=="dm-own" && event["pubkey"]==pub) {
            if(kind==10050 && timestamp>=ownDmTime) {ownDmTime=timestamp;ownDmTags="";serializeJson(event["tags"],ownDmTags);}
            if(kind==10002 && timestamp>=ownRelayTime) {ownRelayTime=timestamp;ownRelayTags="";serializeJson(event["tags"],ownRelayTags);}
        } else if(frame[1]=="dm-peer" && event["pubkey"]==peer && kind==10050 && timestamp>=peerTime) {
            peerTime=timestamp;peerRelays.clear();
            for(JsonArrayConst tag:event["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="relay") {
                String url=tag[1]|"";keys::Relay parsed;
                if(keys::relay(url.c_str(),parsed) && std::find(peerRelays.begin(),peerRelays.end(),url)==peerRelays.end() && peerRelays.size()<3)peerRelays.push_back(url);
            }
        }
    } else if(type=="EOSE") {
        if(frame[1]=="dm-own"){ownEose=true;closeRequest(fromAux?sendAux:mainSend,"dm-own");}
        if(frame[1]=="dm-peer"){peerEose=true;closeRequest(fromAux?sendAux:mainSend,"dm-peer");}
        if(frame[1]=="dm-check")closeRequest(fromAux?sendAux:mainSend,"dm-check");
    } else if(type=="OK" && frame[2].is<bool>()) {
        String id=frame[1]|"",reason=frame[3]|"";bool accepted=frame[2];
        if(!announcementDmId.isEmpty() && id==announcementDmId) {
            auto &entry=publication(fromAux?auxUrl:configured);
            entry.accepted=accepted;
            if(!entry.verified)entry.result=accepted?"acknowledged / awaiting readback":"rejected: "+reason.substring(0,96);
            if(accepted)checkPublished(fromAux?sendAux:mainSend);
            else if(!entry.verified)state="DM relay list rejected: "+reason.substring(0,96);
        }
        if(fromAux && id==authId && !authId.isEmpty()) {if(accepted)queryOnConnect();else state="DM relay authentication rejected";authId="";}
        // Only the recipient connection can acknowledge delivery. Discovery
        // relays never receive this wrapper and cannot complete the outbox.
        if(fromAux && !indexMode && id==deliveryId && !deliveryId.isEmpty()) {
            if(accepted) {
                deliveryAck(deliveryId,true,reason);deliveryId="";delivery="";peer="";retryAt=0;
                state="Modern DM accepted by recipient relay";
            } else if(reason.startsWith("auth-required:")) {state="DM relay requests authentication";}
            else {state="Recipient relay rejected DM: "+reason.substring(0,80);publishedAt=0;retryAt=millis()+1000;++target;}
        }
    } else if(fromAux && type=="AUTH") {
        String wire;if(dmRelay::authenticate(frame[1]|"",auxUrl,wire,authId))sendAux(wire);
    } else if(type=="CLOSED" && frame[1]=="dm-peer") {state="DM relay lookup denied: "+String(frame[2]|"").substring(0,80);}
}
void connectAux(const String &url,bool index) {
    // Mark offline before disconnect callbacks; no nested reconfiguration from callbacks.
    auxOnline=false;auxiliary.disconnect();auxOnline=false;auxUrl=url;indexMode=index;authId="";
    keys::Relay parsed;if(!keys::relay(url.c_str(),parsed)){state="Invalid DM relay URL";return;}
    auxiliary.beginSslWithBundle(parsed.host.c_str(),parsed.port,parsed.path.c_str(),caBundle,"");
    auxiliary.setReconnectInterval(8000);active=true;
    auxConnectAt=millis();auxPublishAt=0;
    if(!index)publishedAt=millis();
}
void auxiliaryEvent(WStype_t type,uint8_t *payload,size_t length) {
    pagerSync::Lock guard;
    if(type==WStype_CONNECTED){auxOnline=true;auxConnectAt=millis();queryOnConnect();return;}
    if(type==WStype_DISCONNECTED){auxOnline=false;return;}
    if(type!=WStype_TEXT || length>16384)return;
    DynamicJsonDocument doc(24576);if(!deserializeJson(doc,payload,length) && doc.is<JsonArray>()) {
        if(indexMode)contacts::event(doc.as<JsonArrayConst>());
        process(doc.as<JsonArrayConst>(),true);
    }
}
}
namespace dmRelay {
void begin(const String &key,const String &publicKey,const String &relay,FrameSender send,DeliveryAck ack,const std::vector<String> &inboxes,const std::vector<String> &publics) {
    secret=key;pub=publicKey;configured=relay;mainSend=send;deliveryAck=ack;auxiliary.onEvent(auxiliaryEvent);
    savedInboxes=inboxes;announceAt=millis()+7000;
    savedPublics=publics;
    if(secret.isEmpty())state="Private key required to publish DM inbox";
}
bool authenticate(const String &challenge,const String &url,String &frame,String &id) {
    if(secret.isEmpty() || challenge.isEmpty() || challenge.length()>512 || time(nullptr)<1700000000)return false;
    DynamicJsonDocument doc(2048);JsonObject event=doc.to<JsonObject>();
    event["kind"]=22242;event["pubkey"]=pub;event["created_at"]=uint32_t(time(nullptr));event["content"]="";
    JsonArray tags=event.createNestedArray("tags"),r=tags.createNestedArray(),c=tags.createNestedArray();r.add("relay");r.add(url);c.add("challenge");c.add(challenge);
    if(!crypto::sign(event,secret))return false;id=event["id"].as<String>();String json;serializeJson(event,json);frame="[\"AUTH\","+json+"]";return true;
}
void connected() {
    mainOnline=true;if(secret.isEmpty())return;
    request(mainSend,"dm-own",pub,true);ownEose=false;ownReady=false;announcementsSent=false;announceAt=millis()+7000;
    publications.clear();shareIndex=0;mainPublishAt=0;
    if(!deliveryId.isEmpty()){request(mainSend,"dm-peer",peer,false);lookupAt=millis()+7000;retryAt=0;}
    if(!active || !auxOnline)connectAux(Indexes[configured==Indexes[0]?1:0],true);
}
void disconnected() {mainOnline=false;}
void reconnect() {
    auxiliary.disconnect();active=false;auxOnline=false;ownReady=false;announcementsSent=false;
    announceAt=millis()+7000;shareIndex=0;
    if(!deliveryId.isEmpty()){indexAttempt=0;lookupAt=millis()+7000;retryAt=0;}
}
void cancel(const String &id) {
    if(id!=deliveryId)return;deliveryId="";delivery="";peer="";retryAt=0;lookupAt=0;publishedAt=0;
    if(!indexMode){auxiliary.disconnect();active=false;auxOnline=false;}
}
void event(JsonArrayConst frame) {if(!secret.isEmpty())process(frame,false);}
void deliver(const String &recipient,const String &wire,const String &id) {
    if(secret.isEmpty() || id.isEmpty() || id==deliveryId)return;
    peer=recipient;delivery=wire;deliveryId=id;peerRelays.clear();peerTime=0;peerEose=false;target=0;indexAttempt=0;
    lookupAt=millis()+7000;retryAt=0;publishedAt=0;state="Finding recipient DM inbox relays...";
    if(mainOnline)request(mainSend,"dm-peer",peer,false);
    if(auxOnline && indexMode && auxUrl==Indexes[0])request(sendAux,"dm-peer",peer,false);
    else connectAux(Indexes[0],true);
}
void loop(bool networkReady) {
    if(secret.isEmpty())return;
    if(!networkReady){auxiliary.disconnect();auxOnline=false;active=false;return;}
    if(!active)connectAux(Indexes[0],true);
    if(active)pagerSync::blocking([]{auxiliary.loop();});
    // A relay that omits EOSE must not prevent publishing indefinitely.
    if(!ownReady && int32_t(millis()-announceAt)>=0)ownReady=true;
    announce();
    if(mainOnline && announcementsSent && !publication(configured).verified && millis()-mainPublishAt>=10000) {
        publishLists(mainSend,configured);mainPublishAt=millis();
    }
    if(deliveryId.isEmpty()) {
        if(indexMode && !auxOnline && millis()-auxConnectAt>=15000) {
            shareIndex=(shareIndex+1)%IndexCount;connectAux(Indexes[shareIndex],true);return;
        }
        if(announcementsSent && indexMode && shareIndex>=IndexCount && millis()-auxConnectAt>=300000) {
            shareIndex=0;connectAux(Indexes[shareIndex],true);return;
        }
        // Delivery temporarily borrows the index socket; resume metadata
        // sharing afterwards so an outgoing DM cannot interrupt publication.
        if(announcementsSent && !indexMode){shareIndex=0;connectAux(Indexes[0],true);return;}
        // Keep each index connected until readback succeeds or its bounded
        // publication window expires; retry once before moving on.
        if(announcementsSent && indexMode) {
            if(auxOnline && auxPublishAt && !publication(auxUrl).verified && millis()-auxPublishAt>=10000) {
                publishLists(sendAux,auxUrl);auxPublishAt=millis();
            }
            if(publication(auxUrl).verified || millis()-auxConnectAt>=20000) {
                while(shareIndex<IndexCount && (String(Indexes[shareIndex])==auxUrl || String(Indexes[shareIndex])==configured))++shareIndex;
                if(shareIndex<IndexCount)connectAux(Indexes[shareIndex++],true);
            }
        }
        return;
    }
    if(indexMode && lookupAt && ((peerEose && !peerRelays.empty()) || int32_t(millis()-lookupAt)>=0)) {
        if(!peerRelays.empty()) {lookupAt=0;target=0;connectAux(peerRelays[0],false);}
        else if(++indexAttempt<IndexCount) {lookupAt=millis()+7000;connectAux(Indexes[indexAttempt],true);}
        else {state="Recipient has no DM inbox relay list / retrying";lookupAt=0;retryAt=millis()+60000;}
    }
    bool timedOut=!indexMode && publishedAt && millis()-publishedAt>20000;
    if(timedOut)++target;
    if(!indexMode && (timedOut || (retryAt && int32_t(millis()-retryAt)>=0))) {
        publishedAt=0;retryAt=0;
        if(target>=peerRelays.size()) {target=0;retryAt=millis()+60000;state="Recipient DM relays unavailable / retrying";}
        else connectAux(peerRelays[target],false);
    }
    if(indexMode && retryAt && int32_t(millis()-retryAt)>=0) {
        retryAt=0;indexAttempt=0;lookupAt=millis()+7000;
        if(auxOnline && auxUrl==Indexes[0])request(sendAux,"dm-peer",peer,false);else connectAux(Indexes[0],true);
    }
}
String status() {return state;}
void refreshContacts(){if(auxOnline && indexMode)contacts::query(sendAux);}
std::vector<String> inboxRelays() {
    std::vector<String> urls;if(secret.isEmpty())return urls;
    urls.push_back(configured);
    for(const auto &url:savedInboxes)if(std::find(urls.begin(),urls.end(),url)==urls.end() && urls.size()<7)urls.push_back(url);
    DynamicJsonDocument tags(8192);
    if(!deserializeJson(tags,ownDmTags))for(JsonArrayConst tag:tags.as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="relay") {
        String url=tag[1]|"";keys::Relay parsed;
        if(keys::relay(url.c_str(),parsed) && std::find(urls.begin(),urls.end(),url)==urls.end() && urls.size()<7)urls.push_back(url);
    }
    return urls;
}
void publicationRows(JsonArray rows) {
    for(const auto &entry:publications){JsonObject row=rows.createNestedObject();row["url"]=entry.url;row["accepted"]=entry.accepted;row["verified"]=entry.verified;row["result"]=entry.result;}
}
String publicationStatus() {
    if(secret.isEmpty())return "Private key required to publish DM inbox";
    if(!announcementsSent)return "Waiting for relay connection / metadata discovery";
    String result="Kind 10050 inbox list: ";unsigned verified=0;
    for(const auto &entry:publications)if(entry.verified)++verified;
    result+=String(verified)+" relay(s) verified.";
    for(const auto &entry:publications)result+="\n"+entry.url+": "+entry.result;
    return result;
}
}
