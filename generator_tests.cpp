#include "generator.h"
#include "io.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

using namespace studio;

namespace {
int checks = 0;
void require(bool condition,const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool close(double a,double b,double tolerance=1e-12) { return std::abs(a-b)<=tolerance; }

NetworkGeneration small() {
    NetworkGeneration g;
    g.excitatory.count = 12; g.inhibitory.count = 4;
    g.excitatory.in_degree = g.inhibitory.in_degree = {3,0};
    g.excitatory.weight = {.4,0}; g.inhibitory.weight = {.7,0};
    return g;
}

void rejected(const Config& base,const NetworkGeneration& g,bool append,const std::string& label) {
    const auto before = serialize(base);
    GenerationSummary summary;
    summary.new_neurons = -123;
    summary.in_degrees = {91,92};
    bool failed = false;
    try { (void)generate_network(base,g,append,&summary); }
    catch (const std::invalid_argument&) { failed = true; }
    require(failed,label+" rejected");
    require(serialize(base)==before,label+" input is atomic");
    require(summary.new_neurons==-123 && summary.in_degrees==std::vector<int>({91,92}),label+" summary is atomic");
}

void graph_invariants(const Config& c,int offset,int new_count) {
    std::set<std::pair<int,int>> edges;
    for (const auto& s:c.synapses) {
        if (s.source<offset && s.target<offset) continue;
        require(s.source>=offset && s.source<offset+new_count && s.target>=offset && s.target<offset+new_count,
                "new edges remain within the new group");
        require(s.source!=s.target,"no self edges");
        require(edges.emplace(s.source,s.target).second,"no duplicate edges");
        require(std::abs(s.weight)<=1,"absolute weight within [0,1]");
        require(c.neurons[s.source].inhibitory ? s.weight<=0 : s.weight>=0,"weight sign follows source type");
        require(!s.stdp || !c.neurons[s.source].inhibitory,"STDP only on excitatory sources");
        require(s.astro==-1 && s.enabled,"new edges enabled and without astrocyte feedback");
    }
}

void test_generation() {
    const Config base = preset(0);
    auto g = small();
    GenerationSummary summary;
    const auto c = generate_network(base,g,false,&summary);
    require(validate(c).empty(),"generated config validates");
    require(c.neurons.size()==16 && c.synapses.size()==48,"exact counts and fixed in-degree");
    require(summary.new_neurons==16 && summary.new_synapses==48 && summary.excitatory_count==12 &&
            summary.inhibitory_count==4,"population summary");
    require(summary.in_degrees==std::vector<int>(16,3) && close(summary.degrees.mean,3) && close(summary.degrees.sd,0),
            "actual degree summary");
    require(c.astrocytes.empty() && c.gaps.empty() && c.noise_sigma==0,"replacement starts without astrocytes or noise");
    require(c.seed==g.seed && c.duration==g.duration && c.sample==g.sample,"replacement run settings");
    for (std::size_t i=0;i<c.neurons.size();++i) {
        const auto& n = c.neurons[i];
        require(n.model==NeuronModel::Izhikevich && n.inhibitory==(i>=12),"neuron population type");
        require(n.px>=.05 && n.px<=.95 && n.py>=.05 && n.py<=.95,"normalized positions");
        require(n.izh_a==.02 && n.izh_b==.2 && n.izh_c==-65 && n.izh_d==8,"zero SD produces exact RS parameters");
        require(n.current==0 && n.amplitude==0 && n.frequency==0 && n.noise_sigma==0,"no hidden stimulation");
        require(n.voltage==n.izh_c && n.izh_u==n.izh_b*n.voltage && n.equilibrium_u,"consistent initial state");
    }
    graph_invariants(c,0,16);
    double weight_sum=0, weight_square_sum=0;
    for (const auto& s:c.synapses) {
        const double w=std::abs(s.weight);
        require(w==(c.neurons[s.source].inhibitory?.7:.4),"weight distribution belongs to source population");
        weight_sum+=w; weight_square_sum+=w*w;
        require(s.model==SynapseModel::TsodyksMarkram && s.gain==20 && s.release==.5 &&
                s.tau_syn==.010 && s.tau_d==.050 && s.tau_f==1 && !s.stdp,"TM defaults");
    }
    const double mean=weight_sum/c.synapses.size();
    require(close(summary.absolute_weights.mean,mean) &&
            close(summary.absolute_weights.sd,std::sqrt(weight_square_sum/c.synapses.size()-mean*mean)),"actual weight statistics");
    require(serialize(c)==serialize(generate_network(base,g,false)),"same seed regenerates identical configuration");
    ++g.seed;
    const auto other=generate_network(base,g,false);
    require(c.neurons[0].px!=other.neurons[0].px || c.neurons[0].py!=other.neurons[0].py,"different seed changes positions");

    g=small(); g.synapse.stdp=true; g.synapse.stdp_rule=StdpRule::Multiplicative;
    g.synapse.model=SynapseModel::ExponentialCurrent;
    const auto learned=generate_network(base,g,false);
    graph_invariants(learned,0,16);
    for (const auto& s:learned.synapses)
        require(s.stdp==!learned.neurons[s.source].inhibitory && s.model==SynapseModel::ExponentialCurrent &&
                s.stdp_rule==StdpRule::Multiplicative,"STDP template applied only to E outputs");
    g.excitatory.in_degree={2,0}; g.inhibitory.in_degree={4,0};
    generate_network(base,g,false,&summary);
    for(int i=0;i<16;++i) require(summary.in_degrees[i]==(i<12?2:4),"degree distribution belongs to target population");
}

void test_topologies() {
    Config base;
    auto g=small();
    g.excitatory.count=3; g.inhibitory.count=2;
    g.topology=NetworkTopology::DirectedRing;
    const auto ring=generate_network(base,g,false);
    require(ring.synapses.size()==5,"directed ring edge count");
    for(const auto& s:ring.synapses) require(s.target==(s.source+1)%5,"directed ring orientation");
    graph_invariants(ring,0,5);
    g.excitatory.count=1; g.inhibitory.count=0;
    require(generate_network(base,g,false).synapses.empty(),"singleton ring has no self connection");
    g=small(); g.topology=NetworkTopology::NoConnections;
    GenerationSummary summary;
    const auto empty=generate_network(base,g,false,&summary);
    require(empty.synapses.empty() && summary.in_degrees==std::vector<int>(16,0) &&
            summary.absolute_weights.mean==0 && summary.absolute_weights.sd==0,"edge-free topology and empty statistics");

    g=small(); g.topology=NetworkTopology::NearestNeighbors;
    const auto nearest=generate_network(base,g,false);
    for(int target=0;target<16;++target) {
        std::vector<std::pair<double,int>> distances;
        for(int source=0;source<16;++source) if(source!=target) {
            const double dx=nearest.neurons[source].px-nearest.neurons[target].px;
            const double dy=nearest.neurons[source].py-nearest.neurons[target].py;
            distances.emplace_back(dx*dx+dy*dy,source);
        }
        std::sort(distances.begin(),distances.end());
        std::set<int> actual;
        for(const auto& s:nearest.synapses) if(s.target==target) actual.insert(s.source);
        require(actual==std::set<int>({distances[0].second,distances[1].second,distances[2].second}),
                "nearest topology selects exactly the closest eligible sources");
    }
    graph_invariants(nearest,0,16);
    g.excitatory.count=3; g.inhibitory.count=2;
    g.excitatory.in_degree=g.inhibitory.in_degree={99,0};
    const auto complete=generate_network(base,g,false,&summary);
    require(complete.synapses.size()==20 && summary.degree_clamped==5,"excess degree clamped and reported");
    graph_invariants(complete,0,5);
    g.excitatory.count=30; g.inhibitory.count=0;
    g.excitatory.in_degree={0,2};
    generate_network(base,g,false,&summary);
    require(summary.degree_clamped>0,"negative Gaussian degree draws clamped and reported");
}

void test_append_and_recording() {
    auto base=preset(0);
    base.noise_sigma=2; base.seed=71;
    base.parameters[na::g_L]*=1.1;
    base.neurons[0].name="Existing input";
    auto g=small(); g.excitatory.count=6; g.inhibitory.count=2;
    g.duration=7; g.sample=.002; g.seed=123;
    auto appended=generate_network(base,g,true);
    graph_invariants(appended,static_cast<int>(base.neurons.size()),8);
    require(appended.neurons.size()==base.neurons.size()+8 && appended.synapses.size()==base.synapses.size()+24,
            "append counts use offsets");
    appended.neurons.resize(base.neurons.size());
    appended.synapses.resize(base.synapses.size());
    require(appended.generation.size()==base.generation.size()+1,"append adds provenance");
    appended.generation=base.generation;
    require(serialize(appended)==serialize(base),"append preserves every old node, edge and run setting");

    base.record_all=false; base.record_neurons={1}; base.record_synapses={0};
    appended=generate_network(base,g,true);
    require(!appended.record_all && appended.record_neurons==base.record_neurons &&
            appended.record_synapses==base.record_synapses,"append preserves explicit recording selection");

    NetworkGeneration large;
    GenerationSummary summary;
    const auto c=generate_network(base,large,false,&summary);
    require(c.neurons.size()==500 && summary.recording_reduced && !c.record_all,"large generation selects compact recording");
    require(c.record_neurons==std::vector<int>({0,1,2,3,4,5,6,7}) &&
            c.record_synapses==std::vector<int>({0,1,2,3,4,5,6,7}),"recording fallback selects first eight nodes/edges");
    require(validate(c).empty() && planned_samples(c.duration,c.sample)*recorded_dimension(c)<=MaxStoredValues,
            "large recording within budget");
    require(std::abs(summary.degrees.mean-15)<.5 && std::abs(summary.absolute_weights.mean-.5)<.03,
            "Gaussian degree and weight summaries are plausible");

    base=preset(0);
    appended=generate_network(base,large,true,&summary);
    require(summary.recording_reduced && !appended.record_all && appended.duration==base.duration &&
            appended.sample==base.sample,"append reduces oversized full history while preserving time settings");
}

void test_gaussians_and_failures() {
    const Config base=preset(0);
    auto g=small();
    g.excitatory.a={.0001,.02};
    g.inhibitory.a={1,.02};
    g.excitatory.weight={0,.2};
    g.inhibitory.weight={1,.2};
    GenerationSummary summary;
    const auto bounded=generate_network(base,g,false,&summary);
    require(summary.parameter_resamples>0,"truncated Gaussian resamples are counted");
    for(const auto& n:bounded.neurons) require(n.izh_a>=.0001 && n.izh_a<=1,"Gaussian parameters remain supported");
    for(const auto& s:bounded.synapses) require(std::abs(s.weight)>0 && std::abs(s.weight)<1,
                                               "out-of-range weights are resampled, not clipped onto boundaries");
    require(serialize(bounded)==serialize(generate_network(base,g,false)),"resampling remains seed reproducible");
    g=small(); g.excitatory.count=-1; rejected(base,g,false,"negative count");
    g=small(); g.excitatory.count=g.inhibitory.count=0; rejected(base,g,false,"empty network");
    g=small(); g.excitatory.count=MaxNeurons; g.inhibitory.count=0; rejected(base,g,true,"append neuron limit");
    g=small(); g.excitatory.count=300; g.inhibitory.count=0; g.excitatory.in_degree={200,0};
    rejected(base,g,false,"edge budget (not truncated)");
    g=small(); g.excitatory.a.mean=-.02; rejected(base,g,false,"out-of-range mean");
    g=small(); g.inhibitory.b.sd=-1; rejected(base,g,false,"negative SD");
    g=small(); g.excitatory.weight.sd=std::numeric_limits<double>::quiet_NaN(); rejected(base,g,false,"NaN SD");
    g=small(); g.excitatory.a.sd=1e300; rejected(base,g,false,"bounded rejection exhaustion");
    g=small(); g.synapse.release=2; rejected(base,g,false,"invalid synaptic parameter");
    g=small(); g.synapse.model=SynapseModel::ConductanceSTP; rejected(base,g,false,"unsupported synapse model");
    g=small(); g.synapse.stdp=true; g.synapse.weight_min=.6; rejected(base,g,false,"initial STDP distribution outside bounds");
    g=small(); g.topology=static_cast<NetworkTopology>(-1); rejected(base,g,false,"unknown topology");
    g=small(); g.sample=0; rejected(base,g,false,"invalid sample interval");
    g=small(); g.duration=120; g.sample=.000025; rejected(base,g,false,"sample-row limit");
    auto badbase=base; badbase.neurons[0].voltage=1e6;
    bool invalid_base_rejected=false;
    try { (void)generate_network(badbase,small(),true); }
    catch(const std::invalid_argument&) { invalid_base_rejected=true; }
    require(invalid_base_rejected && badbase.neurons[0].voltage==1e6,"invalid existing config rejected without mutation");
    require(validate(generate_network(badbase,small(),false)).empty(),"replacement does not depend on discarded base values");

    g=small(); g.excitatory.count=1; g.inhibitory.count=0; g.topology=NetworkTopology::NoConnections;
    g.duration=.020;
    Simulator quiet(generate_network(base,g,false));
    while(!quiet.done()) require(quiet.step(),"quiet generated network integrates");
    require(quiet.error.empty() && quiet.spikes[0]==0,"generated RS network has no automatic firing");
}
} // namespace

int main(int argc,char** argv) {
    try {
        if(argc!=1 && !(argc==3 && std::string(argv[1])=="--write-example"))
            throw std::invalid_argument("Usage: network_studio_generator_tests [--write-example path]");
        test_generation();
        test_topologies();
        test_append_and_recording();
        test_gaussians_and_failures();
        std::cout<<"Generator checks passed: "<<checks<<"\n";
        if(argc==3) {
            NetworkGeneration settings;
            settings.excitatory.in_degree=settings.inhibitory.in_degree={30,3};
            GenerationSummary summary;
            const auto config=generate_network(Config{},settings,false,&summary);
            save_config(config,std::filesystem::path(argv[2]));
            std::cout<<"Saved "<<summary.new_neurons<<" neurons / "<<summary.new_synapses<<" synapses to "<<argv[2]<<"\n";
        }
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"Generator check failed: "<<e.what()<<"\n";
        return 1;
    }
}
