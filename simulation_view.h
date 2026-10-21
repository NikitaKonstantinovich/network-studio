#pragma once
#include "network.h"
#include <memory>
#include <limits>

namespace studio {
struct RunnerMetrics {
    bool running=false;
    bool archive_active=false,archive_required=false,archive_deliveries=false;
    double memory_samples_from=0,memory_events_from=0,memory_stimuli_from=0,disk_from=0,disk_until=0;
    std::string archive_id,archive_status;
    // Runtime execution setting, independent of the saved numerical state.
    // Effective count is 1 when the solver selects its serial fallback.
    int compute_threads=1,effective_compute_threads=1;
    // Active wall time and model/wall rate; compute counts step() alone. ETA is
    // approximate: current manual stage end, otherwise configured duration.
    double elapsed_seconds=0,compute_seconds=0,model_per_second=0,eta_seconds=0,publish_seconds=0;
    uint64_t publish_count=0;
    size_t command_queue_size=0,command_queue_bytes=0,snapshot_bytes=0,history_doubles=0;
};
struct PendingDeliverySummary {
    size_t count=0;
    size_t size() const {return count;}
    bool empty() const {return count==0;}
};
struct SampleColumnDelta {size_t index=0;bool weight=false;std::vector<double> values;};
struct SampleDelta {
    uint64_t begin=0,end=0,append_from=0;
    size_t value_columns=0,weight_columns=0;
    std::vector<double> time;
    std::vector<SampleColumnDelta> columns;
};
template<class T> struct JournalDelta {
    // Remove older records, replace the last published time group, then append.
    // Replacing the group handles equal-time events without deduplication.
    double keep_from=0,replace_from=0,available_after=0,last_time=-std::numeric_limits<double>::infinity();
    std::vector<T> entries;
};
template<class T> struct ActivityDelta {
    uint64_t begin=0,end=0,append_from=0;
    std::vector<T> entries;
};
// Published only after a completed macro step (or a paused controller action).
// No field points into Simulator. Only one unconsumed packet exists in runner.
struct SimulationSnapshot {
    uint64_t generation=0,sequence=0,base_sequence=0,steps=0,last_command_id=0;
    double time=0;
    IntegrationStatistics integration;
    bool done=false,truncated=false;
    std::string error;
    RunnerMetrics metrics;
    std::shared_ptr<const Config> config;
    std::shared_ptr<const ExperimentRuntime> experiment;
    AnimatRuntime animat;
    State state;
    std::vector<StdpState> plasticity;
    std::vector<uint64_t> spikes,releases;
    PendingDeliverySummary pending_deliveries;
    SampleDelta samples;
    JournalDelta<Event> events;
    JournalDelta<Delivery> deliveries;
    JournalDelta<StimulusEvent> stimuli;
    std::shared_ptr<const std::vector<SharedPulse>> shared_pulses;
    double shared_after=0;
    ActivityDelta<Delivery> activity_deliveries;
    ActivityDelta<Event> activity_spikes;
    double activity_available_after=0;
    std::shared_ptr<const std::vector<StimulatorCommand>> stimulus_commands;
    std::shared_ptr<const std::vector<StimulatorChange>> stimulus_changes;
};
// GUI-owned retained display data. Apply packets before a frame and pass const
// references during the entire frame. Scientific exports use the worker, not this.
class SimulationView {
public:
    IntegrationStatistics integration;
    Config config;
    AnimatRuntime animat;
    State state;
    History history;
    std::vector<StdpState> plasticity;
    std::vector<uint64_t> spikes,releases;
    PendingDeliverySummary pending_deliveries;
    ExperimentRuntime experiment;
    RecentActivity activity;
    std::vector<StimulatorCommand> stimulus_commands;
    std::vector<StimulatorChange> stimulus_changes;
    uint64_t generation=0,sequence=0,steps=0,sample_begin=0,last_command_id=0;
    double time=0;
    std::string error;
    bool truncated=false;
    RunnerMetrics metrics;
    bool apply(const SimulationSnapshot& snapshot);
    bool apply(const std::shared_ptr<const SimulationSnapshot>& snapshot) {return snapshot&&apply(*snapshot);}
    bool done() const {return done_;}
    double historical_input(int neuron,double at) const;
private:
    bool done_=false;
    std::shared_ptr<const Config> config_version_;
    std::shared_ptr<const ExperimentRuntime> experiment_version_;
    std::shared_ptr<const std::vector<SharedPulse>> shared_version_;
    std::shared_ptr<const std::vector<StimulatorCommand>> commands_version_;
    std::shared_ptr<const std::vector<StimulatorChange>> changes_version_;
};
// Common adapter for existing numerical/GUI tests and the production display.
// It must never be constructed from the worker-owned Simulator on the GUI thread.
struct SimulationReadView {
    const Config& config;
    const State& state;
    const AnimatRuntime& animat;
    const History& history;
    const std::vector<StdpState>& plasticity;
    const std::vector<uint64_t>& spikes;
    const std::vector<uint64_t>& releases;
    const ExperimentRuntime& experiment;
    const RecentActivity& activity;
    const std::vector<StimulatorCommand>& stimulus_commands;
    const std::vector<StimulatorChange>& stimulus_changes;
    const std::string& error;
    PendingDeliverySummary pending_deliveries;
    uint64_t steps=0,generation=0;
    double time=0;
    const void* identity=nullptr;
    bool truncated=false;
    SimulationReadView(const Simulator& simulation);
    SimulationReadView(const SimulationView& view);
    bool done() const {return done_;}
    double historical_input(int neuron,double at) const;
private:
    bool done_=false;
};
}
