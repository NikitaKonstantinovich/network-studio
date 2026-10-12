#include "io.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <algorithm>
using namespace studio;

Config base() {
    Config c=preset(3);c.neurons[0].amplitude=0;c.duration=3;c.sample=.001;
    c.astrocytes.push_back({});c.astrocytes[0].threshold=1.6;c.astrocytes[0].feedback=false;
    return c;
}
void probe(const std::filesystem::path& dir) {
    std::ofstream out(dir/"rhs_probes.csv");out<<std::setprecision(17);
    out<<"GammaA,IP3,Ca,h,xA,GA,GammaS,glutamate";
    for(const char* prefix:{"network","kernel"})for(int k=0;k<7;++k)out<<","<<prefix<<"_"<<k;
    out<<"\n";
    std::mt19937 rng(735193);
    auto u=[&](){return (double(rng())+.5)/4294967296.;};
    Config c=preset(0);State state=initial_state(c),dy;int b=astro_offset(c,0);
    for(int row=0;row<200;++row) {
        double a[7]={u(),2*u(),1.6*u(),u(),u(),5*u(),u()};const double g=20*u();
        if(row>=160){int k=(row-160)/2%7;bool upper=(row-160)%2;a[k]=upper?1.:0.;if(k==2&&upper)a[k]=c.parameters[na::C_T]/(1+c.parameters[na::rho_A]);}
        for(int k=0;k<7;++k)state[b+k]=a[k];
        state[syn_offset(c,0)+3]=g/c.synapses[0].spill;state[syn_offset(c,1)+3]=0;
        derivative(c,state,0,dy);
        double y[na::VAR_COUNT],p[na::COUNT+1],d[na::ODE_COUNT];na::defaults(y,p);
        for(int k=0;k<7;++k)y[na::GammaA+k]=a[k];y[na::Y1]=g/p[na::spill1];y[na::Y2]=0;
        na::rhs(y,p,0.,0.,d);
        for(double x:a)out<<x<<",";out<<g;
        for(int k=0;k<7;++k)out<<","<<dy[b+k];
        for(int k=0;k<7;++k)out<<","<<d[na::GammaA+k];out<<"\n";
    }
    Config gaps=base();gaps.astrocytes.push_back(gaps.astrocytes[0]);gaps.gaps={{0,1,.7,.4}};
    for(int p:{na::O_beta,na::O_delta,na::O_3K,na::Omega_5P,na::Omega_C,na::Omega_L,na::O_P})gaps.parameters[p]=0;
    State gy=initial_state(gaps),gd;int a=astro_offset(gaps,0),bb=astro_offset(gaps,1);
    gy[a+1]=.4;gy[bb+1]=1.2;gy[a+2]=.1;gy[bb+2]=.3;derivative(gaps,gy,0,gd);
    std::ofstream gap(dir/"gap_balance.csv");gap<<std::setprecision(17)<<"cell,Ca,dCa_dt,inferred_ER,dER_dt,local_total,dtotal_dt\n";
    for(int k:{a,bb})gap<<(k==a?0:1)<<","<<gy[k+2]<<","<<gd[k+2]<<","<<(gaps.parameters[na::C_T]-gy[k+2])/gaps.parameters[na::rho_A]<<","<<-gd[k+2]/gaps.parameters[na::rho_A]<<","<<gaps.parameters[na::C_T]<<",0\n";
    std::ofstream sic(dir/"feedback_probes.csv");sic<<std::setprecision(17)<<"case,post_dV_dt,SIC_current\n";
    auto feedback=preset(0);State fy=initial_state(feedback),fdy;
    fy[astro_offset(feedback,0)+5]=2;
    for(int mode=0;mode<3;++mode){
        feedback.astrocytes[0].feedback=mode!=0;
        if(mode==2){feedback.synapses.push_back(feedback.synapses[0]);feedback.synapses.back().conductance=0;State z=initial_state(feedback);z[astro_offset(feedback,0)+5]=2;fy=z;}
        derivative(feedback,fy,0,fdy);sic<<mode<<","<<fdy[8]<<","<<(mode?(.02*2/3*65):0)<<"\n";
    }
}
int main(int argc,char** argv) {
    try {
        if(argc<3)throw std::runtime_error("usage: harness probe output_dir | scenario output_prefix dt");
        std::filesystem::path output=argv[2];std::string name=argv[1];
        std::filesystem::create_directories(name=="probe"?output:output.parent_path());
        if(name=="probe"){probe(output);return 0;}
        Config c=base();c.dt=std::stod(argv[3]);c.name="Astrocyte audit: "+name;
        bool seed_receptor=false;
        if(name=="rest") {}
        else if(name=="transient") {c.astrocytes[0].ip3=.8;c.astrocytes[0].calcium=.1;}
        else if(name=="release") {c.astrocytes[0].ip3=.8;c.astrocytes[0].calcium=.1999;c.astrocytes[0].threshold=.2;}
        else if(name=="initial_above") {c.astrocytes[0].ip3=.8;c.astrocytes[0].calcium=.3;c.astrocytes[0].threshold=.2;c.duration=.001;c.sample=c.dt;}
        else if(name=="gap") {
            c.astrocytes.push_back(c.astrocytes[0]);c.gaps={{0,1,.7,.4}};c.duration=2;
            c.astrocytes[0].ip3=.4;c.astrocytes[1].ip3=1.2;c.astrocytes[0].calcium=.1;c.astrocytes[1].calcium=.3;
            for(int p:{na::O_beta,na::O_delta,na::O_3K,na::Omega_5P,na::Omega_C,na::Omega_L,na::O_P})c.parameters[p]=0;
        } else if(name=="receptors") {
            c.duration=2;seed_receptor=true;c.parameters[na::Omega_Y]=0;c.parameters[na::Omega_G]=0;
            c.neurons.push_back(c.neurons[0]);c.synapses.push_back({0,1,0});c.synapses[0].conductance=0;
        } else if(name=="active" || name=="active_off") {
            const double dt=c.dt;c=preset(name=="active"?0:1);c.dt=dt;c.duration=5;c.sample=.001;
        } else throw std::runtime_error("unknown scenario");
        Simulator s(c);if(!s.error.empty())throw std::runtime_error(s.error);
        if(seed_receptor) {
            // Explicit test-only initial-state override; captured in initial_state.csv.
            s.state[syn_offset(c,0)+3]=2/c.synapses[0].spill;
            int b=astro_offset(c,0);s.state[b]=.1;s.state[b+5]=3;s.state[b+6]=.2;
            s.history.time.clear();for(auto& v:s.history.values)v.clear();for(auto& v:s.history.weights)v.clear();s.record();
        }
        std::ofstream initial(output.string()+"_initial_state.csv");initial<<std::setprecision(17)<<"index,value\n";
        for(size_t i=0;i<s.state.size();++i)initial<<i<<","<<s.state[i]<<"\n";initial.close();
        while(!s.done())if(!s.step())break;
        export_results(s,output);
        std::cout<<name<<" dt="<<c.dt<<" time="<<s.time<<" error="<<s.error<<" releases=";
        for(auto value:s.releases)std::cout<<value<<",";std::cout<<"\n";
        return s.error.empty()?0:1;
    } catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
