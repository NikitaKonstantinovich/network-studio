#include "io.h"
#include <iostream>
#include <iomanip>
#include <optional>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
static double parse_number(const std::string& text) {
    size_t used=0;double value=std::stod(text,&used);
    if(used!=text.size())throw std::runtime_error("Invalid numeric argument: "+text);return value;
}
static int run_cli(int argc,char** argv) {
    try {
        studio::Config c=studio::preset(0);std::filesystem::path output="experiment",save,resume,checkpoint;
        bool config_options=false,continue_current=false;std::filesystem::path plan,trained,stimulus_replay;std::string branch_origin;
        std::optional<int> stage,sequence;std::optional<bool> learning;std::optional<double> learning_rate;
        std::optional<double> until,stop,run_for;
        int compute_threads=1;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") {
                std::cout<<"network_studio_cli [--config file.json | --preset 0..7 | --resume file.nscp]\n"
                    "  [--duration seconds] [--dt seconds] [--sample seconds] [--save file.json]\n"
                    "  [--until absolute_end] [--stop-at absolute_time | --run-for seconds]\n"
                    "  [--checkpoint file.nscp] [--output prefix]\n"
                    "  [--threads 1|2|4|8|auto] (runtime setting; default 1)\n"
                    "  [--stimulus-commands file.json] (recorded absolute model times)\n"
                    "  [--plan config_with_stages.json] [--stage zero_based | --sequence zero_based]\n"
                    "  [--learning enabled|frozen] [--learning-rate factor] [--fork provenance]\n"
                    "  [--export-trained new_initial_config.json] [--continue-current yes]\n"
                    "Stage durations are relative seconds rounded upward to a completed macro step.\n"
                    "Stage controls preserve state; --plan imports only stages and analysis settings.\n"
                    "All times are seconds. --save writes only the initial configuration.\n"
                    "--resume restores full state; config/dt/sample/duration/--save overrides are forbidden.\n"
                    "--until extends a restored final time (absolute from t=0).\n"
                    "--stop-at / --run-for stop at the first completed step at or after the target,\n"
                    "within the configured final time. --run-for is relative to restored/start time.\n"
                    "--checkpoint writes the full snapshot including pending deliveries (also at completion).\n"
                    "JSON synapses: delay in seconds; stdp_timing = arrival (default) or emission.\n"
                    "Exports: _events.csv contains emissions, _deliveries.csv actual arrivals,\n"
                    "_pending_deliveries.csv includes arrivals beyond the current final time.\n";return 0;
            }
            if(i+1>=argc)throw std::runtime_error("Missing option value.");std::string value=argv[++i];
            if(arg=="--config"){c=studio::load_config(std::filesystem::u8path(value));config_options=true;}
            else if(arg=="--preset") {double index=parse_number(value);if(!std::isfinite(index)||index<0||index>7||index!=std::floor(index))throw std::runtime_error("Preset must be 0..7.");c=studio::preset(int(index));config_options=true;}
            else if(arg=="--duration"){c.duration=parse_number(value);config_options=true;}
            else if(arg=="--dt"){c.dt=parse_number(value);config_options=true;}
            else if(arg=="--sample"){c.sample=parse_number(value);config_options=true;}
            else if(arg=="--until")until=parse_number(value);
            else if(arg=="--stop-at")stop=parse_number(value);
            else if(arg=="--run-for")run_for=parse_number(value);
            else if(arg=="--threads") {
                if(value=="auto")compute_threads=0;
                else if(value=="1"||value=="2"||value=="4"||value=="8")compute_threads=std::stoi(value);
                else throw std::runtime_error("--threads must be 1, 2, 4, 8 or auto.");
            }
            else if(arg=="--resume")resume=std::filesystem::u8path(value);
            else if(arg=="--checkpoint")checkpoint=std::filesystem::u8path(value);
            else if(arg=="--save")save=std::filesystem::u8path(value);
            else if(arg=="--continue-current"){if(value!="yes")throw std::runtime_error("Use --continue-current yes");continue_current=true;}
            else if(arg=="--stimulus-commands")stimulus_replay=std::filesystem::u8path(value);
            else if(arg=="--plan")plan=std::filesystem::u8path(value);
            else if(arg=="--stage"||arg=="--sequence") {double n=parse_number(value);if(n<0||n>255||n!=std::floor(n))throw std::runtime_error("Stage index must be 0..255");(arg=="--stage"?stage:sequence)=int(n);}
            else if(arg=="--learning"){if(value!="enabled"&&value!="frozen")throw std::runtime_error("Learning: enabled or frozen");learning=value=="enabled";}
            else if(arg=="--learning-rate")learning_rate=parse_number(value);
            else if(arg=="--fork")branch_origin=value;
            else if(arg=="--export-trained")trained=std::filesystem::u8path(value);
            else if(arg=="--output")output=std::filesystem::u8path(value);
            else throw std::runtime_error("Unknown option: "+arg);
        }
        if(!resume.empty() && (config_options || !save.empty()))throw std::runtime_error("A checkpoint cannot be combined with configuration overrides or --save; use --until to extend.");
        if(stop && run_for)throw std::runtime_error("Choose --stop-at or --run-for.");
        if(!save.empty()) {
            if(!checkpoint.empty() || stop || run_for || until)throw std::runtime_error("--save cannot be combined with execution/checkpoint options.");
            studio::save_config(c,save);std::cout<<"Configuration saved.\n";return 0;
        }
        if(resume.empty() && until)c.duration=*until;
        auto simulator=resume.empty()?std::make_unique<studio::Simulator>(c):studio::load_checkpoint(resume);
        auto& s=*simulator;
        s.set_compute_threads(compute_threads);
        if(!s.error.empty())throw std::runtime_error("Snapshot/configuration is not resumable: "+s.error);
        if(!resume.empty() && until)s.extend_duration(*until);
        if(continue_current)s.continue_current();
        if(stage&&sequence)throw std::runtime_error("Choose --stage or --sequence");
        if(!plan.empty()){auto p=studio::load_config(plan);auto candidate=s.config;candidate.stages=p.stages;candidate.analysis=p.analysis;auto err=studio::validate(candidate);if(!err.empty())throw std::runtime_error(err);s.config.stages=p.stages;s.config.analysis=p.analysis;}
        if(!branch_origin.empty())s.fork_experiment(branch_origin);
        if(stage||sequence)s.enter_stage(stage?*stage:*sequence,bool(sequence));
        if(learning||learning_rate)s.set_learning(learning.value_or(s.experiment.learning),learning_rate.value_or(s.experiment.learning_rate));
        if(!stimulus_replay.empty())studio::load_stimulator_replay(s,stimulus_replay);
        double target=s.config.duration;
        if(run_for) {
            if(!std::isfinite(*run_for)||*run_for<0)throw std::runtime_error("--run-for must be finite and nonnegative.");
            target=s.time+*run_for;
        } else if(stop)target=*stop;
        if(!std::isfinite(target)||target<s.time||target>s.config.duration)throw std::runtime_error("Stop target must lie between current and final absolute time; use --until to extend.");
        const auto begin=std::chrono::steady_clock::now();
        auto progress=begin;
        while(!s.done() && (!(stop||run_for)||s.time<target)) {
            if(!s.step())break;
            if(s.steps%256==0&&std::chrono::steady_clock::now()-progress>std::chrono::seconds(30)) {
                progress=std::chrono::steady_clock::now();std::cerr<<"Progress t="<<std::setprecision(10)<<s.time<<" s, steps="<<s.steps<<", events_after="<<s.history.events_after<<", deliveries_after="<<s.history.deliveries_after<<"\n";
            }
        }
        std::cout<<"Wall seconds: "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"\n";
        std::cout<<"Compute threads: "<<(compute_threads==0?"auto":std::to_string(compute_threads))<<" / effective "<<s.effective_compute_threads()<<"\n";
        if(!trained.empty())studio::save_config(s.learned_config(),trained);
        if(!checkpoint.empty())studio::save_checkpoint(s,checkpoint);
        if(!s.error.empty())throw std::runtime_error(s.error);
        studio::export_results(s,output);
        std::cout<<std::setprecision(17)<<(s.done()?"Completed ":"Paused ")<<s.time<<" s / "<<s.steps<<" steps / "<<s.history.time.size()<<" samples\n";
        for(size_t i=0;i<s.spikes.size();++i)std::cout<<"N"<<i+1<<": "<<s.spikes[i]<<" spikes\n";
        for(size_t i=0;i<s.releases.size();++i)std::cout<<"A"<<i+1<<": "<<s.releases[i]<<" releases\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
#ifdef _WIN32
static std::string unicode_argument(const wchar_t* value) {
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,nullptr,0,nullptr,nullptr);
    if(size<=0)throw std::runtime_error("Invalid Unicode command-line argument.");
    std::string result(size,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,result.data(),size,nullptr,nullptr))
        throw std::runtime_error("Cannot convert command-line argument to UTF-8.");
    result.pop_back();return result;
}
int wmain(int argc,wchar_t** argv) {
    try {
        std::vector<std::string> values;std::vector<char*> arguments;
        for(int i=0;i<argc;++i)values.push_back(unicode_argument(argv[i]));
        for(auto& value:values)arguments.push_back(value.data());
        return run_cli(argc,arguments.data());
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
#else
int main(int argc,char** argv) {return run_cli(argc,argv);}
#endif
