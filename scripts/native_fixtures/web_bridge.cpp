#include "web_bridge.h"
#include <cassert>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

struct Snapshot {unsigned generation;std::string text;};
using Cache=webBridge::SnapshotCache<Snapshot>;
using Jobs=webBridge::Operations<std::string,std::string,2>;

int main() {
    Cache cache;
    assert(!cache.get());
    cache.publish(std::make_shared<Snapshot>(Snapshot{0,"0"}));
    auto old=cache.get();
    Jobs jobs;
    auto browser=jobs.create("send once","one","session-a",0);
    auto *queued=browser.get();std::weak_ptr<Jobs::Job> lifetime=browser;
    browser.reset(); // Browser disconnects; the registry must keep the job alive.
    assert(!lifetime.expired() && !jobs.find("one","session-b"));
    std::promise<void> releaseWorker,workerStarted;
    auto release=releaseWorker.get_future();
    std::thread worker([&]{
        auto hold=queued->shared_from_this();workerStarted.set_value();
        release.wait(); // Simulates a stalled relay/TLS operation.
        queued->result="accepted";queued->ok=true;queued->finish(20);
    });
    workerStarted.get_future().wait();
    auto second=jobs.create("another action","two","session-b",200000);
    assert(second && !jobs.create("full","three","session-a",400000));
    // Cached GETs and session-owned operation polling work while the worker stalls.
    for(unsigned i=0;i<10000;++i) {
        auto snapshot=cache.get();assert(snapshot->text=="0");
        auto pending=jobs.find("one","session-a");assert(pending && !pending->finished());
    }
    releaseWorker.set_value();worker.join();
    auto completed=jobs.find("one","session-a");
    assert(completed->finished() && completed->ok && completed->result=="accepted");
    completed.reset();
    // Replace a completed result without dropping another pending operation.
    auto replacement=jobs.create("replacement","three","session-a",21);
    assert(replacement && !jobs.find("one","session-a") && lifetime.expired());
    assert(jobs.find("two","session-b")==second);
    replacement->ok=false;replacement->result="invalid recipient";replacement->finish(0xfffffff0);
    replacement.reset();
    auto next=jobs.create("after rollover","four","session-a",200000);
    assert(next && !jobs.find("three","session-a"));
    assert(jobs.find("four","session-a")==next);
    jobs.remove(next);assert(!jobs.find("four","session-a"));

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for(unsigned i=0;i<3;++i)readers.emplace_back([&]{
        do {
            auto snapshot=cache.get();
            assert(snapshot->text==std::to_string(snapshot->generation));
        } while(!stop.load());
    });
    for(unsigned i=1;i<=10000;++i)
        cache.publish(std::make_shared<Snapshot>(Snapshot{i,std::to_string(i)}));
    stop.store(true);for(auto &reader:readers)reader.join();
    assert(old->text=="0" && cache.get()->generation==10000);
    puts("Web bridge: cached reads during blocked relay work, immutable concurrent snapshots, session ownership, job lifetime/capacity and rollover passed.");
}
