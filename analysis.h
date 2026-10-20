#pragma once
#include "network.h"
#include <filesystem>
#include <functional>
namespace studio {
// Presentation/postprocessing state, deliberately outside Config and checkpoint.
struct AnalysisInterval { double from=0,to=1; bool current_end=true; };
struct AnalysisProfile { std::string name; AnalysisSettings settings; };
inline constexpr char AnalysisAlgorithm[]="trailing-hysteresis-context-v2";
bool valid_analysis_interval(const AnalysisInterval& interval);
bool same_analysis_settings(const AnalysisSettings& a,const AnalysisSettings& b);
std::string analysis_profile_json(const AnalysisProfile& profile);
AnalysisProfile analysis_profile_from_json(const std::string& text);
std::string analysis_interval_json(const AnalysisInterval& interval);
AnalysisInterval analysis_interval_from_json(const std::string& text);
struct NetworkBurst { double start=0,end=0; uint64_t spikes=0; int participants=0; bool complete=true; };
struct StimulusResponse { double stimulus=0,response=-1,latency=-1; };
struct NetworkAnalysis {
    double from=0,until=0,stimulus_hz=0,response_hz=0;
    double requested_from=0,requested_until=0,available_from=0,available_until=0,detector_certain_from=0;
    bool full_interval=false,prehistory_complete=false;
    size_t population_size=0,missed=0,extra=0,incomplete_stimuli=0;
    std::vector<double> time,group_hz,neuron_hz,burst_hz;
    std::vector<NetworkBurst> bursts;
    std::vector<StimulusResponse> responses;
    std::string error;
};
NetworkAnalysis analyze_network(const Config& c,const History& history,double until,const AnalysisSettings& settings,const AnalysisInterval& interval={});
// Already population-filtered events, sorted by time. Used by the disk pager.
NetworkAnalysis analyze_event_source(const Config&,const History& stimulus_context,double until,const AnalysisSettings&,const AnalysisInterval&,size_t count,const std::function<Event(size_t)>& event);
// Empty means comparable. Names are labels; settings, coverage and detector context decide.
std::string compare_analysis(const NetworkAnalysis& a,const AnalysisSettings& sa,const NetworkAnalysis& b,const AnalysisSettings& sb);
std::string analysis_result_json(const NetworkAnalysis& result,const AnalysisProfile& profile);
std::string compare_analysis_files(const std::filesystem::path& a,const std::filesystem::path& b);
void export_analysis(const NetworkAnalysis& result,const AnalysisSettings& settings,const std::filesystem::path& prefix,const std::string& profile_name="");
// Read-only postprocessing of an existing export; never restores an incompatible checkpoint.
NetworkAnalysis analyze_export(const std::filesystem::path& prefix,const AnalysisSettings& settings,const AnalysisInterval& interval);
bool analysis_cli(int argc,char** argv,int& exit_code);
}
