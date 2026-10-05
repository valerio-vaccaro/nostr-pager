#pragma once
#include <algorithm>
#include <stdint.h>
namespace messageExpiry {
inline bool expired(uint32_t expiresAt,uint32_t now) {return expiresAt && now>=expiresAt;}
template<class Records> bool prune(Records &records,uint32_t now) {
    auto size=records.size();
    records.erase(std::remove_if(records.begin(),records.end(),[&](const typename Records::value_type &record){
        return expired(record.expiresAt,now);
    }),records.end());
    return size!=records.size();
}
}
