#include "pager.h"
#include "board.h"
#include "contacts.h"
#include <fstream>
#include <cassert>
#include <cstring>
#include <iostream>
#include <algorithm>
uint32_t clockMs = 0;
extern "C" uint32_t millis(void) { return clockMs; }
namespace pager {
std::vector<Message> messages;
String status = "Online / waiting for messages", publicKey = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
String webAddress="https://pager.local/",webIpAddress="https://192.168.1.42/", webPin="482916", webFingerprint="8f2a123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
bool canSend = true;
bool inboxReceivedOnly = true;
bool stationConnected=true,hasConfiguration=true;
bool wifiConnected(){return stationConnected;}
bool configured(){return hasConfiguration;}
unsigned resets=0;bool erased=false;
unsigned reconnects=0,deletions=0;
bool deletedAll=false;
void reconnectRelays(){++reconnects;}
bool deleteMessages(const String &id,bool all,String &) {
    ++deletions;deletedAll=all;
    messages.erase(std::remove_if(messages.begin(),messages.end(),[&](const Message &message){return all || message.id==id;}),messages.end());return true;
}
bool reset(bool eraseData,String &){++resets;erased=eraseData;return true;}
void configure() { webIpAddress="https://192.168.4.1/"; status = "Configuration / Wi-Fi setup"; ui::portal("Nostr-Pager-a1b2", "8b47ad19cf22", "Choose Wi-Fi, a Nostr key and your relay.\nSave to connect and start receiving."); }
void resume() { ui::inbox(); }
void markRead(const String &id) { for (auto &message : messages) if (message.id == id) message.read = true; }
void older() {}
bool send(const String &, const String &, bool, const String &, String &) { return true; }
}
namespace contacts {
std::vector<Contact> book;
const std::vector<Contact> &all(){return book;}
String label(const Contact &c){return c.alias.isEmpty()?c.name:c.alias;}
String display(const String &pubkey){for(const auto &c:book)if(c.pubkey==pubkey)return label(c);return pubkey.substring(0,10);}
bool save(const String &,const String &,String &){return true;}
bool remove(const String &,String &){return true;}
void refresh(){}
String status(){return String("Nostr names cached / ")+String(unsigned(book.size()))+" contacts";}
}
namespace {
lv_color_t framebuffer[320 * 480];
unsigned flushes=0;
bool checkOnly=false;
void flush(lv_disp_drv_t *display, const lv_area_t *area, lv_color_t *pixels) {
    ++flushes;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) framebuffer[y * 320 + x] = *pixels++;
    lv_disp_flush_ready(display);
}
void settle() {
    for (int i = 0; i < 5; ++i) { clockMs += 40; ui::loop(); }
    lv_refr_now(nullptr);
}
bool contains(lv_obj_t *root,const char *text) {
    if(lv_obj_check_type(root,&lv_label_class) && strstr(lv_label_get_text(root),text))return true;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i)if(contains(lv_obj_get_child(root,i),text))return true;
    return false;
}
void capture(const char *name) {
    settle();
    assert(contains(lv_scr_act(),pager::webPin.c_str())==(strcmp(name,"setup")==0));
    if(checkOnly)return;
    std::ofstream file(std::string("assets/previews/") + name + ".ppm", std::ios::binary);
    file << "P6\n320 480\n255\n";
    for (auto pixel : framebuffer) {
        lv_color32_t rgb;
        rgb.full = lv_color_to32(pixel);
        char bytes[] = {char(rgb.ch.red), char(rgb.ch.green), char(rgb.ch.blue)};
        file.write(bytes, 3);
    }
}
lv_obj_t *findLabel(lv_obj_t *root, const char *text) {
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
        if (auto *found = findLabel(lv_obj_get_child(root, i), text)) return found;
    return nullptr;
}
void click(const char *label) {
    auto *object = findLabel(lv_scr_act(), label);
    if(!object)std::cerr<<"Missing preview control: "<<label<<"\n";
    assert(object && "Preview control missing");
    lv_event_send(lv_obj_get_parent(object), LV_EVENT_CLICKED, nullptr);
    settle();
}
void chooseContact(uint16_t row,uint16_t col) {
    lv_obj_t *object=nullptr;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i){auto *child=lv_obj_get_child(lv_scr_act(),i);if(lv_obj_check_type(child,&lv_table_class))object=child;}
    assert(object);auto *table=reinterpret_cast<lv_table_t*>(object);table->row_act=row;table->col_act=col;
    lv_event_send(object,LV_EVENT_VALUE_CHANGED,nullptr);settle();
}
}
int main(int argc,char **argv) {
    checkOnly=argc>1 && strcmp(argv[1],"--check-only")==0;
    lv_init();
    static lv_color_t pixels[320 * 24];
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, pixels, nullptr, 320 * 24);
    static lv_disp_drv_t display;
    lv_disp_drv_init(&display);
    display.hor_res = 320; display.ver_res = 480;
    display.draw_buf = &buffer; display.flush_cb = flush;
    lv_disp_drv_register(&display);
    ui::begin(); capture("empty");
    Message a; a.kind = 14; a.id = "a"; a.peer = pager::publicKey;
    a.text = "Hey! Coffee at the Bitcoin meetup?"; a.timestamp = 1790937600;
    Message b = a; b.id = "b"; b.read = true; b.kind = 1;
    b.peer = "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e";
    b.text = "The next block is yours. See you there."; b.timestamp -= 600;
    Message c = a; c.id = "c"; c.outgoing = c.read = true; c.delivery = "accepted";
    c.text = "On my way. Keep a seat for me!"; c.timestamp -= 1200;
    pager::status = "Online / 3 messages";
    contacts::Contact contact;contact.pubkey=pager::publicKey;contact.name="Alice";contacts::book={contact};
    contact.pubkey=b.peer;contact.name="Bob";contacts::book.push_back(contact);
    contact.pubkey="c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";contact.name="Carol";contacts::book.push_back(contact);
    contact.pubkey="f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";contact.name="A contact with a very long display name";contacts::book.push_back(contact);
    pager::inboxReceivedOnly = false; // keep sent copies listed for the navigation checks below
    pager::messages = {a, b, c}; ui::changed(); capture("inbox");
    auto *unchangedScreen=lv_scr_act();unsigned unchangedFlushes=flushes;
    ui::changed();settle();
    assert(lv_scr_act()==unchangedScreen && flushes==unchangedFlushes);
    assert(!contains(lv_scr_act(),"The next block"));
    click(LV_SYMBOL_LEFT " Prev");assert(contains(lv_scr_act(),a.text.c_str()));
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"The next block"));
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"On my way."));
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"On my way."));
    click(LV_SYMBOL_LEFT " Prev");click(LV_SYMBOL_LEFT " Prev");
    Message newest=a;newest.id="new";newest.text="A new message arrived while browsing.";
    pager::messages.insert(pager::messages.begin(),newest);ui::changed();settle();
    assert(contains(lv_scr_act(),a.text.c_str()) && contains(lv_scr_act(),"2 / 4"));
    pager::messages.erase(pager::messages.begin());ui::changed();settle();
    click(a.text.c_str()); capture("message");
    // A relay update arriving before the next render must not change which
    // message the visible inbox card opens.
    ui::inbox();settle();
    pager::messages.insert(pager::messages.begin(),newest);ui::changed();
    click(a.text.c_str());assert(pager::messages[1].read && !pager::messages[0].read);
    pager::messages.erase(pager::messages.begin());ui::changed();settle();
    String longText;
    for(unsigned i=0;i<8;++i)longText=longText+"A long message keeps its reading position.\n";
    pager::messages[0].text=longText;ui::changed();settle();
    auto *bodyText=findLabel(lv_scr_act(),longText.c_str());assert(bodyText);
    auto *body=lv_obj_get_parent(bodyText);
    lv_obj_scroll_to_y(body,60,LV_ANIM_OFF);settle();
    const auto readingPosition=lv_obj_get_scroll_y(body);assert(readingPosition>0);
    pager::messages.insert(pager::messages.begin(),newest);ui::changed();settle();
    bodyText=findLabel(lv_scr_act(),longText.c_str());assert(bodyText);body=lv_obj_get_parent(bodyText);
    assert(lv_obj_get_scroll_y(body)==readingPosition);
    pager::messages.erase(pager::messages.begin());pager::messages[0].text=a.text;ui::changed();settle();
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"The next block"));
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"On my way."));
    click("Next " LV_SYMBOL_RIGHT);assert(contains(lv_scr_act(),"On my way."));
    click(LV_SYMBOL_LEFT " Prev");click(LV_SYMBOL_LEFT " Prev");
    assert(pager::messages[0].read && pager::messages[1].read);
    pager::messages.erase(pager::messages.begin());ui::changed();settle();
    assert(contains(lv_scr_act(),"This message is no longer in the cache."));
    auto *replyButton=lv_obj_get_parent(findLabel(lv_scr_act(),LV_SYMBOL_EDIT " Reply"));
    assert(lv_obj_has_state(replyButton,LV_STATE_DISABLED));
    pager::messages.insert(pager::messages.begin(),a);ui::changed();settle();
    click("Delete");capture("message-delete-confirm");assert(pager::deletions==0);
    click("Cancel");assert(pager::deletions==0 && contains(lv_scr_act(),a.text.c_str()));
    click("Delete");click("Confirm deletion");assert(pager::deletions==1 && !pager::deletedAll && pager::messages.size()==2);
    assert(contains(lv_scr_act(),"The next block"));
    pager::messages.insert(pager::messages.begin(),a);ui::changed();settle();click(LV_SYMBOL_LEFT " Prev");click(a.text.c_str());
    click(LV_SYMBOL_EDIT " Reply"); capture("composer");
    lv_obj_t *keyboard = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(lv_scr_act()); ++i) {
        auto *child = lv_obj_get_child(lv_scr_act(), i);
        if (lv_obj_check_type(child, &lv_keyboard_class)) keyboard = child;
    }
    assert(keyboard && "Composer keyboard missing");
    lv_area_t keyboardArea;
    lv_obj_get_coords(keyboard, &keyboardArea);
    assert(keyboardArea.y1 == 326 && keyboardArea.y2 == 479);
    lv_btnmatrix_set_selected_btn(keyboard, 1); // q in the default lower-case map
    lv_event_send(keyboard, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(strcmp(lv_textarea_get_text(lv_keyboard_get_textarea(keyboard)), "q") == 0);
    pager::stationConnected=false;settle();
    assert(contains(lv_scr_act(),"Connect to Wi-Fi"));
    assert(!findLabel(lv_scr_act(),LV_SYMBOL_UPLOAD " Send"));
    pager::stationConnected=true;settle();
    keyboard=nullptr;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i){auto *child=lv_obj_get_child(lv_scr_act(),i);if(lv_obj_check_type(child,&lv_keyboard_class))keyboard=child;}
    assert(keyboard && strcmp(lv_textarea_get_text(lv_keyboard_get_textarea(keyboard)),"q")==0);
    click("Contacts");capture("contacts");chooseContact(1,0);
    keyboard=nullptr;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i){auto *child=lv_obj_get_child(lv_scr_act(),i);if(lv_obj_check_type(child,&lv_keyboard_class))keyboard=child;}
    assert(keyboard && strcmp(lv_textarea_get_text(lv_keyboard_get_textarea(keyboard)),"q")==0);
    ui::inbox();settle();click("Contacts");click(LV_SYMBOL_PLUS " Add");capture("contact-edit");
    ui::inbox(); settle(); pager::configure(); capture("setup");
    click("Reboot");capture("reset-confirm");assert(pager::resets==0);
    click("Cancel");assert(pager::resets==0);
    click("Reboot");click("Confirm reboot");assert(pager::resets==1 && !pager::erased);
    click("Cancel");click("Delete data + reset");capture("delete-confirm");assert(pager::resets==1);
    click("Cancel");assert(pager::resets==1);
    click("Delete data + reset");click("Confirm delete");assert(pager::resets==2 && pager::erased);
    click("Cancel");
    click("Web details " LV_SYMBOL_RIGHT); capture("web-access");
    click("Reconnect");assert(pager::reconnects==1 && pager::resets==2);
    ui::inbox();settle();click("Delete all");assert(pager::deletions==1);
    click("Cancel");assert(pager::deletions==1);
    click("Delete all");click("Confirm deletion");assert(pager::deletions==2 && pager::deletedAll && pager::messages.empty());
    pager::webAddress="https://192.168.1.42/";
    pager::status = "Online / public mentions";
    pager::messages = {b}; pager::canSend = false;
    ui::inbox(); settle(); ui::changed(); capture("read-only");
    pager::stationConnected=false;settle();
    assert(contains(lv_scr_act(),"Connect to Wi-Fi") && !contains(lv_scr_act(),b.text.c_str()));
    assert(!findLabel(lv_scr_act(),"Contacts"));
    click("Open Setup");
    assert(contains(lv_scr_act(),"Connect your pager") && contains(lv_scr_act(),pager::webPin.c_str()));
    assert(!findLabel(lv_scr_act(),"Reboot") && !findLabel(lv_scr_act(),"Delete data + reset"));
    pager::stationConnected=true;settle();
    assert(findLabel(lv_scr_act(),"Reboot"));
    click(LV_SYMBOL_LEFT " Back to inbox");assert(contains(lv_scr_act(),b.text.c_str()));
    pager::hasConfiguration=false;settle();assert(contains(lv_scr_act(),"Finish setup"));
    pager::hasConfiguration=true;settle();assert(contains(lv_scr_act(),b.text.c_str()));
    // Received-only inbox (the default): sent copies are hidden and a new incoming message jumps to the first entry.
    pager::inboxReceivedOnly = true; pager::canSend = true;
    Message older = b; older.id = "older"; older.text = "An older received message."; older.timestamp -= 5000;
    pager::messages = {b, c, older}; ui::inbox(); settle();
    assert(contains(lv_scr_act(), b.text.c_str()) && contains(lv_scr_act(), "1 / 2"));
    click("Next " LV_SYMBOL_RIGHT); assert(contains(lv_scr_act(), older.text.c_str()) && !contains(lv_scr_act(), c.text.c_str()));
    Message fresh = a; fresh.id = "fresh"; fresh.text = "Brand new message."; fresh.timestamp += 100; fresh.read = false;
    pager::messages.insert(pager::messages.begin(), fresh); ui::changed(true); settle();
    assert(contains(lv_scr_act(), fresh.text.c_str()) && contains(lv_scr_act(), "1 / 3"));
    click(LV_SYMBOL_DOWNLOAD " Received"); // toggle to show sent copies too
    assert(!pager::inboxReceivedOnly && findLabel(lv_scr_act(), LV_SYMBOL_LIST " All") && contains(lv_scr_act(), "1 / 4"));
    click(LV_SYMBOL_LIST " All"); assert(pager::inboxReceivedOnly && contains(lv_scr_act(), "1 / 3"));
    std::cout << "Offline connection prompts, simplified setup, automatic recovery and draft preservation passed.\n";
    std::cout << "Rendered LVGL screens; button navigation, cache changes, drafts, setup-only PIN, reconnect, and reset/single/all message deletion confirmations/cancellation passed.\n";
}
