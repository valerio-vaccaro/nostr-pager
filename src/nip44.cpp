#include "nip44.h"
#include <Bitcoin.h>
#include <Hash.h>
#include <cstring>
namespace {
void wipe(void *data,size_t size) { volatile uint8_t *p=static_cast<uint8_t*>(data);while(size--)*p++=0; }
bool textValid(const std::string &text) {
    for(size_t i=0;i<text.size();) {
        uint8_t lead=uint8_t(text[i++]);if(!lead)return false;if(lead<128)continue;
        unsigned remaining;uint32_t code,minimum;
        if(lead>=0xc2 && lead<=0xdf){remaining=1;code=lead&31;minimum=0x80;}
        else if(lead>=0xe0 && lead<=0xef){remaining=2;code=lead&15;minimum=0x800;}
        else if(lead>=0xf0 && lead<=0xf4){remaining=3;code=lead&7;minimum=0x10000;}
        else return false;
        if(i+remaining>text.size())return false;
        while(remaining--){uint8_t next=uint8_t(text[i++]);if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
        if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return false;
    }
    return true;
}
void messageKeys(const uint8_t key[32],const uint8_t nonce[32],uint8_t result[76]) {
    uint8_t previous[32]={},input[65];size_t used=0;
    for(uint8_t block=1;block<=3;++block) {
        size_t offset=block==1?0:32;
        if(offset)memcpy(input,previous,32);
        memcpy(input+offset,nonce,32);input[offset+32]=block;
        sha256Hmac(key,32,input,offset+33,previous);
        size_t copy=std::min(size_t(32),size_t(76)-used);memcpy(result+used,previous,copy);used+=copy;
    }
    wipe(previous,sizeof(previous));wipe(input,sizeof(input));
}
uint32_t load(const uint8_t *p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint32_t rotate(uint32_t value,unsigned shift) {return (value<<shift)|(value>>(32-shift));}
void quarter(uint32_t &a,uint32_t &b,uint32_t &c,uint32_t &d) {
    a+=b;d=rotate(d^a,16);c+=d;b=rotate(b^c,12);
    a+=b;d=rotate(d^a,8);c+=d;b=rotate(b^c,7);
}
bool chacha(const uint8_t key[32],const uint8_t nonce[12],std::vector<uint8_t> &data) {
    uint32_t state[16]={0x61707865,0x3320646e,0x79622d32,0x6b206574};
    for(int i=0;i<8;++i)state[4+i]=load(key+4*i);
    state[12]=0;for(int i=0;i<3;++i)state[13+i]=load(nonce+4*i);
    for(size_t offset=0;offset<data.size();offset+=64) {
        uint32_t block[16];memcpy(block,state,sizeof(block));
        for(int round=0;round<10;++round) {
            quarter(block[0],block[4],block[8],block[12]);quarter(block[1],block[5],block[9],block[13]);
            quarter(block[2],block[6],block[10],block[14]);quarter(block[3],block[7],block[11],block[15]);
            quarter(block[0],block[5],block[10],block[15]);quarter(block[1],block[6],block[11],block[12]);
            quarter(block[2],block[7],block[8],block[13]);quarter(block[3],block[4],block[9],block[14]);
        }
        for(size_t i=0;i<64 && offset+i<data.size();++i) {
            uint32_t word=block[i/4]+state[i/4];data[offset+i]^=uint8_t(word>>(8*(i%4)));
        }
        ++state[12];wipe(block,sizeof(block));
    }
    wipe(state,sizeof(state));return true;
}
}
namespace nip44 {
size_t paddedSize(size_t size) {
    if(size<=32)return 32;
    size_t next=1;while(next<size)next<<=1;
    size_t chunk=next<=256?32:next/8;return chunk*((size-1)/chunk+1);
}
bool conversation(const uint8_t secret[32],const uint8_t peer[32],uint8_t key[32]) {
    static const uint8_t order[32]={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x41};
    if(memcmp(secret,order,32)>=0)return false;
    PublicKey pub;pub.from_x(peer,32);PrivateKey priv(secret);
    uint8_t shared[32];
    if(!pub.isValid() || !priv.isValid() || priv.ecdh(pub,shared,false)!=1)return false;
    const uint8_t salt[]={'n','i','p','4','4','-','v','2'};
    sha256Hmac(salt,sizeof(salt),shared,32,key);wipe(shared,32);return true;
}
bool encrypt(const uint8_t key[32],const uint8_t nonce[32],const std::string &plain,std::vector<uint8_t> &payload) {
    if(plain.empty() || plain.size()>MaxPlaintext || !textValid(plain))return false;
    std::vector<uint8_t> content(2+paddedSize(plain.size()),0);
    content[0]=plain.size()>>8;content[1]=plain.size();memcpy(content.data()+2,plain.data(),plain.size());
    uint8_t keys[76];messageKeys(key,nonce,keys);
    bool ok=chacha(keys,keys+32,content);
    if(ok) {
        payload.resize(65+content.size());payload[0]=2;memcpy(payload.data()+1,nonce,32);memcpy(payload.data()+33,content.data(),content.size());
        sha256Hmac(keys+44,32,payload.data()+1,32+content.size(),payload.data()+33+content.size());
    }
    wipe(keys,sizeof(keys));wipe(content.data(),content.size());return ok;
}
bool decrypt(const uint8_t key[32],const std::vector<uint8_t> &payload,std::string &plain) {
    plain.clear();
    if(payload.size()<99 || payload.size()>65+2+paddedSize(MaxPlaintext) || payload[0]!=2)return false;
    size_t size=payload.size()-65;uint8_t keys[76],mac[32];messageKeys(key,payload.data()+1,keys);
    sha256Hmac(keys+44,32,payload.data()+1,32+size,mac);
    unsigned mismatch=0;for(size_t i=0;i<32;++i)mismatch|=mac[i]^payload[33+size+i];
    if(mismatch){wipe(keys,sizeof(keys));return false;}
    std::vector<uint8_t> content(payload.begin()+33,payload.end()-32);
    bool ok=chacha(keys,keys+32,content);wipe(keys,sizeof(keys));
    size_t length=(size_t(content[0])<<8)|content[1];
    if(!ok || !length || length>MaxPlaintext || size!=2+paddedSize(length))ok=false;
    if(ok) {
        for(size_t i=length+2;i<content.size();++i)if(content[i]!=0)ok=false;
        // Arduino String cannot represent embedded NUL; validate UTF-8 text separately at the envelope boundary.
        if(memchr(content.data()+2,0,length))ok=false;
    }
    if(ok){plain.assign(reinterpret_cast<char*>(content.data()+2),length);if(!textValid(plain)){std::fill(plain.begin(),plain.end(),'\0');plain.clear();ok=false;}}
    wipe(content.data(),content.size());return ok;
}
}
