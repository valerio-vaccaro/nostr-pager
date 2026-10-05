#pragma once
// Minimal host adapter for rendering the production LVGL UI, never used by firmware.
#include <stdlib.h>
#include <stdint.h>
#ifdef __cplusplus
#include <string>
#include <ctime>
class String {
    std::string value;
public:
    String() = default;
    String(const char *text): value(text ? text : "") {}
    String(const std::string &text): value(text) {}
    String(unsigned number): value(std::to_string(number)) {}
    const char *c_str() const { return value.c_str(); }
    bool isEmpty() const { return value.empty(); }
    size_t length() const { return value.size(); }
    String substring(size_t begin, size_t end = std::string::npos) const {
        if (begin >= value.size()) return "";
        return value.substr(begin, end == std::string::npos ? end : end - begin);
    }
    int indexOf(const char *text) const {
        size_t at = value.find(text);
        return at == std::string::npos ? -1 : int(at);
    }
    friend String operator+(const String &a, const String &b) { return a.value + b.value; }
    friend bool operator==(const String &a, const String &b) { return a.value == b.value; }
    friend bool operator!=(const String &a, const String &b) { return !(a == b); }
};
extern "C" {
#endif
uint32_t millis(void);
static inline void *ps_malloc(size_t size) { return malloc(size); }
static inline void *ps_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
#ifdef __cplusplus
}
#endif
