#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>
#include <utility>

namespace webBridge {
// Exchange immutable snapshots. The lock protects only a shared_ptr swap/copy;
// building JSON, rendering pages and networking never hold it.
template<class T> class SnapshotCache {
    std::mutex mutex;
    std::shared_ptr<const T> current;
public:
    void publish(std::shared_ptr<const T> next) {
        {std::lock_guard<std::mutex> lock(mutex);current.swap(next);}
    }
    std::shared_ptr<const T> get() {
        std::lock_guard<std::mutex> lock(mutex);return current;
    }
};

template<class Request,class Result> struct Operation:
        std::enable_shared_from_this<Operation<Request,Result>> {
    Request request;
    Result result;
    bool ok=true;
    const std::string id,owner;
    uint32_t completedAt=0;
private:
    std::atomic<bool> ready{false};
public:
    Operation(Request input,std::string identifier,std::string session):
        request(std::move(input)),id(std::move(identifier)),owner(std::move(session)) {}
    // Worker writes result/ok, then publishes completion; reader checks finished
    // before accessing them. No worker access is allowed after finish().
    void finish(uint32_t now) {completedAt=now;ready.store(true,std::memory_order_release);}
    bool finished() const {return ready.load(std::memory_order_acquire);}
};

// Registry is used only by the HTTPS task. It owns queued jobs until the worker
// completes them, regardless of browser disconnects or elapsed time.
template<class Request,class Result,size_t Capacity=4> class Operations {
public:
    using Job=Operation<Request,Result>;
    using Handle=std::shared_ptr<Job>;
private:
    std::array<Handle,Capacity> entries{};
public:
    Handle create(Request request,const std::string &id,const std::string &owner,uint32_t now) {
        size_t slot=Capacity;
        for(size_t i=0;i<Capacity;++i) {
            if(entries[i] && entries[i]->finished() && now-entries[i]->completedAt>=180000)entries[i].reset();
            if(!entries[i] && slot==Capacity)slot=i;
        }
        if(slot==Capacity) {
            // Under pressure only completed results may be replaced.
            uint32_t age=0;
            for(size_t i=0;i<Capacity;++i)if(entries[i]->finished() &&
                    (slot==Capacity || now-entries[i]->completedAt>=age)) {
                slot=i;age=now-entries[i]->completedAt;
            }
        }
        if(slot==Capacity)return {};
        auto job=std::make_shared<Job>(std::move(request),id,owner);
        entries[slot]=job;return job;
    }
    Handle find(const std::string &id,const std::string &owner) const {
        for(const auto &job:entries)if(job && job->id==id && job->owner==owner)return job;
        return {};
    }
    void remove(const Handle &job) {
        for(auto &entry:entries)if(entry==job){entry.reset();return;}
    }
};
}
