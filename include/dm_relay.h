#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
namespace dmRelay {
using FrameSender=void (*)(const String &);
using DeliveryAck=void (*)(const String &,bool,const String &);
void begin(const String &secret,const String &pub,const String &relay,FrameSender send,DeliveryAck ack,const std::vector<String> &inboxes={},const std::vector<String> &publics={});
void connected();
void disconnected();
void event(JsonArrayConst frame);
void loop(bool networkReady=true);
void deliver(const String &peer,const String &event,const String &wireId);
bool authenticate(const String &challenge,const String &url,String &frame,String &id);
String status();
String publicationStatus();
void refreshContacts();
std::vector<String> inboxRelays();
void publicationRows(JsonArray rows);
void reconnect();
void cancel(const String &id);
}
