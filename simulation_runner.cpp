#include "simulation_runner.h"
#include "io.h"
#include "analysis.h"
#include "history_archive.h"
#include "performance/profile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace studio {
namespace {
using Clock=std::chrono::steady_clock;
double seconds(Clock::duration d){return std::chrono::duration<double>(d).count();}
size_t neuron_bytes(const Neuron& n){return sizeof(n)+n.name.size()+n.pulse_intervals.size()*sizeof(PulseInterval);}
size_t stimulator_bytes(const Stimulator& s){return sizeof(s)+s.name.size()+s.recipients.size()*sizeof(int)+neuron_bytes(s.signal);}
size_t stages_bytes(const std::vector<ExperimentStage>& stages){size_t n=stages.size()*sizeof(ExperimentStage);for(const auto& s:stages)n+=s.name.size()+s.neurons.size()*sizeof(int)+s.stimulator_ids.size()*sizeof(uint64_t);return n;}
size_t config_bytes(const Config& c) {
    size_t n=sizeof(c)+c.name.size()+c.synapses.size()*sizeof(Synapse)+c.gaps.size()*sizeof(Gap)+c.space.unit.size();
    for(const auto& v:c.neurons)n+=neuron_bytes(v);
    for(const auto& v:c.astrocytes)n+=sizeof(v)+v.name.size();
    for(const auto& v:c.stimulators)n+=stimulator_bytes(v);
    for(const auto& v:c.generation)n+=sizeof(v)+v.settings_json.size();
    return n+stages_bytes(c.stages)+(c.record_neurons.size()+c.record_synapses.size()+c.analysis.neurons.size())*sizeof(int);
}
size_t command_bytes(const RunnerCommand& c){return sizeof(c)+config_bytes(c.config)+stages_bytes(c.stages)+stimulator_bytes(c.stimulator)+c.analysis.neurons.size()*sizeof(int)+c.text.size()+c.path.native().size()*sizeof(std::filesystem::path::value_type);}
size_t experiment_bytes(const ExperimentRuntime& e){size_t n=sizeof(e)+e.origin.size();for(const auto& p:e.protocols){n+=sizeof(p)+p.name.size();for(const auto& v:p.neurons)n+=neuron_bytes(v);for(const auto& v:p.stimulators)n+=stimulator_bytes(v);}return n;}
template<class T,class Time> void journal_delta(const std::vector<T>& source,JournalDelta<T>& out,double previous,double available,Time at,bool& truncated) {
    size_t start=source.size()>SimulationRunner::MaxDisplayJournal?source.size()-SimulationRunner::MaxDisplayJournal:0;
    // Do not claim availability of half an equal-time event group.
    while(start&&start<source.size()&&at(source[start])==at(source[start-1]))++start;
    out.available_after=available;
    if(start){out.available_after=std::max(out.available_after,std::nextafter(at(source[start-1]),INFINITY));truncated=true;}
    if(available>0)truncated=true;
    if(start==source.size()){out.keep_from=INFINITY;out.replace_from=0;return;}
    out.keep_from=at(source[start]);out.replace_from=std::max(previous,out.keep_from);out.last_time=at(source.back());
    auto first=std::lower_bound(source.begin()+start,source.end(),out.replace_from,[&](const auto& v,double t){return at(v)<t;});
    out.entries.assign(first,source.end());
}
template<class T,class Time> void activity_delta(const std::deque<T>& source,uint64_t source_begin,ActivityDelta<T>& out,uint64_t previous,double& available,Time at,bool& truncated) {
    const size_t offset=source.size()>SimulationRunner::MaxDisplayJournal?source.size()-SimulationRunner::MaxDisplayJournal:0;
    out.begin=source_begin+offset;out.end=source_begin+source.size();out.append_from=std::min(out.end,std::max(out.begin,previous));
    if(offset){available=std::max(available,std::nextafter(at(source[offset-1]),INFINITY));truncated=true;}
    out.entries.assign(source.begin()+size_t(out.append_from-source_begin),source.end());
}
bool same_experiment(const ExperimentRuntime& a,const ExperimentRuntime& b) {
    return a.learning==b.learning&&a.automatic==b.automatic&&a.stage_active==b.stage_active&&a.learning_rate==b.learning_rate&&
        a.stage_end==b.stage_end&&a.stage_index==b.stage_index&&a.origin==b.origin&&a.protocols.size()==b.protocols.size()&&
        (a.protocols.empty()||a.protocols.back().end==b.protocols.back().end);
}
}
struct SimulationRunner::Impl {
    struct Request {RunnerCommand command;std::promise<RunnerResult> promise;uint64_t id=0;size_t bytes=0;};
    std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<Request>> queue;
    size_t queue_bytes=0;
    bool stopping=false;
    uint64_t next_command=1;
    std::shared_ptr<const SimulationSnapshot> mailbox,consumed;
    std::atomic<double> display_hz{10};
    std::thread worker;
    // Everything below is accessed only by worker, including all Simulator APIs.
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<Request> target_request;
    std::string runner_error;
    double target_time=0;
    bool running=false,config_dirty=true;
    uint64_t generation=1,sequence=0,last_command=0;
    double compute_seconds=0,publish_seconds=0,active_seconds=0,model_start=0;
    uint64_t publish_count=0;
    Clock::time_point active_begin=Clock::now(),next_publish=Clock::now();
    std::shared_ptr<const Config> config_cache;
    std::shared_ptr<const ExperimentRuntime> experiment_cache;
    std::shared_ptr<const std::vector<SharedPulse>> shared_cache;
    std::shared_ptr<const std::vector<StimulatorCommand>> commands_cache;
    std::shared_ptr<const std::vector<StimulatorChange>> changes_cache;

    // Start in the constructor body: every worker-owned field must exist before
    // the new thread can observe this object.
    explicit Impl(Config initial){worker=std::thread([this,c=std::move(initial)]() mutable {loop(std::move(c));});}
    ~Impl(){shutdown();}
    void shutdown() {
        std::lock_guard<std::mutex> join_lock(join_mutex);
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;}
        wake.notify_all();if(worker.joinable())worker.join();
    }
    RunnerResult result(bool ok=true,const std::string& error={}) const {
        RunnerResult r;r.ok=ok;r.error=error;r.generation=generation;
        if(sim){r.steps=sim->steps;r.time=r.applied_time=sim->time;}return r;
    }
    void finish(std::unique_ptr<Request> request,RunnerResult r) {
        if(!request)return;r.command_id=request->id;request->promise.set_value(std::move(r));
    }
    void set_running(bool value) {
        if(value==running)return;
        if(running)active_seconds+=seconds(Clock::now()-active_begin);else active_begin=Clock::now();
        running=value;
    }
    void cancel_target(const std::string& reason) {if(target_request)finish(std::move(target_request),result(false,reason));}
    void replace(std::unique_ptr<Simulator> candidate) {
        // Preserve the execution preference, including Auto, without changing
        // checkpoint state or replacing the current experiment on setup failure.
        candidate->set_compute_threads(sim?sim->compute_threads():1);
        if(sim)flush_archive(*sim);
        cancel_target("Advance cancelled by replacement of the experiment.");set_running(false);sim=std::move(candidate);
        ++generation;sequence=0;compute_seconds=publish_seconds=active_seconds=0;publish_count=0;model_start=sim->time;
        runner_error.clear();
        config_dirty=true;config_cache.reset();experiment_cache.reset();shared_cache.reset();commands_cache.reset();changes_cache.reset();
        std::shared_ptr<const SimulationSnapshot> retired_mailbox,retired_consumed;
        {std::lock_guard<std::mutex> lock(mutex);retired_mailbox=std::move(mailbox);retired_consumed=std::move(consumed);}
        profile::reset();
    }
    void cache_metadata() {
        if(config_dirty||!config_cache){config_cache=std::make_shared<const Config>(sim->config);config_dirty=false;}
        if(!experiment_cache||!same_experiment(*experiment_cache,sim->experiment))experiment_cache=std::make_shared<const ExperimentRuntime>(sim->experiment);
        // Existing pulse contents are immutable except end/complete. The journal
        // can erase non-prefix records, therefore this channel is versioned whole.
        const auto& pulses=sim->history.shared_pulses;
        size_t start=pulses.size(),recipients=0;
        while(start&&pulses.size()-start<MaxDisplayJournal&&recipients+pulses[start-1].recipients.size()<=1000000){--start;recipients+=pulses[start].recipients.size();}
        bool same=shared_cache&&shared_cache->size()==pulses.size()-start;
        for(size_t i=0;same&&i<shared_cache->size();++i){const auto& a=(*shared_cache)[i];const auto& b=pulses[start+i];same=a.stimulator==b.stimulator&&a.pulse==b.pulse&&a.end==b.end&&a.complete==b.complete;}
        if(!same)shared_cache=std::make_shared<const std::vector<SharedPulse>>(pulses.begin()+start,pulses.end());
        bool same_commands=commands_cache&&commands_cache->size()==sim->stimulus_commands.size();
        for(size_t i=0;same_commands&&i<commands_cache->size();++i)same_commands=(*commands_cache)[i].sequence==sim->stimulus_commands[i].sequence;
        if(!same_commands)commands_cache=std::make_shared<const std::vector<StimulatorCommand>>(sim->stimulus_commands);
        if(!changes_cache||changes_cache->size()!=sim->stimulus_changes.size())changes_cache=std::make_shared<const std::vector<StimulatorChange>>(sim->stimulus_changes);
    }
    bool publish(bool force) {
        std::shared_ptr<const SimulationSnapshot> base;
        size_t queued=0,queued_bytes=0;
        {std::lock_guard<std::mutex> lock(mutex);if(stopping)return false;if(mailbox&&!force)return false;base=consumed;queued=queue.size();queued_bytes=queue_bytes;}
        if(base&&base->generation!=generation)base.reset();
        const auto started=Clock::now();cache_metadata();auto out=std::make_shared<SimulationSnapshot>();
        out->generation=generation;out->base_sequence=base?base->sequence:0;out->last_command_id=last_command;
        out->steps=sim->steps;out->time=sim->time;out->done=sim->done()||!runner_error.empty();out->error=runner_error.empty()?sim->error:runner_error;
        out->config=config_cache;out->experiment=experiment_cache;out->state=sim->state;out->plasticity=sim->plasticity;
        out->spikes=sim->spikes;out->releases=sim->releases;out->pending_deliveries.count=sim->pending_deliveries.size();
        out->stimulus_commands=commands_cache;out->stimulus_changes=changes_cache;
        const auto& h=sim->history;auto& samples=out->samples;
        size_t recorded=1;for(const auto& v:h.values)if(!v.empty())++recorded;for(const auto& v:h.weights)if(!v.empty())++recorded;
        const size_t capacity=std::max<size_t>(1,std::min(MaxDisplaySamples,MaxDisplayDoubles/recorded));
        samples.end=h.sample_begin+h.time.size();samples.begin=h.sample_begin+(h.time.size()>capacity?h.time.size()-capacity:0);
        samples.append_from=std::min(samples.end,std::max(samples.begin,base?base->samples.end:0));
        samples.value_columns=h.values.size();samples.weight_columns=h.weights.size();
        samples.time.assign(h.time.begin()+size_t(samples.append_from-h.sample_begin),h.time.end());
        for(int weights=0;weights<2;++weights){const auto& source=weights?h.weights:h.values;
            for(size_t i=0;i<source.size();++i)if(!source[i].empty()){
                SampleColumnDelta column;column.index=i;column.weight=weights!=0;
                column.values.assign(source[i].begin()+size_t(samples.append_from-h.sample_begin),source[i].end());samples.columns.push_back(std::move(column));
            }
        }
        out->truncated=samples.begin>0;
        const double none=-INFINITY;
        journal_delta(h.events,out->events,base?base->events.last_time:none,h.events_after,[](const Event& e){return e.time;},out->truncated);
        journal_delta(h.deliveries,out->deliveries,base?base->deliveries.last_time:none,h.deliveries_after,[](const Delivery& e){return e.t_arrival;},out->truncated);
        journal_delta(h.stimuli,out->stimuli,base?base->stimuli.last_time:none,h.stimuli_after,[](const StimulusEvent& e){return e.time;},out->truncated);
        out->shared_pulses=shared_cache;out->shared_after=h.shared_after;
        if(shared_cache->size()<h.shared_pulses.size()){out->shared_after=std::max(out->shared_after,std::nextafter(h.shared_pulses[h.shared_pulses.size()-shared_cache->size()-1].start,INFINITY));out->truncated=true;}
        if(out->shared_after>0)out->truncated=true;
        out->activity_available_after=sim->activity.available_after;
        activity_delta(sim->activity.deliveries,sim->activity.delivery_begin,out->activity_deliveries,base?base->activity_deliveries.end:0,out->activity_available_after,[](const Delivery& e){return e.t_arrival;},out->truncated);
        activity_delta(sim->activity.spikes,sim->activity.spike_begin,out->activity_spikes,base?base->activity_spikes.end:0,out->activity_available_after,[](const Event& e){return e.time;},out->truncated);
        auto& m=out->metrics;m.running=running;m.elapsed_seconds=active_seconds+(running?seconds(Clock::now()-active_begin):0);m.compute_seconds=compute_seconds;
        m.memory_samples_from=h.time.empty()?sim->time:h.time.front();m.memory_events_from=h.events_after;m.memory_stimuli_from=std::max(h.stimuli_after,h.shared_after);
        m.archive_active=bool(sim->archive);m.archive_required=!sim->archive_anchor.empty();
        if(sim->archive){auto info=sim->archive->info();m.archive_deliveries=info.deliveries;m.archive_id=info.id;m.disk_from=info.from;m.disk_until=info.until;m.archive_status=info.status;}
        m.compute_threads=sim->compute_threads();m.effective_compute_threads=sim->effective_compute_threads();
        m.model_per_second=m.elapsed_seconds>0?(sim->time-model_start)/m.elapsed_seconds:0;
        const double until=sim->experiment.stage_active&&!sim->experiment.automatic?sim->experiment.stage_end:sim->config.duration;
        m.eta_seconds=!out->done&&m.model_per_second>0?std::max(0.,until-sim->time)/m.model_per_second:0;
        m.command_queue_size=queued;m.command_queue_bytes=queued_bytes;m.history_doubles=(samples.end-samples.begin)*recorded;
        m.snapshot_bytes=sizeof(*out)+out->state.size()*sizeof(double)+out->plasticity.size()*sizeof(StdpState)+(out->spikes.size()+out->releases.size())*sizeof(uint64_t)+samples.time.size()*recorded*sizeof(double)+samples.columns.size()*sizeof(SampleColumnDelta)+
            out->events.entries.size()*sizeof(Event)+out->deliveries.entries.size()*sizeof(Delivery)+out->stimuli.entries.size()*sizeof(StimulusEvent)+out->activity_deliveries.entries.size()*sizeof(Delivery)+out->activity_spikes.entries.size()*sizeof(Event)+config_bytes(*config_cache)+experiment_bytes(*experiment_cache);
        for(const auto& p:*shared_cache)m.snapshot_bytes+=sizeof(p)+p.recipients.size()*sizeof(int);
        for(const auto& c:*commands_cache)m.snapshot_bytes+=sizeof(c)+stimulator_bytes(c.value);
        for(const auto& c:*changes_cache)m.snapshot_bytes+=sizeof(c)+stimulator_bytes(c.value)+c.recipients.size()*sizeof(int);
        publish_seconds+=seconds(Clock::now()-started);m.publish_seconds=publish_seconds;m.publish_count=publish_count+1;
        std::shared_ptr<const SimulationSnapshot> retired;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(stopping)return false;
            // A concurrent take ACK changes the required delta base. Never publish
            // a packet that would force GUI to skip or replay accepted history.
            auto actual=consumed;if(actual&&actual->generation!=generation)actual.reset();
            if(actual!=base)return false;
            out->sequence=++sequence;++publish_count;retired=std::move(mailbox);mailbox=std::move(out);
        }
        next_publish=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1/display_hz.load()));
        return true;
    }
    bool stopped(){std::lock_guard<std::mutex> lock(mutex);return stopping;}
    void acknowledge(std::unique_ptr<Request> request,RunnerResult r) {
        // Force publication reconciles against the last consumed packet, so a
        // caller can wait for a result without draining the mailbox to make room.
        try {
            while(!publish(true)){if(stopped()){finish(std::move(request),result(false,"Runner stopped before command acknowledgement."));return;}}
            r.snapshot_sequence=sequence;finish(std::move(request),std::move(r));
        }catch(const std::exception& e){finish(std::move(request),result(false,std::string("Snapshot publication failed: ")+e.what()));throw;}
        catch(...){finish(std::move(request),result(false,"Snapshot publication failed."));throw;}
    }
    void command(std::unique_ptr<Request> request) {
        last_command=request->id;const auto& c=request->command;auto r=result();bool defer=false;
        try {
            using K=RunnerCommand::Kind;
            switch(c.kind) {
            case K::Run:
                if(!sim->archive_anchor.empty()&&!sim->archive)throw std::runtime_error("Attach archive: continue previous recording or create a new branch.");
                if(sim->done())throw std::runtime_error(sim->error.empty()?"Simulation is complete; extend or reset it before running.":sim->error);
                cancel_target("Advance superseded by Run.");set_running(true);break;
            case K::Pause:set_running(false);cancel_target("Advance interrupted by Pause.");flush_archive(*sim);break;
            case K::Advance:case K::RunTo: {
                if(!sim->archive_anchor.empty()&&!sim->archive)throw std::runtime_error("Attach archive before continuing this checkpoint.");
                if(!std::isfinite(c.value)||c.value<0)throw std::runtime_error("Invalid advance target.");
                const double at=c.kind==K::Advance?std::min(sim->config.duration,sim->time+c.value):c.value;
                if(!std::isfinite(at)||at>sim->config.duration)throw std::runtime_error("Advance target exceeds the configured duration.");
                if(at>sim->time&&sim->done())throw std::runtime_error(sim->error.empty()?"Cannot advance a completed stage or experiment.":sim->error);
                cancel_target("Advance superseded by another target.");
                if(at>sim->time){target_time=at;target_request=std::move(request);set_running(true);defer=true;}else set_running(false);
                break;
            }
            case K::Reset: {auto candidate=std::make_unique<Simulator>(c.flag?sim->config:c.config);if(!candidate->error.empty())throw std::runtime_error(candidate->error);replace(std::move(candidate));break;}
            case K::LoadConfig: {auto candidate=std::make_unique<Simulator>(load_config(c.path));if(!candidate->error.empty())throw std::runtime_error(candidate->error);replace(std::move(candidate));break;}
            case K::Restore: {auto candidate=load_checkpoint(c.path);if(!c.text.empty())candidate->fork_experiment(c.text);replace(std::move(candidate));break;}
            case K::SaveCheckpoint:save_checkpoint(*sim,c.path);break;
            case K::Export:export_results(*sim,c.path);break;
            case K::ExportTrained:save_config(sim->learned_config(),c.path);break;
            case K::ExportAnalysis:export_analysis(analyze_network(sim->config,sim->history,sim->time,c.analysis,c.analysis_interval),c.analysis,c.path,c.text);break;
            case K::Extend:sim->extend_duration(c.value);config_dirty=true;break;
            case K::EnterStage: {
                Config candidate=sim->config;candidate.stages=c.stages;const auto issue=validate_experiment(candidate);if(!issue.empty())throw std::runtime_error(issue);
                if(c.index<0||size_t(c.index)>=candidate.stages.size())throw std::runtime_error("Invalid stage index.");
                auto old=std::move(sim->config.stages);sim->config.stages=std::move(candidate.stages);
                try{sim->enter_stage(size_t(c.index),c.flag);}catch(...){sim->config.stages=std::move(old);throw;}
                config_dirty=true;cancel_target("Advance interrupted by stage transition.");set_running(c.flag2);break;
            }
            case K::ContinueCurrent:sim->continue_current();break;
            case K::Learning:sim->set_learning(c.flag,c.value,c.flag2);break;
            case K::Plan: {Config candidate=sim->config;candidate.stages=c.stages;auto issue=validate_experiment(candidate);if(!issue.empty())throw std::runtime_error(issue);sim->config.stages=c.stages;config_dirty=true;break;}
            case K::Analysis: {Config candidate=sim->config;candidate.analysis=c.analysis;auto issue=validate_experiment(candidate);if(!issue.empty())throw std::runtime_error(issue);sim->config.analysis=c.analysis;config_dirty=true;break;}
            case K::BoundedJournals:
                if(!c.flag&&(sim->history.events_after||sim->history.deliveries_after||sim->history.stimuli_after||sim->history.shared_after))throw std::runtime_error("Trimmed history cannot be declared complete.");
                sim->config.bounded_journals=c.flag;config_dirty=true;break;
            case K::Stimulator: {
                const uint64_t id=c.stimulator.id?c.stimulator.id:sim->config.next_stimulator_id;
                auto source=c.stimulator;
                if(c.value<0&&c.flag2){source.signal.start=sim->time;source.signal.stop=std::max(sim->time+1,sim->config.duration);}
                r.applied_time=sim->schedule_stimulator(std::move(source),c.value<0?sim->time:c.value,c.flag);r.id=id;
                const auto sequence=sim->next_stimulus_command-1;
                for(const auto& command:sim->stimulus_commands)if(command.sequence==sequence)r.accepted_stimulator=command.value;
                for(const auto& change:sim->stimulus_changes)if(change.sequence==sequence)r.accepted_stimulator=change.value;
                config_dirty=true;break;
            }
            case K::MoveStimulator:r.applied_time=sim->move_stimulator(c.id,c.value,c.second,c.third<0?sim->time:c.third);r.id=c.id;config_dirty=true;break;
            case K::RemoveStimulator:sim->remove_stimulator(c.id);config_dirty=true;break;
            case K::MoveNeuron:sim->move_neuron(c.index,c.value,c.second);config_dirty=true;break;
            case K::MoveAstro:
                if(c.index<0||size_t(c.index)>=sim->config.astrocytes.size()||!std::isfinite(c.value)||!std::isfinite(c.second)||c.value<0||c.value>1||c.second<0||c.second>1)throw std::runtime_error("Invalid astrocyte display position.");
                sim->config.astrocytes[c.index].px=c.value;sim->config.astrocytes[c.index].py=c.second;config_dirty=true;break;
            case K::WriteProfile:profile::write(c.path);break;
            case K::ComputeThreads:sim->set_compute_threads(c.index);break;
            case K::StartArchive: {
                if(running)throw std::runtime_error("Pause before attaching a history archive");
                const auto old=sim->config;
                if(sim->steps==0){sim->config.bounded_journals=true;sim->config.journal_capacity=50000;sim->config.history_capacity=size_t(std::max(2,c.index));}
                try{auto issue=validate(sim->config);if(!issue.empty())throw std::runtime_error(issue);start_archive(*sim,c.path,c.flag,c.flag2);}
                catch(...){sim->config=old;throw;}
                config_dirty=true;break;
            }
            case K::FlushArchive:flush_archive(*sim);break;
            case K::CaptureWeights:
                r.weights=std::make_shared<const WeightSnapshot>(capture_weights(sim->config,sim->plasticity,sim->time,sim->steps,c.text.empty()?"t="+std::to_string(sim->time)+" s":c.text));break;
            case K::Barrier:break;
            }
            if(defer)return;
            // Replacement changes generation/time. Preserve only explicitly returned
            // source ID and scheduled time from the old result object.
            const auto id=r.id;const double applied=r.applied_time;auto accepted=std::move(r.accepted_stimulator);auto weights=std::move(r.weights);r=result();r.id=id;r.accepted_stimulator=std::move(accepted);r.weights=std::move(weights);
            if(c.kind==K::Stimulator||c.kind==K::MoveStimulator)r.applied_time=applied;
        }catch(const std::exception& e){if(sim->archive&&sim->archive->info().status=="failed"){set_running(false);runner_error=e.what();}r=result(false,e.what());}
        catch(...){r=result(false,"Controller command failed with an unknown exception.");}
        acknowledge(std::move(request),std::move(r));
    }
    void loop(Config initial) noexcept {
        std::string failure="Runner is shutting down.";bool failed=false;
        try {
            sim=std::make_unique<Simulator>(initial);profile::reset();model_start=sim->time;publish(true);
            for(;;) {
                std::unique_ptr<Request> request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if(stopping)break;
                    if(!queue.empty()){request=std::move(queue.front());queue.pop_front();queue_bytes-=request->bytes;}
                    else if(!running){wake.wait(lock,[&]{return stopping||!queue.empty();});continue;}
                }
                if(request){command(std::move(request));continue;}
                if(stopped())break;
                const size_t versions=sim->experiment.protocols.size();const auto before=Clock::now();
                const bool ok=sim->step();compute_seconds+=seconds(Clock::now()-before);
                if(versions!=sim->experiment.protocols.size())config_dirty=true;
                const bool reached=target_request&&sim->time>=target_time;
                if(reached||!ok||sim->done()) {
                    flush_archive(*sim);set_running(false);
                    if(target_request){auto r=result(sim->error.empty(),sim->error);acknowledge(std::move(target_request),std::move(r));}
                    else while(!publish(true)&&!stopped()){}
                }else if(Clock::now()>=next_publish)publish(false);
            }
        }catch(const std::exception& e){failure=std::string("Runner failed: ")+e.what();failed=true;}
        catch(...){failure="Runner failed with an unknown exception.";failed=true;}
        set_running(false);
        if(failed&&sim) {
            // Best effort terminal status; never turn a controller/transport
            // failure into a fabricated numerical diagnostic in Simulator.
            try{runner_error=failure;while(!publish(true)&&!stopped()){}}catch(...){}
        }
        cancel_target(failure);
        std::deque<std::unique_ptr<Request>> abandoned;
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;queue_bytes=0;abandoned.swap(queue);}
        for(auto& request:abandoned)finish(std::move(request),result(false,failure));wake.notify_all();
        // Explicit shutdown completes the entire computation lifetime, not just
        // the controller. Destroy/join its helper pool on the owner thread.
        // Published snapshots own their data and remain valid independently.
        try{if(sim)flush_archive(*sim);}catch(const std::exception& e){runner_error=e.what();}
        sim.reset();
    }
};
SimulationRunner::SimulationRunner(const Config& config):impl_(std::make_unique<Impl>(config)){}
SimulationRunner::~SimulationRunner()=default;
std::shared_future<RunnerResult> SimulationRunner::submit(RunnerCommand command) {
    auto request=std::make_unique<Impl::Request>();request->bytes=command_bytes(command);request->command=std::move(command);
    auto future=request->promise.get_future().share();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);request->id=impl_->next_command++;
        if(impl_->stopping||impl_->queue.size()>=MaxCommands||request->bytes>MaxCommandBytes||impl_->queue_bytes>MaxCommandBytes-request->bytes) {
            RunnerResult result;result.command_id=request->id;result.error=impl_->stopping?"Runner is stopped.":"Busy: controller command queue is full.";
            request->promise.set_value(std::move(result));return future;
        }
        const size_t bytes=request->bytes;
        impl_->queue.push_back(std::move(request));impl_->queue_bytes+=bytes;
    }
    impl_->wake.notify_one();return future;
}
std::shared_ptr<const SimulationSnapshot> SimulationRunner::take_snapshot() {
    std::shared_ptr<const SimulationSnapshot> snapshot,retired;
    {std::lock_guard<std::mutex> lock(impl_->mutex);snapshot=std::move(impl_->mailbox);if(snapshot){retired=std::move(impl_->consumed);impl_->consumed=snapshot;}}
    return snapshot;
}
void SimulationRunner::set_display_hz(double hz) {
    if(!std::isfinite(hz)||hz<.1||hz>120)throw std::invalid_argument("Display publication rate must be 0.1..120 Hz.");
    impl_->display_hz.store(hz);impl_->wake.notify_one();
}
void SimulationRunner::shutdown(){impl_->shutdown();}
}
