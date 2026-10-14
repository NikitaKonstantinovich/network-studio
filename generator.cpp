#include "generator.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

namespace studio {

NetworkGeneration::NetworkGeneration() {
    excitatory.count = 400;
    inhibitory.count = 100;
    synapse.model = SynapseModel::TsodyksMarkram;
    synapse.gain = 20;
    synapse.release = .5;
    synapse.tau_syn = .010;
    synapse.tau_d = .050;
    synapse.tau_f = 1;
    synapse.weight_min = 0;
    synapse.weight_max = 1;
    synapse.astro = -1;
    synapse.spill = 0;
}

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument("Network generation: " + message);
}

void check_gaussian(const GaussianParameter& g, double lo, double hi, const std::string& label) {
    if (!std::isfinite(g.mean) || g.mean < lo || g.mean > hi ||
        !std::isfinite(g.sd) || g.sd < 0)
        fail(label + " needs a finite mean in [" + std::to_string(lo) + ", " +
             std::to_string(hi) + "] and a finite nonnegative SD.");
}

void check_population(const PopulationGeneration& p, const std::string& label) {
    check_gaussian(p.a,.0001,1,label + " a");
    check_gaussian(p.b,-2,2,label + " b");
    check_gaussian(p.c,-100,0,label + " c");
    check_gaussian(p.d,-100,100,label + " d");
    check_gaussian(p.in_degree,0,std::numeric_limits<double>::max(),label + " in-degree");
    check_gaussian(p.weight,0,1,label + " absolute weight");
}

struct Sampler {
    std::mt19937 engine;
    std::normal_distribution<double> normal{0,1};
    std::uniform_real_distribution<double> position{.05,.95};
    std::size_t rejected = 0;
    explicit Sampler(uint32_t seed) : engine(seed) {}
    double draw(const GaussianParameter& g) {
        return g.sd == 0 ? g.mean : g.mean + g.sd * normal(engine);
    }
    double bounded(const GaussianParameter& g, double lo, double hi, const char* label) {
        for (int attempt=0; attempt<1024; ++attempt) {
            const double x = draw(g);
            if (std::isfinite(x) && x >= lo && x <= hi) return x;
            ++rejected;
        }
        fail(std::string(label) + " did not yield a valid sample after 1024 attempts; reduce its SD.");
    }
};

template<class T> DistributionSummary describe(const std::vector<T>& values) {
    DistributionSummary out;
    if (values.empty()) return out;
    out.min = out.max = static_cast<double>(values.front());
    double m2 = 0;
    std::size_t count = 0;
    for (const auto value : values) {
        const double x = static_cast<double>(value);
        out.min = std::min(out.min,x);
        out.max = std::max(out.max,x);
        const double delta = x-out.mean;
        out.mean += delta / static_cast<double>(++count);
        m2 += delta*(x-out.mean);
    }
    out.sd = std::sqrt(std::max(0.0,m2 / static_cast<double>(count)));
    return out;
}

void check_settings(const NetworkGeneration& g) {
    if (g.excitatory.count < 0 || g.inhibitory.count < 0 ||
        g.excitatory.count > MaxNeurons || g.inhibitory.count > MaxNeurons ||
        g.excitatory.count + g.inhibitory.count < 1 ||
        g.excitatory.count + g.inhibitory.count > MaxNeurons)
        fail("population counts must be nonnegative, with 1.." + std::to_string(MaxNeurons) + " neurons in total.");
    check_population(g.excitatory,"Excitatory");
    check_population(g.inhibitory,"Inhibitory");
    switch (g.topology) {
        case NetworkTopology::UniformRandom:
        case NetworkTopology::NearestNeighbors:
        case NetworkTopology::DirectedRing:
        case NetworkTopology::NoConnections: break;
        default: fail("unknown topology.");
    }
    if (g.synapse.model != SynapseModel::ExponentialCurrent && g.synapse.model != SynapseModel::TsodyksMarkram)
        fail("choose exponential current or Tsodyks-Markram synapses.");

    // Validate all editable synaptic fields even for an edge-free topology.
    // Endpoints and polarity are generated, so they are not template inputs.
    Config probe;
    probe.neurons.resize(2);
    auto syn = g.synapse;
    syn.source = 0; syn.target = 1; syn.astro = -1;
    syn.weight = .5; syn.stdp = false;
    probe.synapses.push_back(syn);
    const auto error = validate(probe);
    if (!error.empty()) fail(error);
    if (g.synapse.stdp && g.excitatory.count) {
        const double lo = std::max(0.0,g.synapse.weight_min);
        const double hi = std::min(1.0,g.synapse.weight_max);
        if (lo > hi) fail("STDP bounds do not overlap the supported absolute weight range [0,1].");
        check_gaussian(g.excitatory.weight,lo,hi,"Excitatory initial STDP weight");
        if (lo == hi && g.excitatory.weight.sd > 0)
            fail("a zero-width initial STDP weight range requires zero SD.");
    }
}

} // namespace

Config generate_network(const Config& base, const NetworkGeneration& g,
                        bool append, GenerationSummary* summary) {
    check_settings(g);
    if (append) {
        const auto error = validate(base);
        if (!error.empty()) fail("invalid existing experiment: " + error);
    }
    Config c = append ? base : Config{};
    const int offset = static_cast<int>(c.neurons.size());
    const int count = g.excitatory.count + g.inhibitory.count;
    if (offset + count > MaxNeurons)
        fail("the resulting neuron count exceeds " + std::to_string(MaxNeurons) + ".");
    if (!append) {
        c.name = g.name;
        c.duration = g.duration;
        c.sample = g.sample;
        c.seed = g.seed;
    }

    GenerationSummary report;
    report.new_neurons = count;
    report.excitatory_count = g.excitatory.count;
    report.inhibitory_count = g.inhibitory.count;
    report.in_degrees.resize(count,0);
    Sampler random(g.seed);
    c.neurons.reserve(offset+count);
    for (int i=0; i<count; ++i) {
        const bool inhibitory = i >= g.excitatory.count;
        const auto& p = inhibitory ? g.inhibitory : g.excitatory;
        Neuron n;
        n.name = std::string(inhibitory ? "I " : "E ") + std::to_string(offset+i+1);
        n.model = NeuronModel::Izhikevich;
        n.inhibitory = inhibitory;
        n.px = random.position(random.engine);
        n.py = random.position(random.engine);
        n.izh_a = random.bounded(p.a,.0001,1,"a");
        n.izh_b = random.bounded(p.b,-2,2,"b");
        n.izh_c = random.bounded(p.c,-100,0,"c");
        n.izh_d = random.bounded(p.d,-100,100,"d");
        n.voltage = n.izh_c;
        n.izh_u = n.izh_b*n.voltage;
        n.current = n.amplitude = n.frequency = 0;
        n.start = n.stop = n.width = 0;
        n.individual_noise = false;
        n.noise_sigma = 0;
        c.neurons.push_back(std::move(n));
    }

    std::size_t edge_count = 0;
    for (int target=0; target<count; ++target) {
        int degree = 0;
        if (g.topology == NetworkTopology::DirectedRing) {
            degree = count > 1 ? 1 : 0;
        } else if (g.topology != NetworkTopology::NoConnections) {
            const auto& p = target < g.excitatory.count ? g.excitatory : g.inhibitory;
            const double raw = std::round(random.draw(p.in_degree));
            if (!std::isfinite(raw)) fail("in-degree draw overflowed; reduce its mean or SD.");
            if (raw < 0) { degree = 0; ++report.degree_clamped; }
            else if (raw > count-1) { degree = count-1; ++report.degree_clamped; }
            else degree = static_cast<int>(raw);
        }
        report.in_degrees[target] = degree;
        edge_count += static_cast<std::size_t>(degree);
    }
    if (edge_count > static_cast<std::size_t>(MaxSynapses)-c.synapses.size())
        fail("the resulting edge count exceeds " + std::to_string(MaxSynapses) + "; reduce the in-degree or population size.");
    c.synapses.reserve(c.synapses.size()+edge_count);
    std::vector<double> weights;
    weights.reserve(edge_count);
    std::vector<int> candidates;
    candidates.reserve(count-1);
    for (int target=0; target<count; ++target) {
        const int degree = report.in_degrees[target];
        if (!degree) continue;
        candidates.clear();
        if (g.topology == NetworkTopology::DirectedRing) {
            candidates.push_back((target+count-1)%count);
        } else {
            for (int source=0; source<count; ++source)
                if (source != target) candidates.push_back(source);
            if (g.topology == NetworkTopology::UniformRandom) {
                std::shuffle(candidates.begin(),candidates.end(),random.engine);
            } else {
                const auto& t = c.neurons[offset+target];
                auto distance = [&](int i) {
                    const auto& n = c.neurons[offset+i];
                    const double dx = n.px-t.px, dy = n.py-t.py;
                    return dx*dx+dy*dy;
                };
                std::sort(candidates.begin(),candidates.end(),[&](int a,int b) {
                    const double da=distance(a), db=distance(b);
                    return da == db ? a < b : da < db;
                });
            }
        }
        for (int k=0; k<degree; ++k) {
            const int source = candidates[k];
            const bool inhibitory = source >= g.excitatory.count;
            const auto& p = inhibitory ? g.inhibitory : g.excitatory;
            Synapse syn = g.synapse;
            syn.source = offset+source;
            syn.target = offset+target;
            syn.astro = -1;
            syn.enabled = true;
            syn.stdp = g.synapse.stdp && !inhibitory;
            const double lo = syn.stdp ? std::max(0.0,syn.weight_min) : 0;
            const double hi = syn.stdp ? std::min(1.0,syn.weight_max) : 1;
            const double weight = random.bounded(p.weight,lo,hi,"absolute weight");
            syn.weight = inhibitory ? -weight : weight;
            syn.reversal = inhibitory ? -80 : 0;
            weights.push_back(weight);
            c.synapses.push_back(std::move(syn));
        }
    }

    // History selection is the only run-setting change permitted on append.
    // Preserve an existing explicit selection; a full history that would exceed
    // the budget falls back to the first eight neurons and eight synapses.
    if (c.record_all && std::isfinite(c.duration) && std::isfinite(c.sample) && c.sample > 0) {
        const size_t rows = planned_samples(c.duration,c.sample);
        if (rows*recorded_dimension(c) > MaxStoredValues) {
            c.record_all = false;
            c.record_neurons.resize(std::min(8,static_cast<int>(c.neurons.size())));
            c.record_synapses.resize(std::min(8,static_cast<int>(c.synapses.size())));
            std::iota(c.record_neurons.begin(),c.record_neurons.end(),0);
            std::iota(c.record_synapses.begin(),c.record_synapses.end(),0);
            report.recording_reduced = true;
        }
    }
    const auto error = validate(c);
    if (!error.empty()) fail(error);
    report.new_synapses = static_cast<int>(edge_count);
    report.parameter_resamples = random.rejected;
    report.degrees = describe(report.in_degrees);
    report.absolute_weights = describe(weights);
    if (summary) *summary = std::move(report);
    return c;
}

}
