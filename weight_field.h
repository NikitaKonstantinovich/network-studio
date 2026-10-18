#pragma once
#include "network.h"
#include <utility>

namespace studio {
enum class FieldPolarity { All, Excitatory, Inhibitory };
enum class FieldModel { All, ConductanceSTP, ExponentialCurrent, TsodyksMarkram };
// Presentation only; coordinates and arrow scale use the normalized model plane.
struct TopologyViewState {
    bool field=false, edges=true, grid=true, all_edges=false, current=true, auto_scale=false;
    int columns=16, rows=12;
    double scale=.04, zoom=1, pan_x=0, pan_y=0;
    FieldPolarity polarity=FieldPolarity::All;
    FieldModel model=FieldModel::All;
    bool activity=false;
    int activity_mode=0;
    double activity_window=.05,response_min=0,response_max=.02,activity_scale=.04;
    FieldPolarity activity_polarity=FieldPolarity::All;
    FieldModel activity_model=FieldModel::All;
};
bool valid_topology_view(const TopologyViewState& s);
bool inhibitory_weight_edge(const Synapse& s,double weight);
struct WeightCell {
    double x=0,y=0;
    size_t count=0;
    uint64_t events=0;
    double magnitude() const;
};
// Half-open cells (outer border 1 included). Positive-length intersections only.
// Grid crossing parameters within 8*epsilon are one corner; canonical endpoint
// ordering makes traversal invariant to reversing an edge. No screen coordinates.
std::vector<uint32_t> weight_segment_cells(double x0,double y0,double x1,double y1,int columns,int rows);
class WeightField {
public:
    void update(const Config& c,const std::vector<StdpState>* current,const TopologyViewState& settings);
    std::vector<WeightCell> cells;
    size_t included_edges=0, geometry_builds=0, sum_updates=0, intersections=0;
    bool available=true;
    double maximum=0, geometry_ms=0, sum_ms=0, update_ms=0;
    size_t cache_bytes() const;
    std::vector<WeightCell> activity_cells(const Config& c,const std::vector<StdpState>& current,const TopologyViewState& settings,const std::vector<uint64_t>& counts) const;
private:
    struct Geometry {double x=0,y=0;size_t begin=0,end=0;};
    struct Input {double weight=0;bool included=false;};
    std::vector<std::pair<double,double>> positions;
    std::vector<std::pair<int,int>> endpoints;
    std::vector<Geometry> geometry;
    std::vector<uint32_t> crossed;
    std::vector<Input> inputs;
    std::vector<std::pair<double,double>> corrections;
    int columns=0,rows=0;
};
}
