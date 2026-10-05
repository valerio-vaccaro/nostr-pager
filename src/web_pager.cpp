#include "web_pager.h"
#include "web_auth.h"
#include "pager.h"
#include "pager_sync.h"
#include "nostr_crypto.h"
#include "dm_relay.h"
#include "version.h"
#include "contacts.h"
#include "reset_control.h"
#include "tls_memory.h"
#include "web_bridge.h"
#include "message_expiry.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_https_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <new>
#include <memory>
#include <algorithm>

namespace {
httpd_handle_t https=nullptr;
QueueHandle_t work=nullptr;
Preferences tlsStore;
String certificate,privateKey;
webAuth::Guard guard;
String pin;
uint32_t retryAt=0;
bool mdnsStarted=false;
String mdnsAddress;
uint32_t mdnsRetryAt=0;
enum class Action { Save, Send, Read, Older, Renew, ContactSave, ContactDelete, ContactRefresh, Reset, RelaySave, RelayDefaults, Reconnect, Delete };
struct Request {
    Action action=Action::Send;
    String input,back;
};
webBridge::Operations<Request,String> operations;
using Job=webBridge::Operations<Request,String>::Job;
struct Snapshot {String state,setup,etag;uint32_t at=0;bool ready=false;};
webBridge::SnapshotCache<Snapshot> snapshots;
uint32_t snapshotAt=0;
constexpr const char *CsrfPlaceholder="__PAGER_SESSION_CSRF__";
void refreshSnapshot(bool force);
int randomBytes(void *,unsigned char *out,size_t size) { esp_fill_random(out,size);return 0; }
String randomToken() { uint8_t bytes[16];esp_fill_random(bytes,sizeof(bytes));return keys::hex(bytes,sizeof(bytes)).c_str(); }
String escape(const String &input) {
    String out;
    for(char c:input) switch(c) {
        case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;
        case '\"':out+="&quot;";break;case '\'':out+="&#39;";break;default:out+=c;
    }
    return out;
}
String address() {
    // Prefer the real LAN address; the setup access point (192.168.4.1) is only the fallback.
    if(WiFi.status()==WL_CONNECTED)return WiFi.localIP().toString();
    if(WiFi.getMode()&WIFI_AP)return WiFi.softAPIP().toString();
    return WiFi.localIP().toString();
}
void updateAddresses() {
    bool online=https && (WiFi.status()==WL_CONNECTED || (WiFi.getMode()&WIFI_AP));
    String ip=online?"https://"+address()+"/":String("");
    String url=online && mdnsStarted && WiFi.status()==WL_CONNECTED?String("https://pager.local/"):ip;
    pagerSync::Lock guard;
    if(pager::webAddress!=url || pager::webIpAddress!=ip){pager::webAddress=url;pager::webIpAddress=ip;ui::changed();}
}
void updateMdns() {
    bool online=https && (WiFi.status()==WL_CONNECTED || (WiFi.getMode()&WIFI_AP));
    if(!online) {
        if(mdnsStarted){MDNS.end();mdnsStarted=false;}mdnsRetryAt=0;return;
    }
    // Re-announce on the interface that is actually reachable when the address changes
    // (for example when Wi-Fi joins while the setup access point is up).
    if(mdnsStarted && mdnsAddress!=address()){MDNS.end();mdnsStarted=false;mdnsRetryAt=0;}
    if(mdnsStarted || (mdnsRetryAt && int32_t(millis()-mdnsRetryAt)<0))return;
    mdnsRetryAt=millis()+30000;
    if(!MDNS.begin("pager")){MDNS.end();Serial.println("mDNS startup failed / retrying");return;}
    MDNS.setInstanceName("Nostr Pager");
    if(!MDNS.addService("https","tcp",443) || mdns_service_txt_item_set("_https","_tcp","path","/")!=ESP_OK) {
        MDNS.end();Serial.println("mDNS HTTPS service failed / retrying");return;
    }
    mdnsStarted=true;mdnsAddress=address();Serial.printf("HTTPS mDNS address: https://pager.local/ -> %s\n",mdnsAddress.c_str());
}
bool generateCertificate() {
    // Keep PEM staging buffers off the task stack: mbedTLS signing itself
    // needs substantial stack space, especially with hardware MPI enabled.
    constexpr size_t KeyPemSize=1024,CertPemSize=2048;
    std::unique_ptr<unsigned char[]> keyPem(new(std::nothrow) unsigned char[KeyPemSize]{});
    std::unique_ptr<unsigned char[]> crtPem(new(std::nothrow) unsigned char[CertPemSize]{});
    if(!keyPem || !crtPem){Serial.println("HTTPS certificate: PEM allocation failed");return false;}
    Serial.printf("HTTPS certificate: generating; loop stack minimum free=%u bytes\n",
        unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    mbedtls_x509write_cert crt;mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi serial;mbedtls_mpi_init(&serial);
    unsigned char serialBytes[16];esp_fill_random(serialBytes,sizeof(serialBytes));serialBytes[0]&=0x7f;serialBytes[15]|=1;
    int rc=mbedtls_pk_setup(&key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if(!rc)rc=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(key),randomBytes,nullptr);
    if(!rc)rc=mbedtls_mpi_read_binary(&serial,serialBytes,sizeof(serialBytes));
    mbedtls_x509write_crt_set_version(&crt,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt,&key);mbedtls_x509write_crt_set_issuer_key(&crt,&key);
    if(!rc)rc=mbedtls_x509write_crt_set_serial(&crt,&serial);
    if(!rc)rc=mbedtls_x509write_crt_set_subject_name(&crt,"CN=Nostr Pager,O=Nostr Pager");
    if(!rc)rc=mbedtls_x509write_crt_set_issuer_name(&crt,"CN=Nostr Pager,O=Nostr Pager");
    // Provisioning works before NTP; trust is established by the device fingerprint.
    if(!rc)rc=mbedtls_x509write_crt_set_validity(&crt,"20200101000000","20491231235959");
    if(!rc)rc=mbedtls_x509write_crt_set_basic_constraints(&crt,0,-1);
    if(!rc)rc=mbedtls_x509write_crt_set_key_usage(&crt,MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    unsigned char san[27]={0x30,25,0x87,4,192,168,4,1,0x87,4,0,0,0,0,
        0x82,11,'p','a','g','e','r','.','l','o','c','a','l'};
    IPAddress ip=WiFi.status()==WL_CONNECTED?WiFi.localIP():WiFi.softAPIP();
    for(int i=0;i<4;++i)san[10+i]=ip[i];
    if(!rc)rc=mbedtls_x509write_crt_set_extension(&crt,MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),0,san,sizeof(san));
    if(!rc)rc=mbedtls_pk_write_key_pem(&key,keyPem.get(),KeyPemSize);
    if(!rc)rc=mbedtls_x509write_crt_pem(&crt,crtPem.get(),CertPemSize,randomBytes,nullptr);
    bool ok=false;
    if(!rc) {
        DynamicJsonDocument doc(4096);doc["cert"]=(char*)crtPem.get();doc["key"]=(char*)keyPem.get();doc["hostname"]="pager.local";
        String identity;serializeJson(doc,identity);
        if(!doc.overflowed() && tlsStore.putString("identity",identity)==identity.length()) {
            certificate=(char*)crtPem.get();privateKey=(char*)keyPem.get();tlsStore.putBool("renew",false);ok=true;
        }
    }
    mbedtls_platform_zeroize(keyPem.get(),KeyPemSize);mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);mbedtls_pk_free(&key);
    Serial.printf("HTTPS certificate: %s; mbedTLS result=%d; loop stack minimum free=%u bytes\n",
        ok?"saved":"failed",rc,unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    return ok;
}
bool fingerprint() {
    mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    bool ok=mbedtls_x509_crt_parse(&crt,(const uint8_t*)certificate.c_str(),certificate.length()+1)==0;
    if(ok)ok=mbedtls_pk_parse_key(&key,(const uint8_t*)privateKey.c_str(),privateKey.length()+1,nullptr,0)==0;
    if(ok)ok=mbedtls_pk_check_pair(&crt.pk,&key)==0;
    uint8_t hash[32];
    if(ok)ok=mbedtls_sha256_ret(crt.raw.p,crt.raw.len,hash,0)==0;
    if(ok){pagerSync::Lock guard;pager::webFingerprint=keys::hex(hash,sizeof(hash)).c_str();}
    mbedtls_pk_free(&key);mbedtls_x509_crt_free(&crt);return ok;
}
String header(httpd_req_t *req,const char *name) {
    size_t length=httpd_req_get_hdr_value_len(req,name);
    if(!length || length>1024)return "";
    char value[1025];
    if(httpd_req_get_hdr_value_str(req,name,value,sizeof(value))!=ESP_OK)return "";
    return value;
}
String cookieToken(httpd_req_t *req) {
    String cookies=header(req,"Cookie");
    int start=0;
    while(start<int(cookies.length())) {
        int end=cookies.indexOf(';',start);if(end<0)end=cookies.length();
        String part=cookies.substring(start,end);part.trim();
        if(part.startsWith("pager_session="))return part.substring(14);
        start=end+1;
    }
    return "";
}
void securityHeaders(httpd_req_t *req) {
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    // no-referrer makes browser form POSTs send Origin: null, which our
    // origin check correctly rejects. Preserve the origin for local forms.
    httpd_resp_set_hdr(req,"Referrer-Policy","same-origin");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; style-src 'unsafe-inline'; script-src 'self'; connect-src 'self'; img-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}
esp_err_t respond(httpd_req_t *req,const String &body,const char *status="200 OK",const char *type="text/html; charset=utf-8") {
    securityHeaders(req);httpd_resp_set_status(req,status);httpd_resp_set_type(req,type);
    return httpd_resp_send(req,body.c_str(),body.length());
}
esp_err_t redirect(httpd_req_t *req,const char *to) {
    securityHeaders(req);httpd_resp_set_status(req,"303 See Other");httpd_resp_set_hdr(req,"Location",to);
    return httpd_resp_send(req,nullptr,0);
}
String decode(const String &input,bool &ok) {
    String out;
    auto hex=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=0;i<input.length();++i) {
        char c=input[i];
        if(c=='+')c=' ';
        else if(c=='%') {
            if(i+2>=input.length() || hex(input[i+1])<0 || hex(input[i+2])<0){ok=false;return "";}
            c=char(hex(input[i+1])*16+hex(input[i+2]));i+=2;
        }
        if(c==0){ok=false;return "";}
        out+=c;
    }
    return out;
}
bool parseForm(const String &input,JsonDocument &doc) {
    int start=0;bool ok=true;
    while(start<int(input.length())) {
        int end=input.indexOf('&',start);if(end<0)end=input.length();
        String part=input.substring(start,end);int split=part.indexOf('=');
        if(split<1)return false;
        String name=decode(part.substring(0,split),ok),value=decode(part.substring(split+1),ok);
        if(!ok || name.length()>32 || doc.containsKey(name))return false;
        doc[name]=value;start=end+1;
    }
    return !doc.overflowed();
}
bool readForm(httpd_req_t *req,JsonDocument &doc) {
    if(req->content_len>8192 || !header(req,"Content-Type").startsWith("application/x-www-form-urlencoded"))return false;
    String body;body.reserve(req->content_len);
    char block[256];size_t remaining=req->content_len;
    while(remaining) {
        int count=httpd_req_recv(req,block,std::min(remaining,sizeof(block)));
        if(count<=0)return false;
        body.concat(block,count);remaining-=count;
    }
    return parseForm(body,doc);
}
String query(httpd_req_t *req,const char *name) {
    size_t size=httpd_req_get_url_query_len(req);
    if(!size || size>1024)return "";
    char text[1025];if(httpd_req_get_url_query_str(req,text,sizeof(text))!=ESP_OK)return "";
    DynamicJsonDocument doc(2048);if(!parseForm(text,doc))return "";
    return doc[name]|"";
}
#include "web_assets.h"
String page(const String &body) {
    String script="<script src='/app.js' defer></script>";
    return String(webHead)+body+script+"<footer><small>Firmware "+escape(PAGER_VERSION)+"</small></footer></main></body></html>";
}
String hidden(const String &csrf) {return "<input type='hidden' name='csrf' value='"+escape(csrf)+"'>";}
String postButton(const String &path,const String &text,const String &csrf) {
    return "<form class='inline' method='post' action='"+path+"'>"+hidden(csrf)+"<button>"+text+"</button></form>";
}
String relayTable(JsonArrayConst rows) {
    String body="<div class='contact-table-wrap'><table class='contact-table relay-table'><thead><tr><th>Relay</th><th>Connection / inbox</th><th>Events</th><th>Public publication</th></tr></thead><tbody>";
    for(JsonObjectConst row:rows) {
        body+="<tr><td><code>"+escape(row["url"].as<String>())+"</code><small>"+escape(row["role"].as<String>())+"</small></td><td>"+escape(row["state"].as<String>());
        if((row["retry"]|0U)>0)body+="<small>Retry in "+String(row["retry"].as<unsigned>())+" s</small>";
        body+="<small>Public: "+String((row["publicSubscribed"]|false)?"subscribed":"inactive")+" / DM: "+String((row["dmSubscribed"]|false)?"subscribed":"inactive")+"</small>";
        body+="<small>"+String(row["failures"].as<unsigned>())+" connection failures</small>";
        if(!row["error"].as<String>().isEmpty())body+="<small class='error'>"+escape(row["error"].as<String>())+"</small>";
        body+="</td><td>"+String(row["events"].as<unsigned>())+"</td><td>"+escape(row["publication"].as<String>())+"</td></tr>";
    }
    return body+"</tbody></table></div>";
}
String publicationTable(JsonArrayConst rows) {
    String body="<div class='contact-table-wrap'><table class='contact-table relay-table'><thead><tr><th>Relay</th><th>Accepted</th><th>Readback verified</th><th>Result</th></tr></thead><tbody>";
    for(JsonObjectConst row:rows)body+="<tr><td><code>"+escape(row["url"].as<String>())+"</code></td><td>"+String((row["accepted"]|false)?"Yes":"No")+"</td><td>"+String((row["verified"]|false)?"Yes":"No")+"</td><td>"+escape(row["result"].as<String>())+"</td></tr>";
    if(rows.size()==0)body+="<tr><td colspan='4'>Waiting for DM relay discovery / private key required.</td></tr>";
    return body+"</tbody></table></div>";
}
String connectionPrompt(bool hidden=false) {
    return String("<section id='connection-required'")+(hidden?" hidden":"")+
        "><h1>Connect your pager</h1><p id='connection-hint'>Connect the pager to Wi-Fi and configure your Nostr identity to use messages.</p>"
        "<a class='button' href='/setup'>Configure Wi-Fi and identity</a>"
        "<p class='muted'>Saved Wi-Fi reconnects automatically. This page checks the connection.</p>"
        "<a href='/messages'>Check connection</a></section>";
}
esp_err_t login(httpd_req_t *req) {
    if(req->method==HTTP_GET)return respond(req,page("<div class='eyebrow'>DEVICE ACCESS</div><h1>Your private signal.</h1><p>Open Setup on the pager display to read the six-digit PIN.</p><form method='post' action='/login'><label>Session PIN</label><input name='pin' type='password' inputmode='numeric' pattern='[0-9]{6}' minlength='6' maxlength='6' autocomplete='off' required autofocus><button>Unlock pager</button></form><p class='muted'>This login expires after 30 minutes without activity. The PIN changes when the device restarts.</p>"));
    DynamicJsonDocument form(1024);
    if(!readForm(req,form))return respond(req,"Invalid form.","400 Bad Request","text/plain");
    if(guard.limited(millis())){httpd_resp_set_hdr(req,"Retry-After","60");return respond(req,"Too many attempts. Wait one minute.","429 Too Many Requests","text/plain");}
    String input=form["pin"]|"";
    if(!guard.checkPin(input.c_str(),pin.c_str(),millis()))return respond(req,page("<h1>PIN required</h1><p>The PIN was not accepted. Read the current PIN on the display.</p><a class='button' href='/login'>Try again</a>"),"401 Unauthorized");
    String token=randomToken(),csrf=randomToken();guard.create(token.c_str(),csrf.c_str(),millis());
    String cookie="pager_session="+token+"; Path=/; Secure; HttpOnly; SameSite=Strict";
    httpd_resp_set_hdr(req,"Set-Cookie",cookie.c_str());return redirect(req,"/messages");
}
esp_err_t handler(httpd_req_t *req) {
    String path=req->uri;int question=path.indexOf('?');if(question>=0)path=path.substring(0,question);
    if(req->method==HTTP_POST) {
        String origin=header(req,"Origin"),host=header(req,"Host");
        if(!origin.isEmpty() && origin!="https://"+host)
            return respond(req,"Cross-origin request refused.","403 Forbidden","text/plain");
    }
    if(path=="/app.js")return respond(req,webScript,"200 OK","application/javascript"); // public: login page animation
    if(path=="/login")return login(req);
    String token=cookieToken(req);
    auto *session=guard.find(token.c_str(),millis());
    if(!session) {
        if(path=="/")return redirect(req,"/login");
        return respond(req,"PIN login required. Open /login.","401 Unauthorized","text/plain");
    }
    String csrf=session->csrf.c_str();
    if(req->method==HTTP_POST) {
        DynamicJsonDocument form(8192);
        if(!readForm(req,form))return respond(req,"Invalid or oversized form.","400 Bad Request","text/plain");
        String supplied=form["csrf"]|"";
        if(!webAuth::equal(supplied.c_str(),csrf.c_str()))return respond(req,"Invalid session token.","403 Forbidden","text/plain");
        if(path=="/logout") {
            guard.remove(token.c_str(),millis());
            httpd_resp_set_hdr(req,"Set-Cookie","pager_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict");
            return redirect(req,"/login");
        }
        Request request;
        if(path=="/send")request.action=Action::Send;
        else if(path=="/read")request.action=Action::Read;
        else if(path=="/older")request.action=Action::Older;
        else if(path=="/certificate/renew")request.action=Action::Renew;
        else if(path=="/save")request.action=Action::Save;
        else if(path=="/contacts/save")request.action=Action::ContactSave;
        else if(path=="/contacts/delete")request.action=Action::ContactDelete;
        else if(path=="/contacts/refresh")request.action=Action::ContactRefresh;
        else if(path=="/device/reset")request.action=Action::Reset;
        else if(path=="/relays/save")request.action=Action::RelaySave;
        else if(path=="/relays/defaults")request.action=Action::RelayDefaults;
        else if(path=="/relays/reconnect")request.action=Action::Reconnect;
        else if(path=="/messages/delete")request.action=Action::Delete;
        else return respond(req,"Not found.","404 Not Found","text/plain");
        serializeJson(form,request.input);
        request.back=path.startsWith("/contacts/")?"/contacts":path.startsWith("/relays/")?"/relays":"/messages";
        String id=randomToken();
        auto job=operations.create(std::move(request),id.c_str(),token.c_str(),millis());
        if(!job)return respond(req,"Action queue full. Nothing was submitted; try again shortly.","503 Service Unavailable","text/plain");
        Job *pointer=job.get();
        if(!work || xQueueSend(work,&pointer,0)!=pdTRUE) {
            operations.remove(job);
            return respond(req,"Action queue full. Nothing was submitted; try again shortly.","503 Service Unavailable","text/plain");
        }
        String target="/operations?id="+id;
        return redirect(req,target.c_str());
    }
    if(path=="/operations") {
        String id=query(req,"id");
        auto job=operations.find(id.c_str(),token.c_str());
        if(!job)return respond(req,"Operation expired or not available for this session.","404 Not Found","text/plain");
        if(!job->finished()) {
            String refresh="1; url=/operations?id="+id;
            httpd_resp_set_hdr(req,"Refresh",refresh.c_str());
            String body="<h1>Action queued</h1><p>The pager will process it when its messaging worker is available. This page checks automatically; do not submit the action again.</p>";
            if(job->request.action==Action::Renew || job->request.action==Action::Save || job->request.action==Action::Reset)
                body+="<p>If this action succeeds, the pager restarts. Open Setup on the display for the new PIN and address, then reconnect.</p>";
            return respond(req,page(body+"<a class='button secondary' href='"+job->request.back+"'>Continue browsing</a>"),"202 Accepted");
        }
        if(!job->ok)return respond(req,page("<h1>Check your input</h1><p class='error'>"+escape(job->result)+"</p><p>Use your browser's Back button to keep your draft.</p><a class='button secondary' href='"+job->request.back+"'>Continue browsing</a>"),"400 Bad Request");
        if(job->request.action==Action::Renew || job->request.action==Action::Save || job->request.action==Action::Reset) {
            guard.remove(token.c_str(),millis());
            return respond(req,page("<h1>Restarting</h1><p>"+escape(job->result)+"</p><p>Open Setup on the display to read the new PIN and HTTPS address, then reconnect. A new certificate requires accepting the certificate again.</p>"));
        }
        return redirect(req,job->request.back.c_str());
    }
    if(path=="/" )return redirect(req,"/messages");
    if(path=="/reset/confirm") {
        String mode=query(req,"mode");bool erase=mode=="erase";
        if(mode!="restart" && !erase)return respond(req,"Invalid reset mode.","400 Bad Request","text/plain");
        String body=String("<h1>")+(erase?"Delete data and reset?":"Reboot the pager?")+"</h1>";
        body+=erase?"<p>This deletes Wi-Fi credentials, your Nostr identity, all messages, contacts and the HTTPS certificate. Keep a separate backup of your private key. Deletion cannot be undone. The pager restarts in first-time setup.</p>":"<p>Reboot the board? Nothing is deleted: settings, messages, contacts and the certificate stay saved. Browser sessions end and the setup PIN changes.</p>";
        body+="<form method='post' action='/device/reset'>"+hidden(csrf)+"<input type='hidden' name='mode' value='"+mode+"'><input type='hidden' name='confirm' value='yes'><button style='"+String(erase?"background:#a92d43;color:#fff":"background:#f4c780;color:#0c111b")+"'>"+String(erase?"Confirm deletion and reset":"Confirm reboot")+"</button></form><a class='button secondary' href='/setup'>Cancel</a>";
        return respond(req,page(body));
    }
    if(path=="/setup") {
        auto snapshot=snapshots.get();
        if(!snapshot)return respond(req,"State unavailable.","503 Service Unavailable","text/plain");
        String setup=snapshot->setup;
        setup.replace(String("name='csrf' value='")+CsrfPlaceholder+"'",String("name='csrf' value='")+csrf+"'");
        return respond(req,setup);
    }
    if(path=="/certificate") {httpd_resp_set_hdr(req,"Content-Disposition","attachment; filename=nostr-pager.pem");return respond(req,certificate,"200 OK","application/x-pem-file");}
    if(path!="/messages" && path!="/message" && path!="/compose" && path!="/settings" && path!="/contacts" && path!="/api/state" && path!="/relays" && path!="/messages/delete/confirm")return respond(req,"Not found.","404 Not Found","text/plain");
    auto snapshot=snapshots.get();
    if(!snapshot)return respond(req,"State unavailable.","503 Service Unavailable","text/plain");
    if(!snapshot->ready && path!="/api/state")return respond(req,page(connectionPrompt()));
    String age=String(unsigned(millis()-snapshot->at));
    httpd_resp_set_hdr(req,"X-Pager-Snapshot-Age-Ms",age.c_str());
    if(path=="/api/state") {
        // Unchanged state is acknowledged with an empty 304 instead of resending the whole JSON.
        httpd_resp_set_hdr(req,"ETag",snapshot->etag.c_str());
        if(header(req,"If-None-Match")==snapshot->etag){securityHeaders(req);httpd_resp_set_status(req,"304 Not Modified");return httpd_resp_send(req,nullptr,0);}
    }
    DynamicJsonDocument state(98304);
    if(deserializeJson(state,snapshot->state))return respond(req,"Not enough memory.","503 Service Unavailable","text/plain");
    // Core 1 may be blocked in a handshake: do not serve expired cached DMs.
    JsonArray visible=state["messages"].as<JsonArray>();
    for(size_t i=visible.size();i>0;--i) {
        uint32_t expires=visible[i-1]["expiresAt"]|0U;
        if(messageExpiry::expired(expires,uint32_t(time(nullptr))))visible.remove(i-1);
    }
    if(path=="/api/state"){String json;serializeJson(state,json);return respond(req,json,"200 OK","application/json");}
    String body=connectionPrompt(true)+"<div id='connected-content'><nav><a href='/messages'>Inbox</a><a href='/compose'>New</a><a href='/contacts'>Contacts</a><a href='/relays'>Relays</a><a href='/settings'>Settings</a>"+postButton("/logout","Lock",csrf)+"</nav><div class='status' id='status'>"+escape(state["status"].as<String>())+"</div>";
    bool canSend=state["canSend"]|false;
    if(path=="/messages") {
        body+="<h1>Inbox <span id='unread' class='badge'></span></h1><div id='messages'></div><div class='message-nav' aria-label='Message navigation'><button id='previous' class='secondary' disabled>Previous</button><span id='position' class='muted' aria-live='polite'>0 / 0</span><button id='next' class='secondary' disabled>Next</button></div>"+postButton("/older","Fetch older messages",csrf)+"";
        body+="<a class='button danger' href='/messages/delete/confirm?all=yes'>Delete all messages</a>";
    } else if(path=="/messages/delete/confirm") {
        String id=query(req,"id");bool all=query(req,"all")=="yes";
        bool found=false;for(JsonObjectConst message:state["messages"].as<JsonArrayConst>())if(message["id"]==id)found=true;
        if((!all && !found) || (all && !id.isEmpty()))return respond(req,"Message not found or invalid deletion mode.","400 Bad Request","text/plain");
        body+="<h1>"+String(all?"Delete all messages?":"Delete this message?")+"</h1><p>Remove local history and stop retries for deleted pending messages? This cannot be undone. Already published messages remain on Nostr relays. Settings, keys and contacts stay saved.</p><form method='post' action='/messages/delete'>"+hidden(csrf)+"<input type='hidden' name='confirm' value='yes'><input type='hidden' name='all' value='"+String(all?"yes":"no")+"'><input type='hidden' name='id' value='"+escape(id)+"'><button class='danger'>Confirm deletion</button></form><a class='button secondary' href='/messages'>Cancel</a>";
    } else if(path=="/relays") {
        body+="<h1>Relays</h1><p>The primary inbox stays connected. One additional relay stays connected next to the primary (none while the setup network is open); more rotate in every 30 seconds. Failed endpoints use backoff; other relays keep working.</p>";
        body+=postButton("/relays/reconnect","Reconnect relays",csrf)+postButton("/relays/defaults","Reload default relays",csrf)+relayTable(state["relays"].as<JsonArrayConst>());
        body+="<p class='muted'>Reload to update. Public posts must mention your key. Relay acceptance does not confirm recipient delivery.</p><form method='post' action='/relays/save'>"+hidden(csrf)+"<label>Additional public relays</label><textarea name='publicRelays' rows='4' maxlength='1212'>"+escape(state["publicRelayList"].as<String>())+"</textarea><label>Backup DM inbox relays</label><textarea name='dmRelays' rows='3' maxlength='1212'>"+escape(state["dmRelayList"].as<String>())+"</textarea><small>One wss:// URL per line, up to six per list. Blank disables that list. Saving preserves Wi-Fi, keys, messages and browser sessions. Private-key mode publishes backup DM inboxes in your kind-10050 relay list.</small><button>Save relay lists and reconnect</button></form><h2>DM relay-list publication</h2>"+publicationTable(state["dmPublications"].as<JsonArrayConst>());
    } else if(path=="/contacts") {
        body+="<h1>Address book</h1><p class='status'>"+escape(state["contactStatus"].as<String>())+"</p><p>Save an npub once. Use its Nostr profile name or give it your own nickname. Up to 24 contacts are stored on the pager.</p>";
        String editKey=query(req,"edit"),newKey=query(req,"pubkey"),alias;
        for(JsonObjectConst contact:state["contacts"].as<JsonArrayConst>())if(contact["pubkey"]==editKey)alias=contact["alias"].as<String>();
        bool editing=!editKey.isEmpty();String keyInput=editing?editKey:newKey;
        body+="<details id='contact-editor' class='contact-editor'"+String(!keyInput.isEmpty()?" open":"")+"><summary>"+String(editing?"Edit nickname":"Add contact")+"</summary><form method='post' action='/contacts/save'>"+hidden(csrf)+"<label>Public key</label><input name='npub' maxlength='512' placeholder='npub / hex / nprofile' required"+String(editing?" readonly":"")+" value='"+escape(keyInput)+"'><label>Local nickname (optional)</label><input name='alias' maxlength='32' value='"+escape(alias)+"' placeholder='Leave blank to fetch the Nostr name'><button>Save contact</button></form></details>";
        body+=postButton("/contacts/refresh","Refresh Nostr names",csrf);
        body+="<div class='contact-table-wrap' tabindex='0' aria-label='Saved contacts'><table class='contact-table'><thead><tr><th scope='col'>Name</th><th scope='col'>Public key</th><th scope='col'>Actions</th></tr></thead><tbody>";
        for(JsonObjectConst contact:state["contacts"].as<JsonArrayConst>()) {
            String key=contact["pubkey"].as<String>(),npub=contact["npub"].as<String>();
            body+="<tr><td><strong>"+escape(contact["label"].as<String>())+"</strong>";
            if(contact["name"]!=contact["label"] && !contact["name"].as<String>().isEmpty())body+="<small>"+escape(contact["name"].as<String>())+"</small>";
            body+="</td><td><details class='contact-key'><summary title='"+escape(npub)+"'>"+escape(npub.substring(0,12))+"&hellip;"+escape(npub.substring(npub.length()-6))+"</summary><code>"+escape(npub)+"</code></details></td><td><div class='contact-actions'>";
            if(canSend)body+="<a class='mini' href='/compose?to="+escape(key)+"'>Message</a>";
            body+="<a class='mini' href='/contacts?edit="+escape(key)+"#contact-editor'>Edit</a>";
            body+="<form class='inline' method='post' action='/contacts/delete'>"+hidden(csrf)+"<input type='hidden' name='npub' value='"+escape(key)+"'><button class='remove'>Delete</button></form></div></td></tr>";
        }
        if(state["contacts"].as<JsonArrayConst>().size()==0)body+="<tr><td colspan='3' class='muted'>No contacts saved. Use Add contact above.</td></tr>";
        body+="</tbody></table></div>";
        body+="<p class='muted'>Reload to see fetched names. Names are labels; verify the public key when adding a contact.</p>";
    } else if(path=="/message" || path=="/compose") {
        String id=query(req,path=="/message"?"id":"reply");JsonObjectConst selected;
        if(!id.isEmpty())for(JsonObjectConst m:state["messages"].as<JsonArrayConst>())if(m["id"]==id){selected=m;break;}
        if(!id.isEmpty() && selected.isNull())return respond(req,page("<h1>Message no longer cached</h1><a href='/messages'>Back to inbox</a>"),"404 Not Found");
        if(path=="/message") {
            if(selected.isNull())return respond(req,"Message ID required.","400 Bad Request","text/plain");
            body+="<h1>Message</h1><section id='messageContent'><small>"+String((selected["outgoing"]|false)?"RECIPIENT":"SENDER")+"</small><p class='key'>"+escape(selected["peer"].as<String>())+"</p><span class='badge'>"+String((selected["kind"]|4)!=1?"Encrypted DM":"Public mention")+"</span><p class='message'>"+escape(selected["text"].as<String>())+"</p><small>"+escape(selected["delivery"].as<String>())+"</small></section>";
            if((selected["expiresAt"]|0U)>0)body+="<span id='messageExpiry' data-expires='"+String(selected["expiresAt"].as<unsigned>())+"'></span>";
            JsonArrayConst entries=state["messages"].as<JsonArrayConst>();size_t index=0;
            for(JsonObjectConst entry:entries){if(entry["id"]==id)break;++index;}
            body+="<div class='message-nav' aria-label='Message navigation'>";
            body+=index>0?"<a class='button secondary' href='/message?id="+escape(entries[index-1]["id"].as<String>())+"'>Previous</a>":"<span class='button secondary disabled' aria-disabled='true'>Previous</span>";
            body+="<span class='muted'>"+String(unsigned(index+1))+" / "+String(unsigned(entries.size()))+"</span>";
            body+=index+1<entries.size()?"<a class='button secondary' href='/message?id="+escape(entries[index+1]["id"].as<String>())+"'>Next</a>":"<span class='button secondary disabled' aria-disabled='true'>Next</span>";
            body+="</div><a class='button secondary' href='/messages?anchor="+escape(id)+"'>Back to inbox</a>";
            body+="<form method='post' action='/read'>"+hidden(csrf)+"<input type='hidden' name='id' value='"+escape(id)+"'><button>Mark read</button></form>";
            if(canSend)body+="<a class='button' href='/compose?reply="+escape(id)+"'>Reply</a>";
            body+="<a class='button secondary' href='/contacts?pubkey="+escape(selected["peer"].as<String>())+"'>Save contact</a>";
            body+="<a class='button danger' href='/messages/delete/confirm?id="+escape(id)+"'>Delete message</a>";
        } else if(!canSend)body+="<h1>Read-only identity</h1><p>Configure a private key to send messages.</p>";
        else {
            String recipient=selected.isNull()?query(req,"to"):selected["peer"].as<String>();
            body+="<h1>"+String(id.isEmpty()?"New message":"Reply")+"</h1><form method='post' action='/send'>"+hidden(csrf)+"<input type='hidden' name='reply' value='"+escape(id)+"'>";
            body+="<label>Saved contact</label><select id='contactChoice' name='contact'><option value=''>Type a name or public key below</option>";
            for(JsonObjectConst contact:state["contacts"].as<JsonArrayConst>()) {
                String key=contact["pubkey"].as<String>();
                body+="<option value='"+escape(key)+"'"+String(key==recipient?" selected":"")+">"+escape(contact["label"].as<String>())+" · "+escape(key.substring(0,8))+"</option>";
            }
            body+="</select><label>Recipient name / public key</label><input id='recipient' name='recipient' maxlength='512' placeholder='Saved name / npub / hex / nprofile' value='"+escape(recipient)+"'><label>Message</label><textarea id='text' name='text' maxlength='512' rows='6' placeholder=\"What's your signal?\" required></textarea><small id='count'>0 / 512 bytes</small><label>Privacy</label><select name='mode'><option value='dm'"+String((selected["kind"]|4)!=1?" selected":"")+">Encrypted DM</option><option value='public'"+String((selected["kind"]|4)==1?" selected":"")+">Public mention (visible to everyone)</option></select><button>Send message</button></form>";
        }
    } else {
        body+="<h1>Device settings</h1><p class='status'>"+escape(state["dmStatus"].as<String>())+"</p><section><small>HTTPS ADDRESS</small><p class='key'>"+escape(state["address"].as<String>())+"</p><small>CERTIFICATE SHA-256</small><p class='key'>"+escape(state["fingerprint"].as<String>())+"</p><a href='/certificate'>Download certificate</a></section><p>Generating a certificate creates a new TLS key, restarts the pager and invalidates browser sessions. Compare its fingerprint with the display before accepting it.</p>"+postButton("/certificate/renew","Generate new certificate",csrf)+"<p><a class='button secondary' href='/setup'>Wi-Fi and Nostr setup</a></p>";
    }
    if(path=="/settings")body+="<h2>Relay connections</h2>"+postButton("/relays/reconnect","Reconnect relays",csrf)+relayTable(state["relays"].as<JsonArrayConst>())+"<p><a href='/relays'>Manage relay lists</a></p><h2>DM relay-list publication</h2>"+publicationTable(state["dmPublications"].as<JsonArrayConst>())+"<p class='muted'>Verified means the relay returned the signed inbox list after publication.</p>";
    if(path=="/settings")body+="<p class='muted'>IP fallback: <a href='"+escape(state["ipAddress"].as<String>())+"'>"+escape(state["ipAddress"].as<String>())+"</a></p>";
    if(path=="/settings") {
        body+="<section><small>MESSAGING CONNECTION</small><p class='message'>"+escape(state["messaging"].as<String>())+"</p><small>Reload to update. Public notes must tag this key; senders must publish DMs to this inbox relay.</small></section>";
        auto memory=tlsMemory::stats();
        body+="<section><small>TLS MEMORY / PSRAM PREFERRED</small><p>Internal free: "+String(unsigned(memory.internalFree))+" bytes; largest block: "+String(unsigned(memory.internalLargest))+" bytes.</p><p>PSRAM free: "+String(unsigned(memory.psramFree))+" bytes; largest block: "+String(unsigned(memory.psramLargest))+" bytes.</p><p>Internal fallbacks: "+String(memory.fallbacks)+"; failed allocations: "+String(memory.failures)+".</p></section>";
    }
    return respond(req,page(body+"</div>"));
}
void refreshSnapshot(bool force) {
    if(!force && snapshotAt && millis()-snapshotAt<1000)return;
    // Never stall the web worker behind the relay: keep serving the previous snapshot on timeout.
    pagerSync::Lock guard(force?250:20);
    if(!guard)return;
    bool wifi=pager::wifiConnected(),configured=pager::configured();
    bool ready=wifi && configured;
    DynamicJsonDocument doc(ready?98304:1024);
    doc["wifiConnected"]=wifi;doc["configured"]=configured;
    if(ready) {
    doc["version"]=PAGER_VERSION;doc["status"]=pager::status;doc["dmStatus"]=dmRelay::status();doc["dmPublication"]=dmRelay::publicationStatus();doc["canSend"]=pager::canSend;doc["address"]=pager::webAddress;doc["fingerprint"]=pager::webFingerprint;
    doc["ipAddress"]=pager::webIpAddress;
    doc["messaging"]=pager::messagingDiagnostics();
    pager::relayRows(doc.createNestedArray("relays"));dmRelay::publicationRows(doc.createNestedArray("dmPublications"));
    doc["publicRelayList"]=pager::publicRelayList();doc["dmRelayList"]=pager::dmRelayList();
    doc["contactStatus"]=contacts::status();JsonArray book=doc.createNestedArray("contacts");
    for(const auto &contact:contacts::all()) {
        JsonObject row=book.createNestedObject();row["pubkey"]=contact.pubkey;row["npub"]=keys::npub(contact.pubkey.c_str()).c_str();
        row["label"]=contacts::label(contact);row["alias"]=contact.alias;row["name"]=contact.name;
    }
    JsonArray array=doc.createNestedArray("messages");
    for(const auto &m:pager::messages) {
        JsonObject row=array.createNestedObject();row["id"]=m.id;row["peer"]=m.peer;row["text"]=m.text;
        row["timestamp"]=m.timestamp;row["kind"]=m.kind;row["outgoing"]=m.outgoing;row["read"]=m.read;row["delivery"]=m.delivery;
        row["expiresAt"]=m.expiresAt;
        row["peerName"]=contacts::display(m.peer);
    }
    } else {
        doc["version"]=PAGER_VERSION;doc["canSend"]=false;
        doc["status"]=wifi?"Configuration needed":"Wi-Fi not connected";
        doc.createNestedArray("messages");
    }
    if(doc.overflowed())return;
    auto next=std::make_shared<Snapshot>();next->ready=ready;
    serializeJson(doc,next->state);
    if(next->state.isEmpty())return;
    next->setup=pager::configurationPage(CsrfPlaceholder);
    uint32_t hash=2166136261u;for(size_t i=0;i<next->state.length();++i){hash^=uint8_t(next->state[i]);hash*=16777619u;}
    char tag[16];snprintf(tag,sizeof(tag),"\"%08x\"",unsigned(hash));next->etag=tag;
    next->at=millis();
    snapshots.publish(std::move(next));snapshotAt=millis();
}

}
namespace webPager {
void begin() {
    work=xQueueCreate(4,sizeof(Job*));tlsStore.begin("pager-tls",false);
    uint32_t random;
    do {random=esp_random();}while(random>=4294000000UL);
    char digits[7];snprintf(digits,sizeof(digits),"%06u",unsigned(random%1000000));pin=digits;pager::webPin=pin;
}
void ensureStarted() {
    if(https || !work || (retryAt && int32_t(millis()-retryAt)<0))return;
    if(WiFi.status()!=WL_CONNECTED && !(WiFi.getMode()&WIFI_AP))return;
    retryAt=millis()+30000;
    DynamicJsonDocument doc(4096);
    if(!deserializeJson(doc,tlsStore.getString("identity","{}"))) {certificate=doc["cert"]|"";privateKey=doc["key"]|"";}
    if(doc["hostname"]!="pager.local" || tlsStore.getBool("renew",false) || certificate.isEmpty() || privateKey.isEmpty() || !fingerprint()) {
        {pagerSync::Lock guard;pager::status="Generating HTTPS certificate...";}ui::changed();
        if(!generateCertificate() || !fingerprint()){pagerSync::Lock guard;pager::status="HTTPS certificate generation failed";return;}
    }
    httpd_ssl_config_t config=HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.core_id=0;
    // Three client sockets (plus httpd's 3 internal ones = 6 of lwIP's ~10) so a browser can load a page and
    // app.js in parallel; idle keep-alive connections are purged.
    config.httpd.stack_size=16384;config.httpd.max_open_sockets=3;config.httpd.lru_purge_enable=true;config.httpd.recv_wait_timeout=5;config.httpd.send_wait_timeout=5;config.httpd.max_uri_handlers=2;
    config.httpd.uri_match_fn=httpd_uri_match_wildcard;
    config.cacert_pem=(const uint8_t*)certificate.c_str();config.cacert_len=certificate.length()+1;
    config.prvtkey_pem=(const uint8_t*)privateKey.c_str();config.prvtkey_len=privateKey.length()+1;
    refreshSnapshot(true);
    if(httpd_ssl_start(&https,&config)!=ESP_OK){https=nullptr;pagerSync::Lock guard;pager::status="HTTPS server failed / retrying";return;}
    httpd_uri_t get{};get.uri="/*";get.method=HTTP_GET;get.handler=handler;
    httpd_uri_t post=get;post.method=HTTP_POST;
    if(httpd_register_uri_handler(https,&get)!=ESP_OK || httpd_register_uri_handler(https,&post)!=ESP_OK) {
        httpd_ssl_stop(https);https=nullptr;pagerSync::Lock guard;pager::status="HTTPS routing failed";return;
    }
    updateMdns();updateAddresses();
    Serial.printf("HTTPS task pinned to core 0; pager/relay worker core %d\n",xPortGetCoreID());
}
void loop() {
    ensureStarted();
    updateMdns();updateAddresses();
    refreshSnapshot(false);
    Job *job=nullptr;
    if(!work || xQueueReceive(work,&job,0)!=pdTRUE)return;
    auto hold=job->shared_from_this();
    DynamicJsonDocument input(8192);
    if(deserializeJson(input,job->request.input)) {
        job->ok=false;job->result="Could not decode the queued action. Nothing was changed.";
        job->request.input="";job->finish(millis());return;
    }
    if(job->request.action==Action::Save) {
        pager::Configuration config;config.ssid=input["ssid"]|"";config.password=input["password"]|"";
        config.key=input["key"]|"";config.relay=input["relay"]|"";config.open=input.containsKey("open");config.rawPrivate=input["keytype"]=="private";
        config.publicRelays=input["publicRelays"]|"";config.dmRelays=input["dmRelays"]|"";
        job->ok=pager::applyConfiguration(config,job->result);
        if(job->ok)job->result="Configuration saved. Reconnect using the address on the display.";
    } else if(job->request.action==Action::RelaySave)job->ok=pager::saveRelayLists(input["publicRelays"]|"",input["dmRelays"]|"",job->result);
    else if(job->request.action==Action::RelayDefaults){job->ok=pager::restoreDefaultRelays(job->result);if(job->ok)job->result="Default relays restored.";}
    else if(job->request.action==Action::Reconnect)pager::reconnectRelays();
    else if(job->request.action==Action::Delete) {
        String mode=input["all"]|"",id=input["id"]|"";
        if(input["confirm"]!="yes" || (mode!="yes" && mode!="no") || (mode=="yes" && !id.isEmpty()) || (mode=="no" && id.isEmpty())){job->ok=false;job->result="Confirm the message deletion first.";}
        else job->ok=pager::deleteMessages(id,mode=="yes",job->result);
    } else if(job->request.action==Action::Send) {
        String mode=input["mode"]|"";
        if(mode!="dm" && mode!="public"){job->ok=false;job->result="Select a privacy mode.";}
        else {
            String recipient=input["contact"]|"";if(recipient.isEmpty())recipient=input["recipient"]|"";
            job->ok=pager::send(recipient,input["text"]|"",mode=="public",input["reply"]|"",job->result);
        }
    } else if(job->request.action==Action::ContactSave){pagerSync::Lock guard;job->ok=contacts::save(input["npub"]|"",input["alias"]|"",job->result);}
    else if(job->request.action==Action::ContactDelete){pagerSync::Lock guard;job->ok=contacts::remove(input["npub"]|"",job->result);}
    else if(job->request.action==Action::ContactRefresh){pagerSync::Lock guard;contacts::refresh();}
    else if(job->request.action==Action::Reset) {
        auto mode=resetControl::confirmed(input["mode"]|"",input["confirm"]|"");
        if(mode==resetControl::Mode::Invalid){job->ok=false;job->result="Reset requires explicit confirmation and a valid mode.";}
        else {
            job->ok=pager::reset(mode==resetControl::Mode::Erase,job->result);
            if(job->ok)job->result=mode==resetControl::Mode::Erase?"Saved data will be deleted before setup starts.":"Restart requested. Saved data is preserved.";
        }
    }
    else if(job->request.action==Action::Read) {pager::markRead(input["id"]|"");ui::changed();}
    else if(job->request.action==Action::Older)pager::older();
    else if(job->request.action==Action::Renew) {
        job->ok=tlsStore.putBool("renew",true)==1;
        job->result=job->ok?"A fresh certificate will be generated after restart.":"Could not schedule certificate generation.";
        if(job->ok)pager::restartSoon("Restarting / new HTTPS certificate");
    }
    job->request.input="";
    refreshSnapshot(true);
    job->finish(millis());
}
}
