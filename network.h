#pragma once
#include "vendor/systems/neuro_astro/model.cuh"
#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <memory>
#include "numerics/integrator.h"
#include "numerics/worker_pool.h"

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
void update_stdp(const Synapse& synapse, StdpState& state, uint64_t pre, uint64_t post, double dt, double rate=1);
void update_synaptic_stdp(const Synapse& synapse, StdpState& state,
                         uint64_t emitted, uint64_t arrived, uint64_t post, double dt, double rate=1);
// Derived coefficients only. Trace arithmetic and update order stay identical.
class StdpDecayCache {
    double dt_=0,plus_=0,minus_=0,slow_=0;
    bool valid_=false,triplet_=false;
    std::array<double,3> factors_{};
public:
    const std::array<double,3>& factors(const Synapse& synapse,double dt);
};
void update_stdp_cached(const Synapse& synapse,StdpState& state,uint64_t pre,uint64_t post,
                        double dt,double rate,StdpDecayCache& cache);
struct Gap {
    int a = 0, b = 1;
    double ip3 = 0.05, calcium = 0;
};
struct ModelSpace { double width=1, height=1; std::string unit="model_unit"; };
struct GenerationMetadata { std::string settings_json; int first=0, count=0; };
enum class StimulatorFilter { All, Excitatory, Inhibitory };
struct Stimulator {
    uint64_t id=0;
    std::string name="Stimulator";
    bool enabled=true, spatial=false;
    double x=.5,y=.5,radius=.1,first_delay=0;
    StimulatorFilter filter=StimulatorFilter::All;
    std::vector<int> recipients;
    // Only pulse fields are used; shares the established half-open protocol.
    Neuron signal;
};
struct StimulatorCommand { uint64_t sequence=0; double time=0; Stimulator value; bool restart=false; };
struct SharedPulse { uint64_t stimulator=0,pulse=0; double start=0,end=0,amplitude=0; bool complete=false; int stage=-1; std::vector<int> recipients; };
struct StimulatorLatch { uint64_t id=0,next_pulse=1,active_pulse=0; };
struct StimulatorChange { uint64_t sequence=0; double time=0; int stage=-1; Stimulator value; std::vector<int> recipients; bool restart=false; };
enum class AnalysisPopulation { All, Excitatory, Inhibitory, Selected };
struct AnalysisSettings {
    AnalysisPopulation population=AnalysisPopulation::All;
    std::vector<int> neurons;
    double bin=.01, burst_window=.02, start_rate=100, end_rate=25;
    int min_neurons=2;
    double min_fraction=0, min_gap=.05, burst_rate_window=1;
    int reference_neuron=0;
    uint64_t reference_stimulator=0; // 0 selects the legacy neuron pulse train.
    double response_min=0, response_max=.1;
};
struct ExperimentStage {
    std::string name="Stage";
    double duration=1;
    bool learning=true, clear_traces=false;
    double learning_rate=1;
    // Empty group leaves existing protocols unchanged. Modified pulse trains
    // restart at entry+first_pulse; an old active pulse is truncated at entry.
    std::vector<int> neurons;
    bool stimulus_enabled=false;
    double amplitude=30, frequency=1, width=.001, first_pulse=0;
    std::vector<uint64_t> stimulator_ids;
};
struct ProtocolVersion {
    double time=0, end=0;
    std::string name="Initial protocol";
    bool learning=true, cleared=false;
    double learning_rate=1;
    std::vector<Neuron> neurons;
    std::vector<Stimulator> stimulators; // Spatial recipients captured at this version.
};
struct ExperimentRuntime {
    bool learning=true, automatic=false, stage_active=false;
    double learning_rate=1, stage_end=0;
    int stage_index=-1;
    std::string origin;
    std::vector<ProtocolVersion> protocols;
};
struct StimulusEvent { double time=0, amplitude=0; int neuron=0, protocol=0; };
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
    ModelSpace space;
    std::vector<GenerationMetadata> generation;
    std::vector<ExperimentStage> stages;
    AnalysisSettings analysis;
    // Rolling journals are separate from the live delivery queue. Legacy JSON
    // retains full journals; new long runs explicitly opt into this policy.
    bool bounded_journals=false;
    size_t journal_capacity=MaxEvents;
    std::vector<Stimulator> stimulators;
    uint64_t next_stimulator_id=1;
    double activity_horizon=1;
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
// A disposable per-neuron cache keyed by the absolute noise interval and all
// noise parameters. Pulse/shared input is always evaluated at the requested time.
class NoiseCache {
    struct Entry { uint64_t bin=0; uint32_t seed=0; double sigma=0,interval=0,value=0; bool valid=false; };
    std::vector<Entry> entries;
public:
    double current(const Config& c,int neuron,double time);
};
double pulse_current(const Neuron& neuron,double time);
double next_pulse_boundary(const Neuron& neuron,double time,double limit);
Neuron stimulator_signal(const Stimulator& s);
std::vector<int> stimulator_recipients(const Config& c,const Stimulator& s);
double shared_current(const Config& c,int neuron,double time);
std::string validate_stimulators(const Config& c);
double external_current_with_protocol(const Config& c,const Neuron& neuron,int index,double time);
std::string validate_experiment(const Config& c);
// First strictly later discontinuity of the sample-and-hold input, or limit.
double next_input_boundary(const Config& c,double time,double limit);
void derivative(const Config& c, const State& y, double midpoint, State& dy,
                const std::vector<StdpState>* plasticity = nullptr,
                bool omit_synapse_kinetics = false,const std::vector<double>* held_inputs=nullptr);
struct Event { double time; int index; bool astro; };
struct Delivery { double t_emit, t_arrival; int synapse; };
struct RecentActivity {
    std::deque<Delivery> deliveries;
    std::deque<Event> spikes;
    uint64_t delivery_begin=0,spike_begin=0;
    double available_after=0;
};
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
    std::vector<StimulusEvent> stimuli;
    std::vector<SharedPulse> shared_pulses;
    double shared_after=0;
    double events_after=0, deliveries_after=0, stimuli_after=0;
    std::vector<uint64_t> dropped_spikes,dropped_releases,dropped_deliveries;
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
    ExperimentRuntime experiment;
    RecentActivity activity;
    std::vector<StimulatorCommand> stimulus_commands;
    std::vector<StimulatorChange> stimulus_changes;
    std::vector<StimulatorLatch> stimulus_latches;
    uint64_t next_stimulus_command=1;
    uint64_t steps = 0;
    double time = 0;
    std::string error;
    explicit Simulator(const Config& c);
    // Runtime execution policy, deliberately excluded from checkpoints/Config.
    // 0=Auto; compute::ThreadModes includes the controlling solver thread.
    void set_compute_threads(int count);
    int compute_threads() const {return requested_threads;}
    int effective_compute_threads() const {return pool?pool->count():1;}
    bool step();
    bool done() const;
    void record();
    // Absolute final time; extending does not reset learning or history.
    void extend_duration(double end_time);
    void enter_stage(size_t index,bool automatic=false);
    void continue_current();
    void set_learning(bool enabled,double rate,bool clear=false);
    double historical_input(int neuron,double time) const;
    Config learned_config() const;
    void fork_experiment(const std::string& origin);
    // Safe controller API. Frequency changes preserve the current cycle and
    // apply at its next boundary unless restart is explicitly requested.
    double schedule_stimulator(Stimulator value,double requested_time,bool restart=false);
    void replay_stimulators(std::vector<StimulatorCommand> commands); // recorded absolute times, no phase conversion
    double move_stimulator(uint64_t id,double x,double y,double requested_time);
    void remove_stimulator(uint64_t id);
    void move_neuron(int index,double px,double py);
private:
    friend struct CheckpointAccess;
    bool stepping=false, checkpoint_boundary=true;
    bool step_impl();
    numerical::Flow flow;
    std::vector<numerical::Threshold> thresholds;
    double held_input_time=0, suggested_step=0;
    std::vector<double> held_inputs;
    NoiseCache noise_cache;
    StdpDecayCache stdp_decay_cache;
    struct alignas(64) WorkerStdpCache {StdpDecayCache cache;};
    std::array<WorkerStdpCache,compute::MaxThreads> worker_stdp_caches;
    int requested_threads=1;
    std::unique_ptr<numerical::WorkerPool> pool;
    bool parallel_eligible() const;
    void prepare_workers();
    // Derived, disposable views and scratch: never serialized. Rebuild views
    // every macro step so same-size parameter edits cannot leave stale data.
    // Views contain indices/values, not pointers into movable Config vectors.
    struct CurrentEdge {
        double scale,reversal;
        int synapse,target,activity;
        bool conductance;
    };
    struct KineticRange {
        int begin,end;
        SynapseModel model;
        double tau_f,tau_d,tau_syn;
    };
    bool compact_network=false,current_edges_only=false,izh_only=false;
    std::vector<CurrentEdge> current_edges;
    std::vector<CurrentEdge> target_edges;
    std::vector<size_t> target_edge_offsets;
    std::vector<size_t> target_edge_cursor;
    std::vector<KineticRange> kinetic_ranges;
    struct KineticFactors {double facilitation,recovery,active,transfer;};
    mutable std::vector<KineticFactors> kinetic_factors;
    std::vector<double> rhs_input;
    void prepare_solver_views();
    void compact_derivative(const State& value,State& result);
    numerical::Flow projection_flow;
    State projection_initial;
    struct ProjectionScratch {numerical::Flow flow;State initial;};
    std::array<ProjectionScratch,compute::MaxThreads> worker_projection;
    double projected_threshold(const State& old,double h,int coordinate,numerical::Flow& local,State& initial);
    std::vector<int> event_group;
    std::vector<bool> event_fired,event_released;
    std::vector<Delivery> event_deliveries,event_created;
    std::vector<size_t> event_inspect;
    std::vector<uint64_t> event_arrivals;
    void exact_flow(const State& old,double dt,State& result) const;
    double neuron_threshold_sample(const State& old,double h,int coordinate);
    std::vector<std::vector<int>> incoming;
    double kinetic_step_limit() const;
    bool physical(const State& value) const;
    double parallel_error_norm(const State& old,const State& next,const State& full) const;
    std::vector<std::vector<int>> outgoing;
    std::vector<int> recorded_state, recorded_weights;
    uint64_t next_sample = 1;
    void append_protocol(const std::string& name,bool cleared=false);
    void log_stimuli(double at);
    void trim_journals(size_t events,size_t deliveries);
    void apply_stimulus_commands();
    void synchronize_stimulators(bool close_only=false);
    void capture_activity(const std::vector<Delivery>& deliveries,const std::vector<bool>& fired,double at);
};
}
