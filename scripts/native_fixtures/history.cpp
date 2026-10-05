#include "history_store.h"
#include <cassert>
#include <iostream>
struct Record {std::string id;uint32_t timestamp;std::string text;};
int main() {
    using historyStore::Result;
    historyStore::Filter filter;
    std::vector<Record> records={{"one",10,"pending payload"},{"two",20,"confirmed"},{"future",300,"clock skew"}};
    auto reject=[&]{assert(records.size()==2 && filter.hides("one",10));return false;};
    assert(historyStore::remove(records,filter,"one",false,100,reject)==Result::WriteFailed);
    assert(records.size()==3 && records[0].id=="one" && records[0].text=="pending payload" && filter.deleted.empty());
    assert(historyStore::remove(records,filter,"missing",false,100,[]{assert(false);return true;})==Result::NotFound);
    assert(historyStore::remove(records,filter,"one",false,100,[]{return true;})==Result::Ok);
    assert(records.size()==2 && filter.hides("one",10) && !filter.hides("two",20));
    auto before=filter;
    assert(historyStore::remove(records,filter,"",true,100,[&]{assert(records.empty());return false;})==Result::WriteFailed);
    assert(records.size()==2 && records[0].id=="two" && records[1].id=="future" && filter.before==before.before && filter.deleted.size()==before.deleted.size());
    assert(historyStore::remove(records,filter,"",true,100,[]{return true;})==Result::Ok);
    assert(records.empty() && filter.hides("old unseen relay event",99) && filter.hides("future",300) && !filter.hides("new",101));
    historyStore::Filter rebooted=filter;assert(rebooted.hides("one",10) && rebooted.hides("future",300));
    filter={};for(unsigned i=0;i<historyStore::MarkerLimit;++i)assert(filter.remember(std::to_string(i),10));
    records={{"fresh id",20,"body"}};
    assert(historyStore::remove(records,filter,"fresh id",false,100,[]{assert(false);return true;})==Result::MarkersFull);
    assert(records.size()==1 && filter.deleted.size()==historyStore::MarkerLimit);
    assert(historyStore::remove(records,filter,"",true,100,[]{return true;})==Result::Ok);
    assert(filter.deleted.empty() && filter.before==100);
    assert(historyStore::remove(records,filter,"",true,90,[]{return true;})==Result::Ok && filter.before==100);
    std::cout<<"Single/all deletion, write-failure rollback, pending payload retention, durable suppression, future timestamps and bounded marker recovery passed.\n";
}
