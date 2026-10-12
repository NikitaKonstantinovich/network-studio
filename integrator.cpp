#include "network.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace studio {
void Simulator::exact_flow(const State& old,double h,State& out) const {
    const auto& c=config;const auto& p=c.parameters;
    for(int i=0;i<int(c.synapses.size());++i) {
        const auto& s=c.synapses[i];const int b=syn_offset(c,i);
        if(s.model!=SynapseModel::ExponentialCurrent) {
            out[b]=old[b]*std::exp(-h/s.tau_f);
            if(s.model==SynapseModel::TsodyksMarkram) {
                const double z=1-old[b+1]-old[b+2],a=1/s.tau_d,r=1/s.tau_syn;
                out[b+1]=old[b+1]+z*(-std::expm1(-a*h))+old[b+2]*numerical::recovered(a,r,h);
            } else out[b+1]=old[b+1]+(1-old[b+1])*(-std::expm1(-h/s.tau_d));
        } else {out[b]=old[b];out[b+1]=old[b+1];}
        out[b+2]=old[b+2]*std::exp(-h/s.tau_syn);
        out[b+3]=old[b+3]*std::exp(-h*p[na::Omega_Y]);
    }
    for(int i=0;i<int(c.astrocytes.size());++i) {
        const int b=astro_offset(c,i);
        out[b+4]=old[b+4]+(1-old[b+4])*(-std::expm1(-h/p[na::tau_A]));
        out[b+5]=old[b+5]*std::exp(-h*p[na::Omega_G]);
        bool driven=false,diffusion=false;
        for(int j=0;j<int(c.synapses.size());++j) {
            const auto& s=c.synapses[j];
            driven=driven || (s.enabled && s.astro==i && s.spill>0 && old[syn_offset(c,j)+3]>0);
        }
        for(const auto& g:c.gaps)if((g.a==i||g.b==i) && g.ip3>0)diffusion=true;
        if(p[na::O_N]==0 || !driven)out[b]=old[b]*std::exp(-h*p[na::Omega_N]);
        if(p[na::O_G]==0 || old[b+5]==0)out[b+6]=old[b+6]*std::exp(-h*p[na::Omega_S]);
        if(p[na::O_beta]==0 && p[na::O_delta]==0 && p[na::O_3K]==0 && !diffusion)
            out[b+1]=old[b+1]*std::exp(-h*p[na::Omega_5P]);
    }
}
double Simulator::kinetic_step_limit() const {
    const auto& c=config;const auto& p=c.parameters;double rate=0;
    std::vector<double> conductance(c.neurons.size(),0),glutamate(c.astrocytes.size(),0);
    for(int i=0;i<int(c.synapses.size());++i) {
        const auto& s=c.synapses[i];const int b=syn_offset(c,i);if(!s.enabled)continue;
        if(s.model==SynapseModel::ConductanceSTP)
            conductance[s.target]+=s.conductance*plasticity[i].weight*state[b+2];
        if(s.astro>=0) {
            glutamate[s.astro]+=s.spill*state[b+3];
            if(s.spill>0 && state[b+3]>0)rate=std::max(rate,p[na::Omega_Y]);
        }
    }
    for(int i=0;i<int(c.neurons.size());++i) {
        const auto& n=c.neurons[i];const int b=i*4;const double v=state[b];
        if(n.model==NeuronModel::Izhikevich) {
            rate=std::max(rate,1000*(std::abs(.08*v+5)+n.izh_a));
        } else {
            const double am=.1*na::vtrap(-v-40,10.),bm=4*std::exp(-(v+65)/18);
            const double ah=.07*std::exp(-(v+65)/20),bh=1/(1+std::exp(-(v+35)/10));
            const double an=.01*na::vtrap(-v-55,10.),bn=.125*std::exp(-(v+65)/80);
            rate=std::max(rate,1000*std::max({am+bm,ah+bh,an+bn,
                (p[na::g_Na]*na::cube(state[b+1])*state[b+2]+p[na::g_K]*std::pow(state[b+3],4)+p[na::g_L]+conductance[i])/p[na::C_m]}));
        }
    }
    for(int i=0;i<int(c.astrocytes.size());++i) {
        const int b=astro_offset(c,i);const double ca=state[b+2],ip3=state[b+1];
        const double q=p[na::d2]*(ip3+p[na::d1])/(ip3+p[na::d3]);
        rate=std::max({rate,p[na::O_N]*glutamate[i]+p[na::Omega_N],
            p[na::O_G]*state[b+5]+p[na::Omega_S],p[na::O_2]*(q+ca),
            p[na::Omega_5P]+p[na::O_3K]*na::hill4(ca,p[na::K_D])/p[na::K_3K],
            (p[na::Omega_C]+p[na::Omega_L])*(1+p[na::rho_A])+2*p[na::O_P]/p[na::K_P]});
        if(state[b+5]>0 && (p[na::O_G]>0 || c.astrocytes[i].feedback))rate=std::max(rate,p[na::Omega_G]);
    }
    for(const auto& g:c.gaps)rate=std::max(rate,2*std::max(g.ip3,g.calcium)*double(c.gaps.size()));
    return rate>0 ? .25/rate : c.dt;
}
bool Simulator::physical(const State& y) const {
    const auto& c=config;const auto& p=c.parameters;
    for(double value:y)if(!std::isfinite(value))return false;
    auto fraction=[&](int i){return y[i]>=-1e-10 && y[i]<=1+1e-10;};
    for(int i=0;i<int(c.neurons.size());++i) {
        const int b=4*i;
        if(y[b]<-200 || y[b]>200)return false;
        if(c.neurons[i].model==NeuronModel::Izhikevich) {if(std::abs(y[b+1])>1e5)return false;}
        else if(!fraction(b+1)||!fraction(b+2)||!fraction(b+3))return false;
    }
    for(int i=0;i<int(c.synapses.size());++i) {
        const int b=syn_offset(c,i);
        if(!fraction(b)||!fraction(b+1)||y[b+2]<-1e-10||y[b+3]<-1e-10 ||
           (c.synapses[i].model==SynapseModel::TsodyksMarkram && y[b+1]+y[b+2]>1+1e-10))return false;
    }
    for(int i=0;i<int(c.astrocytes.size());++i) {
        const int b=astro_offset(c,i);
        if(!fraction(b)||!fraction(b+3)||!fraction(b+4)||!fraction(b+6)||y[b+1]<-1e-10||y[b+5]<-1e-10||
           y[b+2]<-1e-10||y[b+2]>p[na::C_T]/(1+p[na::rho_A])+1e-10)return false;
    }
    return true;
}
bool Simulator::step() {
    if(done())return false;
    const auto& c=config;const auto& p=c.parameters;
    flow.rhs=[this](const State& y,State& dy){derivative(config,y,held_input_time,dy,&plasticity);};
    flow.exact=[this](const State& y,double h,State& out){exact_flow(y,h,out);};
    const double macro_end=std::min(double(steps+1)*c.dt,c.duration);
    size_t pieces=0;
    while(time<macro_end) {
        if(++pieces>200000) {error="Too many internal steps; revise extreme kinetic parameters.";return false;}
        const double sample_time=double(next_sample)*c.sample;
        const double limit=next_input_boundary(c,time,std::min(macro_end,sample_time));
        held_input_time=time; // Right-continuous [start, stop) input on this segment.
        double h=std::min({limit-time,suggested_step,kinetic_step_limit()});
        std::vector<int> group;
        double norm=0,advance=0;
        bool accepted=false;
        for(int retry=0;retry<64;++retry) {
            if(!(h>0) || time+h==time) {error="Required numerical step is below clock resolution.";return false;}
            norm=flow.trial(state,h);
            if(norm<=1) {
                advance=flow.events(state,h,time,thresholds,group);
                if(physical(flow.next)) {accepted=true;break;}
            }
            h*=.5;
        }
        if(!accepted) {error="Could not resolve the coupled kinetics within the error/bounds tolerance.";return false;}
        if(history.events.size()+group.size()>MaxEvents) {error="Event recording limit exceeded; shorten the experiment.";return false;}
        const double event_time=(advance==limit-time ? limit : time+advance);
        if(!(event_time>time)) {error="Event localization did not advance the clock.";return false;}
        auto& next=flow.next;
        std::vector<bool> fired(c.neurons.size(),false),released(c.astrocytes.size(),false);
        for(int index:group) {
            if(index<int(c.neurons.size()))fired[index]=true;
            else released[index-int(c.neurons.size())]=true;
        }
        // All event decisions use the same pre-jump trajectory. HH is recorded
        // without reset; Izhikevich resets before integrating the remainder.
        for(int i=0;i<int(c.neurons.size());++i)if(fired[i]) {
            const auto& n=c.neurons[i];
            if(n.model==NeuronModel::Izhikevich){next[4*i]=n.izh_c;next[4*i+1]+=n.izh_d;}
            for(int j:outgoing[i]) {
                const auto& s=c.synapses[j];if(!s.enabled)continue;const int b=syn_offset(c,j);
                double u=s.release;
                if(s.astro>=0 && c.astrocytes[s.astro].feedback)
                    u+=(c.astrocytes[s.astro].alpha-u)*next[astro_offset(c,s.astro)+6];
                double r=1;
                if(s.model!=SynapseModel::ExponentialCurrent) {next[b]+=u*(1-next[b]);r=next[b]*next[b+1];next[b+1]-=r;}
                next[b+2]+=r;
                const bool excitatory=s.model==SynapseModel::ConductanceSTP ? s.reversal>-40 : (s.stdp || s.weight>0);
                if(excitatory)next[b+3]+=p[na::Y_release]*r;
            }
        }
        for(int i=0;i<int(c.astrocytes.size());++i)if(released[i]) {
            const int b=astro_offset(c,i);const double r=p[na::U_A]*next[b+4];next[b+4]-=r;next[b+5]+=p[na::G_release]*r;
        }
        if(!physical(next)) {error="Event jump left the physical state bounds.";return false;}
        // Grouped old traces, one weight update/clamp, then new traces. Weights
        // affect the remaining flow immediately, including existing tails.
        for(size_t j=0;j<c.synapses.size();++j) {
            const auto& s=c.synapses[j];update_stdp(s,plasticity[j],fired[s.source],fired[s.target],advance);
        }
        for(int i=0;i<int(c.neurons.size());++i)if(fired[i]){++spikes[i];history.events.push_back({event_time,i,false});}
        for(int i=0;i<int(c.astrocytes.size());++i)if(released[i]){++releases[i];history.events.push_back({event_time,i,true});}
        state.swap(next);time=event_time;
        for(int index:group)thresholds[index].armed=false;
        for(auto& threshold:thresholds)
            if(state[threshold.coordinate]<threshold.value-numerical::threshold_tolerance(threshold.value))threshold.armed=true;
        suggested_step=std::min(c.dt,h*(norm>1e-16 ? std::clamp(.9*std::pow(norm,-.2),.5,2.) : 2.));
        if(time==sample_time) {record();++next_sample;}
        if(!error.empty())return false;
    }
    ++steps;
    if(time==c.duration && history.time.back()!=time)record();
    return error.empty();
}
}
