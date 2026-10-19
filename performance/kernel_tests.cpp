#include "network.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace studio;

namespace {
size_t checks=0;

void require(bool ok,const std::string& message) {
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
bool equal_bits(double a,double b) {
    return std::memcmp(&a,&b,sizeof(double))==0;
}
void vector_bits(const State& a,const State& b,const std::string& label) {
    require(a.size()==b.size(),label+": different lengths");
    require(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(double))==0,
            label+": different double bits");
}
void flow_output_storage() {
    auto configure=[](numerical::Flow& flow) {
        flow.nonlinear_prefix=1;
        flow.rhs=[](const State& y,State& dy) {
            dy.assign(y.size(),0.);
            dy[0]=y[0]*y[0]+.7*y[1]-.3*y[2];
        };
        flow.exact=[](const State& old,double h,State& out) {
            out[1]=old[1]*std::exp(-h/.003);
            out[2]=old[2]*std::exp(-h/.007);
        };
    };
    numerical::Flow actual_flow(3),reference_flow(3);
    configure(actual_flow);configure(reference_flow);
    State old{.2,.3,.4},actual,expected(3);
    for(int i=0;i<12;++i) {
        const double h=(i+1)*1e-5;
        reference_flow.sample(old,h,expected);
        if(i%3==1) {
            // Internal output immediately after a call with empty external out.
            actual_flow.sample(old,h,actual_flow.next);
            vector_bits(actual_flow.next,expected,"Flow internal output after empty output");
        } else {
            if(i%3==0)actual=State{};
            else actual.clear();
            actual_flow.sample(old,h,actual);
            vector_bits(actual,expected,"Flow empty/cleared external output");
        }
        old=expected;
    }
}
void deliveries(const std::vector<Delivery>& a,const std::vector<Delivery>& b,
                const std::string& label) {
    require(a.size()==b.size(),label+": different lengths");
    for(size_t i=0;i<a.size();++i)
        require(a[i].synapse==b[i].synapse&&equal_bits(a[i].t_emit,b[i].t_emit)&&
                equal_bits(a[i].t_arrival,b[i].t_arrival),label+": different event or order at "+std::to_string(i));
}

Config fixture(bool mixed) {
    auto c=preset(4);
    c.name=mixed?"Kernel reference mixed HH/Izh":"Kernel reference all Izh";
    c.dt=25e-6;c.duration=.03;c.sample=.000137;
    c.seed=0xfedcba98u;c.noise_sigma=1.25;c.noise_interval=.00073;
    c.neurons.resize(7,c.neurons[0]);c.synapses.clear();c.astrocytes.clear();c.gaps.clear();
    c.record_all=true;c.record_neurons.clear();c.record_synapses.clear();
    for(int i=0;i<7;++i) {
        auto& n=c.neurons[i];
        n.name="N"+std::to_string(i);n.model=mixed&&(i==0||i==2||i==6)?
            NeuronModel::HodgkinHuxley:NeuronModel::Izhikevich;
        n.voltage=-65;n.equilibrium_u=true;n.izh_a=i%2?.1:.02;n.izh_b=.2;n.izh_c=-65;n.izh_d=4;
        n.current=18+1.7*i;n.amplitude=1.8;n.frequency=137;n.start=.000101+i*7e-6;
        n.stop=.029037;n.width=.001031;n.pulse_intervals.clear();
        n.individual_noise=i==0||i==1||i==4;
        n.noise_sigma=i==0?0:i==1?.7:1.5;
    }
    // Deliberately retain this unsorted edge order. Incoming degrees are
    // 0,1,3,6,0,0,0 (including one initially disabled edge). A projected root
    // scratch buffer therefore visits different sizes, including zero input.
    const std::array<std::array<int,2>,10> endpoints{{
        {{6,3}},{{0,2}},{{3,1}},{{5,3}},{{1,3}},
        {{4,2}},{{0,3}},{{6,2}},{{2,3}},{{4,3}}
    }};
    const std::array<SynapseModel,10> models{{
        SynapseModel::ConductanceSTP,SynapseModel::ExponentialCurrent,
        SynapseModel::TsodyksMarkram,SynapseModel::TsodyksMarkram,
        SynapseModel::TsodyksMarkram,SynapseModel::ConductanceSTP,
        SynapseModel::ExponentialCurrent,SynapseModel::TsodyksMarkram,
        SynapseModel::ConductanceSTP,SynapseModel::TsodyksMarkram
    }};
    for(int j=0;j<int(endpoints.size());++j) {
        Synapse s;s.source=endpoints[j][0];s.target=endpoints[j][1];s.model=models[j];
        const int tuple=j==3?2:j%3;
        s.conductance=.008+.001*j;s.reversal=0;s.gain=.12+.03*j;s.weight=.4+.02*j;
        s.release=.2+.02*(j%3);s.tau_f=.011+.004*tuple;
        s.tau_d=.031+.006*tuple;s.tau_syn=.002+.001*tuple;
        if(j==9)s.tau_d=s.tau_syn; // The coincident TM-rate limit.
        s.delay=j==7?.05:37e-6*(j%4); // Keep some arrivals past duration.
        s.enabled=j!=9;s.spill=0;s.astro=-1;
        s.stdp=true;s.stdp_rule=StdpRule(j%3);
        s.stdp_timing=j%2?StdpTiming::Emission:StdpTiming::Arrival;
        s.a_plus=.001;s.a_minus=.0012;
        s.tau_plus=.013+.003*(j%2);s.tau_minus=.019+.002*(j%3);s.tau_slow=.041;
        c.synapses.push_back(s);
    }
    // The additional reference astrocyte must contribute constant coordinates:
    // no coupling, nonlinear error, events, or kinetic step restriction.
    for(int k:{na::O_N,na::Omega_N,na::O_G,na::Omega_S,na::Omega_G,
               na::O_beta,na::O_delta,na::O_3K,na::Omega_5P,na::O_2,
               na::Omega_C,na::Omega_L,na::O_P})c.parameters[k]=0;
    require(validate(c).empty(),"Invalid kernel fixture: "+validate(c));
    return c;
}

void change_numeric_parameters(Config& c,const Config& original,bool changed,bool current_only) {
    // Low-level cache invalidation probe, applied only between completed steps.
    // This is not a new GUI/protocol editing contract. Topology, synapse models,
    // delays, neuron parameters and the recording layout remain unchanged.
    for(size_t i=0;i<c.synapses.size();++i) {
        auto& s=c.synapses[i];const auto& before=original.synapses[i];
        s.gain=changed?before.gain*1.125:before.gain;
        s.conductance=changed?before.conductance+.003:before.conductance;
        s.reversal=changed?5.:before.reversal;
        s.tau_f=changed?before.tau_f*1.25:before.tau_f;
        s.tau_d=changed?before.tau_d*.75:before.tau_d;
        s.tau_syn=changed?before.tau_syn*1.5:before.tau_syn;
        // Changes a packed enabled-edge view while retaining endpoint order.
        s.enabled=changed?(current_only?s.model!=SynapseModel::ConductanceSTP:i!=0):before.enabled;
    }
    require(validate(c).empty(),"Numeric cache probe produced invalid parameters: "+validate(c));
}

void compare_runtime(const Simulator& a,const Simulator& b) {
    const std::string at=" at step "+std::to_string(a.steps);
    require(a.error.empty()&&b.error.empty(),"Solver failure"+at+": "+a.error+" / "+b.error);
    require(a.steps==b.steps&&equal_bits(a.time,b.time),"Clock mismatch"+at);
    require(b.state.size()==a.state.size()+7,"Reference astrocyte dimensions changed");
    require(std::memcmp(a.state.data(),b.state.data(),a.state.size()*sizeof(double))==0,
            "Common state prefix differs"+at);
    require(a.spikes==b.spikes,"Spike counters differ"+at);
    require(b.releases.size()==1&&b.releases[0]==0,"Inert reference astrocyte released");
    require(a.plasticity.size()==b.plasticity.size(),"Learning dimensions differ");
    for(size_t i=0;i<a.plasticity.size();++i) {
        const auto& p=a.plasticity[i];const auto& q=b.plasticity[i];
        require(equal_bits(p.weight,q.weight)&&equal_bits(p.pre_trace,q.pre_trace)&&
                equal_bits(p.post_trace,q.post_trace)&&equal_bits(p.post_slow_trace,q.post_slow_trace),
                "STDP bits differ for edge "+std::to_string(i)+at);
    }
    deliveries(a.pending_deliveries,b.pending_deliveries,"Pending heap"+at);
}

void compare_history(const Simulator& a,const Simulator& b) {
    const auto& x=a.history;const auto& y=b.history;
    vector_bits(x.time,y.time,"History clock");
    require(y.values.size()==x.values.size()+7,"History dimensions differ");
    for(size_t i=0;i<x.values.size();++i)vector_bits(x.values[i],y.values[i],"State history "+std::to_string(i));
    require(x.weights.size()==y.weights.size(),"Weight history dimensions differ");
    for(size_t i=0;i<x.weights.size();++i)vector_bits(x.weights[i],y.weights[i],"Weight history "+std::to_string(i));
    require(x.events.size()==y.events.size(),"Event history lengths differ");
    for(size_t i=0;i<x.events.size();++i) {
        const auto& p=x.events[i];const auto& q=y.events[i];
        require(equal_bits(p.time,q.time)&&p.index==q.index&&p.astro==q.astro,
                "Spike identity, order or time bits differ at "+std::to_string(i));
    }
    deliveries(x.deliveries,y.deliveries,"Delivery history");
    require(x.stimuli.size()==y.stimuli.size(),"Stimulus history lengths differ");
    for(size_t i=0;i<x.stimuli.size();++i) {
        const auto& p=x.stimuli[i];const auto& q=y.stimuli[i];
        require(equal_bits(p.time,q.time)&&equal_bits(p.amplitude,q.amplitude)&&
                p.neuron==q.neuron&&p.protocol==q.protocol,"Stimulus history differs");
    }
    require(equal_bits(x.events_after,y.events_after)&&equal_bits(x.deliveries_after,y.deliveries_after)&&
            equal_bits(x.stimuli_after,y.stimuli_after),"History availability boundaries differ");
    require(x.dropped_spikes==y.dropped_spikes&&x.dropped_deliveries==y.dropped_deliveries,
            "Dropped journal counters differ");
}

void run_case(bool mixed) {
    const auto c=fixture(mixed);auto full=c;
    full.astrocytes={{}};full.astrocytes[0].feedback=false;full.astrocytes[0].threshold=1;
    require(validate(full).empty(),"Invalid full reference: "+validate(full));
    Simulator optimized(c),reference(full);
    compare_runtime(optimized,reference);
    const State inert_initial(reference.state.end()-7,reference.state.end());
    while(!optimized.done()) {
        if(optimized.steps==160||optimized.steps==320||optimized.steps==480) {
            const bool changed=optimized.steps!=480,current_only=optimized.steps==320;
            // The middle phase exercises a current-only enabled-edge view,
            // then restoring parameters returns to mixed synapse currents.
            change_numeric_parameters(optimized.config,c,changed,current_only);
            change_numeric_parameters(reference.config,c,changed,current_only);
        }
        require(optimized.step(),"Optimized solver failed: "+optimized.error);
        require(reference.step(),"Full reference failed: "+reference.error);
        compare_runtime(optimized,reference);
    }
    require(reference.done(),"Reference did not finish at the same time");
    vector_bits(State(reference.state.end()-7,reference.state.end()),inert_initial,"Inert astrocyte state");
    compare_history(optimized,reference);
    require(!optimized.history.deliveries.empty()&&!optimized.pending_deliveries.empty(),
            "Fixture must exercise both deliveries and future queued events");
    for(size_t i=0;i<optimized.spikes.size();++i)
        require(optimized.spikes[i]>0,"Fixture neuron "+std::to_string(i)+" did not exercise root localization");
    std::array<bool,3> changed_rule{};
    for(size_t i=0;i<c.synapses.size();++i)
        if(!equal_bits(optimized.plasticity[i].weight,c.synapses[i].weight))
            changed_rule[size_t(c.synapses[i].stdp_rule)]=true;
    for(bool changed:changed_rule)require(changed,"Fixture did not change weights for every STDP rule");
    std::cout<<(mixed?"Mixed HH/Izh":"All Izh")<<": "<<optimized.steps<<" steps, "
             <<optimized.history.events.size()<<" spikes, "<<optimized.history.deliveries.size()
             <<" deliveries; all reference bits equal\n";
}
}

int main() {
    try {
        flow_output_storage();
        run_case(false);run_case(true);
        std::cout<<"PASS packed kernel and root scratch: "<<checks<<" checks\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAIL packed kernel and root scratch: "<<e.what()<<'\n';
        return 1;
    }
}
