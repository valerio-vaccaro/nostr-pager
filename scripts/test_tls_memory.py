#!/usr/bin/env python3
"""Exercise the production TLS allocator with simulated ESP heaps."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='pager-tls-') as temporary:
    out = Path(temporary)
    (out/'mbedtls').mkdir()
    (out/'esp_heap_caps.h').write_text('''#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
void *heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
size_t heap_caps_get_total_size(unsigned);
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
''')
    (out/'mbedtls/platform.h').write_text('''#pragma once
#include <stddef.h>
int mbedtls_platform_set_calloc_free(void *(*)(size_t,size_t), void (*)(void *));
''')
    (out/'test.cpp').write_text('''#include "tls_memory.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <cstdio>
static void *(*allocate)(size_t,size_t)=nullptr;
static void (*release)(void *)=nullptr;
static bool psram=false, failExternal=false, failInternal=false;
static unsigned calls=0, lastCaps=0;
int mbedtls_platform_set_calloc_free(void *(*a)(size_t,size_t),void (*f)(void *)) {
    allocate=a;release=f;return 0;
}
void *heap_caps_calloc(size_t n,size_t s,unsigned caps) {
    ++calls;lastCaps=caps;
    if((caps&MALLOC_CAP_SPIRAM)?failExternal:failInternal)return nullptr;
    return std::calloc(n,s);
}
void heap_caps_free(void *p) { std::free(p); }
size_t heap_caps_get_total_size(unsigned caps) { return (caps&MALLOC_CAP_SPIRAM)?(psram?8000000:0):300000; }
size_t heap_caps_get_free_size(unsigned caps) { return (caps&MALLOC_CAP_SPIRAM)?7000000:100000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { return (caps&MALLOC_CAP_SPIRAM)?6000000:50000; }
int main() {
    assert(!tlsMemory::begin() && !allocate);psram=true;
    assert(tlsMemory::begin() && allocate && release);
    auto *p=static_cast<unsigned char *>(allocate(2,16384));
    assert(p && calls==1 && lastCaps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    for(size_t i=0;i<32768;++i)assert(p[i]==0);
    release(p);release(nullptr);
    assert(!allocate(0,8) && !allocate(8,0) && calls==1);
    assert(!allocate(std::numeric_limits<size_t>::max(),2) && calls==1);
    failExternal=true;p=static_cast<unsigned char *>(allocate(1,128));
    assert(p && calls==3 && lastCaps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    for(size_t i=0;i<128;++i)assert(p[i]==0);
    release(p);
    failInternal=true;assert(!allocate(1,16384) && calls==5);
    auto stats=tlsMemory::stats();
    assert(stats.fallbacks==1 && stats.failures==2);
    assert(stats.internalFree==100000 && stats.internalLargest==50000);
    assert(stats.psramFree==7000000 && stats.psramLargest==6000000);
    puts("TLS allocator: PSRAM preference, zeroing, fallback, overflow and failure counters passed");
}
''')
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(out), '-I'+str(root/'include'),
                    str(root/'src/tls_memory.cpp'), str(out/'test.cpp'),
                    '-o', str(out/'test')], check=True)
    subprocess.run([str(out/'test')], check=True)
