#pragma once
#include "network.h"
#include <filesystem>
#include <memory>

namespace studio {
// Presentation data only. No trajectory, caches, callbacks or solver ownership.
struct WeightSnapshot {
    Config config;
    std::vector<double> weights;
    double time=0;
    uint64_t steps=0;
    std::string name,origin;
};
struct WeightSettings {
    int polarity=0,model=0,enabled=0; // all/E/I; all/STP/exp/TM; all/on/off
    bool absolute=false;
    int bins=40;
    double boundary_tolerance=1e-6; // absolute tolerance in stored weight units
};
struct WeightStatistics {
    size_t count=0,learning_count=0,at_lower=0,at_upper=0;
    double mean=0,median=0,stddev=0,minimum=0,maximum=0,sum=0;
};
struct WeightDistribution {
    WeightStatistics stats;
    std::vector<size_t> indices;
    std::vector<double> values,counts;
};
struct WeightChange {size_t before=0,after=0;double raw_delta=0,delta=0;std::string id;};
struct WeightComparison {
    std::string error;
    WeightStatistics stats;
    size_t unchanged=0;
    std::vector<WeightChange> changes; // descending |display delta|, then stable ID
    std::vector<double> edges,counts;
};
struct WeightAnalysis {
    std::vector<double> edges;
    std::vector<WeightDistribution> distributions;
    WeightComparison difference;
    size_t reference=0,target=0;
};
WeightSnapshot capture_weights(const Config&,const std::vector<StdpState>&,double,uint64_t,const std::string& name);
WeightSnapshot initial_weights(const Config&,const std::string& name="Исходные из Config");
void validate_weights(const WeightSnapshot&);
bool inhibitory_analysis_edge(const Synapse&);
std::vector<std::string> weight_edge_ids(const Config&,std::string& error);
std::string compatible_weights(const WeightSnapshot&,const WeightSnapshot&);
WeightAnalysis analyze_weights(const std::vector<const WeightSnapshot*>&,const WeightSettings&,size_t reference=0,size_t target=0);
void save_weights(const WeightSnapshot&,const std::filesystem::path&);
WeightSnapshot load_weights(const std::filesystem::path&);
// Legacy arrays have no embedded identity: require the same-prefix experiment sidecar.
WeightSnapshot load_weight_summary(const std::filesystem::path& summary);
void export_weights(const std::vector<const WeightSnapshot*>&,const WeightSettings&,size_t reference,size_t target,const std::filesystem::path& prefix);
bool weights_cli(int argc,char** argv,int& exit_code);
}
