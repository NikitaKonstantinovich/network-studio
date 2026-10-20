#include "analysis.h"
#include "io.h"
#include "history_archive.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <map>
namespace studio {
bool analysis_cli(int argc,char** argv,int& exit_code) {
    if(argc<2)return false;std::string mode=argv[1];
    if(mode!="--analyze-export"&&mode!="--compare-analysis"&&mode!="--analyze-archive"&&mode!="--export-archive"&&mode!="--inspect-archive")return false;
    if(mode=="--inspect-archive") {
        if(argc!=3)throw std::runtime_error("Usage: --inspect-archive directory");auto path=std::filesystem::u8path(argv[2]);auto info=inspect_archive(path);auto config=load_config(path/"config.json");
        std::cout<<std::setprecision(17)<<"network_studio_history schema=1\nID="<<info.id<<"\nparent="<<info.parent<<"\nstatus="<<info.status<<"\ncommitted_range="<<info.from<<","<<info.until<<"\nblocks="<<info.blocks<<"\nrecords="<<info.records<<"\nuncommitted_tail="<<info.orphan_tail<<"\ndeliveries="<<info.deliveries<<"\ndt="<<config.dt<<"\nsample="<<config.sample<<"\ninitial_duration="<<config.duration<<"\n";exit_code=0;return true;
    }
    if(mode=="--compare-analysis") {
        if(argc!=4)throw std::runtime_error("Usage: --compare-analysis first_analysis.json second_analysis.json");
        auto reason=compare_analysis_files(std::filesystem::u8path(argv[2]),std::filesystem::u8path(argv[3]));
        std::cout<<(reason.empty()?"Comparable: identical settings and interval; full coverage and sufficient prehistory.":reason)<<"\n";exit_code=reason.empty()?0:2;return true;
    }
    std::map<std::string,std::string> options;
    for(int i=1;i<argc;i+=2){if(i+1==argc||!options.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("Missing/duplicate analysis option");}
    for(auto& x:options)if(x.first!="--analyze-export"&&x.first!="--analyze-archive"&&x.first!="--export-archive"&&x.first!="--analysis-profile"&&x.first!="--analysis-from"&&x.first!="--analysis-to"&&x.first!="--output")throw std::runtime_error("Unknown analysis option: "+x.first);
    for(auto key:{"--analysis-from","--analysis-to","--output"})if(!options.count(key))throw std::runtime_error(std::string("Required: ")+key);
    auto path=[&](const char* key){return std::filesystem::u8path(options.at(key));};
    if(mode=="--export-archive") {
        auto real=[&](const char* key){size_t n=0;double v=std::stod(options.at(key),&n);if(n!=options.at(key).size())throw std::runtime_error("Invalid export time");return v;};
        export_archive(path(mode.c_str()),real("--analysis-from"),real("--analysis-to"),path("--output"));exit_code=0;return true;
    }
    if(!options.count("--analysis-profile"))throw std::runtime_error("Required: --analysis-profile");
    auto profile_path=path("--analysis-profile");if(std::filesystem::file_size(profile_path)>65536)throw std::runtime_error("Profile exceeds 64 KiB");
    std::ifstream file(profile_path,std::ios::binary);std::string text{std::istreambuf_iterator<char>(file),{}};auto profile=analysis_profile_from_json(text);
    auto real=[&](const char* key){size_t n;double v=std::stod(options.at(key),&n);if(n!=options.at(key).size())throw std::runtime_error("Invalid analysis time");return v;};
    AnalysisInterval interval{real("--analysis-from"),real("--analysis-to"),false};
    // Do not allow an offline analysis to replace its own input export.
    if(std::filesystem::absolute(path("--output")).lexically_normal()==std::filesystem::absolute(path(mode.c_str())).lexically_normal())throw std::runtime_error("Choose a separate output prefix");
    if(mode=="--analyze-archive"){auto info=inspect_archive(path(mode.c_str()));std::cout<<"Archive status: "<<info.status<<"; uncommitted tail ignored: "<<info.orphan_tail<<"; ID "<<info.id<<"\n";}
    auto begin=std::chrono::steady_clock::now();auto result=mode=="--analyze-archive"?analyze_archive(path(mode.c_str()),profile.settings,interval):analyze_export(path(mode.c_str()),profile.settings,interval);
    export_analysis(result,profile.settings,path("--output"),profile.name);
    std::cout<<std::setprecision(17)<<"Requested ["<<result.requested_from<<","<<result.requested_until<<"); effective ["<<result.from<<","<<result.until<<") s\n"
        <<"Full interval: "<<result.full_interval<<"; prehistory: "<<result.prehistory_complete<<"; complete bursts: "<<result.response_hz*(result.until-result.from)<<"; matched: "<<result.responses.size()-result.missed<<" / "<<result.responses.size()<<"\n"
        <<"Postprocessing wall seconds: "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<" (no simulation steps)\n";
    exit_code=0;return true;
}
}
