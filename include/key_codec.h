#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace keys {
struct Key { std::string hex; bool secret = false; };
bool decode(std::string input, bool hexIsSecret, Key &out, std::string &error);
bool unhex(const std::string &hex, uint8_t *out, size_t size);
std::string hex(const uint8_t *data, size_t size);
std::string npub(const std::string &hexKey);
struct Relay { std::string host, path; uint16_t port = 443; };
bool relay(const std::string &url, Relay &out);
}
