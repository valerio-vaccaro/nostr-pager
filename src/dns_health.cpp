#include "dns_health.h"
#include "dns_recovery.h"
#include <WiFi.h>
#include <esp_netif.h>

namespace {
dnsRecovery::Policy policy;
IPAddress station,routerDns,routerBackup;
bool connected=false;
unsigned mode=0; // 0: DHCP/missing-server repair, 1: Cloudflare, 2: Google
const IPAddress Cloudflare(1,1,1,1),Google(8,8,8,8);
uint32_t checkedAt=0;
String configurationError;
bool valid(IPAddress ip) {
    return ip[0]!=0 && ip[0]!=127 && ip[0]<224 && ip!=WiFi.localIP() &&
           (!(WiFi.getMode()&WIFI_AP) || ip!=WiFi.softAPIP());
}
bool setServer(esp_netif_t *netif,unsigned slot,IPAddress ip) {
    if(WiFi.dnsIP(slot)==ip)return true;
    esp_netif_dns_info_t info{};info.ip.type=ESP_IPADDR_TYPE_V4;info.ip.u_addr.ip4.addr=uint32_t(ip);
    if(esp_netif_set_dns_info(netif,esp_netif_dns_type_t(slot),&info)==ESP_OK)return true;
    configurationError="Could not configure DNS slot "+String(slot);return false;
}
bool install(unsigned next) {
    auto *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!netif){configurationError="Station network interface unavailable";return false;}
    IPAddress primary=next==1?Cloudflare:next==2?Google:valid(routerDns)?routerDns:Cloudflare;
    IPAddress backup=next==1?Google:next==2?Cloudflare:valid(routerBackup)?routerBackup:(primary==Cloudflare?Google:Cloudflare);
    IPAddress fallback=next && valid(routerDns) && routerDns!=primary && routerDns!=backup?routerDns:(backup==Google?Cloudflare:Google);
    configurationError="";
    // Apply primary last: a failed auxiliary update must not initiate failover.
    return setServer(netif,1,backup) && setServer(netif,2,fallback) && setServer(netif,0,primary);
}
void logServers() {
    Serial.printf("[DNS] station=%s gateway=%s; main=%s backup=%s fallback=%s; mode=%u\n",
        WiFi.localIP().toString().c_str(),WiFi.gatewayIP().toString().c_str(),
        WiFi.dnsIP(0).toString().c_str(),WiFi.dnsIP(1).toString().c_str(),WiFi.dnsIP(2).toString().c_str(),mode);
}
}

// Optional hooks in the vendored ESP32 WebSockets client. WiFiClientSecure in
// Arduino 2.0.14 leaves lastError()==0 only when connect(host) fails DNS lookup;
// TCP/TLS failures set a nonzero error. All relay clients use this same path.
extern "C" void websocketsDNSFailed(const char *host) {
    policy.failed(host,millis());
    Serial.printf("[DNS] lookup failed: %s; recent failures=%u\n",host,policy.streak);
}
extern "C" void websocketsDNSResolved(const char *) {policy.resetBurst();}

bool dnsHealth::loop() {
    if(WiFi.status()!=WL_CONNECTED){connected=false;return false;}
    if(!connected || station!=WiFi.localIP()) {
        connected=true;station=WiFi.localIP();routerDns=WiFi.dnsIP(0);routerBackup=WiFi.dnsIP(1);
        mode=0;policy.resetBurst();policy.hasChanged=false;checkedAt=millis();
        bool missing=!valid(routerDns);
        if(install(0)){logServers();return missing;}
        logServers();return false;
    }
    if(policy.due(millis())) {
        unsigned next=mode==1?2:1;
        if(install(next)) {
            mode=next;policy.changed(millis());checkedAt=millis();
            Serial.println("[DNS] multiple relay lookups failed: trying another resolver");logServers();return true;
        }
        policy.changed(millis()); // Bound retries even if the API itself fails.
        Serial.println(configurationError);return false;
    }
    if(millis()-checkedAt>=30000) {
        checkedAt=millis();
        IPAddress primary=WiFi.dnsIP(0);
        IPAddress expected=mode==1?Cloudflare:mode==2?Google:valid(routerDns)?routerDns:Cloudflare;
        if(primary!=expected && valid(primary)) {
            // A DHCP renewal may replace DNS. Honor it instead of pinning a
            // resolver from the previous lease/network indefinitely.
            routerDns=primary;routerBackup=WiFi.dnsIP(1);mode=0;policy.resetBurst();policy.hasChanged=false;
        }
        bool repairedPrimary=!valid(primary);
        if(install(mode) && repairedPrimary){logServers();return true;}
    }
    return false;
}
String dnsHealth::diagnostics() {
    String result="Station IP: "+WiFi.localIP().toString()+" / gateway: "+WiFi.gatewayIP().toString();
    result+="\nDNS main: "+WiFi.dnsIP(0).toString()+" / backup: "+WiFi.dnsIP(1).toString()+" / fallback: "+WiFi.dnsIP(2).toString();
    result+="\nDNS policy: "+String(mode==1?"Cloudflare recovery":mode==2?"Google recovery":"DHCP with fallback resolvers");
    result+="\nRelay DNS failures: "+String(policy.failures);
    if(!policy.lastHost.empty())result+=" / last: "+String(policy.lastHost.c_str());
    if(!configurationError.isEmpty())result+="\nDNS configuration error: "+configurationError;
    return result;
}
