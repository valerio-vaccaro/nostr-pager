#pragma once
#include <string>
namespace resetControl {
enum class Mode { Invalid, Restart, Erase };
inline Mode confirmed(const std::string &mode,const std::string &confirmation) {
    if(confirmation!="yes")return Mode::Invalid;
    if(mode=="restart")return Mode::Restart;
    if(mode=="erase")return Mode::Erase;
    return Mode::Invalid;
}
template<class Storage> bool eraseStoredData(Storage &storage) {
    // The durable wipe flag lives separately from credentials. Clear it last
    // so interruption while clearing configuration cannot cancel the wipe.
    bool ok=storage.openTls() && storage.formatFiles() && storage.clearTls() && storage.clearConfiguration() && storage.finishWipe();
    storage.closeTls();return ok;
}
}
