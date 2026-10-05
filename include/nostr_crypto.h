#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "key_codec.h"
namespace crypto {
bool publicKey(const keys::Key &key, String &pub);
bool verify(JsonObjectConst event,String *error=nullptr);
bool identify(JsonObject event);
bool rumorValid(JsonObjectConst event);
bool wrap(JsonObjectConst rumor,const String &secret,const String &recipient,String &wire);
bool unwrap(JsonObjectConst wrapper,const String &secret,const String &own,JsonDocument &rumor,String *error=nullptr,uint32_t *expiresAt=nullptr);
bool sign(JsonObject event, const String &secret);
bool encrypt(const String &secret, const String &peer, const String &plain, String &cipher);
bool decrypt(const String &secret, const String &peer, const String &cipher, String &plain);
}
