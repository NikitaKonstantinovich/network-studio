#include "io.h"
#include "analysis.h"
#include "history_archive.h"
#include <windows.h>
#include <psapi.h>
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

using Clock=std::chrono::steady_clock;
double sec(Clock::duration d){return std::chrono::duration<double>(d).count();}
int main(int argc,char** argv){try{
    if(argc<6||argc>7)throw std::runtime_error("Usage: network_studio_lab_benchmark CONFIG_OR_NSCP NEW_OUTPUT THREADS ABSOLUTE_START ABSOLUTE_STOP [archive|rolling]");
    const auto output=std::filesystem::u8path(argv[2]);
    if(!std::filesystem::create_directory(output))throw std::runtime_error("Output must be a new directory under an existing parent");
    const auto setup=Clock::now();const auto input=std::filesystem::u8path(argv[1]);
    const bool disk=argc==7&&std::string(argv[6])=="archive";
    const bool rolling=argc==7;
    if(rolling&&!disk&&std::string(argv[6])!="rolling")throw std::runtime_error("Expected archive or rolling");
    auto make=[&](){auto c=load_config(input);if(rolling)c.history_capacity=10000;return std::make_unique<Simulator>(c);};
    auto ptr=input.extension()==".nscp"?load_checkpoint(input):make();
    auto& s=*ptr;s.set_compute_threads(std::stoi(argv[3]));
    const double start=std::stod(argv[4]),stop=std::stod(argv[5]);
    if(!std::isfinite(start)||!std::isfinite(stop)||start<s.time||stop<=start||stop>s.config.duration)throw std::runtime_error("Invalid measurement interval");
    if(!s.error.empty())throw std::runtime_error(s.error);
    const auto loaded=Clock::now();
    if(disk)start_archive(s,output/"archive",true);
    while(s.time<start&&!s.done())if(!s.step())break;
    if(!s.error.empty()||s.time<start)throw std::runtime_error("Warmup failed: "+s.error);
    const double model_start=s.time;const auto spikes_start=s.history.events.size(),deliveries_start=s.history.deliveries.size();
    profile::reset();const auto begin=Clock::now();
    while(s.time<stop&&!s.done())if(!s.step())break;
    const auto end=Clock::now();if(!s.error.empty()||s.time<stop)throw std::runtime_error("Measurement failed: "+s.error);
    PROCESS_MEMORY_COUNTERS solve_memory{};GetProcessMemoryInfo(GetCurrentProcess(),&solve_memory,sizeof(solve_memory));
    profile::write(output/"profile.json");
    const auto flush_begin=Clock::now();flush_archive(s);const auto flushed=Clock::now();
    AnalysisInterval interval;interval.from=model_start;interval.to=s.time;interval.current_end=false;
    const auto analyzed=analyze_network(s.config,s.history,s.time,s.config.analysis,interval);const auto analysis_end=Clock::now();
    double disk_analysis_seconds=0;
    if(disk){const auto disk_begin=Clock::now();const auto result=analyze_archive(output/"archive",s.config.analysis,interval);disk_analysis_seconds=sec(Clock::now()-disk_begin);export_analysis(result,s.config.analysis,output/"disk_analysis","lab6 unchanged config");}
    export_analysis(analyzed,s.config.analysis,output/"analysis","lab6 unchanged config");
    NumericalDump(output/"numbers.bin").dump(s);
    const auto export_begin=Clock::now();export_results(s,output/"result");const auto exported=Clock::now();
    save_checkpoint(s,output/"final.nscp");const auto saved=Clock::now();
    PROCESS_MEMORY_COUNTERS memory{};GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
    std::ofstream f(output/"timing.json");f<<std::setprecision(17)
      <<"{\"setup_seconds\":"<<sec(loaded-setup)<<",\"warmup_seconds\":"<<sec(begin-loaded)
      <<",\"solve_seconds\":"<<sec(end-begin)<<",\"archive_flush_seconds\":"<<sec(flushed-flush_begin)
      <<",\"solve_peak_working_set_bytes\":"<<solve_memory.PeakWorkingSetSize<<",\"archive_analysis_seconds\":"<<disk_analysis_seconds
      <<",\"analysis_seconds\":"<<sec(analysis_end-flushed)<<",\"export_seconds\":"<<sec(exported-export_begin)
      <<",\"checkpoint_seconds\":"<<sec(saved-exported)<<",\"peak_working_set_bytes\":"<<memory.PeakWorkingSetSize
      <<",\"requested_threads\":"<<s.compute_threads()<<",\"effective_threads\":"<<s.effective_compute_threads()
      <<",\"model_start_seconds\":"<<model_start<<",\"model_seconds\":"<<s.time<<",\"steps\":"<<s.steps
      <<",\"events\":"<<s.history.events.size()<<",\"window_events\":"<<s.history.events.size()-spikes_start
      <<",\"deliveries\":"<<s.history.deliveries.size()<<",\"window_deliveries\":"<<s.history.deliveries.size()-deliveries_start<<"}\n";
    std::cout<<sec(end-begin)<<" seconds; "<<model_start<<".."<<s.time<<" model s; "<<s.history.events.size()-spikes_start<<" spikes\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
