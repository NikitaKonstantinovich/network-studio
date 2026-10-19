#include "io.h"
#include "performance/profile.h"
#include <iostream>
#include <numeric>
using namespace studio;
// No struct padding, build metadata, paths, wall clocks or decimal rounding.
// Compare the bytes, NOT checkpoint envelopes (which retain strict compatibility).
class NumericalDump {
    std::ofstream f;
public:
    explicit NumericalDump(const std::filesystem::path& p):f(p,std::ios::binary){f.exceptions(std::ios::badbit|std::ios::failbit);}
    void value(double x){f.write(reinterpret_cast<const char*>(&x),sizeof(x));}
    void size(uint64_t x){f.write(reinterpret_cast<const char*>(&x),sizeof(x));}
    void vector(const std::vector<double>& v){size(v.size());for(auto x:v)value(x);}
    void delivery(const Delivery& e){value(e.t_emit);value(e.t_arrival);size(e.synapse);}
    void dump(const Simulator& s){
        size(1);value(s.time);size(s.steps);vector(s.state);
        size(s.plasticity.size());for(auto& p:s.plasticity){value(p.weight);value(p.pre_trace);value(p.post_trace);value(p.post_slow_trace);}
        size(s.spikes.size());for(auto x:s.spikes)size(x);size(s.releases.size());for(auto x:s.releases)size(x);
        vector(s.history.time);size(s.history.values.size());for(auto& v:s.history.values)vector(v);
        size(s.history.weights.size());for(auto& v:s.history.weights)vector(v);
        size(s.history.events.size());for(auto& e:s.history.events){value(e.time);size(e.index);size(e.astro);}
        size(s.history.deliveries.size());for(auto& e:s.history.deliveries)delivery(e);
        size(s.pending_deliveries.size());for(auto& e:s.pending_deliveries)delivery(e);
    }
};
int main(int argc,char** argv){try{
    if(argc<3||argc>4)throw std::runtime_error("Usage: network_studio_benchmark CONFIG OUTPUT_DIRECTORY [THREADS=1; 0=Auto]");
    const auto output=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(output);
    const auto start=std::chrono::steady_clock::now();auto c=load_config(std::filesystem::u8path(argv[1]));Simulator s(c);
    if(!s.error.empty())throw std::runtime_error(s.error);
    if(argc>3){size_t used=0;int threads=std::stoi(argv[3],&used);if(argv[3][used])throw std::runtime_error("Invalid thread count");s.set_compute_threads(threads);}
    const auto begin=std::chrono::steady_clock::now();profile::reset();while(!s.done())if(!s.step())break;
    const auto end=std::chrono::steady_clock::now();if(!s.error.empty())throw std::runtime_error(s.error);
    profile::write(output/"profile.json");NumericalDump(output/"numbers.bin").dump(s);
    export_results(s,output/"result");save_checkpoint(s,output/"final.nscp");
    std::ofstream f(output/"timing.json");f<<std::setprecision(17)<<"{\"setup_seconds\":"<<std::chrono::duration<double>(begin-start).count()
        <<",\"solve_seconds\":"<<std::chrono::duration<double>(end-begin).count()<<",\"export_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-end).count()
        <<",\"requested_threads\":"<<s.compute_threads()<<",\"effective_threads\":"<<s.effective_compute_threads()
        <<",\"model_seconds\":"<<s.time<<",\"steps\":"<<s.steps<<",\"events\":"<<s.history.events.size()<<",\"deliveries\":"<<s.history.deliveries.size()<<"}\n";
    std::cout<<std::chrono::duration<double>(end-begin).count()<<" seconds, "<<s.history.events.size()<<" spikes\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
