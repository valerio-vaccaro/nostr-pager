#!/usr/bin/env python3
"""Test the production DNS recovery module using a simulated station/DNS API."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='pager-dns-') as temporary:
    out=Path(temporary)
    (out/'WiFi.h').write_text('''#pragma once
#include <Arduino.h>
#define WL_CONNECTED 3
#define WIFI_AP 2
class IPAddress {
    uint32_t address=0;
public:
    IPAddress()=default;
    IPAddress(uint32_t value):address(value) {}
    IPAddress(unsigned a,unsigned b,unsigned c,unsigned d):address(a|(b<<8)|(c<<16)|(d<<24)) {}
    operator uint32_t() const {return address;}
    unsigned operator[](unsigned index) const {return (address>>(8*index))&255;}
    bool operator==(IPAddress other) const {return address==other.address;}
    bool operator!=(IPAddress other) const {return !(*this==other);}
    String toString() const {return String((*this)[0])+"."+String((*this)[1])+"."+String((*this)[2])+"."+String((*this)[3]);}
};
struct MockWiFi {
    bool online=false;unsigned mode=1;
    IPAddress local{192,168,1,50},gateway{192,168,1,1},ap{192,168,4,1},servers[3];
    int status() const {return online?WL_CONNECTED:0;}
    unsigned getMode() const {return mode;}
    IPAddress localIP() const {return local;}
    IPAddress gatewayIP() const {return gateway;}
    IPAddress softAPIP() const {return ap;}
    IPAddress dnsIP(unsigned slot) const {return servers[slot];}
};
extern MockWiFi WiFi;
struct MockSerial {
    template<class... Args> void printf(const char *,Args...) {}
    void println(const String &) {}
};
extern MockSerial Serial;
''')
    (out/'esp_netif.h').write_text('''#pragma once
#include <stdint.h>
#define ESP_OK 0
#define ESP_IPADDR_TYPE_V4 0
struct esp_netif_t {};
enum esp_netif_dns_type_t {ESP_NETIF_DNS_MAIN,ESP_NETIF_DNS_BACKUP,ESP_NETIF_DNS_FALLBACK};
struct esp_netif_dns_info_t {struct {int type;union {struct {uint32_t addr;} ip4;} u_addr;} ip;};
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
int esp_netif_set_dns_info(esp_netif_t *,esp_netif_dns_type_t,esp_netif_dns_info_t *);
''')
    (out/'test.cpp').write_text('''#include "dns_health.h"
#include "dns_recovery.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <cassert>
#include <cstdio>
MockWiFi WiFi;MockSerial Serial;
static uint32_t clockAt=1;static bool failWrites=false;static unsigned writes=0;
extern "C" uint32_t millis() {return clockAt;}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *) {static esp_netif_t netif;return &netif;}
int esp_netif_set_dns_info(esp_netif_t *,esp_netif_dns_type_t slot,esp_netif_dns_info_t *info) {
    ++writes;if(failWrites)return -1;WiFi.servers[slot]=IPAddress(info->ip.u_addr.ip4.addr);return 0;
}
extern "C" void websocketsDNSFailed(const char *);
extern "C" void websocketsDNSResolved(const char *);
void join(IPAddress primary,IPAddress backup) {
    WiFi.online=false;dnsHealth::loop();WiFi.servers[0]=primary;WiFi.servers[1]=backup;
    WiFi.servers[2]=IPAddress();WiFi.online=true;++clockAt;
}
void outage() {
    websocketsDNSFailed("relay.damus.io");++clockAt;
    websocketsDNSFailed("nos.lol");++clockAt;
    websocketsDNSFailed("purplepag.es");++clockAt;
}
int main() {
    const IPAddress router(192,168,1,1),second(192,168,1,2),cf(1,1,1,1),google(8,8,8,8);
    join(router,second);assert(!dnsHealth::loop());
    assert(WiFi.servers[0]==router && WiFi.servers[1]==second && WiFi.servers[2]==google);
    assert(dnsHealth::diagnostics().indexOf("192.168.1.1")>=0);
    unsigned unchanged=writes;clockAt+=31000;dnsHealth::loop();assert(writes==unchanged);
    for(unsigned i=0;i<4;++i)websocketsDNSFailed("invalid.example");
    assert(!dnsHealth::loop() && WiFi.servers[0]==router);
    websocketsDNSResolved("nos.lol");assert(!dnsHealth::loop());
    outage();assert(dnsHealth::loop());
    assert(WiFi.servers[0]==cf && WiFi.servers[1]==google && WiFi.servers[2]==router);
    outage();assert(!dnsHealth::loop()); // Minimum one-minute resolver hold.
    clockAt+=60000;outage();assert(dnsHealth::loop());assert(WiFi.servers[0]==google);
    // A fresh DHCP lease is honored without forcing a Wi-Fi disconnect/static IP.
    WiFi.servers[0]=second;WiFi.servers[1]=router;clockAt+=31000;
    assert(!dnsHealth::loop() && WiFi.servers[0]==second);
    join(WiFi.local,IPAddress());assert(dnsHealth::loop());assert(WiFi.servers[0]==cf);
    join(IPAddress(127,0,0,1),IPAddress());assert(dnsHealth::loop());assert(WiFi.servers[0]==cf);
    WiFi.mode=3;join(WiFi.ap,IPAddress());assert(dnsHealth::loop());assert(WiFi.servers[0]==cf);
    join(router,second);dnsHealth::loop();failWrites=true;outage();
    assert(!dnsHealth::loop() && WiFi.servers[0]==router);
    assert(dnsHealth::diagnostics().indexOf("DNS configuration error")>=0);
    failWrites=false;
    dnsRecovery::Policy rollover;
    rollover.failed("a",0xffffffe0);rollover.failed("b",0xfffffff0);rollover.failed("a",10);
    assert(rollover.due(20));rollover.changed(20);
    rollover.failed("a",21);rollover.failed("b",22);rollover.failed("a",23);
    assert(!rollover.due(100) && rollover.due(60020));
    puts("DNS recovery: DHCP preservation/renewal, invalid/self-address repair, cross-host failures, resolver rotation/cooldown, successful lookup reset, API failure and rollover passed.");
}
''')
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                    '-I'+str(out),'-I'+str(root/'scripts/native_fixtures/crypto'),'-I'+str(root/'include'),
                    str(root/'src/dns_health.cpp'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True)
