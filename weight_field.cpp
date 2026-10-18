#include "weight_field.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace studio {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
bool coordinate(double x){return std::isfinite(x)&&x>=0&&x<=1;}
int grid_index(double x,int count) {
    int i=std::clamp(int(std::floor(x*count)),0,count-1);
    if(i>0&&x<double(i)/count)--i;
    if(i+1<count&&x>=double(i+1)/count)++i;
    return i;
}
void compensated(double value,double& sum,double& correction) {
    const double adjusted=value-correction, next=sum+adjusted;
    correction=(next-sum)-adjusted;sum=next;
}
}
bool valid_topology_view(const TopologyViewState& s) {
    return s.columns>=1&&s.columns<=64&&s.rows>=1&&s.rows<=64&&
        std::isfinite(s.scale)&&s.scale>=1e-6&&s.scale<=100&&
        std::isfinite(s.zoom)&&s.zoom>=.25&&s.zoom<=8&&
        std::isfinite(s.pan_x)&&std::abs(s.pan_x)<=8&&std::isfinite(s.pan_y)&&std::abs(s.pan_y)<=8&&
        int(s.polarity)>=0&&int(s.polarity)<=2&&int(s.model)>=0&&int(s.model)<=3;
}
bool inhibitory_weight_edge(const Synapse& s,double weight) {
    // Model/edge polarity, not the source neuron's display label. A zero
    // nonnegative weight stays in the excitatory filter but contributes zero.
    return s.model==SynapseModel::ConductanceSTP?s.reversal<=-40:(!s.stdp&&weight<0);
}
double WeightCell::magnitude() const{return std::hypot(x,y);}
std::vector<uint32_t> weight_segment_cells(double x0,double y0,double x1,double y1,int columns,int rows) {
    if(columns<1||columns>64||rows<1||rows>64)throw std::invalid_argument("Weight field grid must be 1..64 by 1..64.");
    if(!coordinate(x0)||!coordinate(y0)||!coordinate(x1)||!coordinate(y1)||(x0==x1&&y0==y1))return {};
    if(std::make_pair(x1,y1)<std::make_pair(x0,y0)){std::swap(x0,x1);std::swap(y0,y1);}
    const double dx=x1-x0,dy=y1-y0;
    std::vector<double> crossings{0,1};crossings.reserve(columns+rows+2);
    if(dx)for(int k=1;k<columns;++k){double t=(double(k)/columns-x0)/dx;if(t>0&&t<1)crossings.push_back(t);}
    if(dy)for(int k=1;k<rows;++k){double t=(double(k)/rows-y0)/dy;if(t>0&&t<1)crossings.push_back(t);}
    std::sort(crossings.begin(),crossings.end());
    std::vector<uint32_t> result;result.reserve(columns+rows);
    double previous=0;
    for(double next:crossings) {
        if(next-previous<=8*std::numeric_limits<double>::epsilon())continue;
        if(next<=previous)continue;
        const double t=previous+(next-previous)*.5;
        const int x=grid_index(x0+dx*t,columns);
        const int y=grid_index(y0+dy*t,rows);
        const auto cell=uint32_t(y*columns+x);
        if(result.empty()||result.back()!=cell)result.push_back(cell);
        previous=next;
    }
    return result;
}
size_t WeightField::cache_bytes() const {
    return positions.capacity()*sizeof(positions[0])+endpoints.capacity()*sizeof(endpoints[0])+
        geometry.capacity()*sizeof(Geometry)+crossed.capacity()*sizeof(uint32_t)+inputs.capacity()*sizeof(Input)+
        cells.capacity()*sizeof(WeightCell)+corrections.capacity()*sizeof(corrections[0]);
}
void WeightField::update(const Config& c,const std::vector<StdpState>* current,const TopologyViewState& s) {
    const auto started=Clock::now();
    if(!valid_topology_view(s)||c.neurons.size()>MaxNeurons||c.synapses.size()>MaxSynapses)
        throw std::invalid_argument("Invalid weight field settings or graph size.");
    bool rebuild=columns!=s.columns||rows!=s.rows||positions.size()!=c.neurons.size()||endpoints.size()!=c.synapses.size();
    for(size_t i=0;!rebuild&&i<positions.size();++i)rebuild=positions[i]!=std::make_pair(c.neurons[i].px,c.neurons[i].py);
    for(size_t i=0;!rebuild&&i<endpoints.size();++i)rebuild=endpoints[i]!=std::make_pair(c.synapses[i].source,c.synapses[i].target);
    if(rebuild) {
        const auto begin=Clock::now();columns=s.columns;rows=s.rows;
        positions.clear();for(const auto& n:c.neurons)positions.emplace_back(n.px,n.py);
        endpoints.clear();geometry.clear();crossed.clear();
        for(const auto& edge:c.synapses) {
            endpoints.emplace_back(edge.source,edge.target);Geometry g;g.begin=crossed.size();
            if(edge.source>=0&&edge.target>=0&&size_t(edge.source)<positions.size()&&size_t(edge.target)<positions.size()&&edge.source!=edge.target) {
                const auto a=positions[edge.source],b=positions[edge.target];
                auto path=weight_segment_cells(a.first,a.second,b.first,b.second,columns,rows);
                if(!path.empty()) {
                    const double dx=b.first-a.first,dy=b.second-a.second,length=std::hypot(dx,dy);
                    g.x=dx/length;g.y=dy/length;crossed.insert(crossed.end(),path.begin(),path.end());
                }
            }
            g.end=crossed.size();geometry.push_back(g);
        }
        cells.assign(columns*rows,{});corrections.resize(cells.size());inputs.assign(c.synapses.size(),{});
        intersections=crossed.size();++geometry_builds;geometry_ms=elapsed(begin);
    }
    bool recalculate=rebuild||!available;
    available=!s.current||(current&&current->size()==c.synapses.size());
    for(size_t j=0;j<c.synapses.size();++j) {
        const auto& edge=c.synapses[j];Input next;
        if(available) {
            const double weight=s.current?(*current)[j].weight:edge.weight;
            if(!std::isfinite(weight)){available=false;break;}
            const bool inhibitory=inhibitory_weight_edge(edge,weight);
            next.included=edge.enabled&&geometry[j].end>geometry[j].begin&&
                int(edge.model)>=0&&int(edge.model)<=2&&(s.model==FieldModel::All||int(s.model)==int(edge.model)+1)&&
                (s.polarity==FieldPolarity::All||(s.polarity==FieldPolarity::Inhibitory)==inhibitory);
            next.weight=next.included?std::abs(weight):0;
        }
        if(next.weight!=inputs[j].weight||next.included!=inputs[j].included)recalculate=true;
        inputs[j]=next;
    }
    if(!available){for(auto& cell:cells)cell={};included_edges=0;maximum=0;update_ms=elapsed(started);return;}
    if(recalculate) {
        const auto begin=Clock::now();std::fill(cells.begin(),cells.end(),WeightCell{});
        std::fill(corrections.begin(),corrections.end(),std::pair<double,double>{});included_edges=0;
        for(size_t j=0;j<geometry.size();++j)if(inputs[j].included) {
            ++included_edges;const auto& g=geometry[j];const double x=inputs[j].weight*g.x,y=inputs[j].weight*g.y;
            for(size_t k=g.begin;k<g.end;++k) {
                const auto index=crossed[k];auto& cell=cells[index];++cell.count;
                compensated(x,cell.x,corrections[index].first);compensated(y,cell.y,corrections[index].second);
            }
        }
        maximum=0;for(const auto& cell:cells)maximum=std::max(maximum,cell.magnitude());
        ++sum_updates;sum_ms=elapsed(begin);
    }
    update_ms=elapsed(started);
}
}
