#include "key_codec.h"
#include <algorithm>
#include <cctype>
namespace keys {
static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = std::tolower(static_cast<unsigned char>(c));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool unhex(const std::string &s, uint8_t *out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        int a = nibble(s[i * 2]), b = nibble(s[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (a << 4) | b;
    }
    return true;
}
std::string hex(const uint8_t *data, size_t n) {
    static const char *digits = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2*i] = digits[data[i] >> 4]; s[2*i+1] = digits[data[i] & 15]; }
    return s;
}
static uint32_t step(uint32_t chk, uint8_t value) {
    static const uint32_t g[] = {0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3};
    uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ value;
    for (int i=0; i<5; ++i) if ((top >> i) & 1) chk ^= g[i];
    return chk;
}
std::string npub(const std::string &hexKey) {
    uint8_t bytes[32];if(!unhex(hexKey,bytes,32))return "";
    const std::string hrp="npub",alphabet="qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    uint32_t chk=1,acc=0;int bits=0;std::string result=hrp+"1";
    for(char c:hrp)chk=step(chk,uint8_t(c)>>5);
    chk=step(chk,0);for(char c:hrp)chk=step(chk,c&31);
    auto append=[&](unsigned v){result+=alphabet[v];chk=step(chk,v);};
    for(uint8_t byte:bytes){acc=((acc<<8)|byte)&0xffff;bits+=8;while(bits>=5){bits-=5;append((acc>>bits)&31);}}
    if(bits)append((acc<<(5-bits))&31);
    for(int i=0;i<6;++i)chk=step(chk,0);chk^=1;
    for(int i=5;i>=0;--i)result+=alphabet[(chk>>(5*i))&31];
    return result;
}
bool decode(std::string s, bool hexSecret, Key &out, std::string &error) {
    out = {};
    error = "Use hex, npub, nsec or nprofile (optionally nostr:).";
    size_t begin=s.find_first_not_of(" \t\r\n"), end=s.find_last_not_of(" \t\r\n");
    if (begin==std::string::npos) return false;
    s=s.substr(begin,end-begin+1);
    if (s.compare(0,6,"nostr:")==0) s.erase(0,6);
    uint8_t bytes[32];
    bool secret = hexSecret;
    if (unhex(s,bytes,32)) out.hex = hex(bytes,32);
    else {
        if (s.size()>512) return false;
        bool lower=false,upper=false;
        for (char c:s) { lower |= c>='a'&&c<='z'; upper |= c>='A'&&c<='Z'; }
        if (lower&&upper) { error="Mixed-case Bech32 is invalid."; return false; }
        std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});
        size_t sep=s.rfind('1');
        if (sep==std::string::npos || sep<1 || s.size()-sep<7) return false;
        std::string hrp=s.substr(0,sep);
        if (hrp!="npub" && hrp!="nsec" && hrp!="nprofile") return false;
        secret=hrp=="nsec";
        uint32_t chk=1;
        for (char c:hrp) chk=step(chk,uint8_t(c)>>5);
        chk=step(chk,0);
        for (char c:hrp) chk=step(chk,c&31);
        const std::string alphabet="qpzry9x8gf2tvdw0s3jn54khce6mua7l";
        std::vector<uint8_t> values,payload;
        for (size_t i=sep+1;i<s.size();++i) {
            size_t v=alphabet.find(s[i]);
            if (v==std::string::npos) return false;
            chk=step(chk,v); values.push_back(v);
        }
        if (chk!=1) { error="Invalid Bech32 checksum."; return false; }
        uint32_t acc=0; int bits=0;
        for (size_t i=0;i<values.size()-6;++i) {
            acc=((acc<<5)|values[i])&0xffff; bits+=5;
            if (bits>=8) {bits-=8;payload.push_back((acc>>bits)&255);}
        }
        if (bits>=5 || ((acc<<(8-bits))&255)) return false;
        if (hrp=="nprofile") {
            std::vector<uint8_t> pub;
            for (size_t i=0;i<payload.size();) {
                if (payload.size()-i<2) return false;
                int type=payload[i++], n=payload[i++];
                if (i+n>payload.size()) return false;
                if (type==0) {
                    if (n!=32 || !pub.empty()) return false;
                    pub.assign(payload.begin()+i,payload.begin()+i+n);
                }
                i+=n;
            }
            payload=pub;
        }
        if (payload.size()!=32) return false;
        out.hex=hex(payload.data(),32);
    }
    out.secret=secret;
    if (secret && (out.hex==std::string(64,'0') || out.hex>="fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141")) {
        error="Private key is outside the secp256k1 range."; out={}; return false;
    }
    error.clear(); return true;
}
bool relay(const std::string &url, Relay &out) {
    if (url.compare(0,6,"wss://") || url.size()>200) return false;
    size_t slash=url.find('/',6);
    std::string authority=url.substr(6,slash==std::string::npos?slash:slash-6);
    out={}; out.path=slash==std::string::npos?"/":url.substr(slash);
    size_t colon=authority.find(':');
    out.host=authority.substr(0,colon);
    if (out.host.empty()) return false;
    for (char c:out.host) if (!(std::isalnum(static_cast<unsigned char>(c)) || c=='.' || c=='-')) return false;
    if (colon!=std::string::npos) {
        unsigned port=0; std::string p=authority.substr(colon+1);
        if (p.empty() || p.size()>5) return false;
        for (char c:p) { if (c<'0'||c>'9') return false; port=port*10+c-'0'; }
        if (!port || port>65535) return false;
        out.port=port;
    }
    for (char c:out.path) if (c<=32 || c>=127 || c=='#') return false;
    return true;
}
}
