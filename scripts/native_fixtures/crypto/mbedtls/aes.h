#pragma once
#include <openssl/evp.h>
#include <cstring>
#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0
struct mbedtls_aes_context {unsigned char key[32];};
inline void mbedtls_aes_init(mbedtls_aes_context *) {}
inline void mbedtls_aes_free(mbedtls_aes_context *ctx) {memset(ctx,0,sizeof(*ctx));}
inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx,const unsigned char *key,unsigned bits) {if(bits!=256)return -1;memcpy(ctx->key,key,32);return 0;}
inline int mbedtls_aes_setkey_dec(mbedtls_aes_context *ctx,const unsigned char *key,unsigned bits) {return mbedtls_aes_setkey_enc(ctx,key,bits);}
inline int mbedtls_aes_crypt_cbc(mbedtls_aes_context *ctx,int mode,size_t length,unsigned char *iv,const unsigned char *input,unsigned char *output) {
    auto *evp=EVP_CIPHER_CTX_new();int count=0,tail=0;
    bool ok=evp && EVP_CipherInit_ex(evp,EVP_aes_256_cbc(),nullptr,ctx->key,iv,mode)==1 &&
        EVP_CIPHER_CTX_set_padding(evp,0)==1 && EVP_CipherUpdate(evp,output,&count,input,int(length))==1 &&
        EVP_CipherFinal_ex(evp,output+count,&tail)==1 && size_t(count+tail)==length;
    EVP_CIPHER_CTX_free(evp);return ok?0:-1;
}
