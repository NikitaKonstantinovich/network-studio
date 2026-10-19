#include "simulation_view.h"
#include <algorithm>
#include <stdexcept>

namespace studio {
namespace {
double input(const Config& config,const ExperimentRuntime& experiment,int neuron,double at) {
    // Same display reconstruction and arithmetic order as Simulator::historical_input.
    if(neuron<0||neuron>=int(config.neurons.size()))throw std::out_of_range("Missing input neuron");
    auto it=std::upper_bound(experiment.protocols.begin(),experiment.protocols.end(),at,[](double t,const auto& p){return t<p.time;});
    if(it==experiment.protocols.begin())return external_current(config,neuron,at);
    --it;double value=external_current_with_protocol(config,it->neurons[neuron],neuron,at);
    if(it->stimulators.empty())return value;
    double shared=0;for(const auto& s:it->stimulators)if(s.enabled&&std::find(s.recipients.begin(),s.recipients.end(),neuron)!=s.recipients.end())shared+=pulse_current(stimulator_signal(s),at);
    return value+shared;
}
template<class T,class Time> void apply_journal(std::vector<T>& out,const JournalDelta<T>& delta,Time at) {
    auto keep=std::lower_bound(out.begin(),out.end(),delta.keep_from,[&](const T& v,double t){return at(v)<t;});
    out.erase(out.begin(),keep);
    auto tail=std::lower_bound(out.begin(),out.end(),delta.replace_from,[&](const T& v,double t){return at(v)<t;});
    out.erase(tail,out.end());out.insert(out.end(),delta.entries.begin(),delta.entries.end());
}
template<class T> void apply_activity(std::deque<T>& out,uint64_t old_begin,const ActivityDelta<T>& delta) {
    const auto removed=std::min<uint64_t>(out.size(),delta.begin>old_begin?delta.begin-old_begin:0);
    for(uint64_t i=0;i<removed;++i)out.pop_front();
    // Packets are sequential, but a capped window can skip all old entries.
    if(delta.append_from==delta.begin)out.clear();
    out.insert(out.end(),delta.entries.begin(),delta.entries.end());
}
}
bool SimulationView::apply(const SimulationSnapshot& s) {
    if(!s.config||!s.experiment||!s.shared_pulses||!s.stimulus_commands||!s.stimulus_changes)return false;
    if(s.generation<generation||s.sequence<=sequence&&s.generation==generation)return false;
    const bool replacement=s.generation!=generation;
    if(s.base_sequence!=(replacement?0:sequence))return false;
    if(s.samples.end<s.samples.append_from||s.samples.append_from<s.samples.begin||s.samples.time.size()!=s.samples.end-s.samples.append_from)return false;
    for(const auto& col:s.samples.columns)if(col.values.size()!=s.samples.time.size()||col.index>=(col.weight?s.samples.weight_columns:s.samples.value_columns))return false;
    if(replacement) {
        history=History{};activity=RecentActivity{};sample_begin=s.samples.begin;
        config_version_.reset();experiment_version_.reset();shared_version_.reset();commands_version_.reset();changes_version_.reset();
    }
    if(config_version_!=s.config){config=*s.config;config_version_=s.config;}
    if(experiment_version_!=s.experiment){experiment=*s.experiment;experiment_version_=s.experiment;}
    state=s.state;plasticity=s.plasticity;spikes=s.spikes;releases=s.releases;
    pending_deliveries=s.pending_deliveries;steps=s.steps;time=s.time;error=s.error;done_=s.done;
    generation=s.generation;sequence=s.sequence;last_command_id=s.last_command_id;truncated=s.truncated;metrics=s.metrics;
    history.values.resize(s.samples.value_columns);history.weights.resize(s.samples.weight_columns);
    const auto removed=size_t(std::min<uint64_t>(history.time.size(),s.samples.begin>sample_begin?s.samples.begin-sample_begin:0));
    history.time.erase(history.time.begin(),history.time.begin()+removed);
    for(auto& col:history.values)col.erase(col.begin(),col.begin()+std::min(removed,col.size()));
    for(auto& col:history.weights)col.erase(col.begin(),col.begin()+std::min(removed,col.size()));
    if(s.samples.append_from==s.samples.begin) {
        history.time.clear();for(auto& col:history.values)col.clear();for(auto& col:history.weights)col.clear();
    }
    sample_begin=s.samples.begin;history.time.insert(history.time.end(),s.samples.time.begin(),s.samples.time.end());
    for(const auto& col:s.samples.columns){auto& target=(col.weight?history.weights:history.values)[col.index];target.insert(target.end(),col.values.begin(),col.values.end());}
    apply_journal(history.events,s.events,[](const Event& e){return e.time;});history.events_after=s.events.available_after;
    apply_journal(history.deliveries,s.deliveries,[](const Delivery& e){return e.t_arrival;});history.deliveries_after=s.deliveries.available_after;
    apply_journal(history.stimuli,s.stimuli,[](const StimulusEvent& e){return e.time;});history.stimuli_after=s.stimuli.available_after;
    if(shared_version_!=s.shared_pulses){history.shared_pulses=*s.shared_pulses;shared_version_=s.shared_pulses;}
    history.shared_after=s.shared_after;
    apply_activity(activity.deliveries,activity.delivery_begin,s.activity_deliveries);activity.delivery_begin=s.activity_deliveries.begin;
    apply_activity(activity.spikes,activity.spike_begin,s.activity_spikes);activity.spike_begin=s.activity_spikes.begin;
    activity.available_after=s.activity_available_after;
    if(commands_version_!=s.stimulus_commands){stimulus_commands=*s.stimulus_commands;commands_version_=s.stimulus_commands;}
    if(changes_version_!=s.stimulus_changes){stimulus_changes=*s.stimulus_changes;changes_version_=s.stimulus_changes;}
    return true;
}
double SimulationView::historical_input(int neuron,double at) const{return input(config,experiment,neuron,at);}
SimulationReadView::SimulationReadView(const Simulator& s):config(s.config),state(s.state),history(s.history),plasticity(s.plasticity),spikes(s.spikes),releases(s.releases),experiment(s.experiment),activity(s.activity),stimulus_commands(s.stimulus_commands),stimulus_changes(s.stimulus_changes),error(s.error),pending_deliveries{s.pending_deliveries.size()},steps(s.steps),time(s.time),identity(&s),done_(s.done()){}
SimulationReadView::SimulationReadView(const SimulationView& s):config(s.config),state(s.state),history(s.history),plasticity(s.plasticity),spikes(s.spikes),releases(s.releases),experiment(s.experiment),activity(s.activity),stimulus_commands(s.stimulus_commands),stimulus_changes(s.stimulus_changes),error(s.error),pending_deliveries(s.pending_deliveries),steps(s.steps),generation(s.generation),time(s.time),identity(&s),truncated(s.truncated),done_(s.done()){}
double SimulationReadView::historical_input(int neuron,double at) const{return input(config,experiment,neuron,at);}
}
