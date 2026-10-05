#include "inbox_requests.h"
#include <cassert>
#include <cstdio>

int main() {
    DynamicJsonDocument doc(2048);String own=std::string(64,'a').c_str();
    inboxRequests::build(doc,"pager",own,false,true,20,false,0,1790000000);
    assert(doc[0]=="REQ" && doc[1]=="pager" && doc.size()==3);
    assert(doc[2]["kinds"][0]==1 && doc[2]["#p"][0]==own && doc[2]["limit"]==20);
    assert(!doc[2].containsKey("since") && !doc[2].containsKey("until"));
    inboxRequests::build(doc,"pager-dm",own,true,true,20,false,0,1790000000);
    assert(doc.size()==5 && doc[2]["kinds"][0]==4 && doc[3]["kinds"][0]==1059);
    assert(doc[3]["#p"][0]==own && doc[3]["limit"]==200 && doc[3]["since"]==1789740800U);
    assert(doc[4]["authors"][0]==own && !doc[4].containsKey("#p"));
    inboxRequests::build(doc,"dm-older",own,true,false,60,true,1000,1790000000);
    assert(doc.size()==4 && doc[2]["until"]==999 && doc[3]["until"]==999);
    assert(doc[3]["limit"]==60 && !doc[3].containsKey("since"));
    inboxRequests::build(doc,"older-dm",own,true,true,20,true,0,1790000000);
    assert(doc[3]["until"]==0 && doc[3]["limit"]==20);
    assert(!doc.overflowed());
    puts("Inbox requests: independent public/DM filters, own sent-copy filter, randomized-envelope replay window and older-history boundaries passed.");
}
