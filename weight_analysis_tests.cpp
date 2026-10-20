#include "weight_analysis.h"
#include "simulation_runner.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <cmath>
using namespace studio;
static size_t checks=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>static void rejects(F f,const char* message){bool caught=false;try{f();}catch(...){caught=true;}check(caught,message);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static Config fixture(){Config c;c.duration=.05;c.neurons.resize(5);for(int i=0;i<5;++i){c.neurons[i].name="N "+std::to_string(i);c.neurons[i].model=NeuronModel::Izhikevich;c.neurons[i].current=16;}for(int i=0;i<4;++i){Synapse s;s.source=i;s.target=4;s.model=SynapseModel::ExponentialCurrent;s.weight=.5;s.stdp=true;s.weight_max=4;s.gain=2;s.delay=.000037;c.synapses.push_back(s);}c.record_all=false;c.record_synapses={0};c.record_neurons={0};c.noise_sigma=.1;return c;}
int main()try{
    const auto begin=std::chrono::steady_clock::now();auto root=std::filesystem::current_path()/("weight-tests-"+std::to_string(begin.time_since_epoch().count()));std::filesystem::create_directories(root);
    auto c=fixture();auto a=initial_weights(c);a.weights={1,2,3,4};a.name="Исходный";WeightSettings options;options.bins=4;auto r=analyze_weights({&a},options);auto s=r.distributions[0].stats;
    check(s.count==4&&s.sum==10&&s.mean==2.5&&s.median==2.5&&s.minimum==1&&s.maximum==4,"analytic statistics");check(s.stddev==std::sqrt(1.25),"population deviation");check(s.learning_count==4&&s.at_upper==1&&s.at_lower==0,"learning bounds denominator");check(r.distributions[0].counts.back()==1&&std::accumulate(r.distributions[0].counts.begin(),r.distributions[0].counts.end(),0.)==4,"histogram upper endpoint");
    a.weights={0,1,2,4};options.boundary_tolerance=0;r=analyze_weights({&a},options);check(r.distributions[0].counts==std::vector<double>({1,1,1,1}),"half-open histogram bins and right endpoint");check(r.distributions[0].stats.at_lower==1&&r.distributions[0].stats.at_upper==1,"exact learning bounds");
    a.weights={.25,.25,.25,.25};r=analyze_weights({&a},options);check(r.distributions[0].stats.stddev==0&&r.edges.front()<.25&&r.edges.back()>.25,"constant weights");
    auto decimal=a;decimal.weights={.1,.1,.1};decimal.config.synapses.pop_back();r=analyze_weights({&decimal},options);check(r.distributions[0].stats.mean==.1&&r.distributions[0].stats.stddev==0,"identical decimal weights need exact zero deviation");
    auto adjacent=a;adjacent.weights[1]=std::nextafter(.25,1.);r=analyze_weights({&adjacent},options);check(std::adjacent_find(r.edges.begin(),r.edges.end(),std::greater_equal<double>())==r.edges.end(),"near-identical weights produced zero-width bins");
    options.model=1;r=analyze_weights({&a},options);check(r.distributions[0].stats.count==0&&r.difference.stats.count==0,"empty filter");options.model=0;
    auto no_edges=a;no_edges.config.synapses.clear();no_edges.config.record_synapses.clear();no_edges.weights.clear();r=analyze_weights({&no_edges},options);check(r.distributions[0].stats.count==0&&r.difference.error.empty(),"zero-edge network");
    auto near=a;near.weights={0,.000001,3.999999,4};options.boundary_tolerance=.000002;r=analyze_weights({&near},options);check(r.distributions[0].stats.at_lower==2&&r.distributions[0].stats.at_upper==2,"absolute learning boundary tolerance");options.boundary_tolerance=0;
    auto b=a;b.time=.025;b.weights={.5,.25,.1,.25};r=analyze_weights({&a,&b},options,0,1);check(r.difference.error.empty()&&r.difference.unchanged==2&&r.difference.changes[0].before==0&&r.difference.changes[0].delta==.25,"delta/order");check(r.distributions[0].counts.size()==r.distributions[1].counts.size(),"shared bin count");
    auto reorder=b;std::reverse(reorder.config.synapses.begin(),reorder.config.synapses.end());std::reverse(reorder.weights.begin(),reorder.weights.end());std::swap(reorder.config.neurons[0],reorder.config.neurons[1]);for(auto& e:reorder.config.synapses){if(e.source<2)e.source=1-e.source;if(e.target<2)e.target=1-e.target;}
    auto d=analyze_weights({&b,&reorder},options,0,1).difference;check(d.error.empty()&&d.unchanged==4&&d.stats.sum==0,"reorder matched by indices instead of identities");
    auto wrong=b;wrong.config.synapses[0].target=3;check(!compatible_weights(a,wrong).empty(),"rewired graph accepted");wrong=b;wrong.config.neurons[0].name="different";check(!compatible_weights(a,wrong).empty(),"renamed identity silently matched");
    wrong=b;wrong.config.synapses.push_back(wrong.config.synapses[0]);wrong.weights.push_back(.25);r=analyze_weights({&wrong,&wrong},options,0,1);check(!r.difference.error.empty()&&r.distributions[0].stats.count==5,"parallel-edge ambiguity not rejected");
    wrong=b;wrong.config.neurons[1].name=wrong.config.neurons[0].name;check(!compatible_weights(a,wrong).empty(),"duplicate neuron identity accepted");
    a.config.synapses[0].stdp=false;a.config.synapses[0].weight=-.5;a.weights[0]=-.5;a.config.synapses[1].model=SynapseModel::ConductanceSTP;a.config.synapses[1].reversal=-80;a.config.synapses[1].stdp=false;a.weights[1]=.5;
    options.polarity=2;options.absolute=true;r=analyze_weights({&a},options);check(r.distributions[0].stats.count==2&&r.distributions[0].stats.mean==.5&&r.distributions[0].stats.learning_count==0,"inhibitory type/absolute weights");options.absolute=false;r=analyze_weights({&a},options);check(r.distributions[0].stats.sum==0&&r.distributions[0].stats.minimum==-.5,"signed stored weights");
    options.polarity=0;options.enabled=2;r=analyze_weights({&a},options);check(r.distributions[0].stats.count==0,"disabled filter");a.config.synapses[2].enabled=false;r=analyze_weights({&a},options);check(r.distributions[0].stats.count==1,"disabled selection");options.enabled=0;
    a.steps=UINT64_MAX-4;save_weights(a,root/std::filesystem::u8path("снимок.nsweights"));auto restored=load_weights(root/std::filesystem::u8path("снимок.nsweights"));check(restored.weights==a.weights&&restored.steps==a.steps&&restored.name==a.name,"snapshot precision/Unicode/uint64");auto document=bytes(root/std::filesystem::u8path("снимок.nsweights"));write_atomic(root/"truncated.nsweights",document.substr(0,document.size()/2));rejects([&]{load_weights(root/"truncated.nsweights");},"truncated document accepted");
    auto bad=a;bad.weights[0]=NAN;rejects([&]{save_weights(bad,root/std::filesystem::u8path("снимок.nsweights"));},"NaN accepted");check(document==bytes(root/std::filesystem::u8path("снимок.nsweights")),"failed save destroyed file");
    bad=a;bad.weights[1]=-.1;rejects([&]{validate_weights(bad);},"negative conductance accepted");bad=a;bad.weights.pop_back();rejects([&]{validate_weights(bad);},"missing weight accepted");
    auto incompatible_document=document;auto version=incompatible_document.find("\"schema_version\":1");incompatible_document.replace(version,18,"\"schema_version\":9");write_atomic(root/"incompatible.nsweights",incompatible_document);rejects([&]{load_weights(root/"incompatible.nsweights");},"incompatible schema accepted");
    auto partly=b;partly.config.synapses[0].enabled=false;options={};options.enabled=1;r=analyze_weights({&b,&partly},options,0,1);check(r.distributions[0].stats.count==4&&r.distributions[1].stats.count==3&&r.difference.stats.count==3,"pair filters must intersect both snapshots");
    options={};export_weights({&b,&reorder},options,0,1,root/"comparison");check(bytes(root/"comparison_changes.csv").find("raw_delta")!=std::string::npos,"missing raw export");
    // Controller capture executes at a safe boundary, including unrecorded edges.
    SimulationRunner runner(c);RunnerCommand command;command.kind=RunnerCommand::Kind::Advance;command.value=.015;check(runner.submit(command).get().ok,"runner advance");
    command={};command.kind=RunnerCommand::Kind::SaveCheckpoint;command.path=root/"before.nscp";check(runner.submit(command).get().ok,"save before");
    command={};command.kind=RunnerCommand::Kind::CaptureWeights;auto captured=runner.submit(command).get();check(captured.ok&&captured.weights&&captured.weights->weights.size()==4&&captured.weights->time==captured.time&&captured.weights->steps==captured.steps,"incoherent runner capture");
    auto saved=captured.weights->weights;command={};command.kind=RunnerCommand::Kind::SaveCheckpoint;command.path=root/"after.nscp";check(runner.submit(command).get().ok,"save after");check(bytes(root/"before.nscp")==bytes(root/"after.nscp"),"capture changed solver/history/queue");
    command={};command.kind=RunnerCommand::Kind::Run;check(runner.submit(command).get().ok,"run for concurrent capture");command.kind=RunnerCommand::Kind::CaptureWeights;auto live=runner.submit(command).get();check(live.ok&&live.weights->time==live.time&&live.weights->steps==live.steps,"running capture not coherent");command.kind=RunnerCommand::Kind::Pause;check(runner.submit(command).get().ok,"pause");check(captured.weights->weights==saved,"captured snapshot mutated");runner.shutdown();
    auto paused=load_checkpoint(root/"before.nscp");auto state=paused->state;auto taken=capture_weights(paused->config,paused->plasticity,paused->time,paused->steps,"checkpoint");check(taken.weights==saved&&paused->state==state,"checkpoint extraction altered state");
    std::cout<<checks<<" weight checks passed; wall_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"; "<<root.u8string()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
