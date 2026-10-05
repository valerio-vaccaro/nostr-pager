#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tlsMemory {
// Install once at startup, before any certificate or TLS context is created.
bool begin();
struct Stats {
    size_t internalFree, internalLargest, psramFree, psramLargest;
    uint32_t fallbacks, failures;
};
Stats stats();
}
