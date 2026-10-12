// Same event-aware flow engine as Simulator, with the exact homogeneous
// per-source pool / per-target current aggregation retained for large graphs.
bool Population::step() {
    if(done())return false;
    const size_t count=state.size();const auto& n=config.neurons[0];
    const auto& s=synapse;const auto& p=config.parameters;
    State y(7*count);
    for(size_t i=0;i<count;++i) {
        const auto& v=state[i];const size_t b=7*i;
        y[b]=v.v;y[b+1]=v.u;y[b+2]=v.current;y[b+3]=v.tm_u;y[b+4]=v.tm_x;y[b+5]=v.tm_y;y[b+6]=v.glutamate;
    }
    auto unpack=[&](const State& value) {
        std::vector<NodeState> result(count);
        for(size_t i=0;i<count;++i) {const size_t b=7*i;result[i]={value[b],value[b+1],value[b+2],value[b+3],value[b+4],value[b+5],value[b+6]};}
        return result;
    };
    flow.rhs=[&](const State& v,State& d) {
        d.assign(v.size(),0);
        for(size_t i=0;i<count;++i) {
            const size_t b=7*i;
            d[b]=1000*(.04*v[b]*v[b]+5*v[b]+140-v[b+1]+input[i]+v[b+2]);
            d[b+1]=1000*n.izh_a*(n.izh_b*v[b]-v[b+1]);
            // The remaining independent linear coordinates are supplied exactly
            // at every Runge-Kutta stage, so no approximate slopes are needed.
        }
    };
    flow.exact=[&](const State& old,double h,State& out) {
        for(size_t i=0;i<count;++i) {
            const size_t b=7*i;const double decay=std::exp(-h/s.tau_syn);
            out[b+2]=old[b+2]*decay;out[b+5]=old[b+5]*decay;out[b+6]=old[b+6]*std::exp(-h*p[na::Omega_Y]);
            out[b+3]=old[b+3];out[b+4]=old[b+4];
            if(s.model==SynapseModel::TsodyksMarkram) {
                out[b+3]=old[b+3]*std::exp(-h/s.tau_f);
                out[b+4]=old[b+4]+(1-old[b+4]-old[b+5])*(-std::expm1(-h/s.tau_d))+
                    old[b+5]*numerical::recovered(1/s.tau_d,1/s.tau_syn,h);
            }
        }
    };
    auto physical=[&](const State& v) {
        for(size_t i=0;i<count;++i) {const size_t b=7*i;
            for(size_t j=0;j<7;++j)if(!std::isfinite(v[b+j]))return false;
            if(v[b]<-200||v[b]>200||std::abs(v[b+1])>1e5||v[b+3]<-1e-10||v[b+3]>1+1e-10||
               v[b+4]<-1e-10||v[b+4]>1+1e-10||v[b+5]<-1e-10||v[b+6]<-1e-10||
               (s.model==SynapseModel::TsodyksMarkram && v[b+4]+v[b+5]>1+1e-10))return false;
        }return true;
    };
    fired.clear();events.clear();samples.clear();sampled=false;
    const double macro_end=std::min(double(steps+1)*config.dt,config.duration);
    size_t pieces=0;
    while(time<macro_end) {
        if(++pieces>200000){error="Too many internal population steps.";return false;}
        const double sample_time=double(next_sample)*config.sample;
        const double limit=next_input_boundary(config,time,std::min(macro_end,sample_time));
        for(size_t i=0;i<count;++i)input[i]=external_current(config,int(i),time);
        double rate=0;
        for(size_t i=0;i<count;++i)rate=std::max(rate,1000*(std::abs(.08*y[7*i]+5)+n.izh_a));
        double h=std::min({limit-time,suggested_step,rate>0 ? .25/rate : config.dt}),norm=0,advance=0;
        std::vector<int> group;bool accepted=false;
        for(int retry=0;retry<64;++retry) {
            if(!(h>0)||time+h==time){error="Population step below clock resolution.";return false;}
            norm=flow.trial(y,h);
            if(norm<=1) {advance=flow.events(y,h,time,thresholds,group);if(physical(flow.next)){accepted=true;break;}}
            h*=.5;
        }
        if(!accepted){error="Cannot resolve population dynamics.";return false;}
        const double event_time=advance==limit-time ? limit : time+advance;
        auto& next=flow.next;
        for(int i:group) {
            const size_t b=7*i;next[b]=n.izh_c;next[b+1]+=n.izh_d;
            double release=1;
            if(s.model==SynapseModel::TsodyksMarkram) {next[b+3]+=s.release*(1-next[b+3]);release=next[b+3]*next[b+4];next[b+4]-=release;}
            next[b+5]+=release;next[b+6]+=p[na::Y_release]*release;
            for(const auto& edge:outgoing[i])next[7*edge.target+2]+=edge.weight*s.gain*release;
        }
        if(!physical(next)){error="Population event left physical bounds.";return false;}
        for(int i:group){fired.push_back(i);events.push_back({event_time,i,false});++spikes[i];}
        y.swap(next);time=event_time;
        for(int index:group)thresholds[index].armed=false;
        for(size_t i=0;i<count;++i)if(y[7*i]<n.izh_peak-numerical::threshold_tolerance(n.izh_peak))thresholds[i].armed=true;
        suggested_step=std::min(config.dt,h*(norm>1e-16 ? std::clamp(.9*std::pow(norm,-.2),.5,2.) : 2.));
        if(time==sample_time){samples.push_back({time,unpack(y)});++next_sample;}
    }
    state=unpack(y);++steps;
    if(time==config.duration && (samples.empty() || samples.back().first!=time))samples.push_back({time,state});
    sampled=!samples.empty();return true;
}
