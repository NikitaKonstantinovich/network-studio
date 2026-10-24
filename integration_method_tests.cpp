#include "io.h"
#include "history_archive.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace studio;
static int checks=0;
static void check(bool b,const std::string& why){++checks;if(!b)throw std::runtime_error(why);}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error);check(s.error.empty(),s.error);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static Config quiet(){auto c=preset(4);c.duration=.004;c.dt=.0005;c.sample=.001;c.neurons[0].amplitude=0;c.neurons[0].current=3;return c;}
static void analytic(const std::filesystem::path& out) {
    std::ofstream report(out/"orders.csv");report<<std::setprecision(17)<<"method,problem,h,error\n";
    for(const auto& mode:IntegrationMethods) {
        numerical::Flow f(2);f.method=mode.method;int calls=0;
        f.rhs=[&](const State& y,State& dy){++calls;dy[0]=y[1];dy[1]=-y[0];};
        f.exact=[](const State&,double,State&){};
        State y{1,0};const auto norm=f.trial(y,.1);
        check(calls==(mode.method==IntegrationMethod::Euler?1:mode.method==IntegrationMethod::RK4?4:12),"Hidden step doubling");
        check((mode.method==IntegrationMethod::AdaptiveRK4)==std::isfinite(norm),"Fictitious fixed-method error estimate");
        for(int problem=0;problem<2;++problem) {
            if(problem==1) {
                f.rhs=[](const State& y,State& dy){dy[0]=y[1];dy[1]=-y[1];};
                f.exact=[](const State& y,double h,State& z){z[1]=y[1]*std::exp(-h);};
            }
            double previous=0;
            for(double h:{.1,.05,.025,.0125}) {
                y=problem==0?State{1,0}:State{0,1};
                for(int k=0;k<int(std::lround(1/h));++k){f.sample(y,h,f.next);y.swap(f.next);}
                const double error=problem==0?std::hypot(y[0]-std::cos(1.),y[1]+std::sin(1.)):std::abs(y[0]-(1-std::exp(-1.)));
                report<<mode.argument<<','<<problem<<','<<h<<','<<error<<'\n';
                if(previous)check(previous/error>(mode.method==IntegrationMethod::Euler?1.85:14)&&previous/error<(mode.method==IntegrationMethod::Euler?2.2:18),"Wrong analytic convergence order");
                if(problem==1)check(std::abs(y[1]-std::exp(-1.))<3e-14,"Exact transition lost");
                previous=error;
            }
        }
        // y'=y: the root must lie on the selected one-step polynomial, not
        // on an interpolated CSV or the adaptive method's different flow.
        numerical::Flow root(1);root.method=mode.method;
        root.rhs=[](const State& y,State& dy){dy[0]=y[0];};root.exact=[](const State&,double,State&){};
        State old{1};root.trial(old,1);std::vector<int> group;
        const double t=root.events(old,1,0,{{0,1.5,true}},group);
        check(group==std::vector<int>{0},"Threshold missing");
        auto R=[](double x){return 1+x+x*x/2+x*x*x/6+x*x*x*x/24;};
        const double value=mode.method==IntegrationMethod::Euler?1+t:mode.method==IntegrationMethod::RK4?R(t):R(t/2)*R(t/2);
        check(std::abs(value-1.5)<3e-13&&std::abs(root.next[0]-1.5)<3e-13,"Event state uses another method");
    }
}
static void ranges() {
    for(const auto& mode:IntegrationMethods)for(int kind=0;kind<5;++kind) {
        auto c=quiet();c.integration_method=mode.method;
        if(kind==1)c.neurons[0].model=NeuronModel::HodgkinHuxley;
        if(kind==2)c.neurons.push_back(preset(3).neurons[0]);
        if(kind>=3){c.astrocytes=preset(0).astrocytes;c.astrocytes[0].feedback=kind==4;}
        const auto cap=integration_capabilities(c);const bool supported=kind==0||mode.method==IntegrationMethod::AdaptiveRK4;
        check(cap.supports(mode.method)==supported,"Composition method policy");
        for(double dt:{1e-6,50e-6,500e-6}) {
            c.dt=dt;const bool valid=supported&&dt<=cap.step.maximum;
            check(validate(c).empty()==valid,"Method/dt validation");
            if(valid)check(deserialize(serialize(c)).integration_method==mode.method,"Method JSON roundtrip");
            else {bool rejected=false;try{deserialize(serialize(c));}catch(...){rejected=true;}check(rejected,"Invalid JSON loaded");}
        }
    }
}
static void boundaries(const std::filesystem::path& out) {
    for(const auto& mode:IntegrationMethods) {
        auto c=quiet();c.integration_method=mode.method;c.duration=.006;
        c.neurons[0].start=101e-6;c.neurons[0].stop=.005513;c.neurons[0].frequency=200;
        c.neurons[0].amplitude=100;c.neurons[0].pulse_intervals={{3e-6,13e-6},{211e-6,221e-6}};
        Simulator s(c);finish(s);
        check(s.history.stimuli.size()==4,"Narrow pulses missing/duplicated");
        const double times[]={104e-6,312e-6,.005104,.005312};
        for(int k=0;k<4;++k)check(std::abs(s.history.stimuli[k].time-times[k])<1e-17,"Pulse rounded to macro grid");
        check(s.integration.accepted>s.steps,"Input boundaries ignored");
        // The pulse changes the actual state, including in Euler mode.
        auto off=c;off.neurons[0].amplitude=0;Simulator no_pulse(off);finish(no_pulse);
        check(std::abs(s.state[0]-no_pulse.state[0])>.01,"Pulse absent from dynamics");
        auto fine=c;fine.dt=1e-6;fine.integration_method=IntegrationMethod::AdaptiveRK4;Simulator ref(fine);finish(ref);
        check(std::abs(s.state[0]-ref.state[0])<(mode.method==IntegrationMethod::Euler?.1:.001),"Narrow pulse area/trajectory error");
        c=quiet();c.integration_method=mode.method;c.duration=.002;c.neurons.clear();
        for(int i=0;i<5;++i){auto n=quiet().neurons[0];n.name=std::to_string(i);n.voltage=29-(i<2?0:i*4);n.equilibrium_u=false;n.izh_u=0;c.neurons.push_back(n);}
        for(int i=0;i<4;++i){Synapse e;e.source=i;e.target=4;e.model=SynapseModel::TsodyksMarkram;e.delay=(i<2?13:13+24*i)*1e-6;e.stdp=true;e.gain=.01;c.synapses.push_back(e);}
        Simulator events(c);check(events.step(),events.error);
        check(events.history.events.size()>=5&&events.history.deliveries.size()>=4,"Several events in one macro step missing");
        check(events.history.events[0].time==events.history.events[1].time,"Simultaneous spikes split");
        for(const auto& d:events.history.deliveries)check(d.t_arrival==d.t_emit+c.synapses[d.synapse].delay,"Delay rounded");
        check(events.integration.accepted<100,"Root probes counted as accepted");
        // Exact linear decays remain positive even where explicit Euler would
        // invert their sign. Independent logarithmic comparison excludes zero.
        c=quiet();c.integration_method=mode.method;c.duration=.001;
        Synapse edge;edge.source=edge.target=0;edge.model=SynapseModel::TsodyksMarkram;edge.enabled=false;c.synapses={edge};c.parameters[na::Omega_Y]=200000;
        Simulator decay(c);const int b=syn_offset(c,0);decay.state[b]=.3;decay.state[b+1]=.2;decay.state[b+2]=.7;decay.state[b+3]=500;
        finish(decay);check(decay.state[b+3]>0&&std::abs(std::log(decay.state[b+3])-(std::log(500.)-200))<1e-10,"Fast exact decay");
        check(decay.state[b+1]>=0&&decay.state[b+2]>=0&&decay.state[b+1]+decay.state[b+2]<=1+1e-12,"TM invariant");
        // No-noise quiet equilibrium: one accepted interval, not RK stages.
        c=quiet();c.integration_method=mode.method;Simulator counts(c);finish(counts);
        check(counts.integration.accepted==8&&counts.integration.rejected==0,"Fixed method internal counts");
        check(mode.method==IntegrationMethod::AdaptiveRK4||s.integration.error_rejected==0,"Fixed method performed error rejection");
        if(mode.method!=IntegrationMethod::AdaptiveRK4) {
            Simulator invalid(c);invalid.state[0]=-199;invalid.state[1]=100000;
            check(!invalid.step()&&!invalid.error.empty(),"Physical failure silently accepted");
            check(invalid.integration.state_reductions>0&&invalid.integration.error_rejected==0&&
                invalid.integration.rejected==invalid.integration.state_reductions,"Physical reductions misclassified");
        }
        // Snapshot/restore with seeded noise, pending arrivals and triplet traces.
        c=preset(5);c.integration_method=mode.method;c.dt=.0005;c.sample=.001;c.duration=.03;c.noise_sigma=.3;
        c.neurons[0].current=10;c.neurons[0].amplitude=0;c.synapses[0].delay=.00737;c.synapses[0].stdp=true;c.synapses[0].stdp_rule=StdpRule::Triplet;
        Simulator continuous(c),split(c);finish(continuous);while(split.time<.005)check(split.step(),split.error);
        const auto p=out/(std::string(mode.argument)+".nscp");save_checkpoint(split,p);auto resumed=load_checkpoint(p);resumed->set_compute_threads(6);finish(*resumed);
        save_checkpoint(continuous,out/"a.nscp");save_checkpoint(*resumed,out/"b.nscp");check(bytes(out/"a.nscp")==bytes(out/"b.nscp"),"Resume not byte exact");
    }
}
static std::vector<Config> fixtures() {
    std::vector<Config> all;auto c=quiet();c.name="RS";c.duration=.2;c.neurons[0].current=10;all.push_back(c);
    c.name="FS";c.neurons[0].izh_a=.1;c.neurons[0].izh_d=2;all.push_back(c);
    c=preset(6);c.name="pair";c.duration=.25;c.sample=.001;all.push_back(c);
    c.name="triplet";c.synapses[0].stdp_rule=StdpRule::Triplet;all.push_back(c);
    c=preset(5);c.name="coupled_noise";c.duration=.12;c.sample=.001;c.noise_sigma=.3;c.neurons[0].start=.001;
    c.synapses[0].delay=137e-6;c.synapses[0].stdp=true;c.synapses[0].gain=4;
    auto e=c.synapses[0];e.source=1;e.target=0;e.delay=13e-6;e.gain=2;c.synapses.push_back(e);all.push_back(c);return all;
}
static void convergence(const std::filesystem::path& out) {
    std::ofstream report(out/"accuracy.csv");report<<std::setprecision(17)<<"case,method,dt_us,spikes,reference_spikes,spike_error_s,voltage_error_mV,state_error,weight_error,trace_error,accepted,error_rejections,state_reductions,solve_s,accurate\n";
    for(auto c:fixtures()) {
        c.dt=1e-6;Simulator ref(c);finish(ref);
        for(const auto& mode:IntegrationMethods)for(double dt:{1e-6,25e-6,50e-6,100e-6,250e-6,500e-6}) {
            c.dt=dt;c.integration_method=mode.method;Simulator s(c);const auto begin=std::chrono::steady_clock::now();finish(s);
            const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
            double spike=0,voltage=0,state=0,weight=0,trace=0;
            for(size_t n=0;n<c.neurons.size();++n) {
                std::vector<double> a,b;for(const auto& e:s.history.events)if(!e.astro&&e.index==int(n))a.push_back(e.time);
                for(const auto& e:ref.history.events)if(!e.astro&&e.index==int(n))b.push_back(e.time);
                if(a.size()!=b.size())spike=std::numeric_limits<double>::infinity();
                for(size_t k=0;k<std::min(a.size(),b.size());++k)spike=std::max(spike,std::abs(a[k]-b[k]));
                for(size_t k=0;k<s.history.time.size();++k) {
                    double t=s.history.time[k];bool near=false;
                    for(auto v:a)if(std::abs(v-t)<.002)near=true;
                    for(auto v:b)if(std::abs(v-t)<.002)near=true;
                    if(!near)voltage=std::max(voltage,std::abs(s.history.values[4*n][k]-ref.history.values[4*n][k]));
                }
            }
            for(size_t k=0;k<s.state.size();++k)state=std::max(state,std::abs(s.state[k]-ref.state[k]));
            for(size_t k=0;k<s.plasticity.size();++k){const auto& a=s.plasticity[k];const auto& b=ref.plasticity[k];weight=std::max(weight,std::abs(a.weight-b.weight));trace=std::max({trace,std::abs(a.pre_trace-b.pre_trace),std::abs(a.post_trace-b.post_trace),std::abs(a.post_slow_trace-b.post_slow_trace)});}
            check(s.history.time==ref.history.time,"Recording clock differs");
            // Predeclared research-screening budget, not altered solver tolerances:
            // 0.1 ms event timing, 0.1 mV outside +/-2 ms of either reset, 1e-3
            // absolute weights/traces. Coarse Euler failures are reported, not hidden.
            bool accurate=spike<1e-4&&voltage<.1&&weight<1e-3&&trace<1e-3;
            report<<c.name<<','<<mode.argument<<','<<dt*1e6<<','<<s.history.events.size()<<','<<ref.history.events.size()<<','<<spike<<','<<voltage<<','<<state<<','<<weight<<','<<trace<<','<<s.integration.accepted<<','<<s.integration.error_rejected<<','<<s.integration.state_reductions<<','<<wall<<','<<accurate<<'\n';report.flush();
            if(mode.method!=IntegrationMethod::Euler)check(accurate,c.name+": RK4 accuracy regression");
        }
    }
}
int main(int argc,char** argv){try {
    check(argc==2,"Usage: integration_method_tests OUTPUT");const auto out=std::filesystem::u8path(argv[1]);std::filesystem::create_directories(out);
    analytic(out);ranges();boundaries(out);convergence(out);std::cout<<"PASS "<<checks<<" integration-method checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
