#pragma once
#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
struct Message {
    String id, peer, text, event, delivery, pendingWire;
    uint32_t timestamp=0;
    uint32_t expiresAt=0;
    int kind=4;
    bool outgoing=false, read=false;
};
namespace pager {
extern std::vector<Message> messages;
extern String status, publicKey, webAddress, webIpAddress, webPin, webFingerprint;
extern bool canSend;
extern bool inboxReceivedOnly; // display inbox lists received messages only (default true)
bool wifiConnected();
bool configured();
struct Configuration { String ssid, password, key, relay,publicRelays,dmRelays; bool open=false, rawPrivate=false; };
String configurationPage(const String &token);
bool applyConfiguration(const Configuration &config, String &error);
void restartSoon(const String &reason);
bool reset(bool eraseData,String &error);
String messagingDiagnostics();
void relayRows(JsonArray rows);
String publicRelayList();
String dmRelayList();
bool saveRelayLists(const String &publicList,const String &dmList,String &error);
bool restoreDefaultRelays(String &error);
void reconnectRelays();
bool deleteMessages(const String &id,bool all,String &error);
void configure();
void resume();
void markRead(const String &id);
void older();
bool send(const String &recipient,const String &text,bool publicNote,const String &replyId,String &error);
}
namespace ui {
void begin();
void loop();
void changed(bool notify=false);
void portal(const String &ssid,const String &password,const String &message);
void inbox();
}
