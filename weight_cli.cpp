#include "weight_analysis.h"
#include "io.h"
#include <iostream>
#include <map>
#include <cmath>
namespace studio {
bool weights_cli(int argc,char** argv,int& exit_code){
    if(argc<2||std::string(argv[1]).find("--weights-")!=0)return false;
    std::string mode=argv[1];std::map<std::string,std::string> options;std::vector<std::filesystem::path> paths;int start=2;
    if(mode!="--weights-analyze"){if(argc<3)throw std::runtime_error("Missing weights input");paths.push_back(std::filesystem::u8path(argv[2]));start=3;}
    for(int i=start;i<argc;i+=2){if(i+1>=argc)throw std::runtime_error("Missing weights option value");if(std::string(argv[i])=="--snapshot")paths.push_back(std::filesystem::u8path(argv[i+1]));else if(!options.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("Duplicate weights option");}
    for(const auto& p:options)if(p.first!="--output"&&p.first!="--polarity"&&p.first!="--model"&&p.first!="--enabled"&&p.first!="--representation"&&p.first!="--bins"&&p.first!="--tolerance"&&p.first!="--reference"&&p.first!="--target")throw std::runtime_error("Unknown weights option: "+p.first);
    if(!options.count("--output"))throw std::runtime_error("Required: --output");auto output=std::filesystem::u8path(options.at("--output"));
    if(mode!="--weights-analyze"){
        if(paths.size()!=1||options.size()!=1)throw std::runtime_error("Snapshot import accepts one input and --output only");
        if(std::filesystem::weakly_canonical(paths[0])==std::filesystem::weakly_canonical(output))throw std::runtime_error("Choose a separate snapshot output file");
        WeightSnapshot s;
        if(mode=="--weights-initial")s=initial_weights(load_config(paths[0]));
        else if(mode=="--weights-from-summary")s=load_weight_summary(paths[0]);
        else if(mode=="--weights-from-checkpoint"){auto sim=load_checkpoint(paths[0]);if(!sim->error.empty())throw std::runtime_error("Checkpoint contains a calculation error");s=capture_weights(sim->config,sim->plasticity,sim->time,sim->steps,paths[0].stem().u8string());s.origin="Validated checkpoint: "+paths[0].u8string();}
        else throw std::runtime_error("Unknown weights operation");save_weights(s,output);std::cout<<"Saved "<<s.weights.size()<<" weights at t="<<s.time<<" s; no simulation steps.\n";exit_code=0;return true;
    }
    WeightSettings settings;auto choice=[&](const char* key,std::initializer_list<const char*> values){if(!options.count(key))return 0;int i=0;for(auto v:values){if(options.at(key)==v)return i;++i;}throw std::runtime_error(std::string("Invalid ")+key);};
    settings.polarity=choice("--polarity",{"all","E","I"});settings.model=choice("--model",{"all","stp","exp","tm"});settings.enabled=choice("--enabled",{"all","on","off"});settings.absolute=choice("--representation",{"signed","absolute"})!=0;
    auto numeric=[&](const char* key,double fallback){if(!options.count(key))return fallback;size_t n=0;double v=std::stod(options.at(key),&n);if(n!=options.at(key).size()||!std::isfinite(v))throw std::runtime_error("Invalid numeric weights option");return v;};
    auto index=[&](const char* key,size_t fallback,size_t max){double v=numeric(key,double(fallback));if(v<0||v>max||v!=std::floor(v))throw std::runtime_error("Invalid weights index/bin count");return size_t(v);};
    settings.bins=int(index("--bins",40,512));settings.boundary_tolerance=numeric("--tolerance",1e-6);size_t reference=index("--reference",0,16),target=index("--target",paths.size()>1?1:0,16);
    std::vector<WeightSnapshot> snapshots;if(paths.empty()||paths.size()>17)throw std::runtime_error("Select 1..17 --snapshot files");for(const auto& p:paths)snapshots.push_back(load_weights(p));std::vector<const WeightSnapshot*> views;for(const auto& s:snapshots)views.push_back(&s);
    export_weights(views,settings,reference,target,output);auto result=analyze_weights(views,settings,reference,target);std::cout<<"Weight snapshots analyzed; no simulation steps.\n";exit_code=0;if(paths.size()>1&&!result.difference.error.empty()){std::cerr<<result.difference.error<<"\n";exit_code=2;}return true;
}
}
