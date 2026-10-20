#include "activity_field.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace studio {
void ActivityField::update(const Config& c,const std::vector<StdpState>& weights,const RecentActivity& h,double at,const void* identity,uint64_t revision,const TopologyViewState& v,WeightField& geometry) {
    const auto begin=std::chrono::steady_clock::now();
    if(!valid_topology_view(v))throw std::runtime_error("Invalid activity field settings");
    const bool reset=owner!=identity||generation!=revision||at<time||counts.size()!=c.synapses.size()||arrivals.size()!=c.neurons.size()||mode!=v.activity_mode||
        window!=v.activity_window||response_min!=v.response_min||response_max!=v.response_max||delivery_cursor<h.delivery_begin||spike_cursor<h.spike_begin||
        delivery_cursor>h.delivery_begin+h.deliveries.size()||spike_cursor>h.spike_begin+h.spikes.size();
    if(reset) {
        arrivals.assign(c.neurons.size(),{});counts.assign(c.synapses.size(),0);delivery_cursor=h.delivery_begin;spike_cursor=h.spike_begin;
        owner=identity;generation=revision;mode=v.activity_mode;window=v.activity_window;response_min=v.response_min;response_max=v.response_max;
    }
    const double left=std::max(0.,at-window);
    for(auto& queue:arrivals)while(!queue.empty()&&queue.front().time<=left){if(queue.front().counted)--counts[queue.front().edge];queue.pop_front();}
    // Incremental merge. Deliveries precede posts at the same timestamp; strict
    // t_post > t_arrival explicitly excludes simultaneous zero-lag coincidences.
    while(delivery_cursor<h.delivery_begin+h.deliveries.size()||spike_cursor<h.spike_begin+h.spikes.size()) {
        const double td=delivery_cursor<h.delivery_begin+h.deliveries.size()?h.deliveries[size_t(delivery_cursor-h.delivery_begin)].t_arrival:INFINITY;
        const double ts=spike_cursor<h.spike_begin+h.spikes.size()?h.spikes[size_t(spike_cursor-h.spike_begin)].time:INFINITY;
        if(td<=ts) {
            const auto& d=h.deliveries[size_t(delivery_cursor++-h.delivery_begin)];++processed_events;
            if(d.t_arrival>left){arrivals[c.synapses[d.synapse].target].push_back({d.t_arrival,d.synapse,mode==0});if(mode==0)++counts[d.synapse];}
        } else {
            const auto& e=h.spikes[size_t(spike_cursor++-h.spike_begin)];++processed_events;
            if(mode==1)for(auto& d:arrivals[e.index]) {
                if(d.time>=e.time||d.time>e.time-response_min)break;
                if(!d.counted&&d.time>=e.time-response_max){d.counted=true;++counts[d.edge];}
            }
        }
    }
    columns=v.columns;rows=v.rows;time=at;
    available=window<=c.activity_horizon&&left>=h.available_after;
    message=available?"":"Недостаточно доступных событий для этого окна. Уменьшите окно; учитывается глубина записи и окна отображения.";
    // Shared geometry is prepared by the caller once for both fields.
    cells=geometry.activity_cells(c,weights,v,available?counts:std::vector<uint64_t>(counts.size(),0));
    maximum=0;for(auto cell:cells)maximum=std::max(maximum,cell.magnitude());
    update_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
}
WeightCell ActivityField::vector_at(const Config& c,double x,double y) const {
    if(!available)throw std::runtime_error("Activity window unavailable");
    if(!std::isfinite(x)||!std::isfinite(y)||x<0||y<0||x>c.space.width||y>c.space.height||columns<1||rows<1)return {};
    const int ix=std::min(columns-1,int(x/c.space.width*columns)),iy=std::min(rows-1,int(y/c.space.height*rows));
    return cells.at(size_t(iy*columns+ix));
}
}
