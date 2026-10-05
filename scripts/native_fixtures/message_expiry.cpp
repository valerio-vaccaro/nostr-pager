#include "message_expiry.h"
#include <cassert>
#include <cstdio>
#include <vector>
struct Record {unsigned id;uint32_t expiresAt;};
int main() {
    std::vector<Record> records{{1,0},{2,100},{3,101}};
    assert(!messageExpiry::prune(records,99) && records.size()==3);
    assert(messageExpiry::prune(records,100) && records.size()==2 && records[1].id==3);
    assert(messageExpiry::prune(records,101) && records.size()==1 && records[0].id==1);
    assert(!messageExpiry::prune(records,UINT32_MAX));
    assert(messageExpiry::expired(UINT32_MAX,UINT32_MAX));
    puts("Message expiry: exact boundary, unexpiring messages and stable cache removal passed.");
}
