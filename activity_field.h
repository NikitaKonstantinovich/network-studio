#pragma once
#include "weight_field.h"
#include "simulation_view.h"

namespace studio {
// A visualization/controller cache. Reads only RecentActivity, never full History.
// Rebuilt from checkpoint's bounded event window; not part of the solver state.
class ActivityField {
public:
    std::vector<WeightCell> cells;
    double time=-1,maximum=0,update_ms=0;
    bool available=true;
    std::string message;
    size_t processed_events=0;
    void update(const SimulationReadView& s,const TopologyViewState& settings,WeightField& geometry);
    WeightCell vector_at(const Config& c,double x,double y) const;
private:
    struct Arrival {double time;int edge;bool counted;};
    std::vector<std::deque<Arrival>> arrivals;
    std::vector<uint64_t> counts;
    uint64_t delivery_cursor=0,spike_cursor=0;
    const void* owner=nullptr;
    uint64_t generation=0;
    int mode=-1,columns=0,rows=0;
    double window=0,response_min=0,response_max=0;
};
}
