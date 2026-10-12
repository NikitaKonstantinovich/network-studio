#include "io.h"
#include "generator.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <map>
#include <limits>
using namespace studio;
static int checks=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
static void close(double x,double y,double tolerance,const char* message){check(std::abs(x-y)<=tolerance,message);}
template<class F> static void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,"invalid checkpoint/config accepted");}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error.c_str());check(s.error.empty(),s.error.c_str());}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"missing test file");return {std::istreambuf_iterator<char>(f),{}};}
static void equal(const Simulator& a,const Simulator& b) {
    check(a.state==b.state&&a.spikes==b.spikes&&a.releases==b.releases&&a.time==b.time&&a.steps==b.steps,"runtime mismatch");
    check(a.history.time==b.history.time&&a.history.values==b.history.values&&a.history.weights==b.history.weights,"history mismatch");
    check(a.plasticity.size()==b.plasticity.size(),"plasticity shape");
    for(size_t j=0;j<a.plasticity.size();++j) {auto x=a.plasticity[j],y=b.plasticity[j];
        check(x.weight==y.weight&&x.pre_trace==y.pre_trace&&x.post_trace==y.post_trace&&x.post_slow_trace==y.post_slow_trace,"learning mismatch");}
    check(a.history.events.size()==b.history.events.size(),"event count mismatch");
    for(size_t k=0;k<a.history.events.size();++k){auto x=a.history.events[k],y=b.history.events[k];check(x.time==y.time&&x.index==y.index&&x.astro==y.astro,"event mismatch");}
    auto deliveries=[](std::vector<Delivery> x,std::vector<Delivery> y) {
        auto less=[](auto a,auto b){return DeliveryLater{}(b,a);};std::sort(x.begin(),x.end(),less);std::sort(y.begin(),y.end(),less);
        check(x.size()==y.size(),"delivery count mismatch");for(size_t k=0;k<x.size();++k)check(x[k].t_emit==y[k].t_emit&&x[k].t_arrival==y[k].t_arrival&&x[k].synapse==y[k].synapse,"delivery mismatch");
    };
    deliveries(a.history.deliveries,b.history.deliveries);deliveries(a.pending_deliveries,b.pending_deliveries);
}
static Config capacitor() {
    Config c;c.name="controlled capacitor spikes";c.dt=.000025;c.duration=.030;c.sample=.00005;
    c.parameters[na::g_Na]=c.parameters[na::g_K]=c.parameters[na::g_L]=0;c.parameters[na::C_m]=1;c.parameters[na::V_spike]=0;
    Neuron pre;pre.name="pre at 10 ms";pre.amplitude=0;pre.voltage=-10;pre.current=1;
    Neuron post=pre;post.name="post at 15 ms";post.voltage=-15;
    c.neurons={pre,post};Synapse s;s.model=SynapseModel::ExponentialCurrent;s.gain=0;s.delay=.010;s.stdp=true;
    c.synapses={s};return c;
}
// Independent direct sum of past events, not recurrence of the production traces.
static double reference(const Synapse& s,const std::vector<double>& pres,const std::vector<double>& posts,double end) {
    std::map<double,std::pair<int,int>> timeline;
    for(double t:pres)if(t<=end)++timeline[t].first;for(double t:posts)if(t<=end)++timeline[t].second;
    double weight=s.weight;
    for(auto [t,n]:timeline) {
        double p=0,q=0,z=0;for(double x:pres)if(x<t)p+=std::exp(-(t-x)/s.tau_plus);
        for(double x:posts)if(x<t){q+=std::exp(-(t-x)/s.tau_minus);z+=std::exp(-(t-x)/s.tau_slow);}
        double up=s.stdp_rule==StdpRule::Additive?1:s.weight_max-weight,down=s.stdp_rule==StdpRule::Additive?1:weight-s.weight_min;
        const double delta=n.second*s.a_plus*up*p*(s.stdp_rule==StdpRule::Triplet?z:1)-n.first*s.a_minus*down*q;
        weight=std::clamp(weight+delta,s.weight_min,s.weight_max);
    }return weight;
}
static void learning_tests() {
    for(auto rule:{StdpRule::Additive,StdpRule::Multiplicative,StdpRule::Triplet})for(auto mode:{StdpTiming::Arrival,StdpTiming::Emission}) {
        Synapse s;s.stdp=true;s.stdp_rule=rule;s.stdp_timing=mode;s.delay=.010;StdpState p;
        std::vector<double> emitted={.010,.028,.040,.040},posts={.004,.015,.033,.050,.050};
        std::vector<double> used;std::map<double,std::array<uint64_t,3>> timeline;
        for(double t:emitted){++timeline[t][0];++timeline[t+s.delay][1];used.push_back(mode==StdpTiming::Arrival?t+s.delay:t);}
        for(double t:posts)++timeline[t][2];
        double last=0;for(auto [t,n]:timeline){update_synaptic_stdp(s,p,n[0],n[1],n[2],t-last);last=t;
            close(p.weight,reference(s,used,posts,t),4e-15,"independent pair/triplet weight mismatch");}
    }
    for(auto mode:{StdpTiming::Arrival,StdpTiming::Emission}) {
        auto c=capacitor();auto& edge=c.synapses[0];edge.stdp_timing=mode;
        StdpState p;update_synaptic_stdp(edge,p,1,0,0,.010);update_synaptic_stdp(edge,p,0,0,1,.005);update_synaptic_stdp(edge,p,0,1,0,.005);
        const double expected=1+(mode==StdpTiming::Emission?.01:-.012)*std::exp(-.005/.020);
        close(p.weight,expected,2e-16,"controlled 10/15/20 ms STDP formula");
        Simulator run(c);finish(run);check(run.spikes==std::vector<uint64_t>({1,1})&&run.history.deliveries.size()==1,"controlled network extra spikes");
        close(run.plasticity[0].weight,expected,2e-12,"integrated 10/15/20 ms STDP formula");
        const double pre=mode==StdpTiming::Arrival?run.history.deliveries[0].t_arrival:run.history.deliveries[0].t_emit;
        close(run.plasticity[0].pre_trace,std::exp(-(run.time-pre)/edge.tau_plus),2e-13,"pre trace inserted at wrong time or twice");
    }
    // No mutual pair for simultaneous events, even if several pre/post events.
    Synapse s;s.stdp=true;StdpState p;update_stdp(s,p,3,2,.1);
    check(p.weight==1&&p.pre_trace==3&&p.post_trace==2,"simultaneous count handling");
}
static void zero_tests() {
    for(int preset_id=0;preset_id<8;++preset_id) {
        auto c=preset(preset_id);c.duration=.05;c.sample=.0001;c.noise_sigma=.1;c.noise_interval=.000071;
        Simulator a(c);for(auto& s:c.synapses)s.stdp_timing=StdpTiming::Emission;Simulator b(c);finish(a);finish(b);equal(a,b);
    }
}
static void delivery_tests(const std::filesystem::path& root) {
    for(auto model:{SynapseModel::ConductanceSTP,SynapseModel::ExponentialCurrent,SynapseModel::TsodyksMarkram}) {
        auto c=capacitor();auto& edge=c.synapses[0];edge.model=model;edge.stdp=false;edge.conductance=0;edge.delay=.0000137;
        c.neurons[1].current=0;c.neurons[1].voltage=-65;c.duration=.013;c.sample=.000025;
        Simulator s(c);finish(s);check(s.history.deliveries.size()==1,"single delivery missing");
        const auto e=s.history.deliveries[0];check(e.t_arrival==e.t_emit+edge.delay,"delay rounded");
        close(e.t_emit,.010,2e-12,"capacitor spike time");
        const int b=syn_offset(c,0);const double r=model==SynapseModel::ExponentialCurrent?1:edge.release;
        for(size_t k=0;k<s.history.time.size();++k) {
            const double age=s.history.time[k]-e.t_arrival;
            const double activity=age<0?0:r*std::exp(-age/edge.tau_syn);
            close(s.history.values[b+2][k],activity,3e-14,"activation before arrival or wrong decay");
            close(s.history.values[b+3][k],age<0?0:c.parameters[na::Y_release]*r*std::exp(-age*c.parameters[na::Omega_Y]),2e-10,"glutamate delivery/decay");
            if(model!=SynapseModel::ExponentialCurrent) {
                close(s.history.values[b][k],age<0?0:edge.release*std::exp(-age/edge.tau_f),3e-14,"facilitation timing");
                double x=1;
                if(age>=0) {double a=1/edge.tau_d,z=1/edge.tau_syn;
                    x=model==SynapseModel::ConductanceSTP ? 1-r*std::exp(-a*age) : 1-r*(z*std::exp(-a*age)-a*std::exp(-z*age))/(z-a);}
                close(s.history.values[b+1][k],x,4e-13,"resource recovery timing");
                check(s.history.values[b+1][k]>=0&&activity>=0,"resource positivity");
                if(model==SynapseModel::TsodyksMarkram)check(s.history.values[b+1][k]+activity<=1+1e-12,"TM invariant");
            }
        }
    }
    // Physical current after delivery uses the live weight and its full tail.
    auto c=capacitor();c.duration=.014;auto& edge=c.synapses[0];edge.delay=.00123;edge.stdp=false;edge.gain=2;
    c.neurons[1].voltage=-65;c.neurons[1].current=0;Simulator driven(c);finish(driven);
    const double age=driven.time-driven.history.deliveries[0].t_arrival;
    close(driven.state[4],-65+1000*edge.gain*edge.tau_syn*(-std::expm1(-age/edge.tau_syn)),2e-10,"delivery did not drive actual voltage");
    State rhs1,rhs2;derivative(c,driven.state,driven.time,rhs1,&driven.plasticity);driven.plasticity[0].weight=2;
    derivative(c,driven.state,driven.time,rhs2,&driven.plasticity);close(rhs2[4],2*rhs1[4],1e-12,"weight frozen in delivery instead of live current");
    // Three arrivals in a single macrostep, different outgoing delays.
    c=capacitor();c.dt=.00005;c.duration=.011;c.synapses[0].stdp=false;c.neurons.push_back(c.neurons[1]);c.neurons.push_back(c.neurons[1]);
    for(int j=1;j<3;++j){auto e=c.synapses[0];e.target=j+1;c.synapses.push_back(e);}
    for(int j=0;j<3;++j)c.synapses[j].delay=(3+7*j)*1e-6;
    Simulator multi(c);finish(multi);check(multi.history.deliveries.size()==3,"multiple outgoing delivery count");
    for(auto e:multi.history.deliveries)check(e.t_arrival==e.t_emit+c.synapses[e.synapse].delay,"outgoing delay ignored");
    // Repeated arrivals at the SAME edge inside a macrostep must not be boolean-collapsed.
    c=capacitor();c.dt=.00005;c.duration=.001;c.sample=.00005;c.synapses[0].stdp=false;c.synapses[0].delay=.000013;
    auto& n=c.neurons[0];n.model=NeuronModel::Izhikevich;n.voltage=n.izh_c=-1;n.izh_peak=0;n.izh_b=n.izh_d=n.current=0;
    Simulator repeated(c);finish(repeated);check(repeated.spikes[0]>repeated.steps*3,"multiple-event fixture failed");
    check(repeated.history.deliveries.size()+repeated.pending_deliveries.size()==repeated.spikes[0],"lost repeated arrival");
    double activity=0;for(auto e:repeated.history.deliveries)activity+=std::exp(-(repeated.time-e.t_arrival)/c.synapses[0].tau_syn);
    close(repeated.state[syn_offset(c,0)+2],activity,2e-11,"multiple arrivals collapsed");
    // A completed run retains its future arrivals and extension delivers them.
    c=capacitor();c.duration=.012;Simulator short_run(c);finish(short_run);
    check(short_run.history.deliveries.empty()&&short_run.pending_deliveries.size()==1,"future-end arrival lost/executed");
    save_checkpoint(short_run,root/"future.nscp");auto resumed=load_checkpoint(root/"future.nscp");
    short_run.extend_duration(.030);resumed->extend_duration(.030);finish(short_run);finish(*resumed);equal(short_run,*resumed);
    check(resumed->history.deliveries.size()==1&&resumed->pending_deliveries.empty(),"extension did not consume saved queue");
}
static void simultaneous_tests(const std::filesystem::path& root) {
    auto c=capacitor();c.synapses[0].delay=.005;Simulator s(c);finish(s);
    const auto& d=s.history.deliveries[0];double post=0;for(auto e:s.history.events)if(e.index==1)post=e.time;
    check(d.t_arrival==post,"simultaneous arrival and threshold not grouped");check(s.plasticity[0].weight==1,"mutual zero-lag pair at arrival");
    // Anchor a delivery exactly to stimulus/noise/sample boundaries using a
    // genuinely recorded source spike, with a load between emit and arrival.
    c=capacitor();c.duration=.012;Simulator probe(c);finish(probe);double emit=probe.history.events[0].time;
    c.duration=.025;c.synapses[0].delay=.020-emit;c.neurons[1].amplitude=1;c.neurons[1].start=.020;c.neurons[1].stop=.021;c.neurons[1].width=.001;
    c.noise_interval=.020;Simulator combined(c);while(combined.time<.012)check(combined.step(),"combined boundaries");
    save_checkpoint(combined,root/"boundary.nscp");auto restored=load_checkpoint(root/"boundary.nscp");finish(combined);finish(*restored);equal(combined,*restored);
    check(combined.history.deliveries[0].t_arrival==.020,"boundary timestamp changed");
    auto sample=std::find(combined.history.time.begin(),combined.history.time.end(),.020);check(sample!=combined.history.time.end(),"delivery sample missing");
    close(combined.history.values[syn_offset(c,0)+2][sample-combined.history.time.begin()],1,0,"sample taken before jump");
    // Renumber neurons and reverse edge order, preserving the physical graph.
    c=capacitor();c.synapses[0].delay=.005;c.synapses[0].gain=1;c.neurons.push_back(c.neurons[0]);auto edge=c.synapses[0];edge.source=2;c.synapses.push_back(edge);
    Simulator original(c);std::swap(c.neurons[0],c.neurons[1]);for(auto& e:c.synapses){e.source=e.source<2?1-e.source:e.source;e.target=e.target<2?1-e.target:e.target;}
    std::reverse(c.synapses.begin(),c.synapses.end());Simulator permuted(c);finish(original);finish(permuted);
    for(int i=0;i<3;++i)for(int k=0;k<4;++k)close(original.state[4*i+k],permuted.state[4*(i<2?1-i:i)+k],0,"node traversal changed trajectory");
    for(int j=0;j<2;++j){check(original.plasticity[j].weight==permuted.plasticity[1-j].weight,"edge traversal changed learning");
        check(original.plasticity[j].pre_trace==permuted.plasticity[1-j].pre_trace,"edge traversal changed traces");}
}
static std::vector<Config> restart_fixtures() {
    std::vector<Config> all;
    for(int j=0;j<3;++j)for(auto mode:{StdpTiming::Arrival,StdpTiming::Emission}) {
        auto c=preset(6);c.duration=.035;c.sample=.000175;c.noise_sigma=.45;c.noise_interval=.000073;
        c.synapses[0].model=SynapseModel(j);c.synapses[0].stdp_rule=StdpRule(j);c.synapses[0].stdp_timing=mode;c.synapses[0].delay=.013731;c.synapses[0].gain=3;
        for(auto& n:c.neurons){n.current=13;n.start=.001;n.stop=.026025;n.frequency=200;n.pulse_intervals={{0,.00001},{.002,.002031}};}
        all.push_back(c);
    }
    auto c=preset(2);c.duration=.035;c.sample=.000225;c.noise_sigma=.2;c.noise_interval=.0001;c.parameters[na::O_P]=0;
    for(auto& n:c.neurons){n.current=10;n.amplitude=1;n.start=.00005;n.stop=.024025;n.width=.00001;}
    for(auto& a:c.astrocytes){a.threshold=.100001;a.ip3=.8;a.sic=.2;}
    for(size_t j=0;j<c.synapses.size();++j)c.synapses[j].delay=.020+double(j)*.0000137;
    all.push_back(c);c.record_all=false;c.record_neurons={1};c.record_synapses={};all.push_back(c);return all;
}
static void restart_tests(const std::filesystem::path& root) {
    for(auto c:restart_fixtures()) {
        Simulator a(c),save_only(c);auto b=std::make_unique<Simulator>(c);size_t queued=0;
        while(!a.done()) {
            check(a.step()&&b->step()&&save_only.step(),"restart fixture integration failed");
            if(a.steps%31==0 || !a.pending_deliveries.empty()&&a.steps%7==0) {
                save_checkpoint(save_only,root/"untouched.nscp");save_checkpoint(*b,root/std::filesystem::u8path("очередь.nscp"));
                b=load_checkpoint(root/std::filesystem::u8path("очередь.nscp"));queued+=!b->pending_deliveries.empty();equal(a,*b);
            }
        }
        check(queued>0,"restart fixture never checkpointed queue");equal(a,*b);equal(a,save_only);
        save_checkpoint(a,root/"a.nscp");save_checkpoint(*b,root/"b.nscp");check(bytes(root/"a.nscp")==bytes(root/"b.nscp"),"snapshot not byte identical");
    }
}
static void format_tests(const std::filesystem::path& root) {
    auto c=capacitor();check(serialize(deserialize(serialize(c)))==serialize(c),"delay JSON roundtrip");
    for(double value:{-1.,121.,std::numeric_limits<double>::infinity()}){c.synapses[0].delay=value;check(!validate(c).empty(),"invalid delay accepted");}
    c=capacitor();c.synapses[0].stdp_timing=StdpTiming(9);check(!validate(c).empty(),"invalid timing accepted");
    c=capacitor();auto encoded=serialize(c);auto pos=encoded.find("arrival");encoded.replace(pos,7,"invalid");rejects([&]{deserialize(encoded);});
    c.duration=.012;Simulator s(c);finish(s);save_checkpoint(s,root/"valid.nscp");auto blob=bytes(root/"valid.nscp");
    // Tail: one pending event, no delivered events; mutation preserves checksum
    // so validation, not just the corruption detector, is exercised.
    const size_t event=blob.size()-8-24,count=event-8;
    auto put=[](std::string& b,size_t o,uint64_t v){for(int k=0;k<8;++k)b[o+k]=char(v>>(8*k));};
    for(auto mutation:std::vector<std::pair<size_t,uint64_t>>{{count,MaxPendingDeliveries+1},{event+16,99999},{event,0},{event+8,0},{event+8,0x7ff0000000000000ull}}) {
        auto bad=blob;put(bad,mutation.first,mutation.second);uint64_t h=14695981039346656037ull;for(size_t k=0;k<bad.size()-8;++k){h^=(unsigned char)bad[k];h*=1099511628211ull;}put(bad,bad.size()-8,h);
        write_atomic(root/"bad.nscp",bad);rejects([&]{load_checkpoint(root/"bad.nscp");});
    }
    c=capacitor();Simulator limit(c);limit.pending_deliveries.assign(MaxPendingDeliveries,{.001,1,0});
    while(!limit.done())limit.step();check(limit.error.find("Pending delivery limit")!=std::string::npos,"queue limit not enforced");
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--fixtures") {std::filesystem::create_directories(argv[2]);int i=0;
            for(auto c:restart_fixtures())save_config(c,std::filesystem::path(argv[2])/("case"+std::to_string(i++)+".json"));return 0;}
        auto root=std::filesystem::current_path()/("network-studio-test-delays-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
        learning_tests();zero_tests();delivery_tests(root);simultaneous_tests(root);restart_tests(root);format_tests(root);
        std::filesystem::remove_all(root);std::cout<<"PASS: "<<checks<<" delay/timing checks (analytical delivery, pair/triplet oracle, ties, permutation, repeated arrivals, exact restart, invalid queues).\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<"\n";return 1;}
}
