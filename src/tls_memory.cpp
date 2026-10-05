#include "tls_memory.h"
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <atomic>
#include <limits>

namespace {
std::atomic<uint32_t> fallbacks{0}, failures{0};
constexpr unsigned Internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr unsigned External = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void *allocate(size_t count, size_t size) {
    if(!count || !size)return nullptr;
    if(count > std::numeric_limits<size_t>::max()/size) {
        failures.fetch_add(1,std::memory_order_relaxed);return nullptr;
    }
    // The precompiled Arduino SDK uses an internal-only default allocator.
    // Its supported runtime hook also applies to HTTPS and WiFiClientSecure.
    void *memory=heap_caps_calloc(count,size,External);
    if(memory)return memory;
    memory=heap_caps_calloc(count,size,Internal);
    if(memory)fallbacks.fetch_add(1,std::memory_order_relaxed);
    else failures.fetch_add(1,std::memory_order_relaxed);
    return memory;
}
void release(void *memory) { heap_caps_free(memory); }
}

bool tlsMemory::begin() {
    if(!heap_caps_get_total_size(External))return false;
    return mbedtls_platform_set_calloc_free(allocate,release)==0;
}
tlsMemory::Stats tlsMemory::stats() {
    return {heap_caps_get_free_size(Internal),heap_caps_get_largest_free_block(Internal),
            heap_caps_get_free_size(External),heap_caps_get_largest_free_block(External),
            fallbacks.load(std::memory_order_relaxed),failures.load(std::memory_order_relaxed)};
}
