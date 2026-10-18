#include "analysis.h"
#include "io.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
namespace studio {
NetworkAnalysis analyze_network(const Config& c,const History& h,double until,const AnalysisSettings& a) {
    NetworkAnalysis out;out.from=std::max(h.events_after,h.stimuli_after);out.until=until;
    Config probe;probe.neurons=c.neurons;probe.analysis=a;auto issue=validate_experiment(probe);
    if(!issue.empty()||!std::isfinite(until)||until<out.from){out.error=issue.empty()?"Invalid analysis interval":issue;return out;}
    std::vector<bool> selected(c.neurons.size(),false);
    for(size_t i=0;i<selected.size();++i)selected[i]=a.population==AnalysisPopulation::All||
        (a.population==AnalysisPopulation::Excitatory&&!c.neurons[i].inhibitory)||(a.population==AnalysisPopulation::Inhibitory&&c.neurons[i].inhibitory)||
        (a.population==AnalysisPopulation::Selected&&std::find(a.neurons.begin(),a.neurons.end(),int(i))!=a.neurons.end());
    out.population_size=std::count(selected.begin(),selected.end(),true);
    if(!out.population_size||until<=out.from)return out;
    if((until-out.from)/a.bin>200000){out.error="Analysis exceeds 200000 bins; increase the analysis bin width.";return out;}
    // Pointers into the shared event journal, no trajectory/history copy.
    std::vector<const Event*> events;
    for(const auto& e:h.events)if(!e.astro&&e.index>=0&&e.index<int(selected.size())&&selected[e.index]&&e.time>=out.from&&e.time<=until)events.push_back(&e);
    // Event-driven rectangular trailing window (t-W,t]. Departures are processed
    // before arrivals at the same timestamp. Hysteresis thresholds use group Hz.
    size_t add=0,remove=0;int active=0;bool bursting=false;double start=0;
    std::vector<std::pair<double,double>> candidates;
    while(add<events.size()||remove<events.size()) {
        const double arrival=add<events.size()?events[add]->time:INFINITY;
        const double expiry=remove<add?events[remove]->time+a.burst_window:INFINITY;
        const double t=std::min(arrival,expiry);if(t>until||!std::isfinite(t))break;
        while(remove<add&&events[remove]->time+a.burst_window<=t){++remove;--active;}
        while(add<events.size()&&events[add]->time==t){++add;++active;}
        const double rate=active/a.burst_window;
        if(!bursting&&rate>=a.start_rate){bursting=true;start=events[remove]->time;}
        if(bursting&&rate<=a.end_rate){candidates.push_back({start,t});bursting=false;}
    }
    if(bursting)candidates.push_back({start,until});
    std::vector<std::pair<double,double>> merged;
    for(auto b:candidates){if(!merged.empty()&&b.first-merged.back().second<a.min_gap)merged.back().second=b.second;else merged.push_back(b);}
    for(auto b:merged) {
        std::set<int> participants;uint64_t count=0;
        auto first=std::lower_bound(events.begin(),events.end(),b.first,[](const Event* e,double t){return e->time<t;});
        for(auto it=first;it!=events.end()&&(*it)->time<=b.second;++it){++count;participants.insert((*it)->index);}
        if(participants.size()>=size_t(a.min_neurons)&&participants.size()>=std::ceil(a.min_fraction*out.population_size))
            out.bursts.push_back({b.first,b.second,count,int(participants.size()),!(bursting&&b.second==until)&&(out.from==0||b.first>=out.from+a.burst_window)});
    }
    size_t event=0,burst_begin=0,burst_end=0;
    for(uint64_t k=uint64_t(std::floor(out.from/a.bin));; ++k) {
        const double left=std::max(out.from,double(k)*a.bin),right=std::min(until,double(k+1)*a.bin);
        if(!(right>left))break;
        while(event<events.size()&&events[event]->time<left)++event;
        auto first=event;while(event<events.size()&&events[event]->time<right)++event;
        out.time.push_back(right);out.group_hz.push_back(double(event-first)/(right-left));out.neuron_hz.push_back(out.group_hz.back()/out.population_size);
        while(burst_end<out.bursts.size()&&out.bursts[burst_end].start<right)++burst_end;
        const double bleft=std::max(out.from,right-a.burst_rate_window);
        while(burst_begin<burst_end&&out.bursts[burst_begin].start<bleft)++burst_begin;
        out.burst_hz.push_back(double(burst_end-burst_begin)/(right-bleft));
        if(right==until)break;
    }
    std::vector<double> stimuli;
    for(const auto& e:h.stimuli)if(e.neuron==a.reference_neuron&&e.time>=out.from&&e.time<=until&&(stimuli.empty()||stimuli.back()!=e.time))stimuli.push_back(e.time);
    std::vector<bool> used(out.bursts.size(),false);size_t candidate=0;
    for(double t:stimuli) {
        if(t+a.response_max>until){++out.incomplete_stimuli;continue;}
        StimulusResponse response;response.stimulus=t;
        while(candidate<out.bursts.size()&&(!out.bursts[candidate].complete||out.bursts[candidate].start<t+a.response_min))++candidate;
        if(candidate<out.bursts.size()&&out.bursts[candidate].start<=t+a.response_max) {
            used[candidate]=true;response.response=out.bursts[candidate++].start;response.latency=response.response-t;
        }
        if(response.response<0)++out.missed;out.responses.push_back(response);
    }
    for(size_t j=0;j<out.bursts.size();++j)if(out.bursts[j].complete&&!used[j])++out.extra;
    out.stimulus_hz=stimuli.size()/(until-out.from);
    out.response_hz=std::count_if(out.bursts.begin(),out.bursts.end(),[](auto& b){return b.complete;})/(until-out.from);
    return out;
}
void export_analysis(const NetworkAnalysis& r,const AnalysisSettings& a,const std::filesystem::path& prefix) {
    if(!r.error.empty())throw std::runtime_error(r.error);
    auto stream=[](){std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17);return o;};
    auto save=[&](const wchar_t* suffix,const std::string& text){auto p=prefix;p+=suffix;write_atomic(p,text);};
    auto activity=stream();activity<<"time_s,group_spikes_per_s,mean_spikes_per_neuron_s,bursts_per_s\n";
    for(size_t i=0;i<r.time.size();++i)activity<<r.time[i]<<","<<r.group_hz[i]<<","<<r.neuron_hz[i]<<","<<r.burst_hz[i]<<"\n";
    save(L"_activity.csv",activity.str());auto bursts=stream();bursts<<"start_s,end_s,duration_s,spikes,participants,complete\n";
    for(const auto& b:r.bursts)bursts<<b.start<<","<<b.end<<","<<b.end-b.start<<","<<b.spikes<<","<<b.participants<<","<<b.complete<<"\n";
    save(L"_bursts.csv",bursts.str());auto responses=stream();responses<<"stimulus_s,response_s,latency_s\n";
    for(auto r:r.responses)responses<<r.stimulus<<","<<r.response<<","<<r.latency<<"\n";
    save(L"_responses.csv",responses.str());auto settings=stream();
    settings<<"available_from_s="<<r.from<<"\nuntil_s="<<r.until<<"\npopulation="<<int(a.population)<<"\nneurons=";
    for(int i:a.neurons)settings<<i<<",";
    settings<<"\nN="<<r.population_size<<"\nbin_s="<<a.bin<<"\nburst_window_s="<<a.burst_window<<"\nstart_group_hz="<<a.start_rate<<"\nend_group_hz="<<a.end_rate
        <<"\nmin_neurons="<<a.min_neurons<<"\nmin_fraction="<<a.min_fraction<<"\nmin_gap_s="<<a.min_gap<<"\nburst_rate_window_s="<<a.burst_rate_window
        <<"\nreference_neuron_0based="<<a.reference_neuron<<"\nresponse_min_s="<<a.response_min<<"\nresponse_max_s="<<a.response_max
        <<"\nstimulus_hz="<<r.stimulus_hz<<"\nburst_hz="<<r.response_hz<<"\neligible_stimuli="<<r.responses.size()<<"\nmatched="<<r.responses.size()-r.missed<<"\nresponse_fraction="<<(r.responses.empty()?0.:double(r.responses.size()-r.missed)/r.responses.size())<<"\nmissed="<<r.missed<<"\nextra="<<r.extra<<"\nincomplete_stimuli="<<r.incomplete_stimuli<<"\n";
    save(L"_analysis.txt",settings.str());
}
}
