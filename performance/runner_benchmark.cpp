#include "simulation_runner.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <numeric>
#include <thread>

using namespace studio;
namespace {
using Clock=std::chrono::steady_clock;
using Kind=RunnerCommand::Kind;
double seconds(Clock::duration duration) {return std::chrono::duration<double>(duration).count();}
RunnerCommand command(Kind kind) {RunnerCommand c;c.kind=kind;return c;}
double number(const char* text,double lo,double hi) {
    size_t end=0;const double value=std::stod(text,&end);
    if(text[end]||!std::isfinite(value)||value<lo||value>hi)throw std::runtime_error("Invalid benchmark numeric argument");
    return value;
}
struct TransportMetrics {
    uint64_t snapshots=0,sample_entries=0,sample_scalar_entries=0,event_entries=0,delivery_entries=0,stimulus_entries=0;
    uint64_t activity_spike_entries=0,activity_delivery_entries=0;
    size_t peak_snapshot_bytes=0,peak_history_doubles=0,peak_retained_sample_bytes=0,peak_command_count=0,peak_command_bytes=0;
    double apply_seconds=0;
    bool truncated=false;
};
struct Consumer {
    SimulationRunner& runner;
    SimulationView view;
    double wait_ms;
    TransportMetrics transport;
    bool measuring=false;
    void poll() {
        if(auto packet=runner.take_snapshot()) {
            const auto before=Clock::now();
            if(!view.apply(packet))throw std::runtime_error("Snapshot generation/base sequence mismatch");
            const double apply=seconds(Clock::now()-before);
            if(!view.error.empty())throw std::runtime_error(view.error);
            if(!measuring)return;
            auto& m=transport;++m.snapshots;m.apply_seconds+=apply;
            m.sample_entries+=packet->samples.time.size();m.sample_scalar_entries+=packet->samples.time.size();
            for(const auto& column:packet->samples.columns)m.sample_scalar_entries+=column.values.size();
            m.event_entries+=packet->events.entries.size();m.delivery_entries+=packet->deliveries.entries.size();
            m.stimulus_entries+=packet->stimuli.entries.size();m.activity_spike_entries+=packet->activity_spikes.entries.size();
            m.activity_delivery_entries+=packet->activity_deliveries.entries.size();
            m.peak_snapshot_bytes=std::max(m.peak_snapshot_bytes,packet->metrics.snapshot_bytes);
            m.peak_history_doubles=std::max(m.peak_history_doubles,packet->metrics.history_doubles);
            size_t values=view.history.time.size();
            for(const auto& column:view.history.values)values+=column.size();
            for(const auto& column:view.history.weights)values+=column.size();
            m.peak_retained_sample_bytes=std::max(m.peak_retained_sample_bytes,values*sizeof(double));
            m.peak_command_count=std::max(m.peak_command_count,packet->metrics.command_queue_size);
            m.peak_command_bytes=std::max(m.peak_command_bytes,packet->metrics.command_queue_bytes);
            m.truncated=m.truncated||packet->truncated;
        }
    }
    RunnerResult wait(const std::shared_future<RunnerResult>& future,bool final=false) {
        const auto deadline=Clock::now()+std::chrono::hours(24);
        RunnerResult result;bool acknowledged=false;
        for(;;) {
            poll();
            if(!acknowledged&&future.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                result=future.get();acknowledged=true;
                if(!result.ok)throw std::runtime_error("Worker command failed: "+result.error);
            }
            if(acknowledged&&view.generation==result.generation&&view.sequence>=result.snapshot_sequence&&
               view.last_command_id>=result.command_id&&view.steps==result.steps&&view.time==result.time&&
               (!final||(view.done()&&!view.metrics.running)))return result;
            if(Clock::now()>=deadline)throw std::runtime_error("Timed out waiting for a confirmed worker snapshot");
            if(wait_ms>0)std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(wait_ms));
            else std::this_thread::yield();
        }
    }
    RunnerResult call(RunnerCommand c,bool final=false) {return wait(runner.submit(std::move(c)),final);}
};
}
int main(int argc,char** argv) {try {
    if(argc<3||argc>6)throw std::runtime_error("Usage: network_studio_runner_benchmark CONFIG OUTPUT_DIRECTORY [HZ=10] [CONSUMER_MS=1] [THREADS=1; 0=Auto]");
    const double hz=argc>3?number(argv[3],1,60):10,consumer_ms=argc>4?number(argv[4],0,60000):1;
    const auto output=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(output);
    if(std::filesystem::exists(output/"benchmark.json")||std::filesystem::exists(output/"final.nscp"))
        throw std::runtime_error("Refusing to overwrite an existing benchmark");
    const auto setup=Clock::now();const auto config=load_config(std::filesystem::u8path(argv[1]));
    SimulationRunner runner(config);runner.set_display_hz(hz);Consumer consumer{runner,{},consumer_ms};
    consumer.call(command(Kind::Barrier));
    if(argc>5){auto setting=command(Kind::ComputeThreads);const double n=number(argv[5],0,8);if(n!=std::floor(n))throw std::runtime_error("Invalid thread count");setting.index=int(n);consumer.call(setting);}
    const RunnerMetrics initial=consumer.view.metrics;const double model_start=consumer.view.time;
    auto run=command(Kind::RunTo);run.value=config.duration;
    consumer.measuring=true;const auto begin=Clock::now();
    const auto completed=consumer.call(std::move(run),true);
    const auto end=Clock::now();consumer.measuring=false;
    if(completed.time!=config.duration)throw std::runtime_error("Worker stopped before the configured duration (for example at a manual stage boundary)");
    const RunnerMetrics metrics=consumer.view.metrics;const auto transport=consumer.transport;
    const size_t retained_samples=consumer.view.history.time.size(),retained_events=consumer.view.history.events.size();
    const auto neuron_spikes=std::accumulate(consumer.view.spikes.begin(),consumer.view.spikes.end(),uint64_t(0));
    const auto astro_releases=std::accumulate(consumer.view.releases.begin(),consumer.view.releases.end(),uint64_t(0));
    const auto final_sequence=consumer.view.sequence;
    // These commands act on full authoritative worker data, never the bounded view.
    auto export_command=command(Kind::Export);export_command.path=output/"result";consumer.call(std::move(export_command));
    auto checkpoint=command(Kind::SaveCheckpoint);checkpoint.path=output/"final.nscp";consumer.call(std::move(checkpoint));
    const auto exported=Clock::now();
    auto profile=command(Kind::WriteProfile);profile.path=output/"profile.json";consumer.call(std::move(profile));
    runner.shutdown();
    std::ofstream f(output/"benchmark.json");f.exceptions(std::ios::badbit|std::ios::failbit);f.imbue(std::locale::classic());
    f<<std::setprecision(17)<<std::boolalpha
     <<"{\n  \"mode\": \"headless_worker_with_snapshot_consumer\",\n  \"gui\": false,\n"
     <<"  \"display_hz\": "<<hz<<",\n  \"consumer_wait_ms\": "<<consumer_ms
     <<",\n  \"requested_threads\": "<<metrics.compute_threads<<",\n  \"effective_threads\": "<<metrics.effective_compute_threads
     <<",\n  \"setup_seconds\": "<<seconds(begin-setup)<<",\n  \"wall_seconds\": "<<seconds(end-begin)
     <<",\n  \"export_and_checkpoint_seconds\": "<<seconds(exported-end)
     <<",\n  \"model_start_seconds\": "<<model_start<<",\n  \"model_final_seconds\": "<<completed.time
     <<",\n  \"steps\": "<<completed.steps<<",\n  \"final_generation\": "<<completed.generation
     <<",\n  \"final_snapshot_sequence\": "<<final_sequence<<",\n  \"final_command_id\": "<<completed.command_id
     <<",\n  \"final_snapshot_confirmed\": true,\n  \"worker_compute_seconds\": "<<metrics.compute_seconds-initial.compute_seconds
     <<",\n  \"worker_publish_seconds\": "<<metrics.publish_seconds-initial.publish_seconds
     <<",\n  \"worker_elapsed_seconds\": "<<metrics.elapsed_seconds
     <<",\n  \"worker_model_per_second\": "<<metrics.model_per_second<<",\n  \"worker_eta_seconds\": "<<metrics.eta_seconds
     <<",\n  \"worker_publish_count\": "<<metrics.publish_count-initial.publish_count
     <<",\n  \"received_snapshots\": "<<transport.snapshots<<",\n  \"consumer_apply_seconds\": "<<transport.apply_seconds
     <<",\n  \"delta_sample_entries\": "<<transport.sample_entries<<",\n  \"delta_sample_scalar_entries\": "<<transport.sample_scalar_entries
     <<",\n  \"delta_event_entries\": "<<transport.event_entries<<",\n  \"delta_delivery_entries\": "<<transport.delivery_entries
     <<",\n  \"delta_stimulus_entries\": "<<transport.stimulus_entries
     <<",\n  \"delta_activity_spike_entries\": "<<transport.activity_spike_entries
     <<",\n  \"delta_activity_delivery_entries\": "<<transport.activity_delivery_entries
     <<",\n  \"peak_snapshot_bytes_reported\": "<<transport.peak_snapshot_bytes
     <<",\n  \"peak_history_doubles_reported\": "<<transport.peak_history_doubles
     <<",\n  \"peak_retained_sample_bytes\": "<<transport.peak_retained_sample_bytes
     <<",\n  \"peak_command_queue_size_reported\": "<<transport.peak_command_count
     <<",\n  \"peak_command_queue_bytes_reported\": "<<transport.peak_command_bytes
     <<",\n  \"display_was_truncated\": "<<transport.truncated
     <<",\n  \"retained_samples\": "<<retained_samples<<",\n  \"retained_events\": "<<retained_events
     <<",\n  \"neuron_spikes\": "<<neuron_spikes<<",\n  \"astro_releases\": "<<astro_releases
     <<",\n  \"limits\": \"Headless worker and polling consumer, not a GUI/rendering benchmark. wall_seconds includes command admission, simulation, publication, consumer application and confirmation latency; initialization and exports are excluded. Delta counts count transmitted entries, including boundary replacements, not unique scientific events. Reported snapshot bytes and retained scalar bytes are not process RSS or allocator capacity. Full exports and checkpoint are requested from the worker after timing.\"\n}\n";
    std::cout<<seconds(end-begin)<<" seconds through confirmed final snapshot; "<<transport.snapshots<<" packets\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
