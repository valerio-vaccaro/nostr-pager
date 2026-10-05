#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
namespace contacts {
constexpr size_t Limit=24;
struct Contact {String pubkey,alias,name,profileId;uint32_t updated=0;};
using Persist=bool (*)(const String &);
using Changed=void (*)();
using FrameSender=void (*)(const String &);
void begin(const String &saved,Persist persist,Changed changed);
const std::vector<Contact> &all();
String label(const Contact &contact);
String display(const String &pubkey);
bool save(const String &input,const String &alias,String &error);
bool remove(const String &input,String &error);
bool resolve(const String &input,String &pubkey,String &error);
void refresh();
bool refreshPending();
void refreshed();
void query(FrameSender send);
void event(JsonArrayConst frame);
String status();
}
