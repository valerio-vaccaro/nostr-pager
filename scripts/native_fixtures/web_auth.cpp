#include "web_auth.h"
#include "reset_control.h"
#include <cassert>
#include <iostream>
struct ResetStorage {
    bool files=true,certificate=true,configuration=true,wipeFlag=true,closed=false;
    int failure=0,stage=0;
    bool next(){return ++stage!=failure;}
    bool openTls(){return next();}
    bool formatFiles(){if(!next())return false;files=false;return true;}
    bool clearTls(){if(!next())return false;certificate=false;return true;}
    bool clearConfiguration(){if(!next())return false;configuration=false;return true;}
    bool finishWipe(){if(!next())return false;wipeFlag=false;return true;}
    void closeTls(){closed=true;}
};
int main() {
    using namespace webAuth;
    using resetControl::Mode;
    assert(resetControl::confirmed("restart","")==Mode::Invalid);
    assert(resetControl::confirmed("erase","")==Mode::Invalid);
    assert(resetControl::confirmed("erase","no")==Mode::Invalid);
    assert(resetControl::confirmed("unknown","yes")==Mode::Invalid);
    assert(resetControl::confirmed("restart","yes")==Mode::Restart);
    assert(resetControl::confirmed("erase","yes")==Mode::Erase);
    for(int failure=1;failure<=5;++failure) {
        ResetStorage storage;storage.failure=failure;
        assert(!resetControl::eraseStoredData(storage));assert(storage.closed && storage.wipeFlag);
        storage.failure=0;storage.stage=0;
        assert(resetControl::eraseStoredData(storage));
        assert(!storage.files && !storage.certificate && !storage.configuration && !storage.wipeFlag);
    }
    Guard guard;
    assert(!guard.find("",0));
    assert(!guard.checkPin("12345","123456",0));
    assert(!guard.checkPin("1234567","123456",1));
    assert(!guard.checkPin("12x456","123456",2));
    assert(!guard.checkPin("000000","123456",3));
    assert(!guard.checkPin("654321","123456",4));
    assert(guard.limited(5));
    assert(!guard.checkPin("123456","123456",59999));
    assert(!guard.limited(60004));
    assert(guard.checkPin("000007","000007",60005));
    guard.create("token-a","csrf-a",100000);
    assert(!guard.find("token-b",100001));
    assert(!guard.find("token-a-extra",100001));
    auto *session=guard.find("token-a",100001);
    assert(session && equal(session->csrf,"csrf-a"));
    assert(!equal(session->csrf,"csrf-b"));
    assert(!equal("a",std::string("a\0b",3)));
    assert(!guard.find("token-a",100001+Guard::Idle));
    guard.create("absolute","csrf",0);
    for(uint32_t now=100000;now<Guard::Lifetime;now+=100000)assert(guard.find("absolute",now));
    assert(!guard.find("absolute",Guard::Lifetime));
    guard.create("logout","csrf",0);guard.remove("logout",1);
    assert(!guard.find("logout",2));
    Guard afterRestart;
    assert(!afterRestart.find("token-a",1));
    guard.create("one","1",0);guard.create("two","2",1);
    guard.create("three","3",2);guard.create("four","4",3);
    guard.find("one",4);guard.create("five","5",5);
    assert(guard.find("one",6));assert(!guard.find("two",6));
    assert(guard.find("five",6));
    Guard wrapped;
    const uint32_t nearWrap=UINT32_MAX-100;
    wrapped.create("wrap","csrf",nearWrap);
    assert(wrapped.find("wrap",50));
    assert(!wrapped.find("wrap",Guard::Idle+50));
    for(int i=0;i<5;++i)wrapped.checkPin("999999","123456",nearWrap);
    assert(wrapped.limited(50));assert(!wrapped.limited(60000));
    std::cout << "PIN/session/CSRF checks, reset confirmation modes and interrupted deletion recovery passed.\n";
}
