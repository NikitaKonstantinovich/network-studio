#include "network.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace studio;
void require(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
std::ofstream file(const std::filesystem::path& p) {std::ofstream s(p);s<<std::setprecision(17);return s;}
Config base() {
    Config c=preset(4);c.name="Independent synaptic audit";c.neurons.resize(2,c.neurons[0]);
    for(auto& n:c.neurons){n.current=0;n.amplitude=0;n.start=0;n.stop=1;}
    c.astrocytes.clear();c.gaps.clear();c.synapses.clear();c.noise_sigma=0;c.sample=.001;c.duration=.030013;
    return c;
}
int main(int argc,char**argv)try {
    require(argc==2,"Expected output directory");std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
    auto cases=file(out/"cases.csv");
    cases<<"case,model,dt,U,tau_f,tau_d,tau_syn,weight,gain,conductance,reversal,enabled,stdp,rule,a_plus,a_minus,tau_plus,tau_minus,wmin,wmax,omega_y,Y_release\n";
    auto data=file(out/"states.csv");
    data<<"case,time,pre,post,u,x,s,Y,weight,pre_trace,post_trace,current_at_minus65\n";
    const std::vector<double> pre_times={.001,.003,.00305,.009,.010,.017,.020,.025};
    const std::vector<double> post_times={.001,.002,.00305,.00905,.013,.017,.02005,.025};
    int number=0;size_t rows=0;
    for(int model=0;model<3;++model)for(int variant=0;variant<9;++variant)for(double dt:{.00005,.000025}) {
        Config c=base();c.dt=dt;Synapse s;s.model=SynapseModel(model);s.source=0;s.target=1;
        s.release=.25;s.tau_f=.2;s.tau_d=.3;s.tau_syn=.005;s.weight=.6;s.gain=.01;s.conductance=.0001;
        if(variant==1){s.tau_d=s.tau_syn=.010;s.tau_f=.04;}
        if(variant==2)s.release=0;
        if(variant==3)s.release=1;
        if(variant==4){s.tau_d=.001;s.tau_syn=.03;s.tau_f=.001;}
        if(variant==5)s.enabled=false;
        if(variant==6){if(model==0)s.reversal=-80;else s.weight=-.6;}
        if(variant>=7){s.stdp=true;s.stdp_rule=variant==7?StdpRule::Additive:StdpRule::Multiplicative;
            s.a_plus=.2;s.a_minus=.25;s.tau_plus=.02;s.tau_minus=.007;s.weight_min=.2;s.weight_max=.8;}
        c.synapses={s};require(validate(c).empty(),validate(c));Simulator sim(c);require(sim.error.empty(),sim.error);
        const int b=syn_offset(c,0);
        // Nontrivial admissible initial synaptic state isolates the equations;
        // these are harness-only states, not an additional production feature.
        sim.state[b]=model==1?0:.31;sim.state[b+1]=model==1?1:.4;sim.state[b+2]=.2;sim.state[b+3]=120;
        const std::string name="m"+std::to_string(model)+"v"+std::to_string(variant)+"dt"+(dt>.00003?"50":"25");
        cases<<name<<','<<model<<','<<dt<<','<<s.release<<','<<s.tau_f<<','<<s.tau_d<<','<<s.tau_syn<<','<<s.weight<<','<<s.gain<<','<<s.conductance<<','<<s.reversal<<','<<s.enabled<<','<<s.stdp<<','<<int(s.stdp_rule)<<','<<s.a_plus<<','<<s.a_minus<<','<<s.tau_plus<<','<<s.tau_minus<<','<<s.weight_min<<','<<s.weight_max<<','<<c.parameters[na::Omega_Y]<<','<<c.parameters[na::Y_release]<<'\n';
        auto emit=[&](bool pre,bool post) {
            auto y=sim.state;y[4]=-65;State d,off;
            derivative(c,y,sim.time,d,&sim.plasticity);auto disabled=c;disabled.synapses[0].enabled=false;
            derivative(disabled,y,sim.time,off,&sim.plasticity);
            data<<name<<','<<sim.time<<','<<pre<<','<<post<<','<<sim.state[b]<<','<<sim.state[b+1]<<','<<sim.state[b+2]<<','<<sim.state[b+3]<<','<<sim.plasticity[0].weight<<','<<sim.plasticity[0].pre_trace<<','<<sim.plasticity[0].post_trace<<','<<(d[4]-off[4])/1000<<'\n';++rows;
        };
        emit(false,false);
        while(!sim.done()) {
            const double next=std::min(double(sim.steps+1)*c.dt,c.duration);
            auto at=[&](const std::vector<double>& times){return std::any_of(times.begin(),times.end(),[&](double t){return std::abs(t-next)<1e-12;});};
            const bool pre=at(pre_times),post=at(post_times);
            // An explicit event injector: reset V/u before each step so the
            // selected neuron alone crosses at this step's end. This isolates
            // synaptic and STDP updates from neuronal threshold accuracy.
            sim.state[0]=pre?29:-65;sim.state[1]=-13;sim.state[4]=post?29:-65;sim.state[5]=-13;
            const auto old=sim.spikes;require(sim.step(),sim.error);
            require(sim.spikes[0]-old[0]==int(pre)&&sim.spikes[1]-old[1]==int(post),"Event injection mismatch");
            emit(pre,post);
        }++number;
    }
    // Direct derivative probes cover polarity, reversal and HH versus Izh
    // scaling without replacing the production derivative implementation.
    auto currents=file(out/"currents.csv");
    currents<<"model,neuron_model,V,E,weight,gain,g,Cm,s,current,enabled\n";
    for(int model=0;model<3;++model)for(int target=0;target<2;++target)
    for(double v:{-90.,-65.,-40.,0.,40.})for(double w:{0.,.7,1.4})for(double e:{-80.,0.})for(bool enabled:{false,true}) {
        Config c=base();c.parameters[na::C_m]=2.5;c.neurons[1].model=NeuronModel(target);
        Synapse s;s.model=SynapseModel(model);s.weight=(model>0&&e<0)?-w:w;s.gain=17;s.conductance=.31;s.reversal=e;s.enabled=enabled;c.synapses={s};
        State y=initial_state(c),dy,zero;y[4]=v;y[syn_offset(c,0)+2]=.43;
        derivative(c,y,0,dy);auto disabled=c;disabled.synapses[0].enabled=false;derivative(disabled,y,0,zero);
        double recovered=(dy[4]-zero[4])/1000*(target==0?c.parameters[na::C_m]:1);
        currents<<model<<','<<target<<','<<v<<','<<e<<','<<s.weight<<','<<s.gain<<','<<s.conductance<<','<<c.parameters[na::C_m]<<",.43,"<<recovered<<','<<enabled<<'\n';
    }
    // Long traces and large amplitudes stress saturation beyond the existing
    // short three-protocol CLI checker. Each row is one grouped time point.
    auto pairs=file(out/"pairs.csv");
    pairs<<"case,time,pre,post,weight,pre_trace,post_trace,rule,Aplus,Aminus,tauplus,tauminus,wmin,wmax,initial\n";
    for(int rule=0;rule<2;++rule)for(int mode=0;mode<4;++mode) {
        Synapse s;s.stdp=true;s.stdp_rule=StdpRule(rule);s.a_plus=mode==0?0:.31;s.a_minus=mode==1?0:.27;
        s.tau_plus=.003;s.tau_minus=.013;s.weight_min=.2;s.weight_max=.8;
        const double initial=mode==2?.2:mode==3?.8:.53;StdpState state{initial};double t=0;
        for(int k=0;k<350;++k) {
            const double dt=k==0?0:k%17==0?.009:.00005;t+=dt;
            const bool pre=(k%3!=2),post=(k%5!=4);update_stdp(s,state,pre,post,dt);
            pairs<<"r"<<rule<<"m"<<mode<<','<<t<<','<<pre<<','<<post<<','<<state.weight<<','<<state.pre_trace<<','<<state.post_trace<<','<<rule<<','<<s.a_plus<<','<<s.a_minus<<','<<s.tau_plus<<','<<s.tau_minus<<','<<s.weight_min<<','<<s.weight_max<<','<<initial<<'\n';
        }
    }
    std::cout<<"PASS native harness: "<<number<<" state cases, "<<rows<<" state rows; 360 derivative cases; 2800 grouped STDP updates\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
