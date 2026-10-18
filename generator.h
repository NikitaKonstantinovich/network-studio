#pragma once
#include "network.h"
#include <cstddef>

namespace studio {

struct GaussianParameter {
    double mean = 0;
    double sd = 0;
};

struct PopulationGeneration {
    int count = 0;
    GaussianParameter a{.02,0}, b{.2,0}, c{-65,0}, d{8,0};
    // Incoming degree belongs to the target population. Absolute connection
    // weight belongs to the source population, whose type determines its sign.
    GaussianParameter in_degree{15,2}, weight{.5,.1};
};

enum class NetworkTopology { UniformRandom, NearestNeighbors, DirectedRing, NoConnections, WithinRadius };

struct NetworkGeneration {
    PopulationGeneration excitatory, inhibitory;
    NetworkTopology topology = NetworkTopology::UniformRandom;
    uint32_t seed = 42;
    ModelSpace space;
    double radius=.2;
    bool allow_self=false;
    // Shared edge parameters. Endpoints, astro, weight, enabled and inhibitory
    // STDP are assigned by the generator. Only exponential current and TM are
    // supported. All time constants use seconds, as in Config.
    Synapse synapse;
    // These fields apply to replacement only; append preserves the base run.
    double duration = 5, sample = .001;
    std::string name = "Generated Izhikevich network";
    NetworkGeneration();
};

struct DistributionSummary {
    double mean = 0, sd = 0, min = 0, max = 0;
};

struct GenerationSummary {
    int new_neurons = 0, new_synapses = 0;
    int excitatory_count = 0, inhibitory_count = 0;
    // One clamp per target whose rounded Gaussian draw is outside [0,N-1].
    int degree_clamped = 0;
    int insufficient_neighbors = 0;
    std::size_t parameter_resamples = 0;
    bool recording_reduced = false;
    // Generated neurons only, excitatory first, then inhibitory. Standard
    // deviations are population SDs. Empty distributions have zero statistics.
    std::vector<int> in_degrees;
    DistributionSummary degrees, absolute_weights;
    DistributionSummary lengths, excitatory_weights, inhibitory_weights;
    std::vector<double> edge_lengths, signed_weights;
};

// Pure generation: the input and summary are unchanged on failure. Throws
// std::invalid_argument with a diagnostic on invalid settings/size/budget.
// A preview is this same call without assigning the returned Config.
// Append adds an isolated group and preserves all previous nodes/edges and run
// settings. Replacement starts from default shared parameters, with no noise,
// stimulus or astrocytes. Gaussian parameters/weights use bounded rejection in
// their supported ranges, not clipping; degree draws are rounded and clipped.
// Reproducibility is guaranteed for the same build/standard library and seed.
Config generate_network(const Config& base, const NetworkGeneration& settings,
                        bool append, GenerationSummary* summary = nullptr);
std::string generation_json(const NetworkGeneration& settings);
std::vector<int> radius_candidates(const Config& c,int target,int first,int count,double radius,bool self);

}
