#pragma once
#include <functional>
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mutex>
#include <vector>
// Shared pager state (messages, status, relay rows, contacts) is guarded by one
// recursive lock held only for short sections. The relay task releases it around
// blocking network calls (blocking()), and the UI/web tasks never wait long:
// they use timed locks and keep their previous frame/snapshot on timeout.
namespace pagerSync {
extern SemaphoreHandle_t mutex;
extern int depth; // modified only by the current holder
extern std::mutex queueMutex;
extern std::vector<std::function<void()>> queue;
inline void begin(){if(!mutex)mutex=xSemaphoreCreateRecursiveMutex();}
inline bool lock(uint32_t waitMs=0xFFFFFFFFu) {
    if(!mutex)return true;
    TickType_t ticks=waitMs==0xFFFFFFFFu?portMAX_DELAY:pdMS_TO_TICKS(waitMs);
    if(xSemaphoreTakeRecursive(mutex,ticks)!=pdTRUE)return false;
    ++depth;return true;
}
inline void unlock(){if(!mutex)return;--depth;xSemaphoreGiveRecursive(mutex);}
// Run a blocking call (socket loop, TLS handshake, heavy crypto) with the lock released.
template<class F> void blocking(F &&fn) {
    int held=0;
    if(mutex && xSemaphoreGetMutexHolder(mutex)==xTaskGetCurrentTaskHandle()){held=depth;}
    for(int i=0;i<held;++i){--depth;xSemaphoreGiveRecursive(mutex);}
    fn();
    for(int i=0;i<held;++i){xSemaphoreTakeRecursive(mutex,portMAX_DELAY);++depth;}
}
// Defer work onto the relay task, which owns every socket and relay module.
inline void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> guard(queueMutex);
    if(queue.size()<32)queue.push_back(std::move(fn));
}
inline void drain() {
    std::vector<std::function<void()>> work;
    {std::lock_guard<std::mutex> guard(queueMutex);work.swap(queue);}
    for(auto &fn:work)fn();
}
}
#else
namespace pagerSync {
inline void begin(){}
inline bool lock(uint32_t=0){return true;}
inline void unlock(){}
template<class F> void blocking(F &&fn){fn();}
inline void post(std::function<void()> fn){fn();}
inline void drain(){}
}
#endif
namespace pagerSync {
struct Lock {
    bool held;
    explicit Lock(uint32_t waitMs=0xFFFFFFFFu):held(lock(waitMs)){}
    ~Lock(){if(held)unlock();}
    Lock(const Lock &)=delete;Lock &operator=(const Lock &)=delete;
    explicit operator bool() const {return held;}
};
}
