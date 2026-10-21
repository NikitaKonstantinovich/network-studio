#include "io.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace studio;
static int checks=0;
static void check(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error);check(s.error.empty(),s.error);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
template<class F> static void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}check(rejected,"Invalid operation accepted");}
static Config quiet() {
    auto c=preset(4);c.duration=.004;c.dt=.0005;c.sample=.001;
    auto& n=c.neurons[0];n.amplitude=0;n.current=3;return c;
}
static void ranges() {
    for(int kind=0;kind<5;++kind) {
        auto c=quiet();
        if(kind==1)c.neurons[0].model=NeuronModel::HodgkinHuxley;
        if(kind==2){auto n=preset(3).neurons[0];c.neurons.push_back(n);}
        if(kind>=3){c.astrocytes=preset(0).astrocytes;c.astrocytes[0].feedback=kind==4;}
        const auto r=integration_step_range(c);const double maximum=kind==0?500e-6:50e-6;
        check(r.minimum==1e-6&&r.maximum==maximum,"Composition-dependent range");
        for(double dt:{r.minimum,r.maximum}){c.dt=dt;check(validate(c).empty(),validate(c));check(deserialize(serialize(c)).dt==dt,"JSON boundary roundtrip");}
        for(double dt:{std::nextafter(r.minimum,0.),std::nextafter(r.maximum,1.),0.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            c.dt=dt;check(!validate(c).empty(),"Out-of-range dt");if(std::isfinite(dt))rejects([&]{deserialize(serialize(c));});
        }
    }
    auto c=quiet();c.sample=.0001;check(validate(c).find("sample")!=std::string::npos,"Missing recording conflict");
    c.sample=.001;c.noise_interval=.0001;check(validate(c).find("noise_interval")!=std::string::npos,"Missing noise interval conflict");
    c=quiet();const auto initial=serialize(c);Simulator s(c);finish(s);check(serialize(c)==initial,"Validation modified input");
    check(s.integration.accepted==8&&s.integration.rejected==0,"Equilibrium counts RK stages or trial probes");
    check(s.integration.mean_step(s.time)==s.time/8,"Mean step is not actual accepted advancement");
    Simulator reset(c);check(reset.integration.accepted==0&&reset.integration.rejected==0&&reset.integration.mean_step(0)==0,"Reset statistics");
}
static void boundaries(const std::filesystem::path& dir) {
    // A 10 us pulse, including a table, is resolved by an actual state comparison
    // with a 1 us calculation, not just by testing exported pulse labels.
    for(bool table:{false,true})for(double phase:{0.,101e-6,491e-6,501e-6}) {
        auto c=quiet();c.duration=.006;c.neurons[0].amplitude=100;c.neurons[0].frequency=200;
        c.neurons[0].start=phase;c.neurons[0].stop=.005513;c.neurons[0].width=10e-6;
        if(table)c.neurons[0].pulse_intervals={{3e-6,13e-6},{211e-6,221e-6}};
        auto f=c;f.dt=1e-6;Simulator a(c),b(f);finish(a);finish(b);
        for(size_t k=0;k<a.state.size();++k)check(std::abs(a.state[k]-b.state[k])<1e-4,"Narrow stimulus state error");
        check(a.history.time==b.history.time,"Sample clock changed with internal pieces");
        check(a.history.stimuli.size()==b.history.stimuli.size(),"Pulse count changed");
        for(size_t k=0;k<a.history.stimuli.size();++k)check(a.history.stimuli[k].time==b.history.stimuli[k].time,"Pulse time changed");
        check(a.integration.accepted>a.steps,"Short input boundaries not counted");
    }
    auto c=quiet();c.duration=.002;c.neurons.clear();
    for(int i=0;i<4;++i){auto n=quiet().neurons[0];n.name="Near threshold "+std::to_string(i);n.voltage=29-i*4;n.equilibrium_u=false;n.izh_u=0;c.neurons.push_back(n);}
    for(int i=0;i<3;++i){Synapse s;s.source=i;s.target=3;s.model=SynapseModel::TsodyksMarkram;s.delay=(13+24*i)*1e-6;s.stdp=true;s.gain=.01;c.synapses.push_back(s);}
    Simulator events(c);check(events.step(),events.error);
    check(events.history.events.size()>=4&&events.history.deliveries.size()>=3,"Several events/deliveries inside 500 us missing");
    check(events.integration.accepted>=5&&events.integration.accepted<100,"Root-search probes counted as accepted steps");
    for(const auto& d:events.history.deliveries)check(d.t_arrival==d.t_emit+c.synapses[d.synapse].delay,"Delivery rounded to macro grid");
    auto full=quiet();full.duration=.04;full.noise_sigma=.3;full.neurons[0].current=10;
    Simulator continuous(full),split(full);finish(continuous);
    while(split.time<.013)check(split.step(),split.error);
    const auto accepted=split.integration.accepted,rejected=split.integration.rejected;
    save_checkpoint(split,dir/"paused.nscp");auto restored=load_checkpoint(dir/"paused.nscp");
    check(restored->integration.accepted==accepted&&restored->integration.rejected==rejected,"Checkpoint lost statistics");
    restored->set_compute_threads(6);finish(*restored);
    save_checkpoint(continuous,dir/"continuous.nscp");save_checkpoint(*restored,dir/"resumed.nscp");
    check(bytes(dir/"continuous.nscp")==bytes(dir/"resumed.nscp"),"Resume is not byte exact");
    // Error control must actually reject a trial; no tolerances are changed.
    auto reject=quiet();reject.neurons[0].current=100;reject.neurons[0].izh_a=1;reject.duration=.02;
    Simulator rejected_run(reject);finish(rejected_run);check(rejected_run.integration.rejected>0,"Rejection fixture never exercised retries");
}
static std::vector<Config> fixtures() {
    std::vector<Config> all;
    auto c=quiet();c.name="RS";c.duration=.2;c.neurons[0].current=10;all.push_back(c);
    c.name="FS";c.neurons[0].izh_a=.1;c.neurons[0].izh_d=2;all.push_back(c);
    c=preset(6);c.name="pair";c.duration=.25;c.sample=.001;all.push_back(c);
    c.name="triplet";c.synapses[0].stdp_rule=StdpRule::Triplet;all.push_back(c);
    c=preset(5);c.name="coupled";c.duration=.12;c.sample=.001;c.neurons[0].start=.001;c.synapses[0].delay=137e-6;c.synapses[0].stdp=true;c.synapses[0].gain=4;
    auto second=c.synapses[0];second.source=1;second.target=0;second.delay=13e-6;second.gain=2;c.synapses.push_back(second);all.push_back(c);
    c.name="coupled_noise";c.noise_sigma=.3;all.push_back(c);return all;
}
static void convergence(const std::filesystem::path& dir) {
    std::ofstream report(dir/"convergence.csv");report<<std::setprecision(17)
        <<"case,dt_us,spikes,deliveries,max_spike_error_s,max_delivery_error_s,max_final_state_error,max_weight_error,max_trace_error,max_voltage_error_mV,accepted,rejected,mean_step_s\n";
    for(auto c:fixtures()) {
        c.dt=1e-6;Simulator reference(c);finish(reference);
        for(double dt:{25e-6,50e-6,100e-6,250e-6,500e-6}) {
            c.dt=dt;Simulator s(c);finish(s);
            check(s.spikes==reference.spikes,c.name+": spike counts");
            check(s.history.events.size()==reference.history.events.size(),c.name+": events size");
            check(s.history.deliveries.size()==reference.history.deliveries.size(),c.name+": delivery count");
            double spike=0,delivery=0,state=0,weight=0,trace=0,voltage=0;
            for(size_t i=0;i<s.history.events.size();++i){const auto& a=s.history.events[i];const auto& b=reference.history.events[i];check(a.index==b.index,c.name+": spike order");spike=std::max(spike,std::abs(a.time-b.time));}
            for(size_t i=0;i<s.history.deliveries.size();++i){const auto& a=s.history.deliveries[i];const auto& b=reference.history.deliveries[i];check(a.synapse==b.synapse,c.name+": delivery order");delivery=std::max(delivery,std::abs(a.t_arrival-b.t_arrival));}
            for(size_t i=0;i<s.state.size();++i)state=std::max(state,std::abs(s.state[i]-reference.state[i]));
            for(size_t i=0;i<s.plasticity.size();++i) {
                const auto& a=s.plasticity[i];const auto& b=reference.plasticity[i];
                weight=std::max(weight,std::abs(a.weight-b.weight));
                trace=std::max({trace,std::abs(a.pre_trace-b.pre_trace),std::abs(a.post_trace-b.post_trace),std::abs(a.post_slow_trace-b.post_slow_trace)});
            }
            check(s.history.time==reference.history.time,c.name+": history clock");
            for(size_t n=0;n<c.neurons.size();++n)for(size_t k=0;k<s.history.time.size();++k) {
                const double t=s.history.time[k];bool near=false;
                for(const auto& e:reference.history.events)if(e.index==int(n)&&std::abs(e.time-t)<10e-6){near=true;break;}
                if(!near)voltage=std::max(voltage,std::abs(s.history.values[4*n][k]-reference.history.values[4*n][k]));
            }
            report<<c.name<<','<<dt*1e6<<','<<s.history.events.size()<<','<<s.history.deliveries.size()<<','<<spike<<','<<delivery<<','<<state<<','<<weight<<','<<trace<<','<<voltage<<','<<s.integration.accepted<<','<<s.integration.rejected<<','<<s.integration.mean_step(s.time)<<'\n';report.flush();
            // Sub-microsecond spike error; trajectory budget 10 uV away from
            // reset (1e-4 of a ~100 mV spike). Final state budget is tighter.
            // These are reporting/regression criteria, not solver tolerances.
            check(spike<1e-6&&delivery<1e-6,c.name+": spike/delivery timing tolerance");
            check(state<.002&&voltage<.01,c.name+": state/voltage tolerance");
            check(weight<1e-5&&trace<1e-5,c.name+": learning tolerance");
        }
    }
}
static void benchmark(const std::filesystem::path& input,const std::filesystem::path& out,int threads) {
    check(std::filesystem::create_directory(out),"Output must be new");auto c=load_config(input);Simulator s(c);s.set_compute_threads(threads);
    std::ofstream f(out/"timing.csv");f<<std::setprecision(17)<<"from,to,solve_s,accepted,rejected,mean_step_s,spikes,deliveries,requested_threads,effective_threads\n";
    for(double stop:{.05,.1}) {
        const double start=s.time;const auto count=s.integration;auto events=s.history.events.size(),deliveries=s.history.deliveries.size();
        auto begin=std::chrono::steady_clock::now();
        while(s.time<stop&&!s.done())check(s.step(),s.error);
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        check(s.error.empty()&&s.time==stop,"Benchmark interval mismatch");
        const auto accepted=s.integration.accepted-count.accepted;
        f<<start<<','<<s.time<<','<<elapsed<<','<<accepted<<','<<s.integration.rejected-count.rejected<<','<<(s.time-start)/double(accepted)<<','<<s.history.events.size()-events<<','<<s.history.deliveries.size()-deliveries<<','<<s.compute_threads()<<','<<s.effective_compute_threads()<<'\n';
    }
    export_results(s,out/"result");save_checkpoint(s,out/"final.nscp");
    std::ofstream state(out/"final_state.csv");state<<std::setprecision(17)<<"index,value\n";
    for(size_t i=0;i<s.state.size();++i)state<<i<<','<<s.state[i]<<'\n';
}
int main(int argc,char** argv){try {
    if(argc==5){benchmark(std::filesystem::u8path(argv[1]),std::filesystem::u8path(argv[2]),std::stoi(argv[3]));return 0;}
    check(argc==2,"Usage: step_tests OUTPUT_DIR, or CONFIG NEW_OUTPUT THREADS benchmark");
    const auto out=std::filesystem::u8path(argv[1]);std::filesystem::create_directories(out);
    ranges();boundaries(out);convergence(out);
    std::cout<<"PASS "<<checks<<" step-range/integration-statistics checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
