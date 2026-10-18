#pragma once
#include "weight_field.h"
#include <algorithm>
#include <cmath>

namespace studio {
struct FieldArrow {double x=0,y=0;bool saturated=false;};
// A non-square viewport projects direction with the graph, but all arrow
// lengths use ONE pixels-per-model-unit factor, independent of direction.
inline FieldArrow project_weight_arrow(const WeightCell& cell,double scale,double cap,double span_x,double span_y) {
    const double magnitude=cell.magnitude();if(magnitude==0||span_x<=0||span_y<=0)return {};
    const double x=cell.x*span_x,y=cell.y*span_y;
    const double length=std::min(scale*magnitude,cap)*std::min(span_x,span_y);
    const double projection=std::hypot(x,y);
    return {x/projection*length,y/projection*length,scale*magnitude>cap*(1+1e-12)};
}
}
