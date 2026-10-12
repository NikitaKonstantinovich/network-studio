#include "network.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace studio;
static int checks=0;
static void require(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
static void close(double x,double y,double tol,const char* text){require(std::abs(x-y)<=tol,text);}
static void logarithmic(double x,double exact,double tol,const char* text) {
    require(x>0 && exact>0,text);require(std::abs(std::log(x)-std::log(exact))<tol,text);
}
static void finish(Simulator& sim){while(!sim.done())require(sim.step(),sim.error.c_str());require(sim.error.empty(),sim.error.c_str());}
static Config capacitor() {
    auto c=preset(3);c.duration=.016;c.sample=.00007;c.noise_sigma=0;
    c.parameters[na::g_Na]=c.parameters[na::g_K]=c.parameters[na::g_L]=0;
    auto& n=c.neurons[0];n.amplitude=100;n.current=0;n.start=.000101;n.stop=c.duration;n.width=10e-6;n.frequency=200;
    return c;
}
// Analytic intersection lengths, enumerated independently of next_input_boundary.
static double area(const Config& c) {
    const auto& n=c.neurons[0];double total=0;
    for(int k=0;k<10;++k) {
        const double origin=n.start+double(k)/n.frequency;
        auto add=[&](double a,double b){total+=std::max(0.,std::min({origin+b,n.stop,c.duration})-std::max(0.,origin+a));};
        if(n.pulse_intervals.empty())add(0,n.width);else for(const auto& interval:n.pulse_intervals)add(interval.start,interval.stop);
    }
    return total*n.amplitude;
}
static void stimulus_tests() {
    for(double dt:{50e-6,25e-6,12.5e-6})for(double phase:{0.,1e-9,101e-6,110e-6,124.999e-6})for(int mode=0;mode<3;++mode) {
        auto c=capacitor();c.dt=dt;auto& n=c.neurons[0];n.start=phase;
        if(mode==0)n.stop=n.start+10e-6; // A single pulse.
        if(mode==2){n.pulse_intervals={{3e-6,13e-6},{.00498,.005}};n.stop=n.start+.010008;}
        Simulator sim(c);finish(sim);
        close(sim.state[0]-n.voltage,1000*area(c)/c.parameters[na::C_m],2e-9,"Stimulus charge depends on grid phase or table boundaries.");
        for(size_t i=1;i+1<sim.history.time.size();++i)
            require(sim.history.time[i]==double(i)*c.sample,"Internal steps changed the sample clock.");
    }
    auto c=capacitor();auto& n=c.neurons[0];n.start=1;n.stop=2;n.frequency=17;n.pulse_intervals={{0,1/n.frequency}};
    for(int k=1;k<16;++k) {
        const double edge=n.start+k*(1/n.frequency);
        require(external_current(c,0,edge)==n.amplitude && external_current(c,0,std::nextafter(edge,0.))==n.amplitude &&
            external_current(c,0,std::nextafter(edge,3.))==n.amplitude,"Full-period table has a floating-point seam.");
    }
    n.pulse_intervals={{.003,.005}};
    const double edge=n.start+3*(1/n.frequency)+.005;
    require(external_current(c,0,std::nextafter(edge,0.))==n.amplitude && external_current(c,0,edge)==0 &&
        external_current(c,0,std::nextafter(edge,3.))==0,"Table stop is not half-open.");
    c=capacitor();c.neurons[0].amplitude=0;c.noise_sigma=2;c.noise_interval=73e-6;c.duration=.001;
    auto split=c;split.neurons.push_back(split.neurons[0]);split.neurons[1].amplitude=1;split.neurons[1].start=11e-6;
    split.neurons[1].stop=43e-6;split.neurons[1].individual_noise=true;split.neurons[1].noise_sigma=0;
    Simulator first(c),second(split);finish(first);finish(second);
    require(first.history.time==second.history.time,"Unrelated input boundaries changed output cadence.");
    for(size_t i=0;i<first.history.time.size();++i)close(first.history.values[0][i],second.history.values[0][i],5e-11,"Internal splitting changed seeded noise charge.");
    for(int k=1;k<12;++k) {
        const double t=k*c.noise_interval;
        require(external_current(c,0,t)==external_current(c,0,std::nextafter(t,1.)),"Noise update does not use its right-hand bin.");
    }
}
static void kinetics_tests() {
    for(auto model:{SynapseModel::ConductanceSTP,SynapseModel::ExponentialCurrent,SynapseModel::TsodyksMarkram}) {
        auto c=capacitor();c.duration=.001;c.neurons[0].amplitude=0;c.parameters[na::Omega_Y]=200000;
        Synapse edge;edge.source=edge.target=0;edge.model=model;edge.enabled=false;edge.tau_d=edge.tau_syn=.001;c.synapses={edge};
        Simulator sim(c);const int b=syn_offset(c,0);sim.state[b]=.3;sim.state[b+1]=.2;sim.state[b+2]=.7;sim.state[b+3]=500;
        while(!sim.done()) {
            require(sim.step(),sim.error.c_str());
            logarithmic(sim.state[b+3],500*std::exp(-200000*sim.time),2e-10,"Fast glutamate decay lost relative/logarithmic accuracy.");
            require(sim.state[b+2]>=0,"Exact synaptic decay became negative.");
            if(model==SynapseModel::TsodyksMarkram)require(sim.state[b+1]>=0 && sim.state[b+1]+sim.state[b+2]<=1+2e-13,"TM exact flow left the resource simplex.");
        }
        if(model==SynapseModel::TsodyksMarkram) {
            const double e=std::exp(-1.);
            close(sim.state[b+1],1-.8*e-.7*e,2e-13,"Coincident TM time constants have the wrong limit.");
        }
    }
    auto c=capacitor();c.duration=.001;c.neurons[0].amplitude=0;c.astrocytes={{}};
    auto& p=c.parameters;p[na::O_N]=p[na::O_G]=p[na::O_beta]=p[na::O_delta]=p[na::O_3K]=0;
    p[na::Omega_N]=p[na::Omega_S]=p[na::Omega_G]=p[na::Omega_5P]=200000;p[na::tau_A]=1e-6;
    c.astrocytes[0].feedback=false;c.astrocytes[0].threshold=1;
    Simulator sim(c);const int b=astro_offset(c,0);sim.state[b]=.7;sim.state[b+6]=.6;sim.state[b+5]=5;sim.state[b+4]=0;
    finish(sim);
    for(auto item:{std::pair<int,double>{b,.7},{b+1,c.astrocytes[0].ip3},{b+5,5},{b+6,.6}})
        logarithmic(sim.state[item.first],item.second*std::exp(-200),2e-9,"Isolated astrocyte decay failed tiny-positive log test.");
    close(sim.state[b+4],1.,2e-13,"Fast astrocyte resource did not recover.");
}
static void event_tests() {
    auto c=preset(4);c.duration=.001;c.sample=c.dt;c.neurons[0].amplitude=0;
    auto& n=c.neurons[0];n.voltage=0;n.izh_peak=1;n.izh_c=0;n.izh_d=0;n.equilibrium_u=false;n.izh_u=-1000;n.izh_a=.0001;n.izh_b=0;n.current=100;
    Simulator many(c);require(many.step(),many.error.c_str());require(many.spikes[0]>10,"Only one event was handled per main step.");
    for(size_t i=1;i<many.history.events.size();++i)require(many.history.events[i].time>many.history.events[i-1].time,"Event was duplicated at the same threshold.");
    require(many.state[0]>0 && many.state[0]<1,"The remainder after the last reset was not integrated.");
    c.neurons.push_back(c.neurons[0]);c.neurons[1].current=91;
    for(auto rule:{StdpRule::Additive,StdpRule::Multiplicative,StdpRule::Triplet}) {
        Synapse syn;syn.source=0;syn.target=1;syn.model=SynapseModel::TsodyksMarkram;syn.gain=0;
        syn.stdp=true;syn.stdp_rule=rule;syn.a_plus=.0001;syn.a_minus=.00012;syn.weight=.7;c.synapses={syn};
        Simulator rapid(c);require(rapid.step(),rapid.error.c_str());
        std::vector<double> pres,posts;double w=syn.weight;
        for(size_t first=0;first<rapid.history.events.size();) {
            const double t=rapid.history.events[first].time;bool pre=false,post=false;size_t next=first;
            while(next<rapid.history.events.size() && rapid.history.events[next].time==t) {
                if(rapid.history.events[next].index==0)pre=true;else post=true;++next;
            }
            double a=0,b=0,slow=0;
            for(double old:pres)a+=std::exp(-(t-old)/syn.tau_plus);
            for(double old:posts){b+=std::exp(-(t-old)/syn.tau_minus);slow+=std::exp(-(t-old)/syn.tau_slow);}
            const double up=rule==StdpRule::Additive ? 1 : syn.weight_max-w,down=rule==StdpRule::Additive ? 1 : w-syn.weight_min;
            w=std::clamp(w+(post ? syn.a_plus*up*a*(rule==StdpRule::Triplet ? slow : 1) : 0)-(pre ? syn.a_minus*down*b : 0),syn.weight_min,syn.weight_max);
            if(pre)pres.push_back(t);if(post)posts.push_back(t);first=next;
        }
        require(pres.size()>10 && posts.size()>10,"Rapid STDP fixture lacked repeated events within one main step.");
        close(rapid.plasticity[0].weight,w,1e-12,"Multiple events in one main step disagree with direct all-history STDP.");
        const int b=syn_offset(c,0);require(rapid.state[b+1]>=0 && rapid.state[b+2]>=0 && rapid.state[b+1]+rapid.state[b+2]<=1+1e-12,"Rapid repeated TM events violate resource invariants.");
    }
    c=capacitor();c.duration=.001;c.sample=c.dt;c.parameters[na::V_spike]=-64;
    c.neurons[0].start=0;c.neurons[0].stop=10e-6;
    Simulator boundary(c);finish(boundary);
    require(boundary.spikes[0]==1,"HH boundary event was missed or repeated.");
    close(boundary.history.events[0].time,10e-6,2e-12,"HH crossing was not localized at the stimulus stop.");
    close(boundary.state[0],-64,2e-10,"HH event incorrectly reset its voltage.");
    for(auto rule:{StdpRule::Additive,StdpRule::Multiplicative,StdpRule::Triplet}) {
        c=preset(4);c.duration=.03;c.sample=.0001;
        c.neurons[0].current=10;c.neurons[0].amplitude=0;c.neurons.push_back(c.neurons[0]);c.neurons[1].izh_a=.1;
        Synapse s;s.model=SynapseModel::TsodyksMarkram;s.source=0;s.target=1;s.gain=.5;s.stdp=true;s.stdp_rule=rule;
        c.synapses={s};Simulator original(c);finish(original);
        auto permuted=c;std::swap(permuted.neurons[0],permuted.neurons[1]);permuted.synapses[0].source=1;permuted.synapses[0].target=0;
        Simulator reversed(permuted);finish(reversed);
        close(original.plasticity[0].weight,reversed.plasticity[0].weight,2e-12,"STDP depends on neuron traversal order.");
        for(size_t i=0;i<original.history.events.size();++i) {
            require(original.history.events[i].index==1-reversed.history.events[i].index,"Permutation changed event identity.");
            close(original.history.events[i].time,reversed.history.events[i].time,2e-12,"Permutation changed event time.");
        }
        c.neurons[1]=c.neurons[0];c.synapses[0].gain=0;c.duration=.01;
        Simulator simultaneous(c);finish(simultaneous);
        require(simultaneous.spikes[0]==simultaneous.spikes[1] && simultaneous.spikes[0]>0,"Simultaneous fixture failed.");
        close(simultaneous.plasticity[0].weight,s.weight,2e-12,"Fresh simultaneous events learned a zero-lag pair.");
    }
}
int main(){try{stimulus_tests();kinetics_tests();event_tests();std::cout<<"PASS numerical regressions: "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
