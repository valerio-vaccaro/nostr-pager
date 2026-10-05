#pragma once
#include <openssl/rand.h>
#include <cassert>
#include <stdint.h>
inline void esp_fill_random(void *out,size_t size) {assert(RAND_bytes(static_cast<unsigned char*>(out),int(size))==1);}
inline uint32_t esp_random() {uint32_t value;esp_fill_random(&value,sizeof(value));return value;}
