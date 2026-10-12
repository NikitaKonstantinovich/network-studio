#pragma once
#include "network.h"
#include <cstdint>
#include <vector>

namespace studio::population {
struct Edge { int source=0,target=0; double weight=1; };
struct NodeState {
    double v=-65,u=-13,current=0,tm_u=0,tm_x=1,tm_y=0,glutamate=0;
};
// Restricted, homogeneous population model. GUI limits remain unchanged.
// Every outgoing edge from a source starts with identical pools and shares all
// kinetic parameters; its signed weight acts only on postsynaptic current.
class Population {
public:
    Population(const Config& base,int neurons,const std::vector<Edge>& edges);
    bool step();
    bool done() const { return !error.empty() || time>=config.duration; }
    Config config; // Cloned neurons: used only by external_current, never fixed-size core RHS.
    std::vector<NodeState> state;
    std::vector<std::uint64_t> spikes;
    std::vector<int> fired;
    std::vector<Event> events; // All localized events in the last main step.
    std::vector<std::pair<double,std::vector<NodeState>>> samples;
    std::uint64_t steps=0;
    double time=0;
    bool sampled=true;
    std::string error;
private:
    Synapse synapse;
    std::vector<std::vector<Edge>> outgoing;
    std::vector<NodeState> next;
    std::vector<double> input;
    std::uint64_t cached_bin=~std::uint64_t(0),next_sample=1;
    bool constant_input=false;
    numerical::Flow flow;
    std::vector<numerical::Threshold> thresholds;
    double suggested_step=0;
};
std::string validate_base(const Config& base,int neurons,const std::vector<Edge>& edges);
}
