#pragma once
// Opt-in diagnostic scopes. Worker phases are timed on the controller around
// their synchronous barrier (wall time, NOT summed helper CPU time).
#include <array>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
namespace studio::profile {
enum Category { step, trial, thresholds, threshold_sample, exact_synapses, exact_astro,
    rhs_synapses, rhs_neurons, rhs_astro, external, noise, stdp, journals, bounds,
    kinetic, input_boundaries, event_jumps, controls, count };
inline constexpr const char* names[]={"step","trial","thresholds","threshold_sample","exact_synapses","exact_astro",
    "rhs_synapses","rhs_neurons","rhs_astro","external","noise","stdp","journals","bounds",
    "kinetic","input_boundaries","event_jumps","controls"};
#ifdef STUDIO_PROFILE
using Clock=std::chrono::steady_clock;
struct Counter { uint64_t calls=0; double inclusive=0,exclusive=0; };
inline thread_local std::array<Counter,count> counters{};
inline thread_local std::array<uint64_t,5> candidate_histogram{};
inline thread_local uint64_t candidate_total=0,candidate_max=0;
inline void candidates(size_t n) {
    ++candidate_histogram[n==0?0:n==1?1:n<4?2:n<8?3:4];
    candidate_total+=n;candidate_max=std::max(candidate_max,uint64_t(n));
}
struct Scope;
inline thread_local Scope* current=nullptr;
struct Scope {
    Category category; Scope* parent; Clock::time_point begin; double children=0;
    explicit Scope(Category c):category(c),parent(current),begin(Clock::now()){current=this;}
    ~Scope(){double elapsed=std::chrono::duration<double>(Clock::now()-begin).count();auto& c=counters[category];
        ++c.calls;c.inclusive+=elapsed;c.exclusive+=elapsed-children;current=parent;if(parent)parent->children+=elapsed;}
};
inline void reset(){counters={};candidate_histogram={};candidate_total=candidate_max=0;}
inline void write(const std::filesystem::path& path){std::ofstream f(path);f<<std::setprecision(17)<<"{\"enabled\":true,\"categories\":[";
    for(int i=0;i<count;++i){if(i)f<<',';const auto& c=counters[i];f<<"{\"name\":\""<<names[i]<<"\",\"calls\":"<<c.calls<<",\"inclusive_seconds\":"<<c.inclusive<<",\"exclusive_seconds\":"<<c.exclusive<<'}';}
    f<<"],\"threshold_candidates\":{\"total\":"<<candidate_total<<",\"max\":"<<candidate_max<<",\"histogram_0_1_2to3_4to7_8plus\":[";
    for(size_t i=0;i<candidate_histogram.size();++i){if(i)f<<',';f<<candidate_histogram[i];}f<<"]}}\n";}
#else
inline void candidates(size_t){}
inline void reset(){}
inline void write(const std::filesystem::path& path){std::ofstream(path)<<"{\"enabled\":false}\n";}
#endif
}
#define STUDIO_JOIN_IMPL(a,b) a##b
#define STUDIO_JOIN(a,b) STUDIO_JOIN_IMPL(a,b)
#ifdef STUDIO_PROFILE
#define STUDIO_SCOPE(category) ::studio::profile::Scope STUDIO_JOIN(profile_scope_,__LINE__)(::studio::profile::category)
#else
#define STUDIO_SCOPE(category) ((void)0)
#endif
