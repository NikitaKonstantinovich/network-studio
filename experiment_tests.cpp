#include "analysis.h"
#include "generator.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
using namespace studio;
static int checks=0;
static void check(bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error);check(s.error.empty(),s.error);}
static Config stages_fixture() {
    auto c=preset(6);c.duration=.12;c.sample=.0005;c.noise_sigma=.5;c.noise_interval=.0001;
    for(auto& n:c.neurons){n.current=12;n.start=.002;n.stop=.12;n.amplitude=30;n.frequency=100;n.width=.001;}
    c.synapses[0].delay=.004;c.synapses[0].stdp_rule=StdpRule::Multiplicative;
    ExperimentStage a;a.name="Train";a.duration=.04;a.neurons={0,1};a.stimulus_enabled=true;a.frequency=100;a.first_pulse=.002;
    auto b=a;b.name="Freeze";b.learning=false;b.frequency=10;
    auto d=a;d.name="Retrain";d.learning_rate=.4;d.clear_traces=true;
    c.stages={a,b,d};return c;
}
static Config lab6() {
    NetworkGeneration g;g.excitatory.count=400;g.inhibitory.count=100;g.inhibitory.a={.1,0};
    g.excitatory.in_degree=g.inhibitory.in_degree={30,3};g.excitatory.weight=g.inhibitory.weight={.5,.1};
    g.topology=NetworkTopology::WithinRadius;g.space={10,10,"model_unit"};g.radius=2;g.seed=706;
    g.synapse.stdp=true;g.synapse.stdp_rule=StdpRule::Multiplicative;g.synapse.a_plus=.001;g.synapse.a_minus=.0012;g.duration=100;g.sample=.002;
    auto c=generate_network(Config{},g,false);c.dt=.00005;c.noise_sigma=5;c.noise_interval=.001;c.bounded_journals=true;
    c.record_all=false;c.record_neurons={0,1,2,3,4,5};c.record_synapses={0,1,2,3};
    c.analysis.bin=.01;c.analysis.burst_window=.02;c.analysis.start_rate=1000;c.analysis.end_rate=250;c.analysis.min_neurons=25;c.analysis.response_max=.08;
    ExperimentStage b;b.name="Background";b.duration=100;b.learning=false;b.neurons={0,1,2,3,4,5};
    auto train=b;train.name="Training";train.duration=100;train.learning=true;train.stimulus_enabled=true;train.frequency=1;train.amplitude=50;train.width=.003;train.first_pulse=.1;
    auto test=train;test.name="Frozen test";test.duration=200;test.learning=false;
    auto retrain=train;retrain.name="Retraining";retrain.duration=100;retrain.frequency=10;
    c.stages={b,train,test,retrain};return c;
}
static void generator() {
    NetworkGeneration g;g.excitatory.count=40;g.inhibitory.count=10;g.excitatory.in_degree=g.inhibitory.in_degree={15,3};g.topology=NetworkTopology::WithinRadius;g.space={4,2,"mm"};g.radius=.5;
    GenerationSummary report;auto c=generate_network(Config{},g,false,&report);auto again=generate_network(Config{},g,false);
    check(serialize(c)==serialize(again),"radius seed reproducibility");check(report.insufficient_neighbors>0,"sparse neighbors warning");
    std::set<std::pair<int,int>> edges;for(auto e:c.synapses){const auto& a=c.neurons[e.source];const auto& b=c.neurons[e.target];check(std::hypot((a.px-b.px)*4,(a.py-b.py)*2)<=.5,"radius edge too long");check(e.source!=e.target&&edges.insert({e.source,e.target}).second,"duplicate/self radius edge");}
    check(serialize(deserialize(serialize(c)))==serialize(c),"metadata/topology roundtrip");
    Config boundary;boundary.neurons.resize(4);boundary.space={4,2,"mm"};boundary.neurons[0].px=boundary.neurons[0].py=0;
    boundary.neurons[1].px=.25;boundary.neurons[1].py=0;boundary.neurons[2].px=0;boundary.neurons[2].py=.5;boundary.neurons[3].px=1;boundary.neurons[3].py=0;
    check(radius_candidates(boundary,0,0,4,1,false)==std::vector<int>({1,2}),"radius inclusive boundary/no periodic wrap");
    check(radius_candidates(boundary,0,0,4,0,true)==std::vector<int>({0}),"explicit self policy");
    g.radius=0;c=generate_network(Config{},g,false,&report);check(c.synapses.empty()&&report.insufficient_neighbors==50,"zero radius shortage");
    g.allow_self=true;g.excitatory.in_degree=g.inhibitory.in_degree={1,0};c=generate_network(Config{},g,false);check(c.synapses.size()==50,"self-only radius");
}
static void metrics() {
    Config c;c.neurons.resize(4);c.neurons[3].inhibitory=true;History h;h.events={{.01,0,false},{.02,1,false},{.1,2,false},{.1,3,false},{.12,0,false}};
    AnalysisSettings a;a.bin=.1;a.min_neurons=1;a.start_rate=1000;
    auto r=analyze_network(c,h,.15,a);check(r.error.empty()&&r.group_hz.size()==2,"analysis bins");
    check(r.group_hz[0]==20&&std::abs(r.group_hz[1]-60)<1e-12&&r.neuron_hz[0]==5,"half-open bins and partial normalization");
    a.population=AnalysisPopulation::Inhibitory;r=analyze_network(c,h,.15,a);check(r.population_size==1&&std::abs(r.neuron_hz[1]-20)<1e-12,"inhibitory population");
    a.population=AnalysisPopulation::Selected;a.neurons={0,1};r=analyze_network(c,h,.15,a);check(r.population_size==2&&r.neuron_hz[0]==10,"selected population");
    for(double frequency:{1.,10.}) {
        h={};a=AnalysisSettings{};a.bin=.01;a.burst_window=.005;a.start_rate=300;a.end_rate=0;a.min_neurons=2;a.min_gap=.01;a.response_min=.005;a.response_max=.04;
        for(int k=0;k<20;++k){double t=.1+k/frequency;h.stimuli.push_back({t,30,0,0});h.stimuli.push_back({t,30,1,0});if(k!=7){h.events.push_back({t+.02,0,false});h.events.push_back({t+.02,1,false});}}
        h.events.push_back({h.stimuli.back().time+.07,0,false});h.events.push_back({h.stimuli.back().time+.07,1,false});
        r=analyze_network(c,h,h.stimuli.back().time+.2,a);
        check(r.bursts.size()==20&&r.responses.size()==20&&r.missed==1&&r.extra==1,"1/10 Hz matching, omission and extra burst");
        for(auto response:r.responses)if(response.latency>=0)check(std::abs(response.latency-.02)<1e-12,"independent latency");
        for(auto& e:h.events)e.time+=.05;r=analyze_network(c,h,h.stimuli.back().time+.3,a);
        check(r.missed>15,"equal average frequencies not called synchronized");
    }
    h={};h.events={{.01,0,false},{.01,1,false},{.011,2,false}};h.stimuli={{0,30,0,0},{.005,30,0,0}};
    a.response_min=0;a.response_max=.03;r=analyze_network(c,h,.1,a);check(r.responses.size()==2&&r.missed==1,"one burst cannot match twice");
}
static void stages(const std::filesystem::path& root) {
    auto c=stages_fixture();Simulator continuous(c);auto resumed=std::make_unique<Simulator>(c);
    continuous.enter_stage(0,true);resumed->enter_stage(0,true);bool frozen=false;double weight=0;int transitions=0;
    while(!continuous.done()) {
        check(continuous.step(),continuous.error);check(resumed->step(),resumed->error);
        auto index=continuous.experiment.stage_index;
        if(index==1&&!frozen){weight=continuous.plasticity[0].weight;frozen=true;}
        if(index==1)check(continuous.plasticity[0].weight==weight,"frozen weights moved");
        if(continuous.steps%73==0) {
            save_checkpoint(*resumed,root/"resume.nscp");resumed=load_checkpoint(root/"resume.nscp");++transitions;
            auto old_state=continuous.state;auto old_weights=continuous.plasticity[0].weight;auto old_steps=continuous.steps;
            auto analysis=analyze_network(continuous.config,continuous.history,continuous.time,c.analysis);
            check(old_state==continuous.state&&old_weights==continuous.plasticity[0].weight&&old_steps==continuous.steps,"analysis mutates solver");
        }
    }
    save_checkpoint(continuous,root/"continuous.nscp");save_checkpoint(*resumed,root/"resumed.nscp");
    check(bytes(root/"continuous.nscp")==bytes(root/"resumed.nscp"),"staged exact resume");check(transitions>10&&frozen,"stages actually tested");
    check(continuous.plasticity[0].weight!=c.synapses[0].weight,"learning actually changed weight");
    check(continuous.historical_input(0,.0025)==external_current(c,0,.0025),"new protocol changed old input");
    auto state=continuous.state;auto q=continuous.pending_deliveries;auto t=continuous.time;auto w=continuous.plasticity[0].weight;
    continuous.set_learning(false,0);check(state==continuous.state&&t==continuous.time&&w==continuous.plasticity[0].weight&&q.size()==continuous.pending_deliveries.size(),"learning control resets trajectory");
    continuous.set_learning(true,.5,true);check(continuous.plasticity[0].pre_trace==0&&continuous.plasticity[0].weight==w,"clear traces changes weights");
    check(continuous.learned_config().synapses[0].weight==w&&continuous.config.synapses[0].weight==c.synapses[0].weight,"learned weight export overwrites original");
    for(auto rule:{StdpRule::Additive,StdpRule::Multiplicative,StdpRule::Triplet}){auto edge=c.synapses[0];edge.stdp_rule=rule;StdpState x{.5,2,3,4},y=x,z=x;
        update_stdp(edge,x,1,1,.001,1);update_stdp(edge,y,1,1,.001,.3);update_stdp(edge,z,1,1,.001,0);
        check(std::abs((y.weight-.5)-.3*(x.weight-.5))<1e-15&&z.weight==.5&&x.pre_trace==z.pre_trace&&x.post_trace==z.post_trace,"learning gain/freeze trace semantics");}
    // Live delivery queue remains intact after bounded journals have rolled.
    NetworkGeneration g;g.excitatory.count=40;g.inhibitory.count=0;g.excitatory.in_degree={39,0};g.synapse.gain=0;g.synapse.delay=.01;g.duration=.5;g.sample=.01;
    auto stress=generate_network(Config{},g,false);stress.bounded_journals=true;stress.journal_capacity=50000;
    for(auto& n:stress.neurons){n.current=90;n.izh_a=.1;n.izh_d=2;}
    Simulator bounded(stress);finish(bounded);check(bounded.history.deliveries_after>0&&bounded.history.deliveries.size()<=50000,"delivery history did not roll");
    save_checkpoint(bounded,root/"bounded.nscp");auto loaded=load_checkpoint(root/"bounded.nscp");save_checkpoint(*loaded,root/"bounded2.nscp");check(bytes(root/"bounded.nscp")==bytes(root/"bounded2.nscp"),"rolling history checkpoint mismatch");
    std::cout<<"bounded_deliveries_after="<<bounded.history.deliveries_after<<" retained="<<bounded.history.deliveries.size()<<" pending="<<bounded.pending_deliveries.size()<<"\n";
    auto full_config=stress;full_config.bounded_journals=false;full_config.journal_capacity=MaxEvents;
    Simulator full(full_config);finish(full);
    // Normalize ONLY the intentionally different retention policy/history, then
    // compare all solver state, thresholds, adaptive control and live queue.
    full.config=bounded.config;full.history=bounded.history;
    save_checkpoint(full,root/"full-retention.nscp");
    check(bytes(root/"bounded.nscp")==bytes(root/"full-retention.nscp"),"journal retention changed numerical state");
    bounded.extend_duration(.52);loaded->extend_duration(.52);finish(bounded);finish(*loaded);
    save_checkpoint(bounded,root/"bounded-end.nscp");save_checkpoint(*loaded,root/"resumed-end.nscp");
    check(bytes(root/"bounded-end.nscp")==bytes(root/"resumed-end.nscp"),"resume after journal rollover changed continuation");
}
int main(int argc,char** argv)try {
    auto root=std::filesystem::current_path()/"build"/("experiment-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    if(argc==3&&std::string(argv[1])=="--fixtures") {
        root=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(root);save_config(stages_fixture(),root/"stages.json");
        auto c=lab6();save_config(c,root/"lab6_1hz.json");for(auto& s:c.stages)if(s.stimulus_enabled)s.frequency=10;save_config(c,root/"lab6_10hz.json");
        c.duration=.1;for(auto& s:c.stages)s.duration=.1;save_config(c,root/"benchmark.json");return 0;
    }
    generator();metrics();stages(root);std::cout<<checks<<" spatial/analysis/stage checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
