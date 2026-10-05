#include "key_codec.h"
#include <Bitcoin.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <array>
static uint8_t testAux[32];
extern "C" void random_buffer(uint8_t *buf,size_t len) {
    assert(len==32);memcpy(buf,testAux,32);
}
int main() {
    keys::Key key;std::string error;
    const std::string pub="3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
    const std::string npub="npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
    assert(keys::npub(pub)==npub);assert(keys::npub("bad").empty());
    assert(keys::decode(npub,false,key,error) && key.hex==pub && !key.secret);
    assert(keys::decode("nostr:"+npub,true,key,error) && !key.secret);
    assert(keys::decode(" \n"+pub+"\t",false,key,error) && key.hex==pub);
    auto upper=npub;std::transform(upper.begin(),upper.end(),upper.begin(),::toupper);
    assert(keys::decode(upper,false,key,error));
    upper=npub;upper[0]='N';assert(!keys::decode(upper,false,key,error));
    assert(keys::decode("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5",false,key,error));
    assert(key.secret && key.hex=="67dea2ed018072d675f5415ecfaed7d2597555e202d85b3d65ea4e58d2d92ffa");
    assert(keys::decode("nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69uhhytnc9e3k7mgpz4mhxue69uhkg6nzv9ejuumpv34kytnrdaksjlyr9p",false,key,error));
    assert(key.hex==pub && !key.secret);
    for(size_t i=5;i<npub.size();++i) {auto bad=npub;bad[i]=bad[i]=='q'?'p':'q';assert(!keys::decode(bad,false,key,error));}
    for(auto bad:{std::string(64,'0'),std::string(64,'f'),std::string("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141")})assert(!keys::decode(bad,true,key,error));
    assert(keys::decode("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140",true,key,error));
    assert(!keys::decode(std::string(1024,'q'),false,key,error));
    keys::Relay relay;
    assert(keys::relay("wss://relay.example.org:8443/nostr",relay));
    assert(relay.host=="relay.example.org" && relay.port==8443 && relay.path=="/nostr");
    for(auto s:{"ws://relay.example.org","wss://","wss://user@relay.example.org","wss://host:65536/","wss://host:0","wss://host/path\r\nInjected"})assert(!keys::relay(s,relay));
    std::ifstream input("scripts/native_fixtures/bip340.csv");std::string line;std::getline(input,line);unsigned vectors=0;
    while(std::getline(input,line)) {
        std::array<std::string,8> field;std::istringstream row(line);
        for(auto &f:field)std::getline(row,f,',');
        if(field[4].size()!=64)continue; // Nostr signs exactly a 32-byte event hash.
        uint8_t pk[32],hash[32],sig[64];
        assert(keys::unhex(field[2],pk,32) && keys::unhex(field[4],hash,32) && keys::unhex(field[5],sig,64));
        PublicKey p;p.from_x(pk,32);
        bool valid=p.isValid() && p.schnorr_verify(SchnorrSignature(sig),hash);
        if(valid!=(field[6]=="TRUE")) {std::cerr<<"BIP340 vector "<<field[0]<<" failed\n";return 1;}
        if(!field[1].empty()) {
            uint8_t sk[32];assert(keys::unhex(field[1],sk,32));PrivateKey k(sk);
            assert(k.publicKey().x()==keys::hex(pk,32));
            assert(keys::unhex(field[3],testAux,32));
            auto signedHash=k.schnorr_sign(hash);
            assert(k.publicKey().schnorr_verify(signedHash,hash));
            assert(signedHash.toString()==keys::hex(sig,64));
        }
        ++vectors;
    }
    assert(vectors==15);
    // Failed compressed-point decoding must not reuse a previous valid point.
    uint8_t invalid[32];assert(keys::unhex("eefdea4cdb677750a420fee807eacf21eb9898ae79b9768766e4faa04a2d4a34",invalid,32));
    for(int i=0;i<100;++i) {
        PublicKey point("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
        assert(point.isValid());point.from_x(invalid,32);assert(!point.isValid());
    }
    // Raw shared x-coordinate, independently computed using Python cryptography SECP256K1.
    uint8_t a[32]={},b[32]={},shared[32];a[31]=3;b[31]=7;
    PrivateKey alice(a),bob(b);alice.ecdh(bob.publicKey(),shared,false);
    assert(keys::hex(shared,32)=="352bbf4a4cdd12564f93fa332ce333301d9ad40271f8107181340aef25be59d5");
    std::cout<<"NIP-19 key/URI/checksum/range checks, relay URL checks, "<<vectors<<" BIP-340 vectors, signing and raw ECDH passed.\n";
}
