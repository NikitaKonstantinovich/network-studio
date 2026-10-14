#include "network.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace studio {
double Simulator::neuron_threshold_sample(const State& old,double h,int coordinate) const {
    // Without astrocytes, each neuron's between-event flow depends only on its
    // own state and independent incoming activity tails. Preserve the full
    // solver's arithmetic and edge summation order, including both half steps.
    const auto& c=config;const auto& p=c.parameters;
    const int index=coordinate/4;const auto& n=c.neurons[index];const auto& edges=incoming[index];
    State initial(4+edges.size());
    for(int k=0;k<4;++k)initial[k]=old[coordinate+k];
    for(size_t k=0;k<edges.size();++k)initial[4+k]=old[syn_offset(c,edges[k])+2];
    numerical::Flow local(initial.size());
    const double external=external_current(c,index,held_input_time);
    local.rhs=[&](const State& y,State& dy) {
        dy.assign(y.size(),0);double input=external;
        for(size_t k=0;k<edges.size();++k) {
            const int j=edges[k];const auto& s=c.synapses[j];if(!s.enabled)continue;
            const double weight=plasticity[j].weight;
            if(s.model==SynapseModel::ConductanceSTP)
                input+=s.conductance*weight*y[4+k]*(s.reversal-y[0]);
            else input+=weight*s.gain*y[4+k];
        }
        if(n.model==NeuronModel::Izhikevich) {
            dy[0]=1000*(.04*y[0]*y[0]+5*y[0]+140-y[1]+input);
            dy[1]=1000*n.izh_a*(n.izh_b*y[0]-y[1]);
        } else na::hh_rhs(y.data(),dy.data(),0,input,p.data());
    };
    local.exact=[&](const State& y,double dt,State& out) {
        double previous_tau=0,decay=0;
        for(size_t k=0;k<edges.size();++k) {
            const double tau=c.synapses[edges[k]].tau_syn;
            if(tau!=previous_tau){decay=std::exp(-dt/tau);previous_tau=tau;}
            out[4+k]=y[4+k]*decay;
        }
    };
    local.sample(initial,h,local.next);return local.next[0];
}
void Simulator::exact_flow(const State& old,double h,State& out) const {
    const auto& c=config;const auto& p=c.parameters;
    // These transitions depend only on h and the kinetic parameters, not state.
    // Reuse the last parameter tuple within this call (no persistent solver state).
    const double glutamate_decay=std::exp(-h*p[na::Omega_Y]);
    const Synapse* previous=nullptr;
    double facilitation_decay=0,recovery=0,active_decay=0,transfer=0;
    for(int i=0;i<int(c.synapses.size());++i) {
        const auto& s=c.synapses[i];const int b=syn_offset(c,i);
        if(!previous || s.model!=previous->model || s.tau_f!=previous->tau_f ||
           s.tau_d!=previous->tau_d || s.tau_syn!=previous->tau_syn) {
            active_decay=std::exp(-h/s.tau_syn);
            if(s.model!=SynapseModel::ExponentialCurrent) {
                facilitation_decay=std::exp(-h/s.tau_f);
                if(s.model==SynapseModel::TsodyksMarkram) {
                    const double a=1/s.tau_d,r=1/s.tau_syn;
                    recovery=-std::expm1(-a*h);
                    transfer=numerical::recovered(a,r,h);
                } else recovery=-std::expm1(-h/s.tau_d);
            }
            previous=&s;
        }
        if(s.model!=SynapseModel::ExponentialCurrent) {
            out[b]=old[b]*facilitation_decay;
            if(s.model==SynapseModel::TsodyksMarkram) {
                const double z=1-old[b+1]-old[b+2];
                out[b+1]=old[b+1]+z*recovery+old[b+2]*transfer;
            } else out[b+1]=old[b+1]+(1-old[b+1])*recovery;
        } else {out[b]=old[b];out[b+1]=old[b+1];}
        out[b+2]=old[b+2]*active_decay;
        out[b+3]=old[b+3]*glutamate_decay;
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
    if(stepping)throw std::runtime_error("Simulator::step is not reentrant.");
    stepping=true;checkpoint_boundary=false;
    try {
        const bool ok=step_impl();
        checkpoint_boundary=ok;stepping=false;return ok;
    } catch(const std::exception& e) {
        stepping=false;error=std::string("Integration failed: ")+e.what();return false;
    } catch(...) {stepping=false;error="Integration failed with an unknown exception.";return false;}
}
bool Simulator::step_impl() {
    const auto& c=config;const auto& p=c.parameters;
    flow.rhs=[this](const State& y,State& dy){derivative(config,y,held_input_time,dy,&plasticity,flow.nonlinear_prefix!=0);};
    flow.exact=[this](const State& y,double h,State& out){exact_flow(y,h,out);};
    flow.threshold_sample={};
    if(c.astrocytes.empty())flow.threshold_sample=[this](const State& y,double h,int coordinate) {
        return neuron_threshold_sample(y,h,coordinate);
    };
    const double macro_end=std::min(double(steps+1)*c.dt,c.duration);
    size_t pieces=0;
    while(time<macro_end) {
        if(++pieces>200000) {error="Too many internal steps; revise extreme kinetic parameters.";return false;}
        const double sample_time=double(next_sample)*c.sample;
        const double delivery_time=pending_deliveries.empty() ? std::numeric_limits<double>::infinity() : pending_deliveries.front().t_arrival;
        if(delivery_time<=time) {error="Delivery queue contains an already due event; reset the experiment.";return false;}
        const double limit=next_input_boundary(c,time,std::min({macro_end,sample_time,delivery_time}));
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
                // A threshold localized at a scheduled arrival shares its exact
                // timestamp (within the existing event-localization tolerance).
                if(!group.empty() && limit==delivery_time && h==limit-time &&
                   h-advance<=2*numerical::event_tolerance(time)) {
                    advance=h;flow.sample(state,h,flow.next);
                }
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
        std::vector<Delivery> deliveries,created;
        // Inspect just the due heap subtree, without consuming events until
        // the whole event group is validated and committed.
        std::vector<size_t> inspect;
        if(delivery_time==event_time)inspect.push_back(0);
        while(!inspect.empty()) {
            const size_t index=inspect.back();inspect.pop_back();
            if(index>=pending_deliveries.size() || pending_deliveries[index].t_arrival!=event_time)continue;
            deliveries.push_back(pending_deliveries[index]);
            inspect.push_back(2*index+1);inspect.push_back(2*index+2);
        }
        const size_t consumed=deliveries.size();
        for(int i=0;i<int(c.neurons.size());++i)if(fired[i])for(int j:outgoing[i]) {
            const auto& edge=c.synapses[j];if(!edge.enabled)continue;
            Delivery e{event_time,event_time+edge.delay,j};
            if(e.t_arrival==event_time)deliveries.push_back(e);else created.push_back(e);
        }
        if(pending_deliveries.size()-consumed+created.size()>MaxPendingDeliveries) {
            error="Pending delivery limit (1000000) exceeded; reduce firing, connections or delays.";return false;
        }
        if(history.deliveries.size()+deliveries.size()>MaxDeliveries) {
            error="Delivery journal limit (2000000) exceeded; shorten the experiment.";return false;
        }
        std::sort(deliveries.begin(),deliveries.end(),[](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);});
        std::vector<uint64_t> arrivals(c.synapses.size(),0);
        // All event decisions use the same pre-jump trajectory. HH is recorded
        // without reset; Izhikevich resets before integrating the remainder.
        for(int i=0;i<int(c.neurons.size());++i)if(fired[i]) {
            const auto& n=c.neurons[i];
            if(n.model==NeuronModel::Izhikevich){next[4*i]=n.izh_c;next[4*i+1]+=n.izh_d;}
        }
        for(const auto& e:deliveries) {
                const int j=e.synapse;const auto& s=c.synapses[j];const int b=syn_offset(c,j);
                ++arrivals[j];
                double u=s.release;
                if(s.astro>=0 && c.astrocytes[s.astro].feedback)
                    u+=(c.astrocytes[s.astro].alpha-u)*next[astro_offset(c,s.astro)+6];
                double r=1;
                if(s.model!=SynapseModel::ExponentialCurrent) {next[b]+=u*(1-next[b]);r=next[b]*next[b+1];next[b+1]-=r;}
                next[b+2]+=r;
                const bool excitatory=s.model==SynapseModel::ConductanceSTP ? s.reversal>-40 : (s.stdp || s.weight>0);
                if(excitatory)next[b+3]+=p[na::Y_release]*r;
        }
        for(int i=0;i<int(c.astrocytes.size());++i)if(released[i]) {
            const int b=astro_offset(c,i);const double r=p[na::U_A]*next[b+4];next[b+4]-=r;next[b+5]+=p[na::G_release]*r;
        }
        if(!physical(next)) {error="Event jump left the physical state bounds.";return false;}
        // Grouped old traces, one weight update/clamp, then new traces. Weights
        // affect the remaining flow immediately, including existing tails.
        for(size_t j=0;j<c.synapses.size();++j) {
            const auto& s=c.synapses[j];update_synaptic_stdp(s,plasticity[j],fired[s.source],arrivals[j],fired[s.target],advance);
        }
        for(size_t k=0;k<consumed;++k) {
            std::pop_heap(pending_deliveries.begin(),pending_deliveries.end(),DeliveryLater{});pending_deliveries.pop_back();
        }
        for(const auto& e:created) {
            pending_deliveries.push_back(e);std::push_heap(pending_deliveries.begin(),pending_deliveries.end(),DeliveryLater{});
        }
        history.deliveries.insert(history.deliveries.end(),deliveries.begin(),deliveries.end());
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
