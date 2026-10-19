#include "network.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace studio {
double Simulator::neuron_threshold_sample(const State& old,double h,int coordinate) {
    STUDIO_SCOPE(threshold_sample);
    return projected_threshold(old,h,coordinate,projection_flow,projection_initial);
}
double Simulator::projected_threshold(const State& old,double h,int coordinate,numerical::Flow& local,State& initial) {
    // Without astrocytes, each neuron's between-event flow depends only on its
    // own state and independent incoming activity tails. Preserve the full
    // solver's arithmetic and edge summation order, including both half steps.
    const auto& c=config;
    const int index=coordinate/4;const auto& edges=incoming[index];
    initial.resize(4+edges.size());
    for(int k=0;k<4;++k)initial[k]=old[coordinate+k];
    for(size_t k=0;k<edges.size();++k)initial[4+k]=old[syn_offset(c,edges[k])+2];
    local.resize(initial.size());
    // Rebind before every use: Simulator may have moved since the previous
    // search. Callbacks retain no references to this function's local variables.
    local.rhs=[this,index](const State& y,State& dy) {
        const auto& c=config;const auto& n=c.neurons[index];const auto& edges=incoming[index];
        dy.assign(y.size(),0);double input=held_inputs[index];
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
        } else na::hh_rhs(y.data(),dy.data(),0,input,c.parameters.data());
    };
    local.exact=[this,index](const State& y,double dt,State& out) {
        const auto& edges=incoming[index];
        double previous_tau=0,decay=0;
        for(size_t k=0;k<edges.size();++k) {
            const double tau=config.synapses[edges[k]].tau_syn;
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
    {
    STUDIO_SCOPE(exact_synapses);
    if(pool) {
        // Coefficients are prepared once by the controller and immutable for
        // the phase. Each block writes disjoint four-coordinate resources.
        kinetic_factors.resize(kinetic_ranges.size());
        for(size_t k=0;k<kinetic_ranges.size();++k) {
            const auto& r=kinetic_ranges[k];auto& f=kinetic_factors[k];
            f.active=std::exp(-h/r.tau_syn);f.facilitation=std::exp(-h/r.tau_f);
            const double a=1/r.tau_d,rate=1/r.tau_syn;
            f.recovery=-std::expm1(-a*h);f.transfer=numerical::recovered(a,rate,h);
        }
        pool->run(c.synapses.size(),[&](size_t begin,size_t end,int) {
            const int offset=int(c.neurons.size()*4);
            int b=offset+int(begin*4),stop=offset+int(end*4);
            size_t k=0;while(k<kinetic_ranges.size()&&kinetic_ranges[k].end<=b)++k;
            while(b<stop) {
                const auto& r=kinetic_ranges[k];const auto& f=kinetic_factors[k];
                const int limit=std::min(stop,r.end);
                for(;b<limit;b+=4) {
                    out[b]=old[b]*f.facilitation;
                    const double z=1-old[b+1]-old[b+2];
                    out[b+1]=old[b+1]+z*f.recovery+old[b+2]*f.transfer;
                    out[b+2]=old[b+2]*f.active;
                    out[b+3]=old[b+3]*glutamate_decay;
                }
                ++k;
            }
        });
    } else if(compact_network) {
        // Contiguous runs have exactly the parameter equality used by the
        // reference loop below. State updates retain their original edge order.
        for(const auto& range:kinetic_ranges) {
            active_decay=std::exp(-h/range.tau_syn);
            if(range.model!=SynapseModel::ExponentialCurrent) {
                facilitation_decay=std::exp(-h/range.tau_f);
                if(range.model==SynapseModel::TsodyksMarkram) {
                    const double a=1/range.tau_d,r=1/range.tau_syn;
                    recovery=-std::expm1(-a*h);
                    transfer=numerical::recovered(a,r,h);
                    for(int b=range.begin;b<range.end;b+=4) {
                        out[b]=old[b]*facilitation_decay;
                        const double z=1-old[b+1]-old[b+2];
                        out[b+1]=old[b+1]+z*recovery+old[b+2]*transfer;
                        out[b+2]=old[b+2]*active_decay;
                        out[b+3]=old[b+3]*glutamate_decay;
                    }
                } else {
                    recovery=-std::expm1(-h/range.tau_d);
                    for(int b=range.begin;b<range.end;b+=4) {
                        out[b]=old[b]*facilitation_decay;
                        out[b+1]=old[b+1]+(1-old[b+1])*recovery;
                        out[b+2]=old[b+2]*active_decay;
                        out[b+3]=old[b+3]*glutamate_decay;
                    }
                }
            } else for(int b=range.begin;b<range.end;b+=4) {
                out[b]=old[b];out[b+1]=old[b+1];
                out[b+2]=old[b+2]*active_decay;
                out[b+3]=old[b+3]*glutamate_decay;
            }
        }
    } else for(int i=0;i<int(c.synapses.size());++i) {
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
    }
    STUDIO_SCOPE(exact_astro);
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
    STUDIO_SCOPE(kinetic);
    const auto& c=config;const auto& p=c.parameters;double rate=0;
    if(izh_only) {
        // With no astrocytes and only Izhikevich neurons the original bound
        // never reads the edge sums. Retain precisely the same max order.
        for(int i=0;i<int(c.neurons.size());++i) {
            const auto& n=c.neurons[i];const double v=state[i*4];
            rate=std::max(rate,1000*(std::abs(.08*v+5)+n.izh_a));
        }
        return rate>0 ? .25/rate : c.dt;
    }
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
    STUDIO_SCOPE(bounds);
    const auto& c=config;const auto& p=c.parameters;
    if(pool) {
        // Only Izh/TM/no-astro enters this branch. Logical reduction cannot
        // reorder any floating-point accumulation or alter an event decision.
        for(size_t i=0;i<c.neurons.size();++i) {
            const size_t b=4*i;
            for(size_t k=0;k<4;++k)if(!std::isfinite(y[b+k]))return false;
            if(y[b]<-200||y[b]>200||std::abs(y[b+1])>1e5)return false;
        }
        std::array<uint8_t,8> valid{}; // Not packed bits; one writer per slot.
        pool->run(c.synapses.size(),[&](size_t begin,size_t end,int id) {
            bool ok=true;
            for(size_t j=begin;j<end&&ok;++j) {
                const size_t b=c.neurons.size()*4+j*4;
                for(size_t k=0;k<4;++k)if(!std::isfinite(y[b+k]))ok=false;
                if(y[b]<-1e-10||y[b]>1+1e-10||y[b+1]<-1e-10||y[b+1]>1+1e-10||
                    y[b+2]<-1e-10||y[b+3]<-1e-10||y[b+1]+y[b+2]>1+1e-10)ok=false;
            }
            valid[id]=uint8_t(ok);
        });
        for(int id=0;id<pool->count();++id)if(!valid[id])return false;
        return true;
    }
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
double Simulator::parallel_error_norm(const State& old,const State& next,const State& full) const {
    std::array<double,8> errors{};
    pool->run(old.size(),[&](size_t begin,size_t end,int id) {
        double error=0;
        for(size_t i=begin;i<end;++i) {
            if(!std::isfinite(next[i])||!std::isfinite(full[i])){error=std::numeric_limits<double>::infinity();break;}
            const double scale=1e-11+1e-8*std::max(std::abs(old[i]),std::abs(next[i]));
            error=std::max(error,std::abs(next[i]-full[i])/(15*scale));
        }
        errors[id]=error;
    });
    // Values are nonnegative finite maxima or +infinity, so this max (not sum)
    // yields the exact original value, including zero and the invalid-state case.
    double error=0;for(int id=0;id<pool->count();++id)error=std::max(error,errors[id]);return error;
}
bool Simulator::step() {
    STUDIO_SCOPE(step);
    if(done())return false;
    if(stepping)throw std::runtime_error("Simulator::step is not reentrant.");
    stepping=true;checkpoint_boundary=false;
    try {
        const bool ok=step_impl();
        checkpoint_boundary=ok;stepping=false;
        if(ok&&experiment.stage_active&&time>=experiment.stage_end) {
            if(experiment.automatic&&experiment.stage_index+1<int(config.stages.size()))enter_stage(experiment.stage_index+1,true);
            else experiment.automatic=false;
        }
        if(ok){apply_stimulus_commands();synchronize_stimulators(true);}
        return ok;
    } catch(const std::exception& e) {
        stepping=false;error=std::string("Integration failed: ")+e.what();return false;
    } catch(...) {stepping=false;error="Integration failed with an unknown exception.";return false;}
}
bool Simulator::step_impl() {
    const auto& c=config;const auto& p=c.parameters;
    prepare_workers();
    prepare_solver_views();
    // Both callbacks and disposable views are refreshed at this safe boundary;
    // checkpoint restore and Simulator moves require no serialized cache state.
    flow.rhs=[this](const State& y,State& dy){
        if(compact_network)compact_derivative(y,dy);
        else derivative(config,y,held_input_time,dy,&plasticity,flow.nonlinear_prefix!=0,&held_inputs);
    };
    flow.exact=[this](const State& y,double h,State& out){exact_flow(y,h,out);};
    flow.error_norm={};
    if(pool)flow.error_norm=[this](const State& old,const State& next,const State& full){return parallel_error_norm(old,next,full);};
    flow.threshold_sample={};
    flow.event_workers=pool.get();flow.worker_threshold_sample={};
    if(pool)flow.worker_threshold_sample=[this](const State& y,double h,int coordinate,int id) {
        auto& scratch=worker_projection[id];
        return projected_threshold(y,h,coordinate,scratch.flow,scratch.initial);
    };
    if(c.astrocytes.empty())flow.threshold_sample=[this](const State& y,double h,int coordinate) {
        return neuron_threshold_sample(y,h,coordinate);
    };
    const double macro_end=std::min({double(steps+1)*c.dt,c.duration,experiment.stage_active?experiment.stage_end:std::numeric_limits<double>::infinity()});
    size_t pieces=0;
    while(time<macro_end) {
        apply_stimulus_commands();synchronize_stimulators();
        if(++pieces>200000) {error="Too many internal steps; revise extreme kinetic parameters.";return false;}
        const double sample_time=double(next_sample)*c.sample;
        const double delivery_time=pending_deliveries.empty() ? std::numeric_limits<double>::infinity() : pending_deliveries.front().t_arrival;
        if(delivery_time<=time) {error="Delivery queue contains an already due event; reset the experiment.";return false;}
        const double command_time=stimulus_commands.empty()?std::numeric_limits<double>::infinity():stimulus_commands.front().time;
        const double limit=next_input_boundary(c,time,std::min({macro_end,sample_time,delivery_time,command_time}));
        log_stimuli(time);
        held_input_time=time; // Right-continuous [start, stop) input on this segment.
        {
            STUDIO_SCOPE(external);
            for(int i=0;i<int(c.neurons.size());++i)held_inputs[i]=noise_cache.current(c,i,time);
        }
        double h=std::min({limit-time,suggested_step,kinetic_step_limit()});
        auto& group=event_group;group.clear();
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
        if(!c.bounded_journals&&history.events.size()+group.size()>c.journal_capacity) {error="Event recording limit exceeded; shorten the experiment.";return false;}
        const double event_time=(advance==limit-time ? limit : time+advance);
        if(!(event_time>time)) {error="Event localization did not advance the clock.";return false;}
        auto& next=flow.next;
        STUDIO_SCOPE(event_jumps);
        auto& fired=event_fired;auto& released=event_released;
        fired.assign(c.neurons.size(),false);released.assign(c.astrocytes.size(),false);
        for(int index:group) {
            if(index<int(c.neurons.size()))fired[index]=true;
            else released[index-int(c.neurons.size())]=true;
        }
        auto& deliveries=event_deliveries;auto& created=event_created;
        deliveries.clear();created.clear();
        // Inspect just the due heap subtree, without consuming events until
        // the whole event group is validated and committed.
        auto& inspect=event_inspect;inspect.clear();
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
        if(!c.bounded_journals&&history.deliveries.size()+deliveries.size()>c.journal_capacity) {
            error="Delivery journal limit (2000000) exceeded; shorten the experiment.";return false;
        }
        std::sort(deliveries.begin(),deliveries.end(),[](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);});
        auto& arrivals=event_arrivals;arrivals.assign(c.synapses.size(),0);
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
        // Without a threshold event or delivery, next is unchanged since the
        // successful pre-jump bounds check above.
        if((!group.empty()||!deliveries.empty())&&!physical(next)) {error="Event jump left the physical state bounds.";return false;}
        // Grouped old traces, one weight update/clamp, then new traces. Weights
        // affect the remaining flow immediately, including existing tails.
        {
        STUDIO_SCOPE(stdp);
        auto update=[&](size_t begin,size_t end,StdpDecayCache& cache) {
            for(size_t j=begin;j<end;++j) {
                const auto& s=c.synapses[j];
                update_stdp_cached(s,plasticity[j],s.stdp_timing==StdpTiming::Arrival?arrivals[j]:fired[s.source],
                    fired[s.target],advance,experiment.learning?experiment.learning_rate:0,cache);
            }
        };
        if(pool)pool->run(c.synapses.size(),[&](size_t begin,size_t end,int id){update(begin,end,worker_stdp_caches[id].cache);});
        else update(0,c.synapses.size(),stdp_decay_cache);
        }
        for(size_t k=0;k<consumed;++k) {
            std::pop_heap(pending_deliveries.begin(),pending_deliveries.end(),DeliveryLater{});pending_deliveries.pop_back();
        }
        for(const auto& e:created) {
            pending_deliveries.push_back(e);std::push_heap(pending_deliveries.begin(),pending_deliveries.end(),DeliveryLater{});
        }
        {
        STUDIO_SCOPE(journals);
        trim_journals(group.size(),deliveries.size());
        capture_activity(deliveries,fired,event_time);
        history.deliveries.insert(history.deliveries.end(),deliveries.begin(),deliveries.end());
        for(int i=0;i<int(c.neurons.size());++i)if(fired[i]){++spikes[i];history.events.push_back({event_time,i,false});}
        for(int i=0;i<int(c.astrocytes.size());++i)if(released[i]){++releases[i];history.events.push_back({event_time,i,true});}
        }
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
