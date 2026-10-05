#include "nostr_crypto.h"
#include "nip44.h"
#include <Bitcoin.h>
#include <Conversion.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cassert>
#include <fstream>
#include <iostream>
extern "C" void random_buffer(uint8_t *out,size_t size) {assert(RAND_bytes(out,int(size))==1);}
int main(int argc,char **argv) {
    uint8_t a[32]={},b[32]={},nonce[32]={},conversation[32];a[31]=1;b[31]=2;nonce[31]=1;
    PrivateKey alice(a),bob(b);uint8_t peer[32];assert(keys::unhex(bob.publicKey().x(),peer,32));
    assert(nip44::conversation(a,peer,conversation));
    assert(keys::hex(conversation,32)=="c41c775356fd92eadc63ff5a0dc1da211b268cbea22316767095b2871ea1412d");
    std::vector<uint8_t> payload;assert(nip44::encrypt(conversation,nonce,"a",payload));
    unsigned char base64[256];EVP_EncodeBlock(base64,payload.data(),int(payload.size()));
    assert(std::string(reinterpret_cast<char*>(base64))=="AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABee0G5VSK0/9YypIObAtDKfYEAjD35uVkHyB0F4DwrcNaCXlCWZKaArsGrY6M9wnuTMxWfp1RTN9Xga8no+kF5Vsb");
    std::string plain;assert(nip44::decrypt(conversation,payload,plain) && plain=="a");
    for(size_t i=0;i<payload.size();++i){auto corrupt=payload;corrupt[i]^=1;assert(!nip44::decrypt(conversation,corrupt,plain));}
    for(size_t size:{size_t(1),32ul,33ul,64ul,65ul,255ul,256ul,257ul,511ul,512ul,513ul,1024ul,4096ul}) {
        std::string text(size,'a');assert(nip44::encrypt(conversation,nonce,text,payload));assert(nip44::decrypt(conversation,payload,plain)&&plain==text);
    }
    assert(!nip44::encrypt(conversation,nonce,"",payload));assert(!nip44::encrypt(conversation,nonce,std::string(4097,'x'),payload));
    assert(!nip44::encrypt(conversation,nonce,std::string("a\0b",3),payload));
    assert(!nip44::encrypt(conversation,nonce,std::string("\xc0\x80",2),payload));
    assert(nip44::encrypt(conversation,nonce,u8"Ciao 💜",payload));assert(nip44::decrypt(conversation,payload,plain)&&plain==u8"Ciao 💜");
    uint8_t bad[32]={};assert(!nip44::conversation(bad,peer,conversation));memset(bad,0xff,32);assert(!nip44::conversation(bad,peer,conversation));
    assert(keys::unhex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141",bad,32));assert(!nip44::conversation(bad,peer,conversation));
    String own=alice.publicKey().x(),recipient=bob.publicKey().x(),secret=keys::hex(a,32),recipientSecret=keys::hex(b,32);
    DynamicJsonDocument rumor(8192);JsonObject message=rumor.to<JsonObject>();
    message["pubkey"]=own;message["kind"]=14;message["created_at"]=1790000000U;message["content"]="Hello from the pager";
    JsonArray tag=message.createNestedArray("tags").createNestedArray();tag.add("p");tag.add(recipient);
    assert(crypto::identify(message));
    String wire,self;assert(crypto::wrap(message,secret,recipient,wire));assert(crypto::wrap(message,secret,own,self));
    std::ofstream outgoing(".build-cache/modern/firmware-wrap.json");outgoing<<wire.c_str();outgoing.close();
    DynamicJsonDocument wrapper(16384),opened(8192);assert(!deserializeJson(wrapper,wire));
    String diagnostic="stale error";
    assert(crypto::unwrap(wrapper.as<JsonObjectConst>(),recipientSecret,recipient,opened,&diagnostic) && diagnostic.isEmpty());
    assert(opened["id"]==message["id"] && opened["content"]==message["content"]);
    assert(!crypto::unwrap(wrapper.as<JsonObjectConst>(),secret,own,opened,&diagnostic) && diagnostic.indexOf("another identity")>=0);
    assert(!crypto::unwrap(wrapper.as<JsonObjectConst>(),secret,recipient,opened,&diagnostic) && diagnostic.indexOf("Gift-wrap NIP-44")>=0);
    wrapper["sig"]="00";assert(!crypto::unwrap(wrapper.as<JsonObjectConst>(),recipientSecret,recipient,opened,&diagnostic) && diagnostic.indexOf("signature")>=0);
    assert(!deserializeJson(wrapper,self));assert(crypto::unwrap(wrapper.as<JsonObjectConst>(),secret,own,opened));
    message["pubkey"]=recipient;assert(crypto::identify(message));
    assert(!crypto::wrap(message,secret,recipient,wire));
    // Signing with a different sender must never authenticate the rumor's claimed author.
    if(argc==2) {
        std::ifstream fixture(argv[1]);DynamicJsonDocument cases(65536);assert(!deserializeJson(cases,fixture));
        assert(crypto::unwrap(cases["valid"].as<JsonObjectConst>(),recipientSecret,recipient,opened));
        assert(opened["content"]=="Independent JS message");
        assert(keys::unhex(bob.publicKey().x(),peer,32));assert(nip44::conversation(a,peer,conversation));
        for(JsonObjectConst vector:cases["vectors"].as<JsonArrayConst>()) {
            std::string text(size_t(vector["size"].as<unsigned>()),'a');
            assert(nip44::encrypt(conversation,nonce,text,payload));
            std::vector<unsigned char> encoded(4*((payload.size()+2)/3)+1);
            EVP_EncodeBlock(encoded.data(),payload.data(),int(payload.size()));
            assert(std::string(reinterpret_cast<char*>(encoded.data()))==vector["payload"].as<std::string>());
        }
        for(auto name:{"impersonation","group","wrongId","wrongRecipient"})assert(!crypto::unwrap(cases[name].as<JsonObjectConst>(),recipientSecret,recipient,opened));
        uint32_t expires=0;
        assert(crypto::unwrap(cases["expiring"].as<JsonObjectConst>(),recipientSecret,recipient,opened,&diagnostic,&expires));
        assert(expires==UINT32_MAX && diagnostic.isEmpty() && opened["content"]=="Independent JS message");
        struct Rejection {const char *fixture,*reason;};
        for(auto test:{Rejection{"expired","DM has expired"},Rejection{"outerExpired","DM has expired"},
                Rejection{"badExpiration","malformed expiration"},
                Rejection{"sealMissingId","Missing or malformed event ID"},Rejection{"sealWrongId","canonical JSON hash"},
                Rejection{"sealBadSignature","Schnorr signature verification failed"},Rejection{"sealWrongKind","Expected kind-13"},
                Rejection{"sealTamperedTags","canonical JSON hash"}}) {
            expires=100;
            assert(!crypto::unwrap(cases[test.fixture].as<JsonObjectConst>(),recipientSecret,recipient,opened,&diagnostic,&expires));
            assert(diagnostic.indexOf(test.reason)>=0 && expires==0);
        }
        assert(crypto::unwrap(cases["unsupportedSealTag"].as<JsonObjectConst>(),recipientSecret,recipient,opened,&diagnostic,&expires) && opened["content"].as<String>().length()>0);
        std::cout<<"Independent seal expiration compatibility, expired-message rejection and precise seal validation diagnostics passed.\n";
    }
    std::cout<<"Published NIP-44 vector, authenticated payload tampering, padding bounds, gift-wrap roundtrips and sender impersonation checks passed.\n";
}
