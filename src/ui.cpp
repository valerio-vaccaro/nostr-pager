#include "pager.h"
#include "board.h"
#include "version.h"
#include "contacts.h"
#include "key_codec.h"
#include "pager_sync.h"
#include <lvgl.h>
#include <cstring>
#include <algorithm>

namespace {
enum class View { Inbox, Detail, Compose, Portal, Web, Contacts, ContactEdit, ResetConfirm, DeleteConfirm };
View view = View::Inbox;
int pending = -1;
bool dirty = true;
String selected, recipient, replyId, portalSsid, portalPassword, portalMessage, shownStatus;
String draft,contactKey,contactAlias;
String inboxAnchor;
size_t inboxPage=0;
Message drawnMessage;
String drawnName;
size_t drawnIndex=0, drawnCount=0;
unsigned drawnUnread=0;
bool drawnCanSend=false;
bool drawnReady=true;
bool contactReturnCompose=false;
bool eraseReset=false;
bool deleteAll=false;
bool publicReply = false;
lv_obj_t *statusLabel = nullptr, *statusDot = nullptr, *notification = nullptr, *list = nullptr;
lv_obj_t *detailBody = nullptr;
lv_obj_t *recipientArea = nullptr, *bodyArea = nullptr, *keyboard = nullptr;
lv_obj_t *modeSwitch = nullptr, *modeLabel = nullptr, *errorLabel = nullptr, *counter = nullptr;
uint32_t alertUntil = 0;
volatile bool notifyRequested = false;
constexpr uint32_t Background = 0x0c111b, Surface = 0x151e2d, Border = 0x27344a;
constexpr uint32_t Foreground = 0xeaf0fa, Muted = 0x92a2bb, Mint = 0x68e0c4;
constexpr uint32_t Violet = 0xb7a1ff, Amber = 0xf4c780, Coral = 0xff9caa;
lv_color_t color(uint32_t value) { return lv_color_hex(value); }
void request(View target) { pending = int(target); }

lv_obj_t *label(lv_obj_t *parent, const String &text, int x, int y, int width,
                const lv_font_t *font = &lv_font_montserrat_14, uint32_t ink = Foreground) {
    auto *object = lv_label_create(parent);
    lv_label_set_text(object, text.c_str());
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color(ink), 0);
    return object;
}
void singleLine(lv_obj_t *object) {
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_obj_set_height(object, lv_obj_get_style_text_font(object, 0)->line_height);
}
lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t fill = Surface) {
    auto *object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color(fill), 0);
    lv_obj_set_style_border_color(object, color(Border), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}
lv_obj_t *pill(lv_obj_t *parent, const String &text, int x, int y, int width,
               uint32_t ink = Mint, uint32_t fill = 0x19372f) {
    auto *object = panel(parent, x, y, width, 22, fill);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 8, 0);
    auto *title = label(object, text, 0, 0, width, &lv_font_montserrat_10, ink);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(title);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}
lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int width,
                 lv_event_cb_t callback, bool primary = false, void *data = nullptr, int height = 44) {
    auto *object = lv_btn_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, color(primary ? Mint : Surface), 0);
    lv_obj_set_style_bg_color(object, color(primary ? 0x45bda3 : 0x26364a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(object, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(object, color(Border), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, data);
    auto *textLabel = label(object, text, 0, 0, width - 4, &lv_font_montserrat_14,
                            primary ? Background : Foreground);
    lv_obj_set_style_text_align(textLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(textLabel);
    return object;
}
// Messages the display inbox lists. With pager::inboxReceivedOnly (default true) sent copies are hidden.
std::vector<const Message *> shownList;
volatile bool showFirstRequested = false;
void refreshShown() {
    shownList.clear();
    for (const auto &message : pager::messages) if (!pager::inboxReceivedOnly || !message.outgoing) shownList.push_back(&message);
}
Message *findSelected() {
    for (auto &message : pager::messages) if (message.id == selected) return &message;
    return nullptr;
}
size_t messageIndex(const String &id) {
    for(size_t i=0;i<shownList.size();++i)if(shownList[i]->id==id)return i;
    return shownList.size();
}
// Skip screen rebuilding when a change does not affect visible data.
// Status is updated separately.
bool messageViewChanged(bool remember=false) {
    if(view!=View::Inbox && view!=View::Detail)return true;
    size_t index=messageIndex(view==View::Inbox?inboxAnchor:selected);
    const Message empty;
    const auto &m=index<shownList.size()?(*shownList[index]):empty;
    String name=m.id.isEmpty()?String(""):contacts::display(m.peer);
    unsigned unread=0;
    if(view==View::Inbox)for(const auto *entry:shownList)if(!entry->read && !entry->outgoing)++unread;
    bool changed=drawnIndex!=index || drawnCount!=shownList.size() || drawnUnread!=unread ||
        drawnCanSend!=pager::canSend || drawnName!=name || drawnMessage.id!=m.id ||
        drawnMessage.peer!=m.peer || drawnMessage.text!=m.text || drawnMessage.kind!=m.kind ||
        drawnMessage.timestamp!=m.timestamp || drawnMessage.outgoing!=m.outgoing ||
        drawnMessage.read!=m.read || drawnMessage.delivery!=m.delivery;
    if(remember) {
        // Keep only display data, never duplicate encrypted event payloads.
        drawnMessage.id=m.id;drawnMessage.peer=m.peer;drawnMessage.text=m.text;
        drawnMessage.kind=m.kind;drawnMessage.timestamp=m.timestamp;drawnMessage.outgoing=m.outgoing;
        drawnMessage.read=m.read;drawnMessage.delivery=m.delivery;
        drawnName=name;drawnIndex=index;drawnCount=shownList.size();drawnUnread=unread;drawnCanSend=pager::canSend;
    }
    return changed;
}
void navigation(lv_obj_t *screen,size_t index,int y,bool detail) {
    auto *previous=button(screen,LV_SYMBOL_LEFT " Prev",12,y,94,[](lv_event_t *event){
        bool detail=lv_event_get_user_data(event)!=nullptr;
        size_t index=detail?messageIndex(selected):messageIndex(inboxAnchor);
        if(index==0 || index>=shownList.size())return;
        --index;inboxPage=index;inboxAnchor=(*shownList[index]).id;
        if(detail){selected=inboxAnchor;pager::markRead(selected);}
        request(detail?View::Detail:View::Inbox);
    },false,detail?reinterpret_cast<void *>(1):nullptr,36);
    auto *next=button(screen,"Next " LV_SYMBOL_RIGHT,214,y,94,[](lv_event_t *event){
        bool detail=lv_event_get_user_data(event)!=nullptr;
        size_t index=detail?messageIndex(selected):messageIndex(inboxAnchor);
        if(index>=shownList.size() || index+1>=shownList.size())return;
        ++index;inboxPage=index;inboxAnchor=(*shownList[index]).id;
        if(detail){selected=inboxAnchor;pager::markRead(selected);}
        request(detail?View::Detail:View::Inbox);
    },false,detail?reinterpret_cast<void *>(1):nullptr,36);
    if(index==0 || index>=shownList.size())lv_obj_add_state(previous,LV_STATE_DISABLED);
    if(index>=shownList.size() || index+1>=shownList.size())lv_obj_add_state(next,LV_STATE_DISABLED);
    auto *position=label(screen,String(unsigned(index<shownList.size()?index+1:0))+" / "+String(unsigned(shownList.size())),110,y+11,100,&lv_font_montserrat_12,Muted);
    lv_obj_set_style_text_align(position,LV_TEXT_ALIGN_CENTER,0);
}
lv_obj_t *dangerButton(lv_obj_t *parent,const char *text,int x,int y,int width,lv_event_cb_t callback,int height=38) {
    auto *object=button(parent,text,x,y,width,callback,false,nullptr,height);
    lv_obj_set_style_bg_color(object,color(0xa92d43),0);
    lv_obj_set_style_bg_color(object,color(0xc63e56),LV_STATE_PRESSED);
    lv_obj_set_style_border_width(object,0,0);return object;
}
lv_obj_t *warnButton(lv_obj_t *parent,const char *text,int x,int y,int width,lv_event_cb_t callback,int height=38) {
    auto *object=button(parent,text,x,y,width,callback,false,nullptr,height);
    lv_obj_set_style_bg_color(object,color(Amber),0);
    lv_obj_set_style_bg_color(object,color(0xdcae62),LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(object,0),color(Background),0);
    lv_obj_set_style_border_width(object,0,0);return object;
}
String date(uint32_t timestamp) {
    time_t time = timestamp;
    struct tm out;
    gmtime_r(&time, &out);
    char buffer[24];
    strftime(buffer, sizeof(buffer), "%d %b  %H:%M UTC", &out);
    return buffer;
}
void compose(bool reply) {
    if (!pager::canSend) return;
    if (reply) {
        auto *message = findSelected();
        if (!message) return;
        recipient = message->peer;
        replyId = message->id;
        publicReply = message->kind == 1;
    } else {
        recipient = "";
        replyId = "";
        publicReply = false;
    }
    draft="";
    request(View::Compose);
}
void updateStatus() {
    String status=drawnReady?pager::status:pager::wifiConnected()?String("Configuration needed"):String("Wi-Fi not connected");
    if (!statusLabel || shownStatus == status) return;
    shownStatus = status;
    lv_label_set_text(statusLabel, shownStatus.c_str());
    uint32_t ink = Amber;
    if (shownStatus.indexOf("Online") >= 0 || shownStatus.indexOf("Accepted") >= 0 ||
        shownStatus.indexOf("Relay connected") >= 0 || shownStatus.indexOf("relay list published") >= 0) ink = Mint;
    if (shownStatus.indexOf("failed") >= 0 || shownStatus.indexOf("rejected") >= 0 ||
        shownStatus.indexOf("Invalid") >= 0 || shownStatus.indexOf("unavailable") >= 0) ink = Coral;
    lv_obj_set_style_bg_color(statusDot, color(ink), 0);
}
void header(lv_obj_t *screen) {
    label(screen, "Nostr Pager", 12, 12, 224, &lv_font_montserrat_20);
    auto *version = label(screen, PAGER_VERSION, 242, 18, 66, &lv_font_montserrat_10, Muted);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_RIGHT, 0);singleLine(version);
    statusDot = panel(screen, 12, 48, 7, 7, Amber);
    lv_obj_set_style_border_width(statusDot, 0, 0);
    lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
    statusLabel = label(screen, pager::status, 26, 44, 282, &lv_font_montserrat_12, Muted);
    singleLine(statusLabel);
    shownStatus = "";
    updateStatus();
    notification = panel(screen, 0, 0, 320, 3, Mint);
    lv_obj_set_style_border_width(notification, 0, 0);
    lv_obj_set_style_radius(notification, 0, 0);
    lv_obj_set_style_bg_opa(notification, alertUntil ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}
void inboxView(lv_obj_t *screen) {
    size_t anchored=messageIndex(inboxAnchor);
    if(anchored<shownList.size())inboxPage=anchored;
    else inboxPage=shownList.empty()?0:std::min(inboxPage,shownList.size()-1);
    if(!shownList.empty())inboxAnchor=(*shownList[inboxPage]).id;
    unsigned unread = 0;
    for (const auto *entry : shownList) if (!entry->read && !entry->outgoing) ++unread;
    label(screen, "Inbox", 12, 80, 170, &lv_font_montserrat_24);
    button(screen,"Contacts",196,78,112,[](lv_event_t *){contactReturnCompose=false;request(View::Contacts);},false,nullptr,36);
    label(screen, String(unread) + " unread  /  " +
          (pager::canSend ? "DMs + mentions" : "Read only"), 12, 118, 190, &lv_font_montserrat_12, Muted);
    button(screen, pager::inboxReceivedOnly ? LV_SYMBOL_DOWNLOAD " Received" : LV_SYMBOL_LIST " All", 208, 112, 100,
           [](lv_event_t *) {
               pager::inboxReceivedOnly = !pager::inboxReceivedOnly;
               inboxAnchor = ""; inboxPage = 0;
               request(View::Inbox);
           }, false, nullptr, 26);
    list = panel(screen, 12, 144, 296, 196, Background);
    lv_obj_set_style_border_width(list, 0, 0);
    if (shownList.empty()) {
        auto *title = label(list, "No messages yet", 12, 56, 272, &lv_font_montserrat_20);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        auto *hint = label(list, "Messages appear here when connected.\nOpen Setup to configure your pager.",
                           20, 94, 256, &lv_font_montserrat_12, Muted);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }
    if(inboxPage<shownList.size()) {
        const auto &message = (*shownList[inboxPage]);
        bool fresh = !message.read && !message.outgoing;
        auto *card = lv_btn_create(list);
        lv_obj_set_size(card, 296, 192);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_bg_color(card, color(fresh ? 0x142c2b : Surface), 0);
        lv_obj_set_style_bg_color(card, color(0x26364a), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, color(fresh ? 0x315b50 : Border), 0);
        lv_obj_add_event_cb(card, [](lv_event_t *) {
            size_t index = messageIndex(inboxAnchor);
            if (index >= shownList.size()) return;
            selected = (*shownList[index]).id;
            inboxAnchor=selected;inboxPage=index;
            pager::markRead(selected);
            request(View::Detail);
        }, LV_EVENT_CLICKED, nullptr);
        singleLine(label(card, (message.outgoing ? "To " : "From ") + contacts::display(message.peer),
                         0, 0, 212, &lv_font_montserrat_14));
        pill(card, message.kind != 1 ? "DM" : "NOTE", 222, 0, 48,
             message.kind != 1 ? Mint : Violet, message.kind != 1 ? 0x19372f : 0x29223f);
        auto *preview = label(card, message.text, 0, 38, 270, &lv_font_montserrat_20);
        lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);
        lv_obj_set_height(preview, 96);
        label(card, date(message.timestamp), 0, 148, 174, &lv_font_montserrat_10, Muted);
        if (message.outgoing) {
            uint32_t ink = message.delivery == "accepted" ? Mint : message.delivery == "rejected" ? Coral : Amber;
            auto *delivery = label(card, message.delivery, 180, 148, 90, &lv_font_montserrat_10, ink);
            lv_obj_set_style_text_align(delivery, LV_TEXT_ALIGN_RIGHT, 0);
            singleLine(delivery);
        }
    }
    if(!shownList.empty()) {
        dangerButton(screen,"Delete all",214,388,94,[](lv_event_t *){deleteAll=true;request(View::DeleteConfirm);},24);
        navigation(screen,inboxPage,348,false);
    }
    button(screen, "Web access", 12, 388, 140, [](lv_event_t *) { request(View::Web); }, false, nullptr, 28);
    button(screen, LV_SYMBOL_LIST " Older", 12, 424, 86, [](lv_event_t *) { pager::older(); });
    auto *create = button(screen, LV_SYMBOL_PLUS " New", 106, 424, 100,
                          [](lv_event_t *) { compose(false); }, true);
    if (!pager::canSend) lv_obj_add_state(create, LV_STATE_DISABLED);
    button(screen, LV_SYMBOL_SETTINGS " Setup", 214, 424, 94, [](lv_event_t *) { pager::configure(); });
}
void detailView(lv_obj_t *screen) {
    auto *message = findSelected();
    if (message) {
        auto *peer = panel(screen, 12, 84, 296, 86);
        singleLine(label(peer, contacts::display(message->peer), 14, 13, 264, &lv_font_montserrat_14, Mint));
        auto *key = label(peer, message->peer, 14, 35, 268, &lv_font_montserrat_10);
        lv_label_set_long_mode(key, LV_LABEL_LONG_WRAP);
        button(peer,"Save contact",180,58,102,[](lv_event_t *){
            auto *message=findSelected();if(!message)return;
            contactKey=message->peer;contactAlias="";contactReturnCompose=false;
            for(const auto &c:contacts::all())if(c.pubkey==contactKey)contactAlias=c.alias;
            request(View::ContactEdit);
        },false,nullptr,24);
        dangerButton(peer,"Delete",14,58,88,[](lv_event_t *){deleteAll=false;request(View::DeleteConfirm);},24);
        pill(screen, message->kind != 1 ? LV_SYMBOL_ENVELOPE " DM" : LV_SYMBOL_EYE_OPEN " PUBLIC",
             12, 180, message->kind != 1 ? 64 : 88, message->kind != 1 ? Mint : Violet,
             message->kind != 1 ? 0x19372f : 0x29223f);
        auto *time = label(screen, date(message->timestamp), 108, 186, 200, &lv_font_montserrat_12, Muted);
        lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_RIGHT, 0);
        auto *body = panel(screen, 12, 212, 296, 152);
        detailBody = body;
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        lv_obj_set_style_pad_all(body, 14, 0);
        auto *text = label(body, message->text, 0, 0, 266, &lv_font_montserrat_14);
        lv_obj_set_style_text_line_space(text, 5, 0);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        navigation(screen,messageIndex(selected),376,true);
    }
    else {label(screen,"This message is no longer in the cache.",12,140,296);navigation(screen,shownList.size(),376,true);}
    button(screen, LV_SYMBOL_LEFT " Inbox", 12, 424, 140, [](lv_event_t *) { request(View::Inbox); });
    auto *reply = button(screen, LV_SYMBOL_EDIT " Reply", 160, 424, 148, [](lv_event_t *) { compose(true); }, true);
    if (!pager::canSend || !message) lv_obj_add_state(reply, LV_STATE_DISABLED);
}
void portalView(lv_obj_t *screen) {
    bool joined = pager::wifiConnected();
    label(screen, drawnReady?"Setup":"Connect your pager", 12, 109, 296, &lv_font_montserrat_24);
    singleLine(label(screen, drawnReady?portalMessage:String("Choose Wi-Fi and your Nostr key in the browser."), 12, 143, 296, &lv_font_montserrat_12, Muted));
    if(!joined) {
        auto *network = panel(screen, 12, 165, 296, 108);
        pill(network, LV_SYMBOL_WIFI "  01", 12, 10, 54);
        label(network, "JOIN THIS WI-FI", 78, 15, 202, &lv_font_montserrat_12, Muted);
        singleLine(label(network, portalSsid, 14, 40, 266, &lv_font_montserrat_14));
        label(network, "WI-FI PASSWORD", 14, 64, 266, &lv_font_montserrat_12, Muted);
        label(network, portalPassword, 14, 80, 266, &lv_font_montserrat_20, Mint);
    }
    int top = joined ? 169 : 281, height = joined ? 195 : 90;
    auto *browser = panel(screen, 12, top, 296, height);
    if(!joined)pill(browser, "02", 12, 8, 34, Violet, 0x29223f);
    label(browser, "OPEN IN YOUR BROWSER", joined ? 14 : 58, joined ? 14 : 12, 222, &lv_font_montserrat_12, Muted);
    label(browser, pager::webAddress.isEmpty() ? "HTTPS starting..." : pager::webAddress,
          14, joined ? 40 : 30, 268, joined ? &lv_font_montserrat_20 : &lv_font_montserrat_14, Mint);
    label(browser, pager::webIpAddress, 14, joined ? 72 : 48, 268, joined ? &lv_font_montserrat_14 : &lv_font_montserrat_12, Muted);
    if(joined)label(browser, "SESSION PIN", 14, 112, 268, &lv_font_montserrat_12, Muted);
    label(browser, joined ? pager::webPin : "PIN " + pager::webPin, 14, joined ? 130 : 64, 180, joined ? &lv_font_montserrat_24 : &lv_font_montserrat_20);
    auto *details = label(browser, "Web details " LV_SYMBOL_RIGHT, joined ? 170 : 188, joined ? 168 : 70, 110, &lv_font_montserrat_12, Violet);
    lv_obj_add_flag(browser, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(browser, [](lv_event_t *) { request(View::Web); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_clear_flag(details, LV_OBJ_FLAG_CLICKABLE);
    if(drawnReady) {
        warnButton(screen,"Reboot",12,376,94,[](lv_event_t *){eraseReset=false;request(View::ResetConfirm);});
        dangerButton(screen,"Delete data + reset",114,376,194,[](lv_event_t *){eraseReset=true;request(View::ResetConfirm);});
    } else label(screen,"Save the settings to connect.\nSaved Wi-Fi reconnects automatically.",12,381,296,&lv_font_montserrat_12,Muted);
    auto *back = button(screen, LV_SYMBOL_LEFT " Back to inbox", 12, 424, 296,
                        [](lv_event_t *) { pager::resume(); }, true);
    if (!drawnReady) lv_obj_add_state(back, LV_STATE_DISABLED);
}
void connectionView(lv_obj_t *screen) {
    label(screen,pager::wifiConnected()?"Finish setup":"Connect to Wi-Fi",12,116,296,&lv_font_montserrat_24);
    label(screen,"Connect and configure your pager\nto use messages and contacts.",12,170,296,&lv_font_montserrat_14);
    label(screen,pager::configured()?"Trying your saved Wi-Fi.\nOpen Setup to change the connection.":
          "Open Setup, then use your phone\nto choose Wi-Fi and a Nostr identity.",12,244,296,&lv_font_montserrat_14,Muted);
    button(screen,"Open Setup",12,424,296,[](lv_event_t *){pager::configure();},true);
}
void webView(lv_obj_t *screen) {
    label(screen, "Web access", 12, 109, 296, &lv_font_montserrat_24);
    auto *connection = panel(screen, 12, 150, 296, 112);
    label(connection, "OPEN ON THE SAME WI-FI", 14, 14, 268, &lv_font_montserrat_10, Muted);
    label(connection, pager::webAddress.isEmpty() ? "Waiting for Wi-Fi..." : pager::webAddress,
          14, 35, 268, &lv_font_montserrat_20, Mint);
    label(connection, pager::webIpAddress, 14, 64, 268, &lv_font_montserrat_14, Muted);
    label(connection, "Session PIN is shown only in Setup.", 14, 85, 268, &lv_font_montserrat_10, Muted);
    label(screen, "Accept the self-signed certificate after checking its fingerprint below. Open Setup on the display to read the login PIN.",
          12, 279, 296, &lv_font_montserrat_12, Muted);
    label(screen, "Firmware " PAGER_VERSION, 12, 327, 296, &lv_font_montserrat_10, Muted);
    label(screen, "CERTIFICATE SHA-256", 12, 359, 296, &lv_font_montserrat_10, Muted);
    label(screen, pager::webFingerprint.isEmpty() ? "Certificate not ready" : pager::webFingerprint,
          12, 376, 296, &lv_font_montserrat_12, Violet);
    button(screen,drawnReady?"Reconnect":"Open Setup",12,424,140,[](lv_event_t *){
        if(drawnReady)pager::reconnectRelays();else pager::configure();
    });
    button(screen, LV_SYMBOL_LEFT " Back", 160, 424, 148,
           [](lv_event_t *) { request(portalSsid.isEmpty() ? View::Inbox : View::Portal); }, true);
}
void deleteConfirmView(lv_obj_t *screen) {
    label(screen,deleteAll?"Delete all messages?":"Delete this message?",12,108,296,&lv_font_montserrat_24);
    auto *warning=panel(screen,12,154,296,232,0x291c29);
    label(warning,"Remove local message history?\n\nThis cannot be undone. Deleted pending messages stop retrying. Already published messages remain on Nostr relays.\n\nSettings, your identity and contacts stay saved.",14,18,268,&lv_font_montserrat_14);
    errorLabel=label(screen,"",12,392,296,&lv_font_montserrat_10,Coral);singleLine(errorLabel);
    button(screen,"Cancel",12,424,140,[](lv_event_t *){request(deleteAll?View::Inbox:View::Detail);});
    dangerButton(screen,"Confirm deletion",160,424,148,[](lv_event_t *){
        String error;if(pager::deleteMessages(selected,deleteAll,error)){selected="";request(View::Inbox);}
        else lv_label_set_text(errorLabel,error.c_str());
    },44);
}
void resetConfirmView(lv_obj_t *screen) {
    label(screen,eraseReset?"Delete data + reset?":"Reboot the pager?",12,108,296,&lv_font_montserrat_24);
    auto *warning=panel(screen,12,154,296,240,0x291c29);
    label(warning,eraseReset?"This deletes Wi-Fi credentials, your Nostr identity, all messages, contacts and the HTTPS certificate.\n\nKeep a separate backup of your private key. Deletion cannot be undone.\n\nThe pager restarts in first-time setup.":
        "Restart the board now?\n\nYour settings, messages, contacts and certificate will stay saved.\n\nBrowser sessions end and the setup PIN changes.",
        14,18,268,&lv_font_montserrat_14);
    errorLabel=label(screen,"",12,399,296,&lv_font_montserrat_10,Coral);singleLine(errorLabel);
    button(screen,"Cancel",12,424,140,[](lv_event_t *){request(View::Portal);});
    auto confirm=[](lv_event_t *){
        String error;if(!pager::reset(eraseReset,error))lv_label_set_text(errorLabel,error.c_str());
    };
    if(eraseReset)dangerButton(screen,"Confirm delete",160,424,148,confirm,44);
    else warnButton(screen,"Confirm reboot",160,424,148,confirm,44);
}
void updateMode() {
    bool publicNote = lv_obj_has_state(modeSwitch, LV_STATE_CHECKED);
    lv_label_set_text(modeLabel, publicNote ? LV_SYMBOL_EYE_OPEN " Public mention" : "Encrypted DM");
    lv_obj_set_style_text_color(modeLabel, color(publicNote ? Violet : Mint), 0);
}
void styleTextarea(lv_obj_t *area) {
    lv_obj_set_style_bg_color(area, color(Surface), 0);
    lv_obj_set_style_border_color(area, color(Border), 0);
    lv_obj_set_style_border_color(area, color(Mint), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(area, 1, 0);
    lv_obj_set_style_radius(area, 10, 0);
    lv_obj_set_style_pad_all(area, 10, 0);
    lv_obj_set_style_text_color(area, color(Foreground), 0);
    lv_obj_set_style_text_color(area, color(Muted), LV_PART_TEXTAREA_PLACEHOLDER);
}
void contactsView(lv_obj_t *screen) {
    label(screen,"Contacts",12,108,200,&lv_font_montserrat_24);
    button(screen,LV_SYMBOL_LEFT " Back",224,104,84,[](lv_event_t *){
        request(contactReturnCompose?View::Compose:View::Inbox);
    },false,nullptr,36);
    singleLine(label(screen,contacts::status(),12,140,296,&lv_font_montserrat_10,Muted));
    list=lv_table_create(screen);lv_obj_set_pos(list,12,160);lv_obj_set_size(list,296,248);
    lv_obj_set_style_bg_color(list,color(Surface),0);lv_obj_set_style_border_color(list,color(Border),0);
    lv_obj_set_style_border_width(list,1,0);lv_obj_set_style_radius(list,12,0);lv_obj_set_style_pad_all(list,0,0);
    lv_obj_set_style_bg_color(list,color(Surface),LV_PART_ITEMS);lv_obj_set_style_text_color(list,color(Foreground),LV_PART_ITEMS);
    lv_obj_set_style_text_font(list,&lv_font_montserrat_12,LV_PART_ITEMS);
    lv_obj_set_style_border_color(list,color(Border),LV_PART_ITEMS);lv_obj_set_style_border_width(list,1,LV_PART_ITEMS);
    lv_obj_set_style_pad_top(list,12,LV_PART_ITEMS);lv_obj_set_style_pad_bottom(list,12,LV_PART_ITEMS);
    lv_obj_set_style_pad_left(list,8,LV_PART_ITEMS);lv_obj_set_style_pad_right(list,8,LV_PART_ITEMS);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_table_set_col_cnt(list,3);lv_table_set_row_cnt(list,contacts::all().size()+1);
    lv_table_set_col_width(list,0,144);lv_table_set_col_width(list,1,96);lv_table_set_col_width(list,2,54);
    lv_table_set_cell_value(list,0,0,"NAME");lv_table_set_cell_value(list,0,1,"KEY");lv_table_set_cell_value(list,0,2,"EDIT");
    if(contacts::all().empty()) {
        lv_table_set_cell_value(list,1,0,"No contacts. Tap Add.");
        lv_table_add_cell_ctrl(list,1,0,LV_TABLE_CELL_CTRL_MERGE_RIGHT);
        lv_table_add_cell_ctrl(list,1,1,LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    }
    for(size_t i=0;i<contacts::all().size();++i) {
        const auto &contact=contacts::all()[i];uint16_t row=i+1;
        for(unsigned col=0;col<3;++col)lv_table_add_cell_ctrl(list,row,col,LV_TABLE_CELL_CTRL_TEXT_CROP);
        lv_table_set_cell_value(list,row,0,contacts::label(contact).c_str());
        lv_table_set_cell_value(list,row,1,String(keys::npub(contact.pubkey.c_str()).c_str()).substring(0,10).c_str());
        lv_table_set_cell_value(list,row,2,LV_SYMBOL_EDIT);
    }
    lv_obj_add_event_cb(list,[](lv_event_t *event){
        uint16_t row,col;lv_table_get_selected_cell(lv_event_get_target(event),&row,&col);
        if(row==0 || row==LV_TABLE_CELL_NONE || row>contacts::all().size())return;
        const auto &contact=contacts::all()[row-1];
        if(col==2){contactKey=contact.pubkey;contactAlias=contact.alias;request(View::ContactEdit);}
        else if(pager::canSend){recipient=contact.pubkey;if(!contactReturnCompose){replyId="";draft="";publicReply=false;}request(View::Compose);}
    },LV_EVENT_VALUE_CHANGED,nullptr);
    lv_obj_add_event_cb(list,[](lv_event_t *event){
        auto *draw=lv_event_get_draw_part_dsc(event);if(draw->part!=LV_PART_ITEMS)return;
        unsigned row=draw->id/3,col=draw->id%3;
        if(draw->rect_dsc)draw->rect_dsc->bg_color=color(row==0?0x19372f:row%2?Surface:0x111a28);
        if(draw->label_dsc){draw->label_dsc->color=color(row==0 || col==2?Mint:col==1?Muted:Foreground);draw->label_dsc->flag=LV_TEXT_FLAG_EXPAND;}
    },LV_EVENT_DRAW_PART_BEGIN,nullptr);
    button(screen,LV_SYMBOL_PLUS " Add",12,424,140,[](lv_event_t *){contactKey="";contactAlias="";request(View::ContactEdit);},true);
    button(screen,LV_SYMBOL_REFRESH " Names",160,424,148,[](lv_event_t *){contacts::refresh();ui::changed();});
}
void contactEditView(lv_obj_t *screen) {
    label(screen,"CONTACT NPUB / PUBLIC KEY",12,85,220,&lv_font_montserrat_10,Muted);
    bool saved=false;for(const auto &contact:contacts::all())if(contact.pubkey==contactKey)saved=true;
    if(saved)button(screen,"Delete",232,82,76,[](lv_event_t *){
        String error;if(contacts::remove(contactKey,error))request(View::Contacts);else lv_label_set_text(errorLabel,error.c_str());
    },false,nullptr,24);
    recipientArea=lv_textarea_create(screen);lv_obj_set_pos(recipientArea,12,116);lv_obj_set_size(recipientArea,296,62);styleTextarea(recipientArea);
    lv_textarea_set_max_length(recipientArea,512);lv_textarea_set_placeholder_text(recipientArea,"npub / hex / nprofile");
    lv_textarea_set_text(recipientArea,contactKey.c_str());
    if(!contactKey.isEmpty())lv_obj_add_state(recipientArea,LV_STATE_DISABLED);
    label(screen,"LOCAL NICKNAME (OPTIONAL)",12,188,296,&lv_font_montserrat_10,Muted);
    bodyArea=lv_textarea_create(screen);lv_obj_set_pos(bodyArea,12,203);lv_obj_set_size(bodyArea,296,38);styleTextarea(bodyArea);
    lv_textarea_set_one_line(bodyArea,true);lv_textarea_set_max_length(bodyArea,32);lv_textarea_set_text(bodyArea,contactAlias.c_str());
    label(screen,"Leave blank to use the fetched Nostr name.",12,247,296,&lv_font_montserrat_10,Muted);
    errorLabel=label(screen,"",12,264,296,&lv_font_montserrat_10,Coral);singleLine(errorLabel);
    button(screen,"Cancel",12,280,140,[](lv_event_t *){request(View::Contacts);},false,nullptr,38);
    button(screen,"Save",160,280,148,[](lv_event_t *){
        String error;if(contacts::save(lv_textarea_get_text(recipientArea),lv_textarea_get_text(bodyArea),error))request(View::Contacts);
        else lv_label_set_text(errorLabel,error.c_str());
    },true,nullptr,38);
    keyboard=lv_keyboard_create(screen);lv_obj_set_align(keyboard,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(keyboard,0,326);lv_obj_set_size(keyboard,320,154);
    lv_obj_set_style_bg_color(keyboard,color(0x101927),0);lv_obj_set_style_bg_color(keyboard,color(0x202d40),LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard,color(Foreground),LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard,contactKey.isEmpty()?recipientArea:bodyArea);
    for(auto *area:{recipientArea,bodyArea})lv_obj_add_event_cb(area,[](lv_event_t *e){lv_keyboard_set_textarea(keyboard,lv_event_get_target(e));},LV_EVENT_FOCUSED,nullptr);
}
void composeView(lv_obj_t *screen) {
    singleLine(label(screen, recipient.isEmpty()?String("TO"):String("TO  ")+contacts::display(recipient), 12, 85, 296, &lv_font_montserrat_10, Muted));
    recipientArea = lv_textarea_create(screen);
    lv_obj_set_pos(recipientArea, 12, 100);
    lv_obj_set_size(recipientArea, 188, 38);
    button(screen,"Contacts",208,100,100,[](lv_event_t *){
        recipient=lv_textarea_get_text(recipientArea);draft=lv_textarea_get_text(bodyArea);
        publicReply=lv_obj_has_state(modeSwitch,LV_STATE_CHECKED);contactReturnCompose=true;request(View::Contacts);
    },false,nullptr,38);
    styleTextarea(recipientArea);
    lv_textarea_set_one_line(recipientArea, true);
    lv_textarea_set_max_length(recipientArea, 512);
    lv_textarea_set_placeholder_text(recipientArea, "Name / npub / hex");
    lv_textarea_set_text(recipientArea, recipient.c_str());
    label(screen, "MESSAGE", 12, 145, 140, &lv_font_montserrat_10, Muted);
    counter = label(screen, "0 / 512 bytes", 166, 145, 142, &lv_font_montserrat_10, Muted);
    lv_obj_set_style_text_align(counter, LV_TEXT_ALIGN_RIGHT, 0);
    bodyArea = lv_textarea_create(screen);
    lv_obj_set_pos(bodyArea, 12, 160);
    lv_obj_set_size(bodyArea, 296, 66);
    styleTextarea(bodyArea);
    lv_textarea_set_max_length(bodyArea, 512);
    lv_textarea_set_placeholder_text(bodyArea, "What's your signal?");
    lv_textarea_set_text(bodyArea,draft.c_str());
    lv_label_set_text(counter,(String(unsigned(draft.length()))+" / 512 bytes").c_str());
    lv_obj_add_event_cb(bodyArea, [](lv_event_t *) {
        size_t bytes = strlen(lv_textarea_get_text(bodyArea));
        String text = String(unsigned(bytes)) + " / 512 bytes";
        lv_label_set_text(counter, text.c_str());
        lv_obj_set_style_text_color(counter, color(bytes > 512 ? Coral : Muted), 0);
        lv_label_set_text(errorLabel, "");
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    modeSwitch = lv_switch_create(screen);
    lv_obj_set_pos(modeSwitch, 12, 236);
    lv_obj_set_size(modeSwitch, 40, 22);
    lv_obj_set_style_bg_color(modeSwitch, color(Border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(modeSwitch, color(Violet), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (publicReply) lv_obj_add_state(modeSwitch, LV_STATE_CHECKED);
    modeLabel = label(screen, "", 64, 239, 148, &lv_font_montserrat_12);
    label(screen, "Tap to switch", 222, 240, 86, &lv_font_montserrat_10, Muted);
    updateMode();
    lv_obj_add_event_cb(modeSwitch, [](lv_event_t *) { updateMode(); }, LV_EVENT_VALUE_CHANGED, nullptr);
    errorLabel = label(screen, "", 12, 261, 296, &lv_font_montserrat_10, Coral);
    singleLine(errorLabel);
    button(screen, LV_SYMBOL_CLOSE " Cancel", 12, 280, 140,
           [](lv_event_t *) { request(View::Inbox); }, false, nullptr, 38);
    button(screen, LV_SYMBOL_UPLOAD " Send", 160, 280, 148, [](lv_event_t *) {
        String error;
        if (pager::send(lv_textarea_get_text(recipientArea), lv_textarea_get_text(bodyArea),
                        lv_obj_has_state(modeSwitch, LV_STATE_CHECKED), replyId, error)) request(View::Inbox);
        else lv_label_set_text(errorLabel, error.c_str());
    }, true, nullptr, 38);
    keyboard = lv_keyboard_create(screen);
    lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(keyboard, 0, 326);
    lv_obj_set_size(keyboard, 320, 154);
    lv_obj_set_style_bg_color(keyboard, color(0x101927), 0);
    lv_obj_set_style_pad_all(keyboard, 5, 0);
    lv_obj_set_style_pad_row(keyboard, 5, 0);
    lv_obj_set_style_pad_column(keyboard, 4, 0);
    lv_obj_set_style_bg_color(keyboard, color(0x202d40), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, color(0x315b50), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, color(Foreground), LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, recipient.isEmpty() ? recipientArea : bodyArea);
    for (auto *area : {recipientArea, bodyArea}) lv_obj_add_event_cb(area, [](lv_event_t *event) {
        lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
    }, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(keyboard, [](lv_event_t *event) {
        if (lv_event_get_code(event) == LV_EVENT_READY) lv_keyboard_set_textarea(keyboard, bodyArea);
        else if (lv_event_get_code(event) == LV_EVENT_CANCEL) request(View::Inbox);
    }, LV_EVENT_ALL, nullptr);
}
void render() {
    refreshShown();
    drawnReady=pager::wifiConnected() && pager::configured();
    lv_coord_t scroll = view==View::Detail && detailBody && drawnMessage.id==selected
        ? lv_obj_get_scroll_y(detailBody) : 0;
    detailBody = nullptr;
    list = nullptr;
    auto *old = lv_scr_act(), *screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, color(Background), 0);
    lv_obj_set_style_text_color(screen, color(Foreground), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    header(screen);
    if (!drawnReady && view!=View::Portal && view!=View::Web && view!=View::ResetConfirm) connectionView(screen);
    else if (view == View::Inbox) inboxView(screen);
    else if (view == View::Detail) detailView(screen);
    else if (view == View::Portal) portalView(screen);
    else if (view == View::Web) webView(screen);
    else if (view == View::Contacts) contactsView(screen);
    else if (view == View::ContactEdit) contactEditView(screen);
    else if (view == View::ResetConfirm) resetConfirmView(screen);
    else if (view == View::DeleteConfirm) deleteConfirmView(screen);
    else composeView(screen);
    if(detailBody && scroll>0) {
        lv_obj_update_layout(screen);
        lv_obj_scroll_to_y(detailBody,scroll,LV_ANIM_OFF);
    }
    lv_scr_load(screen);
    lv_obj_del_async(old);
    messageViewChanged(true);
}
}
namespace ui {
void begin() {
    lv_disp_set_theme(nullptr, lv_theme_default_init(nullptr, color(Mint), color(Violet), true, &lv_font_montserrat_14));
    render();
    dirty = false;
}
void loop() {
    refreshShown();
    if (showFirstRequested) {
        showFirstRequested = false;
        if (view == View::Inbox && !shownList.empty()) {inboxAnchor = shownList[0]->id; inboxPage = 0; dirty = true;}
    }
    bool connectionChanged=drawnReady!=(pager::wifiConnected() && pager::configured());
    if(connectionChanged && drawnReady && view==View::Compose) {
        recipient=lv_textarea_get_text(recipientArea);draft=lv_textarea_get_text(bodyArea);
        publicReply=lv_obj_has_state(modeSwitch,LV_STATE_CHECKED);
    }
    if(connectionChanged && drawnReady && view==View::ContactEdit) {
        contactKey=lv_textarea_get_text(recipientArea);contactAlias=lv_textarea_get_text(bodyArea);
    }
    bool transition = pending >= 0;
    if (transition) { view = View(pending); pending = -1; dirty = true; }
    if (connectionChanged || transition || (dirty && view != View::Compose && view != View::ContactEdit)) {
        if(connectionChanged || transition || messageViewChanged())render();
        dirty = false;
    }
    updateStatus();
    if (notifyRequested) {
        notifyRequested = false;
        alertUntil = millis() + 1800;
        if (notification) lv_obj_set_style_bg_opa(notification, LV_OPA_COVER, 0);
    }
    if (alertUntil && int32_t(millis() - alertUntil) >= 0) {
        alertUntil = 0;
        if (notification) lv_obj_set_style_bg_opa(notification, LV_OPA_TRANSP, 0);
    }
    lv_timer_handler();
}
void changed(bool notify) {
    dirty = true;
    if (notify) {notifyRequested = true; showFirstRequested = true;}
}
void portal(const String &ssid, const String &password, const String &message) {
    pagerSync::Lock guard;
    portalSsid = ssid;
    portalPassword = password;
    portalMessage = message;
    request(View::Portal);
}
void inbox() { pagerSync::Lock guard; portalSsid = ""; request(View::Inbox); }
}
