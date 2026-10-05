#include "contacts.h"
#include "nostr_crypto.h"
#include <algorithm>
#include <cctype>
namespace {
std::vector<contacts::Contact> entries;
contacts::Persist persist=nullptr;
contacts::Changed changed=nullptr;
bool pendingRefresh=false;
String state="No contacts saved";
std::string trim(const String &value) {
    std::string text=value.c_str();size_t a=text.find_first_not_of(" \t\r\n"),b=text.find_last_not_of(" \t\r\n");
    return a==std::string::npos?std::string():text.substr(a,b-a+1);
}
std::string folded(const String &value) {
    auto text=trim(value);for(char &c:text)if(static_cast<unsigned char>(c)<128)c=std::tolower(static_cast<unsigned char>(c));return text;
}
bool nameValid(const std::string &text,size_t maximum) {
    if(text.size()>maximum)return false;
    // Validate UTF-8 and reject controls, including newline and embedded NUL.
    for(size_t i=0;i<text.size();) {
        uint8_t c=text[i++];if(c<32 || c==127)return false;if(c<128)continue;
        unsigned count;uint32_t code;
        if(c>=0xc2 && c<=0xdf){count=1;code=c&31;}
        else if(c>=0xe0 && c<=0xef){count=2;code=c&15;}
        else if(c>=0xf0 && c<=0xf4){count=3;code=c&7;}else return false;
        unsigned n=count;if(i+count>text.size())return false;
        while(count--){uint8_t next=text[i++];if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
        if((n==2 && code<0x800) || (n==3 && code<0x10000) || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return false;
    }
    return true;
}
bool publicKey(const String &input,String &pubkey) {
    keys::Key key;std::string error;
    return keys::decode(input.c_str(),false,key,error) && !key.secret && crypto::publicKey(key,pubkey);
}
bool commit(const std::vector<contacts::Contact> &next,String &error) {
    DynamicJsonDocument doc(16384);JsonArray array=doc.to<JsonArray>();
    for(const auto &contact:next) {
        JsonObject item=array.createNestedObject();item["pubkey"]=contact.pubkey;item["alias"]=contact.alias;
        item["name"]=contact.name;item["updated"]=contact.updated;item["profileId"]=contact.profileId;
    }
    String saved;serializeJson(doc,saved);
    if(doc.overflowed() || !persist || !persist(saved)){error="Could not save contacts to flash.";return false;}
    entries=next;if(changed)changed();return true;
}
}
namespace contacts {
void begin(const String &saved,Persist writer,Changed callback) {
    entries.clear();persist=writer;changed=callback;pendingRefresh=false;
    DynamicJsonDocument doc(16384);
    if(deserializeJson(doc,saved) || !doc.is<JsonArray>()){state="Contact storage could not be read";return;}
    for(JsonObjectConst item:doc.as<JsonArrayConst>()) {
        String pubkey;if(!publicKey(item["pubkey"]|"",pubkey))continue;
        if(entries.size()>=Limit || std::any_of(entries.begin(),entries.end(),[&](const Contact &c){return c.pubkey==pubkey;}))continue;
        Contact c;c.pubkey=pubkey;c.alias=item["alias"]|"";c.name=item["name"]|"";
        if(!nameValid(c.alias.c_str(),32) || !nameValid(c.name.c_str(),64))continue;
        c.updated=item["updated"]|0U;c.profileId=item["profileId"]|"";
        entries.push_back(c);
    }
    pendingRefresh=!entries.empty();state=entries.empty()?"No contacts saved":"Saved contacts loaded";
}
const std::vector<Contact> &all(){return entries;}
String label(const Contact &c){return !c.alias.isEmpty()?c.alias:!c.name.isEmpty()?c.name:String("npub ")+c.pubkey.substring(0,8);}
String display(const String &pubkey){for(const auto &c:entries)if(c.pubkey==pubkey)return label(c);return pubkey.substring(0,10);}
bool save(const String &input,const String &alias,String &error) {
    String pubkey;if(!publicKey(input,pubkey)){error="Enter a public npub, hex key or nprofile. Private keys are not contacts.";return false;}
    std::string nickname=trim(alias);
    if(!nameValid(nickname,32)){error="Use a nickname of at most 32 UTF-8 bytes without controls.";return false;}
    auto next=entries;auto found=std::find_if(next.begin(),next.end(),[&](const Contact &c){return c.pubkey==pubkey;});
    if(found==next.end()) {
        if(next.size()>=Limit){error="Address book is full (24 contacts).";return false;}
        Contact contact;contact.pubkey=pubkey;next.push_back(contact);found=next.end()-1;
    }
    found->alias=nickname.c_str();
    if(!commit(next,error))return false;
    refresh();state="Contact saved / profile lookup queued";return true;
}
bool remove(const String &input,String &error) {
    String pubkey;if(!publicKey(input,pubkey)){error="Invalid contact key.";return false;}
    auto next=entries;auto end=std::remove_if(next.begin(),next.end(),[&](const Contact &c){return c.pubkey==pubkey;});
    if(end==next.end()){error="Contact not found.";return false;}next.erase(end,next.end());
    if(!commit(next,error))return false;refresh();state="Contact removed";return true;
}
bool resolve(const String &input,String &pubkey,String &error) {
    if(publicKey(input,pubkey))return true;
    auto name=folded(input);unsigned matches=0;
    for(const auto &c:entries)if(!name.empty() && name==folded(label(c))){pubkey=c.pubkey;++matches;}
    if(matches==1)return true;pubkey="";
    error=matches>1?"Name matches multiple contacts. Select a contact or use its npub.":"Enter a saved contact name or a public npub / hex / nprofile.";return false;
}
void refresh(){pendingRefresh=true;state=entries.empty()?"No contacts saved":"Profile lookup queued / waiting for relay";if(changed)changed();}
bool refreshPending(){return pendingRefresh;}
void refreshed(){pendingRefresh=false;}
void query(FrameSender send) {
    if(!send)return;
    send("[\"CLOSE\",\"contacts\"]");if(entries.empty())return;
    DynamicJsonDocument doc(8192);JsonArray req=doc.to<JsonArray>();req.add("REQ");req.add("contacts");
    for(const auto &c:entries){JsonObject filter=req.createNestedObject();filter.createNestedArray("authors").add(c.pubkey);filter.createNestedArray("kinds").add(0);filter["limit"]=1;}
    if(doc.overflowed()){state="Profile lookup allocation failed";return;}
    String wire;serializeJson(req,wire);send(wire);state="Fetching names from Nostr profiles";if(changed)changed();
}
void event(JsonArrayConst frame) {
    if(frame[0]=="EOSE" && frame[1]=="contacts"){state="Profile lookup complete / cached names available";if(changed)changed();return;}
    if(frame[0]=="CLOSED" && frame[1]=="contacts"){state="Profile lookup denied: "+String(frame[2]|"").substring(0,80);if(changed)changed();return;}
    if(frame[0]!="EVENT" || frame[1]!="contacts")return;
    JsonObjectConst profile=frame[2].as<JsonObjectConst>();
    if(profile["kind"]!=0 || measureJson(profile)>8192 || !profile["created_at"].is<uint32_t>())return;
    String pubkey=profile["pubkey"]|"";auto found=std::find_if(entries.begin(),entries.end(),[&](const Contact &c){return c.pubkey==pubkey;});
    if(found==entries.end())return;
    uint32_t stamp=profile["created_at"];String id=profile["id"]|"";
    if(stamp>uint32_t(time(nullptr))+600 || stamp<found->updated ||
        (stamp==found->updated && !found->profileId.isEmpty() && std::string(id.c_str())>=std::string(found->profileId.c_str())) || !crypto::verify(profile))return;
    DynamicJsonDocument metadata(8192);if(deserializeJson(metadata,profile["content"].as<String>()) || !metadata.is<JsonObject>())return;
    String name;
    for(const char *field:{"display_name","name"}) {
        if(!metadata[field].is<const char*>())continue;
        std::string candidate=trim(metadata[field].as<String>());
        if(!candidate.empty() && nameValid(candidate,64)){name=candidate.c_str();break;}
    }
    auto next=entries;auto &contact=next[size_t(found-entries.begin())];contact.name=name;contact.updated=stamp;contact.profileId=id;
    String error;if(!commit(next,error)){state=error;if(changed)changed();}else state="Nostr profile name saved";
}
String status(){return state;}
}
