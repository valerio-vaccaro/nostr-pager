#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace inboxRequests {
// Keep public and private requests independent: rejection of a private filter
// must not close the public mention inbox on the same connection.
inline void build(JsonDocument &doc,const char *id,const String &own,bool privateInbox,
                  bool sentLegacy,unsigned limit,bool older,uint32_t until,uint32_t now,bool legacy=true) {
    JsonArray frame=doc.to<JsonArray>();frame.add("REQ");frame.add(id);
    auto filter=[&](unsigned kind,bool sent=false) {
        JsonObject row=frame.createNestedObject();row.createNestedArray("kinds").add(kind);
        row.createNestedArray(sent?"authors":"#p").add(own);
        row["limit"]=kind==1059 && !older?200:limit;
        if(older)row["until"]=until>0?until-1:0;
        // Envelopes can be backdated two days. Query a three-day window instead
        // of only the 20 most recent envelope timestamps, which can omit a new DM.
        else if(kind==1059)row["since"]=now>259200?now-259200:0;
    };
    if(!privateInbox)filter(1);
    // Some relays (e.g. Damus) demand authentication for kind-4 filters and refuse the whole request;
    // legacy=false keeps the modern gift-wrap (kind 1059) inbox working without it.
    else {if(legacy)filter(4);filter(1059);if(sentLegacy && legacy)filter(4,true);}
}
}
