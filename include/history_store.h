#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace historyStore {
constexpr size_t MarkerLimit=128;
struct Marker {std::string id;uint32_t timestamp;};
struct Filter {
    uint32_t before=0;
    std::vector<Marker> deleted;
    bool hides(const std::string &id,uint32_t timestamp) const {
        if(before && timestamp<=before)return true;
        for(const auto &marker:deleted)if(marker.id==id)return true;
        return false;
    }
    bool remember(const std::string &id,uint32_t timestamp) {
        if(hides(id,timestamp))return true;
        if(deleted.size()>=MarkerLimit)return false;
        deleted.push_back({id,timestamp});return true;
    }
};
enum class Result {Ok,NotFound,MarkersFull,WriteFailed};
template<class Record,class Save>
Result remove(std::vector<Record> &records,Filter &filter,const std::string &id,bool all,uint32_t now,Save save) {
    auto oldFilter=filter;
    if(all) {
        filter.before=std::max(filter.before,now);
        filter.deleted.erase(std::remove_if(filter.deleted.begin(),filter.deleted.end(),[&](const Marker &marker){return marker.timestamp<=filter.before;}),filter.deleted.end());
        for(const auto &record:records)if(!filter.remember(record.id.c_str(),record.timestamp)){filter=std::move(oldFilter);return Result::MarkersFull;}
        auto oldRecords=std::move(records);records.clear();
        if(save())return Result::Ok;
        records=std::move(oldRecords);filter=std::move(oldFilter);return Result::WriteFailed;
    }
    auto found=std::find_if(records.begin(),records.end(),[&](const Record &record){return record.id.c_str()==id;});
    if(found==records.end())return Result::NotFound;
    if(!filter.remember(id,found->timestamp))return Result::MarkersFull;
    size_t index=found-records.begin();Record removed=std::move(*found);records.erase(found);
    if(save())return Result::Ok;
    records.insert(records.begin()+index,std::move(removed));filter=std::move(oldFilter);return Result::WriteFailed;
}
}
