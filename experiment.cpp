#include "network.h"
#include "history_archive.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace studio {
std::string validate_experiment(const Config& c) {
    if(auto issue=validate_stimulators(c);!issue.empty())return issue;
    auto range=[](double v,double lo,double hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    auto indices=[&](const std::vector<int>& list){std::set<int> seen;for(int i:list)if(i<0||i>=int(c.neurons.size())||!seen.insert(i).second)return false;return true;};
    if(!range(c.space.width,1e-9,1e9)||!range(c.space.height,1e-9,1e9)||c.space.unit.empty()||c.space.unit.size()>32||c.space.unit.find_first_of("\r\n\t")!=std::string::npos)return "Invalid model space/units.";
    if(c.generation.size()>1000)return "Too many generation metadata records.";
    for(const auto& g:c.generation)if(g.settings_json.size()>65536||g.first<0||g.count<0||g.first+g.count>MaxNeurons)return "Invalid generation metadata.";
    if(c.journal_capacity<50000||c.journal_capacity>MaxEvents)return "Journal capacity must be 50000..2000000.";
    const auto& a=c.analysis;
    if(int(a.population)<0||int(a.population)>3||!indices(a.neurons)||!range(a.bin,1e-6,1e6)||!range(a.burst_window,1e-6,1e6)||
       !range(a.start_rate,1e-9,1e12)||!range(a.end_rate,0,a.start_rate)||a.min_neurons<1||a.min_neurons>MaxNeurons||
       !range(a.min_fraction,0,1)||!range(a.min_gap,0,1e6)||!range(a.burst_rate_window,1e-6,1e6)||
       a.reference_neuron<0||a.reference_neuron>=int(c.neurons.size())||!range(a.response_min,0,1e6)||!range(a.response_max,a.response_min,1e6))return "Invalid network analysis settings.";
    if(c.stages.size()>256)return "At most 256 experiment stages are supported.";
    for(const auto& s:c.stages)if(s.name.empty()||s.name.size()>120||s.name.find_first_of("\r\n\t")!=std::string::npos||
       !range(s.duration,c.dt,1e9)||!range(s.learning_rate,0,100)||!indices(s.neurons)||!range(s.amplitude,-30,100)||
       !range(s.frequency,0,200)||!range(s.width,0,1)||!range(s.first_pulse,0,s.duration))return "Invalid experiment stage.";
    return {};
}
void Simulator::append_protocol(const std::string& name,bool cleared) {
    if(experiment.protocols.size()>=4096)throw std::runtime_error("Protocol journal limit (4096 transitions) reached.");
    if(!experiment.protocols.empty())experiment.protocols.back().end=time;
    experiment.protocols.push_back({time,time,name,experiment.learning,cleared,experiment.learning_rate,config.neurons});
    for(auto s:config.stimulators){s.recipients=stimulator_recipients(config,s);s.spatial=false;experiment.protocols.back().stimulators.push_back(std::move(s));}
    if(archive)archive->protocol(*this);
}
void Simulator::set_learning(bool enabled,double rate,bool clear) {
    if(stepping||!checkpoint_boundary||!error.empty())throw std::runtime_error("Learning controls require a safe paused boundary.");
    if(!std::isfinite(rate)||rate<0||rate>100)throw std::runtime_error("Learning rate must be 0..100.");
    if(experiment.protocols.size()>=4096)throw std::runtime_error("Protocol journal is full.");
    experiment.learning=enabled;experiment.learning_rate=rate;
    if(clear)for(auto& p:plasticity)p.pre_trace=p.post_trace=p.post_slow_trace=0;
    append_protocol(clear?"Clear STDP traces":"Learning control",clear);
}
void Simulator::continue_current() {
    if(stepping||!checkpoint_boundary||!error.empty()||experiment.protocols.size()>=4096)throw std::runtime_error("Cannot continue from this boundary");
    experiment.stage_active=experiment.automatic=false;append_protocol("Continue current state");
}
void Simulator::enter_stage(size_t index,bool automatic) {
    if(stepping||!checkpoint_boundary||!error.empty())throw std::runtime_error("Stage entry requires a safe completed step.");
    const auto issue=validate_experiment(config);if(!issue.empty())throw std::runtime_error(issue);
    if(index>=config.stages.size()||experiment.protocols.size()>=4096)throw std::runtime_error("Invalid stage or full protocol journal.");
    const auto& stage=config.stages[index];
    // Round duration upward to a regular macro boundary; never shorten it.
    double boundary=std::ceil((time+stage.duration)/config.dt);
    double end=boundary*config.dt;if(end<time+stage.duration)end=(boundary+1)*config.dt;
    if(!(end>time)||!std::isfinite(end))throw std::runtime_error("Stage duration cannot advance the clock.");
    Config candidate=config;candidate.duration=std::max(candidate.duration,end);
    for(int i:stage.neurons) {
        auto& n=candidate.neurons[i];n.frequency=stage.stimulus_enabled?stage.frequency:0;
        n.amplitude=stage.amplitude;n.width=stage.width;n.start=time+stage.first_pulse;
        n.stop=end;n.pulse_intervals.clear();
    }
    for(auto id:stage.stimulator_ids)for(auto& s:candidate.stimulators)if(s.id==id) {
        s.enabled=stage.stimulus_enabled;s.signal.amplitude=stage.amplitude;s.signal.frequency=stage.frequency;
        s.signal.width=stage.width;s.signal.start=time;s.first_delay=stage.first_pulse;s.signal.stop=end;s.signal.pulse_intervals.clear();
    }
    const auto problem=validate(candidate);if(!problem.empty())throw std::runtime_error(problem);
    if(end>config.duration)extend_duration(end);
    config.neurons=std::move(candidate.neurons);
    for(auto id:stage.stimulator_ids)for(auto& l:stimulus_latches)if(l.id==id&&l.active_pulse) {
        for(auto& p:history.shared_pulses)if(p.stimulator==id&&p.pulse==l.active_pulse){p.end=time;p.complete=true;if(archive)archive->pulse(p);}l.active_pulse=0;
    }
    config.stimulators=std::move(candidate.stimulators);
    experiment.learning=stage.learning;experiment.learning_rate=stage.learning_rate;
    experiment.stage_active=true;experiment.automatic=automatic;experiment.stage_index=int(index);experiment.stage_end=end;
    if(stage.clear_traces)for(auto& p:plasticity)p.pre_trace=p.post_trace=p.post_slow_trace=0;
    append_protocol(stage.name,stage.clear_traces);
}
double Simulator::historical_input(int neuron,double at) const {
    if(neuron<0||neuron>=int(config.neurons.size()))throw std::out_of_range("Missing input neuron");
    auto it=std::upper_bound(experiment.protocols.begin(),experiment.protocols.end(),at,[](double t,const auto& p){return t<p.time;});
    if(it==experiment.protocols.begin())return external_current(config,neuron,at);
    --it;double value=external_current_with_protocol(config,it->neurons[neuron],neuron,at);
    if(it->stimulators.empty())return value;
    double shared=0;for(const auto& s:it->stimulators)if(s.enabled&&std::find(s.recipients.begin(),s.recipients.end(),neuron)!=s.recipients.end())shared+=pulse_current(stimulator_signal(s),at);
    return value+shared;
}
Config Simulator::learned_config() const {
    Config c=config;for(size_t j=0;j<c.synapses.size();++j)c.synapses[j].weight=plasticity[j].weight;return c;
}
void Simulator::fork_experiment(const std::string& origin) {
    if(stepping||!checkpoint_boundary||!error.empty()||origin.size()>4096)throw std::runtime_error("Cannot fork this snapshot.");
    // Keep the absolute clock: delays, counter noise, thresholds and phase remain exact.
    // Shared ancestry remains in the history; future execution is independent.
    experiment.origin=origin;experiment.automatic=false;
    append_protocol("Independent continuation");
}
void Simulator::trim_journals(size_t events,size_t deliveries) {
    if(!config.bounded_journals)return;
    const auto cap=config.journal_capacity;
    if(history.events.size()+events>cap) {
        size_t n=std::max(history.events.size()/2,history.events.size()+events-cap);
        if(n>history.events.size())throw std::runtime_error("One event group exceeds the journal capacity.");
        while(n<history.events.size()&&n&&history.events[n].time==history.events[n-1].time)++n;
        for(size_t i=0;i<n;++i){const auto& e=history.events[i];++(e.astro?history.dropped_releases:history.dropped_spikes)[e.index];}
        if(n)history.events_after=history.events[n-1].time;
        history.events.erase(history.events.begin(),history.events.begin()+n);
    }
    if(history.deliveries.size()+deliveries>cap) {
        size_t n=std::max(history.deliveries.size()/2,history.deliveries.size()+deliveries-cap);
        if(n>history.deliveries.size())throw std::runtime_error("One delivery group exceeds the journal capacity.");
        while(n<history.deliveries.size()&&n&&history.deliveries[n].t_arrival==history.deliveries[n-1].t_arrival)++n;
        for(size_t i=0;i<n;++i)++history.dropped_deliveries[history.deliveries[i].synapse];
        if(n)history.deliveries_after=history.deliveries[n-1].t_arrival;
        history.deliveries.erase(history.deliveries.begin(),history.deliveries.begin()+n);
    }
}
void Simulator::log_stimuli(double at) {
    STUDIO_SCOPE(journals);
    for(int i=0;i<int(config.neurons.size());++i) {
        const auto& n=config.neurons[i];const double value=pulse_current(n,at);
        if(value==0)continue;
        const bool new_protocol=experiment.protocols.back().time==at;
        if(at>0&&!new_protocol&&pulse_current(n,std::nextafter(at,-std::numeric_limits<double>::infinity()))!=0)continue;
        // Unchanged protocols at a stage boundary are continuous, not new stimuli.
        if(new_protocol&&at>0&&n.start!=at) {
            auto previous=std::lower_bound(experiment.protocols.begin(),experiment.protocols.end(),at,[](const auto& p,double t){return p.time<t;});
            if(previous!=experiment.protocols.begin()&&pulse_current((--previous)->neurons[i],std::nextafter(at,-std::numeric_limits<double>::infinity()))==value)continue;
        }
        if(history.stimuli.size()>=config.journal_capacity) {
            if(!config.bounded_journals)throw std::runtime_error("Stimulus journal full; use bounded journals for long runs.");
            size_t k=history.stimuli.size()/2;
            while(k<history.stimuli.size()&&history.stimuli[k].time==history.stimuli[k-1].time)++k;
            history.stimuli_after=history.stimuli[k-1].time;
            history.stimuli.erase(history.stimuli.begin(),history.stimuli.begin()+k);
        }
        history.stimuli.push_back({at,value,i,int(experiment.protocols.size()-1)});
        if(archive)archive->stimulus(history.stimuli.back());
    }
}
}
