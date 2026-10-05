#pragma once
// Minimal host adapter for rendering the production LVGL UI, never used by firmware.
#include <stdlib.h>
#include <stdint.h>
#ifdef __cplusplus
#include <string>
#include <ctime>
#include <cctype>
class String {
    std::string value;
public:
    String() = default;
    String(const char *text): value(text ? text : "") {}
    String(const std::string &text): value(text) {}
    String(const char *text,size_t size): value(text,size) {}
    String(unsigned number): value(std::to_string(number)) {}
    bool startsWith(const char *prefix) const {return value.rfind(prefix,0)==0;}
    void trim() {while(!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))value.pop_back();while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))value.erase(value.begin());}
    operator std::string() const { return value; }
    bool concat(const char *text,size_t size) {value.append(text,size);return true;}
    bool concat(const char *text) {value+=text;return true;}
    String &operator+=(const String &other) {value+=other.value;return *this;}
    String &operator+=(char c) {value+=c;return *this;}
    char operator[](size_t i) const {return value[i];}
    void setCharAt(size_t i,char c) {value[i]=c;}
    void reserve(size_t size) {value.reserve(size);}
    auto begin() const -> std::string::const_iterator {return value.begin();}
    auto end() const -> std::string::const_iterator {return value.end();}
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
