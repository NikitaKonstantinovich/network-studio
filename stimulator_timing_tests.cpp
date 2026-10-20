#include "io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace studio;
static int checks=0;
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Missing comparison artifact: "+p.u8string());return {std::istreambuf_iterator<char>(f),{}};}
static Config fixture(){
    auto c=preset(6);c.duration=.36;c.dt=50e-6;c.sample=.001;c.noise_sigma=.1;
    for(auto& n:c.neurons){n.current=0;n.frequency=0;}
    c.synapses[0].delay=.003013;
    Stimulator s;s.id=1;s.signal.start=0;s.first_delay=.05;s.signal.stop=.36;
    s.signal.frequency=10;s.signal.width=.02;s.signal.amplitude=20;s.recipients={0,1};
    c.stimulators={s};c.next_stimulator_id=2;return c;
}
static void until(Simulator& s,double at){while(s.time<at&&!s.done())check(s.step(),s.error.c_str());check(s.error.empty(),s.error.c_str());}
static void finish(Simulator& s){until(s,s.config.duration);}
static void pulses(const Simulator& s,const std::vector<double>& expected){
    check(s.history.shared_pulses.size()==expected.size(),"Extra/missing shared pulse");
    for(size_t i=0;i<expected.size();++i){
        check(std::abs(s.history.shared_pulses[i].start-expected[i])<2e-15,"Wrong actual pulse time");
        check(s.history.shared_pulses[i].complete,"Unclosed shared pulse");
    }
}
static double schedule(Simulator& s,Stimulator v,double at,bool restart=false){
    const auto state=s.state;const auto learning=s.plasticity;const auto pending=s.pending_deliveries;
    const auto time=s.time;const auto steps=s.steps;const auto spikes=s.spikes;const auto releases=s.releases;
    const auto samples=s.history.time;const auto values=s.history.values;const auto weights=s.history.weights;
    const auto result=s.schedule_stimulator(v,at,restart);
    check(state==s.state&&time==s.time&&steps==s.steps&&spikes==s.spikes&&releases==s.releases,"Command changed network state/counters");
    check(samples==s.history.time&&values==s.history.values&&weights==s.history.weights,"Command changed recorded network history");
    for(size_t i=0;i<learning.size();++i){const auto& a=learning[i];const auto& b=s.plasticity[i];
        check(a.weight==b.weight&&a.pre_trace==b.pre_trace&&a.post_trace==b.post_trace&&a.post_slow_trace==b.post_slow_trace,"Command changed learning");}
    check(pending.size()==s.pending_deliveries.size(),"Command changed delivery queue");
    for(size_t i=0;i<pending.size();++i)check(pending[i].t_emit==s.pending_deliveries[i].t_emit&&pending[i].t_arrival==s.pending_deliveries[i].t_arrival&&pending[i].synapse==s.pending_deliveries[i].synapse,"Command changed delivery");
    return result;
}
static void probe(){
    auto c=fixture();Simulator s(c);auto v=c.stimulators[0];v.signal.frequency=1;
    std::cout<<std::setprecision(17)<<"short_apply="<<schedule(s,v,.01)<<"\n";finish(s);
    for(const auto& p:s.history.shared_pulses)std::cout<<"actual_pulse="<<p.start<<".."<<p.end<<"\n";
    c.duration=151;c.sample=.01;c.stimulators[0].signal.start=150;c.stimulators[0].signal.stop=151;
    Simulator long_clock(c);v=c.stimulators[0];v.signal.frequency=1;
    const auto at=schedule(long_clock,v,150);const auto& accepted=long_clock.stimulus_commands.front().value;
    std::cout<<"clock150_apply="<<at<<" first="<<stimulator_signal(accepted).start<<" current_at_150="<<pulse_current(stimulator_signal(accepted),150)<<"\n";
}
static void timing(){
    auto c=fixture();
    {Simulator s(c);until(s,.01);auto v=c.stimulators[0];v.signal.frequency=1;
     check(schedule(s,v,s.time)==s.time,"Pre-first command delayed");finish(s);pulses(s,{.05});}
    {auto delayed=c;delayed.stimulators[0].signal.pulse_intervals={{.03,.04}};
     Simulator s(delayed);auto v=delayed.stimulators[0];v.signal.frequency=20;
     check(schedule(s,v,.06)==.06,"Pre-first table command delayed");finish(s);pulses(s,{.08,.13,.18,.23,.28,.33});}
    const double edge=.05+1/10.;
    for(double request:{.06,.083,std::nextafter(edge,0.),edge}){
        Simulator s(c);auto v=c.stimulators[0];v.signal.frequency=20;
        check(schedule(s,v,request)==edge,"Old cycle not completed at exact boundary");
        finish(s);pulses(s,{.05,edge,edge+.05,edge+.1,edge+.15,edge+.2});
        check(s.history.shared_pulses.front().end==.05+.02,"Frequency edit truncated active pulse");
    }
    {Simulator s(c);auto v=c.stimulators[0];v.signal.frequency=20;
     check(schedule(s,v,std::nextafter(edge,INFINITY))==.05+2/10.,"One ULP after boundary was treated as simultaneous");
     finish(s);pulses(s,{.05,edge,.25,.30,.35});}
    {Simulator s(c);until(s,.083);auto v=c.stimulators[0];v.signal.frequency=20;v.signal.start=.3;v.first_delay=.017;
     const auto at=s.time;check(schedule(s,v,at,true)==at,"Restart delayed");
     check(s.config.stimulators[0].signal.start==at&&s.config.stimulators[0].first_delay==.017,"Restart did not accept canonical phase");
     finish(s);std::vector<double> expected={.05};for(int i=0;at+.017+i*.05<c.duration;++i)expected.push_back(at+.017+i*.05);pulses(s,expected);}
    {Simulator s(c);auto v=c.stimulators[0];v.signal.frequency=20;v.first_delay=.04;
     const auto before=serialize_stimulus_runtime(s);bool rejected=false;try{s.schedule_stimulator(v,.06);}catch(const std::exception&){rejected=true;}
     check(rejected&&before==serialize_stimulus_runtime(s),"Mixed frequency/phase edit must reject atomically without restart");}
    for(bool frequency_zero:{false,true}){
        Simulator s(c);until(s,.06);auto v=s.config.stimulators[0];if(frequency_zero)v.signal.frequency=0;else v.enabled=false;
        const auto off=s.time;check(schedule(s,v,off)==off,"Disable must apply immediately");
        check(shared_current(s.config,0,s.time)==0,"Disabled source still active");
        until(s,.065);v=s.config.stimulators[0];v.enabled=true;v.signal.frequency=10;
        const auto on=s.time;check(schedule(s,v,on)==on,"Enable must apply immediately");finish(s);
        pulses(s,{.05,on,edge,.25,.35});check(s.history.shared_pulses.front().end==off,"Disable did not truncate pulse at application");
    }
    {Simulator s(c);until(s,.06);auto v=s.config.stimulators[0];v.signal.frequency=20;v.first_delay=.007;
     const auto at=s.time;schedule(s,v,at,true);finish(s);std::vector<double> expected={.05};
     for(int i=0;at+.007+i*.05<c.duration;++i)expected.push_back(at+.007+i*.05);
     pulses(s,expected);check(s.history.shared_pulses.front().end==at,"Restart did not close current pulse");}
    {Simulator edited(c),control(c);until(edited,.06);until(control,.06);auto v=edited.config.stimulators[0];v.name="Only the label changes";
     schedule(edited,v,edited.time);finish(edited);finish(control);
     check(edited.state==control.state&&edited.spikes==control.spikes&&edited.releases==control.releases&&edited.steps==control.steps,"Metadata edit changed later dynamics");
     check(edited.history.time==control.history.time&&edited.history.values==control.history.values&&edited.history.weights==control.history.weights,"Metadata edit changed later trajectories/learning");
     check(edited.history.events.size()==control.history.events.size(),"Metadata edit changed events");
     for(size_t i=0;i<edited.history.events.size();++i){auto a=edited.history.events[i],b=control.history.events[i];check(a.time==b.time&&a.index==b.index&&a.astro==b.astro,"Metadata edit changed event time");}}
}
static void forecast(){
    auto s=fixture().stimulators[0];
    check(next_stimulator_pulse(s,0)==.05&&next_stimulator_pulse(s,.05)==.05,"Forecast first onset");
    check(next_stimulator_pulse(s,.06)==.05+.1,"Forecast mistook active pulse for next onset");
    s.enabled=false;check(!std::isfinite(next_stimulator_pulse(s,0)),"Disabled source forecast");s.enabled=true;
    check(!std::isfinite(next_stimulator_pulse(s,s.signal.stop)),"Completed source forecast");
    auto disabled=s;disabled.enabled=false;auto active=s;
    std::vector<StimulatorCommand> queue={{1,.06,disabled,false},{2,.065,active,false}};
    check(next_stimulator_pulse(s,queue,.055)==.065,"Forecast ignores disable/re-enable commands");
    queue={{1,.06,active,true},{2,.06,disabled,false}};
    check(!std::isfinite(next_stimulator_pulse(s,queue,.055)),"Forecast emitted intermediate same-time command");
    s.signal.pulse_intervals={{.01,.02},{.02,.03},{.07,.1}};
    check(next_stimulator_pulse(s,.07)==.05+.07,"Forecast emitted touching table interval");
    s.signal.pulse_intervals.clear();s.signal.width=.1;
    check(!std::isfinite(next_stimulator_pulse(s,.06)),"Continuous current falsely forecasts repeated pulses");
}
static void continuation(){
    const auto root=std::filesystem::current_path()/("stimulator-timing-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    for(double split:{.01,.06,.1,.15}){
        auto c=fixture();Simulator full(c);auto part=std::make_unique<Simulator>(c);auto v=c.stimulators[0];v.signal.frequency=20;
        schedule(full,v,.01);schedule(*part,v,.01);
        until(full,split);until(*part,split);
        if(split>=.06){v=full.config.stimulators[0];v.signal.frequency=10;schedule(full,v,full.time);schedule(*part,v,part->time);}
        save_checkpoint(*part,root/"split.nscp");part=load_checkpoint(root/"split.nscp");
        finish(full);finish(*part);save_checkpoint(full,root/"full.nscp");save_checkpoint(*part,root/"split.nscp");
        check(bytes(root/"full.nscp")==bytes(root/"split.nscp"),"Resume around first/active/pending/boundary pulse differs");
    }
}
static void create_process(const std::filesystem::path& dir){
    std::filesystem::create_directories(dir);auto c=fixture();Simulator full(c),part(c);auto v=c.stimulators[0];v.signal.frequency=20;
    schedule(full,v,.06);schedule(part,v,.06);until(part,.1);check(!part.stimulus_commands.empty(),"Missing pending frequency at checkpoint");
    save_checkpoint(part,dir/"pending.nscp");finish(full);save_checkpoint(full,dir/"continuous.nscp");export_results(full,dir/"continuous");
    // Completed short display fixture retains a future command. Native smoke
    // can render both versions without advancing through its application time.
    c.duration=.1;Simulator display(c);schedule(display,v,.06);finish(display);
    save_checkpoint(display,dir/"native_pending.nscp");
}
static void resume_process(const std::filesystem::path& dir){
    auto s=load_checkpoint(dir/"pending.nscp");check(s->stimulus_commands.size()==1,"Pending frequency not restored");finish(*s);
    save_checkpoint(*s,dir/"resumed.nscp");export_results(*s,dir/"resumed");
    check(bytes(dir/"continuous.nscp")==bytes(dir/"resumed.nscp"),"Checkpoint resume not bit-exact");
    for(const char* suffix:{"_trace.csv","_weights.csv","_events.csv","_deliveries.csv","_pending_deliveries.csv","_shared_stimuli.csv","_stimulator_commands.json","_protocols.json"})
        check(bytes(dir/(std::string("continuous")+suffix))==bytes(dir/(std::string("resumed")+suffix)),"Resumed history/export mismatch");
    pulses(*s,{.05,.15,.2,.25,.3,.35});
}
int main(int argc,char** argv)try{
    if(argc==2&&std::string(argv[1])=="--probe"){probe();return 0;}
    if(argc==3){const auto dir=std::filesystem::u8path(argv[2]);if(std::string(argv[1])=="--create")create_process(dir);else if(std::string(argv[1])=="--resume")resume_process(dir);else return 2;}
    else {timing();forecast();continuation();}
    std::cout<<"PASS "<<checks<<" stimulator timing checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
