#include "animat.h"
#include "generator.h"
namespace studio {
Config animat_example(bool spatial) {
    Config c;
    if(spatial) {
        NetworkGeneration g;g.name="Neuroanimat: spatial learning and changed environment";
        g.excitatory.count=48;g.inhibitory.count=12;g.seed=42;
        g.topology=NetworkTopology::WithinRadius;g.space={2,1,"model_unit"};g.radius=.5;
        g.excitatory.in_degree={8,1};g.inhibitory.in_degree={8,1};
        g.synapse.model=SynapseModel::TsodyksMarkram;g.synapse.stdp=true;g.synapse.delay=.001;
        g.duration=4;g.sample=.002;
        c=generate_network(c,g,false);
        c.noise_sigma=3;
    } else {
        c.name="Neuroanimat: directed two-neuron controller check";
        c.neurons.resize(2);c.synapses.resize(1);c.astrocytes.clear();c.gaps.clear();
        for(auto& n:c.neurons){n.model=NeuronModel::Izhikevich;n.amplitude=0;n.current=6;}
        c.neurons[0].name="Left";c.neurons[0].px=.2;c.neurons[0].py=.5;
        c.neurons[1].name="Right";c.neurons[1].px=.8;c.neurons[1].py=.5;
        c.synapses[0].model=SynapseModel::TsodyksMarkram;c.synapses[0].weight=.5;
        c.synapses[0].source=0;c.synapses[0].target=1;c.synapses[0].astro=-1;
        c.space={2,1,"model_unit"};c.duration=1;c.sample=.001;
    }
    c.dt=.000025;c.activity_horizon=.5;
    c.animat.enabled=true;c.animat.stimulator=1;
    c.animat.initial_x=spatial?.5:1;c.animat.initial_y=.5;c.animat.interval=.02;
    c.animat.gain=.04;c.animat.max_displacement=.04;
    c.animat.field.columns=8;c.animat.field.rows=4;c.animat.field.activity_window=.1;
    c.animat.environment.radius=.5;c.animat.environment.width=.003;
    ArenaZone safe;safe.id=1;safe.name="Safe 1 Hz";safe.width=1;safe.height=1;safe.frequency=1;
    ArenaZone high=safe;high.id=2;high.name="High 10 Hz";high.x=1;high.frequency=10;
    c.animat.environment.zones={safe,high};
    Stimulator stim;stim.id=1;stim.name="Robot spatial source";stim.spatial=true;
    stim.x=c.animat.initial_x;stim.y=c.animat.initial_y;stim.radius=.5;stim.first_delay=.05;
    stim.signal.start=0;stim.signal.stop=1000;stim.signal.frequency=spatial?1:10;
    stim.signal.width=.003;stim.signal.amplitude=30;c.stimulators={stim};c.next_stimulator_id=2;
    if(spatial) {
        for(int i=0;i<4;++i) {
            ExperimentStage s;s.name=i==0?"Observe frozen":i==1?"Learn":i==2?"Test frozen":"Changed zones / relearn";
            s.duration=1;s.learning=i==1||i==3;s.animat.marker=true;
            if(i==3){s.animat.environment=true;s.animat.value=c.animat.environment;
                s.animat.value.zones[0].frequency=10;s.animat.value.zones[1].frequency=1;}
            c.stages.push_back(s);
        }
    }
    return c;
}
}
