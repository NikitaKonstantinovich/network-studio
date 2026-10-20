#include "io.h"
#include "analysis.h"
#include "history_archive.h"
#include "archive_codec.h"
#include "checkpoint_build.h"
#include "vendor/json/json.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <sstream>
#include <algorithm>
#include <thread>

namespace studio {
namespace {
std::string quote(const std::string& s) { return json::parsing::encode_string(s.c_str()); }
double number(const std::string& s) {
    size_t n=0;double x=std::stod(s,&n);
    if(n!=s.size()||!std::isfinite(x))throw std::runtime_error("Invalid package time");return x;
}
void fresh(const std::filesystem::path& p) {
    if(std::filesystem::exists(p))throw std::runtime_error("Package output already exists: "+p.u8string());
}
}
// CLI-only inspection/export. It does not modify the solver or advance a step.
bool package_cli(int argc,char** argv,int& code) {
    if(argc<2)return false;const std::string mode=argv[1];
    if(mode!="--package-build-info"&&mode!="--package-probe"&&mode!="--package-records")return false;
    if(mode=="--package-build-info") {
        if(argc!=2)throw std::runtime_error("--package-build-info takes no arguments");
        std::cout<<"{\"document_type\":\"network_studio_package_build\",\"schema_version\":1,\"checkpoint_build\":"<<quote(STUDIO_CHECKPOINT_BUILD_ID)
            <<",\"numerical_algorithm\":"<<quote(CheckpointAlgorithm)<<",\"hardware_concurrency\":"<<std::thread::hardware_concurrency()
            <<",\"pointer_bits\":"<<sizeof(void*)*8<<",\"thread_modes\":"<<quote(compute::choices())<<"}\n";
        code=0;return true;
    }
    std::map<std::string,std::string> o;
    for(int i=2;i<argc;i+=2)if(i+1>=argc||!o.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("Missing/duplicate package option");
    for(auto& x:o)if(x.first!="--config"&&x.first!="--resume"&&x.first!="--until"&&x.first!="--threads"&&x.first!="--commands"&&x.first!="--profile"&&x.first!="--output"&&x.first!="--canonical-state"&&x.first!="--archive")throw std::runtime_error("Unknown package option: "+x.first);
    auto path=[&](const char* k){return std::filesystem::u8path(o.at(k));};
    if(mode=="--package-records") {
        if(o.size()!=2||!o.count("--archive")||!o.count("--output"))throw std::runtime_error("Usage: --package-records --archive directory --output NEW.jsonl");
        const auto dir=path("--archive"),dest=path("--output");fresh(dest);auto tmp=dest;tmp+=".tmp";fresh(tmp);
        const auto info=inspect_archive(dir);if(info.orphan_tail||info.status=="failed")throw std::runtime_error("Cannot compare incomplete/failed archive");
        std::ofstream f(tmp,std::ios::binary);f.imbue(std::locale::classic());f<<std::setprecision(17);
        if(!f)throw std::runtime_error("Cannot create package records");
        scan_archive(dir,[&](ArchiveRecord type,const std::string& b){
            using archive_codec::Reader;Reader r{b};const double t=r.d();
            if(type==ArchiveRecord::Event){auto index=r.u(),astro=r.u();r.end();f<<"["<<quote(astro?"release":"spike")<<","<<t<<","<<index<<"]\n";}
            else if(type==ArchiveRecord::Stimulus){auto amp=r.d();auto neuron=r.u(),protocol=r.u();r.end();f<<"[\"stimulus\","<<t<<","<<amp<<","<<neuron<<","<<protocol<<"]\n";}
            else if(type==ArchiveRecord::Pulse){auto source=r.u(),pulse=r.u();auto start=r.d(),end=r.d(),amp=r.d();auto complete=r.u(),stage=r.u(),n=r.u();
                if(n>MaxNeurons)throw std::runtime_error("Invalid archive pulse recipients");std::vector<uint64_t> recipients;for(uint64_t i=0;i<n;++i)recipients.push_back(r.u());auto context=r.u();r.end();
                if(!context){f<<"[\"pulse\","<<t<<","<<source<<","<<pulse<<","<<start<<","<<end<<","<<amp<<","<<complete<<","<<stage<<",[";for(size_t i=0;i<n;++i){if(i)f<<",";f<<recipients[i];}f<<"]]\n";}}
            if(!f)throw std::runtime_error("Cannot write package records");
        },&info);f.close();if(!f)throw std::runtime_error("Cannot flush package records");std::filesystem::rename(tmp,dest);code=0;return true;
    }
    if(o.count("--config")+o.count("--resume")!=1||o.count("--archive"))throw std::runtime_error("Package probe requires --config or --resume");
    auto s=o.count("--resume")?load_checkpoint(path("--resume")):std::make_unique<Simulator>(load_config(path("--config")));
    if(!s->error.empty())throw std::runtime_error("Checkpoint is not resumable: "+s->error);
    if(o.count("--threads"))s->set_compute_threads(compute::parse(std::string_view(o.at("--threads")),"--threads"));
    if(o.count("--until"))s->extend_duration(number(o.at("--until")));
    if(o.count("--canonical-state")) {
        if(!o.count("--resume")||o.count("--commands")||o.count("--until"))throw std::runtime_error("Canonical comparison must use an unmodified loaded checkpoint");
        fresh(path("--canonical-state"));
        // Strip only storage provenance, after the normal strict NSCP loader.
        // Original checkpoints/archives are never changed. Numerical and event
        // history (including private integrator state) remains in NSCP verbatim.
        auto anchor=std::move(s->archive_anchor);s->archive_anchor.clear();save_checkpoint(*s,path("--canonical-state"));s->archive_anchor=std::move(anchor);
    }
    if(o.count("--commands"))load_stimulator_replay(*s,path("--commands"));
    if(o.count("--profile")) {
        const auto p=analysis_profile_from_json(archive_codec::read(path("--profile"),65536));auto c=s->config;c.analysis=p.settings;
        // A reference source may first be introduced by a future command.
        for(auto& command:s->stimulus_commands){auto it=std::find_if(c.stimulators.begin(),c.stimulators.end(),[&](auto& x){return x.id==command.value.id;});if(it==c.stimulators.end())c.stimulators.push_back(command.value);else *it=command.value;}
        const auto error=validate_experiment(c);if(!error.empty())throw std::runtime_error(error);
        if(p.settings.reference_stimulator&&std::none_of(c.stimulators.begin(),c.stimulators.end(),[&](auto& x){return x.id==p.settings.reference_stimulator;}))throw std::runtime_error("Analysis reference stimulator is absent from the protocol");
        if(p.settings.population==AnalysisPopulation::Selected&&p.settings.neurons.empty())throw std::runtime_error("Analysis population is empty");
    }
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    out<<"{\"document_type\":\"network_studio_package_probe\",\"schema_version\":1,\"time_s\":"<<s->time<<",\"steps\":"<<quote(std::to_string(s->steps))
       <<",\"threads_requested\":"<<s->compute_threads()<<",\"threads_effective\":"<<s->effective_compute_threads()
       <<",\"archive_linked\":"<<(!s->archive_anchor.empty()?"true":"false")<<",\"stage_active\":"<<(s->experiment.stage_active?"true":"false")
       <<",\"events_after_s\":"<<s->history.events_after<<",\"stimuli_after_s\":"<<s->history.stimuli_after<<",\"shared_after_s\":"<<s->history.shared_after
       <<",\"config\":"<<serialize(s->config)<<",\"runtime\":"<<serialize_runtime(*s)<<",\"stimulator_runtime\":"<<serialize_stimulus_runtime(*s)<<"}\n";
    if(o.count("--output")){fresh(path("--output"));write_atomic(path("--output"),out.str());}else std::cout<<out.str();code=0;return true;
}
}
