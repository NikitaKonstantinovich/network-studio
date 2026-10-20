#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "field_settings.h"

namespace studio {
constexpr size_t MaxAnimatRows=1000000, MaxAnimatActions=4096, MaxAnimatZones=64;
enum class ZoneShape { Rectangle, Circle };
enum class AnimatBoundary { Clamp, Reject };
enum class AnimatSilence { Stay, RepeatLast };
struct ArenaZone {
    uint64_t id=1;
    std::string name="Zone";
    ZoneShape shape=ZoneShape::Rectangle;
    int priority=0;
    double x=0,y=0,width=.5,height=1,frequency=10;
};
struct AnimatEnvironment {
    std::vector<ArenaZone> zones;
    double outside_frequency=1,amplitude=30,width=.001,radius=.15;
    bool enabled=true;
};
struct AnimatConfig {
    bool enabled=false;
    uint64_t stimulator=0;
    double initial_x=.5,initial_y=.5,interval=.02,gain=.05,max_displacement=.05,zero_threshold=1e-12;
    AnimatBoundary boundary=AnimatBoundary::Clamp;
    AnimatSilence silence=AnimatSilence::Stay;
    // Only scientific grid/filter/window fields are serialized and used.
    // Display scaling, camera, arrows and update rate have no role in control.
    TopologyViewState field;
    AnimatEnvironment environment;
};
struct AnimatStage {
    bool environment=false,home=false,marker=false,position=false;
    double x=.5,y=.5;
    AnimatEnvironment value;
};
enum class AnimatActionKind { Environment, Home, Stage, Learning, Continue };
struct AnimatAction {
    uint64_t sequence=0;
    double time=0;
    AnimatActionKind kind=AnimatActionKind::Home;
    AnimatEnvironment environment;
    int stage=0;
    bool automatic=false,learning=true,clear_traces=false;
    double learning_rate=1;
};
enum class AnimatRowKind { Initial, Tick, Home, Environment, Stage, Stimulator };
struct AnimatRow {
    double time=0,x0=0,y0=0,x=0,y=0,vx=0,vy=0,requested_x=0,requested_y=0;
    double frequency=0,requested_frequency=0,apply_time=0;
    double amplitude=0,start=0,stop=0,width=0,first_delay=0,radius=0;
    uint64_t zone=0,environment=0,stage_run=0;
    int stage=-1;
    AnimatRowKind kind=AnimatRowKind::Initial;
    bool enabled=false,learning=true,marker=false;
    double learning_rate=1;
};
struct AnimatEnvironmentVersion {double time=0;AnimatEnvironment value;};
struct AnimatSavedState {double time=0;uint64_t stage_run=0;bool exit=false;std::vector<double> state,plasticity;};
struct AnimatRuntime {
    std::string initial_config;
    std::vector<AnimatSavedState> saved;
    double x=0,y=0,last_x=0,last_y=0,desired_frequency=0;
    uint64_t next_step=0,period_steps=0,stage_run=0,next_action=1;
    std::vector<AnimatRow> rows;
    std::vector<AnimatEnvironmentVersion> environments;
    std::vector<AnimatAction> pending,actions;
};
}
