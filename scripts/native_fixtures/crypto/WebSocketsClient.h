#pragma once
#include <Arduino.h>
#include <functional>
#include <algorithm>
#include <vector>
enum WStype_t {WStype_CONNECTED,WStype_DISCONNECTED,WStype_TEXT,WStype_ERROR};
struct WebSocketsClient {
    struct Sent {std::string host,frame;};
    std::vector<Sent> sent;
    std::function<void(WStype_t,uint8_t*,size_t)> callback;
    std::string host;
    bool waiting=false;
    static WebSocketsClient *instance;
    static std::vector<WebSocketsClient *> &all() {static std::vector<WebSocketsClient *> clients;return clients;}
    static WebSocketsClient *find(const std::string &name) {for(auto *client:all())if(client->host==name)return client;return nullptr;}
    WebSocketsClient() {instance=this;all().push_back(this);}
    ~WebSocketsClient() {
        auto &clients=all();clients.erase(std::remove(clients.begin(),clients.end(),this),clients.end());
        if(instance==this)instance=clients.empty()?nullptr:clients.back();
    }
    void onEvent(std::function<void(WStype_t,uint8_t*,size_t)> cb) {callback=cb;}
    void beginSslWithBundle(const char *name,uint16_t,const char *,const uint8_t *,const char *) {host=name;waiting=true;}
    void setReconnectInterval(uint32_t) {}
    void enableHeartbeat(uint32_t,uint32_t,uint8_t) {}
    void disconnect() {waiting=false;if(callback)callback(WStype_DISCONNECTED,nullptr,0);}
    void loop() {if(waiting){waiting=false;callback(WStype_CONNECTED,nullptr,0);}}
    void sendTXT(const char *text) {sent.push_back({host,text});}
    void emit(const std::string &text) {callback(WStype_TEXT,(uint8_t*)text.data(),text.size());}
};
