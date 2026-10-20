#include "analysis.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace studio;
static int checks=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
static std::string bytes(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
template<class F> static void rejects(F action,const char* label){bool caught=false;try{action();}catch(const std::exception&){caught=true;}check(caught,label);}
int main(int argc,char** argv)try {
    auto dir=std::filesystem::current_path()/"verification"/("analysis-profile-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(argc==3&&std::string(argv[1])=="--fixtures")dir=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(dir);
    Config c;c.neurons.resize(4);AnalysisSettings a;a.bin=.25;a.burst_window=.02;a.start_rate=100;a.end_rate=0;a.min_neurons=2;a.min_gap=.01;a.response_max=.1;
    History h;auto burst=[&](double t){h.events.push_back({t,0,false});h.events.push_back({t,1,false});};
    burst(.99);burst(1.5);burst(1.98);burst(2.0);h.stimuli={{1.45,25,0,0},{1.95,25,0,0},{2,25,0,0}};
    AnalysisInterval interval{1,2,false};auto r=analyze_network(c,h,3,a,interval);
    check(r.error.empty()&&r.full_interval&&r.prehistory_complete,"interval coverage");
    check(r.bursts.size()==2&&r.bursts[0].start==1.5&&r.bursts[0].complete&&!r.bursts[1].complete,"left-crossing burst excluded; right censored; exact right edge excluded");
    check(r.responses.size()==1&&r.responses[0].response==1.5&&r.incomplete_stimuli==1&&r.missed==0,"incomplete response window excluded, not missed");
    check(r.group_hz==std::vector<double>({0,0,8,8}),"rates only count [from,to), bins not zoom");
    h.events.clear();burst(1);burst(1.5);burst(2);r=analyze_network(c,h,3,a,interval);
    check(r.bursts.size()==2&&r.bursts.front().start==1,"exact left edge included");
    h.events_after=1.2;r=analyze_network(c,h,3,a,interval);
    check(!r.full_interval&&r.from==1.2&&!r.prehistory_complete,"partial history not full interval");
    auto later=analyze_network(c,h,3,a,{1.4,2,false});check(later.full_interval&&later.prehistory_complete,"silent prehistory synchronization");
    History dense;dense.events_after=.5;for(int k=0;k<200;++k)dense.events.push_back({.5+k*.01,k%2,false});
    auto unknown=analyze_network(c,dense,3,a,interval);check(!unknown.prehistory_complete&&unknown.full_interval,"W alone cannot restore hysteresis after truncated long burst");
    auto short_data=analyze_network(c,h,1.7,a,{1.4,2,false});check(!short_data.full_interval&&short_data.until==1.7,"requested future end not silently shortened");
    check(!analyze_network(c,h,1.7,a,{2,3,false}).error.empty(),"no overlap explicitly unavailable");
    check(!analyze_network(c,h,3,a,{2,1,false}).error.empty(),"invalid range rejected");
    check(!analyze_network(c,h,3,a,{NAN,3,false}).error.empty(),"nonfinite range rejected");
    // Gap-merging must use retained prehistory even when a second candidate starts in range.
    History merge;merge.events={{.94,0,false},{.94,1,false},{1.01,0,false},{1.01,1,false}};auto merge_settings=a;merge_settings.min_gap=.1;
    auto merged=analyze_network(c,merge,2,merge_settings,interval);check(merged.bursts.empty(),"left-crossing merged burst must not become new in-range burst");
    // Context on the right prevents a provisional closing from being called confirmed.
    History closing;closing.events={{1.96,0,false},{1.96,1,false}};auto near=analyze_network(c,closing,3,a,interval);
    check(near.bursts.size()==1&&!near.bursts[0].complete,"min_gap + detector window right context");
    AnalysisProfile p{u8"ЛР6 / профиль",a};auto encoded=analysis_profile_json(p);auto decoded=analysis_profile_from_json(encoded);
    check(decoded.name==p.name&&same_analysis_settings(decoded.settings,a),"profile roundtrip");
    auto ir=analysis_interval_from_json(analysis_interval_json(interval));check(ir.from==1&&ir.to==2&&!ir.current_end,"interval roundtrip");
    rejects([&]{analysis_profile_from_json(encoded.substr(0,encoded.size()/2));},"truncated profile");
    auto wrong=encoded;auto at=wrong.find("network_studio_analysis_profile");wrong.replace(at,31,"unknown");rejects([&]{analysis_profile_from_json(wrong);},"incompatible profile");
    rejects([&]{analysis_profile_from_json(std::string(65537,'x'));},"oversize profile");
    auto invalid=p;invalid.settings.bin=0;rejects([&]{analysis_profile_json(invalid);},"invalid settings");
    check(compare_analysis(later,a,later,a).empty(),"matching settings/interval compare");
    auto other=a;other.start_rate+=1;check(!compare_analysis(later,a,later,other).empty(),"profile mismatch detected");
    auto different=later;different.requested_from+=.1;check(!compare_analysis(later,a,different,a).empty(),"interval mismatch detected");
    check(!compare_analysis(later,a,short_data,a).empty()&&!compare_analysis(unknown,a,unknown,a).empty(),"incomplete comparisons refused");
    a.population=AnalysisPopulation::Selected;a.neurons={0,1};other=a;other.neurons={1,0};check(same_analysis_settings(a,other),"population set order is not significant");
    a=p.settings;export_analysis(later,a,dir/L"первый",p.name);export_analysis(later,a,dir/L"второй","Different name, same settings");
    check(compare_analysis_files(dir/L"первый_analysis.json",dir/L"второй_analysis.json").empty(),"export comparison uses settings not labels");
    export_analysis(later,other,dir/"different",p.name);check(!compare_analysis_files(dir/L"первый_analysis.json",dir/"different_analysis.json").empty(),"export profile mismatch");
    auto raw=bytes(dir/L"первый_analysis.json");write_atomic(dir/"bad.json",raw.substr(0,20));rejects([&]{compare_analysis_files(dir/"bad.json",dir/L"первый_analysis.json");},"truncated comparison input");
    // Solver, queue, plasticity, RNG, integrator and history must be byte-identical.
    auto config=preset(6);config.duration=.2;config.sample=.001;config.noise_sigma=.1;config.synapses[0].delay=.12;
    config.analysis.bin=.001;config.analysis.burst_window=.002;config.analysis.min_gap=0;config.analysis.min_neurons=1;config.analysis.response_max=.01;Simulator sim(config);
    while(!sim.done())check(sim.step(),"fixture step");save_checkpoint(sim,dir/"before.nscp");
    AnalysisInterval requested{.02,.15,false};AnalysisProfile fixture{"process-fixture",config.analysis};
    auto result=analyze_network(config,sim.history,sim.time,fixture.settings,requested);export_analysis(result,fixture.settings,dir/"expected",fixture.name);
    check(!result.bursts.empty()&&!result.responses.empty()&&result.responses.front().latency>=0,"process equivalence fixture must include real bursts and matched responses");
    write_atomic(dir/"profile.json",analysis_profile_json(fixture));export_results(sim,dir/"source");save_checkpoint(sim,dir/"after.nscp");
    check(bytes(dir/"before.nscp")==bytes(dir/"after.nscp"),"analysis changed full checkpoint");
    auto imported=analyze_export(dir/"source",fixture.settings,requested);export_analysis(imported,fixture.settings,dir/"imported",fixture.name);
    for(auto suffix:{"_analysis.json","_activity.csv","_bursts.csv","_responses.csv","_analysis.txt"})check(bytes(dir/(std::string("expected")+suffix))==bytes(dir/(std::string("imported")+suffix)),"offline import differs from in-memory analysis");
    std::cout<<checks<<" analysis profile / interval checks passed. "<<dir.u8string()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
