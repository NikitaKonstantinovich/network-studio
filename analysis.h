#pragma once
#include "network.h"
#include <filesystem>
namespace studio {
struct NetworkBurst { double start=0,end=0; uint64_t spikes=0; int participants=0; bool complete=true; };
struct StimulusResponse { double stimulus=0,response=-1,latency=-1; };
struct NetworkAnalysis {
    double from=0,until=0,stimulus_hz=0,response_hz=0;
    size_t population_size=0,missed=0,extra=0,incomplete_stimuli=0;
    std::vector<double> time,group_hz,neuron_hz,burst_hz;
    std::vector<NetworkBurst> bursts;
    std::vector<StimulusResponse> responses;
    std::string error;
};
NetworkAnalysis analyze_network(const Config& c,const History& history,double until,const AnalysisSettings& settings);
void export_analysis(const NetworkAnalysis& result,const AnalysisSettings& settings,const std::filesystem::path& prefix);
}
