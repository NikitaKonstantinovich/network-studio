#include "io.h"
#include "activity_field.h"
#include "analysis.h"
#include "generator.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace studio;
static int checks=0;
static void check(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
static void near(double a,double b,double tol,const char* why){check(std::abs(a-b)<=tol,why);}
template<class F> static void rejects(F f,const char* why){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,why);}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error);check(s.error.empty(),s.error);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static Stimulator source(uint64_t id=1){Stimulator s;s.id=id;s.signal.start=.000037;s.signal.stop=.12;s.signal.frequency=25;s.signal.width=.012;s.signal.amplitude=30;return s;}
static Config shared_fixture(bool spatial=false) {
    auto c=preset(6);c.name="Shared source / delayed TM + STDP";c.duration=.12;c.sample=.0005;c.noise_sigma=.2;
    for(auto& n:c.neurons){n.current=0;n.frequency=0;}
    c.neurons[0].px=.15;c.neurons[0].py=.4;c.neurons[1].px=.8;c.neurons[1].py=.6;
    c.synapses[0].delay=.004013;c.synapses[0].stdp=true;c.synapses[0].weight=.6;
    c.space={100,100,"um"};auto s=source();s.spatial=spatial;s.x=20;s.y=40;s.radius=40;s.recipients={0,1};
    c.stimulators={s};c.next_stimulator_id=2;c.analysis.reference_stimulator=1;return c;
}
static void analytic() {
    for(double dt:{50e-6,25e-6,12.5e-6})for(double phase:{0.,1e-9,101e-6,124.999e-6})for(bool table:{false,true}) {
        auto c=preset(3);c.dt=dt;c.duration=.011;c.sample=.00007;c.noise_sigma=0;
        c.parameters[na::g_Na]=c.parameters[na::g_K]=c.parameters[na::g_L]=0;
        c.neurons[0].frequency=0;c.neurons[0].current=0;c.neurons.resize(6,c.neurons[0]);
        auto st=source();st.signal.start=phase;st.signal.stop=.010008+phase;st.signal.frequency=200;st.signal.width=10e-6;st.signal.amplitude=100;
        st.recipients={0,1,2,3,4,5};if(table)st.signal.pulse_intervals={{3e-6,13e-6},{.00498,.005}};
        c.stimulators={st};c.next_stimulator_id=2;Simulator s(c);finish(s);
        double area=0;size_t pulses=0;
        auto intervals=st.signal.pulse_intervals;if(intervals.empty())intervals={{0,st.signal.width}};
        for(int k=0;k<4;++k)for(auto p:intervals){double origin=phase+k/st.signal.frequency;double length=std::max(0.,std::min({origin+p.stop,st.signal.stop,c.duration})-std::max(0.,origin+p.start));area+=length*100;if(length>0)++pulses;}
        for(int i=0;i<6;++i)near(s.state[4*i]-c.neurons[i].voltage,1000*area/c.parameters[na::C_m],3e-9,"Shared capacitor charge/grid independence");
        check(s.history.shared_pulses.size()==pulses,"Six targets multiplied pulse count");
        for(auto p:s.history.shared_pulses)check(p.recipients.size()==6&&p.complete&&p.end>p.start,"Shared pulse identity/closure");
        for(size_t i=1;i+1<s.history.time.size();++i)check(s.history.time[i]==i*c.sample,"Shared boundaries changed sample cadence");
    }
    auto c=shared_fixture();c.neurons[0].frequency=10;c.neurons[0].start=0;c.neurons[0].stop=.1;c.neurons[0].width=.02;c.neurons[0].amplitude=4;c.neurons[0].current=3;
    auto b=c.stimulators[0];b.id=2;b.signal.amplitude=-2;c.stimulators.push_back(b);c.next_stimulator_id=3;
    const double t=.001;double old=external_current_with_protocol(c,c.neurons[0],0,t);
    check(external_current(c,0,t)==old+28,"Additive DC/legacy/shared/noise");
    auto encoded=serialize(c);check(serialize(deserialize(encoded))==encoded,"Stimulator config exact round trip");
    auto invalid=c;invalid.stimulators[1].id=1;check(!validate(invalid).empty(),"Duplicate ID accepted");
    invalid=c;invalid.stimulators[0].recipients={0,0};check(!validate(invalid).empty(),"Duplicate recipient accepted");
    auto high=c;high.analysis.reference_stimulator=0;high.stimulators={high.stimulators[0]};high.stimulators[0].id=(1ull<<53)+17;high.next_stimulator_id=(1ull<<53)+18;
    check(serialize(deserialize(serialize(high)))==serialize(high),"Stable ID lost uint64 precision");
    remove_neuron(c,0);check(c.stimulators[0].id==1&&c.stimulators[1].id==2&&c.stimulators[0].recipients==std::vector<int>{0},"Deletion changed IDs or failed remapping");
}
static void commands(const std::filesystem::path& root) {
    auto c=shared_fixture(true);c.neurons.resize(4,c.neurons[0]);c.synapses.clear();c.neurons[0].px=.5;c.neurons[0].py=.5;
    c.neurons[1].px=.9;c.neurons[1].py=.5;c.neurons[2].px=.90000001;c.neurons[2].py=.5;c.neurons[3].px=.5;c.neurons[3].py=.51;c.neurons[3].inhibitory=true;
    auto& v=c.stimulators[0];v.x=v.y=50;v.radius=40;
    check(stimulator_recipients(c,v)==std::vector<int>({0,1,3}),"Physical radius inside/boundary/outside");
    v.filter=StimulatorFilter::Excitatory;check(stimulator_recipients(c,v)==std::vector<int>({0,1}),"Spatial E filter");
    v.filter=StimulatorFilter::Inhibitory;check(stimulator_recipients(c,v)==std::vector<int>({3}),"Spatial I filter");v.filter=StimulatorFilter::All;
    Simulator s(c);while(s.time<.003)check(s.step(),s.error);
    check(s.history.shared_pulses.size()==1,"No initial shared pulse");auto state=s.state;auto steps=s.steps;auto pulse=s.history.shared_pulses[0].pulse;double old=s.historical_input(0,.001);
    const double at=s.move_stimulator(1,95,50,s.time);
    check(at==s.time&&state==s.state&&steps==s.steps&&s.history.shared_pulses[0].pulse==pulse,"Move resets dynamics or pulse");
    check(s.historical_input(0,.001)==old&&shared_current(s.config,0,s.time)==0,"Move changed old input or failed immediate new recipients");
    s.move_neuron(0,.9,.5);check(shared_current(s.config,0,s.time)==30,"Moving neuron did not update recipients");
    auto changed=s.config.stimulators[0];changed.signal.frequency=50;
    const double boundary=s.schedule_stimulator(changed,s.time);
    check(boundary==c.stimulators[0].signal.start+1/25.,"Frequency did not finish old cycle");
    const auto pending=s.stimulus_commands.front();finish(s);
    check(s.stimulus_changes.back().time==boundary&&s.config.stimulators[0].signal.frequency==50,"Scheduled frequency not applied exactly");
    size_t changes=0;for(const auto& p:s.history.shared_pulses)if(p.start==boundary)++changes;check(changes==1,"Frequency created duplicate pulse");
    for(int k=1;k<30;++k){auto exact=c;exact.stimulators[0].signal.frequency=17;exact.stimulators[0].signal.stop=5;Simulator edge(exact);auto replacement=exact.stimulators[0];replacement.signal.frequency=19;double t=exact.stimulators[0].signal.start+k*(1/17.);check(edge.schedule_stimulator(replacement,t)==t,"Exact old-period boundary shifted by floating-point quotient");}
    // Replay exact recorded times, including a sub-macro-step movement. No GUI clock.
    auto base=shared_fixture(true);Simulator first(base),second(base);
    auto moved=base.stimulators[0];moved.x=80;
    first.replay_stimulators({{1,.009013,moved,false},{2,.061117,base.stimulators[0],false}});
    write_atomic(root/"commands.json",serialize_stimulus_runtime(first));load_stimulator_replay(second,root/"commands.json");
    finish(first);finish(second);save_checkpoint(first,root/"replay1.nscp");save_checkpoint(second,root/"replay2.nscp");
    check(bytes(root/"replay1.nscp")==bytes(root/"replay2.nscp"),"Recorded-time replay mismatch");
    check(first.history.shared_pulses.front().start==base.stimulators[0].signal.start&&first.history.shared_pulses.front().end==base.stimulators[0].signal.start+base.stimulators[0].signal.width,"Move split or restarted pulse");
    auto before=serialize_stimulus_runtime(second);rejects([&]{second.replay_stimulators({{1,.2,moved,false}});},"Duplicate replay sequence accepted");check(before==serialize_stimulus_runtime(second),"Rejected replay mutated state");
    Simulator stage(base);ExperimentStage a;a.duration=.025;a.stimulus_enabled=true;a.frequency=100;a.width=.002;a.stimulator_ids={1};stage.config.stages={a};stage.enter_stage(0);finish(stage);
    check(stage.history.shared_pulses.size()==3&&stage.history.shared_pulses[0].stage==0&&stage.history.shared_pulses[0].start==0,"Stage source protocol");
}
static void fields() {
    auto c=shared_fixture();c.space={1,1,"model_unit"};c.stimulators.clear();c.next_stimulator_id=1;c.analysis.reference_stimulator=0;
    c.neurons[0].px=.1;c.neurons[0].py=.5;c.neurons[1].px=.9;c.neurons[1].py=.5;c.neurons.push_back(c.neurons[0]);
    Synapse a=c.synapses[0];a.source=0;a.target=1;a.weight=2;a.delay=.01;a.enabled=true;
    Synapse b=a;b.source=1;b.target=0;b.weight=2;Synapse d=a;d.source=2;d.target=1;d.weight=3;d.weight_max=5;c.synapses={a,b,d};
    Simulator s(c);check(s.error.empty(),s.error);s.time=.02;TopologyViewState v;v.columns=v.rows=1;v.activity_window=.05;WeightField geometry;ActivityField f;
    auto update=[&]{geometry.update(c,&s.plasticity,v);f.update(s,v,geometry);return f.vector_at(c,.5,.5);};
    check(update().count==0,"Silent field nonzero");
    s.activity.deliveries={{.001,.011,0},{.002,.012,0},{.003,.013,2}};
    auto cell=update();near(cell.x,5,0,"Active edge counted more than once");check(cell.events==3&&cell.count==2,"Delivery tooltip counts");
    s.activity.deliveries.push_back({.004,.014,1});cell=update();near(cell.x,3,0,"Vector cancellation");
    const auto processed=f.processed_events;update();check(f.processed_events==processed&&geometry.geometry_builds==1,"Paused frames replayed history/rebuilt geometry");
    s.time=.07;check(update().count==0,"Activity did not expire");
    s.time=.02;v.activity_mode=1;v.response_min=.001;v.response_max=.01;s.activity.spikes={{.011,1,false}};
    check(update().count==0,"Simultaneous delivery/post counted as response");
    s.activity.spikes.push_back({.018,1,false});cell=update();near(cell.x,5,0,"One post must match all eligible distinct deliveries");check(cell.events==3,"Responses double-counted or dropped deliveries");
    s.activity.spikes.push_back({.019,1,false});check(update().events==3,"Second post recounted deliveries");
    // This spike could have been externally driven: event criterion intentionally cannot infer causality.
    s.activity.deliveries.push_back({.009,.019,1});s.time=.03;s.activity.spikes.push_back({.025,0,false});near(update().x,3,0,"Operational external-post criterion");
    v.activity_window=2;geometry.update(c,&s.plasticity,v);f.update(s,v,geometry);check(!f.available,"Missing retained interval disguised as valid history");rejects([&]{f.vector_at(c,.5,.5);},"Unavailable field query accepted");
}
static void continuation(const std::filesystem::path& root) {
    for(bool hh:{false,true})for(auto model:{SynapseModel::ConductanceSTP,SynapseModel::ExponentialCurrent,SynapseModel::TsodyksMarkram}) {
        auto c=shared_fixture(true);c.synapses[0].model=model;c.synapses[0].stdp_rule=hh?StdpRule::Triplet:StdpRule::Additive;
        if(hh)for(auto& n:c.neurons)n.model=NeuronModel::HodgkinHuxley;
        c.astrocytes={{}};c.synapses[0].astro=0;c.record_all=false;c.record_neurons={0};c.record_synapses={};
        Simulator full(c);auto split=std::make_unique<Simulator>(c);auto moved=c.stimulators[0];moved.x=75;
        std::vector<StimulatorCommand> cmds={{1,.009013,moved,false},{2,.061117,c.stimulators[0],false}};
        full.replay_stimulators(cmds);split->replay_stimulators(cmds);WeightField geometry;ActivityField field;TopologyViewState v;v.activity_mode=1;
        while(!full.done()) {
            check(full.step()&&split->step(),full.error+split->error);
            if(full.steps%113==0) {save_checkpoint(*split,root/"split.nscp");split=load_checkpoint(root/"split.nscp");geometry.update(split->config,&split->plasticity,v);field.update(*split,v,geometry);}
        }
        save_checkpoint(full,root/"full.nscp");save_checkpoint(*split,root/"split.nscp");check(bytes(root/"full.nscp")==bytes(root/"split.nscp"),"Checkpoint exact continuation/visualization invariance");
        WeightField other;ActivityField f;other.update(full.config,&full.plasticity,v);f.update(full,v,other);geometry.update(split->config,&split->plasticity,v);field.update(*split,v,geometry);
        check(f.cells.size()==field.cells.size(),"Field restored size");for(size_t k=0;k<f.cells.size();++k)check(f.cells[k].x==field.cells[k].x&&f.cells[k].y==field.cells[k].y&&f.cells[k].events==field.cells[k].events,"Activity window not reconstructed exactly");
    }
}
static void benchmark() {
    NetworkGeneration g;g.excitatory.count=400;g.inhibitory.count=100;g.excitatory.in_degree=g.inhibitory.in_degree={30,0};g.duration=.02;g.sample=.001;g.synapse.gain=0;
    auto c=generate_network(Config{},g,false);auto stim=source();stim.signal.stop=.02;stim.spatial=true;stim.x=stim.y=.5;stim.radius=.4;c.stimulators={stim};c.next_stimulator_id=2;
    Simulator s(c);auto begin=std::chrono::steady_clock::now();finish(s);double simulation_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    // Controlled 15k deliveries, all active, independently of whether this short run spiked.
    Simulator synthetic(c);synthetic.time=.02;for(int k=0;k<int(c.synapses.size());++k)synthetic.activity.deliveries.push_back({.01,.01,k});
    WeightField geometry;TopologyViewState v;v.activity_window=.05;ActivityField f;geometry.update(c,&synthetic.plasticity,v);f.update(synthetic,v,geometry);
    const auto count=f.processed_events;begin=std::chrono::steady_clock::now();for(int k=0;k<100;++k){geometry.update(c,&synthetic.plasticity,v);f.update(synthetic,v,geometry);}
    double frame_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/100;
    check(geometry.geometry_builds==1&&f.processed_events==count&&count==15000,"500/15k incremental cache");
    std::cout<<"BENCH neurons="<<c.neurons.size()<<" edges="<<c.synapses.size()<<" geometry_ms="<<geometry.geometry_ms<<" cache_bytes="<<geometry.cache_bytes()<<" both_fields_update_ms="<<frame_ms<<" simulate_20ms_wall_ms="<<simulation_ms<<" recent_deliveries="<<s.activity.deliveries.size()<<"\n";
}
static void fixtures(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);auto c=shared_fixture();c.neurons.resize(6,c.neurons[0]);c.stimulators[0].recipients={0,1,2,3,4,5};
    for(int i=0;i<6;++i){c.neurons[i].name="Recipient "+std::to_string(i+1);c.neurons[i].px=.15+.14*i;c.neurons[i].py=.35+.25*(i%2);}save_config(c,root/"shared.json");
    c=shared_fixture(true);save_config(c,root/"spatial.json");Simulator replay(c);auto moved=c.stimulators[0];moved.x=80;replay.replay_stimulators({{1,.009013,moved,false},{2,.061117,c.stimulators[0],false}});write_atomic(root/"moves.json",serialize_stimulus_runtime(replay));
    c=shared_fixture();c.name="Delayed chain: deliveries do not guarantee responses";c.neurons.resize(4,c.neurons[0]);c.stimulators[0].recipients={0};c.synapses.clear();
    for(int i=0;i<4;++i){c.neurons[i].px=.1+i*.26;c.neurons[i].py=.5;c.neurons[i].name="N"+std::to_string(i+1);if(i<3){Synapse s;s.source=i;s.target=i+1;s.model=SynapseModel::ExponentialCurrent;s.delay=.004+i*.001;s.gain=i==2?0:40;s.weight=.6;c.synapses.push_back(s);}}
    save_config(c,root/"chain.json");c.duration=2.03;c.sample=.001;c.stimulators[0].signal.stop=2.03;c.stimulators[0].signal.frequency=1;save_config(c,root/"fields_1hz.json");c.stimulators[0].signal.frequency=10;save_config(c,root/"fields_10hz.json");
}
int main(int argc,char** argv)try {
    if(argc==3&&std::string(argv[1])=="--fixtures"){fixtures(std::filesystem::u8path(argv[2]));return 0;}
    auto root=std::filesystem::current_path()/"build"/("stimulator-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    analytic();commands(root);fields();continuation(root);benchmark();std::cout<<"PASS "<<checks<<" shared/spatial/activity checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
