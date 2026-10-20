#pragma once
#include "simulation_view.h"
#include "analysis.h"
#include "weight_analysis.h"
#include <filesystem>
#include <future>
#include <memory>
#include <optional>

namespace studio {
struct RunnerCommand {
    enum class Kind {Run,Pause,Advance,RunTo,Reset,LoadConfig,Restore,SaveCheckpoint,Export,Extend,
        EnterStage,ContinueCurrent,Learning,Plan,Analysis,BoundedJournals,Stimulator,MoveStimulator,
        RemoveStimulator,MoveNeuron,MoveAstro,ExportTrained,ExportAnalysis,WriteProfile,ComputeThreads,StartArchive,FlushArchive,CaptureWeights,Barrier};
    Kind kind=Kind::Barrier;
    Config config;
    std::filesystem::path path;
    std::vector<ExperimentStage> stages;
    AnalysisSettings analysis;
    AnalysisInterval analysis_interval;
    studio::Stimulator stimulator;
    double value=0,second=0,third=0;
    // ComputeThreads: requested worker count including controller; 0 = Auto.
    int index=0;
    uint64_t id=0;
    bool flag=false,flag2=false;
    std::string text;
};
struct RunnerResult {
    bool ok=false;
    std::string error;
    uint64_t generation=0,steps=0,id=0,command_id=0,snapshot_sequence=0;
    double time=0,applied_time=0;
    // Normalized parameters actually accepted by the solver (possibly queued).
    std::optional<Stimulator> accepted_stimulator;
    std::shared_ptr<const WeightSnapshot> weights;
};
class SimulationRunner {
public:
    // Admission limits cover queued payload sizes (not reserved capacities or
    // the in-flight command). Display limits count retained logical entries,
    // not process RSS. Solver history and source-bounded Config/protocol metadata
    // remain separate; immutable metadata is shared until its version changes.
    static constexpr size_t MaxCommands=64,MaxCommandBytes=32*1024*1024;
    static constexpr size_t MaxDisplayDoubles=2000000,MaxDisplaySamples=20000,MaxDisplayJournal=100000;
    explicit SimulationRunner(const Config& config);
    ~SimulationRunner();
    SimulationRunner(const SimulationRunner&)=delete;
    SimulationRunner& operator=(const SimulationRunner&)=delete;
    // Nonblocking admission. Busy/stopping returns an already completed error.
    std::shared_future<RunnerResult> submit(RunnerCommand command);
    // Single consumer: consume every returned delta, in order, via apply(). A
    // take acknowledges its cursor even if the caller retains the immutable
    // packet. Keeping arbitrary old packets is outside the retention budget.
    std::shared_ptr<const SimulationSnapshot> take_snapshot();
    void set_display_hz(double hz);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
