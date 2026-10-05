#include "dm_relay.h"
#include "nostr_crypto.h"
#include <WebSocketsClient.h>
#include <Bitcoin.h>
#include <openssl/rand.h>
#include <cassert>
#include <algorithm>
#include <iostream>
WebSocketsClient *WebSocketsClient::instance=nullptr;
uint32_t now=1;
extern "C" uint32_t millis() {return now;}
extern "C" void random_buffer(uint8_t *out,size_t size) {assert(RAND_bytes(out,int(size))==1);}
std::vector<std::string> frames;
std::string accepted;
void send(const String &frame) {frames.emplace_back(frame.c_str());}
void ack(const String &id,bool ok,const String &) {assert(ok);accepted=id.c_str();}
String key(unsigned value) {return (std::string(63,'0')+char('0'+value)).c_str();}
String pub(unsigned value) {uint8_t bytes[32]={};bytes[31]=value;return PrivateKey(bytes).publicKey().x();}
void event(const String &secret,const char *sub,int kind,const std::vector<std::vector<String>> &tags) {
    keys::Key sk;sk.secret=true;sk.hex=secret.c_str();String own;assert(crypto::publicKey(sk,own));
    DynamicJsonDocument doc(8192);JsonObject e=doc.to<JsonObject>();e["kind"]=kind;e["pubkey"]=own;e["created_at"]=uint32_t(time(nullptr));e["content"]="";
    JsonArray arr=e.createNestedArray("tags");for(const auto &tag:tags){JsonArray a=arr.createNestedArray();for(const auto &v:tag)a.add(v);}
    assert(crypto::sign(e,secret));String wire;serializeJson(e,wire);
    DynamicJsonDocument frame(16384);assert(!deserializeJson(frame,"[\"EVENT\",\""+String(sub)+"\","+wire+"]"));dmRelay::event(frame.as<JsonArrayConst>());
}
void input(const char *text) {DynamicJsonDocument doc(1024);assert(!deserializeJson(doc,text));dmRelay::event(doc.as<JsonArrayConst>());}
int main(int argc,char **argv) {
    if(argc>1 && std::string(argv[1])=="offline") {
        dmRelay::begin(key(1),pub(1),"wss://offline-primary.example",send,ack,{"wss://backup-inbox.example"},{"wss://public.example"});
        dmRelay::loop();auto *aux=WebSocketsClient::instance;assert(aux->host=="relay.damus.io");
        event(key(1),"dm-own",10050,{{"relay","wss://another-device.example"}});
        now+=7001;dmRelay::loop();assert(frames.empty());
        bool inbox=false,general=false;
        for(const auto &sent:aux->sent) {
            DynamicJsonDocument frame(16384);assert(!deserializeJson(frame,sent.frame));
            if(frame[0]!="EVENT")continue;JsonObjectConst e=frame[1];assert(crypto::verify(e));
            std::vector<std::string> urls;for(JsonArrayConst tag:e["tags"].as<JsonArrayConst>())urls.emplace_back(tag[1].as<const char*>());
            if(e["kind"]==10050){inbox=true;assert(std::find(urls.begin(),urls.end(),"wss://backup-inbox.example")!=urls.end());assert(std::find(urls.begin(),urls.end(),"wss://another-device.example")!=urls.end());}
            if(e["kind"]==10002){general=true;assert(std::find(urls.begin(),urls.end(),"wss://public.example")!=urls.end());}
        }
        assert(inbox && general && dmRelay::inboxRelays().size()==3);
        const std::string id(64,'c');dmRelay::deliver(pub(2),"{}",id.c_str());
        event(key(2),"dm-peer",10050,{{"relay","wss://recipient.example"}});
        now+=7001;dmRelay::loop();dmRelay::loop();assert(aux->host=="recipient.example");
        aux->emit(("[\"OK\",\""+String(id)+"\",true,\"\"]").c_str());assert(accepted==id);
        dmRelay::disconnected();dmRelay::loop(false);dmRelay::loop(true);dmRelay::loop(true);
        assert(aux->host=="relay.damus.io");
        std::cout<<"Primary-offline metadata publication, backup/general relay announcements, discovered own inboxes and recipient DM delivery passed.\n";return 0;
    }
    if(argc>1) {
        // No EOSE or OK from the configured relay: publication must still
        // proceed, retry, and visit every discovery endpoint.
        dmRelay::begin(key(1),pub(1),"wss://pager.example",send,ack);dmRelay::connected();dmRelay::loop();
        auto *aux=WebSocketsClient::instance;
        now+=7001;dmRelay::loop();
        std::string announcement;
        for(const auto &wire:frames) {
            DynamicJsonDocument doc(16384);assert(!deserializeJson(doc,wire));
            if(doc[0]=="EVENT" && doc[1]["kind"]==10050){assert(crypto::verify(doc[1]));serializeJson(doc[1],announcement);}
        }
        assert(!announcement.empty());assert(dmRelay::publicationStatus().indexOf("0 relay(s) verified")>=0);
        size_t before=frames.size();now+=10001;dmRelay::loop();assert(frames.size()>before);
        DynamicJsonDocument eventDoc(16384);assert(!deserializeJson(eventDoc,announcement));String id=eventDoc["id"].as<String>();
        input(("[\"OK\",\""+id+"\",true,\"\"]").c_str());
        assert(dmRelay::publicationStatus().indexOf("0 relay(s) verified")>=0); // OK is not proof of storage.
        input(("[\"EVENT\",\"dm-check\","+String(announcement)+"]").c_str());
        assert(dmRelay::publicationStatus().indexOf("1 relay(s) verified")>=0);
        before=frames.size();now+=10001;dmRelay::loop();assert(frames.size()==before); // Verified main stops retrying.
        dmRelay::loop();assert(aux->host=="nos.lol");
        aux->emit(("[\"EVENT\",\"dm-check\","+String(announcement)+"]").c_str());
        assert(dmRelay::publicationStatus().indexOf("2 relay(s) verified")>=0);
        dmRelay::loop();dmRelay::loop();assert(aux->host=="purplepag.es");
        bool shared=false;for(const auto &sent:aux->sent)if(sent.host=="purplepag.es" && sent.frame.find(announcement)!=std::string::npos)shared=true;
        assert(shared);
        aux->emit(("[\"OK\",\""+id+"\",false,\"restricted: test\"]").c_str());
        assert(dmRelay::publicationStatus().indexOf("rejected: restricted: test")>=0);
        std::cout<<"No-EOSE publication, unacknowledged retry, readback verification and three-index sharing passed.\n";
        return 0;
    }
    dmRelay::begin(key(1),pub(1),"wss://pager.example",send,ack);dmRelay::connected();dmRelay::loop();
    auto *aux=WebSocketsClient::instance;assert(aux->host=="relay.damus.io");
    event(key(1),"dm-own",10050,{{"relay","wss://existing.example"}});
    event(key(1),"dm-own",10002,{{"r","wss://old.example","write"}});
    input("[\"EOSE\",\"dm-own\"]");now+=7001;dmRelay::loop();
    bool inbox=false,general=false;
    for(auto &wire:frames) {
        DynamicJsonDocument doc(16384);assert(!deserializeJson(doc,wire));
        if(doc[0]!="EVENT")continue;JsonObjectConst e=doc[1].as<JsonObjectConst>();assert(crypto::verify(e));
        std::vector<std::string> urls;for(JsonArrayConst tag:e["tags"].as<JsonArrayConst>())urls.emplace_back(tag[1].as<const char*>());
        assert(std::find(urls.begin(),urls.end(),"wss://pager.example")!=urls.end());
        if(e["kind"]==10050){inbox=true;assert(std::find(urls.begin(),urls.end(),"wss://existing.example")!=urls.end());}
        if(e["kind"]==10002){general=true;assert(std::find(urls.begin(),urls.end(),"wss://old.example")!=urls.end());}
    }
    assert(inbox && general);
    const std::string id(64,'a');String outgoing=("{\"id\":\""+id+"\",\"kind\":1059}").c_str();
    dmRelay::deliver(pub(2),outgoing,id.c_str());
    aux->emit("[\"OK\",\""+id+"\",true,\"\"]");
    assert(accepted.empty()); // An index connection cannot acknowledge delivery.
    event(key(3),"dm-peer",10050,{{"relay","wss://malicious.example"}}); // Wrong author must be ignored.
    event(key(2),"dm-peer",10050,{{"relay","wss://recipient.example"},{"relay","wss://backup.example"}});
    input("[\"EOSE\",\"dm-peer\"]");
    const auto lookupFinished=now;
    dmRelay::loop();dmRelay::loop();assert(aux->host=="recipient.example");
    assert(now==lookupFinished); // Valid discovery completes without the seven-second timeout.
    bool published=false;for(const auto &sent:aux->sent)if(sent.host=="recipient.example" && sent.frame.find(id)!=std::string::npos)published=true;
    assert(published);
    aux->emit("[\"AUTH\",\"challenge-123\"]");
    DynamicJsonDocument auth(8192);assert(!deserializeJson(auth,aux->sent.back().frame));assert(auth[0]=="AUTH");
    assert(auth[1]["kind"]==22242 && crypto::verify(auth[1].as<JsonObjectConst>()));
    assert(auth[1]["tags"][0][1]=="wss://recipient.example");assert(auth[1]["tags"][1][1]=="challenge-123");
    String authId=auth[1]["id"].as<String>();aux->emit(("[\"OK\",\""+authId+"\",true,\"\"]").c_str());
    // A rejected relay should try the next advertised endpoint, never an unlisted one.
    aux->emit("[\"OK\",\""+id+"\",false,\"restricted: example\"]");now+=1001;dmRelay::loop();dmRelay::loop();assert(aux->host=="backup.example");
    aux->emit("[\"OK\",\""+id+"\",true,\"\"]");assert(accepted==id);
    dmRelay::loop();dmRelay::loop();assert(aux->host=="relay.damus.io"); // Resume own metadata publication after delivery.
    const std::string missing(64,'b');size_t sentBefore=aux->sent.size();
    dmRelay::deliver(pub(3),"{}",missing.c_str());dmRelay::loop();now+=7001;dmRelay::loop();dmRelay::loop();now+=7001;dmRelay::loop();dmRelay::loop();now+=7001;dmRelay::loop();
    assert(dmRelay::status().indexOf("no DM inbox relay list")>=0);
    for(size_t i=sentBefore;i<aux->sent.size();++i)assert(aux->sent[i].frame.find("[\"EVENT\",{}]")==std::string::npos);
    dmRelay::disconnected();now+=60001;dmRelay::connected();dmRelay::loop();
    event(key(3),"dm-peer",10050,{{"relay","wss://new.example"}});now+=7001;dmRelay::loop();dmRelay::loop();assert(aux->host=="new.example");
    std::cout<<"Signed inbox/relay announcements preserve existing entries; authenticated recipient routing, failover, missing-inbox retry and reconnect passed.\n";
}
