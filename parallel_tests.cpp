#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif
#include "simulation_runner.h"
#include "io.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>

using namespace studio;
namespace {
using Clock=std::chrono::steady_clock;
using Kind=RunnerCommand::Kind;
uint64_t checks=0,compared_steps=0;
void check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
bool bits(double a,double b) {return std::memcmp(&a,&b,sizeof(double))==0;}
bool bits(const State& a,const State& b) {return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(double))==0);}
std::string bytes(const std::filesystem::path& path) {std::ifstream f(path,std::ios::binary);check(bool(f),"Missing comparison artifact");return {std::istreambuf_iterator<char>(f),{}};}
RunnerCommand command(Kind kind,double value=0) {RunnerCommand c;c.kind=kind;c.value=value;return c;}
#ifdef _WIN32
std::vector<DWORD> process_threads() {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    check(snapshot!=INVALID_HANDLE_VALUE,"Cannot inspect owned process threads");
    std::vector<DWORD> ids;THREADENTRY32 entry{};entry.dwSize=sizeof(entry);const DWORD process=GetCurrentProcessId();
    if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID==process)ids.push_back(entry.th32ThreadID);}while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);check(!ids.empty(),"Current process missing from thread inventory");std::sort(ids.begin(),ids.end());return ids;
}
#endif
void pool_phases() {
    for(int count:{2,4,6,8,12,16}) {
        numerical::WorkerPool pool(count);check(pool.count()==count,"Pool does not include the controller in its count");
        for(int repeat=0;repeat<12;++repeat)for(size_t size:{size_t(0),size_t(1),size_t(5),size_t(11),size_t(17),size_t(2049)}) {
            std::vector<std::atomic<int>> visits(size);
            for(auto& value:visits)value.store(0);
            pool.run(size,[&](size_t begin,size_t end,int id){
                if(begin>end||end>size||id<0||id>=count)throw std::runtime_error("Pool block outside phase domain");
                for(size_t i=begin;i<end;++i)++visits[i];
            });
            for(auto& value:visits)check(value.load()==1,"Pool lost or repeated a disjoint coordinate");
        }
        for(int failing:{0,count-1}) {
            std::atomic<int> completed{0};bool rejected=false;
            try {pool.run(2049,[&](size_t,size_t,int id){if(id==failing)throw std::runtime_error("intentional phase failure");completed.fetch_add(1);});}
            catch(const std::runtime_error& e){rejected=std::string(e.what())=="intentional phase failure";}
            check(rejected&&completed.load()==count-1,"Phase exception returned before all helper blocks completed");
            std::atomic<int> next{0};pool.run(2049,[&](size_t,size_t,int){next.fetch_add(1);});check(next.load()==count,"Pool could not resume after an exception");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::atomic<int> awakened{0};pool.run(2049,[&](size_t,size_t,int){awakened.fetch_add(1);});check(awakened.load()==count,"Parked helper missed a new phase");
    }
}
void delivery_equal(const std::vector<Delivery>& a,const std::vector<Delivery>& b) {
    check(a.size()==b.size(),"Delivery count/order changed with thread count");
    for(size_t i=0;i<a.size();++i)check(bits(a[i].t_emit,b[i].t_emit)&&bits(a[i].t_arrival,b[i].t_arrival)&&a[i].synapse==b[i].synapse,"Delivery timestamp/order changed with thread count");
}
struct Frame {
    State state;
    std::vector<StdpState> plasticity;
    std::vector<uint64_t> spikes,releases;
    std::vector<Event> events;
    std::vector<Delivery> deliveries,pending;
    double time=0;
    uint64_t steps=0;
    explicit Frame(const Simulator& s):state(s.state),plasticity(s.plasticity),spikes(s.spikes),releases(s.releases),events(s.history.events),deliveries(s.history.deliveries),pending(s.pending_deliveries),time(s.time),steps(s.steps){}
    void equal(const Simulator& s) const {
        check(bits(time,s.time)&&steps==s.steps,"Macro clock changed with thread count");
        check(bits(state,s.state),"Numerical state differs at a macro boundary");
        check(spikes==s.spikes&&releases==s.releases,"Event counters differ at a macro boundary");
        check(plasticity.size()==s.plasticity.size(),"Plasticity shape changed");
        for(size_t i=0;i<plasticity.size();++i) {
            const auto& a=plasticity[i];const auto& b=s.plasticity[i];
            check(bits(a.weight,b.weight)&&bits(a.pre_trace,b.pre_trace)&&bits(a.post_trace,b.post_trace)&&bits(a.post_slow_trace,b.post_slow_trace),"Weight or STDP trace differs at a macro boundary");
        }
        check(events.size()==s.history.events.size(),"Event journal size changed");
        for(size_t i=0;i<events.size();++i){const auto& a=events[i];const auto& b=s.history.events[i];check(bits(a.time,b.time)&&a.index==b.index&&a.astro==b.astro,"Event time/group/order changed with thread count");}
        delivery_equal(deliveries,s.history.deliveries);delivery_equal(pending,s.pending_deliveries);++compared_steps;
    }
};
Config fixture(bool noise=false) {
    auto c=preset(4);c.name=noise?"Parallel seeded noise fixture":"Parallel simultaneous spike fixture";
    c.dt=.000025;c.duration=.003;c.sample=.0001;c.seed=706;c.noise_interval=.001;c.noise_sigma=noise?.7:0;
    c.neurons.clear();c.synapses.clear();c.astrocytes.clear();c.gaps.clear();
    for(int i=0;i<64;++i) {
        Neuron n;n.name="N"+std::to_string(i);n.model=NeuronModel::Izhikevich;n.inhibitory=i>=48;
        n.voltage=29;n.current=8;n.amplitude=5;n.frequency=200;n.start=.001125;n.stop=.003;n.width=.000375;
        n.px=(i%8+.5)/8;n.py=(i/8+.5)/8;
        n.individual_noise=noise&&i%7==0;n.noise_sigma=.3;c.neurons.push_back(n);
    }
    // Global storage interleaves targets. Per-neuron parallel current reduction
    // must preserve the original contribution order rather than re-sort edges.
    for(int slot=0;slot<32;++slot)for(int target=0;target<64;++target) {
        Synapse s;s.target=target;s.source=(target+slot+1)%64;s.model=SynapseModel::TsodyksMarkram;
        const int id=int(c.synapses.size());s.weight=c.neurons[s.source].inhibitory?-.25:.35+(id%5)*.013;
        s.enabled=id%97!=0; // Disabled edges still retain their exact kinetic coordinates.
        s.gain=.4;s.stdp=!c.neurons[s.source].inhibitory;s.stdp_rule=StdpRule(id%3);
        s.stdp_timing=id%2?StdpTiming::Arrival:StdpTiming::Emission;
        s.delay=id%3==0?0:id%3==1?.000375:.001125;
        s.release=.4;s.tau_syn=.001+(id%2)*.001;s.tau_d=.05+(id%3)*.01;s.tau_f=.1+(id%4)*.01;
        if(id%13==0)s.tau_d=s.tau_syn; // Equal-rate exact kinetics branch.
        s.tau_plus=.02+(id%2)*.005;s.tau_minus=.03+(id%3)*.005;s.tau_slow=.1+(id%5)*.01;
        c.synapses.push_back(s);
    }
    c.record_all=false;c.record_neurons={0,1,48,63};c.record_synapses={0,1,63,1024,2047};
    check(validate(c).empty(),"Invalid parallel fixture");return c;
}
void automatic_policy() {
    // Hardware caps are tested without relying on this host's CPU count.
    for(unsigned available:{0u,1u,2u,3u,4u,5u,6u,7u,8u,12u,16u,32u}) {
        check(compute::automatic(2047,available)==1,"Small Auto fixture used helpers");
        for(size_t edges:{size_t(2048),size_t(4095),size_t(4096),size_t(8191),size_t(8192)}) {
            const int expected=available>=8&&edges>=8192?8:available>=6&&edges>=4096?6:available>=4?4:available>=2?2:1;
            check(compute::automatic(edges,available)==expected,"Auto hardware cap changed");
        }
    }
    for(auto mode:compute::ThreadModes) {
        check(compute::parse(std::string_view(mode.argument),"--threads")==mode.count,"CLI thread parser diverged from supported modes");
        std::wstring wide;for(char c:std::string_view(mode.argument))wide+=wchar_t(c);
        check(compute::parse(std::wstring_view(wide),"--compute-threads")==mode.count,"GUI thread parser diverged from supported modes");
    }
    for(auto text:{"", "0", "3", "06", "12x", "16.0", "17", "-1", "AUTO"}) {
        bool rejected=false;try{compute::parse(std::string_view(text),"--threads");}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"Invalid thread argument accepted");
    }
    const unsigned available=std::max(1u,std::thread::hardware_concurrency());
    for(size_t edges:{size_t(2048),size_t(4096),size_t(8192)}) {
        auto c=fixture();const auto block=c.synapses;
        while(c.synapses.size()<edges)c.synapses.insert(c.synapses.end(),block.begin(),block.end());
        Simulator s(c);check(s.error.empty(),"Auto policy fixture is invalid");s.set_compute_threads(0);
        const int expected=available>=8&&edges>=8192?8:available>=6&&edges>=4096?6:available>=4?4:available>=2?2:1;
        check(s.compute_threads()==0&&s.effective_compute_threads()==expected,"Auto did not apply network-size target and hardware cap");
    }
}
void configured(Simulator& s,int threads,bool frozen) {
    s.set_compute_threads(threads);check(s.compute_threads()==threads,"Requested thread count not retained");
    if(frozen)s.set_learning(false,.75,false);
}
void assert_active(const Simulator& s,int requested) {
    const auto effective=s.effective_compute_threads();
    check(effective>=1&&effective<=compute::MaxThreads,"Effective thread count outside supported range");
    if(requested==1)check(effective==1,"Serial request used helper threads");
    else if(requested>1)check(effective==requested,"Eligible fixture silently stayed on serial fallback");
}
void numerical_matrix(const std::filesystem::path& dir) {
    for(int scenario=0;scenario<3;++scenario) {
        const bool frozen=scenario==2;auto c=fixture(scenario!=0);c.name+=" "+std::to_string(scenario);
        Simulator serial(c);configured(serial,1,frozen);std::vector<Frame> trace;
        trace.emplace_back(serial);
        while(!serial.done()){check(serial.step(),serial.error.c_str());trace.emplace_back(serial);}
        check(!serial.history.events.empty()&&!serial.history.deliveries.empty(),"Fixture missed events/deliveries");
        if(scenario==0) {
            const double first=serial.history.events.front().time;size_t group=0;
            for(const auto& e:serial.history.events)if(e.time==first&&!e.astro)++group;
            check(group==c.neurons.size(),"Simultaneous spike fixture was not simultaneous");
        }
        if(frozen) {
            bool traced=false;for(size_t i=0;i<serial.plasticity.size();++i){check(bits(serial.plasticity[i].weight,c.synapses[i].weight),"Frozen fixture changed weights");traced=traced||serial.plasticity[i].pre_trace>0;}
            check(traced,"Frozen fixture did not maintain STDP traces");
        }
        const auto reference=dir/("scenario-"+std::to_string(scenario)+"-serial.nscp");save_checkpoint(serial,reference);const auto expected=bytes(reference);
        for(int count:{1,2,4,6,8,12,16})for(int repeat=0;repeat<2;++repeat) {
            Simulator test(c);configured(test,count,frozen);trace.front().equal(test);size_t frame=1;
            while(!test.done()) {check(test.step(),test.error.c_str());check(frame<trace.size(),"Parallel run created extra macro steps");trace[frame++].equal(test);}
            check(frame==trace.size(),"Parallel run stopped early");assert_active(test,count);
            const auto output=dir/("scenario-"+std::to_string(scenario)+"-threads-"+std::to_string(count)+"-repeat-"+std::to_string(repeat)+".nscp");
            save_checkpoint(test,output);check(bytes(output)==expected,"Full checkpoint differs across threads/repetition");
        }
        // Auto remains a runtime choice; it must have the same mathematical result.
        Simulator automatic(c);configured(automatic,0,frozen);while(!automatic.done())check(automatic.step(),automatic.error.c_str());
        trace.back().equal(automatic);const auto output=dir/("scenario-"+std::to_string(scenario)+"-auto.nscp");save_checkpoint(automatic,output);check(bytes(output)==expected,"Auto changed final checkpoint");
    }
}
void resume_and_reconfigure(const std::filesystem::path& dir) {
    std::cout<<"parallel: cross-count checkpoint resume"<<std::endl;
    const auto c=fixture(true);Simulator baseline(c);configured(baseline,1,false);
#ifdef _WIN32
    {
        const auto original=process_threads();Simulator switching(c);
        std::vector<DWORD> previous;
        for(int count:{6,12,16,8,4,2,1}) {
            switching.set_compute_threads(count);const auto current=process_threads();
            for(DWORD id:previous)check(!std::binary_search(current.begin(),current.end(),id),"Replacement did not release an old helper thread");
            previous.clear();std::set_difference(current.begin(),current.end(),original.begin(),original.end(),std::back_inserter(previous));
            check(previous.size()==size_t(count-1),"Pool count does not equal coordinator plus live helper threads");
        }
    }
#endif
    while(!baseline.done())check(baseline.step(),baseline.error.c_str());save_checkpoint(baseline,dir/"resume-reference.nscp");const auto expected=bytes(dir/"resume-reference.nscp");
    for(int before:{1,2,4,6,8,12,16}) {
        Simulator prefix(c);configured(prefix,before,false);
        while(prefix.time<c.duration*.5)check(prefix.step(),prefix.error.c_str());
        const auto path=dir/("prefix-"+std::to_string(before)+".nscp");save_checkpoint(prefix,path);const auto mid=bytes(path);
        for(int after:{1,2,4,6,8,12,16}) {
            auto resumed=load_checkpoint(path);resumed->set_compute_threads(after);
            save_checkpoint(*resumed,dir/"resume-unchanged.nscp");check(bytes(dir/"resume-unchanged.nscp")==mid,"Thread selection altered loaded checkpoint or bypassed compatible loading");
            while(!resumed->done())check(resumed->step(),resumed->error.c_str());
            assert_active(*resumed,after);save_checkpoint(*resumed,dir/"resume-final.nscp");check(bytes(dir/"resume-final.nscp")==expected,"Cross-count checkpoint resume differs");
        }
    }
    std::cout<<"parallel: boundary thread reconfiguration"<<std::endl;
    Simulator changing(c),reference(c);size_t turn=0;
    while(!reference.done()) {
        changing.set_compute_threads(compute::ThreadModes[turn++%compute::ThreadModes.size()].count);
        check(reference.step()&&changing.step(),"Boundary reconfiguration failed");Frame(reference).equal(changing);
    }
    save_checkpoint(changing,dir/"reconfigured.nscp");check(bytes(dir/"reconfigured.nscp")==expected,"Repeated pool reconfiguration changed checkpoint");
    std::cout<<"parallel: same-size kinetic/noise edits"<<std::endl;
    Simulator edited(c),edited_reference(c);edited.set_compute_threads(4);
    while(!edited_reference.done()) {
        if(edited_reference.steps==20) {
            for(auto* s:{&edited,&edited_reference}) {
                // The enabled/topology fields stay fixed: changing them after
                // source events requires a model reset for checkpoint replay.
                s->config.synapses[1].gain=.71;s->config.synapses[4].tau_d=.001;
                s->config.noise_sigma=.9;s->config.neurons[0].individual_noise=true;s->config.neurons[0].noise_sigma=.41;
            }
        }
        check(edited_reference.step()&&edited.step(),"Boundary parameter refresh failed");Frame(edited_reference).equal(edited);
    }
    save_checkpoint(edited,dir/"edited-parallel.nscp");save_checkpoint(edited_reference,dir/"edited-serial.nscp");check(bytes(dir/"edited-parallel.nscp")==bytes(dir/"edited-serial.nscp"),"Derived worker views remained stale after same-size parameter edits");
    for(int invalid:{-1,3,5,7,9,10,11,13,14,15,17,128}) {
        bool rejected=false;try{changing.set_compute_threads(invalid);}catch(const std::exception&){rejected=true;}
        check(rejected,"Unsupported thread selection accepted");
    }
}
void fallback(const std::filesystem::path& dir) {
    std::vector<Config> cases;
    auto c=fixture();c.name="Mixed model fallback";c.duration=.001;c.synapses[0].model=SynapseModel::ExponentialCurrent;cases.push_back(c);
    c=preset(2);c.name="HH astro fallback";c.duration=.001;c.sample=.0001;cases.push_back(c);
    c=preset(4);c.name="Small network fallback";c.duration=.001;c.sample=.0001;cases.push_back(c);
    for(size_t index=0;index<cases.size();++index)for(int count:{6,8,12,16,0}) {
        Simulator serial(cases[index]),threaded(cases[index]);serial.set_compute_threads(1);threaded.set_compute_threads(count);
        while(!serial.done()){check(serial.step()&&threaded.step(),"Serial fallback failed");Frame(serial).equal(threaded);}
        check(threaded.compute_threads()==count&&threaded.effective_compute_threads()==1,"Unsupported/small model did not explicitly report serial fallback");
        save_checkpoint(serial,dir/"fallback-reference.nscp");save_checkpoint(threaded,dir/"fallback-threaded.nscp");check(bytes(dir/"fallback-reference.nscp")==bytes(dir/"fallback-threaded.nscp"),"Fallback changed checkpoint");
    }
}
void failure_equivalence() {
    auto c=fixture();c.duration=.001;
    // Avoid threshold events in this test: invalid coordinates must be rejected
    // by the error norm / physical bounds before any trajectory is committed.
    for(auto& n:c.neurons)n.voltage=-65;
    for(int scenario=0;scenario<6;++scenario)for(int count:{6,12,16}) {
        Simulator serial(c),threaded(c);serial.set_compute_threads(1);threaded.set_compute_threads(count);
        auto invalid=serial.state;
        if(scenario==0)invalid[2]=std::numeric_limits<double>::quiet_NaN();
        else if(scenario==1)invalid[syn_offset(c,1800)+3]=std::numeric_limits<double>::infinity();
        else if(scenario==2)invalid[syn_offset(c,768)]=-std::numeric_limits<double>::infinity();
        else if(scenario==3)invalid[syn_offset(c,1700)+1]=1.25;
        else if(scenario==4)invalid[syn_offset(c,1200)+2]=-.25;
        else {invalid[syn_offset(c,1900)+1]=.8;invalid[syn_offset(c,1900)+2]=.8;}
        serial.state=invalid;threaded.state=invalid;
        const bool serial_ok=serial.step(),parallel_ok=threaded.step();
        check(!serial_ok&&!parallel_ok,"Invalid numerical coordinates were accepted");
        check(!serial.error.empty()&&serial.error==threaded.error,"Parallel failure changed numerical diagnostic");
        check(serial.time==0&&serial.steps==0&&bits(serial.state,invalid),"Rejected trial committed invalid trajectory state");
        check(threaded.effective_compute_threads()==count,"Failure fixture did not use parallel error norm/bounds");
        Frame(serial).equal(threaded);
        // Deliberately invalid runtime states are never exported as checkpoints.
    }
}
struct RunnerHarness {
    SimulationRunner runner;SimulationView view;
    explicit RunnerHarness(const Config& c):runner(c){call(command(Kind::Barrier));}
    void poll(){if(auto p=runner.take_snapshot())check(view.apply(p),"Parallel runner lost snapshot sequence");}
    RunnerResult wait(const std::shared_future<RunnerResult>& f) {
        const auto deadline=Clock::now()+std::chrono::seconds(30);
        while(f.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){poll();check(Clock::now()<deadline,"Parallel runner timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        auto result=f.get();poll();return result;
    }
    RunnerResult call(RunnerCommand c,bool success=true){auto r=wait(runner.submit(std::move(c)));if(success&&!r.ok)throw std::runtime_error(r.error);check(r.ok==success,"Unexpected controller result");return r;}
    void threads(int count){auto c=command(Kind::ComputeThreads);c.index=count;call(c);check(view.metrics.compute_threads==count,"Controller did not acknowledge thread selection");}
    void save(const std::filesystem::path& path){auto c=command(Kind::SaveCheckpoint);c.path=path;call(std::move(c));}
};
void scheduled_protocols(const std::filesystem::path& dir) {
    auto c=fixture(true);c.name="Parallel scheduled stages and shared source";c.duration=.01;
    Stimulator source;source.id=1;source.name="Scheduled shared input";source.recipients={0,1,48,63};
    source.signal.start=.000125;source.signal.stop=.01;source.signal.frequency=200;source.signal.width=.0004;source.signal.amplitude=18;
    c.stimulators={source};c.next_stimulator_id=2;
    for(int i=0;i<3;++i) {
        ExperimentStage stage;stage.name="Stage "+std::to_string(i);stage.duration=.001;
        stage.learning=i!=1;stage.clear_traces=i==2;stage.learning_rate=.75;
        stage.neurons={2,5};stage.stimulator_ids={1};stage.stimulus_enabled=true;
        stage.amplitude=i==1?12:30;stage.frequency=200;stage.width=.0004;stage.first_pulse=i==2?.0003:.000125;
        c.stages.push_back(stage);
    }
    check(validate(c).empty(),"Scheduled parallel fixture is invalid");
    Simulator serial(c),threaded(c);serial.set_compute_threads(1);threaded.set_compute_threads(4);
    serial.enter_stage(0,true);threaded.enter_stage(0,true);
    State frozen_weights;int previous_stage=0;
    while(!serial.done()) {
        check(serial.step()&&threaded.step(),"Parallel automatic stage execution failed");Frame(serial).equal(threaded);
        if(serial.experiment.stage_index==1) {
            if(frozen_weights.empty())for(const auto& p:serial.plasticity)frozen_weights.push_back(p.weight);
            for(size_t i=0;i<frozen_weights.size();++i)check(bits(frozen_weights[i],serial.plasticity[i].weight),"Frozen stage changed weights");
        }
        if(previous_stage==1&&serial.experiment.stage_index==2)
            for(const auto& p:serial.plasticity)check(p.pre_trace==0&&p.post_trace==0&&p.post_slow_trace==0,"Automatic stage did not clear STDP traces at its boundary");
        previous_stage=serial.experiment.stage_index;
    }
    check(threaded.effective_compute_threads()==4&&serial.experiment.stage_index==2&&!serial.experiment.automatic,"Automatic protocol missed its final stage");
    check(!frozen_weights.empty()&&serial.history.shared_pulses.size()>=3,"Fixture missed frozen stage or shared pulses");
    const double freeze_at=serial.experiment.protocols.at(2).time;
    check(std::any_of(serial.history.deliveries.begin(),serial.history.deliveries.end(),[&](const Delivery& e){return e.t_emit<freeze_at&&e.t_arrival>=freeze_at;}),"No delayed delivery crossed the learning-stage boundary");
    save_checkpoint(serial,dir/"scheduled-serial.nscp");save_checkpoint(threaded,dir/"scheduled-parallel.nscp");
    const auto expected=bytes(dir/"scheduled-serial.nscp");check(bytes(dir/"scheduled-parallel.nscp")==expected,"Parallel stages/shared stimuli changed checkpoint");
    RunnerHarness h(c);h.threads(4);auto enter=command(Kind::EnterStage);enter.stages=c.stages;enter.index=0;enter.flag=true;h.call(enter);
    h.call(command(Kind::RunTo,serial.time));h.save(dir/"scheduled-runner.nscp");
    check(h.view.metrics.effective_compute_threads==4&&bytes(dir/"scheduled-runner.nscp")==expected,"Runner changed model-time stage/stimulator schedule");
}
void runner_lifecycle(const std::filesystem::path& dir) {
    auto c=fixture(true);c.duration=.1;c.sample=.001;RunnerHarness h(c);Simulator reference(c);reference.set_compute_threads(1);
    for(int count:{2,4,6,8,12,16,1,0}) {
#ifdef _WIN32
        const auto before_change=process_threads();
#endif
        // Also change modes while running: acknowledgement is between steps.
        h.call(command(Kind::Run));h.threads(count);h.call(command(Kind::Pause));
#ifdef _WIN32
        const auto after_change=process_threads();
        const int effective=h.view.metrics.effective_compute_threads;
        check(after_change.size()<=before_change.size()+size_t(effective-1),"Pool replacement retained old helper threads");
#endif
        h.call(command(Kind::Advance,.00025));
        while(reference.steps<h.view.steps)check(reference.step(),"Reference could not reach runner boundary");
        check(bits(h.view.state,reference.state),"Runner thread change changed state");
        h.call(command(Kind::Run));auto stopped=h.call(command(Kind::Pause));
        while(reference.steps<stopped.steps)check(reference.step(),"Reference could not reach pause boundary");
        check(!h.view.metrics.running,"Parallel controller acknowledged pause before stopping");
        const auto steps=h.view.steps;std::this_thread::sleep_for(std::chrono::milliseconds(2));h.call(command(Kind::Barrier));check(h.view.steps==steps,"Helper threads advanced after confirmed pause");
        h.save(dir/"runner-live.nscp");save_checkpoint(reference,dir/"runner-direct.nscp");check(bytes(dir/"runner-live.nscp")==bytes(dir/"runner-direct.nscp"),"Runner pause/thread change differs from serial checkpoint");
    }
    h.threads(16);h.save(dir/"runner-restore.nscp");const auto prefix=bytes(dir/"runner-restore.nscp");
    h.threads(12);h.call(command(Kind::Advance,.00025));auto restore=command(Kind::Restore);restore.path=dir/"runner-restore.nscp";h.call(restore);
    check(h.view.metrics.compute_threads==12,"Restore replaced runtime thread preference");h.save(dir/"runner-restored.nscp");check(bytes(dir/"runner-restored.nscp")==prefix,"Cross-count runner restore changed mathematical state");
    auto reset=command(Kind::Reset);reset.config=c;h.call(reset);check(h.view.metrics.compute_threads==12&&h.view.time==0,"Reset lost thread preference or retained old trajectory");
    auto invalid=command(Kind::ComputeThreads);invalid.index=3;h.call(invalid,false);check(h.view.metrics.compute_threads==12,"Rejected thread selection mutated preference");
    c.duration=100;c.sample=.1;
    for(int count:{2,4,6,8,12,16}) {
#ifdef _WIN32
        const auto before=process_threads();
#endif
        RunnerHarness active(c);active.threads(count);active.call(command(Kind::Advance,.00005));
        check(active.view.steps>0&&active.view.metrics.effective_compute_threads==count,"Shutdown fixture did not use selected helpers");
#ifdef _WIN32
        const auto live=process_threads();std::vector<DWORD> created;
        std::set_difference(live.begin(),live.end(),before.begin(),before.end(),std::back_inserter(created));
        check(created.size()>=size_t(count),"Shutdown fixture did not expose controller and helper threads");
#endif
        active.call(command(Kind::Run));auto target=active.runner.submit(command(Kind::RunTo,50));const auto start=Clock::now();active.runner.shutdown();
        check(Clock::now()-start<std::chrono::seconds(10),"Parallel shutdown failed to join helpers promptly");
        check(target.wait_for(std::chrono::seconds(0))==std::future_status::ready,"Parallel shutdown orphaned target future");
#ifdef _WIN32
        const auto remaining=process_threads();
        for(DWORD id:created)check(!std::binary_search(remaining.begin(),remaining.end(),id),"Explicit shutdown retained an owned controller/helper thread");
#endif
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--write-fixture") {save_config(fixture(true),std::filesystem::u8path(argv[2]));return 0;}
        const auto dir=std::filesystem::current_path()/"parallel-tests"/std::to_string(Clock::now().time_since_epoch().count());std::filesystem::create_directories(dir);
        std::cout<<"parallel: pool phases and exception barriers"<<std::endl;pool_phases();
        automatic_policy();
        std::cout<<"parallel: exact numerical matrix"<<std::endl;numerical_matrix(dir);
        resume_and_reconfigure(dir);
        std::cout<<"parallel: unsupported/small model fallback"<<std::endl;fallback(dir);
        std::cout<<"parallel: nonfinite and physical-bound rejection"<<std::endl;failure_equivalence();
        std::cout<<"parallel: automatic stages and shared stimuli"<<std::endl;scheduled_protocols(dir);
        std::cout<<"parallel: runner lifecycle and native thread cleanup"<<std::endl;runner_lifecycle(dir);
        std::cout<<"PASS: "<<checks<<" parallel checks; "<<compared_steps<<" exact macro states; threads "<<compute::choices()<<", repeats, full checkpoint bytes, cross-count resume, fallback and controller lifecycle. Artifacts: "<<dir.u8string()<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
