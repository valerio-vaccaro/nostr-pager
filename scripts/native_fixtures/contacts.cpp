#include "contacts.h"
#include "nostr_crypto.h"
#include <Bitcoin.h>
#include <openssl/rand.h>
#include <cassert>
#include <iostream>
extern "C" void random_buffer(uint8_t *out,size_t size){assert(RAND_bytes(out,int(size))==1);}
std::string flash;
bool failure=false;unsigned writes=0;
bool persist(const String &value){if(failure)return false;flash=value.c_str();++writes;return true;}
void changed(){}
String key(unsigned value){uint8_t bytes[32]={};bytes[31]=value;return keys::hex(bytes,32).c_str();}
String pub(unsigned value){keys::Key sk;sk.secret=true;sk.hex=key(value).c_str();String result;assert(crypto::publicKey(sk,result));return result;}
void profile(unsigned author,const char *name,uint32_t timestamp,bool damage=false) {
    DynamicJsonDocument content(2048),doc(8192);content["display_name"]=name;content["name"]="fallback";
    String text;serializeJson(content,text);
    JsonObject event=doc.to<JsonObject>();event["pubkey"]=pub(author);event["kind"]=0;event["created_at"]=timestamp;
    event["content"]=text;event.createNestedArray("tags");assert(crypto::sign(event,key(author)));
    if(damage)event["content"]="{\"display_name\":\"forged\"}";
    String wire;serializeJson(event,wire);DynamicJsonDocument frame(16384);
    assert(!deserializeJson(frame,"[\"EVENT\",\"contacts\","+wire+"]"));contacts::event(frame.as<JsonArrayConst>());
}
std::vector<std::string> frames;
void send(const String &wire){frames.push_back(wire.c_str());}
int main() {
    contacts::begin("[]",persist,changed);String error,resolved;
    const String npub=keys::npub(pub(1).c_str()).c_str();
    assert(contacts::save(npub,"",error));assert(contacts::all().size()==1 && contacts::all()[0].pubkey==pub(1));
    assert(contacts::refreshPending());contacts::query(send);contacts::refreshed();assert(!contacts::refreshPending());
    DynamicJsonDocument query(8192);assert(!deserializeJson(query,frames.back()));
    assert(query[0]=="REQ" && query[1]=="contacts" && query[2]["authors"][0]==pub(1) && query[2]["kinds"][0]==0);
    const uint32_t now=uint32_t(time(nullptr));
    profile(2,"stranger",now);assert(contacts::all()[0].name.isEmpty());
    profile(1,"forged",now,true);assert(contacts::all()[0].name.isEmpty());
    profile(1,"future",now+1000);assert(contacts::all()[0].name.isEmpty());
    profile(1,"Alice",now);assert(contacts::all()[0].name=="Alice");
    unsigned before=writes;profile(1,"Alice",now);profile(1,"stale",now-1);assert(writes==before);
    assert(contacts::resolve(" alice ",resolved,error) && resolved==pub(1));
    // Simulate reboot from the durable JSON; cached names work offline.
    contacts::begin(flash.c_str(),persist,changed);assert(contacts::display(pub(1))=="Alice");
    assert(contacts::save(pub(1),"Coffee friend",error));assert(contacts::all().size()==1);
    profile(1,"New Alice",now+1);assert(contacts::display(pub(1))=="Coffee friend");
    assert(contacts::resolve("coffee FRIEND",resolved,error) && resolved==pub(1));
    assert(contacts::save(pub(2),"Coffee friend",error));assert(!contacts::resolve("Coffee friend",resolved,error));
    assert(error.indexOf("multiple contacts")>=0);assert(contacts::resolve(pub(2),resolved,error) && resolved==pub(2));
    failure=true;auto durable=flash;assert(!contacts::remove(pub(1),error));assert(contacts::all().size()==2 && flash==durable);
    assert(!contacts::save(pub(3),"New",error));assert(contacts::all().size()==2);failure=false;
    assert(contacts::remove(pub(2),error));contacts::begin(flash.c_str(),persist,changed);assert(contacts::all().size()==1);
    assert(!contacts::save("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5","",error));
    assert(!contacts::save(pub(3),"bad\nname",error));assert(!contacts::save(pub(3),std::string(33,'x').c_str(),error));
    assert(contacts::save(pub(3),"Caffè",error));assert(contacts::resolve("Caffè",resolved,error));
    for(unsigned i=4;i<=25;++i)assert(contacts::save(pub(i),"",error));
    assert(contacts::all().size()==24);assert(!contacts::save(pub(26),"",error));
    contacts::begin(flash.c_str(),persist,changed);assert(contacts::all().size()==24);
    contacts::query(send);assert(!deserializeJson(query,frames.back()));assert(query.size()==26);
    contacts::begin("broken",persist,changed);assert(contacts::all().empty());assert(contacts::status().indexOf("could not be read")>=0);
    std::cout<<"Contact persistence/reboot, write failure rollback, signed profile binding/freshness, name ambiguity, key validation and capacity passed.\n";
}
