#include "io.h"
#include "checkpoint_build.h"
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <set>

namespace studio {
namespace {
constexpr char Magic[]="NETWORK-STUDIO-CHECKPOINT\n";
constexpr uint64_t Schema=4;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(std::string("Checkpoint: ")+message);}
// Integrity against accidental corruption; not an authentication signature.
uint64_t checksum(const char* p,size_t n) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<n;++i){h^=static_cast<unsigned char>(p[i]);h*=1099511628211ull;}return h;
}
struct Writer {
    std::string bytes;
    void raw(const char* p,size_t n) {require(n<=MaxCheckpointBytes-bytes.size(),"file exceeds 512 MiB");bytes.append(p,n);}
    void u64(uint64_t v) {char b[8];for(int i=0;i<8;++i)b[i]=char(v>>(8*i));raw(b,8);}
    void real(double v) {static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559);uint64_t bits;std::memcpy(&bits,&v,8);u64(bits);}
    void flag(bool v){u64(v?1:0);}
    void text(const std::string& s){u64(s.size());raw(s.data(),s.size());}
    void reals(const std::vector<double>& v){u64(v.size());for(double x:v)real(x);}
    void counts(const std::vector<uint64_t>& v){u64(v.size());for(auto x:v)u64(x);}
};
struct Reader {
    const std::string& bytes;size_t pos=0,end;
    explicit Reader(const std::string& b):bytes(b),end(b.size()){}
    void need(size_t n)const {require(n<=end-pos,"truncated file");}
    uint64_t u64(){need(8);uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(static_cast<unsigned char>(bytes[pos++]))<<(8*i);return v;}
    double real(){auto bits=u64();double v;std::memcpy(&v,&bits,8);require(std::isfinite(v),"non-finite number");return v;}
    bool flag(){auto v=u64();require(v<=1,"invalid boolean");return v!=0;}
    size_t count(size_t max,size_t width=1){auto n=u64();require(n<=max && n<=(end-pos)/width,"array/string exceeds limits or remaining file");return size_t(n);}
    void expected(size_t n){require(u64()==n,"array dimension does not match graph/recording policy");}
    std::string text(size_t max){auto n=count(max);auto s=bytes.substr(pos,n);pos+=n;return s;}
    std::vector<double> reals(size_t n){expected(n);need(n*8);std::vector<double> v(n);for(auto& x:v)x=real();return v;}
    std::vector<uint64_t> counts(size_t n){expected(n);need(n*8);std::vector<uint64_t> v(n);for(auto& x:v)x=u64();return v;}
};
bool coordinate_valid(const Config& c,size_t i,double v) {
    if(!std::isfinite(v))return false;
    auto fraction=[&](){return v>=-1e-10 && v<=1+1e-10;};
    if(i<c.neurons.size()*4) {
        if(i%4==0)return v>=-200 && v<=200;
        if(c.neurons[i/4].model==NeuronModel::Izhikevich)return i%4==1 ? std::abs(v)<=1e5 : v==0;
        return fraction();
    }
    i-=c.neurons.size()*4;
    if(i<c.synapses.size()*4)return i%4<2 ? fraction() : v>=-1e-10;
    i-=c.synapses.size()*4;
    if(i%7==0 || i%7==3 || i%7==4 || i%7==6)return fraction();
    if(i%7==2)return v>=-1e-10 && v<=c.parameters[na::C_T]/(1+c.parameters[na::rho_A])+1e-10;
    return v>=-1e-10;
}
}
struct CheckpointAccess {
    static void validate_snapshot(const Simulator& s) {
        require(!s.stepping,"save is allowed only between complete step() calls");
        require(validate(s.config).empty(),"invalid configuration");
        const auto& c=s.config;const auto& h=s.history;
        for(double x:c.parameters)require(std::isfinite(x),"invalid full parameter array");
        require(s.error.size()<=65536 && s.error.find('\0')==std::string::npos,"invalid diagnostic string");
        require(s.checkpoint_boundary || !s.error.empty(),"unfinished step has no diagnostic");
        require(std::isfinite(s.time) && s.time>=0 && s.time<=c.duration,"invalid model time");
        require(s.steps<=uint64_t(std::ceil(c.duration/c.dt)),"invalid step counter");
        if(s.checkpoint_boundary) {
            require(s.steps==0 ? s.time==0 : s.time>double(s.steps-1)*c.dt && s.time<=double(s.steps)*c.dt,"step counter/time mismatch");
        } else require(s.time>=double(s.steps ? s.steps-1 : 0)*c.dt && s.time<=double(s.steps+1)*c.dt,"failed step/time mismatch");
        require(std::isfinite(s.suggested_step) && s.suggested_step>0 && s.suggested_step<=c.dt,"invalid adaptive step");
        require(s.state.size()==size_t(dimension(c)),"invalid state dimension");
        for(double x:s.state)require(std::isfinite(x),"non-finite current state");
        if(s.error.empty())require(s.physical(s.state),"state violates physical bounds");
        require(s.thresholds.size()==c.neurons.size()+c.astrocytes.size(),"invalid threshold count");
        require(s.plasticity.size()==c.synapses.size(),"invalid plasticity count");
        require(s.spikes.size()==c.neurons.size() && s.releases.size()==c.astrocytes.size(),"invalid event counters");
        for(size_t j=0;j<s.plasticity.size();++j) {
            const auto& p=s.plasticity[j];const auto& edge=c.synapses[j];
            require(std::isfinite(p.weight) && (edge.stdp ? p.weight>=edge.weight_min && p.weight<=edge.weight_max : p.weight==edge.weight),"invalid learned weight");
            for(double trace:{p.pre_trace,p.post_trace,p.post_slow_trace})require(std::isfinite(trace) && trace>=0 && trace<=MaxEvents,"invalid STDP trace");
            require(p.pre_trace<=s.spikes[edge.source] && p.post_trace<=s.spikes[edge.target] && p.post_slow_trace<=s.spikes[edge.target],"trace exceeds its event counter");
            if(!edge.enabled || !edge.stdp)require(p.pre_trace==0 && p.post_trace==0 && p.post_slow_trace==0,"inactive learning has nonzero traces");
            else if(edge.stdp_rule!=StdpRule::Triplet)require(p.post_slow_trace==0,"pair rule has a slow trace");
        }
        require(!h.time.empty() && h.time.size()<=MaxSamples && h.time.front()==0,"invalid history clock");
        require(uint64_t(h.time.size())*recorded_dimension(c)<=MaxStoredValues,"history exceeds scalar budget");
        require(s.next_sample>=1 && s.next_sample<=MaxSamples+1 && double(s.next_sample-1)*c.sample<=s.time && double(s.next_sample)*c.sample>s.time,"invalid next sample");
        uint64_t grid=0;double previous=-1;
        for(double t:h.time) {
            require(std::isfinite(t) && t>previous && t<=s.time,"unordered or future history time");
            // Off-grid terminal samples from completed, subsequently extended runs remain.
            if(t==double(grid)*c.sample)++grid;
            else require(t<double(grid)*c.sample,"missing regular sample");
            previous=t;
        }
        require(grid==s.next_sample,"history/sample clock mismatch");
        require(!s.error.empty() || s.time!=c.duration || h.time.back()==s.time,"missing terminal sample");
        require(h.values.size()==s.state.size() && h.weights.size()==c.synapses.size(),"invalid history dimensions");
        std::vector<bool> selected(s.state.size(),false),weights(c.synapses.size(),false);
        for(int i:s.recorded_state)selected[i]=true;
        for(int i:s.recorded_weights)weights[i]=true;
        for(size_t i=0;i<h.values.size();++i) {
            require(h.values[i].size()==(selected[i]?h.time.size():0),"history does not match selection");
            for(double v:h.values[i])require(coordinate_valid(c,i,v),"invalid historical state");
            if(selected[i] && h.time.back()==s.time && s.error.empty())require(h.values[i].back()==s.state[i],"last sample/current state mismatch");
        }
        for(size_t j=0;j<h.weights.size();++j) {
            const auto& edge=c.synapses[j];
            require(h.weights[j].size()==(weights[j]?h.time.size():0),"weight history does not match selection");
            for(double v:h.weights[j])require(std::isfinite(v) && (edge.stdp ? v>=edge.weight_min && v<=edge.weight_max : v==edge.weight),"invalid historical weight");
            if(weights[j] && h.time.back()==s.time && s.error.empty())require(h.weights[j].back()==s.plasticity[j].weight,"last weight/current weight mismatch");
            if(weights[j] && edge.model==SynapseModel::TsodyksMarkram) {
                const int b=syn_offset(c,int(j));
                for(size_t k=0;k<h.time.size();++k)require(h.values[b+1][k]+h.values[b+2][k]<=1+1e-10,"invalid historical TM resource sum");
            }
        }
        require(h.events.size()<=MaxEvents,"too many events");
        require(h.dropped_spikes.size()==c.neurons.size()&&h.dropped_releases.size()==c.astrocytes.size()&&h.dropped_deliveries.size()==c.synapses.size(),"invalid dropped counter dimensions");
        for(double t:{h.events_after,h.deliveries_after,h.stimuli_after})require(std::isfinite(t)&&t>=0&&t<=s.time,"invalid journal depth");
        require(c.bounded_journals||(h.events_after==0&&h.deliveries_after==0&&h.stimuli_after==0),"truncated full journal");
        for(size_t i=0;i<s.spikes.size();++i)require(h.dropped_spikes[i]<=s.spikes[i],"dropped spike counter overflow");
        for(size_t i=0;i<s.releases.size();++i)require(h.dropped_releases[i]<=s.releases[i],"dropped release counter overflow");
        for(size_t j=0;j<c.synapses.size();++j)require(h.dropped_deliveries[j]<=(c.synapses[j].enabled?s.spikes[c.synapses[j].source]:0),"dropped delivery counter overflow");
        std::vector<uint64_t> spikes=h.dropped_spikes,releases=h.dropped_releases;previous=h.events_after;
        for(const auto& event:h.events) {
            require(std::isfinite(event.time) && event.time>h.events_after && event.time>=previous && event.time<=s.time,"invalid event time");
            auto& counts=event.astro ? releases : spikes;
            require(event.index>=0 && size_t(event.index)<counts.size(),"invalid event index");
            require(counts[event.index]!=UINT64_MAX,"event counter overflow");++counts[event.index];previous=event.time;
        }
        require(spikes==s.spikes && releases==s.releases,"event log/counters mismatch");
        require(h.deliveries.size()<=MaxDeliveries && s.pending_deliveries.size()<=MaxPendingDeliveries,"delivery budget exceeded");
        require(std::is_heap(s.pending_deliveries.begin(),s.pending_deliveries.end(),DeliveryLater{}),"invalid delivery heap");
        std::vector<std::vector<double>> emissions(c.neurons.size());
        for(const auto& e:h.events)if(!e.astro)emissions[e.index].push_back(e.time);
        std::vector<uint64_t> accounted=h.dropped_deliveries;
        std::vector<double> last_emit(c.synapses.size(),-1);
        auto check_deliveries=[&](const std::vector<Delivery>& list,bool pending) {
            const Delivery* prev=nullptr;
            for(const auto& e:list) {
                require(e.synapse>=0 && size_t(e.synapse)<c.synapses.size(),"invalid delivery synapse");
                const auto& edge=c.synapses[e.synapse];
                require(edge.enabled && std::isfinite(e.t_emit) && std::isfinite(e.t_arrival) &&
                    e.t_emit>0 && e.t_emit<=s.time && e.t_arrival==e.t_emit+edge.delay,"invalid delivery timestamp or disabled edge");
                require(pending ? e.t_arrival>s.time : e.t_arrival<=s.time,"delivery in wrong past/future partition");
                require(!prev || !DeliveryLater{}(*prev,e),"unordered delivery journal");prev=&e;
                require(e.t_emit>last_emit[e.synapse],"duplicate or unordered delivery for edge");last_emit[e.synapse]=e.t_emit;
                const auto& times=emissions[edge.source];
                require(e.t_emit<=h.events_after || std::binary_search(times.begin(),times.end(),e.t_emit),"delivery has no source spike");
                require(accounted[e.synapse]!=UINT64_MAX,"delivery counter overflow");++accounted[e.synapse];
            }
        };
        check_deliveries(h.deliveries,false);
        auto pending=s.pending_deliveries;
        std::sort(pending.begin(),pending.end(),[](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);});
        check_deliveries(pending,true);
        for(size_t j=0;j<c.synapses.size();++j)
            require(accounted[j]==(c.synapses[j].enabled?s.spikes[c.synapses[j].source]:0),"lost or extra delivery");
    }
    static std::string encode(const Simulator& s) {
        validate_snapshot(s);Writer w;w.raw(Magic,sizeof(Magic)-1);w.u64(Schema);
        w.text(CheckpointAlgorithm);w.text(STUDIO_CHECKPOINT_BUILD_ID);
        w.text(serialize(s.config));w.u64(s.config.parameters.size());for(double v:s.config.parameters)w.real(v);
        w.real(s.time);w.u64(s.steps);w.u64(s.next_sample);w.real(s.suggested_step);
        w.flag(s.checkpoint_boundary);w.flag(s.done());w.text(s.error);
        w.reals(s.state);w.u64(s.plasticity.size());
        for(const auto& p:s.plasticity){w.real(p.weight);w.real(p.pre_trace);w.real(p.post_trace);w.real(p.post_slow_trace);}
        w.counts(s.spikes);w.counts(s.releases);w.u64(s.thresholds.size());for(const auto& t:s.thresholds)w.flag(t.armed);
        w.reals(s.history.time);w.u64(s.history.values.size());for(const auto& column:s.history.values)w.reals(column);
        w.u64(s.history.weights.size());for(const auto& column:s.history.weights)w.reals(column);
        w.u64(s.history.events.size());for(const auto& e:s.history.events){w.real(e.time);w.u64(uint64_t(e.index));w.flag(e.astro);}
        auto deliveries=[&](const std::vector<Delivery>& list) {
            w.u64(list.size());for(const auto& e:list){w.real(e.t_emit);w.real(e.t_arrival);w.u64(uint64_t(e.synapse));}
        };
        deliveries(s.history.deliveries);
        auto pending=s.pending_deliveries;
        std::sort(pending.begin(),pending.end(),[](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);});
        deliveries(pending);
        w.counts(s.history.dropped_spikes);w.counts(s.history.dropped_releases);w.counts(s.history.dropped_deliveries);
        w.real(s.history.events_after);w.real(s.history.deliveries_after);w.real(s.history.stimuli_after);
        w.u64(s.history.stimuli.size());for(const auto& e:s.history.stimuli){w.real(e.time);w.real(e.amplitude);w.u64(e.neuron);w.u64(e.protocol);}
        w.text(serialize_runtime(s));
        w.text(serialize_stimulus_runtime(s));w.real(s.history.shared_after);
        w.u64(s.history.shared_pulses.size());
        for(const auto& p:s.history.shared_pulses){w.u64(p.stimulator);w.u64(p.pulse);w.real(p.start);w.real(p.end);w.real(p.amplitude);w.flag(p.complete);w.u64(p.stage+1);w.u64(p.recipients.size());for(int n:p.recipients)w.u64(n);}
        w.u64(s.activity.delivery_begin);w.u64(s.activity.spike_begin);w.real(s.activity.available_after);
        w.u64(s.activity.deliveries.size());for(const auto& d:s.activity.deliveries){w.real(d.t_emit);w.real(d.t_arrival);w.u64(d.synapse);}
        w.u64(s.activity.spikes.size());for(const auto& e:s.activity.spikes){w.real(e.time);w.u64(e.index);}
        auto sum=checksum(w.bytes.data(),w.bytes.size());w.u64(sum);return std::move(w.bytes);
    }
    static std::unique_ptr<Simulator> decode(const std::string& bytes) {
        require(bytes.size()<=MaxCheckpointBytes && bytes.size()>=sizeof(Magic)+16,"invalid file size");
        Reader tail(bytes);tail.pos=bytes.size()-8;require(tail.u64()==checksum(bytes.data(),bytes.size()-8),"integrity checksum mismatch");
        Reader r(bytes);r.end-=8;
        require(bytes.compare(0,sizeof(Magic)-1,Magic)==0,"wrong document type");r.pos=sizeof(Magic)-1;
        require(r.u64()==Schema,"unsupported schema");
        require(r.text(256)==CheckpointAlgorithm,"incompatible numerical algorithm");
        require(r.text(256)==STUDIO_CHECKPOINT_BUILD_ID,"incompatible solver build; use the original build");
        auto c=deserialize(r.text(64*1024*1024));const auto json_parameters=c.parameters;
        r.expected(c.parameters.size());for(double& p:c.parameters)p=r.real();
        for(int i:SharedParameters)require(c.parameters[i]==json_parameters[i],"JSON/full parameter array mismatch");
        require(validate(c).empty(),"invalid full configuration");
        auto s=std::make_unique<Simulator>(c);require(s->error.empty(),"cannot construct simulator");
        s->time=r.real();s->steps=r.u64();s->next_sample=r.u64();s->suggested_step=r.real();
        s->checkpoint_boundary=r.flag();bool stored_done=r.flag();s->error=r.text(65536);
        s->state=r.reals(dimension(c));r.expected(c.synapses.size());
        for(auto& p:s->plasticity){p.weight=r.real();p.pre_trace=r.real();p.post_trace=r.real();p.post_slow_trace=r.real();}
        s->spikes=r.counts(c.neurons.size());s->releases=r.counts(c.astrocytes.size());
        r.expected(s->thresholds.size());for(auto& t:s->thresholds)t.armed=r.flag();
        // Validate the aggregate scalar budget BEFORE allocating history columns.
        size_t rows=r.count(MaxSamples,8);require(rows>0 && uint64_t(rows)*recorded_dimension(c)<=MaxStoredValues,"history exceeds scalar budget");
        s->history.time.resize(rows);for(double& v:s->history.time)v=r.real();
        std::vector<bool> selected(dimension(c),false),weights(c.synapses.size(),false);
        for(int i:s->recorded_state)selected[i]=true;for(int i:s->recorded_weights)weights[i]=true;
        r.expected(dimension(c));for(size_t i=0;i<s->history.values.size();++i)s->history.values[i]=r.reals(selected[i]?rows:0);
        r.expected(c.synapses.size());for(size_t i=0;i<s->history.weights.size();++i)s->history.weights[i]=r.reals(weights[i]?rows:0);
        size_t events=r.count(MaxEvents,24);s->history.events.resize(events);
        for(auto& e:s->history.events){e.time=r.real();auto index=r.u64();e.astro=r.flag();require(index<(e.astro?c.astrocytes.size():c.neurons.size()),"invalid event index");e.index=int(index);}
        auto deliveries=[&](std::vector<Delivery>& list,size_t maximum) {
            list.resize(r.count(maximum,24));
            for(auto& e:list) {e.t_emit=r.real();e.t_arrival=r.real();auto j=r.u64();
                require(j<c.synapses.size(),"invalid delivery synapse");e.synapse=int(j);}
        };
        deliveries(s->history.deliveries,MaxDeliveries);deliveries(s->pending_deliveries,MaxPendingDeliveries);
        require(std::is_sorted(s->pending_deliveries.begin(),s->pending_deliveries.end(),
            [](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);}),"noncanonical pending queue");
        std::make_heap(s->pending_deliveries.begin(),s->pending_deliveries.end(),DeliveryLater{});
        s->history.dropped_spikes=r.counts(c.neurons.size());s->history.dropped_releases=r.counts(c.astrocytes.size());s->history.dropped_deliveries=r.counts(c.synapses.size());
        s->history.events_after=r.real();s->history.deliveries_after=r.real();s->history.stimuli_after=r.real();
        s->history.stimuli.resize(r.count(c.journal_capacity,32));
        double previous_stimulus=s->history.stimuli_after;
        for(auto& e:s->history.stimuli){e.time=r.real();e.amplitude=r.real();auto neuron=r.u64(),protocol=r.u64();
            require(e.time>=previous_stimulus&&e.time<s->time&&e.amplitude!=0&&e.amplitude>=-30&&e.amplitude<=100&&neuron<c.neurons.size()&&protocol<4096,"invalid stimulus journal");
            e.neuron=int(neuron);e.protocol=int(protocol);previous_stimulus=e.time;}
        deserialize_runtime(*s,r.text(128*1024*1024));
        deserialize_stimulus_runtime(*s,r.text(64*1024*1024));s->history.shared_after=r.real();
        require(s->history.shared_after>=0&&s->history.shared_after<=s->time,"invalid shared history depth");
        s->history.shared_pulses.resize(r.count(c.journal_capacity,64));double previous_pulse=0;
        std::set<std::pair<uint64_t,uint64_t>> pulse_ids;
        for(auto& p:s->history.shared_pulses) {
            p.stimulator=r.u64();p.pulse=r.u64();p.start=r.real();p.end=r.real();p.amplitude=r.real();p.complete=r.flag();auto stage=r.u64();
            require(p.stimulator>0&&p.stimulator<c.next_stimulator_id&&p.pulse>0&&pulse_ids.insert({p.stimulator,p.pulse}).second&&p.start>=previous_pulse&&p.start<=s->time&&p.end>=p.start&&p.end<=s->time&&p.amplitude!=0&&p.amplitude>=-30&&p.amplitude<=100&&stage<=256,"invalid shared pulse");
            previous_pulse=p.start;p.stage=int(stage)-1;p.recipients.resize(r.count(c.neurons.size(),8));std::set<int> seen;
            for(auto& n:p.recipients){auto index=r.u64();require(index<c.neurons.size()&&seen.insert(int(index)).second,"invalid shared recipient");n=int(index);}
        }
        for(const auto& l:s->stimulus_latches)if(l.active_pulse)require(std::any_of(s->history.shared_pulses.begin(),s->history.shared_pulses.end(),[&](auto& p){return p.stimulator==l.id&&p.pulse==l.active_pulse&&!p.complete;}),"missing active shared pulse");
        for(const auto& p:s->history.shared_pulses){auto latch=std::find_if(s->stimulus_latches.begin(),s->stimulus_latches.end(),[&](auto& l){return l.id==p.stimulator;});require(latch!=s->stimulus_latches.end()&&p.pulse<latch->next_pulse&&(p.complete||p.pulse==latch->active_pulse),"shared pulse/latch mismatch");}
        s->activity.delivery_begin=r.u64();s->activity.spike_begin=r.u64();s->activity.available_after=r.real();
        require(s->activity.available_after>=0&&s->activity.available_after<=s->time,"invalid activity depth");
        size_t count=r.count(250000,24);require(count<=UINT64_MAX-s->activity.delivery_begin,"activity counter overflow");double previous_arrival=0;
        for(size_t k=0;k<count;++k){Delivery d;d.t_emit=r.real();d.t_arrival=r.real();auto edge=r.u64();require(edge<c.synapses.size()&&d.t_emit>0&&d.t_arrival>=previous_arrival&&d.t_arrival<=s->time&&d.t_arrival==d.t_emit+c.synapses[edge].delay,"invalid activity delivery");d.synapse=int(edge);previous_arrival=d.t_arrival;s->activity.deliveries.push_back(d);}
        count=r.count(250000,16);require(count<=UINT64_MAX-s->activity.spike_begin,"activity counter overflow");double previous_spike=0;
        for(size_t k=0;k<count;++k){const double at=r.real();auto index=r.u64();require(index<c.neurons.size()&&at>0&&at>=previous_spike&&at<=s->time,"invalid activity spike");s->activity.spikes.push_back({at,int(index),false});previous_spike=at;}
        for(const auto& e:s->history.stimuli)require(e.protocol<int(s->experiment.protocols.size())&&e.time>=s->experiment.protocols[e.protocol].time&&pulse_current(s->experiment.protocols[e.protocol].neurons[e.neuron],e.time)==e.amplitude,"stimulus/protocol mismatch");
        require(r.pos==r.end,"unexpected trailing data");require(stored_done==s->done(),"completion/diagnostic mismatch");
        validate_snapshot(*s);
        // Constructor rebuilds outgoing, selected columns, Flow work vectors and
        // threshold coordinates/values. Callbacks bind to this instance in step().
        // held_input_time is assigned before EVERY trial; it is not persistent.
        return s;
    }
};
void save_checkpoint(const Simulator& s,const std::filesystem::path& path) {write_atomic(path,CheckpointAccess::encode(s));}
std::unique_ptr<Simulator> load_checkpoint(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"cannot open file");
    const auto size=file.tellg();require(size>=0 && uint64_t(size)<=MaxCheckpointBytes,"file exceeds 512 MiB");
    std::string bytes(size_t(size),'\0');file.seekg(0);file.read(bytes.data(),std::streamsize(bytes.size()));
    require(bool(file) && file.peek()==std::char_traits<char>::eof(),"file changed or read failed");
    return CheckpointAccess::decode(bytes);
}
}
