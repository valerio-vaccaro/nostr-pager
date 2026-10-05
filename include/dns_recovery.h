#pragma once
#include <stdint.h>
#include <string>

namespace dnsRecovery {
struct Policy {
    unsigned streak=0;
    bool multipleHosts=false,hasChanged=false;
    uint32_t firstAt=0,changedAt=0,failures=0;
    std::string firstHost,lastHost;
    void resetBurst() {streak=0;multipleHosts=false;firstHost.clear();}
    void failed(const char *host,uint32_t now) {
        ++failures;lastHost=host;
        if(streak && now-firstAt>60000)resetBurst();
        if(!streak){firstAt=now;firstHost=host;}
        else if(firstHost!=host)multipleHosts=true;
        ++streak;
    }
    bool due(uint32_t now) const {
        return streak>=3 && multipleHosts && now-firstAt<=60000 &&
            (!hasChanged || now-changedAt>=60000);
    }
    void changed(uint32_t now) {changedAt=now;hasChanged=true;resetBurst();}
};
}
