#include "io.h"
#include "vendor/json/json.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <chrono>
#include <functional>
#include <algorithm>
#include <sstream>
#include <limits>

using namespace studio;
static int checks=0;
static void require(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
static void rejects(const std::function<void()>& fn,const char* message) {
    bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require(rejected,message);
}
static Simulator run(Config c) {
    Simulator s(c);while(!s.done())if(!s.step())break;require(s.error.empty(),s.error.c_str());return s;
}
static bool close(double a,double b,double tolerance=1e-9) { return std::abs(a-b)<=tolerance; }
static Config izh_config() {
    Config c=preset(3);c.name="Izhikevich regression";c.duration=.1;c.sample=.0001;
    auto& n=c.neurons[0];n.model=NeuronModel::Izhikevich;n.amplitude=0;
    return c;
}
static void izhikevich_tests() {
    Config c=izh_config();auto& n=c.neurons[0];n.current=4;
    State y=initial_state(c),dy{};derivative(c,y,0,dy);
    require(close(y[1],n.izh_b*n.voltage)&&y[2]==0&&y[3]==0,"Izhikevich initial state is incorrect.");
    require(close(dy[0],1000*(.04*y[0]*y[0]+5*y[0]+140-y[1]+4)),"Izhikevich voltage equation or time units are wrong.");
    require(close(dy[1],1000*n.izh_a*(n.izh_b*y[0]-y[1]))&&dy[2]==0&&dy[3]==0,"Izhikevich recovery equation or reserved slots are wrong.");
    n.equilibrium_u=false;n.izh_u=-11;require(initial_state(c)[1]==-11,"Manual Izhikevich recovery initialization ignored.");
    c=izh_config();c.neurons[0].voltage=-70;c.duration=1;
    auto resting=run(c);require(resting.spikes[0]==0&&close(resting.state[0],-70,1e-8),"Izhikevich resting equilibrium drifted or spiked.");
    c=izh_config();c.neurons[0].izh_b=.27;c.neurons[0].current=4;c.duration=1;
    auto tonic=run(c);require(tonic.spikes[0]>5,"Tonic Izhikevich neuron does not fire.");
    c.dt/=2;auto refined=run(c);require(tonic.spikes==refined.spikes,"Izhikevich spike counts do not converge under dt refinement.");
    for(size_t i=0;i<tonic.history.events.size();++i)
        require(std::abs(tonic.history.events[i].time-refined.history.events[i].time)<.002,"Izhikevich spike timing changes excessively under dt refinement.");
    for(double rate:{2.,10.,20.}) {
        c=izh_config();auto& stimulated=c.neurons[0];
        // A 1 ms pulse yields one event. The guide's 30 x 3 ms pulse can
        // produce two spikes with the standard equations and exact reset.
        c.duration=2.1;c.sample=.0005;stimulated.amplitude=30;stimulated.width=.001;
        stimulated.start=.1;stimulated.stop=c.duration;stimulated.frequency=rate;
        auto driven=run(c);require(driven.spikes[0]==uint64_t(2*rate),"RS pulse response is not one spike per stimulus at 2, 10 or 20 Hz.");
        std::cout<<"RS pulse "<<rate<<" Hz: "<<driven.spikes[0]<<" spikes\n";
    }
    c=izh_config();c.duration=2.1;c.sample=.0005;c.neurons[0].amplitude=30;c.neurons[0].width=.003;
    c.neurons[0].start=.1;c.neurons[0].stop=2.1;c.neurons[0].frequency=2;
    require(run(c).spikes[0]==8,"The standard model's two-spike response to 30 x 3 ms pulses changed.");
    c=izh_config();c.neurons[0].voltage=29;c.neurons[0].equilibrium_u=false;c.neurons[0].izh_u=-13;
    Simulator resetting(c);require(resetting.step()&&resetting.spikes[0]==1,"Izhikevich threshold crossing was not recorded.");
    require(resetting.history.events[0].time<c.dt && resetting.state[0]!=c.neurons[0].izh_c &&
        std::abs(resetting.state[0]-c.neurons[0].izh_c)<1 && std::abs(resetting.state[1]-(-13+c.neurons[0].izh_d))<.01,
        "Izhikevich reset did not continue the remaining flow.");
    require(resetting.history.events.size()==1&&resetting.history.events[0].index==0,"Izhikevich reset generated incorrect events.");
    c.neurons[0].voltage=c.neurons[0].izh_peak;require(!validate(c).empty(),"Initial Izhikevich voltage at peak accepted.");
    c=izh_config();c.neurons[0].izh_c=c.neurons[0].izh_peak;require(!validate(c).empty(),"Izhikevich reset at peak accepted.");
    c=izh_config();c.neurons[0].model=static_cast<NeuronModel>(99);require(!validate(c).empty(),"Invalid neuron enum accepted.");
}
static void synapse_tests() {
    Config c=izh_config();c.neurons.push_back(c.neurons[0]);c.neurons[1].name="Target";
    c.synapses={{0,1,-1}};auto& syn=c.synapses[0];syn.model=SynapseModel::TsodyksMarkram;
    syn.weight=.7;syn.gain=40;syn.release=.25;syn.tau_d=.3;syn.tau_f=.2;syn.tau_syn=.005;
    State y=initial_state(c),dy{},without{};const int b=syn_offset(c,0);
    y[b]=.1;y[b+1]=.8;y[b+2]=.05;
    derivative(c,y,0,dy);
    require(close(dy[b],-.1/syn.tau_f)&&close(dy[b+1],.15/syn.tau_d)&&close(dy[b+2],-.05/syn.tau_syn),"TM continuous dynamics are not the three-pool model.");
    require(close(-dy[b+1]-dy[b+2],.05/syn.tau_syn-.15/syn.tau_d),"TM inactive-pool flux is not conserved.");
    syn.enabled=false;derivative(c,y,0,without);syn.enabled=true;
    require(close(dy[4]-without[4],1000*.7*40*.05),"Excitatory current-based synapse has wrong strength or units.");
    syn.weight=-.7;derivative(c,y,0,dy);
    require(close(dy[4]-without[4],-1000*.7*40*.05),"Inhibitory current-based synapse has wrong polarity.");
    syn.weight=.7;c.neurons[0].voltage=29;c.neurons[0].equilibrium_u=false;c.neurons[0].izh_u=-13;
    Simulator active(c);active.state[b]=.1;active.state[b+1]=.8;active.state[b+2]=.05;
    syn.enabled=false;Simulator inactive(c);inactive.state=active.state;
    require(active.step()&&inactive.step()&&active.spikes[0]==1,"TM spike-jump fixture failed.");
    const double event=active.history.events[0].time,tail=c.dt-event;
    auto recover=[&](double x,double y,double h) {
        const double a=1/syn.tau_d,r=1/syn.tau_syn;
        const double integral=std::abs(a-r)<1e-12 ? h*std::exp(-a*h) : (std::exp(-r*h)-std::exp(-a*h))/(a-r);
        return 1+(x-1)*std::exp(-a*h)-a*y*integral;
    };
    const double before_u=.1*std::exp(-event/syn.tau_f),before_x=recover(.8,.05,event),before_y=.05*std::exp(-event/syn.tau_syn);
    const double u=before_u+syn.release*(1-before_u),released=u*before_x;
    require(close(active.state[b],u*std::exp(-tail/syn.tau_f),1e-11)&&
        close(active.state[b+1],recover(before_x-released,before_y+released,tail),1e-11)&&
        close(active.state[b+2],(before_y+released)*std::exp(-tail/syn.tau_syn),1e-11),
        "TM flow/jump/remaining flow differs from the exact resource solution.");
    require(active.state[b+1]>=0 && active.state[b+2]>=0 && active.state[b+1]+active.state[b+2]<=1,
        "TM spike jump loses resource positivity.");
    syn.enabled=true;syn.model=SynapseModel::ExponentialCurrent;
    Simulator exponential(c);exponential.state[b+2]=.05;
    require(exponential.step()&&exponential.spikes[0]==1,"Exponential synapse spike fixture failed.");
    require(close(exponential.state[b],0)&&close(exponential.state[b+1],1)&&
        close(exponential.state[b+2],std::exp(-(c.dt-exponential.history.events[0].time)/syn.tau_syn)+.05*std::exp(-c.dt/syn.tau_syn),1e-11),
        "Exponential synapse does not decay its release from the localized spike time.");
    c.neurons[0].voltage=-70;c.neurons[0].equilibrium_u=true;c.duration=.2;syn.model=SynapseModel::TsodyksMarkram;
    Simulator recovering(c);recovering.state[b+1]=.2;recovering.state[b+2]=.7;
    while(!recovering.done()) {
        require(recovering.step(),"TM recovery failed.");
        require(recovering.state[b+1]>=0&&recovering.state[b+2]>=0&&recovering.state[b+1]+recovering.state[b+2]<=1+1e-9,"TM resource pools left the unit simplex.");
    }
    require(recovering.state[b+1]>.2&&recovering.state[b+2]<1e-10,"TM resource recovery does not decay the active pool.");
    syn.model=static_cast<SynapseModel>(99);require(!validate(c).empty(),"Invalid synapse enum accepted.");
}
static Config stdp_config() {
    Config c=izh_config();c.name="STDP ordered pair";c.duration=.15;
    c.neurons.push_back(c.neurons[0]);
    for(size_t i=0;i<c.neurons.size();++i) {
        auto& n=c.neurons[i];n.voltage=-70;n.amplitude=30;n.width=.001;
        n.frequency=10;n.start=.02+.01*i;n.stop=c.duration;
    }
    c.synapses={{0,1,-1}};auto& s=c.synapses[0];
    s.model=SynapseModel::ExponentialCurrent;s.gain=0;s.stdp=true;
    s.a_plus=.1;s.a_minus=.12;s.tau_plus=.02;s.tau_minus=.03;
    return c;
}
static void stdp_tests() {
    Synapse s;s.stdp=true;s.a_plus=.02;s.a_minus=.03;s.tau_plus=.02;s.tau_minus=.03;
    StdpState causal{s.weight},anti{s.weight};
    update_stdp(s,causal,true,false,0);update_stdp(s,causal,false,true,.01);
    require(close(causal.weight,1+s.a_plus*std::exp(-.01/s.tau_plus),1e-13),"Causal STDP pair does not follow A+ exp(-dt/tau+).");
    update_stdp(s,anti,false,true,0);update_stdp(s,anti,true,false,.01);
    require(close(anti.weight,1-s.a_minus*std::exp(-.01/s.tau_minus),1e-13),"Anticausal STDP pair does not follow -A- exp(-dt/tau-).");
    StdpState triplet{s.weight};
    update_stdp(s,triplet,true,false,0);update_stdp(s,triplet,true,false,.01);update_stdp(s,triplet,false,true,.01);
    require(close(triplet.weight,1+s.a_plus*(std::exp(-.02/s.tau_plus)+std::exp(-.01/s.tau_plus)),1e-13),
        "STDP ignores earlier pairs instead of summing all-to-all.");
    const auto previous=triplet;update_stdp(s,triplet,false,false,.04);
    require(triplet.weight==previous.weight&&close(triplet.pre_trace,previous.pre_trace*std::exp(-.04/s.tau_plus),1e-13)&&
        close(triplet.post_trace,previous.post_trace*std::exp(-.04/s.tau_minus),1e-13),"STDP traces do not decay exactly between spikes.");
    StdpState simultaneous{s.weight};update_stdp(s,simultaneous,true,true,0);
    require(simultaneous.weight==s.weight&&simultaneous.pre_trace==1&&simultaneous.post_trace==1,
        "Simultaneous spikes incorrectly form a zero-lag STDP pair.");
    StdpState prior{1,2,3};update_stdp(s,prior,true,true,.01);
    require(close(prior.weight,1+s.a_plus*2*std::exp(-.01/s.tau_plus)-s.a_minus*3*std::exp(-.01/s.tau_minus),1e-13),
        "Simultaneous events do not use only decayed prior spike history.");
    StdpState upper{1.99,3,1},lower{.01,1,3};
    update_stdp(s,upper,true,true,0);update_stdp(s,lower,true,true,0);
    require(upper.weight==s.weight_max&&lower.weight==s.weight_min,"STDP must clamp once after the total simultaneous update.");
    StdpState frozen{1,2,3};s.stdp=false;update_stdp(s,frozen,true,true,.01);
    require(frozen.weight==1,"STDP-off synapse changed weight.");
    s.stdp=true;s.enabled=false;update_stdp(s,frozen,true,true,.01);
    require(frozen.weight==1,"Disabled synapse learned.");
    s.enabled=true;StdpState repotentiated{0};update_stdp(s,repotentiated,true,false,0);update_stdp(s,repotentiated,false,true,.01);
    require(repotentiated.weight>0,"An additive zero-weight synapse cannot potentiate.");
    s.stdp_rule=StdpRule::Multiplicative;s.weight_min=.25;s.weight_max=2;
    StdpState multi_causal{.8},multi_anti{.8},multi_both{.8,2,3};
    update_stdp(s,multi_causal,true,false,0);update_stdp(s,multi_causal,false,true,.01);
    update_stdp(s,multi_anti,false,true,0);update_stdp(s,multi_anti,true,false,.01);
    update_stdp(s,multi_both,true,true,.01);
    require(close(multi_causal.weight,.8+s.a_plus*(2-.8)*std::exp(-.01/s.tau_plus),1e-13),
        "Multiplicative LTP does not scale with the distance to the upper bound.");
    require(close(multi_anti.weight,.8-s.a_minus*(.8-.25)*std::exp(-.01/s.tau_minus),1e-13),
        "Multiplicative LTD does not scale with the distance to the lower bound.");
    require(close(multi_both.weight,.8+s.a_plus*(2-.8)*2*std::exp(-.01/s.tau_plus)-
        s.a_minus*(.8-.25)*3*std::exp(-.01/s.tau_minus),1e-13),
        "Multiplicative simultaneous updates must evaluate both polarities at the same old weight.");
    StdpState multi_lower{s.weight_min};update_stdp(s,multi_lower,true,false,0);update_stdp(s,multi_lower,false,true,.01);
    require(multi_lower.weight>s.weight_min,"A multiplicative synapse cannot recover from its lower weight bound.");

    Config c=stdp_config();auto learned=run(c);const auto& edge=c.synapses[0];
    double expected=edge.weight;
    for(const auto& pre:learned.history.events)if(!pre.astro&&pre.index==edge.source)
        for(const auto& post:learned.history.events)if(!post.astro&&post.index==edge.target) {
            const double delay=post.time-pre.time;
            if(delay>0)expected+=edge.a_plus*std::exp(-delay/edge.tau_plus);
            else if(delay<0)expected-=edge.a_minus*std::exp(delay/edge.tau_minus);
        }
    require(learned.spikes[0]==2&&learned.spikes[1]==2,"STDP paired-stimulus fixture has unexpected spike counts.");
    require(expected>edge.weight&&close(learned.plasticity[0].weight,expected,1e-11),
        "Simulator weight does not match the independent all-pairs sum over its spike events.");
    require(learned.config.synapses[0].weight==edge.weight,"Learning changed the initial experiment configuration.");
    require(learned.history.weights.size()==1&&learned.history.weights[0].size()==learned.history.time.size()&&
        learned.history.weights[0].front()==edge.weight&&learned.history.weights[0].back()==learned.plasticity[0].weight,
        "Weight history is not sample-aligned or does not include initial/final weights.");
    auto partial=c;partial.duration+=.000007;auto tail=run(partial);
    require(tail.time==partial.duration&&close(tail.plasticity[0].weight,learned.plasticity[0].weight,1e-13)&&
        close(tail.plasticity[0].pre_trace,learned.plasticity[0].pre_trace*std::exp(-.000007/edge.tau_plus),1e-12)&&
        close(tail.plasticity[0].post_trace,learned.plasticity[0].post_trace*std::exp(-.000007/edge.tau_minus),1e-12),
        "STDP silent final partial step uses an incorrect trace decay interval.");
    Simulator reset(c);
    require(reset.plasticity[0].weight==edge.weight&&reset.plasticity[0].pre_trace==0&&reset.plasticity[0].post_trace==0&&
        reset.history.weights[0].size()==1&&reset.history.weights[0][0]==edge.weight,"Reset did not restore initial weights and clear traces/history.");
    auto permuted=c;std::swap(permuted.neurons[0],permuted.neurons[1]);
    permuted.synapses[0].source=1;permuted.synapses[0].target=0;auto reordered=run(permuted);
    require(close(learned.plasticity[0].weight,reordered.plasticity[0].weight,1e-13),"STDP depends on neuron iteration order.");
    auto same_time=c;same_time.duration=.09;same_time.neurons[1].start=same_time.neurons[0].start;
    auto simultaneous_run=run(same_time);
    require(simultaneous_run.spikes[0]==1&&simultaneous_run.spikes[1]==1&&simultaneous_run.plasticity[0].weight==edge.weight,
        "Simulator serially paired same-step spikes.");
    auto off=c;off.synapses[0].stdp=false;auto unchanged=run(off);
    require(unchanged.plasticity[0].weight==edge.weight,"Simulator learned with STDP off.");
    off=c;off.synapses[0].enabled=false;unchanged=run(off);
    require(unchanged.plasticity[0].weight==edge.weight,"Simulator learned through a disabled edge.");
    for(auto model:{SynapseModel::ConductanceSTP,SynapseModel::ExponentialCurrent,SynapseModel::TsodyksMarkram}) {
        auto current=c;auto& syn=current.synapses[0];syn.model=model;syn.gain=40;syn.stdp=false;
        State y=initial_state(current),initial_current{},live_current{};const int b=syn_offset(current,0);y[b+1]=.5;y[b+2]=.25;
        derivative(current,y,0,initial_current);derivative(current,y,0,live_current,&learned.plasticity);
        const double scale=model==SynapseModel::ConductanceSTP ? syn.conductance*(syn.reversal-y[4]) : syn.gain;
        require(close(live_current[4]-initial_current[4],1000*scale*y[b+2]*(learned.plasticity[0].weight-syn.weight),1e-9),
            "A synapse model ignores its runtime weight in the target current.");
        Simulator baseline(current),weighted(current);baseline.state=y;weighted.state=y;
        weighted.plasticity[0].weight=learned.plasticity[0].weight;
        require(baseline.step()&&weighted.step()&&weighted.state[4]>baseline.state[4],"RK4 integration does not use the learned weight.");
    }
    auto invalid=c;invalid.synapses[0].target=0;require(!validate(invalid).empty(),"STDP autapse accepted.");
    invalid=c;invalid.synapses[0].weight=-.1;require(!validate(invalid).empty(),"Inhibitory current STDP accepted.");
    invalid=c;invalid.synapses[0].model=SynapseModel::ConductanceSTP;invalid.synapses[0].reversal=-80;
    require(!validate(invalid).empty(),"Inhibitory conductance STDP accepted.");
    invalid=c;invalid.synapses[0].weight_min=-.1;require(!validate(invalid).empty(),"Negative STDP lower bound accepted.");
    invalid=c;invalid.synapses[0].weight_min=invalid.synapses[0].weight_max;require(!validate(invalid).empty(),"Empty STDP weight interval accepted.");
    invalid=c;invalid.synapses[0].weight_max=11;require(!validate(invalid).empty(),"Excessive STDP weight limit accepted.");
    invalid=c;invalid.synapses[0].weight=3;require(!validate(invalid).empty(),"Initial STDP weight outside bounds accepted.");
    invalid=c;invalid.synapses[0].tau_plus=0;require(!validate(invalid).empty(),"Zero STDP time constant accepted.");
    invalid=c;invalid.synapses[0].a_minus=-.1;require(!validate(invalid).empty(),"Negative STDP learning amplitude accepted.");
    invalid=c;invalid.synapses[0].stdp_rule=static_cast<StdpRule>(99);require(!validate(invalid).empty(),"Unknown STDP enum accepted.");
}
static void triplet_tests() {
    Synapse s;s.stdp=true;s.stdp_rule=StdpRule::Triplet;s.a_plus=.03;s.a_minus=.04;
    s.tau_plus=s.tau_minus=.01;s.tau_slow=.1;s.weight_min=0;s.weight_max=1;
    StdpState isolated{.4};update_stdp(s,isolated,true,false,0);update_stdp(s,isolated,false,true,.01);
    require(isolated.weight==.4&&isolated.post_slow_trace==1,"Minimal triplet LTP included its current postspike or a pair-LTP term.");
    update_stdp(s,isolated,false,true,.01);
    require(close(isolated.weight,.4+.03*.6*std::exp(-2.)*std::exp(-.1),1e-14),"Triplet slow-post LTP has the wrong timing or scale.");
    StdpState post_pre_post{.4};update_stdp(s,post_pre_post,false,true,0);update_stdp(s,post_pre_post,true,false,.01);
    const double after_ltd=.4-.04*.4*std::exp(-1.);
    require(close(post_pre_post.weight,after_ltd,1e-14),"Minimal triplet LTD differs from multiplicative pair LTD.");
    update_stdp(s,post_pre_post,false,true,.01);
    require(close(post_pre_post.weight,after_ltd+.03*(1-after_ltd)*std::exp(-1.)*std::exp(-.2),1e-14),
        "Analytical post-pre-post triplet failed.");
    StdpState together{.4};update_stdp(s,together,true,true,0);
    require(together.weight==.4&&together.pre_trace==1&&together.post_trace==1&&together.post_slow_trace==1,
        "Fresh simultaneous triplet spikes trained on their mutual zero-lag contribution.");
    update_stdp(s,together,true,true,.01);
    require(close(together.weight,.4+.03*.6*std::exp(-1.)*std::exp(-.1)-.04*.4*std::exp(-1.),1e-14),
        "Simultaneous triplet events did not use only old decayed histories.");
    StdpState upper{.4,100,0,100},lower{.4,0,100,0};
    update_stdp(s,upper,false,true,0);update_stdp(s,lower,true,false,0);
    require(upper.weight==1&&lower.weight==0,"Triplet bounds are not enforced.");
    s.enabled=false;update_stdp(s,together,true,true,.01);
    require(together.pre_trace==0&&together.post_trace==0&&together.post_slow_trace==0,"Disabled triplet traces were retained.");
    s.enabled=true;
    // Independent explicit sums over every prior event, without trace recurrence.
    StdpState actual{.4};double expected=.4,t=0;std::vector<double> pres,posts;
    for(int k=0;k<180;++k) {
        const double dt=k==0?0:k%11==0?.037:.001;t+=dt;
        const bool pre=k%3!=1,post=k%4!=2;
        double p=0,q=0,r=0;
        for(double old:pres)p+=std::exp(-(t-old)/s.tau_plus);
        for(double old:posts){q+=std::exp(-(t-old)/s.tau_minus);r+=std::exp(-(t-old)/s.tau_slow);}
        expected=std::clamp(expected+(post?s.a_plus*(1-expected)*p*r:0)-(pre?s.a_minus*expected*q:0),0.,1.);
        update_stdp(s,actual,pre,post,dt);
        require(close(actual.weight,expected,2e-13),"Triplet implementation differs from explicit prior-event sums.");
        require(close(actual.post_slow_trace,r+int(post),1e-12),"Triplet slow history differs from explicit prior postsynaptic events.");
        if(pre)pres.push_back(t);if(post)posts.push_back(t);
    }
    Config c=preset(6);c.synapses[0]=s;c.synapses[0].weight=.4;c.duration=.3;
    auto native=run(c);
    require(native.plasticity[0].post_slow_trace>0&&native.plasticity[0].weight!=.4,"Simulator did not apply the triplet rule.");
    Simulator reset(c);require(reset.plasticity[0].weight==.4&&reset.plasticity[0].post_slow_trace==0,"Reset retained the slow post trace.");
    c.synapses[0].tau_slow=0;require(!validate(c).empty(),"Invalid triplet slow time constant accepted.");
}
static void interval_stimulus_tests() {
    Config c=izh_config();auto& n=c.neurons[0];n.current=2;n.amplitude=25;n.frequency=1;n.start=0;n.stop=2;
    n.pulse_intervals={{.05,.053},{.07,.073}};
    for(double t:{0.,.049,.053,.06,.069,.073,1.053,1.073,2.05})
        require(external_current(c,0,t)==2,"Interval stimulus leaked beyond a half-open interval or global stop.");
    for(double t:{.05,.051,.07,.072,1.05,1.052,1.07,1.072})
        require(external_current(c,0,t)==27,"Interval stimulus omitted an active pulse or used the wrong amplitude.");
    n.start=.125;n.stop=1.5;n.pulse_intervals={{.125,.25},{.375,.5}};
    require(external_current(c,0,.25)==27&&external_current(c,0,.375)==2&&external_current(c,0,.5)==27&&
        external_current(c,0,1.25)==27&&external_current(c,0,1.5)==2,"Interval phase is not relative to stimulus start.");
    n.frequency=0;require(external_current(c,0,.25)==2,"Zero frequency did not disable a retained interval table.");
    n.start=0;n.stop=5;n.frequency=3;n.pulse_intervals={{0,.003}};
    const double origin=7*(1./3),negative=-std::numeric_limits<double>::infinity();
    require(external_current(c,0,origin)==27&&external_current(c,0,std::nextafter(origin,negative))==2&&
        external_current(c,0,std::nextafter(origin,-negative))==27,
        "Floating-point division selected the wrong repetition at an exact cycle origin.");
    require(external_current(c,0,origin+.003)==2&&
        external_current(c,0,std::nextafter(origin+.003,negative))==27,
        "Repeated interval end is not half-open at adjacent floating-point timestamps.");
    c=izh_config();c.duration=.15;c.sample=.0001;c.neurons[0].amplitude=30;c.neurons[0].start=.05;
    c.neurons[0].stop=.15;c.neurons[0].frequency=10;c.neurons[0].width=.001;
    auto single=run(c);c.neurons[0].pulse_intervals={{0,.001}};auto table=run(c);
    require(single.spikes==table.spikes&&single.state==table.state&&single.history.values==table.history.values,
        "A one-interval stimulus does not reproduce the equivalent legacy pulse on this aligned grid.");
    c.neurons[0].pulse_intervals={{.01,.02},{.015,.03}};require(!validate(c).empty(),"Overlapping intervals accepted.");
    c.neurons[0].pulse_intervals={{.02,.01}};require(!validate(c).empty(),"Negative pulse width accepted.");
    c.neurons[0].pulse_intervals={{.02,.02}};require(!validate(c).empty(),"Zero pulse width accepted.");
    c.neurons[0].pulse_intervals={{0,.101}};require(!validate(c).empty(),"Interval outside the period accepted.");
    c.neurons[0].pulse_intervals.assign(65,{0,.001});require(!validate(c).empty(),"Excessive interval count accepted.");
    c.neurons[0].pulse_intervals={{0,.001},{.002,.003}};c.synapses.clear();
    require(serialize(deserialize(serialize(c)))==serialize(c),"Pulse interval JSON round trip failed.");
    auto encoded=json::jobject::parse(serialize(c));auto neurons=json::jobject::parse(encoded.get("neurons"));
    auto node=json::jobject::parse(neurons.get(size_t(0)));node.remove("pulse_intervals");
    json::jobject old_nodes(true);old_nodes+=json::kvp("",node.as_string());encoded.set("neurons",old_nodes.as_string());
    require(deserialize(encoded.as_string()).neurons[0].pulse_intervals.empty(),"Legacy JSON did not default to a single-pulse train.");
    node.set("pulse_intervals","{}");json::jobject wrong_nodes(true);wrong_nodes+=json::kvp("",node.as_string());encoded.set("neurons",wrong_nodes.as_string());
    rejects([&]{deserialize(encoded.as_string());},"Non-array pulse intervals accepted.");
    c=preset(6);c.synapses[0].stdp_rule=StdpRule::Triplet;c.synapses[0].tau_slow=.123;
    require(serialize(deserialize(serialize(c)))==serialize(c),"Triplet rule/tau_slow JSON round trip failed.");
    encoded=json::jobject::parse(serialize(c));auto synapses=json::jobject::parse(encoded.get("synapses"));
    auto syn=json::jobject::parse(synapses.get(size_t(0)));syn.remove("tau_slow");syn.set("stdp_rule","\"multiplicative\"");
    json::jobject old_synapses(true);old_synapses+=json::kvp("",syn.as_string());encoded.set("synapses",old_synapses.as_string());
    auto legacy=deserialize(encoded.as_string());
    require(legacy.synapses[0].stdp_rule==StdpRule::Multiplicative&&legacy.synapses[0].tau_slow==.1,"Legacy STDP JSON migration altered the pair rule.");
}
static void noise_tests() {
    Config c=izh_config();c.neurons.push_back(c.neurons[0]);c.noise_sigma=8;c.noise_interval=.001;c.seed=12345;
    const double first=external_current(c,0,.0002),second=external_current(c,1,.0002);
    require(std::isfinite(first)&&first==external_current(c,0,.0002),"Noise is not deterministic for fixed seed, node and time.");
    require(first==external_current(c,0,.0008),"Noise changed within one sample interval.");
    require(first!=second&&first!=external_current(c,0,.0012),"Noise is shared between nodes or repeated between intervals.");
    c.neurons[0].individual_noise=true;c.neurons[0].noise_sigma=0;
    require(external_current(c,0,.0002)==0,"Zero individual noise did not override global noise.");
    c.neurons[0].noise_sigma=4;
    require(close(external_current(c,0,.0002),first/2),"Individual noise amplitude is not an override of global noise.");
    c.duration=.03;auto a=run(c),b=run(c);
    require(a.state==b.state&&a.spikes==b.spikes&&a.history.values==b.history.values,"Fixed-seed simulation is not reproducible.");
    ++c.seed;auto other=run(c);require(a.state!=other.state,"Changing noise seed does not alter simulation.");
    c.noise_interval=0;require(!validate(c).empty(),"Zero noise interval accepted.");
}
static void schema_tests() {
    Config c=izh_config();c.noise_sigma=8;c.noise_interval=.002;c.seed=4294967295u;
    c.neurons.push_back(c.neurons[0]);c.neurons[0].izh_a=.03;c.neurons[0].izh_u=-12;
    c.neurons[0].equilibrium_u=false;c.neurons[0].individual_noise=true;c.neurons[0].noise_sigma=2;
    c.synapses={{0,1,-1},{1,0,-1}};c.synapses[0].model=SynapseModel::ExponentialCurrent;
    c.synapses[1].model=SynapseModel::TsodyksMarkram;c.synapses[1].weight=-.8;c.synapses[1].gain=35;
    c.synapses[0].stdp=true;c.synapses[0].stdp_rule=StdpRule::Multiplicative;c.synapses[0].a_plus=.033;c.synapses[0].a_minus=.041;
    c.synapses[0].tau_plus=.031;c.synapses[0].tau_minus=.043;c.synapses[0].weight_min=.25;c.synapses[0].weight_max=3;
    require(serialize(deserialize(serialize(c)))==serialize(c),"Current schema loses model, noise or STDP parameters.");
    auto encoded=json::jobject::parse(serialize(c));encoded.set("seed","4294967296");
    rejects([&]{deserialize(encoded.as_string());},"Out-of-range noise seed accepted.");
    encoded.set("seed","1.5");rejects([&]{deserialize(encoded.as_string());},"Fractional noise seed accepted.");
    auto invalid_model=serialize(c);auto pos=invalid_model.find("izhikevich");invalid_model.replace(pos,10,"unknown");
    rejects([&]{deserialize(invalid_model);},"Unknown neuron model accepted.");
    invalid_model=serialize(c);pos=invalid_model.find("tsodyks_markram");invalid_model.replace(pos,15,"unknown");
    rejects([&]{deserialize(invalid_model);},"Unknown synapse model accepted.");
    auto legacy=json::jobject::parse(serialize(preset(0)));legacy.set("schema","1");
    for(const char* key:{"noise_sigma","noise_interval","seed"})legacy.remove(key);
    auto neurons=json::jobject::parse(legacy.get("neurons"));json::jobject old_neurons(true);
    for(size_t i=0;i<neurons.size();++i) {
        auto n=json::jobject::parse(neurons.get(i));
        for(const char* key:{"model","izh_a","izh_b","izh_c","izh_d","izh_peak","izh_u","equilibrium_u","individual_noise","noise_sigma"})n.remove(key);
        old_neurons+=json::kvp("",n.as_string());
    }
    legacy.set("neurons",old_neurons.as_string());
    auto synapses=json::jobject::parse(legacy.get("synapses"));json::jobject old_synapses(true);
    for(size_t i=0;i<synapses.size();++i) {
        auto s=json::jobject::parse(synapses.get(i));
        for(const char* key:{"model","weight","gain","stdp","stdp_rule","a_plus","a_minus","tau_plus","tau_minus","weight_min","weight_max"})s.remove(key);
        old_synapses+=json::kvp("",s.as_string());
    }
    legacy.set("synapses",old_synapses.as_string());
    require(serialize(deserialize(legacy.as_string()))==serialize(preset(0)),"Schema 1 migration changed legacy HH/STP behavior.");
    legacy.set("schema","2");rejects([&]{deserialize(legacy.as_string());},"Schema 2 accepted missing model/noise fields.");

    Config previous=preset(0);previous.synapses[0].weight=2.5;
    previous.synapses[1].model=SynapseModel::ExponentialCurrent;previous.synapses[1].weight=-.6;
    auto schema2=json::jobject::parse(serialize(previous));schema2.set("schema","2");
    auto prior_synapses=json::jobject::parse(schema2.get("synapses"));json::jobject version2_synapses(true);
    for(size_t i=0;i<prior_synapses.size();++i) {
        auto syn=json::jobject::parse(prior_synapses.get(i));
        for(const char* key:{"stdp","stdp_rule","a_plus","a_minus","tau_plus","tau_minus","weight_min","weight_max"})syn.remove(key);
        version2_synapses+=json::kvp("",syn.as_string());
    }
    schema2.set("synapses",version2_synapses.as_string());
    auto migrated=deserialize(schema2.as_string());previous.synapses[0].weight=1;
    require(serialize(migrated)==serialize(previous),"Schema 2 migration changed current weights or retained previously ignored conductance weights.");
    require(!migrated.synapses[0].stdp&&!migrated.synapses[1].stdp,"Legacy schema unexpectedly enabled learning.");
    schema2.set("schema","3");rejects([&]{deserialize(schema2.as_string());},"Schema 3 accepted missing STDP fields.");
    auto malformed=json::jobject::parse(serialize(c));auto modified=json::jobject::parse(malformed.get("synapses"));
    auto first=json::jobject::parse(modified.get(size_t(0)));first.set("stdp","1");
    json::jobject bad_synapses(true);bad_synapses+=json::kvp("",first.as_string());
    for(size_t i=1;i<modified.size();++i)bad_synapses+=json::kvp("",modified.get(i));
    malformed.set("synapses",bad_synapses.as_string());
    rejects([&]{deserialize(malformed.as_string());},"Schema 3 accepted non-boolean STDP switch.");
    auto unknown_rule=serialize(c);pos=unknown_rule.find("multiplicative");unknown_rule.replace(pos,14,"unknown");
    rejects([&]{deserialize(unknown_rule);},"Schema 3 accepted an unknown STDP rule.");
}
static void reference(bool feedback) {
    Config c=preset(feedback?0:1);c.sample=.001;
    Simulator s(c);require(s.error.empty(),"Reference config rejected.");
    double unused[na::VAR_COUNT],reference_rhs[na::ODE_COUNT],p[na::COUNT+1];na::defaults(unused,p);
    p[na::feedback]=feedback?1:0;double max_error=0;State actual;
    // Compare the original mathematical RHS on the SAME state and input.
    // The old CUDA time stepper intentionally retains its historical grid-end
    // jumps; it is no longer a numerical oracle for the corrected hybrid flow.
    while(!s.done()) {
        derivative(c,s.state,s.time,actual,&s.plasticity);
        na::rhs(s.state.data(),p,external_current(c,0,s.time),external_current(c,1,s.time),reference_rhs);
        for(int i=0;i<27;++i) {
            const double error=std::abs(reference_rhs[i]-actual[i])/(1+std::abs(reference_rhs[i]));
            max_error=std::max(max_error,error);
        }
        require(s.step(),s.error.c_str());
    }
    require(max_error<1e-12,"Generalized equations differ from the original model RHS.");
    require(s.spikes[0]>0 && s.spikes[1]>0 && s.releases[0]>0,"Default stimulated/astrocyte activity disappeared.");
    std::cout<<"Reference RHS feedback="<<feedback<<": max relative derivative error "<<max_error<<"\n";
}
static void selective_recording_tests() {
    for(int preset_index:{2,6}) {
        auto full_config=preset(preset_index);full_config.duration=.25;full_config.sample=.001;
        full_config.noise_sigma=.5;
        auto selected_config=full_config;selected_config.record_all=false;
        selected_config.record_neurons={0};selected_config.record_synapses={0};
        selected_config.neurons.back().inhibitory=true; // Display type must not alter currents.
        Simulator full(full_config),selected(selected_config);
        require(full.error.empty()&&selected.error.empty(),"Selective recording fixture rejected.");
        while(!full.done()) {
            require(full.step()&&selected.step(),"Selective recording integration failed.");
            require(full.state==selected.state,"Recording policy or neuron display type changed the dynamics.");
        }
        require(full.spikes==selected.spikes&&full.releases==selected.releases&&full.history.time==selected.history.time,
            "Selective recording lost global counts or changed sample times.");
        require(full.history.events.size()==selected.history.events.size(),"Unrecorded neurons lost spike events.");
        for(size_t i=0;i<full.history.events.size();++i) {
            const auto& a=full.history.events[i];const auto& b=selected.history.events[i];
            require(a.time==b.time&&a.index==b.index&&a.astro==b.astro,"Selective recording changed an event.");
        }
        for(int n=0;n<int(full_config.neurons.size());++n)for(int k=0;k<4;++k)
            require(n==0 ? selected.history.values[4*n+k]==full.history.values[4*n+k] : selected.history.values[4*n+k].empty(),
                "Neuron history selection was not applied.");
        for(int j=0;j<int(full_config.synapses.size());++j) {
            for(int k=0;k<4;++k)require(j==0 ?
                selected.history.values[syn_offset(full_config,j)+k]==full.history.values[syn_offset(full_config,j)+k] :
                selected.history.values[syn_offset(full_config,j)+k].empty(),"Synapse history selection was not applied.");
            require(j==0 ? selected.history.weights[j]==full.history.weights[j] : selected.history.weights[j].empty(),
                "Synapse weight history selection was not applied.");
            require(full.plasticity[j].weight==selected.plasticity[j].weight,"Unrecorded synapse stopped learning.");
        }
        for(int a=0;a<int(full_config.astrocytes.size());++a)for(int k=0;k<7;++k)
            require(selected.history.values[astro_offset(full_config,a)+k]==full.history.values[astro_offset(full_config,a)+k],
                "Astrocyte history must always be retained.");
        require(recorded_dimension(selected_config)==9+7*int(full_config.astrocytes.size()),"Stored column estimate excludes selected weights.");
    }
    auto c=preset(2);c.record_all=false;c.record_neurons={0,2,3};c.record_synapses={0,2,3};
    remove_neuron(c,1);
    require(c.record_neurons==std::vector<int>({0,1,2})&&c.record_synapses==std::vector<int>({0,1})&&validate(c).empty(),
        "Removing a neuron did not remap selected neurons and retained edges.");
    c.record_neurons={0,0};require(!validate(c).empty(),"Duplicate recording index accepted.");
    c.record_neurons={-1};require(!validate(c).empty(),"Negative recording index accepted.");
    c.record_neurons={3};require(!validate(c).empty(),"Missing recorded neuron accepted.");
    c.record_neurons={};c.record_synapses={2};require(!validate(c).empty(),"Missing recorded synapse accepted.");
    c=preset(4);c.record_all=false;c.record_neurons.clear();c.duration=.1;auto events_only=run(c);
    require(events_only.history.values[0].empty()&&!events_only.history.time.empty(),"Events-only recording allocated state samples.");

    auto legacy=json::jobject::parse(serialize(preset(6)));legacy.set("schema","3");
    for(const char* field:{"record_all","record_neurons","record_synapses"})legacy.remove(field);
    const auto neurons=json::jobject::parse(legacy.get("neurons"));json::jobject old_neurons(true);
    for(size_t i=0;i<neurons.size();++i) {
        auto neuron=json::jobject::parse(neurons.get(i));neuron.remove("inhibitory");old_neurons+=json::kvp("",neuron.as_string());
    }
    legacy.set("neurons",old_neurons.as_string());
    require(serialize(deserialize(legacy.as_string()))==serialize(preset(6)),"Schema 3 migration changed dynamics or default recording.");
    legacy.set("schema","4");rejects([&]{deserialize(legacy.as_string());},"Schema 4 accepted missing recording metadata.");
    auto invalid=json::jobject::parse(serialize(preset(4)));invalid.set("record_neurons","[0.5]");
    rejects([&]{deserialize(invalid.as_string());},"Noninteger recording index accepted.");
    invalid.set("record_neurons","[0,0]");rejects([&]{deserialize(invalid.as_string());},"Duplicate JSON recording indices accepted.");
    invalid.set("record_neurons","[]");invalid.set("record_all","1");
    rejects([&]{deserialize(invalid.as_string());},"Nonboolean record_all accepted.");
    invalid=json::jobject::parse(serialize(preset(4)));
    auto invalid_neuron=json::jobject::parse(json::jobject::parse(invalid.get("neurons")).get(size_t(0)));
    invalid_neuron.set("inhibitory","0");json::jobject bad_neurons(true);bad_neurons+=json::kvp("",invalid_neuron.as_string());
    invalid.set("neurons",bad_neurons.as_string());rejects([&]{deserialize(invalid.as_string());},"Nonboolean neuron type accepted.");
}
static void large_io_tests(const std::filesystem::path& tmp) {
    auto c=izh_config();c.duration=.001;c.sample=.0005;c.record_all=false;
    const auto neuron=c.neurons[0];c.neurons.assign(500,neuron);
    for(int i=0;i<500;++i){c.neurons[i].name="N"+std::to_string(i+1);c.neurons[i].inhibitory=i>=400;}
    c.neurons[0].voltage=29;c.neurons[0].equilibrium_u=false;c.neurons[0].izh_u=-13;
    for(int source=0;source<500;++source)for(int k=1;k<=30;++k) {
        Synapse syn;syn.source=source;syn.target=(source+k)%500;syn.model=SynapseModel::TsodyksMarkram;
        syn.weight=source>=400 ? -.5 : .5;syn.gain=.1;c.synapses.push_back(syn);
    }
    c.record_neurons={0,499};c.record_synapses={0,14999};
    require(validate(c).empty()&&recorded_dimension(c)==18,"500-neuron selective fixture rejected.");
    require(dimension(c)==62000,"Dynamic large-network state dimension is incorrect.");
    auto excessive=c;excessive.record_all=true;excessive.duration=1;excessive.sample=.001;
    require(!validate(excessive).empty(),"Full-history memory guard ignored the large network.");
    excessive.record_all=false;require(validate(excessive).empty(),"Selective recording did not remove the full-history memory limitation.");
    const auto encoded=serialize(c);require(encoded.size()>1024*1024,"Large JSON fixture does not exercise the old 1-MiB boundary.");
    save_config(c,tmp/"large_network.json");const auto loaded=load_config(tmp/"large_network.json");
    require(serialize(loaded)==encoded,"500-neuron/15000-synapse file round trip lost data.");
    auto s=run(loaded);require(s.state.size()==62000&&s.spikes.size()==500&&s.spikes[0]==1,"Full 500-neuron state was not integrated.");
    require(s.history.values.size()==62000&&s.history.weights.size()==15000,"Sparse history changed logical column indexing.");
    size_t stored=0;for(const auto& column:s.history.values)stored+=column.size();for(const auto& column:s.history.weights)stored+=column.size();
    require(stored==18*s.history.time.size(),"Unselected large-network traces were stored.");
    export_results(s,tmp/"large");
    std::ifstream trace(tmp/"large_trace.csv"),weights(tmp/"large_weights.csv");std::string line;
    std::getline(trace,line);
    require(line=="time_s,N1_V_mV,N1_u,N1_reserved_1,N1_reserved_2,N500_V_mV,N500_u,N500_reserved_1,N500_reserved_2,S1_u,S1_x,S1_y,S1_Y_uM,S15000_u,S15000_x,S15000_y,S15000_Y_uM",
        "Selective CSV header does not preserve original node/edge indices.");
    size_t rows=0;while(std::getline(trace,line)){require(std::count(line.begin(),line.end(),',')==16,"Selective state CSV has missing or extra fields.");++rows;}
    require(rows==s.history.time.size(),"Selective CSV sample count is incorrect.");
    std::getline(weights,line);require(line=="time_s,S1_weight,S15000_weight","Selective weight CSV exports unwanted edges.");
    rows=0;while(std::getline(weights,line)){require(std::count(line.begin(),line.end(),',')==2,"Selective weight CSV has wrong width.");++rows;}
    require(rows==s.history.time.size(),"Selective weight CSV sample count is incorrect.");trace.close();weights.close();
    std::ifstream summary_stream(tmp/"large_summary.json");std::ostringstream content;content<<summary_stream.rdbuf();summary_stream.close();
    const auto summary=json::jobject::parse(content.str());
    require(summary.get("record_all")=="false"&&std::stod(summary.get("recorded_dimension"))==18,
        "Summary omits recording-policy metadata.");
    const auto indices=json::jobject::parse(summary.get("recorded_neurons_0based"));
    require(indices.size()==2&&indices.get(size_t(1))=="499","Summary recording indices changed.");
    std::filesystem::remove(tmp/"large_network.json");
    for(const char* suffix:{"_trace.csv","_weights.csv","_events.csv","_experiment.json","_summary.json"})
        std::filesystem::remove(tmp/(std::string("large")+suffix));
}
int main() {
    try {
        reference(true);reference(false);izhikevich_tests();synapse_tests();stdp_tests();triplet_tests();interval_stimulus_tests();noise_tests();schema_tests();selective_recording_tests();
        Config c=preset(0);
        for(int i=0;i<8;++i) {
            auto p=preset(i);const auto encoded=serialize(p);const auto decoded=deserialize(encoded);
            require(serialize(decoded)==encoded,"JSON round trip loses configuration data.");
        }
        c.name="Сеть / \"тест\"";c.neurons[0].name="Нейрон α";c.neurons[0].current=7.123456789123;
        require(serialize(deserialize(serialize(c)))==serialize(c),"UTF-8 or double precision is lost.");
        auto unicode=serialize(c);auto pos=unicode.find("Сеть");unicode.replace(pos,std::string("Сеть").size(),"\\u0421\\u0435\\u0442\\u044c");
        require(deserialize(unicode).name==c.name,"Escaped Unicode cannot be loaded.");
        rejects([&]{deserialize(std::string(100,'['));},"Excessive nesting accepted.");
        rejects([&]{deserialize("{\"x\":1,}");},"Trailing comma accepted.");
        auto nul=serialize(c);pos=nul.find("Сеть");nul.replace(pos,std::string("Сеть").size(),"\\u0000");
        rejects([&]{deserialize(nul);},"NUL in a decoded name accepted.");
        rejects([&]{deserialize(serialize(c)+" garbage");},"Trailing JSON data accepted.");
        rejects([&]{deserialize("{broken");},"Malformed JSON accepted.");
        auto serialized=serialize(c);auto location=serialized.find("\"schema\": 4");
        if(location==std::string::npos)location=serialized.find("\"schema\":4");
        require(location!=std::string::npos,"Schema was not serialized.");
        std::string bad=serialized;auto digit=bad.find('4',location);bad[digit]='9';
        rejects([&]{deserialize(bad);},"Unsupported schema accepted.");
        c=preset(0);c.synapses[0].source=100;require(!validate(c).empty(),"Dangling synapse accepted.");
        c=preset(0);c.parameters[na::K_SIC]=0;require(!validate(c).empty(),"Zero divisor accepted.");
        c=preset(0);c.sample=1e-6;require(!validate(c).empty(),"Excessive recording accepted.");
        c=preset(0);c.dt=.001;require(!validate(c).empty(),"Unsafe dt accepted.");
        c=preset(0);c.astrocytes[0].calcium=4;require(!validate(c).empty(),"Impossible calcium pool accepted.");
        c=preset(2);remove_neuron(c,1);require(c.neurons.size()==3&&c.synapses.size()==2&&validate(c).empty(),"Neuron removal did not reindex links.");
        remove_astrocyte(c,0);require(c.gaps.empty()&&validate(c).empty(),"Astrocyte removal did not clean links.");
        c=preset(3);remove_neuron(c,0);require(c.neurons.size()==1,"Removed the last neuron.");
        c=preset(2);c.duration=1;c.sample=.001;auto multi=run(c);require(multi.spikes[0]>0&&multi.spikes[3]>0,"Multi-node input not firing.");
        require(multi.history.time.front()==0&&multi.history.time.back()==c.duration,"Recorded time endpoints incorrect.");
        c=preset(3);c.duration=.1234567;c.sample=.001;auto quiet=run(c);
        require(quiet.time==c.duration&&quiet.spikes[0]==0,"Final step or resting neuron incorrect.");
        require(std::abs(quiet.state[0]+65)<.1,"Resting voltage drifted.");
        for(double voltage:{-40.,-55.}) {c.neurons[0].voltage=voltage;run(c);}
        c=preset(3);c.neurons.resize(MaxNeurons);c.astrocytes.resize(MaxAstrocytes);
        for(int i=0;i<MaxSynapses;++i)c.synapses.push_back({i%MaxNeurons,(i+1)%MaxNeurons,i%MaxAstrocytes});
        for(int i=0;i<MaxAstrocytes;++i)for(int j=i+1;j<MaxAstrocytes;++j)c.gaps.push_back({i,j});
        c.duration=.02;c.sample=.001;auto maximum=run(c);
        require(dimension(c)==MaxState&&maximum.history.values.size()==MaxState,"Maximum network state has wrong size.");
        c.synapses.push_back({0,1,0});require(!validate(c).empty(),"Synapse size limit was ignored.");
        c=preset(2);State y=initial_state(c),without{},with{};
        int a=astro_offset(c,0),b=astro_offset(c,1);y[a+1]=.1;y[b+1]=.4;y[a+2]=.1;y[b+2]=.3;
        c.gaps.clear();derivative(c,y,0,without);c.gaps={{0,1,.2,.1}};derivative(c,y,0,with);
        require(std::abs((with[a+1]-without[a+1])+(with[b+1]-without[b+1]))<1e-14,"IP3 diffusion is not conservative.");
        require(std::abs((with[a+2]-without[a+2])+(with[b+2]-without[b+2]))<1e-14,"Calcium diffusion is not conservative.");
        c=preset(0);y=initial_state(c);a=astro_offset(c,0);y[a+5]=1;
        derivative(c,y,0,without);c.synapses.push_back(c.synapses[0]);c.synapses.back().conductance=0;
        State extended=initial_state(c);std::copy(y.begin(),y.begin()+12,extended.begin());
        extended[astro_offset(c,0)+5]=1;derivative(c,extended,0,with);
        require(std::abs(with[8]-without[8])<1e-12,"SIC was counted twice for the same astrocyte-target pair.");
        c=preset(0);c.duration=6;c.sample=.001;auto coarse=run(c);c.dt/=2;auto fine=run(c);
        require(std::abs(coarse.state[astro_offset(coarse.config,0)+2]-fine.state[astro_offset(fine.config,0)+2])<.001,"Calcium does not converge under dt refinement.");
        for(size_t i=0;i<coarse.spikes.size();++i)require(std::abs(double(coarse.spikes[i])-double(fine.spikes[i]))<=1,"Spike counts do not converge.");
        // CTest runs in the writable build directory. Keeping this fixture there
        // also supports Windows sandboxes whose virtual TEMP denies atomic rename.
        std::filesystem::path tmp=std::filesystem::current_path()/("network-studio-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(tmp);
        large_io_tests(tmp);
        save_config(c,tmp/"network.json");require(serialize(load_config(tmp/"network.json"))==serialize(c),"File round trip failed.");
        save_config(c,tmp/"network.json");export_results(coarse,tmp/"result");
        auto loaded=load_config(tmp/"result_experiment.json");require(loaded.dt==coarse.config.dt,"Export used edited parameters instead of run parameters.");
        std::ifstream csv(tmp/"result_trace.csv");std::string line;size_t rows=0;while(std::getline(csv,line))++rows;
        csv.close();
        require(rows==coarse.history.time.size()+1,"CSV row count differs from recorded samples.");
        c=izh_config();c.duration=.002;c.neurons.push_back(Neuron{});c.neurons[1].amplitude=0;
        c.synapses={{0,1,-1}};c.synapses[0].model=SynapseModel::TsodyksMarkram;
        auto mixed=run(c);export_results(mixed,tmp/"mixed");
        std::ifstream mixed_csv(tmp/"mixed_trace.csv");std::getline(mixed_csv,line);
        require(line.find("N1_V_mV,N1_u,N1_reserved_1,N1_reserved_2,N2_V_mV,N2_m,N2_h,N2_n")!=std::string::npos,
            "CSV does not distinguish Izhikevich recovery from HH gates.");
        require(line.find("S1_u,S1_x,S1_y,S1_Y_uM")!=std::string::npos,"CSV does not label the TM active resource pool.");
        require(size_t(std::count(line.begin(),line.end(),','))==size_t(dimension(c)),"CSV header has a different number of state columns.");
        require(serialize(load_config(tmp/"mixed_experiment.json"))==serialize(c),"Exported experiment does not reproduce the run.");
        mixed_csv.close();
        const auto trained=run(stdp_config());export_results(trained,tmp/"plastic");
        std::ifstream weight_csv(tmp/"plastic_weights.csv");std::getline(weight_csv,line);
        require(line=="time_s,S1_weight","Weight CSV has an incorrect header.");
        rows=0;
        while(std::getline(weight_csv,line)) {
            std::istringstream values(line);std::string stamp,weight;
            require(bool(std::getline(values,stamp,','))&&bool(std::getline(values,weight,',')),"Weight CSV row is incomplete.");
            require(rows<trained.history.time.size()&&std::stod(stamp)==trained.history.time[rows]&&
                std::stod(weight)==trained.history.weights[0][rows],"Weight CSV changed recorded time or weight precision.");
            ++rows;
        }
        weight_csv.close();require(rows==trained.history.time.size(),"Weight CSV row count differs from simulation history.");
        const auto initial=load_config(tmp/"plastic_experiment.json");
        require(initial.synapses[0].weight==trained.config.synapses[0].weight&&initial.synapses[0].weight!=trained.plasticity[0].weight,
            "Exported experiment contains the learned weight instead of the reproducible initial weight.");
        std::ifstream summary_file(tmp/"plastic_summary.json");std::ostringstream summary_text;summary_text<<summary_file.rdbuf();summary_file.close();
        const auto summary=json::jobject::parse(summary_text.str());const auto final_weights=json::jobject::parse(summary.get("final_synapse_weights"));
        require(final_weights.is_array()&&final_weights.size()==1&&std::stod(final_weights.get(size_t(0)))==trained.plasticity[0].weight,
            "Summary omits or rounds the final learned weights.");
        c=izh_config();c.duration=.001;auto empty_weights=run(c);export_results(empty_weights,tmp/"no_synapses");
        std::ifstream empty_csv(tmp/"no_synapses_weights.csv");std::getline(empty_csv,line);
        require(line=="time_s"&&empty_weights.history.weights.empty(),"A network without synapses has spurious weight columns.");empty_csv.close();
        // Only remove the specific files created by this test, never a computed tree.
        std::filesystem::remove(tmp/"network.json");
        for(const char* prefix:{"result","mixed","plastic","no_synapses"})
            for(const char* suffix:{"_trace.csv","_weights.csv","_events.csv","_experiment.json","_summary.json"})
                std::filesystem::remove(tmp/(std::string(prefix)+suffix));
        std::filesystem::remove(tmp);
        std::cout<<"PASS: "<<checks<<" checks (including every reference integration step).\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
