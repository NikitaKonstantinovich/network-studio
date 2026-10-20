#pragma once
#include "network.h"
#include <filesystem>
#include <map>

namespace studio {
Config animat_example(bool spatial);
void save_animat_stage_result(Simulator& s,bool exit);
std::string validate_animat(const Config& c);
std::string validate_animat_environment(const Config& c,const AnimatEnvironment& e);
uint64_t arena_zone(const ModelSpace& arena,const AnimatEnvironment& e,double x,double y);
double arena_frequency(const AnimatEnvironment& e,uint64_t id);
struct AnimatMotion {double x=0,y=0,requested_x=0,requested_y=0;};
AnimatMotion animat_motion(const ModelSpace& arena,const AnimatConfig& settings,
                          double x,double y,double vx,double vy,double last_x,double last_y);
double animat_input(const Config& c,const AnimatRuntime& state,int neuron,double time);
std::string animat_environment_json(const AnimatEnvironment& e);
AnimatEnvironment animat_environment_from_json(const std::string& s);
std::string serialize_animat(const Simulator& s);
void deserialize_animat(Simulator& s,const std::string& text);
std::string animat_commands_json(const std::vector<AnimatAction>& actions);
std::vector<AnimatAction> read_animat_commands(const std::string& text);
void validate_animat_runtime(const Simulator& s);
std::string animat_csv_header();
std::string animat_csv_row(const AnimatRow& row);
std::string encode_animat_row(const AnimatRow& row);
AnimatRow decode_animat_row(const std::string& bytes);
struct AnimatVisit {
    uint64_t zone=0,stage_run=0;
    double start=0,end=0;
    bool left_censored=false,right_censored=false;
};
struct AnimatCrossing {
    double time=0,fraction=0;
    uint64_t from=0,to=0,stage_run=0;
    bool teleport=false;
};
struct AnimatZoneStatistics {
    uint64_t zone=0,stage_run=0,entries=0,exits=0;
    double time=0,fraction=0,stage_fraction=0;
};
struct AnimatStageStatistics {
    uint64_t run=0;
    int stage=-1;
    double time=0,path=0,mean_speed=0,first_exit=-1;
};
struct AnimatAnalysis {
    double requested_from=0,requested_to=0,from=0,to=0,path=0,mean_speed=0,first_exit=-1;
    bool complete=false;
    std::vector<AnimatZoneStatistics> zones;
    std::vector<AnimatStageStatistics> stages;
    std::vector<AnimatVisit> visits;
    std::vector<AnimatCrossing> crossings;
};
AnimatAnalysis analyze_animat(const Config& c,const AnimatRuntime& state,double end,double from,double to);
void export_animat(const Simulator& s,const std::filesystem::path& prefix,double from=0,double to=-1);
void export_animat_analysis(const AnimatAnalysis& a,const std::filesystem::path& prefix);
}
