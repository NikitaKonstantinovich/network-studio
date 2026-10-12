#include "io.h"
#include <iostream>
#include <iomanip>
#include <optional>
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
        bool config_options=false;
        std::optional<double> until,stop,run_for;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") {
                std::cout<<"network_studio_cli [--config file.json | --preset 0..7 | --resume file.nscp]\n"
                    "  [--duration seconds] [--dt seconds] [--sample seconds] [--save file.json]\n"
                    "  [--until absolute_end] [--stop-at absolute_time | --run-for seconds]\n"
                    "  [--checkpoint file.nscp] [--output prefix]\n"
                    "All times are seconds. --save writes only the initial configuration.\n"
                    "--resume restores full state; config/dt/sample/duration/--save overrides are forbidden.\n"
                    "--until extends a restored final time (absolute from t=0).\n"
                    "--stop-at / --run-for stop at the first completed step at or after the target,\n"
                    "within the configured final time. --run-for is relative to restored/start time.\n"
                    "--checkpoint writes the full snapshot after stopping (also at completion).\n";return 0;
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
            else if(arg=="--resume")resume=std::filesystem::u8path(value);
            else if(arg=="--checkpoint")checkpoint=std::filesystem::u8path(value);
            else if(arg=="--save")save=std::filesystem::u8path(value);
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
        if(!s.error.empty())throw std::runtime_error("Snapshot/configuration is not resumable: "+s.error);
        if(!resume.empty() && until)s.extend_duration(*until);
        double target=s.config.duration;
        if(run_for) {
            if(!std::isfinite(*run_for)||*run_for<0)throw std::runtime_error("--run-for must be finite and nonnegative.");
            target=s.time+*run_for;
        } else if(stop)target=*stop;
        if(!std::isfinite(target)||target<s.time||target>s.config.duration)throw std::runtime_error("Stop target must lie between current and final absolute time; use --until to extend.");
        while(!s.done() && s.time<target)if(!s.step())break;
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
