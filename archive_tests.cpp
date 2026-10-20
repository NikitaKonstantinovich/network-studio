#include "history_archive.h"
#include "archive_codec.h"
#include "io.h"
#include "simulation_runner.h"
#include <chrono>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
using namespace studio;
static size_t checks=0;
static void check(bool v,const std::string& message){++checks;if(!v)throw std::runtime_error(message);}
template<class F>void rejects(F f,const char* message){bool bad=false;try{f();}catch(...){bad=true;}check(bad,message);}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error);check(s.error.empty(),s.error);flush_archive(s);}
static Config fixture(){auto c=preset(6);c.duration=.3;c.sample=.0001;c.noise_sigma=.3;c.noise_interval=.000073;c.seed=734;
    for(auto& n:c.neurons){n.current=16;n.frequency=100;n.start=.00005;n.stop=.28;n.width=.00001;}
    c.synapses[0].delay=.000037;c.synapses[0].stdp_rule=StdpRule::Triplet;c.synapses[0].gain=2;
    Stimulator s;s.id=1;s.name="Archive shared source";s.recipients={0,1};s.signal.frequency=100;s.signal.amplitude=10;s.signal.width=.000087;s.signal.start=.00001;s.signal.stop=.29;c.stimulators={s};c.next_stimulator_id=2;
    c.analysis.min_neurons=1;c.analysis.start_rate=20;c.analysis.end_rate=0;c.analysis.min_gap=.001;c.analysis.burst_window=.003;c.analysis.response_max=.005;c.analysis.reference_stimulator=1;
    c.bounded_journals=true;c.journal_capacity=50000;c.history_capacity=32;return c;
}
static std::vector<std::string> records(const std::filesystem::path& path){std::vector<std::string> out;scan_archive(path,[&](ArchiveRecord t,const std::string& s){if(t!=ArchiveRecord::Protocol)out.push_back(std::to_string(int(t))+s);});return out;}
static void original_records(const Simulator& reference,const std::filesystem::path& path,bool deliveries){
    size_t row=0,event=0,delivery=0,stimulus=0;const auto& h=reference.history;
    scan_archive(path,[&](ArchiveRecord type,const std::string& bytes){archive_codec::Reader r{bytes};const double time=r.d();
        if(type==ArchiveRecord::Sample){check(row<h.time.size()&&time==h.time[row]&&r.u()==row,"archive sample clock/index differs from original RAM");
            for(const auto& column:h.values)if(!column.empty())check(r.d()==column[row],"archive state value differs from original RAM");
            for(const auto& column:h.weights)if(!column.empty())check(r.d()==column[row],"archive weight differs from original RAM");++row;r.end();
        }else if(type==ArchiveRecord::Event){check(event<h.events.size(),"extra archived event");const auto& e=h.events[event++];check(time==e.time&&r.u()==uint64_t(e.index)&&r.u()==uint64_t(e.astro),"archived event differs from RAM");r.end();
        }else if(type==ArchiveRecord::Delivery){check(delivery<h.deliveries.size(),"extra archived delivery");const auto& d=h.deliveries[delivery++];check(time==d.t_arrival&&r.d()==d.t_emit&&r.u()==uint64_t(d.synapse),"archived delivery differs from RAM");r.end();
        }else if(type==ArchiveRecord::Stimulus){check(stimulus<h.stimuli.size(),"extra archived stimulus");const auto& e=h.stimuli[stimulus++];check(time==e.time&&r.d()==e.amplitude&&r.u()==uint64_t(e.neuron)&&r.u()==uint64_t(e.protocol),"archived stimulus differs from RAM");r.end();}
    });check(row==h.time.size()&&event==h.events.size()&&stimulus==h.stimuli.size()&&delivery==(deliveries?h.deliveries.size():0),"archive lost original records");
}
static void exact(Simulator& a,Simulator& b,const std::filesystem::path& root){
    check(a.state==b.state&&a.spikes==b.spikes&&a.steps==b.steps&&a.time==b.time,"archive changed trajectory/counters");
    auto ca=a.config;auto ha=std::move(a.history);auto aa=a.archive_anchor;
    a.config=b.config;a.history=b.history;flush_archive(b);a.archive_anchor=b.archive?b.archive->anchor():b.archive_anchor;
    save_checkpoint(a,root/"reference.nscp");save_checkpoint(b,root/"recorded.nscp");
    check(archive_codec::read(root/"reference.nscp")==archive_codec::read(root/"recorded.nscp"),"exact solver checkpoint mismatch");
    a.config=ca;a.history=std::move(ha);a.archive_anchor=aa;
}
static void core(const std::filesystem::path& root){
    auto c=fixture(),full=c;full.history_capacity=0;full.bounded_journals=false;full.journal_capacity=MaxEvents;
    Simulator baseline(full),stream(c);auto dir=root/std::filesystem::u8path("полный_опыт");start_archive(stream,dir,true);finish(baseline);finish(stream);exact(baseline,stream,root);
    original_records(baseline,dir,true);
    check(stream.history.sample_begin>0&&stream.history.time.size()<=32,"state history did not roll");
    for(auto interval:{AnalysisInterval{0,.3,false},AnalysisInterval{.08,.27,false},AnalysisInterval{.2,.4,false}}){
        auto ram=analyze_network(baseline.config,baseline.history,baseline.time,c.analysis,interval);auto disk=analyze_archive(dir,c.analysis,interval);
        check(analysis_result_json(ram,{"same",c.analysis})==analysis_result_json(disk,{"same",c.analysis}),"RAM/disk analysis mismatch");check(ram.group_hz==disk.group_hz&&ram.burst_hz==disk.burst_hz,"RAM/disk curves differ");
    }
    auto individual=c.analysis;individual.reference_stimulator=0;check(analysis_result_json(analyze_archive(dir,individual,{0,.3,false}),{"same",individual})==analysis_result_json(analyze_network(full,baseline.history,.3,individual,{0,.3,false}),{"same",individual}),"individual stimulus analysis mismatch");
    export_archive(dir,0,.31,root/"export");check(std::filesystem::file_size(root/"export_states.csv")>1000,"state archive export empty");
    Simulator split(c);auto resume_dir=root/"resume";start_archive(split,resume_dir,true);while(split.time<.100025)check(split.step(),split.error);save_checkpoint(split,root/"pause.nscp");split.archive.reset();
    auto resumed=load_checkpoint(root/"pause.nscp");rejects([&]{resumed->step();},"silent unrecorded checkpoint continuation");start_archive(*resumed,resume_dir,true,true);finish(*resumed);exact(baseline,*resumed,root);check(records(dir)==records(resume_dir),"resume duplicated or lost records");resumed->archive.reset();
    auto stale=load_checkpoint(root/"pause.nscp");rejects([&]{start_archive(*stale,resume_dir,true,true);},"stale checkpoint appended");auto source_before=archive_codec::read(resume_dir/"manifest.nsh");start_archive(*stale,root/"branch",true);finish(*stale);exact(baseline,*stale,root);check(source_before==archive_codec::read(resume_dir/"manifest.nsh"),"branch changed parent");check(inspect_archive(root/"branch").from==.100025&&!inspect_archive(root/"branch").parent.empty(),"missing branch provenance");
    auto branch_analysis=analyze_archive(root/"branch",c.analysis,{0,.3,false});
    check(analysis_result_json(branch_analysis,{"same",c.analysis})==analysis_result_json(analyze_network(full,baseline.history,.3,c.analysis,{0,.3,false}),{"same",c.analysis}),"branch failed to read exact ancestor prefix");
    export_archive(root/"branch",0,.31,root/"branch_export");
    for(auto channel:{"states","events","deliveries","stimuli","shared_updates"})check(archive_codec::read(root/(std::string("export_")+channel+".csv"))==archive_codec::read(root/(std::string("branch_export_")+channel+".csv")),"branch export duplicated ancestor data");
    // An uncommitted partial final block is ignored, never promoted. The committed
    // prefix stays readable; append is refused rather than truncating source data.
    auto info=inspect_archive(resume_dir);std::ostringstream name;name<<std::setw(16)<<std::setfill('0')<<info.blocks+1<<".nshb";write_atomic(resume_dir/name.str(),"partial");
    check(inspect_archive(resume_dir).orphan_tail,"partial tail not reported");check(records(dir)==records(resume_dir),"uncommitted tail entered analysis");
    auto tail=load_checkpoint(root/"pause.nscp");rejects([&]{start_archive(*tail,resume_dir,true,true);},"partial tail appended");
    // Corruption inside a committed block is a hard error.
    auto corrupt=root/"corrupt";std::filesystem::create_directory(corrupt);for(const auto& entry:std::filesystem::directory_iterator(dir))if(entry.path().filename()!=L"writer.lock")std::filesystem::copy_file(entry.path(),corrupt/entry.path().filename());auto block=corrupt/"0000000000000001.nshb";auto b=archive_codec::read(block);b.resize(b.size()/2);write_atomic(block,b);rejects([&]{analyze_archive(corrupt,c.analysis,{0,.3,false});},"truncated committed block accepted");
    Simulator fail(c);auto faildir=root/"write_error";start_archive(fail,faildir);while(fail.time<.005)check(fail.step(),fail.error);
    auto last=inspect_archive(faildir);std::ostringstream next;next<<std::setw(16)<<std::setfill('0')<<last.blocks+1<<".nshb";std::filesystem::create_directory(faildir/next.str());rejects([&]{flush_archive(fail);},"write failure hidden");check(inspect_archive(faildir).until==last.until,"failure advanced committed range");rejects([&]{flush_archive(fail);},"failed writer silently recovered");
    // HH/astrocytes, selective state history, delayed resources and non-armed
    // threshold flags survive normal checkpoint validation in this same build.
    auto hh=preset(2);hh.duration=.04;hh.sample=.0001;hh.record_all=false;hh.record_neurons={0};hh.history_capacity=16;hh.bounded_journals=true;
    for(auto& n:hh.neurons)n.current=15;for(auto& a:hh.astrocytes){a.threshold=.100001;a.ip3=.8;}hh.parameters[na::O_P]=0;
    auto hh_full=hh;hh_full.history_capacity=0;Simulator hb(hh_full),hs(hh);start_archive(hs,root/"hh");finish(hb);finish(hs);exact(hb,hs,root);original_records(hb,root/"hh",false);save_checkpoint(hs,root/"hh.nscp");auto hr=load_checkpoint(root/"hh.nscp");check(hr->history.values==hs.history.values&&hr->releases==hs.releases,"HH/astro checkpoint mismatch");
    // Runner snapshots use absolute sample indices while RAM rows roll.
    SimulationRunner runner(c);RunnerCommand command;command.kind=RunnerCommand::Kind::StartArchive;command.path=root/"runner";command.index=32;
    check(runner.submit(command).get().ok,"runner archive attach failed");SimulationView view;while(auto s=runner.take_snapshot())check(view.apply(s),"archive initial snapshot");
    command={};command.kind=RunnerCommand::Kind::Advance;command.value=.04;check(runner.submit(command).get().ok,"runner advance failed");while(auto s=runner.take_snapshot())check(view.apply(s),"rolling snapshot cursor mismatch");
    check(view.sample_begin>0&&view.history.time.size()<=32&&view.metrics.disk_until==view.time,"runner ranges incorrect");runner.shutdown();
    // I/O-only synthetic journal, not a model integration: enough entries to
    // cross block/page boundaries and exercise a long uninterrupted detector.
    Simulator synthetic(c);auto synthetic_dir=root/"synthetic";start_archive(synthetic,synthetic_dir);History source;
    for(int k=0;k<100000;++k){Event event{double(k/2+1)*.000002,k%2,false};source.events.push_back(event);synthetic.archive->event(event);}
    synthetic.time=.12;synthetic.steps=4800;flush_archive(synthetic);
    for(auto interval:{AnalysisInterval{0,.12,false},AnalysisInterval{.04,.11,false}}){
        auto a=analyze_network(c,source,.12,c.analysis,interval),b=analyze_archive(synthetic_dir,c.analysis,interval);
        check(a.group_hz==b.group_hz&&a.burst_hz==b.burst_hz&&analysis_result_json(a,{"synthetic",c.analysis})==analysis_result_json(b,{"synthetic",c.analysis}),"paged detector changed long burst/prehistory");
    }
    // Applied and queued protocol changes are journalled without modifying a
    // single state value; live schedules retain their original timing semantics.
    Simulator protocol(c),protocol_reference(c);start_archive(protocol,root/"protocol");
    auto source_settings=c.stimulators[0];source_settings.signal.frequency=50;
    protocol.schedule_stimulator(source_settings,.1,true);protocol_reference.schedule_stimulator(source_settings,.1,true);
    protocol.set_learning(false,0);protocol_reference.set_learning(false,0);
    finish(protocol_reference);finish(protocol);exact(protocol_reference,protocol,root);
    size_t transitions=0;scan_archive(root/"protocol",[&](ArchiveRecord type,const std::string&){transitions+=type==ArchiveRecord::Protocol;});check(transitions>=4,"queued/applied protocol missing from archive");
#ifdef _WIN32
    Simulator io_fail(c);auto io_dir=root/"locked_manifest";start_archive(io_fail,io_dir);
    while(io_fail.time<.005)check(io_fail.step(),io_fail.error);
    HANDLE held=CreateFileW((io_dir/L"manifest.nsh").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(held!=INVALID_HANDLE_VALUE,"cannot create write-error fixture");
    rejects([&]{flush_archive(io_fail);},"OS manifest replacement error was hidden");CloseHandle(held);
    check(io_fail.archive->info().status=="failed"&&inspect_archive(io_dir).until==0,"failed manifest write advanced commit");
#endif
    // Deliberately oversized producer metadata: even a rejected record must
    // poison the writer, otherwise later commands could conceal the missing one.
    Simulator oversized(c);const auto oversized_dir=root/"record_limit";start_archive(oversized,oversized_dir);
    const auto before_limit=inspect_archive(oversized_dir);const auto saved_origin=oversized.experiment.origin;
    oversized.experiment.origin.assign(HistoryArchive::MaxRecordBytes+1,'x');
    rejects([&]{oversized.archive->protocol(oversized);},"oversized record accepted");oversized.experiment.origin=saved_origin;
    check(oversized.archive->info().status=="failed","record overflow did not latch writer failure");
    rejects([&]{flush_archive(oversized);},"overflow writer resumed and concealed missing metadata");
    check(inspect_archive(oversized_dir).records==before_limit.records,"record overflow changed committed prefix");
}
int main(int argc,char** argv)try{
    if(argc>=3&&std::string(argv[1])=="--fixtures"){auto root=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(root);save_config(fixture(),root/"fixture.json");write_atomic(root/"profile.json",analysis_profile_json({"archive control",fixture().analysis}));return 0;}
    if(argc==5&&std::string(argv[1])=="--compare"){auto a=load_checkpoint(std::filesystem::u8path(argv[2])),b=load_checkpoint(std::filesystem::u8path(argv[3]));exact(*a,*b,std::filesystem::u8path(argv[4]));return 0;}
    if(argc==5&&std::string(argv[1])=="--bench"){
        auto root=std::filesystem::u8path(argv[2]);double duration=std::stod(argv[3]);bool record=std::string(argv[4])=="disk";std::filesystem::create_directories(root);auto c=fixture();c.duration=duration;c.history_capacity=512;c.sample=.0001;for(auto& n:c.neurons)n.stop=duration;for(auto& s:c.stimulators)s.signal.stop=duration;
        Simulator s(c);auto begin=std::chrono::steady_clock::now();if(record)start_archive(s,root/"history",true);finish(s);double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();uint64_t memory=0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pm{};check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm))!=0,"memory measurement failed");memory=pm.PeakWorkingSetSize;
#endif
        uint64_t bytes=0;if(record)for(const auto& p:std::filesystem::directory_iterator(root/"history"))if(p.is_regular_file())bytes+=p.file_size();
        std::cout<<std::setprecision(17)<<"{\"model_seconds\":"<<duration<<",\"wall_seconds\":"<<seconds<<",\"peak_working_set_bytes\":"<<memory<<",\"archive_bytes\":"<<bytes<<",\"retained_samples\":"<<s.history.time.size()<<",\"steps\":"<<s.steps<<",\"mode\":\""<<(record?"disk":"ram")<<"\"}\n";return 0;
    }
    auto root=argc==2?std::filesystem::u8path(argv[1]):std::filesystem::current_path()/"archive-tests"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);auto begin=std::chrono::steady_clock::now();core(root);std::cout<<checks<<" archive checks passed; wall_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"; "<<root.u8string()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
