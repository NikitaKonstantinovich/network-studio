#include "network.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

namespace studio {
Config::Config() {
    double unused[na::VAR_COUNT];
    na::defaults(unused, parameters.data());
}
int dimension(const Config& c) { return int(c.neurons.size()*4+c.synapses.size()*4+c.astrocytes.size()*7); }
bool records_neuron(const Config& c,int index) {
    return index>=0 && index<int(c.neurons.size()) &&
        (c.record_all || std::find(c.record_neurons.begin(),c.record_neurons.end(),index)!=c.record_neurons.end());
}
bool records_synapse(const Config& c,int index) {
    return index>=0 && index<int(c.synapses.size()) &&
        (c.record_all || std::find(c.record_synapses.begin(),c.record_synapses.end(),index)!=c.record_synapses.end());
}
int recorded_dimension(const Config& c) {
    return int(4*(c.record_all?c.neurons.size():c.record_neurons.size())+
               5*(c.record_all?c.synapses.size():c.record_synapses.size())+7*c.astrocytes.size());
}
size_t planned_samples(double duration,double interval) {
    if(!std::isfinite(duration)||duration<0||!std::isfinite(interval)||interval<=0)return MaxSamples+1;
    const double quotient=std::floor(duration/interval);
    if(!std::isfinite(quotient)||quotient>double(MaxSamples))return MaxSamples+1;
    size_t last=size_t(quotient);
    // Use the same multiplied sample clock as the integrator. Division may
    // round an exact boundary to the adjacent bin, so check both neighbours.
    if(last && double(last)*interval>duration)--last;
    if(double(last+1)*interval<=duration)++last;
    return std::min(MaxSamples+1,last+1+size_t(double(last)*interval<duration));
}
int syn_offset(const Config& c, int i) { return int(c.neurons.size()*4)+i*4; }
int astro_offset(const Config& c, int i) { return int((c.neurons.size()+c.synapses.size())*4)+i*7; }
Config preset(int index) {
    Config c;
    c.neurons = {{"Pre 1", .18,.25}, {"Pre 2", .18,.68}, {"Post", .76,.45}};
    c.neurons[1].frequency = 12.5; c.neurons[1].start = 4;
    c.neurons[2].amplitude = 0;
    c.astrocytes = {{"Astro 1", .48,.76}};
    c.synapses = {{0,2,0}, {1,2,0}};
    if (index == 1) {
        c.name = "Feedback disabled"; c.astrocytes[0].feedback = false;
    } else if (index == 2) {
        c.name = "Four neurons / two astrocytes";
        c.neurons = {{"Input",.12,.3},{"Relay",.38,.3},{"Output",.7,.3},{"Inhibitory",.9,.6}};
        c.neurons[0].current = 7;
        for (int i=1; i<4; ++i) c.neurons[i].amplitude = 0;
        c.neurons[3].current = 6;
        c.astrocytes = {{"Astro 1",.3,.74},{"Astro 2",.65,.74}};
        c.synapses = {{0,1,0},{1,2,1},{2,0,0},{3,2,-1}};
        c.synapses[3].reversal = -80; c.synapses[3].conductance = .15;
        c.synapses[0].conductance = .6; c.synapses[1].conductance = .6;
        c.gaps = {{0,1,.05,0}};
    } else if (index == 3) {
        c.name = "Empty experiment"; c.neurons = {{"Neuron 1",.35,.4}};
        c.synapses.clear(); c.astrocytes.clear();
    } else if (index == 4 || index == 5 || index == 6 || index == 7) {
        c.name = index==4 ? "Izhikevich RS / 10 Hz" : "Izhikevich / Tsodyks-Markram";
        c.neurons = {{"RS input",.25,.4}};
        c.astrocytes.clear(); c.synapses.clear();
        auto& n=c.neurons[0]; n.model=NeuronModel::Izhikevich;
        n.amplitude=30; n.frequency=10; n.start=.1; n.stop=2; n.width=.001;
        c.duration=2; c.sample=.00005;
        if(index==5) {
            Neuron post=n; post.name="RS output"; post.px=.75; post.amplitude=0;
            c.neurons.push_back(post);
            Synapse s; s.source=0; s.target=1; s.model=SynapseModel::TsodyksMarkram;
            s.release=.5; s.tau_syn=.01; s.tau_d=.05; s.tau_f=1; s.gain=40;
            c.synapses.push_back(s);
        }
        if(index==6 || index==7) {
            c.name=index==6 ? "STDP / pre before post" : "STDP / post before pre";
            c.duration=5; c.sample=.001;
            n.name="Pre"; n.stop=c.duration;
            Neuron post=n; post.name="Post"; post.px=.75;
            if(index==6)post.start=.11; else n.start=.11;
            c.neurons.push_back(post);
            Synapse s; s.source=0; s.target=1; s.model=SynapseModel::ExponentialCurrent;
            // Weak transmission keeps the externally imposed spike order clear.
            s.gain=.05; s.stdp=true; s.a_plus=.02; s.a_minus=.025;
            c.synapses.push_back(s);
        }
    }
    return c;
}
static bool range(double x, double a, double b) { return std::isfinite(x) && x>=a && x<=b; }
static bool name_valid(const std::string& s,size_t max) {
    if(s.empty()||s.size()>max)return false;
    for(unsigned char ch:s)if(ch<32||ch==127)return false;
    return true;
}
std::string validate(const Config& c) {
    const auto experiment_error=validate_experiment(c);if(!experiment_error.empty())return experiment_error;
    if (c.neurons.empty() || c.neurons.size()>MaxNeurons || c.astrocytes.size()>MaxAstrocytes ||
        c.synapses.size()>MaxSynapses || c.gaps.size()>MaxGaps) return "Network size is outside supported limits.";
    if (!range(c.dt,1e-6,5e-5) || !std::isfinite(c.duration) || c.duration<.001 ||
        !std::isfinite(c.sample) || c.sample<c.dt)
        return "Use finite duration >= 0.001 s, dt = 1..50 us and finite sample >= dt.";
    // No fixed time horizon. Reject clocks whose double precision cannot
    // resolve RK stages, and prevent overflow of integer step/noise counters.
    if(!std::isfinite(c.duration/c.dt) || c.duration/c.dt>=double(UINT64_MAX) ||
        c.duration+c.dt/8==c.duration)
        return "Duration is too large to resolve the integration step with double precision.";
    if(!range(c.noise_sigma,0,100) || !range(c.noise_interval,c.dt,1))
        return "Noise sigma must be 0..100 and its sampling interval dt..1 s.";
    auto valid_selection=[](const std::vector<int>& selected,size_t count) {
        std::set<int> seen;
        for(int index:selected)if(index<0 || size_t(index)>=count || !seen.insert(index).second)return false;
        return true;
    };
    if(!valid_selection(c.record_neurons,c.neurons.size()) || !valid_selection(c.record_synapses,c.synapses.size()))
        return "Recording selections must contain unique existing zero-based node/edge indices.";
    const size_t rows = planned_samples(c.duration,c.sample);
    if(rows>MaxSamples)
        return "History exceeds 1,000,000 samples (including initial/final samples). Increase the sample interval.";
    if(rows*size_t(recorded_dimension(c))>MaxStoredValues)
        return "History exceeds 32,000,000 state/weight values. Select fewer recorded objects or increase the sample interval.";
    if (!name_valid(c.name,120)) return "Experiment name is empty, too long or contains control characters.";
    const auto& p = c.parameters;
    for (int i:SharedParameters) {
        if (!range(p[i],-1000,1e6)) return std::string("Invalid global parameter: ")+na::parameter_names[i];
        if (i!=na::E_Na && i!=na::E_K && i!=na::E_L && i!=na::E_syn && i!=na::V_spike && p[i]<0)
            return std::string("Negative global parameter: ")+na::parameter_names[i];
    }
    for (int i : {na::C_m,na::K_delta,na::kappa_delta,na::K_D,na::K_3K,na::K_P,na::C_T,
                  na::d1,na::d2,na::d3,na::d5,na::tau_A,na::K_SIC})
        if (p[i]<=0) return std::string("Must be positive: ")+na::parameter_names[i];
    if (!range(p[na::U_A],0,1)) return "U_A must be in [0,1].";
    for (const auto& n:c.neurons) {
        if(n.model!=NeuronModel::HodgkinHuxley && n.model!=NeuronModel::Izhikevich)
            return "Unknown neuron model.";
        if (!name_valid(n.name,64) || !range(n.px,0,1) || !range(n.py,0,1) ||
            !range(n.voltage,-100,60) || !range(n.current,-30,100) || !range(n.amplitude,-30,100) ||
            !range(n.frequency,0,200) || !std::isfinite(n.start) || n.start<0 ||
            !std::isfinite(n.stop) || n.stop<n.start ||
            !range(n.width,0,1)) return "Invalid neuron name, position, initial voltage or stimulus.";
        if(!range(n.noise_sigma,0,100) || !range(n.izh_a,.0001,1) || !range(n.izh_b,-2,2) ||
            !range(n.izh_c,-100,0) || !range(n.izh_d,-100,100) || !range(n.izh_peak,0,100) ||
            !range(n.izh_u,-1000,1000)) return "Invalid Izhikevich or individual noise parameters.";
        if(n.model==NeuronModel::Izhikevich && (n.voltage>=n.izh_peak || n.izh_c>=n.izh_peak))
            return "Izhikevich initial and reset voltages must be below the spike peak.";
        if(n.pulse_intervals.size()>64)return "At most 64 stimulus intervals per period are supported.";
        double previous_stop=0;
        // Frequency zero disables the train, while retaining a valid table.
        const double period=n.frequency>0 ? 1/n.frequency : std::numeric_limits<double>::max();
        for(const auto& interval:n.pulse_intervals) {
            if(!range(interval.start,previous_stop,period) ||
               !range(interval.stop,interval.start,period) || interval.stop<=interval.start)
                return "Stimulus intervals must be ordered, non-overlapping, positive-width and inside one period.";
            previous_stop=interval.stop;
        }
    }
    for (const auto& a:c.astrocytes) {
        if (!name_valid(a.name,64) || !range(a.px,0,1) || !range(a.py,0,1) ||
            !range(a.ip3,0,10) || !range(a.calcium,0,p[na::C_T]/(1+p[na::rho_A])) ||
            !range(a.h,0,1) || !range(a.threshold,1e-6,p[na::C_T]/(1+p[na::rho_A])) ||
            !range(a.alpha,0,1) || !range(a.sic,0,1)) return "Invalid astrocyte state or feedback parameters.";
    }
    std::set<std::pair<int,int>> endpoints;
    for (const auto& s:c.synapses) {
        if(!range(s.delay,0,120))return "Synaptic delay must be finite and within 0..120 seconds.";
        if(s.stdp_timing!=StdpTiming::Arrival && s.stdp_timing!=StdpTiming::Emission)
            return "Unknown STDP timing mode.";
        if(s.model!=SynapseModel::ConductanceSTP && s.model!=SynapseModel::ExponentialCurrent &&
            s.model!=SynapseModel::TsodyksMarkram) return "Unknown synapse model.";
        if (s.source<0 || s.target<0 || s.source>=int(c.neurons.size()) || s.target>=int(c.neurons.size()) ||
            s.astro< -1 || s.astro>=int(c.astrocytes.size())) return "A synapse references a missing node.";
        if (!range(s.conductance,0,5) || !range(s.reversal,-100,50) || !range(s.release,0,1) ||
            !range(s.tau_syn,.001,10) || !range(s.tau_f,.001,10) || !range(s.tau_d,.001,10) ||
            !range(s.spill,0,1) || !range(s.weight,-10,10) || !range(s.gain,0,10000)) return "Invalid synaptic parameters.";
        if(s.model==SynapseModel::ConductanceSTP && s.weight<0)
            return "Conductance synapses require a nonnegative weight; use reversal for polarity.";
        if(s.stdp_rule!=StdpRule::Additive && s.stdp_rule!=StdpRule::Multiplicative && s.stdp_rule!=StdpRule::Triplet)
            return "Unknown STDP rule.";
        if(!range(s.a_plus,0,1) || !range(s.a_minus,0,1) || !range(s.tau_plus,.001,10) ||
            !range(s.tau_minus,.001,10) || !range(s.tau_slow,.001,10) || !range(s.weight_min,0,10) ||
            !range(s.weight_max,0,10) || s.weight_min>=s.weight_max)
            return "Invalid STDP parameters: A=0..1, tau=1..10000 ms, 0 <= weight_min < weight_max <= 10.";
        if(s.stdp) {
            if(s.source==s.target || s.weight<0 ||
                (s.model==SynapseModel::ConductanceSTP && s.reversal<=-40))
                return "STDP requires an excitatory synapse between distinct neurons.";
            if(s.weight<s.weight_min || s.weight>s.weight_max)
                return "The initial STDP weight must lie inside its learning bounds.";
        }
    }
    for (const auto& g:c.gaps) {
        if (g.a<0 || g.b<0 || g.a>=int(c.astrocytes.size()) || g.b>=int(c.astrocytes.size()) || g.a==g.b ||
            !range(g.ip3,0,10) || !range(g.calcium,0,10)) return "Invalid astrocyte diffusion link.";
        if (!endpoints.insert(std::minmax(g.a,g.b)).second) return "Duplicate astrocyte diffusion link.";
    }
    return {};
}
void remove_neuron(Config& c,int index) {
    if (index<0 || index>=int(c.neurons.size()) || c.neurons.size()==1) return;
    c.neurons.erase(c.neurons.begin()+index);
    auto remap=[&](std::vector<int>& list){list.erase(std::remove(list.begin(),list.end(),index),list.end());for(auto& i:list)if(i>index)--i;};
    remap(c.analysis.neurons);if(c.analysis.reference_neuron==index)c.analysis.reference_neuron=0;else if(c.analysis.reference_neuron>index)--c.analysis.reference_neuron;
    for(auto& stage:c.stages)remap(stage.neurons);
    c.record_neurons.erase(std::remove(c.record_neurons.begin(),c.record_neurons.end(),index),c.record_neurons.end());
    for(auto& selected:c.record_neurons)if(selected>index)--selected;
    std::vector<int> edge_map(c.synapses.size(),-1);int retained=0;
    for(size_t j=0;j<c.synapses.size();++j)
        if(c.synapses[j].source!=index && c.synapses[j].target!=index)edge_map[j]=retained++;
    std::vector<int> selected_edges;
    for(int j:c.record_synapses)if(j>=0 && size_t(j)<edge_map.size() && edge_map[j]>=0)selected_edges.push_back(edge_map[j]);
    c.record_synapses=std::move(selected_edges);
    for(auto& stim:c.stimulators){auto& v=stim.recipients;v.erase(std::remove(v.begin(),v.end(),index),v.end());for(auto& i:v)if(i>index)--i;}
    c.synapses.erase(std::remove_if(c.synapses.begin(),c.synapses.end(),[&](const Synapse& s) {
        return s.source==index || s.target==index;
    }),c.synapses.end());
    for (auto& s:c.synapses) { if(s.source>index)--s.source; if(s.target>index)--s.target; }
}
void remove_astrocyte(Config& c,int index) {
    if (index<0 || index>=int(c.astrocytes.size())) return;
    c.astrocytes.erase(c.astrocytes.begin()+index);
    for (auto& s:c.synapses) { if(s.astro==index)s.astro=-1; else if(s.astro>index)--s.astro; }
    c.gaps.erase(std::remove_if(c.gaps.begin(),c.gaps.end(),[&](const Gap& g){return g.a==index||g.b==index;}),c.gaps.end());
    for(auto& g:c.gaps) { if(g.a>index)--g.a; if(g.b>index)--g.b; }
}
State initial_state(const Config& c) {
    State y(dimension(c),0);
    for (int i=0;i<int(c.neurons.size());++i) {
        double v=c.neurons[i].voltage; int b=i*4; y[b]=v;
        if(c.neurons[i].model==NeuronModel::Izhikevich) {
            const auto& n=c.neurons[i]; y[b+1]=n.equilibrium_u ? n.izh_b*v : n.izh_u;
            continue;
        }
        const double am=.1*na::vtrap(-v-40,10.), bm=4*std::exp(-(v+65)/18);
        const double ah=.07*std::exp(-(v+65)/20), bh=1/(1+std::exp(-(v+35)/10));
        const double an=.01*na::vtrap(-v-55,10.), bn=.125*std::exp(-(v+65)/80);
        y[b+1]=am/(am+bm); y[b+2]=ah/(ah+bh); y[b+3]=an/(an+bn);
    }
    for(int i=0;i<int(c.synapses.size());++i)y[syn_offset(c,i)+1]=1;
    for(int i=0;i<int(c.astrocytes.size());++i) {
        int b=astro_offset(c,i); const auto& a=c.astrocytes[i];
        y[b+1]=a.ip3; y[b+2]=a.calcium; y[b+3]=a.h; y[b+4]=1;
    }
    return y;
}
static double interval_edge(const Neuron& n,double cycle,double offset) {
    const double period=1/n.frequency;
    // One canonical expression for a period end and the next period origin.
    return offset==period ? n.start+(cycle+1)*period : n.start+cycle*period+offset;
}
double pulse_current(const Neuron& n,double t) {
    if(n.frequency<=0 || t<n.start || t>=n.stop)return 0;
    const double period=1/n.frequency;
    if(n.pulse_intervals.empty() && n.width>=period)return n.amplitude;
    const double cycle=std::floor((t-n.start)/period);
    // Compare absolute boundaries to avoid subtracting a period from a time
    // exactly on an interval edge (e.g. 1.053 - 1 != .053 in binary).
    // Division can round an exact origin to an adjacent cycle. Checking its
    // neighbors preserves half-open edges without a fuzzy timing tolerance.
    for(int neighbor=-1;neighbor<=1;++neighbor) {
        if(cycle+neighbor<0)continue;
        const double k=cycle+neighbor;
        if(n.pulse_intervals.empty()) {
            if(n.width>0 && t>=interval_edge(n,k,0) && t<interval_edge(n,k,n.width))return n.amplitude;
        } else for(const auto& interval:n.pulse_intervals)
            if(t>=interval_edge(n,k,interval.start) && t<interval_edge(n,k,interval.stop))return n.amplitude;
    }
    return 0;
}
static uint64_t noise_bin(double time,double interval) {
    auto bin=uint64_t(std::floor(std::max(0.,time)/interval));
    if(bin && time<double(bin)*interval)--bin;
    if(time>=double(bin+1)*interval)++bin;
    return bin;
}
double next_pulse_boundary(const Neuron& n,double time,double limit) {
    double next=limit;auto consider=[&](double t){if(t>time&&t<next)next=t;};
    if(n.amplitude==0||n.frequency<=0||(n.pulse_intervals.empty()&&n.width<=0)||time>=n.stop)return next;
    consider(n.start);consider(n.stop);if(next<=n.start)return next;
    const double period=1/n.frequency,cycle=std::floor((std::max(time,n.start)-n.start)/period);
    for(int neighbor=-1;neighbor<=2;++neighbor) {
        const double k=cycle+neighbor;if(k<0)continue;
        auto edge=[&](double offset){const double t=interval_edge(n,k,offset);if(t>=n.start&&t<=n.stop)consider(t);};
        if(n.pulse_intervals.empty()){if(n.width<period){edge(0);edge(n.width);}}
        else for(const auto& interval:n.pulse_intervals){edge(interval.start);edge(interval.stop);}
    }return next;
}
double next_input_boundary(const Config& c,double time,double limit) {
    STUDIO_SCOPE(input_boundaries);
    double next=limit;bool noise=false;
    for(const auto& n:c.neurons){noise=noise||(n.individual_noise?n.noise_sigma>0:c.noise_sigma>0);next=next_pulse_boundary(n,time,next);}
    for(const auto& s:c.stimulators)if(s.enabled)next=next_pulse_boundary(stimulator_signal(s),time,next);
    if(noise){const double t=double(noise_bin(time,c.noise_interval)+1)*c.noise_interval;if(t>time&&t<next)next=t;}
    return next;
}
static uint64_t mix64(uint64_t value) {
    value+=0x9e3779b97f4a7c15ULL;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27))*0x94d049bb133111ebULL;
    return value^(value>>31);
}
double external_current(const Config& c,int index,double time) {double value=external_current_with_protocol(c,c.neurons[index],index,time);return c.stimulators.empty()?value:value+shared_current(c,index,time);}
double external_current_with_protocol(const Config& c,const Neuron& n,int index,double time) {
    double current=n.current+pulse_current(n,time);
    const double sigma=n.individual_noise ? n.noise_sigma : c.noise_sigma;
    if(sigma>0) {
        STUDIO_SCOPE(noise);
        // Counter-based streams: changing dt, plotting, and paused redraws do not
        // consume random draws. Open-interval uniforms make Box-Muller finite.
        const uint64_t bin=noise_bin(time,c.noise_interval);
        const uint64_t key=mix64(uint64_t(c.seed)^(uint64_t(index+1)*0xd1b54a32d192ed03ULL))^mix64(bin);
        const double u1=(double(mix64(key)>>11)+.5)/9007199254740992.;
        const double u2=(double(mix64(key^0x94d049bb133111ebULL)>>11)+.5)/9007199254740992.;
        current+=sigma*std::sqrt(-2*std::log(u1))*std::cos(6.2831853071795864769*u2);
    }
    return current;
}
double NoiseCache::current(const Config& c,int index,double time) {
    const auto& n=c.neurons[index];
    double current=n.current+pulse_current(n,time);
    const double sigma=n.individual_noise?n.noise_sigma:c.noise_sigma;
    if(sigma>0) {
        if(entries.size()!=c.neurons.size())entries.resize(c.neurons.size());
        auto& e=entries[index];const uint64_t bin=noise_bin(time,c.noise_interval);
        if(!e.valid||e.bin!=bin||e.seed!=c.seed||e.sigma!=sigma||e.interval!=c.noise_interval) {
            STUDIO_SCOPE(noise);
            const uint64_t key=mix64(uint64_t(c.seed)^(uint64_t(index+1)*0xd1b54a32d192ed03ULL))^mix64(bin);
            const double u1=(double(mix64(key)>>11)+.5)/9007199254740992.;
            const double u2=(double(mix64(key^0x94d049bb133111ebULL)>>11)+.5)/9007199254740992.;
            // Cache this complete left-associated expression: moving sigma to
            // the end would change rounding relative to external_current().
            e.value=sigma*std::sqrt(-2*std::log(u1))*std::cos(6.2831853071795864769*u2);
            e.bin=bin;e.seed=c.seed;e.sigma=sigma;e.interval=c.noise_interval;e.valid=true;
        }
        current+=e.value;
    }
    return c.stimulators.empty()?current:current+shared_current(c,index,time);
}
const std::array<double,3>& StdpDecayCache::factors(const Synapse& s,double dt) {
    const bool triplet=s.stdp_rule==StdpRule::Triplet;
    if(!valid_||dt_!=dt||plus_!=s.tau_plus||minus_!=s.tau_minus||slow_!=s.tau_slow||triplet_!=triplet) {
        factors_={std::exp(-dt/s.tau_plus),std::exp(-dt/s.tau_minus),triplet?std::exp(-dt/s.tau_slow):0};
        dt_=dt;plus_=s.tau_plus;minus_=s.tau_minus;slow_=s.tau_slow;triplet_=triplet;valid_=true;
    }
    return factors_;
}
static void apply_stdp(const Synapse& s,StdpState& state,uint64_t pre,uint64_t post,
                       double rate,const std::array<double,3>& decay) {
    if(!s.enabled || !s.stdp) {
        state.pre_trace=state.post_trace=state.post_slow_trace=0;
        return;
    }
    state.pre_trace*=decay[0];
    state.post_trace*=decay[1];
    const bool triplet=s.stdp_rule==StdpRule::Triplet;
    if(triplet)state.post_slow_trace*=decay[2];
    else state.post_slow_trace=0;
    const bool multiplicative=s.stdp_rule!=StdpRule::Additive;
    const double up=multiplicative ? s.weight_max-state.weight : 1;
    const double down=multiplicative ? state.weight-s.weight_min : 1;
    const double potentiation=s.a_plus*up*state.pre_trace;
    const double delta=(post ? double(post)*(triplet ? potentiation*state.post_slow_trace : potentiation) : 0)-
        (pre ? double(pre)*(s.a_minus*down*state.post_trace) : 0);
    if(rate!=0)state.weight=std::clamp(state.weight+(rate==1?delta:rate*delta),s.weight_min,s.weight_max);
    if(pre)state.pre_trace+=double(pre);
    if(post)state.post_trace+=double(post);
    if(post && triplet)state.post_slow_trace+=double(post);
}
void update_stdp(const Synapse& s,StdpState& state,uint64_t pre,uint64_t post,double dt,double rate) {
    if(!s.enabled||!s.stdp){state.pre_trace=state.post_trace=state.post_slow_trace=0;return;}
    apply_stdp(s,state,pre,post,rate,{std::exp(-dt/s.tau_plus),std::exp(-dt/s.tau_minus),
        s.stdp_rule==StdpRule::Triplet?std::exp(-dt/s.tau_slow):0});
}
void update_stdp_cached(const Synapse& s,StdpState& state,uint64_t pre,uint64_t post,double dt,double rate,StdpDecayCache& cache) {
    if(!s.enabled||!s.stdp){state.pre_trace=state.post_trace=state.post_slow_trace=0;return;}
    apply_stdp(s,state,pre,post,rate,cache.factors(s,dt));
}
void update_synaptic_stdp(const Synapse& s,StdpState& state,uint64_t emitted,uint64_t arrived,uint64_t post,double dt,double rate) {
    update_stdp(s,state,s.stdp_timing==StdpTiming::Arrival ? arrived : emitted,post,dt,rate);
}
void derivative(const Config& c,const State& y,double midpoint,State& dy,const std::vector<StdpState>* plasticity,bool omit_synapse_kinetics,const std::vector<double>* held_inputs) {
    if(y.size()!=size_t(dimension(c)))throw std::invalid_argument("State dimension does not match the network.");
    dy.assign(y.size(),0); const auto& p=c.parameters;
    std::array<double,MaxNeurons> input{};
    std::array<double,MaxAstrocytes> glutamate{};
    std::array<std::array<bool,MaxNeurons>,MaxAstrocytes> contacts{};
    for(int i=0;i<int(c.neurons.size());++i) input[i]=held_inputs?(*held_inputs)[i]:external_current(c,i,midpoint);
    {
    STUDIO_SCOPE(rhs_synapses);
    for(int i=0;i<int(c.synapses.size());++i) {
        const auto& s=c.synapses[i]; int b=syn_offset(c,i);
        if(!omit_synapse_kinetics) {
            if(s.model!=SynapseModel::ExponentialCurrent) {
                dy[b]=-y[b]/s.tau_f;
                dy[b+1]=(1-y[b+1]-(s.model==SynapseModel::TsodyksMarkram ? y[b+2] : 0))/s.tau_d;
            }
            dy[b+2]=-y[b+2]/s.tau_syn; dy[b+3]=-p[na::Omega_Y]*y[b+3];
        }
        if (!s.enabled) continue;
        const double weight=plasticity ? (*plasticity)[i].weight : s.weight;
        if(s.model==SynapseModel::ConductanceSTP)
            input[s.target]+=s.conductance*weight*y[b+2]*(s.reversal-y[s.target*4]);
        else input[s.target]+=weight*s.gain*y[b+2];
        if(s.astro>=0) { glutamate[s.astro]+=s.spill*y[b+3]; contacts[s.astro][s.target]=true; }
    }
    }
    {
    STUDIO_SCOPE(rhs_astro);
    for(int i=0;i<int(c.astrocytes.size());++i) {
        int b=astro_offset(c,i); const auto& a=c.astrocytes[i];
        const double ip3=y[b+1],ca=y[b+2],h=y[b+3],ga=y[b+5];
        dy[b]=p[na::O_N]*glutamate[i]*(1-y[b])-p[na::Omega_N]*y[b];
        dy[b+1]=p[na::O_beta]*y[b]+p[na::O_delta]*p[na::kappa_delta]/(p[na::kappa_delta]+ip3)*na::hill2(ca,p[na::K_delta])
            -p[na::O_3K]*na::hill4(ca,p[na::K_D])*ip3/(ip3+p[na::K_3K])-p[na::Omega_5P]*ip3;
        const double m=ip3/(ip3+p[na::d1])*ca/(ca+p[na::d5]);
        dy[b+2]=(p[na::Omega_C]*na::cube(m)*na::cube(h)+p[na::Omega_L])*(p[na::C_T]-(1+p[na::rho_A])*ca)-p[na::O_P]*na::hill2(ca,p[na::K_P]);
        const double q=p[na::d2]*(ip3+p[na::d1])/(ip3+p[na::d3]);
        dy[b+3]=p[na::O_2]*(q*(1-h)-ca*h);
        dy[b+4]=(1-y[b+4])/p[na::tau_A]; dy[b+5]=-p[na::Omega_G]*ga;
        dy[b+6]=p[na::O_G]*ga*(1-y[b+6])-p[na::Omega_S]*y[b+6];
        if(a.feedback) for(int n=0;n<int(c.neurons.size());++n) if(contacts[i][n])
            input[n]+=a.sic*ga/(p[na::K_SIC]+ga)*(p[na::E_syn]-y[n*4]);
    }
    for(const auto& g:c.gaps) {
        int a=astro_offset(c,g.a),b=astro_offset(c,g.b);
        double di=g.ip3*(y[b+1]-y[a+1]),dc=g.calcium*(y[b+2]-y[a+2]);
        dy[a+1]+=di; dy[b+1]-=di; dy[a+2]+=dc; dy[b+2]-=dc;
    }
    }
    STUDIO_SCOPE(rhs_neurons);
    for(int i=0;i<int(c.neurons.size());++i) {
        const auto& n=c.neurons[i]; int b=i*4;
        if(n.model==NeuronModel::Izhikevich) {
            // Published equations use milliseconds; the network clock is seconds.
            dy[b]=1000*(.04*y[b]*y[b]+5*y[b]+140-y[b+1]+input[i]);
            dy[b+1]=1000*n.izh_a*(n.izh_b*y[b]-y[b+1]);
        } else na::hh_rhs(y.data(),dy.data(),b,input[i],p.data());
    }
}
void Simulator::prepare_solver_views() {
    compact_network=config.astrocytes.empty();
    current_edges.clear();kinetic_ranges.clear();
    current_edges_only=true;izh_only=compact_network;
    if(!compact_network)return;
    rhs_input.resize(config.neurons.size());
    for(const auto& n:config.neurons)izh_only=izh_only&&n.model==NeuronModel::Izhikevich;
    // Scan the original synapse sequence once per macro step. Never group or
    // reorder current contributions; live weights are read on every RHS.
    for(int i=0;i<int(config.synapses.size());++i) {
        const auto& s=config.synapses[i];const int b=syn_offset(config,i);
        if(s.enabled) {
            const bool conductance=s.model==SynapseModel::ConductanceSTP;
            current_edges.push_back({conductance?s.conductance:s.gain,s.reversal,i,s.target,b+2,conductance});
            current_edges_only=current_edges_only&&!conductance;
        }
        // Disabled synapses still have continuous resource/decay dynamics.
        if(kinetic_ranges.empty() || kinetic_ranges.back().model!=s.model ||
           kinetic_ranges.back().tau_f!=s.tau_f || kinetic_ranges.back().tau_d!=s.tau_d ||
           kinetic_ranges.back().tau_syn!=s.tau_syn)
            kinetic_ranges.push_back({b,b+4,s.model,s.tau_f,s.tau_d,s.tau_syn});
        else kinetic_ranges.back().end=b+4;
    }
    if(pool) {
        // A compact incoming CSR, refreshed from the same current view as the
        // reference loop. Stable scatter preserves j-order within every target,
        // including after same-size parameter edits at a safe boundary.
        target_edge_offsets.assign(config.neurons.size()+1,0);
        for(const auto& edge:current_edges)++target_edge_offsets[edge.target+1];
        for(size_t i=1;i<target_edge_offsets.size();++i)target_edge_offsets[i]+=target_edge_offsets[i-1];
        target_edge_cursor=target_edge_offsets;target_edges.resize(current_edges.size());
        for(const auto& edge:current_edges)target_edges[target_edge_cursor[edge.target]++]=edge;
    }
}
bool Simulator::parallel_eligible() const {
    if(!config.astrocytes.empty()||config.synapses.size()<2048)return false;
    for(const auto& n:config.neurons)if(n.model!=NeuronModel::Izhikevich)return false;
    for(const auto& s:config.synapses)if(s.model!=SynapseModel::TsodyksMarkram)return false;
    return true;
}
void Simulator::prepare_workers() {
    int desired=1;
    if(requested_threads!=1&&parallel_eligible()) {
        desired=requested_threads;
        if(desired==0) {
            // Calibrated on the 500/15013 workload and 64/128/256-neuron
            // crossover fixtures (performance/20261019_parallel). For smaller
            // supported networks prefer four: the small eight-thread gain is
            // close to the run-to-run variation. Never request more than the
            // reported hardware concurrency; an unknown count stays serial.
            const unsigned available=std::thread::hardware_concurrency();
            const int limit=config.synapses.size()<8192?4:8;
            desired=1;
            for(int count:{2,4,8})if(count<=limit&&unsigned(count)<=available)desired=count;
        }
    }
    if(desired==effective_compute_threads())return;
    auto candidate=desired>1?std::make_unique<numerical::WorkerPool>(desired):nullptr;
    pool=std::move(candidate);
}
void Simulator::set_compute_threads(int count) {
    if(count!=0&&count!=1&&count!=2&&count!=4&&count!=8)throw std::invalid_argument("Compute threads must be Auto (0), 1, 2, 4 or 8.");
    if(stepping)throw std::runtime_error("Cannot change compute threads inside a solver step.");
    const int before=requested_threads;requested_threads=count;
    try{prepare_workers();}catch(...){requested_threads=before;throw;}
}
void Simulator::compact_derivative(const State& y,State& dy) {
    // Internal no-astro path only. Flow reads derivative coordinates solely in
    // its nonlinear prefix; the public derivative() keeps its full zero suffix.
    if(y.size()!=size_t(dimension(config)))throw std::invalid_argument("State dimension does not match the network.");
    dy.resize(y.size());
    if(pool) {
        STUDIO_SCOPE(rhs_synapses);
        pool->run(config.neurons.size(),[&](size_t begin,size_t end,int) {
            for(size_t i=begin;i<end;++i) {
                double input=held_inputs[i];
                for(size_t k=target_edge_offsets[i];k<target_edge_offsets[i+1];++k) {
                    const auto& edge=target_edges[k];
                    input+=plasticity[edge.synapse].weight*edge.scale*y[edge.activity];
                }
                const auto& n=config.neurons[i];const size_t b=i*4;
                dy[b]=1000*(.04*y[b]*y[b]+5*y[b]+140-y[b+1]+input);
                dy[b+1]=1000*n.izh_a*(n.izh_b*y[b]-y[b+1]);
                dy[b+2]=dy[b+3]=0.;
            }
        });
        return;
    }
    std::fill_n(dy.begin(),config.neurons.size()*4,0.);
    std::copy(held_inputs.begin(),held_inputs.end(),rhs_input.begin());
    {
        STUDIO_SCOPE(rhs_synapses);
        if(current_edges_only) {
            for(const auto& edge:current_edges)
                rhs_input[edge.target]+=plasticity[edge.synapse].weight*edge.scale*y[edge.activity];
        } else {
            for(const auto& edge:current_edges) {
                const double weight=plasticity[edge.synapse].weight;
                if(edge.conductance)
                    rhs_input[edge.target]+=edge.scale*weight*y[edge.activity]*(edge.reversal-y[edge.target*4]);
                else rhs_input[edge.target]+=weight*edge.scale*y[edge.activity];
            }
        }
    }
    STUDIO_SCOPE(rhs_neurons);
    for(int i=0;i<int(config.neurons.size());++i) {
        const auto& n=config.neurons[i];const int b=i*4;
        if(n.model==NeuronModel::Izhikevich) {
            // Keep the same expression and evaluation order as derivative().
            dy[b]=1000*(.04*y[b]*y[b]+5*y[b]+140-y[b+1]+rhs_input[i]);
            dy[b+1]=1000*n.izh_a*(n.izh_b*y[b]-y[b+1]);
        } else na::hh_rhs(y.data(),dy.data(),b,rhs_input[i],config.parameters.data());
    }
}
Simulator::Simulator(const Config& c):config(c) {
    error=validate(c);
    if(!error.empty()) return;
    state=initial_state(c); held_inputs.resize(c.neurons.size()); history.values.resize(dimension(c));
    flow=numerical::Flow(state.size());
    if(c.astrocytes.empty())flow.nonlinear_prefix=c.neurons.size()*4;
    history.weights.resize(c.synapses.size());
    outgoing.resize(c.neurons.size());
    incoming.resize(c.neurons.size());
    suggested_step=c.dt;
    // Bind at each step as Simulator is movable (GUI replacement / test returns).
    for(int i=0;i<int(c.neurons.size());++i) {
        const double peak=c.neurons[i].model==NeuronModel::Izhikevich ? c.neurons[i].izh_peak : c.parameters[na::V_spike];
        thresholds.push_back({i*4,peak,state[i*4]<peak});
    }
    for(int i=0;i<int(c.astrocytes.size());++i) {
        const int b=astro_offset(c,i)+2;thresholds.push_back({b,c.astrocytes[i].threshold,state[b]<c.astrocytes[i].threshold});
    }
    std::vector<bool> neuron_mask(c.neurons.size(),c.record_all),synapse_mask(c.synapses.size(),c.record_all);
    for(int i:c.record_neurons)neuron_mask[i]=true;
    for(int i:c.record_synapses)synapse_mask[i]=true;
    for(int i=0;i<int(c.neurons.size());++i)if(neuron_mask[i])
        for(int k=0;k<4;++k)recorded_state.push_back(i*4+k);
    for(int i=0;i<int(c.synapses.size());++i) {
        outgoing[c.synapses[i].source].push_back(i);
        incoming[c.synapses[i].target].push_back(i);
        if(synapse_mask[i]) {
            for(int k=0;k<4;++k)recorded_state.push_back(syn_offset(c,i)+k);
            recorded_weights.push_back(i);
        }
    }
    for(int i=0;i<int(c.astrocytes.size());++i)
        for(int k=0;k<7;++k)recorded_state.push_back(astro_offset(c,i)+k);
    for(const auto& s:c.synapses)plasticity.push_back({s.weight,0,0});
    spikes.resize(c.neurons.size()); releases.resize(c.astrocytes.size());
    history.dropped_spikes.resize(spikes.size());history.dropped_releases.resize(releases.size());history.dropped_deliveries.resize(c.synapses.size());
    append_protocol("Initial protocol");record();
}
bool Simulator::done() const { return !error.empty() || time>=config.duration || (experiment.stage_active&&!experiment.automatic&&time>=experiment.stage_end); }
void Simulator::extend_duration(double end_time) {
    if(stepping || !checkpoint_boundary || !error.empty())throw std::runtime_error("Cannot extend an unsafe or failed simulation.");
    if(!std::isfinite(end_time) || end_time<config.duration)throw std::runtime_error("Final time may only be extended.");
    if(end_time==config.duration)return;
    Config candidate=config;candidate.duration=end_time;
    const auto message=validate(candidate);if(!message.empty())throw std::runtime_error(message);
    // Earlier off-grid terminal samples are retained when extending a run.
    size_t extra=0;uint64_t grid=0;
    for(double t:history.time) {if(t==double(grid)*config.sample)++grid;else ++extra;}
    const size_t rows=planned_samples(end_time,config.sample)+extra;
    if(rows>MaxSamples || rows*recorded_dimension(config)>MaxStoredValues)
        throw std::runtime_error("Extended history would exceed the recording limits.");
    config.duration=end_time;
}
void Simulator::record() {
    STUDIO_SCOPE(journals);
    if(history.time.size()>=MaxSamples) { error="Recording limit exceeded."; return; }
    history.time.push_back(time);
    for(int j:recorded_state)history.values[j].push_back(state[j]);
    for(int j:recorded_weights)history.weights[j].push_back(plasticity[j].weight);
}
}
