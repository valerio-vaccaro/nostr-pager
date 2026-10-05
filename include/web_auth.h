#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace webAuth {
inline bool equal(const std::string &a, const std::string &b) {
    size_t length = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = unsigned(a.size() ^ b.size());
    for (size_t i=0;i<length;++i) diff |= (i<a.size()?a[i]:0) ^ (i<b.size()?b[i]:0);
    return diff == 0;
}
struct Session { std::string token, csrf; uint32_t born=0, touched=0; };
class Guard {
    std::array<Session,4> sessions{};
    unsigned failures=0;
    uint32_t blockedAt=0;
    bool blocked=false;
public:
    static constexpr uint32_t Idle=30*60*1000, Lifetime=8*60*60*1000;
    bool limited(uint32_t now) {
        if (blocked && uint32_t(now-blockedAt)>=60000) { blocked=false; failures=0; }
        return blocked;
    }
    bool checkPin(const std::string &input,const std::string &pin,uint32_t now) {
        if(limited(now))return false;
        bool valid=input.size()==6;
        for(char c:input)valid &= c>='0' && c<='9';
        if(!equal(input,pin))valid=false;
        if(!valid) { if(++failures>=5){blocked=true;blockedAt=now;} return false; }
        failures=0;return true;
    }
    Session *find(const std::string &token,uint32_t now,bool touch=true) {
        for(auto &s:sessions) {
            if(s.token.empty())continue;
            if(uint32_t(now-s.born)>=Lifetime || uint32_t(now-s.touched)>=Idle){s=Session{};continue;}
            if(equal(s.token,token)) { if(touch)s.touched=now;return &s; }
        }
        return nullptr;
    }
    Session &create(const std::string &token,const std::string &csrf,uint32_t now) {
        Session *slot=&sessions[0];
        for(auto &s:sessions) {
            if(s.token.empty() || uint32_t(now-s.born)>=Lifetime || uint32_t(now-s.touched)>=Idle){slot=&s;break;}
            if(uint32_t(now-s.touched)>uint32_t(now-slot->touched))slot=&s;
        }
        slot->token=token;slot->csrf=csrf;slot->born=now;slot->touched=now;return *slot;
    }
    void remove(const std::string &token,uint32_t now) { if(auto *s=find(token,now,false))*s=Session{}; }
};
}
