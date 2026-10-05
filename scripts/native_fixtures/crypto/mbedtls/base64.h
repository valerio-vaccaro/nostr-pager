#pragma once
#include <openssl/evp.h>
#include <cstring>
inline int mbedtls_base64_encode(unsigned char *out,size_t capacity,size_t *written,const unsigned char *input,size_t size) {
    if(capacity<4*((size+2)/3)+1)return -1;
    *written=EVP_EncodeBlock(out,input,int(size));return 0;
}
inline int mbedtls_base64_decode(unsigned char *out,size_t capacity,size_t *written,const unsigned char *input,size_t size) {
    if(size%4 || capacity<size)return -1;
    int count=EVP_DecodeBlock(out,input,int(size));if(count<0)return -1;
    if(size && input[size-1]=='=')--count;if(size>1 && input[size-2]=='=')--count;
    *written=count;return 0;
}
