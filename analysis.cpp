#include "analysis.h"
#include "io.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
namespace studio {
bool valid_analysis_interval(const AnalysisInterval& r) {
    return std::isfinite(r.from)&&std::isfinite(r.to)&&r.from>=0&&r.to>=0&&(r.current_end||r.to>r.from);
}
bool same_analysis_settings(const AnalysisSettings& a,const AnalysisSettings& b) {
    auto canonical=[](AnalysisSettings v){if(v.population!=AnalysisPopulation::Selected)v.neurons.clear();else std::sort(v.neurons.begin(),v.neurons.end());if(v.reference_stimulator)v.reference_neuron=0;return analysis_settings_json(v);};
    return canonical(a)==canonical(b);
}
std::string compare_analysis(const NetworkAnalysis& a,const AnalysisSettings& sa,const NetworkAnalysis& b,const AnalysisSettings& sb) {
    if(!a.error.empty()||!b.error.empty())return "Analysis error / ошибка анализа";
    if(!same_analysis_settings(sa,sb))return "Different profiles / различаются параметры профилей";
    if(a.requested_from!=b.requested_from||a.requested_until!=b.requested_until)return "Different requested intervals / различаются запрошенные интервалы";
    if(!a.full_interval||!b.full_interval||a.from!=b.from||a.until!=b.until)return "Incomplete or different available intervals / неполные или разные доступные интервалы";
    if(!a.prehistory_complete||!b.prehistory_complete)return "Insufficient detector prehistory / недостаточная предыстория детектора";
    if(a.population_size!=b.population_size)return "Different population sizes / разный размер популяции";
    return {};
}
NetworkAnalysis analyze_event_source(const Config& c,const History& h,double until,const AnalysisSettings& a,const AnalysisInterval& interval,size_t event_count,const std::function<Event(size_t)>& get_event) {
    NetworkAnalysis out;
    out.requested_from=interval.from;out.requested_until=interval.current_end?until:interval.to;
    out.available_from=std::max(h.events_after,a.reference_stimulator?h.shared_after:h.stimuli_after);out.available_until=until;
    out.from=std::max(out.available_from,out.requested_from);out.until=std::min(until,out.requested_until);
    out.full_interval=out.from==out.requested_from&&out.until==out.requested_until;
    until=out.until;
    Config probe;probe.neurons=c.neurons;probe.analysis=a;probe.stimulators=c.stimulators;probe.next_stimulator_id=c.next_stimulator_id;probe.space=c.space;auto issue=validate_experiment(probe);
    if(!issue.empty()||!valid_analysis_interval(interval)||!std::isfinite(until)||until<0){out.error=issue.empty()?"Invalid analysis interval":issue;return out;}
    std::vector<bool> selected(c.neurons.size(),false);
    for(size_t i=0;i<selected.size();++i)selected[i]=a.population==AnalysisPopulation::All||
        (a.population==AnalysisPopulation::Excitatory&&!c.neurons[i].inhibitory)||(a.population==AnalysisPopulation::Inhibitory&&c.neurons[i].inhibitory)||
        (a.population==AnalysisPopulation::Selected&&std::find(a.neurons.begin(),a.neurons.end(),int(i))!=a.neurons.end());
    out.population_size=std::count(selected.begin(),selected.end(),true);
    if(!out.population_size){out.error="Analysis population is empty";return out;}
    if(until<=out.from){out.full_interval=false;out.error="No data in requested analysis interval";return out;}
    if((until-out.from)/a.bin>200000){out.error="Analysis exceeds 200000 bins; increase the analysis bin width.";return out;}
    if(until/a.bin>4503599627370494.){out.error="Analysis bin is too small to resolve the absolute time grid.";return out;}
    struct Events {size_t count;const std::function<Event(size_t)>& get;size_t size()const{return count;}Event operator[](size_t i)const{return get(i);}} events{event_count,get_event};
    // A rolling journal has unknown hysteresis/merge state, not merely a missing W.
    // An observed silent gap of W+min_gap synchronizes both states. Use ALL retained
    // prehistory rather than restarting the detector at the requested left edge.
    out.detector_certain_from=h.events_after==0?0:until;
    if(h.events_after>0) {
        double previous=h.events_after;const double quiet=a.burst_window+a.min_gap;
        for(size_t k=0;k<events.size();++k){auto e=events[k];if(e.time-previous>=quiet){out.detector_certain_from=previous+quiet;break;}previous=e.time;}
        if(out.detector_certain_from==until&&until-previous>=quiet)out.detector_certain_from=previous+quiet;
    }
    out.prehistory_complete=out.detector_certain_from<=out.from;
    // Event-driven rectangular trailing window (t-W,t]. Departures are processed
    // before arrivals at the same timestamp. Hysteresis thresholds use group Hz.
    size_t add=0,remove=0;int active=0;bool bursting=false;double start=0;
    std::vector<std::pair<double,double>> merged;
    auto add_candidate=[&](double begin,double end){
        if(!merged.empty()&&begin-merged.back().second<a.min_gap)merged.back().second=end;
        else {if(!merged.empty()&&merged.back().first<out.from)merged.pop_back();
            if(merged.size()>=MaxEvents)throw std::runtime_error("Analysis result exceeds burst limit; use a shorter interval");
            merged.push_back({begin,end});}
    };
    while(add<events.size()||remove<events.size()) {
        const double arrival=add<events.size()?events[add].time:INFINITY;
        const double expiry=remove<add?events[remove].time+a.burst_window:INFINITY;
        const double t=std::min(arrival,expiry);if(t>until||!std::isfinite(t))break;
        while(remove<add&&events[remove].time+a.burst_window<=t){++remove;--active;}
        while(add<events.size()&&events[add].time==t){++add;++active;}
        const double rate=active/a.burst_window;
        if(!bursting&&rate>=a.start_rate){bursting=true;start=events[remove].time;}
        if(bursting&&rate<=a.end_rate){add_candidate(start,t);bursting=false;}
    }
    if(bursting)add_candidate(start,until);
    for(auto b:merged) {
        if(b.first<out.from||b.first>=until)continue; // Start ownership: [from,to).
        std::set<int> participants;uint64_t count=0;
        size_t lo=0,hi=events.size();while(lo<hi){auto m=lo+(hi-lo)/2;if(events[m].time<b.first)lo=m+1;else hi=m;}
        for(size_t k=lo;k<events.size()&&events[k].time<=b.second;++k){++count;participants.insert(events[k].index);}
        if(participants.size()>=size_t(a.min_neurons)&&participants.size()>=std::ceil(a.min_fraction*out.population_size))
            out.bursts.push_back({b.first,b.second,count,int(participants.size()),!(bursting&&b.second==until)&&b.first>=out.detector_certain_from&&b.second+a.min_gap+a.burst_window<=until});
    }
    size_t event=0,burst_begin=0,burst_end=0;
    for(uint64_t k=uint64_t(std::floor(out.from/a.bin));; ++k) {
        const double left=std::max(out.from,double(k)*a.bin),right=std::min(until,double(k+1)*a.bin);
        if(!(right>left))break;
        while(event<events.size()&&events[event].time<left)++event;
        auto first=event;while(event<events.size()&&events[event].time<right)++event;
        out.time.push_back(right);out.group_hz.push_back(double(event-first)/(right-left));out.neuron_hz.push_back(out.group_hz.back()/out.population_size);
        while(burst_end<out.bursts.size()&&out.bursts[burst_end].start<right)++burst_end;
        const double bleft=std::max(out.from,right-a.burst_rate_window);
        while(burst_begin<burst_end&&out.bursts[burst_begin].start<bleft)++burst_begin;
        out.burst_hz.push_back(double(burst_end-burst_begin)/(right-bleft));
        if(right==until)break;
    }
    std::vector<double> stimuli;
    if(a.reference_stimulator) {
        for(const auto& e:h.shared_pulses)if(e.stimulator==a.reference_stimulator&&e.start>=out.from&&e.start<until)stimuli.push_back(e.start);
    }else for(const auto& e:h.stimuli)if(e.neuron==a.reference_neuron&&e.time>=out.from&&e.time<until&&(stimuli.empty()||stimuli.back()!=e.time))stimuli.push_back(e.time);
    std::vector<bool> used(out.bursts.size(),false);size_t candidate=0;
    for(double t:stimuli) {
        if(t+a.response_max>=until||t+a.response_min<out.detector_certain_from){++out.incomplete_stimuli;continue;}
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
NetworkAnalysis analyze_network(const Config& c,const History& h,double until,const AnalysisSettings& a,const AnalysisInterval& interval) {
    std::vector<const Event*> selected;
    const double end=interval.current_end?until:std::min(until,interval.to);
    for(const auto& e:h.events)if(!e.astro&&e.index>=0&&e.index<int(c.neurons.size())&&e.time>=h.events_after&&e.time<end) {
        const auto& n=c.neurons[e.index];
        if(a.population==AnalysisPopulation::All||(a.population==AnalysisPopulation::Excitatory&&!n.inhibitory)||(a.population==AnalysisPopulation::Inhibitory&&n.inhibitory)||
            (a.population==AnalysisPopulation::Selected&&std::find(a.neurons.begin(),a.neurons.end(),e.index)!=a.neurons.end()))selected.push_back(&e);
    }
    return analyze_event_source(c,h,until,a,interval,selected.size(),[&](size_t i){return *selected[i];});
}
void export_analysis(const NetworkAnalysis& r,const AnalysisSettings& a,const std::filesystem::path& prefix,const std::string& profile_name) {
    if(!r.error.empty())throw std::runtime_error(r.error);
    const auto metadata=analysis_result_json(r,{profile_name,a}); // Validate before writing any output.
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
        <<"\nreference_stimulator="<<a.reference_stimulator<<"\nreference_neuron_0based="<<a.reference_neuron<<"\nresponse_min_s="<<a.response_min<<"\nresponse_max_s="<<a.response_max
        <<"\nstimulus_hz="<<r.stimulus_hz<<"\nburst_hz="<<r.response_hz<<"\neligible_stimuli="<<r.responses.size()<<"\nmatched="<<r.responses.size()-r.missed<<"\nresponse_fraction="<<(r.responses.empty()?0.:double(r.responses.size()-r.missed)/r.responses.size())<<"\nmissed="<<r.missed<<"\nextra="<<r.extra<<"\nincomplete_stimuli="<<r.incomplete_stimuli<<"\n";
    settings<<"requested_from_s="<<r.requested_from<<"\nrequested_until_s="<<r.requested_until
        <<"\nfull_interval="<<r.full_interval<<"\nprehistory_complete="<<r.prehistory_complete<<"\ndetector_certain_from_s="<<r.detector_certain_from
        <<"\nprofile="<<profile_name<<"\nalgorithm="<<AnalysisAlgorithm<<"\n";
    save(L"_analysis.txt",settings.str());
    save(L"_analysis.json",metadata);
}
}
