#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace nip44 {
constexpr size_t MaxPlaintext=4096; // Includes inner JSON envelopes; chat text remains limited to 512 bytes.
size_t paddedSize(size_t size);
bool conversation(const uint8_t secret[32],const uint8_t peer[32],uint8_t key[32]);
bool encrypt(const uint8_t key[32],const uint8_t nonce[32],const std::string &plain,std::vector<uint8_t> &payload);
bool decrypt(const uint8_t key[32],const std::vector<uint8_t> &payload,std::string &plain);
}
