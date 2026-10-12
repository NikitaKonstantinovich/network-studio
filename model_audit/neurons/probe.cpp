#include "network.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    std::ofstream out(argv[1]); out<<std::setprecision(17);
    if(!out)return 3;
    out<<"case,voltage,m,h,n,dV,dm,dh,dn,float_dV,float_dm,float_dh,float_dn,initial_m,initial_h,initial_n\n";
    auto c=studio::preset(3);c.neurons[0].amplitude=0;c.neurons[0].current=7.3;
    for(double v:{-200.,-100.,-65.,-55.000001,-55.00000001,-55.,-54.99999999,-54.999999,
                  -40.000001,-40.00000001,-40.,-39.99999999,-39.999999,0.,60.,200.}) {
        c.neurons[0].voltage=v;
        const auto init=studio::initial_state(c);
        studio::State state={v,.2,.4,.6},rhs;
        studio::derivative(c,state,0,rhs);
        float fp[neuro_astro::COUNT+1],fy[4]={float(v),.2f,.4f,.6f},fd[4];
        for(int i=0;i<=neuro_astro::COUNT;++i)fp[i]=float(c.parameters[i]);
        neuro_astro::hh_rhs(fy,fd,0,7.3f,fp);
        out<<"hh,"<<v<<",.2,.4,.6";
        for(double a:rhs)out<<','<<a;
        for(float a:fd)out<<','<<a;
        for(int i=1;i<4;++i)out<<','<<init[i];out<<'\n';
    }
    auto iz=studio::preset(4); iz.neurons[0].amplitude=0; iz.neurons[0].current=7.3;
    for(double v:{-100.,-70.,-65.,-55.,-40.,0.,29.9}) {
        studio::State state={v,-13,0,0},rhs;
        studio::derivative(iz,state,0,rhs);
        out<<"izh,"<<v<<",-13,0,0";for(double a:rhs)out<<','<<a;
        out<<",0,0,0,0,0,0,0\n";
    }
    out.close();
    // A diagnostic bridge to the template core, with feedback disconnected.
    // Existing production library is linked; no production objects are rebuilt.
    double y[neuro_astro::VAR_COUNT],next[neuro_astro::VAR_COUNT],p[neuro_astro::COUNT+1];
    neuro_astro::defaults(y,p);p[neuro_astro::amp1]=p[neuro_astro::amp2]=0;
    p[neuro_astro::g_syn1]=p[neuro_astro::g_syn2]=p[neuro_astro::g_SIC]=p[neuro_astro::feedback]=0;
    p[neuro_astro::I_pre1]=10;p[neuro_astro::I_pre2]=4;p[neuro_astro::I_post]=0;
    auto bridge=studio::preset(3);bridge.duration=.1;bridge.sample=.001;
    bridge.neurons.resize(3);for(auto& n:bridge.neurons)n.amplitude=0;
    bridge.neurons[0].current=10;bridge.neurons[1].current=4;bridge.neurons[2].current=0;
    studio::Simulator sim(bridge);
    std::copy(sim.state.begin(),sim.state.end(),y);
    double max_difference=0;
    while(!sim.done()) {
        double dt=std::min(double(sim.steps+1)*bridge.dt,bridge.duration)-sim.time;
        neuro_astro::step(y,next,p,dt);
        if(!sim.step()){std::cerr<<sim.error;return 4;}
        for(size_t i=0;i<sim.state.size();++i)max_difference=std::max(max_difference,std::abs(next[i]-sim.state[i]));
        std::copy(next,next+neuro_astro::VAR_COUNT,y);
    }
    std::cout<<std::setprecision(17)<<"{\"bridge_max_neuron_state_difference\":"<<max_difference
             <<",\"bridge_steps\":"<<sim.steps<<",\"bridge_spikes\":["<<sim.spikes[0]<<','<<sim.spikes[1]<<','<<sim.spikes[2]<<"]}\n";
}
