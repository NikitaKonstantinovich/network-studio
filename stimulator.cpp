#include "network.h"
#include "history_archive.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace studio {
namespace {
bool receives(const Config& c,const Stimulator& s,int i) {
    if(!s.spatial)return std::find(s.recipients.begin(),s.recipients.end(),i)!=s.recipients.end();
    const auto& n=c.neurons[i];
    if((s.filter==StimulatorFilter::Excitatory&&n.inhibitory)||(s.filter==StimulatorFilter::Inhibitory&&!n.inhibitory))return false;
    return std::hypot(n.px*c.space.width-s.x,n.py*c.space.height-s.y)<=s.radius;
}
}
Neuron stimulator_signal(const Stimulator& s) {auto n=s.signal;n.start+=s.first_delay;if(!s.enabled)n.frequency=0;return n;}
std::vector<int> stimulator_recipients(const Config& c,const Stimulator& s) {
    std::vector<int> out;for(int i=0;i<int(c.neurons.size());++i)if(receives(c,s,i))out.push_back(i);return out;
}
double shared_current(const Config& c,int neuron,double time) {
    double result=0;for(const auto& s:c.stimulators)if(s.enabled&&receives(c,s,neuron))result+=pulse_current(stimulator_signal(s),time);return result;
}
std::string validate_stimulators(const Config& c) {
    auto range=[](double x,double a,double b){return std::isfinite(x)&&x>=a&&x<=b;};
    if(c.stimulators.size()>64||!c.next_stimulator_id||!range(c.activity_horizon,.001,10))return "Invalid stimulator count/ID or activity horizon (0.001..10 s).";
    uint64_t previous=0;
    for(const auto& s:c.stimulators) {
        const auto& n=s.signal;
        if(!s.id||s.id<=previous||s.id>=c.next_stimulator_id||s.name.empty()||s.name.size()>120||s.name.find_first_of("\r\n\t")!=s.name.npos||
           !range(s.x,0,c.space.width)||!range(s.y,0,c.space.height)||!range(s.radius,0,1e9)||int(s.filter)<0||int(s.filter)>2||
           !range(n.start,0,1e12)||!range(n.stop,n.start,1e12)||!range(s.first_delay,0,n.stop-n.start)||!range(n.frequency,0,200)||
           !range(n.amplitude,-30,100)||!range(n.width,0,1)||n.pulse_intervals.size()>64)return "Invalid shared stimulator parameters.";
        previous=s.id;std::set<int> seen;
        for(int i:s.recipients)if(i<0||i>=int(c.neurons.size())||!seen.insert(i).second)return "Invalid stimulator recipients.";
        double end=0;for(auto p:n.pulse_intervals) {
            if(!range(p.start,end,1e12)||!range(p.stop,p.start,1e12)||p.stop==p.start||(n.frequency>0&&p.stop>1/n.frequency))return "Invalid shared pulse table.";
            end=p.stop;
        }
    }
    for(const auto& stage:c.stages) {std::set<uint64_t> seen;for(auto id:stage.stimulator_ids)
        if(!seen.insert(id).second||std::none_of(c.stimulators.begin(),c.stimulators.end(),[&](const auto& s){return s.id==id;}))return "Stage refers to a missing/duplicate stimulator.";}
    if(c.analysis.reference_stimulator&&std::none_of(c.stimulators.begin(),c.stimulators.end(),[&](const auto& s){return s.id==c.analysis.reference_stimulator;}))return "Analysis refers to a missing stimulator.";
    return {};
}
double next_stimulator_pulse(const Stimulator& s,double time) {
    const auto n=stimulator_signal(s);
    double next=std::numeric_limits<double>::infinity();
    if(!std::isfinite(time)||!s.enabled||n.frequency<=0||n.amplitude==0||time>=n.stop)return next;
    const double period=1/n.frequency;
    auto onset=[&](double at){
        if(at>=time&&at<n.stop&&at<next&&pulse_current(n,at)!=0&&
           pulse_current(n,std::nextafter(at,-INFINITY))==0)next=at;
    };
    onset(n.start);
    const double cycle=std::floor((std::max(time,n.start)-n.start)/period);
    for(int neighbor=-1;neighbor<=2;++neighbor){
        const double k=cycle+neighbor;if(k<0)continue;
        if(n.pulse_intervals.empty())onset(n.start+k*period);
        else for(const auto& p:n.pulse_intervals)onset(n.start+k*period+p.start);
    }
    return next;
}

double next_stimulator_pulse(Stimulator value,const std::vector<StimulatorCommand>& commands,double time) {
    // Forecast acknowledged commands in the solver's (time, sequence) order.
    // Intermediate values in one group cannot emit pulses.
    double from=time;
    for(size_t i=0;i<commands.size();){
        const double at=commands[i].time;size_t end=i;
        while(end<commands.size()&&commands[end].time==at)++end;
        bool found=false,restart=false;Stimulator after=value;
        for(size_t j=i;j<end;++j)if(commands[j].value.id==value.id){found=true;after=commands[j].value;restart|=commands[j].restart;}
        i=end;if(!found||at<time)continue;
        const double next=next_stimulator_pulse(value,from);if(next<at)return next;
        const bool was_on=pulse_current(stimulator_signal(value),std::nextafter(at,-INFINITY))!=0;
        value=std::move(after);
        if(pulse_current(stimulator_signal(value),at)!=0&&(!was_on||restart))return at;
        from=std::nextafter(at,INFINITY);
    }
    return next_stimulator_pulse(value,from);
}

double Simulator::schedule_stimulator(Stimulator value,double requested,bool restart,bool controller) {
    if(stepping||!checkpoint_boundary||!error.empty()||!std::isfinite(requested)||requested<time||requested>1e12)throw std::runtime_error("Stimulator command requires a safe boundary and finite future time.");
    if(config.animat.enabled&&value.id==config.animat.stimulator&&!controller)throw std::runtime_error("Animat owns this stimulator; use the animat environment controls.");
    if(stimulus_commands.size()+stimulus_changes.size()>=4096||next_stimulus_command==UINT64_MAX||config.next_stimulator_id==UINT64_MAX)throw std::runtime_error("Stimulator command/ID budget exceeded.");
    auto old=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](const auto& s){return s.id==value.id;});
    if(value.id&&old==config.stimulators.end())throw std::runtime_error("Unknown stimulator ID.");
    if(!value.id)value.id=config.next_stimulator_id;
    double at=requested;
    if(!restart&&old!=config.stimulators.end()&&value.signal.frequency!=old->signal.frequency) {
        if(value.signal.start!=old->signal.start||value.first_delay!=old->first_delay)
            throw std::runtime_error("Частота вместе с началом/задержкой: выберите «Применить с новой фазой». Команда не принята.");
        const auto n=stimulator_signal(*old);
        const double first=n.start+(n.pulse_intervals.empty()?0:n.pulse_intervals.front().start);
        if(old->enabled&&value.enabled&&n.frequency>0&&value.signal.frequency>0&&at>=first&&at<n.stop) {
            const double period=1/n.frequency,k=std::floor((at-n.start)/period);
            double boundary=n.stop;
            // Absolute edges, including neighbours of a rounded quotient.
            // One ULP after an edge is after it; there is no fuzzy equality.
            for(int neighbor=-1;neighbor<=2;++neighbor)if(k+neighbor>=0){
                const double edge=n.start+(k+neighbor)*period;
                if(edge>=requested&&edge<boundary)boundary=edge;
            }
            at=boundary;value.signal.start=at;value.first_delay=0;
        }
        // Before the first pulse, or for disabled/ended trains, keep the
        // absolute start and first_delay. Never pull a future first pulse
        // back to requested. Disabling is immediate.
    }
    if(restart)value.signal.start=at;
    Config check=config;auto target=std::find_if(check.stimulators.begin(),check.stimulators.end(),[&](const auto& s){return s.id==value.id;});
    if(target==check.stimulators.end())check.stimulators.push_back(value);else *target=value;
    check.next_stimulator_id=std::max(check.next_stimulator_id,value.id+1);
    auto issue=validate_stimulators(check);if(!issue.empty())throw std::runtime_error(issue);
    if(experiment.protocols.size()>=4096)throw std::runtime_error("Protocol journal is full.");
    config.next_stimulator_id=check.next_stimulator_id;
    stimulus_commands.push_back({next_stimulus_command++,at,std::move(value),restart,controller});
    std::sort(stimulus_commands.begin(),stimulus_commands.end(),[](const auto& a,const auto& b){return a.time!=b.time?a.time<b.time:a.sequence<b.sequence;});
    if(at==time){apply_stimulus_commands();synchronize_stimulators(true);}else if(archive)archive->protocol(*this);return at;
}
void Simulator::replay_stimulators(std::vector<StimulatorCommand> commands) {
    if(stepping||!checkpoint_boundary||!error.empty())throw std::runtime_error("Replay requires a safe boundary.");
    if(stimulus_commands.size()+stimulus_changes.size()+commands.size()>4096||experiment.protocols.size()+stimulus_commands.size()+commands.size()>4096)throw std::runtime_error("Replay exceeds command/protocol budget.");
    for(const auto& v:commands)if(v.controller||(config.animat.enabled&&v.value.id==config.animat.stimulator))throw std::runtime_error("Replay robot actions through the animat protocol, not raw source commands");
    std::set<uint64_t> ids;for(const auto& v:stimulus_commands)ids.insert(v.sequence);for(const auto& v:stimulus_changes)ids.insert(v.sequence);
    auto combined=stimulus_commands;uint64_t next=next_stimulus_command,next_id=config.next_stimulator_id;
    for(auto& v:commands){if(!v.sequence||v.sequence==UINT64_MAX||!ids.insert(v.sequence).second||!std::isfinite(v.time)||v.time<time||v.time>1e12||!v.value.id||v.value.id==UINT64_MAX)throw std::runtime_error("Invalid replay command/time/ID.");next=std::max(next,v.sequence+1);next_id=std::max(next_id,v.value.id+1);combined.push_back(v);}
    std::sort(combined.begin(),combined.end(),[](auto& a,auto& b){return a.time!=b.time?a.time<b.time:a.sequence<b.sequence;});
    Config check=config;check.next_stimulator_id=next_id;
    for(const auto& v:combined){auto it=std::find_if(check.stimulators.begin(),check.stimulators.end(),[&](auto& s){return s.id==v.value.id;});if(it==check.stimulators.end())check.stimulators.push_back(v.value);else *it=v.value;
        std::sort(check.stimulators.begin(),check.stimulators.end(),[](auto& a,auto& b){return a.id<b.id;});auto issue=validate_stimulators(check);if(!issue.empty())throw std::runtime_error(issue);}
    stimulus_commands=std::move(combined);next_stimulus_command=next;config.next_stimulator_id=next_id;
    apply_stimulus_commands();synchronize_stimulators(true);if(archive)archive->protocol(*this);
}

double Simulator::move_stimulator(uint64_t id,double x,double y,double at) {
    auto it=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==id;});
    if(it==config.stimulators.end())throw std::runtime_error("Unknown stimulator ID.");
    auto value=*it;value.x=x;value.y=y;return schedule_stimulator(value,at,false);
}
void Simulator::apply_stimulus_commands() {
    while(!stimulus_commands.empty()&&stimulus_commands.front().time<=time) {
        auto command=stimulus_commands.front();
        if(command.controller){command.value.x=animat.x;command.value.y=animat.y;const auto& e=animat.environments.back().value;command.value.radius=e.radius;command.value.signal.amplitude=e.amplitude;command.value.signal.width=e.width;command.value.enabled=e.enabled;}
        if(command.time!=time)throw std::runtime_error("Missed a scheduled stimulator boundary.");
        if(experiment.protocols.size()>=4096)throw std::runtime_error("Protocol journal is full.");
        if(command.restart)for(auto& l:stimulus_latches)if(l.id==command.value.id&&l.active_pulse) {
            for(auto& p:history.shared_pulses)if(p.stimulator==l.id&&p.pulse==l.active_pulse){p.end=time;p.complete=true;if(archive)archive->pulse(p);}
            l.active_pulse=0;
        }
        auto it=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==command.value.id;});
        if(it==config.stimulators.end())config.stimulators.push_back(command.value);else *it=command.value;
        std::sort(config.stimulators.begin(),config.stimulators.end(),[](auto& a,auto& b){return a.id<b.id;});
        stimulus_commands.erase(stimulus_commands.begin());
        if(command.controller){if(!animat_updating){AnimatRow row;row.kind=AnimatRowKind::Stimulator;row.x0=animat.x;row.y0=animat.y;animat_row(row);}}
        else {stimulus_changes.push_back({command.sequence,time,experiment.stage_index,command.value,stimulator_recipients(config,command.value),command.restart});append_protocol("Stimulator "+std::to_string(command.value.id));}
    }
}
void Simulator::remove_stimulator(uint64_t id) {
    if(config.animat.enabled&&id==config.animat.stimulator)throw std::runtime_error("Animat owns this source; reset/configure the experiment first");
    if(stepping||!checkpoint_boundary||!error.empty()||experiment.protocols.size()>=4095)throw std::runtime_error("Cannot remove stimulator here.");
    auto it=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==id;});if(it==config.stimulators.end())return;
    auto disabled=*it;disabled.enabled=false;schedule_stimulator(disabled,time);
    config.stimulators.erase(std::remove_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==id;}),config.stimulators.end());
    stimulus_commands.erase(std::remove_if(stimulus_commands.begin(),stimulus_commands.end(),[&](auto& c){return c.value.id==id;}),stimulus_commands.end());
    for(auto& s:config.stages)s.stimulator_ids.erase(std::remove(s.stimulator_ids.begin(),s.stimulator_ids.end(),id),s.stimulator_ids.end());
    if(config.analysis.reference_stimulator==id)config.analysis.reference_stimulator=0;
    append_protocol("Remove stimulator "+std::to_string(id));
}
void Simulator::move_neuron(int index,double x,double y) {
    if(config.animat.enabled)throw std::runtime_error("Animat: moving network geometry requires a reset");
    if(stepping||!checkpoint_boundary||!error.empty()||index<0||index>=int(config.neurons.size())||!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1||experiment.protocols.size()>=4096)throw std::runtime_error("Invalid neuron movement boundary/position.");
    config.neurons[index].px=x;config.neurons[index].py=y;
    if(!config.stimulators.empty())append_protocol("Neuron position/recipient update");
}
void Simulator::synchronize_stimulators(bool close_only) {
    for(const auto& s:config.stimulators) {
        auto it=std::find_if(stimulus_latches.begin(),stimulus_latches.end(),[&](auto& l){return l.id==s.id;});
        if(it==stimulus_latches.end()){stimulus_latches.push_back({s.id,1,0});it=stimulus_latches.end()-1;}
        const double value=pulse_current(stimulator_signal(s),time);
        if(it->active_pulse&&value==0) {
            for(auto p=history.shared_pulses.rbegin();p!=history.shared_pulses.rend();++p)if(p->stimulator==s.id&&p->pulse==it->active_pulse){p->end=time;p->complete=true;if(archive)archive->pulse(*p);break;}
            it->active_pulse=0;
        }
        if(!it->active_pulse&&value!=0&&!close_only) {
            if(it->next_pulse==UINT64_MAX)throw std::runtime_error("Stimulator pulse ID overflow.");
            if(history.shared_pulses.size()>=config.journal_capacity) {
                if(!config.bounded_journals)throw std::runtime_error("Shared stimulus journal full; select bounded journals.");
                size_t removed=0;const auto target=history.shared_pulses.size()/2;
                history.shared_pulses.erase(std::remove_if(history.shared_pulses.begin(),history.shared_pulses.end(),[&](const auto& p){
                    if(p.complete&&removed<target){++removed;history.shared_after=std::max(history.shared_after,std::nextafter(p.start,INFINITY));return true;}return false;
                }),history.shared_pulses.end());
                if(!removed)throw std::runtime_error("Too many open shared stimulus records.");
            }
            it->active_pulse=it->next_pulse++;
            history.shared_pulses.push_back({s.id,it->active_pulse,time,time,value,false,experiment.stage_index,stimulator_recipients(config,s)});
            if(archive)archive->pulse(history.shared_pulses.back());
        }
    }
}
void Simulator::capture_activity(const std::vector<Delivery>& delivered,const std::vector<bool>& fired,double at) {
    for(const auto& d:delivered)activity.deliveries.push_back(d);
    for(int i=0;i<int(fired.size());++i)if(fired[i])activity.spikes.push_back({at,i,false});
    const double cutoff=std::max(0.,at-config.activity_horizon);
    while(!activity.deliveries.empty()&&activity.deliveries.front().t_arrival<=cutoff){activity.deliveries.pop_front();++activity.delivery_begin;}
    while(!activity.spikes.empty()&&activity.spikes.front().time<=cutoff){activity.spikes.pop_front();++activity.spike_begin;}
    activity.available_after=std::max(activity.available_after,cutoff);
    constexpr size_t budget=250000;
    while(activity.deliveries.size()>budget){activity.available_after=std::max(activity.available_after,activity.deliveries.front().t_arrival);activity.deliveries.pop_front();++activity.delivery_begin;}
    while(activity.spikes.size()>budget){activity.available_after=std::max(activity.available_after,activity.spikes.front().time);activity.spikes.pop_front();++activity.spike_begin;}
}
}
