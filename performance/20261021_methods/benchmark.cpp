#include "io.h"
#include "performance/profile.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace studio;
using Clock=std::chrono::steady_clock;
static double seconds(Clock::duration d){return std::chrono::duration<double>(d).count();}
static uint64_t spikes(const Simulator& s){return std::accumulate(s.spikes.begin(),s.spikes.end(),uint64_t(0));}
static uint64_t deliveries(const Simulator& s){return s.history.deliveries.size()+std::accumulate(s.history.dropped_deliveries.begin(),s.history.dropped_deliveries.end(),uint64_t(0));}
int main(int argc,char** argv){try {
    if(argc!=5)throw std::runtime_error("Usage: method_benchmark CONFIG NEW_OUTPUT THREADS STOP_SECONDS");
    const auto dir=std::filesystem::u8path(argv[2]);if(!std::filesystem::create_directory(dir))throw std::runtime_error("Output must be new");
    auto begin=Clock::now();Simulator s(load_config(std::filesystem::u8path(argv[1])));s.set_compute_threads(std::stoi(argv[3]));
    const double setup=seconds(Clock::now()-begin),stop=std::stod(argv[4]);
    if(stop<=0||stop>s.config.duration)throw std::runtime_error("Invalid stop");
    std::ofstream log(dir/"segments.csv");log<<std::setprecision(17)<<"from,to,solve_s,accepted,error_rejections,state_reductions,mean_step_s,spikes,deliveries,effective_threads\n";
    profile::reset();double total=0;
    // The first quiet/burst windows match previous measurements; then half-second windows.
    std::vector<double> stops{.05,.1};for(double t=.5;t<stop;t+=.5)stops.push_back(t);stops.push_back(stop);
    for(double end:stops) {
        if(end>stop||end<=s.time)continue;
        const double from=s.time;const auto prior=s.integration;auto ns=spikes(s),nd=deliveries(s);begin=Clock::now();
        while(s.time<end&&!s.done())if(!s.step())throw std::runtime_error(s.error);
        const double elapsed=seconds(Clock::now()-begin);total+=elapsed;
        if(s.time!=end)throw std::runtime_error("Stop must be on main-step grid");
        const auto accepted=s.integration.accepted-prior.accepted;
        log<<from<<','<<s.time<<','<<elapsed<<','<<accepted<<','<<s.integration.error_rejected-prior.error_rejected<<','<<s.integration.state_reductions-prior.state_reductions<<','<<(s.time-from)/double(accepted)<<','<<spikes(s)-ns<<','<<deliveries(s)-nd<<','<<s.effective_compute_threads()<<'\n';log.flush();
        std::cout<<integration_method_info(s.config.integration_method).argument<<" t="<<s.time<<" compute="<<elapsed<<" s\n"<<std::flush;
    }
    profile::write(dir/"profile.json");begin=Clock::now();
    export_results(s,dir/"result");const double exported=seconds(Clock::now()-begin);begin=Clock::now();
    save_checkpoint(s,dir/"final.nscp");const double saved=seconds(Clock::now()-begin);
    std::ofstream state(dir/"state.csv");state<<std::setprecision(17)<<"index,value\n";for(size_t k=0;k<s.state.size();++k)state<<k<<','<<s.state[k]<<'\n';
    std::ofstream traces(dir/"plasticity.csv");traces<<std::setprecision(17)<<"index,weight,pre,post,slow\n";for(size_t k=0;k<s.plasticity.size();++k){const auto& p=s.plasticity[k];traces<<k<<','<<p.weight<<','<<p.pre_trace<<','<<p.post_trace<<','<<p.post_slow_trace<<'\n';}
    std::ofstream report(dir/"timing.json");report<<std::setprecision(17)<<"{\"method\":\""<<integration_method_info(s.config.integration_method).argument<<"\",\"dt_s\":"<<s.config.dt
        <<",\"setup_s\":"<<setup<<",\"solve_s\":"<<total<<",\"export_s\":"<<exported<<",\"checkpoint_s\":"<<saved
        <<",\"model_s\":"<<s.time<<",\"accepted\":"<<s.integration.accepted<<",\"error_rejections\":"<<s.integration.error_rejected<<",\"state_reductions\":"<<s.integration.state_reductions
        <<",\"mean_step_s\":"<<s.integration.mean_step(s.time)<<",\"spikes\":"<<spikes(s)<<",\"deliveries\":"<<deliveries(s)<<",\"requested_threads\":"<<s.compute_threads()<<",\"effective_threads\":"<<s.effective_compute_threads()<<"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
