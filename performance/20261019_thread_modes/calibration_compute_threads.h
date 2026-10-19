#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace studio::compute {
// Runtime policy only; never part of Config or checkpoint state. Count includes
// the coordinator, which executes block zero. GUI/render threads are separate.
struct ThreadMode { int count; const char* argument; };
inline constexpr std::array<ThreadMode,8> ThreadModes{{
    {1,"1"},{2,"2"},{4,"4"},{6,"6"},{8,"8"},{12,"12"},{16,"16"},{0,"auto"}
}};
inline constexpr int MaxThreads=ThreadModes[ThreadModes.size()-2].count;
inline constexpr bool supported(int count) {
    for(auto mode:ThreadModes)if(mode.count==count)return true;
    return false;
}
inline std::string choices() {
    std::string result;
    for(auto mode:ThreadModes){if(!result.empty())result+='|';result+=mode.argument;}
    return result;
}
template<class Char> int parse(std::basic_string_view<Char> text,const char* option) {
    for(auto mode:ThreadModes) {
        const std::string_view candidate=mode.argument;
        if(text.size()!=candidate.size())continue;
        bool equal=true;
        for(size_t i=0;i<text.size();++i)if(text[i]!=Char(candidate[i]))equal=false;
        if(equal)return mode.count;
    }
    throw std::invalid_argument(std::string(option)+" must be "+choices()+".");
}
// Measured conservative policy, deliberately separate from the manual menu.
// 12/16 logical workers need not beat eight physical cores. Unknown hardware
// concurrency stays serial. See performance/20261019_thread_modes/REPORT_RU.md.
inline int automatic(size_t synapses,unsigned available) {
    const int limit=synapses<2048?1:synapses<8192?4:8;
    int result=1;
    for(int count:{2,4,8})if(count<=limit&&unsigned(count)<=available)result=count;
    return result;
}
}
