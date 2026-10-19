#include "simulation_runner.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

using namespace studio;
namespace {
using Clock=std::chrono::steady_clock;
using Kind=RunnerCommand::Kind;
int checks=0;
void check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);check(bool(f),"Missing test artifact");return {std::istreambuf_iterator<char>(f),{}};
}
bool bits(double a,double b) {return std::memcmp(&a,&b,sizeof(double))==0;}
bool bits(const std::vector<double>& a,const std::vector<double>& b) {
    return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(double))==0);
}
RunnerCommand command(Kind kind,double value=0) {RunnerCommand c;c.kind=kind;c.value=value;return c;}
struct Harness {
    SimulationRunner runner;
    SimulationView view;
    explicit Harness(const Config& c):runner(c) {call(command(Kind::Barrier));}
    void poll() {
        if(auto snapshot=runner.take_snapshot()) {
            check(view.apply(snapshot),"Snapshot delta base/generation mismatch");
            check(view.metrics.command_queue_size<=SimulationRunner::MaxCommands,"Command queue exceeded limit");
            check(view.metrics.command_queue_bytes<=SimulationRunner::MaxCommandBytes,"Command byte limit exceeded");
            check(view.metrics.history_doubles<=SimulationRunner::MaxDisplayDoubles,"Display scalar limit exceeded");
            check(view.history.time.size()<=SimulationRunner::MaxDisplaySamples,"Display sample limit exceeded");
        }
    }
    RunnerResult wait(const std::shared_future<RunnerResult>& future,bool consume=true) {
        const auto deadline=Clock::now()+std::chrono::seconds(30);
        while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
            if(consume)poll();check(Clock::now()<deadline,"Runner command timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto result=future.get();if(consume)poll();return result;
    }
    RunnerResult call(RunnerCommand c,bool success=true) {
        auto result=wait(runner.submit(std::move(c)));
        if(success&&!result.ok)throw std::runtime_error("Runner command: "+result.error);
        check(success==result.ok,"Unexpected command success/failure");return result;
    }
    void save(const std::filesystem::path& path) {auto c=command(Kind::SaveCheckpoint);c.path=path;call(std::move(c));}
};
void finish(Simulator& s) {while(!s.done())check(s.step(),s.error.c_str());check(s.error.empty(),"Reference solver error");}
void advance_to_steps(Simulator& s,uint64_t steps) {while(s.steps<steps)check(s.step(),"Reference could not reach worker boundary");check(s.steps==steps,"Worker changed macro-step numbering");}
void exact(Harness& h,const Simulator& reference,const std::filesystem::path& dir,const char* name) {
    const auto direct=dir/(std::string(name)+"-direct.nscp"),worker=dir/(std::string(name)+"-worker.nscp");
    save_checkpoint(reference,direct);h.save(worker);
    check(bytes(direct)==bytes(worker),"Runner changed authoritative checkpoint bytes");
    h.call(command(Kind::Barrier));
    check(h.view.steps==reference.steps&&bits(h.view.time,reference.time),"Display clock differs from paused owner");
    check(bits(h.view.state,reference.state),"Display state differs from owner");
    check(h.view.spikes==reference.spikes&&h.view.releases==reference.releases,"Display event counters differ");
    check(h.view.pending_deliveries.size()==reference.pending_deliveries.size(),"Pending delivery summary differs");
    check(h.view.plasticity.size()==reference.plasticity.size(),"Display plasticity shape differs");
    for(size_t i=0;i<reference.plasticity.size();++i) {
        const auto& a=h.view.plasticity[i];const auto& b=reference.plasticity[i];
        check(bits(a.weight,b.weight)&&bits(a.pre_trace,b.pre_trace)&&bits(a.post_trace,b.post_trace)&&bits(a.post_slow_trace,b.post_slow_trace),"Display lost a learned weight or trace");
    }
    if(!h.view.truncated) {
        check(bits(h.view.history.time,reference.history.time),"Incremental sample time history differs");
        check(h.view.history.values.size()==reference.history.values.size()&&h.view.history.weights.size()==reference.history.weights.size(),"Incremental column shapes differ");
        for(size_t i=0;i<reference.history.values.size();++i)check(bits(h.view.history.values[i],reference.history.values[i]),"Incremental state history differs");
        for(size_t i=0;i<reference.history.weights.size();++i)check(bits(h.view.history.weights[i],reference.history.weights[i]),"Incremental weight history differs");
        check(h.view.history.events.size()==reference.history.events.size(),"Incremental event history differs");
        for(size_t i=0;i<reference.history.events.size();++i) {
            const auto& a=h.view.history.events[i];const auto& b=reference.history.events[i];
            check(bits(a.time,b.time)&&a.index==b.index&&a.astro==b.astro,"Incremental event ordering differs");
        }
        check(h.view.history.deliveries.size()==reference.history.deliveries.size(),"Incremental deliveries differ");
        for(size_t i=0;i<reference.history.deliveries.size();++i) {
            const auto& a=h.view.history.deliveries[i];const auto& b=reference.history.deliveries[i];
            check(bits(a.t_emit,b.t_emit)&&bits(a.t_arrival,b.t_arrival)&&a.synapse==b.synapse,"Incremental delivery ordering differs");
        }
        check(h.view.history.shared_pulses.size()==reference.history.shared_pulses.size(),"Shared pulse display size differs");
        for(size_t i=0;i<reference.history.shared_pulses.size();++i) {
            const auto& a=h.view.history.shared_pulses[i];const auto& b=reference.history.shared_pulses[i];
            check(a.stimulator==b.stimulator&&a.pulse==b.pulse&&bits(a.start,b.start)&&bits(a.end,b.end)&&a.complete==b.complete&&a.recipients==b.recipients,"Mutable shared pulse was not republished");
        }
    }
    for(int i=0;i<int(reference.config.neurons.size());++i)
        for(double time:{0.,reference.time*.5,reference.time})check(bits(h.view.historical_input(i,time),reference.historical_input(i,time)),"Historical stimulus display formula changed");
}
Config fixture(int rule=1) {
    auto c=preset(6);c.name="runner lifecycle deterministic fixture";c.dt=.000025;c.duration=.04;c.sample=.0001;
    c.seed=706;c.noise_sigma=.45;c.noise_interval=.001;
    for(size_t i=0;i<c.neurons.size();++i) {auto& n=c.neurons[i];n.current=8+3*double(i);n.amplitude=30;n.start=.002+i*.0001;n.stop=.039;n.frequency=100;n.width=.0011;}
    c.synapses[0].model=SynapseModel::TsodyksMarkram;c.synapses[0].gain=1;c.synapses[0].stdp=true;c.synapses[0].stdp_rule=StdpRule(rule);c.synapses[0].delay=.001375;
    Stimulator s;s.id=1;s.name="shared pulse";s.recipients={0,1};s.signal.amplitude=3;s.signal.frequency=100;s.signal.width=.0013;s.signal.start=.000375;s.signal.stop=.039;c.stimulators={s};c.next_stimulator_id=2;
    check(validate(c).empty(),"Invalid lifecycle fixture");return c;
}
void refresh_equivalence(const std::filesystem::path& dir) {
    for(int rule=0;rule<3;++rule) {
        auto c=fixture(rule);Simulator direct(c);finish(direct);
        check(!direct.history.events.empty()&&!direct.history.deliveries.empty(),"Lifecycle fixture did not exercise spikes and delayed delivery");
        for(double hz:{1.,10.,60.}) {
            Harness h(c);h.runner.set_display_hz(hz);
            // More than one publication delta plus mutable, equal-time journals.
            for(int part=1;part<=8;++part)h.call(command(Kind::RunTo,c.duration*part/8));
            check(h.view.done()&&!h.view.metrics.running,"RunTo did not stop at target");
            exact(h,direct,dir,("refresh-"+std::to_string(rule)+"-"+std::to_string(int(hz))).c_str());
        }
    }
}
void pause_and_mutate(const std::filesystem::path& dir) {
    auto c=fixture(2);c.duration=10;Harness h(c);Simulator direct(c);
    for(int round=0;round<5;++round) {
        h.call(command(Kind::Advance,.001));advance_to_steps(direct,h.view.steps);
        h.call(command(Kind::Run));
        // Controls are admitted while calculations are active; the reported
        // application boundary is authoritative, not the last rendered time.
        auto learning=command(Kind::Learning,.5+round*.1);learning.flag=round%2==0;learning.flag2=round==2;
        auto applied=h.call(learning);advance_to_steps(direct,applied.steps);direct.set_learning(learning.flag,learning.value,learning.flag2);
        check(bits(applied.time,direct.time),"Learning command did not report its boundary");
        auto stopped=h.call(command(Kind::Pause));advance_to_steps(direct,stopped.steps);
        check(!h.view.metrics.running,"Pause acknowledged before computation stopped");
        const auto stopped_steps=h.view.steps;std::this_thread::sleep_for(std::chrono::milliseconds(3));h.call(command(Kind::Barrier));
        check(h.view.steps==stopped_steps,"Worker kept stepping after acknowledged pause");
        exact(h,direct,dir,("pause-"+std::to_string(round)).c_str());
    }
    auto source=command(Kind::Stimulator,-1);source.stimulator=c.stimulators.front();source.stimulator.signal.amplitude=7;
    auto applied=h.call(source);direct.schedule_stimulator(source.stimulator,direct.time,false);
    check(bits(applied.applied_time,direct.time),"Now stimulus used stale display time");
    h.call(command(Kind::Advance,.003));advance_to_steps(direct,h.view.steps);
    exact(h,direct,dir,"changed-stimulus");
    auto move=command(Kind::MoveNeuron,.42);move.index=0;move.second=.57;h.call(move);direct.move_neuron(0,.42,.57);
    exact(h,direct,dir,"moved-neuron");
    auto interrupted=h.runner.submit(command(Kind::RunTo,9));
    auto paused=h.call(command(Kind::Pause));advance_to_steps(direct,paused.steps);
    auto cancelled=h.wait(interrupted);check(!cancelled.ok&&!cancelled.error.empty(),"Pause left an active target successful or pending");
    exact(h,direct,dir,"cancelled-target");
    auto extend=command(Kind::Extend,11);h.call(extend);direct.extend_duration(11);
    exact(h,direct,dir,"extended");
    auto invalid=command(Kind::Learning,-1);invalid.flag=true;auto rejected=h.call(invalid,false);
    check(!rejected.error.empty(),"Worker swallowed command exception");exact(h,direct,dir,"rejected-command");
}
void restore_and_stages(const std::filesystem::path& dir) {
    auto c=fixture();c.stages={{"learn",.007,true,false,1,{0},true,25,100,.001,0},{"freeze",.009,false,true,1,{1},true,20,100,.001,.000375}};
    Harness h(c);Simulator direct(c);auto stage=command(Kind::EnterStage);stage.stages=c.stages;stage.index=0;stage.flag=true;h.call(stage);direct.enter_stage(0,true);
    h.call(command(Kind::RunTo,.011));advance_to_steps(direct,h.view.steps);exact(h,direct,dir,"stage-mid");
    const auto state=dir/"resume.nscp";h.save(state);const auto before=bytes(state);const auto generation=h.view.generation;
    h.call(command(Kind::Advance,.002));auto restore=command(Kind::Restore);restore.path=state;h.call(restore);
    check(h.view.generation!=generation&&!h.view.metrics.running,"Restore did not create a paused display generation");
    exact(h,direct,dir,"restored");
    auto bad=command(Kind::Restore);bad.path=dir/"missing.nscp";const auto restored_generation=h.view.generation;h.call(bad,false);
    check(h.view.generation==restored_generation,"Failed restore replaced generation");exact(h,direct,dir,"failed-restore");
    // Alter only build identity and update the checksum: strict compatibility
    // rejection must still occur in the real loader on the worker thread.
    auto incompatible=before;auto read64=[&](size_t p){uint64_t n=0;for(int k=0;k<8;++k)n|=uint64_t(static_cast<unsigned char>(incompatible.at(p+k)))<<(8*k);return n;};
    size_t p=26+8;p+=8+read64(p);check(read64(p)>0,"Missing checkpoint build ID");incompatible.at(p+8)^=1;
    uint64_t hash=14695981039346656037ull;for(size_t i=0;i<incompatible.size()-8;++i){hash^=static_cast<unsigned char>(incompatible[i]);hash*=1099511628211ull;}
    for(int k=0;k<8;++k)incompatible[incompatible.size()-8+k]=char(hash>>(8*k));
    bad.path=dir/"incompatible.nscp";write_atomic(bad.path,incompatible);h.call(bad,false);
    check(h.view.generation==restored_generation,"Incompatible checkpoint replaced generation");exact(h,direct,dir,"incompatible-rejected");
    finish(direct);h.call(command(Kind::RunTo,direct.time));exact(h,direct,dir,"stages-finished");
    auto output=command(Kind::Export);output.path=dir/"runner-export";h.call(output);export_results(direct,dir/"direct-export");
    for(const auto& entry:std::filesystem::directory_iterator(dir)) {
        const auto name=entry.path().filename().string();if(name.rfind("runner-export",0)==0&&entry.path().extension()==".csv")
            check(bytes(entry.path())==bytes(dir/("direct-export"+name.substr(std::string("runner-export").size()))),"Worker export differs from direct export");
    }
    auto learned=command(Kind::ExportTrained);learned.path=dir/"trained.json";h.call(learned);check(serialize(load_config(learned.path))==serialize(direct.learned_config()),"Trained export lost final weights");
    auto reset=command(Kind::Reset);reset.config=fixture(0);reset.config.duration=10;h.call(reset);check(h.view.time==0&&h.view.steps==0&&h.view.generation!=restored_generation,"Reset retained trajectory");
    auto interrupted=h.runner.submit(command(Kind::RunTo,9));h.call(reset);
    check(!h.wait(interrupted).ok&&h.view.time==0,"Reset did not cancel previous target and clear trajectory");
    auto path=dir/"loaded.json";save_config(c,path);auto load=command(Kind::LoadConfig);load.path=path;h.call(load);check(h.view.time==0&&serialize(h.view.config)==serialize(c),"LoadConfig did not replace paused experiment");
}
void bounded_transport_and_shutdown(const std::filesystem::path& dir) {
    auto c=fixture();c.neurons.resize(1);c.synapses.clear();c.stimulators.clear();c.duration=.6;c.sample=c.dt;c.noise_sigma=0;c.neurons[0].amplitude=0;c.neurons[0].current=0;
    Harness h(c);h.runner.set_display_hz(60);auto target=h.runner.submit(command(Kind::RunTo,c.duration));
    // No consumer during the whole run: there must be one reconcilable packet,
    // not a growing queue of stale snapshots.
    auto result=h.wait(target,false);check(result.ok,"Unconsumed display stopped computation");h.poll();
    check(!h.runner.take_snapshot(),"More than one unconsumed snapshot was queued");
    check(h.view.truncated&&h.view.sample_begin>0&&h.view.history.time.size()<=SimulationRunner::MaxDisplaySamples,"Display retention did not cap long sample history");
    check(h.view.history.time.back()==c.duration&&h.view.time==c.duration,"Bounded display missed final sample");
    h.save(dir/"full-history.nscp");auto authoritative=load_checkpoint(dir/"full-history.nscp");
    check(authoritative->history.time.size()>h.view.history.time.size(),"Display retention truncated scientific owner history");
    check(bits(h.view.state,authoritative->state),"Truncated display changed final state");
    auto reset=command(Kind::Reset);reset.config=c;h.call(reset);
    auto barrier=h.runner.submit(command(Kind::Barrier));check(h.wait(barrier,false).ok,"Snapshot setup failed");
    const auto retained=h.runner.take_snapshot();check(bool(retained),"Barrier omitted immutable snapshot");
    const auto old_state=retained->state;const auto old_time=retained->time;
    check(h.view.apply(retained),"Immutable snapshot setup lost its delta base");
    h.call(command(Kind::Advance,.002));
    check(bits(retained->state,old_state)&&bits(retained->time,old_time),"Published snapshot was mutated by later steps");
    auto oversized=command(Kind::Barrier);oversized.text.assign(SimulationRunner::MaxCommandBytes+1,'x');h.call(std::move(oversized),false);
    std::vector<std::shared_future<RunnerResult>> waiting;
    for(int i=0;i<256;++i)waiting.push_back(h.runner.submit(command(Kind::Barrier)));
    for(const auto& f:waiting){auto r=h.wait(f);check(r.ok||!r.error.empty(),"Rejected queue command has no error");}
    h.call(command(Kind::Barrier));check(!h.view.metrics.running,"Idle command queue restarted solver");
    c.duration=100;c.sample=.01;
    for(int iteration=0;iteration<12;++iteration) {
        SimulationRunner runner(c);auto started=runner.submit(command(Kind::Run));
        check(started.wait_for(std::chrono::seconds(5))==std::future_status::ready&&started.get().ok,"Shutdown test could not start worker");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto boundary=runner.submit(command(Kind::Barrier));
        check(boundary.wait_for(std::chrono::seconds(5))==std::future_status::ready&&boundary.get().steps>0,"Shutdown test never computed a step");
        auto future=runner.submit(command(Kind::RunTo,c.duration));
        const auto start=Clock::now();runner.shutdown();
        check(Clock::now()-start<std::chrono::seconds(5),"Shutdown did not join promptly");
        check(future.wait_for(std::chrono::seconds(0))==std::future_status::ready,"Shutdown left target promise unresolved");
        auto after=runner.submit(command(Kind::Run));check(after.wait_for(std::chrono::seconds(0))==std::future_status::ready&&!after.get().ok,"Stopped runner accepted command");
        runner.shutdown(); // idempotent; destructor calls it once more.
    }
    // Destruction while active must also resolve promises without a GUI pump.
    std::shared_future<RunnerResult> orphan;
    {SimulationRunner runner(c);orphan=runner.submit(command(Kind::RunTo,c.duration));}
    check(orphan.wait_for(std::chrono::seconds(0))==std::future_status::ready,"Destructor left target promise unresolved");
}
}
int main() {
    try {
        const auto dir=std::filesystem::current_path()/"runner-tests"/std::to_string(Clock::now().time_since_epoch().count());std::filesystem::create_directories(dir);
        refresh_equivalence(dir);pause_and_mutate(dir);restore_and_stages(dir);bounded_transport_and_shutdown(dir);
        std::cout<<"PASS: "<<checks<<" runner checks; exact full checkpoints, refresh independence, lifecycle, strict restore, incremental bounded snapshots. Artifacts: "<<dir.u8string()<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
