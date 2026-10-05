#pragma once
#include <Arduino.h>

namespace dnsHealth {
// Core 1 only. Returns true after changing the primary resolver so the caller
// can reset relay backoff without losing pending messages or browser sessions.
bool loop();
String diagnostics();
}
