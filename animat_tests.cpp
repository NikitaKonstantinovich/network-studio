#include "animat.h"
#include "io.h"
#include "activity_field.h"
#include "simulation_runner.h"
#include "history_archive.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
using namespace studio;
static int checks=0;
static void check(bool v,const std::string& why){++checks;if(!v)throw std::runtime_error(why);}
static void near(double a,double b,const char* why){check(std::abs(a-b)<1e-12,why);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void until(Simulator& s,double t){while(s.time<t&&!s.done())if(!s.step())throw std::runtime_error(s.error);check(s.error.empty(),s.error);}
static void finish(Simulator& s){while(!s.done())if(!s.step())throw std::runtime_error(s.error);check(s.error.empty(),s.error);}
static void geometry() {
    auto c=animat_example(false);check(validate(c).empty(),validate(c));
    auto off_grid=c;off_grid.duration=.019999;check(!validate(off_grid).empty(),"off-grid final time must not trigger an early controller tick");
    check(arena_zone(c.space,c.animat.environment,1,.5)==2,"half-open rectangle seam");
    check(arena_zone(c.space,c.animat.environment,2,1)==2,"closed outside arena edge");
    ArenaZone circle;circle.id=3;circle.shape=ZoneShape::Circle;circle.x=1;circle.y=.5;circle.width=.25;circle.priority=1;
    c.animat.environment.zones.push_back(circle);
    check(arena_zone(c.space,c.animat.environment,1.25,.5)==3,"closed circle wins priority");
    auto twin=circle;twin.id=4;c.animat.environment.zones.push_back(twin);
    check(arena_zone(c.space,c.animat.environment,1,.5)==3,"stable ID tie");
    std::reverse(c.animat.environment.zones.begin(),c.animat.environment.zones.end());
    check(arena_zone(c.space,c.animat.environment,1,.5)==3,"priority independent of ordering");
    c.stimulators[0].x=.4;c.stimulators[0].y=.5;c.stimulators[0].radius=.1;
    check(stimulator_recipients(c,c.stimulators[0])==std::vector<int>{0},"physical / normalized recipient conversion");
    auto a=c.animat;a.gain=1;a.max_displacement=10;
    auto m=animat_motion(c.space,a,.5,.25,.1,.2,0,0);
    near(m.x,.7,"rectangular Jacobian x");near(m.y,.45,"rectangular Jacobian y");
    a.max_displacement=.1;m=animat_motion(c.space,a,1,.5,1,1,0,0);
    near(std::hypot(m.requested_x,m.requested_y),.1,"physical cap");
    m=animat_motion(c.space,a,1,.5,0,0,.1,0);near(m.x,1,"silence stays");
    a.silence=AnimatSilence::RepeatLast;m=animat_motion(c.space,a,1,.5,0,0,.1,0);near(m.x,1.1,"explicit repeat last");
    m=animat_motion(c.space,a,1.99,.5,1,0,0,0);near(m.x,2,"clamp");
    a.boundary=AnimatBoundary::Reject;m=animat_motion(c.space,a,1.99,.5,1,0,0,0);near(m.x,1.99,"reject");
    Simulator s(c);auto v=c.animat.field;WeightField g;g.update(c,&s.plasticity,v);ActivityField f;
    s.activity.deliveries.push_back({.01,.011,0});f.update(c,s.plasticity,s.activity,.02,&s,1,v,g);
    auto field=f.vector_at(c,1,.5);check(field.x>0&&field.y==0,"directed delivery field");
    v.activity_mode=1;f.update(c,s.plasticity,s.activity,.02,&s,1,v,g);check(f.vector_at(c,1,.5).x==0,"response criterion needs later post");
    s.activity.spikes.push_back({.012,1,false});f.update(c,s.plasticity,s.activity,.02,&s,1,v,g);
    check(f.vector_at(c,1,.5).x>0,"delivery followed by response");
}
static void statistics() {
    auto c=animat_example(false);AnimatRuntime a;a.environments.push_back({0,c.animat.environment});
    for(auto [t,x]:{std::pair{0.,.5},std::pair{1.,1.5},std::pair{3.,.5}}) {
        AnimatRow r;r.time=t;r.x=x;r.x0=a.rows.empty()?x:a.rows.back().x;r.y=r.y0=.5;
        r.zone=arena_zone(c.space,c.animat.environment,x,.5);r.kind=t?AnimatRowKind::Tick:AnimatRowKind::Initial;a.rows.push_back(r);
    }
    auto stats=analyze_animat(c,a,4,0,4);
    near(stats.path,2,"known path");near(stats.mean_speed,.5,"known mean speed");near(stats.first_exit,1,"known first exit");
    check(stats.complete&&stats.visits.size()==3&&stats.crossings.size()==2,"visits/crossings");
    near(stats.zones[0].time,2,"zone 1 time");near(stats.zones[1].time,2,"zone 2 time");
    a.rows[0].stage_run=1;a.rows[1].stage_run=a.rows[2].stage_run=2;a.rows[1].kind=AnimatRowKind::Stage;
    auto stage_stats=analyze_animat(c,a,4,0,4);near(stage_stats.stages[1].first_exit,2,"first exit after stage teleport");
    for(auto& r:a.rows)r.stage_run=0;a.rows[1].kind=AnimatRowKind::Tick;
    auto partial=analyze_animat(c,a,4,.5,5);check(!partial.complete&&partial.visits.front().left_censored&&partial.visits.back().right_censored,"partial interval and visits");
    // Traversing a middle zone with identical endpoint membership still records
    // two crossings and a zero-duration visit.
    c.animat.environment.zones.clear();ArenaZone middle;middle.x=.8;middle.width=.4;middle.height=1;
    c.animat.environment.zones={middle};a.environments[0].value=c.animat.environment;
    a.rows.resize(2);for(auto& r:a.rows)r.zone=0;
    stats=analyze_animat(c,a,2,0,2);
    check(stats.crossings.size()==2&&stats.visits.size()==3,"transit splits outside visit");
    near(stats.visits[1].end-stats.visits[1].start,0,"transit has zero dwell");check(stats.first_exit==-1,"outside isn't a zone first-exit");
}
static Config quiet() {
    auto c=animat_example(false);c.animat.gain=0;c.duration=.36;
    for(auto& n:c.neurons){n.current=0;n.amplitude=0;}
    c.stimulators[0].signal.frequency=10;return c;
}
static void timing_and_replay(const std::filesystem::path& root) {
    for(double edit:{.01,.06,.15,.151}) {
        auto c=quiet();Simulator s(c);
        AnimatAction action;action.kind=AnimatActionKind::Environment;action.environment=c.animat.environment;
        for(auto& z:action.environment.zones)z.frequency=1;
        until(s,edit);action.time=s.time;
        auto state=s.state;auto weights=s.history.weights;auto pending=s.pending_deliveries;
        s.schedule_animat(action);
        check(state==s.state&&weights==s.history.weights&&pending.size()==s.pending_deliveries.size(),"environment edit mutated network");
        finish(s);check(!s.history.shared_pulses.empty(),"no pulses");
        near(s.history.shared_pulses.front().start,.05,"first pulse not advanced");
        check(s.history.shared_pulses.size()==(edit<.05?1:edit<=.15?2:3),"extra/missing zone-frequency pulse");
        Simulator replay(c);replay.replay_animat(read_animat_commands(animat_commands_json(s.animat.actions)));finish(replay);
        save_checkpoint(s,root/"live.nscp");save_checkpoint(replay,root/"replay.nscp");
        check(bytes(root/"live.nscp")==bytes(root/"replay.nscp"),"action replay not byte-exact");
    }
    auto c=quiet();Simulator s(c);until(s,.06);
    AnimatAction a;a.kind=AnimatActionKind::Environment;a.environment=c.animat.environment;a.environment.enabled=false;a.time=s.time;s.schedule_animat(a);
    check(shared_current(s.config,0,s.time)==0,"disabled source");
    until(s,.07);a.time=s.time;a.environment.enabled=true;s.schedule_animat(a);finish(s);
    check(s.history.shared_pulses.size()==4,"enable/disable pulse semantics changed");
}
static void resume(const std::filesystem::path& root) {
    {auto c=animat_example(false);c.duration=.08;c.synapses[0].stdp=true;Simulator frozen(c);const auto initial=frozen.state;frozen.set_learning(false,1,false);finish(frozen);
     check(frozen.plasticity[0].weight==c.synapses[0].weight&&frozen.plasticity[0].pre_trace>0,"frozen learning must preserve weight and evolve traces");
     check(frozen.state[syn_offset(c,0)]!=initial[syn_offset(c,0)],"STP stopped when STDP was frozen");}
    {auto c=animat_example(false);c.duration=.08;Simulator reference(c);c.animat.field.scale=10;c.animat.field.activity_scale=20;c.animat.field.zoom=2;c.animat.field.auto_scale=true;c.animat.field.current=false;Simulator visual(c);finish(reference);finish(visual);
     save_checkpoint(reference,root/"field-reference.nscp");save_checkpoint(visual,root/"field-visual.nscp");check(bytes(root/"field-reference.nscp")==bytes(root/"field-visual.nscp"),"visual field settings changed controller");}

    auto c=animat_example(false);c.duration=.24;c.synapses[0].stdp=true;c.noise_sigma=.3;c.synapses[0].delay=.001013;
    for(double split:{.019975,.02,.049975,.05,.050025,.099975,.1}) {
        Simulator full(c),part(c);
        AnimatAction a;a.time=.06;a.kind=AnimatActionKind::Environment;a.environment=c.animat.environment;
        for(auto& z:a.environment.zones)z.frequency=1;full.schedule_animat(a);part.schedule_animat(a);
        until(part,split);save_checkpoint(part,root/"состояние.nscp");auto restored=load_checkpoint(root/"состояние.nscp");
        finish(full);finish(*restored);save_checkpoint(full,root/"full.nscp");save_checkpoint(*restored,root/"restored.nscp");
        check(bytes(root/"full.nscp")==bytes(root/"restored.nscp"),"checkpoint exact continuation");
    }
    {auto c=animat_example(false);ExperimentStage stage;stage.animat.position=true;stage.animat.x=.25;stage.animat.y=.75;c.stages={stage};Simulator placed(c);auto state=placed.state;placed.enter_stage(0,false);check(placed.animat.x==.25&&placed.animat.y==.75&&placed.state==state,"custom stage placement changed network or ignored coordinates");}
    auto plan=animat_example(true);for(auto& st:plan.stages)st.duration=.05;plan.duration=.2;
    Simulator s(plan);AnimatAction a;a.kind=AnimatActionKind::Stage;a.automatic=true;s.schedule_animat(a);
    auto initial=s.plasticity;until(s,.049975);
    for(size_t i=0;i<initial.size();++i)check(initial[i].weight==s.plasticity[i].weight,"frozen stage weight changed");
    finish(s);check(s.animat.saved.size()==8,"stage entry/exit results not captured");
    check(s.animat.environments.size()==2&&s.animat.stage_run==4,"automatic environment/stage sequence");
    save_checkpoint(s,root/"stages.nscp");auto restored=load_checkpoint(root/"stages.nscp");
    check(serialize_animat(s)==serialize_animat(*restored),"saved stage results lost");
    export_results(s,root/"stages");
}
static void runner(const std::filesystem::path& root) {
    auto c=animat_example(false);c.duration=.12;c.noise_sigma=.3;c.synapses[0].stdp=true;
    Simulator control(c);finish(control);save_checkpoint(control,root/"direct.nscp");
    for(int hz:{1,60}) {
        SimulationRunner runner(c);runner.set_display_hz(hz);
        RunnerCommand run;run.kind=RunnerCommand::Kind::Advance;run.value=c.duration;
        check(runner.submit(run).get().ok,"runner calculation");
        RunnerCommand save;save.kind=RunnerCommand::Kind::SaveCheckpoint;save.path=root/("runner"+std::to_string(hz)+".nscp");
        check(runner.submit(save).get().ok,"runner checkpoint");
        check(bytes(save.path)==bytes(root/"direct.nscp"),"GUI refresh frequency changed numerical state");
        runner.shutdown();
    }
}
static void archive_check(const std::filesystem::path& root) {
    auto c=animat_example(false);c.duration=.16;c.bounded_journals=true;c.history_capacity=16;
    Simulator full(c),part(c);finish(full);
    auto base=root/("archive-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto branch=base;branch+="-branch";
    start_archive(part,base,true,false);until(part,.08);save_checkpoint(part,root/"archived.nscp");
    part.archive.reset();auto resumed=load_checkpoint(root/"archived.nscp");
    start_archive(*resumed,branch,true,false);finish(*resumed);flush_archive(*resumed);
    check(full.state==resumed->state&&serialize_animat(full)==serialize_animat(*resumed),"archive branch changed trajectory/dynamics");
    std::vector<std::string> rows;
    auto read=[&](const auto& path){scan_archive(path,[&](ArchiveRecord type,const std::string& bytes){if(type==ArchiveRecord::Animat)rows.push_back(bytes);});};
    read(base);read(branch);check(rows.size()==full.animat.rows.size(),"archive omitted/duplicated controller record");
    for(size_t i=0;i<rows.size();++i)check(rows[i]==encode_animat_row(full.animat.rows[i]),"archive row differs from memory");
    auto exported=base;exported+="-export";export_archive(branch,0,.17,exported);
    std::string csv=animat_csv_header();for(const auto& row:full.animat.rows)csv+=animat_csv_row(row);
    check(bytes(exported.string()+"_animat.csv")==csv,"disk and memory full trajectory CSV differ");
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path root=argc>2?std::filesystem::u8path(argv[2]):std::filesystem::path("animat-test-results");
        std::filesystem::create_directories(root);
        const std::string mode=argc>1?argv[1]:"test";
        if(mode=="examples") {
            save_config(animat_example(false),root/"minimal.json");save_config(animat_example(true),root/"spatial_learning.json");return 0;
        }
        if(mode=="process-prepare") {
            auto c=animat_example(false);c.duration=.3;c.noise_sigma=.3;c.synapses[0].stdp=true;
            Simulator s(c);AnimatAction a;a.time=.11;a.kind=AnimatActionKind::Home;s.schedule_animat(a);
            until(s,.1);save_checkpoint(s,root/"pending.nscp");finish(s);save_checkpoint(s,root/"expected.nscp");export_results(s,root/"expected");return 0;
        }
        if(mode=="process-resume") {
            auto s=load_checkpoint(root/"pending.nscp");s->set_compute_threads(6);finish(*s);
            save_checkpoint(*s,root/"actual.nscp");export_results(*s,root/"actual");
            check(bytes(root/"actual.nscp")==bytes(root/"expected.nscp"),"new-process resume bytes differ");return 0;
        }
        geometry();statistics();timing_and_replay(root);resume(root);runner(root);archive_check(root);
        std::cout<<"PASS "<<checks<<" neuroanimat checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
