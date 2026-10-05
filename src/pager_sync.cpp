#include "pager_sync.h"
#if defined(ARDUINO_ARCH_ESP32)
namespace pagerSync {
SemaphoreHandle_t mutex=nullptr;
int depth=0;
std::mutex queueMutex;
std::vector<std::function<void()>> queue;
}
#endif
