#pragma once
#include "vendor/systems/neuro_astro/model.cuh"
#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include "numerics/integrator.h"

namespace studio {
namespace na = neuro_astro;
constexpr int MaxNeurons = 1000, MaxAstrocytes = 8, MaxSynapses = 50000, MaxGaps = 28;
constexpr int MaxState = 4*MaxNeurons + 4*MaxSynapses + 7*MaxAstrocytes;
constexpr size_t MaxSamples = 1000000, MaxStoredValues = 32000000, MaxEvents = 2000000;
constexpr size_t MaxPendingDeliveries = 1000000, MaxDeliveries = 2000000;
using State = std::vector<double>;
// Only these parameters are shared. Stimuli, synapses, initial conditions and
// feedback have their own per-node/per-edge fields in the configurable model.
inline constexpr int SharedParameters[] = {
    na::C_m,na::g_Na,na::g_K,na::g_L,na::E_Na,na::E_K,na::E_L,na::V_spike,
    na::Y_release,na::Omega_Y,na::O_N,na::Omega_N,na::O_beta,na::O_delta,na::kappa_delta,na::K_delta,
    na::O_3K,na::K_D,na::K_3K,na::Omega_5P,na::Omega_C,na::Omega_L,na::O_P,na::K_P,
    na::C_T,na::rho_A,na::O_2,na::d1,na::d2,na::d3,na::d5,na::U_A,na::tau_A,na::G_release,
    na::Omega_G,na::O_G,na::Omega_S,na::K_SIC,na::E_syn
};
enum class NeuronModel { HodgkinHuxley = 0, Izhikevich = 1 };
enum class SynapseModel { ConductanceSTP = 0, ExponentialCurrent = 1, TsodyksMarkram = 2 };
enum class StdpRule { Additive = 0, Multiplicative = 1, Triplet = 2 };
enum class StdpTiming { Arrival = 0, Emission = 1 };
struct PulseInterval { double start = 0, stop = .003; };
struct Neuron {
    std::string name = "Neuron";
    double px = 0.5, py = 0.5, voltage = -65;
    double current = 0, amplitude = 10, frequency = 10, start = 1, stop = 20, width = 0.005;
    // Model-specific fields are appended to preserve aggregate presets.
    // Izhikevich uses [v,u,0,0]; HH uses [v,m,h,n].
    NeuronModel model = NeuronModel::HodgkinHuxley;
    double izh_a = .02, izh_b = .2, izh_c = -65, izh_d = 8, izh_peak = 30, izh_u = -13;
    bool equilibrium_u = true, individual_noise = false;
    double noise_sigma = 0;
    // Network-generator/display type; current polarity remains an edge property.
    bool inhibitory = false;
    // Optional ordered, non-overlapping half-open intervals within each period,
    // relative to start. Empty preserves the legacy single pulse of width.
    std::vector<PulseInterval> pulse_intervals;
};
struct Astrocyte {
    std::string name = "Astrocyte";
    double px = 0.5, py = 0.8, ip3 = 0.16, calcium = 0.1, h = 0.9;
    double threshold = 0.2, alpha = 0.8, sic = 0.02;
    bool feedback = true;
};
struct Synapse {
    int source = 0, target = 1, astro = -1;
    double conductance = 0.25, reversal = 0, release = 0.25;
    double tau_syn = 0.005, tau_f = 0.2, tau_d = 0.3, spill = 0.01;
    bool enabled = true;
    SynapseModel model = SynapseModel::ConductanceSTP;
    double weight = 1, gain = 40;
    // All-to-all pair STDP; amplitudes are increments (additive) or rates per
    // distance from the weight bound (multiplicative). Time constants are in s.
    bool stdp = false;
    StdpRule stdp_rule = StdpRule::Additive;
    double a_plus = .01, a_minus = .012, tau_plus = .020, tau_minus = .020;
    double weight_min = 0, weight_max = 2;
    // Minimal multiplicative triplet LTP uses the OLD postsynaptic slow trace.
    // Pair LTD is unchanged; no additive pair-LTP term in this rule.
    double tau_slow = .100;
    double delay = 0; // Fixed conduction delay in seconds, independent of layout.
    StdpTiming stdp_timing = StdpTiming::Arrival;
};
struct StdpState { double weight = 1, pre_trace = 0, post_trace = 0, post_slow_trace = 0; };
// Decay old traces, apply both updates together, then insert new spikes.
// Spikes sharing a timestamp have no mutual zero-lag contribution.
void update_stdp(const Synapse& synapse, StdpState& state, uint64_t pre, uint64_t post, double dt);
void update_synaptic_stdp(const Synapse& synapse, StdpState& state,
                         uint64_t emitted, uint64_t arrived, uint64_t post, double dt);
struct Gap {
    int a = 0, b = 1;
    double ip3 = 0.05, calcium = 0;
};
struct Config {
    std::string name = "Two inputs / one astrocyte";
    std::vector<Neuron> neurons;
    std::vector<Astrocyte> astrocytes;
    std::vector<Synapse> synapses;
    std::vector<Gap> gaps;
    std::array<double, na::COUNT+1> parameters{};
    double dt = 0.000025, duration = 10, sample = 0.00025;
    double noise_sigma = 0, noise_interval = .001;
    uint32_t seed = 42;
    bool record_all = true;
    std::vector<int> record_neurons, record_synapses;
    Config();
};
Config preset(int index);
std::string validate(const Config& c);
void remove_neuron(Config& c, int index);
void remove_astrocyte(Config& c, int index);
int dimension(const Config& c);
bool records_neuron(const Config& c,int index);
bool records_synapse(const Config& c,int index);
// Number of stored scalar columns, including selected synaptic weights.
int recorded_dimension(const Config& c);
// Includes t=0 and a final off-grid sample; saturates at MaxSamples+1.
size_t planned_samples(double duration, double interval);
int syn_offset(const Config& c, int index);
int astro_offset(const Config& c, int index);
State initial_state(const Config& c);
// Deterministic sample-and-hold Gaussian current. Individual sigma overrides
// the global sigma; streams are independent per neuron and repeat after reset.
double external_current(const Config& c, int neuron, double time);
// First strictly later discontinuity of the sample-and-hold input, or limit.
double next_input_boundary(const Config& c,double time,double limit);
void derivative(const Config& c, const State& y, double midpoint, State& dy,
                const std::vector<StdpState>* plasticity = nullptr,
                bool omit_synapse_kinetics = false);
struct Event { double time; int index; bool astro; };
struct Delivery { double t_emit, t_arrival; int synapse; };
// Total deterministic order; usable both as a min-heap comparator and for export.
struct DeliveryLater {
    bool operator()(const Delivery& a,const Delivery& b) const {
        if(a.t_arrival!=b.t_arrival)return a.t_arrival>b.t_arrival;
        if(a.synapse!=b.synapse)return a.synapse>b.synapse;
        return a.t_emit>b.t_emit;
    }
};
struct History {
    std::vector<double> time;
    std::vector<std::vector<double>> values;
    std::vector<std::vector<double>> weights;
    std::vector<Event> events;
    std::vector<Delivery> deliveries;
};
class Simulator {
public:
    Config config;
    State state{};
    History history;
    // Runtime learning state; config.synapses retains the initial experiment.
    std::vector<StdpState> plasticity;
    std::vector<uint64_t> spikes, releases;
    // Min-heap with DeliveryLater; includes arrivals beyond config.duration.
    // Protocol/state edits require a reset; do not mutate this during a run.
    std::vector<Delivery> pending_deliveries;
    uint64_t steps = 0;
    double time = 0;
    std::string error;
    explicit Simulator(const Config& c);
    bool step();
    bool done() const;
    void record();
    // Absolute final time; extending does not reset learning or history.
    void extend_duration(double end_time);
private:
    friend struct CheckpointAccess;
    bool stepping=false, checkpoint_boundary=true;
    bool step_impl();
    numerical::Flow flow;
    std::vector<numerical::Threshold> thresholds;
    double held_input_time=0, suggested_step=0;
    void exact_flow(const State& old,double dt,State& result) const;
    double neuron_threshold_sample(const State& old,double h,int coordinate) const;
    std::vector<std::vector<int>> incoming;
    double kinetic_step_limit() const;
    bool physical(const State& value) const;
    std::vector<std::vector<int>> outgoing;
    std::vector<int> recorded_state, recorded_weights;
    uint64_t next_sample = 1;
};
}
