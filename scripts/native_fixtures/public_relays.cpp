#include "public_relays.h"
#include "nostr_crypto.h"
#include <WebSocketsClient.h>
#include <Bitcoin.h>
#include <openssl/rand.h>
#include <cassert>
#include <iostream>
#include <set>
WebSocketsClient *WebSocketsClient::instance=nullptr;
uint32_t now=1;
extern "C" uint32_t millis(){return now;}
extern "C" void random_buffer(uint8_t *out,size_t size){assert(RAND_bytes(out,int(size))==1);}
String secret(unsigned n){return (std::string(63,'0')+char('0'+n)).c_str();}
String pub(unsigned n){uint8_t bytes[32]={};bytes[31]=n;return PrivateKey(bytes).publicKey().x();}
unsigned received=0,invalid=0,acks=0;
bool accepted=false;
String acknowledged;
void receive(JsonObjectConst event) {
    if(!crypto::verify(event)){++invalid;return;}
    if(event["kind"]==1059){DynamicJsonDocument rumor(8192);assert(crypto::unwrap(event,secret(1),pub(1),rumor));}
    ++received;
}
void ack(const String &id,bool ok,const String &){++acks;accepted=ok;acknowledged=id;publicRelays::clear(id);}
bool auth(const String &challenge,const String &url,String &wire,String &id) {
    DynamicJsonDocument doc(2048);JsonObject event=doc.to<JsonObject>();
    event["pubkey"]=pub(1);event["kind"]=22242;event["created_at"]=uint32_t(time(nullptr));event["content"]="";
    JsonArray tags=event.createNestedArray("tags"),r=tags.createNestedArray(),c=tags.createNestedArray();r.add("relay");r.add(url);c.add("challenge");c.add(challenge);
    assert(crypto::sign(event,secret(1)));id=event["id"].as<String>();String payload;serializeJson(event,payload);wire="[\"AUTH\","+payload+"]";return true;
}
String note(unsigned author,const String &to,const char *text) {
    DynamicJsonDocument doc(4096);JsonObject event=doc.to<JsonObject>();event["kind"]=1;event["pubkey"]=pub(author);event["created_at"]=uint32_t(time(nullptr));event["content"]=text;
    JsonArray tag=event.createNestedArray("tags").createNestedArray();tag.add("p");tag.add(to);assert(crypto::sign(event,secret(author)));String wire;serializeJson(event,wire);return wire;
}
void eoseOn(const char *host){auto *ws=WebSocketsClient::find(host);assert(ws);ws->emit("[\"EOSE\",\"public\"]");ws->emit("[\"EOSE\",\"dm-inbox\"]");}
void eoseAll(){for(auto *ws:std::vector<WebSocketsClient*>(WebSocketsClient::all()))ws->emit("[\"EOSE\",\"public\"]"),ws->emit("[\"EOSE\",\"dm-inbox\"]");}
void ok(const char *host,const String &id,bool value){auto *ws=WebSocketsClient::find(host);assert(ws);ws->emit(("[\"OK\",\""+id+"\","+String(value?"true":"false")+",\"restricted: test\"]").c_str());}
size_t live(){return WebSocketsClient::all().size();}
bool sentTo(const char *host,const String &id){auto *ws=WebSocketsClient::find(host);if(!ws)return false;for(const auto &frame:ws->sent)if(frame.frame.find(id.c_str())!=std::string::npos)return true;return false;}
void spin(unsigned passes=4){for(unsigned i=0;i<passes;++i)publicRelays::loop(true);}
int main() {
    std::vector<String> urls;String error;
    assert(publicRelays::parse(" WSS://bad.example",urls,error)==false);
    assert(publicRelays::parse(" wss://ONE.example:443/ \r\nwss://one.example\nwss://two.example",urls,error) && urls.size()==2 && urls[0]=="wss://one.example");
    auto original=urls;assert(!publicRelays::parse("ws://insecure.example",urls,error) && urls==original);
    assert(!publicRelays::parse("wss://a\nwss://b\nwss://c\nwss://d\nwss://e\nwss://f\nwss://g",urls,error));
    publicRelays::setBudget(3);
    urls.insert(urls.begin(),"wss://primary.example");
    publicRelays::begin(pub(1),"wss://primary.example/",urls,receive,ack,auth,true);
    publicRelays::inboxes({"wss://primary.example","wss://one.example","wss://dm.example"});
    assert(publicRelays::statuses().size()==3);
    // Several relays are connected at the same time.
    spin();assert(live()==3);
    auto *one=WebSocketsClient::find("one.example"),*two=WebSocketsClient::find("two.example"),*dm=WebSocketsClient::find("dm.example");
    assert(one && two && dm);
    for(const auto &row:publicRelays::statuses())assert(row.connected);
    DynamicJsonDocument frame(8192);assert(!deserializeJson(frame,one->sent.back().frame));assert(frame[0]=="REQ" && frame[1]=="dm-inbox" && frame.size()==4 && frame[2]["kinds"][0]==4 && frame[3]["kinds"][0]==1059);
    assert(frame[3]["limit"]==200 && frame[3]["since"].is<uint32_t>());
    assert(!deserializeJson(frame,one->sent[one->sent.size()-2].frame));assert(frame[1]=="public" && frame.size()==3 && frame[2]["kinds"][0]==1);
    eoseAll();
    // One relay failing leaves the others live.
    std::string failure="TLS certificate rejected";one->callback(WStype_ERROR,(uint8_t*)failure.data(),failure.size());
    spin();assert(live()==2 && !WebSocketsClient::find("one.example") && WebSocketsClient::find("two.example") && WebSocketsClient::find("dm.example"));
    assert(publicRelays::statuses()[0].failures==1 && publicRelays::statuses()[0].retryAt>now);
    two=WebSocketsClient::find("two.example");dm=WebSocketsClient::find("dm.example");
    String incoming=note(2,pub(1),"A public mention");two->emit(("[\"EVENT\",\"public\","+incoming+"]").c_str());assert(received==1);
    incoming=note(2,pub(3),"Wrong recipient");two->emit(("[\"EVENT\",\"public\","+incoming+"]").c_str());assert(received==1);
    incoming=note(2,pub(1),"DM-only relay must not supply public feed");dm->emit(("[\"EVENT\",\"public\","+incoming+"]").c_str());assert(received==1);
    DynamicJsonDocument rumor(8192);JsonObject message=rumor.to<JsonObject>();message["kind"]=14;message["pubkey"]=pub(2);message["created_at"]=uint32_t(time(nullptr));message["content"]="Backup inbox DM";
    JsonArray tag=message.createNestedArray("tags").createNestedArray();tag.add("p");tag.add(pub(1));assert(crypto::identify(message));String wrapped;assert(crypto::wrap(message,secret(2),pub(1),wrapped));
    dm->emit(("[\"EVENT\",\"public\","+wrapped+"]").c_str());assert(received==1); // Wrong subscription cannot inject a DM.
    dm->emit(("[\"EVENT\",\"dm-inbox\","+wrapped+"]").c_str());assert(received==2);
    assert(!publicRelays::publish(wrapped,message["id"].as<String>()));
    // Public publication reaches every online public relay immediately.
    String outgoing=note(1,pub(2),"Public fan-out"),id;
    assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();
    assert(publicRelays::publish(outgoing,id));assert(sentTo("two.example",id) && !sentTo("dm.example",id));
    ok("two.example","wrong id",true);assert(acks==0);
    two->emit("[\"AUTH\",\"challenge\"]");assert(!deserializeJson(frame,two->sent.back().frame));assert(frame[1]["kind"]==22242 && crypto::verify(frame[1]));String authId=frame[1]["id"].as<String>();
    size_t framesBefore=two->sent.size();ok("two.example",authId,true);assert(two->sent.size()>framesBefore);
    two->emit("[\"EOSE\",\"public\"]");
    two->emit("[\"CLOSED\",\"dm-inbox\",\"auth-required: authenticate first\"]");
    assert(publicRelays::statuses()[1].publicSubscribed && !publicRelays::statuses()[1].dmSubscribed);
    // The failed relay comes back after backoff and receives the queued note too.
    { // A relay refusing the kind-4 filter must still get a modern-only DM request.
        size_t beforeFrames=dm->sent.size();dm->emit("[\"CLOSED\",\"dm-inbox\",\"auth-required: requested filter requires authentication\"]");
        bool modernOnly=false;
        for(size_t k=beforeFrames;k<dm->sent.size();++k){DynamicJsonDocument f(4096);if(!deserializeJson(f,dm->sent[k].frame) && f[1]=="dm-inbox")modernOnly=f.size()==3 && f[2]["kinds"][0]==1059;}
        assert(modernOnly);
    }
    two->emit("[\"EOSE\",\"public\"]");eoseOn("dm.example");now+=20000;spin();one=WebSocketsClient::find("one.example");assert(one && live()==3 && sentTo("one.example",id));
    ok("two.example",id,false);publicRelays::primaryRejected(id,"Primary refused");assert(acks==0);
    ok("one.example",id,false);assert(acks==1 && !accepted); // all public relays refused
    outgoing=note(1,pub(2),"Second note accepted once");assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();assert(publicRelays::publish(outgoing,id));
    ok("two.example",id,true);assert(acks==2 && accepted && acknowledged==id);
    // Reconnect keeps a queued publication and resends it on every relay.
    outgoing=note(1,pub(2),"Reconnect retains queued publication");assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();assert(publicRelays::publish(outgoing,id));
    publicRelays::reconnect();assert(live()==0);spin();eoseAll();assert(live()==3 && sentTo("one.example",id) && sentTo("two.example",id));
    publicRelays::cancel(id);publicRelays::inboxes({});assert(publicRelays::statuses().size()==2 && !publicRelays::statuses()[0].dmInbox);
    spin();assert(live()==2);
    outgoing=note(1,pub(2),"Cancel pending publication");assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();assert(publicRelays::publish(outgoing,id));publicRelays::cancel(id);
    publicRelays::reconnect();publicRelays::loop(false);spin();eoseAll();for(auto *ws:WebSocketsClient::all())for(const auto &sentFrame:ws->sent)assert(sentFrame.frame.find(id.c_str())==std::string::npos);
    publicRelays::older(uint32_t(time(nullptr)));one=WebSocketsClient::find("one.example");assert(!deserializeJson(frame,one->sent.back().frame));assert(frame[1]=="public-older" && frame[2]["until"].is<uint32_t>());
    // More relays than the connection cap rotate in.
    publicRelays::begin(pub(1),"wss://primary.example",{"wss://r1.example","wss://r2.example","wss://r3.example","wss://r4.example","wss://r5.example"},receive,ack,auth,true);
    spin(8);eoseAll();assert(live()==3);
    std::set<std::string> seen;for(auto *ws:WebSocketsClient::all())seen.insert(ws->host);
    for(int round=0;round<4;++round){now+=30001;spin(8);eoseAll();assert(live()==3);for(auto *ws:WebSocketsClient::all())seen.insert(ws->host);}
    assert(seen.size()==5);
    // Exponential backoff, reset and millis rollover on a single relay.
    publicRelays::begin(pub(1),"wss://primary.example",{"wss://one.example"},receive,ack,auth,true);
    publicRelays::loop(true);one=WebSocketsClient::find("one.example");
    one->callback(WStype_ERROR,(uint8_t*)failure.data(),failure.size());
    uint32_t firstRetry=publicRelays::statuses()[0].retryAt;assert(firstRetry-now>=8000 && firstRetry-now<9000);
    publicRelays::loop(true);assert(live()==0);now=firstRetry-1;publicRelays::loop(true);assert(live()==0);
    now=firstRetry+1;publicRelays::loop(true);assert(live()==1);WebSocketsClient::find("one.example")->callback(WStype_ERROR,(uint8_t*)failure.data(),failure.size());
    uint32_t secondRetry=publicRelays::statuses()[0].retryAt;assert(secondRetry-now>=16000 && secondRetry-now<17000);
    publicRelays::loop(true);now=secondRetry+1;publicRelays::loop(true);eoseAll();
    now=0xfffffff0U;WebSocketsClient::find("one.example")->callback(WStype_ERROR,(uint8_t*)failure.data(),failure.size());
    uint32_t wrappedRetry=publicRelays::statuses()[0].retryAt;assert(wrappedRetry-now>=8000 && wrappedRetry-now<9000);
    publicRelays::loop(true);now=wrappedRetry-1;publicRelays::loop(true);assert(live()==0);
    now=wrappedRetry+1;publicRelays::loop(true);eoseAll();assert(publicRelays::statuses()[0].connected && publicRelays::statuses()[0].attempts==0);
    publicRelays::begin(pub(1),"wss://primary.example",{"wss://one.example"},receive,ack,nullptr,false);
    publicRelays::inboxes({"wss://dm.example"});assert(publicRelays::statuses().size()==1 && !publicRelays::statuses()[0].dmInbox);
    spin();eoseAll();
    outgoing=note(1,pub(2),"Accepted public replication expires");assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();
    assert(publicRelays::publish(outgoing,id));publicRelays::clear(id);now+=600001;publicRelays::loop(false);
    assert(publicRelays::statuses()[0].publication=="Replication window expired");
    spin();eoseAll();
    outgoing=note(1,pub(2),"Unacknowledged outbox must not expire");assert(!deserializeJson(frame,outgoing));id=frame["id"].as<String>();assert(publicRelays::publish(outgoing,id));
    publicRelays::loop(false);now+=600001;publicRelays::loop(true);spin();eoseAll();
    assert(sentTo("one.example",id));
    std::cout<<"Simultaneous relay connections, per-relay failure isolation and backoff/rollover, connection-cap rotation, authentication, public publication fan-out and failover, reconnect retention, cancellation and history fetching passed.\n";
}
