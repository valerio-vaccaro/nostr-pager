#include "nostr_crypto.h"
#include "nip44.h"
#include <Bitcoin.h>
#include <Hash.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <vector>
namespace {
String hex(const uint8_t *p, size_t n) { return keys::hex(p,n).c_str(); }
bool raw(const String &s,uint8_t *p,size_t n) { return keys::unhex(s.c_str(),p,n); }
bool hashEvent(JsonObjectConst e,uint8_t hash[32]) {
    if (!e["pubkey"].is<const char *>() || !e["created_at"].is<uint32_t>() || !e["kind"].is<unsigned>() ||
        !e["tags"].is<JsonArrayConst>() || !e["content"].is<const char *>()) return false;
    DynamicJsonDocument doc(12288);
    JsonArray a=doc.to<JsonArray>();
    a.add(0); a.add(e["pubkey"]); a.add(e["created_at"]); a.add(e["kind"]); a.add(e["tags"]); a.add(e["content"]);
    if (doc.overflowed()) return false;
    String canonical; serializeJson(a,canonical);
    sha256(canonical,hash);
    return true;
}
bool shared(const String &secret,const String &peer,uint8_t key[32]) {
    uint8_t sk[32],pk[32];
    if (!raw(secret,sk,32)||!raw(peer,pk,32)) return false;
    PublicKey pub; pub.from_x(pk,32);
    if (!pub.isValid()) {memset(sk,0,32);return false;}
    PrivateKey priv(sk); memset(sk,0,32);
    return priv.ecdh(pub,key,false)==1;
}
String b64(const uint8_t *data,size_t len) {
    std::vector<uint8_t> buf(4*((len+2)/3)+1); size_t size=0;
    if (mbedtls_base64_encode(buf.data(),buf.size(),&size,data,len)!=0) return "";
    return String(reinterpret_cast<char *>(buf.data()),size);
}
bool unb64(const String &s,std::vector<uint8_t> &data) {
    if (s.isEmpty() || s.length()>6144) return false;
    data.resize(s.length()); size_t len=0;
    if (mbedtls_base64_decode(data.data(),data.size(),&len,reinterpret_cast<const uint8_t *>(s.c_str()),s.length())!=0) return false;
    data.resize(len); return true;
}
}
namespace crypto {
bool publicKey(const keys::Key &key,String &pub) {
    uint8_t b[32]; if (!keys::unhex(key.hex,b,32)) return false;
    if (key.secret) {PrivateKey priv(b);pub=priv.publicKey().x();memset(b,0,32);return priv.publicKey().isValid();}
    PublicKey p; p.from_x(b,32); pub=key.hex.c_str(); return p.isValid();
}
bool verify(JsonObjectConst e,String *error) {
    if(error)*error="";
    auto reject=[&](const char *reason){if(error)*error=reason;return false;};
    uint8_t hash[32],id[32],pk[32],sig[64];
    if(!raw(e["id"]|"",id,32))return reject("Missing or malformed event ID");
    if(!raw(e["pubkey"]|"",pk,32))return reject("Missing or malformed signing public key");
    if(!raw(e["sig"]|"",sig,64))return reject("Missing or malformed Schnorr signature");
    if(!e["created_at"].is<uint32_t>())return reject("Event created_at must be an unsigned integer");
    if(!e["kind"].is<unsigned>())return reject("Event kind must be an unsigned integer");
    if(!e["tags"].is<JsonArrayConst>())return reject("Event tags must be an array");
    if(!e["content"].is<const char *>())return reject("Event content must be a string");
    if(!hashEvent(e,hash))return reject("Invalid event fields or canonical serialization too large");
    if(memcmp(hash,id,32))return reject("Event ID does not match canonical JSON hash");
    PublicKey pub;pub.from_x(pk,32);
    if(!pub.isValid())return reject("Invalid signing curve point");
    if(!pub.schnorr_verify(SchnorrSignature(sig),hash))return reject("Schnorr signature verification failed");
    return true;
}
bool identify(JsonObject event) {
    uint8_t hash[32];if(!hashEvent(event,hash))return false;
    event["id"]=hex(hash,32);return true;
}
bool rumorValid(JsonObjectConst event) {
    uint8_t hash[32],id[32];
    return !event.containsKey("sig") && raw(event["id"]|"",id,32) && hashEvent(event,hash) && memcmp(id,hash,32)==0;
}
namespace {
bool encrypt44(const String &secret,const String &peer,const String &plain,String &cipher) {
    uint8_t sk[32],pk[32],key[32],nonce[32];
    if(!raw(secret,sk,32) || !raw(peer,pk,32))return false;
    bool ok=nip44::conversation(sk,pk,key);memset(sk,0,32);
    if(!ok)return false;
    esp_fill_random(nonce,32);std::vector<uint8_t> payload;
    ok=nip44::encrypt(key,nonce,std::string(plain.c_str(),plain.length()),payload);memset(key,0,32);
    if(ok)cipher=b64(payload.data(),payload.size());return ok && !cipher.isEmpty();
}
bool decrypt44(const String &secret,const String &peer,const String &cipher,String &plain) {
    if(cipher.length()<132 || cipher.length()>6144)return false;
    uint8_t sk[32],pk[32],key[32];std::vector<uint8_t> payload;
    if(!raw(secret,sk,32) || !raw(peer,pk,32) || !unb64(cipher,payload))return false;
    bool ok=nip44::conversation(sk,pk,key);memset(sk,0,32);if(!ok)return false;
    std::string decoded;ok=nip44::decrypt(key,payload,decoded);memset(key,0,32);
    if(ok)plain=decoded.c_str();std::fill(decoded.begin(),decoded.end(),'\0');return ok;
}
uint32_t hiddenTime() {return uint32_t(time(nullptr))-esp_random()%172800;}
}
bool wrap(JsonObjectConst rumor,const String &secret,const String &recipient,String &wire) {
    if(!rumorValid(rumor) || rumor["kind"]!=14)return false;
    String json,cipher;serializeJson(rumor,json);
    if(!encrypt44(secret,recipient,json,cipher))return false;
    DynamicJsonDocument sealDoc(8192);JsonObject seal=sealDoc.to<JsonObject>();
    seal["pubkey"]=rumor["pubkey"];seal["kind"]=13;seal["created_at"]=hiddenTime();seal["content"]=cipher;
    seal.createNestedArray("tags");if(!sign(seal,secret))return false;
    json="";serializeJson(seal,json);
    uint8_t bytes[32];keys::Key ephemeral;ephemeral.secret=true;String ephemeralPub;
    do {esp_fill_random(bytes,32);ephemeral.hex=keys::hex(bytes,32);}while(!publicKey(ephemeral,ephemeralPub));
    memset(bytes,0,32);String ephemeralSecret=ephemeral.hex.c_str();
    bool ok=encrypt44(ephemeralSecret,recipient,json,cipher);
    DynamicJsonDocument doc(12288);JsonObject wrapper=doc.to<JsonObject>();
    wrapper["pubkey"]=ephemeralPub;wrapper["kind"]=1059;wrapper["created_at"]=hiddenTime();wrapper["content"]=cipher;
    JsonArray tag=wrapper.createNestedArray("tags").createNestedArray();tag.add("p");tag.add(recipient);
    if(ok)ok=sign(wrapper,ephemeralSecret);
    std::fill(ephemeral.hex.begin(),ephemeral.hex.end(),'0');for(size_t i=0;i<ephemeralSecret.length();++i)ephemeralSecret.setCharAt(i,'0');
    if(ok)serializeJson(wrapper,wire);return ok;
}
bool unwrap(JsonObjectConst wrapper,const String &secret,const String &own,JsonDocument &rumor,String *error,uint32_t *expiresAt) {
    if(error)*error="";
    if(expiresAt)*expiresAt=0;
    auto reject=[&](const char *reason){if(error)*error=reason;return false;};
    if(wrapper["kind"]!=1059 || !verify(wrapper))return reject("Gift-wrap signature or ID invalid");
    bool addressed=false;for(JsonArrayConst tag:wrapper["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="p" && tag[1]==own)addressed=true;
    if(!addressed)return reject("Gift wrap is addressed to another identity");
    String json;if(!decrypt44(secret,wrapper["pubkey"]|"",wrapper["content"]|"",json))return reject("Gift-wrap NIP-44 decryption failed");
    DynamicJsonDocument doc(12288);
    if(deserializeJson(doc,json))return reject("Decrypted seal JSON invalid or too large");JsonObjectConst seal=doc.as<JsonObjectConst>();
    if(seal["kind"]!=13){if(error)*error="Expected kind-13 seal; received kind "+String(seal["kind"]|0U);return false;}
    if(!seal["tags"].is<JsonArrayConst>())return reject("Seal tags must be an array");
    String verification;
    if(!verify(seal,&verification)){if(error)*error="Seal: "+verification;return false;}
    uint32_t expiration=0;
    auto readExpiration=[&](JsonArrayConst tags,bool sealTags) {
        for(JsonArrayConst tag:tags) {
            if(tag[0]!="expiration")continue; // other clients add e.g. "p" to signed seals; they are metadata only
            if(tag.size()!=2 || !tag[1].is<const char *>())return false;
            const char *text=tag[1].as<const char *>();uint64_t value=0;
            if(!*text)return false;
            for(const char *at=text;*at;++at) {
                if(*at<'0' || *at>'9')return false;
                value=value*10+unsigned(*at-'0');if(value>UINT32_MAX)return false;
            }
            if(!value)return false;
            expiration=expiration?std::min(expiration,uint32_t(value)):uint32_t(value);
        }
        return true;
    };
    // The seal signature authenticates every tag. Real clients send seals with extra tags, so only
    // the expiration tag is interpreted and unknown tags are ignored.
    if(!readExpiration(seal["tags"].as<JsonArrayConst>(),true))return reject("Malformed seal expiration");
    if(!readExpiration(wrapper["tags"].as<JsonArrayConst>(),false))return reject("Malformed gift-wrap expiration");
    if(expiration && uint32_t(time(nullptr))>=expiration)return reject("DM has expired");
    if(!decrypt44(secret,seal["pubkey"]|"",seal["content"]|"",json))return reject("Seal NIP-44 decryption failed");
    if(deserializeJson(rumor,json))return reject("Decrypted message JSON invalid or too large");
    JsonObjectConst message=rumor.as<JsonObjectConst>();
    if(message["kind"]!=14)return reject("Unsupported modern DM kind (text kind 14 required)");
    if(message["pubkey"]!=seal["pubkey"] || !rumorValid(message))return reject("Message ID or authenticated sender invalid");
    // Only two-person text chats are supported; do not flatten group conversations into private replies.
    String sender=message["pubkey"]|"",peer;
    bool recipientFound=false,ownRecipient=false;
    for(JsonArrayConst tag:message["tags"].as<JsonArrayConst>())if(tag.size()>=2 && tag[0]=="p") {
        String recipient=tag[1]|"";recipientFound=true;if(recipient==own)ownRecipient=true;
        uint8_t point[32];PublicKey parsed;
        if(!raw(recipient,point,32))return reject("Invalid message recipient key");parsed.from_x(point,32);if(!parsed.isValid())return reject("Invalid message recipient curve point");
        if(sender!=own && recipient!=own && recipient!=sender)return reject("Group DMs are unsupported");
        if(sender==own && recipient!=own) {
            if(!peer.isEmpty() && peer!=recipient)return reject("Group DMs are unsupported");
            peer=recipient;
        }
    }
    String content=message["content"]|"";
    if(!recipientFound || (sender!=own && !ownRecipient))return reject("Message recipient does not match this identity");
    if(content.isEmpty() || content.length()>512)return reject("DM text must contain 1-512 UTF-8 bytes");
    if(expiresAt)*expiresAt=expiration;
    return true;
}
bool sign(JsonObject e,const String &secret) {
    uint8_t hash[32],sk[32];
    if (!raw(secret,sk,32)||!hashEvent(e,hash)) return false;
    PrivateKey priv(sk);memset(sk,0,32);
    e["id"]=hex(hash,32); e["sig"]=priv.schnorr_sign(hash).toString();
    return verify(e);
}
bool encrypt(const String &secret,const String &peer,const String &plain,String &cipher) {
    if (plain.isEmpty() || plain.length()>512) return false;
    uint8_t key[32],iv[16],workIv[16];
    if (!shared(secret,peer,key)) return false;
    esp_fill_random(iv,16);memcpy(workIv,iv,16);
    uint8_t pad=16-plain.length()%16;
    std::vector<uint8_t> data(plain.length()+pad,pad);
    memcpy(data.data(),plain.c_str(),plain.length());
    mbedtls_aes_context aes;mbedtls_aes_init(&aes);
    int err=mbedtls_aes_setkey_enc(&aes,key,256);
    if (!err) err=mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_ENCRYPT,data.size(),workIv,data.data(),data.data());
    mbedtls_aes_free(&aes);memset(key,0,32);
    if (err) return false;
    cipher=b64(data.data(),data.size())+"?iv="+b64(iv,16);return true;
}
bool decrypt(const String &secret,const String &peer,const String &cipher,String &plain) {
    int pos=cipher.indexOf("?iv=");
    if (pos<0) return false;
    std::vector<uint8_t> data,iv;
    if (!unb64(cipher.substring(0,pos),data)||!unb64(cipher.substring(pos+4),iv)||iv.size()!=16||data.empty()||data.size()%16) return false;
    uint8_t key[32]; if (!shared(secret,peer,key)) return false;
    mbedtls_aes_context aes;mbedtls_aes_init(&aes);
    int err=mbedtls_aes_setkey_dec(&aes,key,256);
    if (!err) err=mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_DECRYPT,data.size(),iv.data(),data.data(),data.data());
    mbedtls_aes_free(&aes);memset(key,0,32);
    if (err) return false;
    uint8_t pad=data.back();
    if (!pad || pad>16 || pad>data.size()) return false;
    for (size_t i=data.size()-pad;i<data.size();++i) if (data[i]!=pad) return false;
    if (data.size()-pad>512) return false;
    plain=String(reinterpret_cast<char *>(data.data()),data.size()-pad);
    memset(data.data(),0,data.size());return true;
}
}
