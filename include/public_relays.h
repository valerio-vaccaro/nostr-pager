#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
namespace publicRelays {
constexpr size_t Limit=6;
struct Status {
    String url,state="Queued",error,publication="No publication";
    bool connected=false,subscribed=false,rejected=false;
    bool publicSubscribed=false,dmSubscribed=false;
    bool publicRelay=true,dmInbox=false;
    uint32_t events=0,failures=0,retryAt=0;
    uint8_t attempts=0;
};
using Receiver=void (*)(JsonObjectConst);
using Ack=void (*)(const String &,bool,const String &);
using Auth=bool (*)(const String &,const String &,String &,String &);
String defaults();
bool parse(const String &input,std::vector<String> &urls,String &error);
void begin(const String &pub,const String &inbox,const std::vector<String> &urls,Receiver receive,Ack ack,Auth auth,bool privateMode=false);
void inboxes(const std::vector<String> &urls);
void loop(bool ready);
void reconnect();
void setBudget(size_t sockets);
void older(uint32_t until);
bool publish(const String &event,const String &id);
void primaryRejected(const String &id,const String &reason);
void clear(const String &id);
void cancel(const String &id);
const std::vector<Status> &statuses();
}
