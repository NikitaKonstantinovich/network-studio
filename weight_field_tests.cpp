#include "weight_field.h"
#include "weight_field_projection.h"
#include "generator.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <set>

using namespace studio;
namespace {
int checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void near(double a,double b,const char* message){check(std::abs(a-b)<1e-11*(1+std::abs(b)),message);}
Config fixture() {
    Config c;c.neurons={{"Pre",.1,.5},{"Post",.9,.5}};
    for(auto& n:c.neurons){n.model=NeuronModel::Izhikevich;n.amplitude=0;}
    c.synapses={{0,1,-1}};c.synapses[0].model=SynapseModel::ExponentialCurrent;c.synapses[0].weight=.5;
    c.duration=.03;c.dt=.00005;c.sample=.001;return c;
}
Config four_sources(int variant) {
    auto c=fixture();c.name="Four sources: "+std::to_string(variant);
    c.neurons={{"Source 1",.12,.12},{"Source 2",.12,.36},{"Source 3",.12,.62},{"Source 4",.12,.88},{"Receiver",.73,.5}};
    for(auto& n:c.neurons){n.model=NeuronModel::Izhikevich;n.amplitude=0;}
    c.synapses.clear();for(int i=0;i<4;++i){Synapse s;s.source=i;s.target=4;s.model=SynapseModel::ExponentialCurrent;s.weight=.5;c.synapses.push_back(s);}
    if(variant){c.neurons[0].px=.95;c.neurons[0].py=.16;}
    if(variant==2)c.synapses[0].weight=.1;
    return c;
}
Config large(int count=500,int degree=30) {
    NetworkGeneration g;g.excitatory.count=count*4/5;g.inhibitory.count=count/5;
    g.excitatory.in_degree=g.inhibitory.in_degree={double(degree),0};g.synapse.stdp=true;g.synapse.gain=.05;
    g.duration=.03;g.sample=.001;auto c=generate_network(preset(3),g,false);c.dt=.00005;
    c.record_all=false;c.record_neurons={0,1,2};c.record_synapses={0,1,2};
    for(size_t i=0;i<c.neurons.size();++i)c.neurons[i].current=13+double(i%11);
    return c;
}
std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::set<uint32_t> cellset(double a,double b,double c,double d,int cols,int rows) {
    auto v=weight_segment_cells(a,b,c,d,cols,rows);check(std::set<uint32_t>(v.begin(),v.end()).size()==v.size(),"Repeated cell");return {v.begin(),v.end()};
}
void boundaries() {
    check(cellset(0,0,1,1,4,4)==std::set<uint32_t>({0,5,10,15}),"Corner touched side cells");
    check(cellset(1,1,0,0,4,4)==cellset(0,0,1,1,4,4),"Reversal changed cells");
    check(cellset(.5,0,.5,1,4,4)==std::set<uint32_t>({2,6,10,14}),"Vertical boundary ownership");
    check(cellset(0,.5,1,.5,4,4)==std::set<uint32_t>({8,9,10,11}),"Horizontal boundary ownership");
    check(cellset(1,0,1,1,4,4)==std::set<uint32_t>({3,7,11,15}),"Outer border ownership");
    check(cellset(.1,.1,.5,.1,4,4)==std::set<uint32_t>({0,1}),"Endpoint-only touch included");
    check(cellset(.1,.1,.1,.1,4,4).empty(),"Coincident segment accepted");
    check(cellset(0,0,1,1-1e-10,4,4).count(1)==1,"Near corner erased a real side-cell intersection");
    check(cellset(0,0,1,std::nextafter(1.,0.),4,4)==std::set<uint32_t>({0,5,10,15}),"Corner roundoff policy changed");
    for(int grid=2;grid<=64;++grid)for(int x=1;x<grid;++x) {
        auto cells=cellset(double(x)/grid,0,double(x)/grid,1,grid,grid);
        check(cells.size()==size_t(grid),"Grid-line cell count");for(auto i:cells)check(int(i%grid)==x,"Rounded boundary assigned left");
    }
    // Independent slab clipping, not grid traversal; random edges avoid ties.
    std::mt19937 rng(1024);std::uniform_real_distribution<double> uniform(0,1);
    for(int trial=0;trial<300;++trial) {
        double x0=uniform(rng),y0=uniform(rng),x1=uniform(rng),y1=uniform(rng);int cols=3+trial%13,rows=2+trial%7;
        std::set<uint32_t> expected;
        for(int y=0;y<rows;++y)for(int x=0;x<cols;++x) {
            double lo=0,hi=1;
            auto clip=[&](double a,double b,double low,double high){double t0=(low-a)/(b-a),t1=(high-a)/(b-a);lo=std::max(lo,std::min(t0,t1));hi=std::min(hi,std::max(t0,t1));};
            clip(x0,x1,double(x)/cols,double(x+1)/cols);clip(y0,y1,double(y)/rows,double(y+1)/rows);
            if(hi>lo)expected.insert(y*cols+x);
        }
        check(cellset(x0,y0,x1,y1,cols,rows)==expected,"Traversal disagrees with independent slab clipping");
        check(cellset(x1,y1,x0,y0,cols,rows)==expected,"Reversed traversal disagrees with slab clipping");
    }
}
void sums() {
    auto horizontal=project_weight_arrow({.5,0,1},.04,.1,1000,200);
    auto vertical=project_weight_arrow({0,.5,1},.04,.1,1000,200);
    auto diagonal=project_weight_arrow({.3,.4,1},.04,.1,1000,200);
    near(std::hypot(horizontal.x,horizontal.y),4,"Arrow common scale");
    near(std::hypot(vertical.x,vertical.y),4,"Aspect ratio changed vertical magnitude");
    near(std::hypot(diagonal.x,diagonal.y),4,"Aspect ratio changed diagonal magnitude");
    auto doubled=project_weight_arrow({1,0,1},.04,.1,1000,200);near(doubled.x,8,"Screen arrow not proportional");
    auto saturated=project_weight_arrow({100,0,1},.04,.1,1000,200);check(saturated.saturated,"Saturation not flagged");near(saturated.x,20,"Saturation cap");
    auto c=fixture();TopologyViewState v;v.current=false;v.columns=v.rows=1;WeightField f;
    f.update(c,nullptr,v);near(f.cells[0].x,.5,"Single edge");near(f.cells[0].y,0,"Single direction");
    c.synapses[0].weight=1;f.update(c,nullptr,v);near(f.cells[0].x,1,"Weight not proportional");check(f.geometry_builds==1,"Weight rebuilt geometry");
    c.neurons[1].px=.3;f.update(c,nullptr,v);near(f.cells[0].x,1,"Length multiplied weight");
    Synapse reverse=c.synapses[0];reverse.source=1;reverse.target=0;c.synapses.push_back(reverse);
    f.update(c,nullptr,v);near(f.maximum,0,"Opposite edges do not cancel");check(f.cells[0].count==2,"Count after cancellation");
    c.neurons.push_back(c.neurons[0]);c.neurons.back().py=.9;c.synapses[1].source=0;c.synapses[1].target=2;
    f.update(c,nullptr,v);near(f.cells[0].x,1,"Orthogonal x");near(f.cells[0].y,1,"Orthogonal y");near(f.maximum,std::sqrt(2.),"Orthogonal magnitude");
    c.synapses[0].weight=-.5;v.polarity=FieldPolarity::Inhibitory;f.update(c,nullptr,v);
    near(f.cells[0].x,.5,"Negative weight reversed direction");check(f.included_edges==1,"Current inhibitory filter");
    c.synapses[1].model=SynapseModel::ConductanceSTP;c.synapses[1].reversal=-80;
    f.update(c,nullptr,v);check(f.included_edges==2,"Reversal-potential inhibitory classification");
    for(int model=1;model<=3;++model) {
        c.synapses[0].model=SynapseModel(model-1);c.synapses[0].weight=.5;c.synapses[0].reversal=0;
        v.model=FieldModel(model);v.polarity=FieldPolarity::Excitatory;f.update(c,nullptr,v);
        check(f.included_edges==1,"Model/excitatory filters disagree");near(f.cells[0].x,.5,"Model altered raw weight");
    }
    v.model=FieldModel::All;v.polarity=FieldPolarity::All;c.synapses[0].enabled=false;
    f.update(c,nullptr,v);check(f.included_edges==1,"Disabled edge included");c.synapses[1].target=0;
    f.update(c,nullptr,v);check(f.included_edges==0,"Self edge included");
    c.synapses[1].target=2;c.neurons[2]=c.neurons[0];f.update(c,nullptr,v);check(f.included_edges==0,"Coincident nodes included");
    c=fixture();Simulator s(c);s.plasticity[0].weight=.8;v.current=true;f.update(c,&s.plasticity,v);near(f.maximum,.8,"Actual trained weight ignored");
    v.current=false;f.update(c,&s.plasticity,v);near(f.maximum,.5,"Initial weight replaced by trained weight");
    v.current=true;f.update(c,nullptr,v);check(!f.available&&f.maximum==0,"Unavailable weight silently substituted");
    f.update(c,&s.plasticity,v);near(f.maximum,.8,"Unavailable-to-available refresh");
    const auto builds=f.geometry_builds,updates=f.sum_updates;v.zoom=2;v.pan_x=.2;v.scale=.2;v.auto_scale=true;
    f.update(c,&s.plasticity,v);check(f.geometry_builds==builds&&f.sum_updates==updates,"Camera changed field/cache");
    c.astrocytes.resize(2);c.gaps={{0,1}};f.update(c,&s.plasticity,v);near(f.maximum,.8,"Astro diffusion entered field");
    c.neurons.clear();c.synapses.clear();std::vector<StdpState> empty;f.update(c,&empty,v);
    check(f.cells.size()==1&&f.included_edges==0&&f.maximum==0,"Empty graph field");
    c=fixture();c.neurons.push_back(c.neurons[0]);c.synapses.push_back(c.synapses[0]);c.synapses[1].source=2;
    v.current=false;f.update(c,nullptr,v);near(f.cells[0].x,1,"Parallel contributions did not add");check(f.cells[0].count==2,"Parallel count");
    v.columns=8;v.rows=6;
    for(int variant=0;variant<3;++variant) {
        c=four_sources(variant);f.update(c,nullptr,v);const auto cell=f.cells[2*8+5];
        double expected_x=0,expected_y=0;
        for(int source:{0,1}) {
            const double dx=c.neurons[4].px-c.neurons[source].px,dy=c.neurons[4].py-c.neurons[source].py;
            const double weight=c.synapses[source].weight;
            expected_x+=weight*dx/std::hypot(dx,dy);expected_y+=weight*dy/std::hypot(dx,dy);
        }
        check(cell.count==2,"Four-source comparison cell count");near(cell.x,expected_x,"Four-source analytical x");near(cell.y,expected_y,"Four-source analytical y");
        std::cout<<"Four sources variant "<<variant<<", cell [6,3]: Vx="<<cell.x<<", Vy="<<cell.y<<", magnitude="<<cell.magnitude()<<"\n";
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--fixtures") {
            auto dir=std::filesystem::path(argv[2]);std::filesystem::create_directories(dir);
            for(int i=0;i<3;++i)save_config(four_sources(i),dir/("four_sources_"+std::to_string(i)+".json"));
            save_config(large(),dir/"large_stdp.json");auto astro=preset(2);astro.duration=.15;astro.sample=.001;
            astro.neurons[1].model=NeuronModel::Izhikevich;save_config(astro,dir/"mixed_astro.json");return 0;
        }
        boundaries();sums();
        auto c=large();check(c.neurons.size()==500&&c.synapses.size()==15000,"Wrong benchmark size");
        Simulator s(c);WeightField f;TopologyViewState view;view.field=true;
        f.update(c,&s.plasticity,view);double build=f.geometry_ms;size_t cache=f.cache_bytes();
        auto root=std::filesystem::current_path()/"weight-field-tests"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        save_checkpoint(s,root/"before.nscp");
        auto begin=std::chrono::steady_clock::now();for(int i=0;i<200;++i)f.update(c,&s.plasticity,view);
        double unchanged=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/200;
        save_checkpoint(s,root/"after.nscp");check(bytes(root/"before.nscp")==bytes(root/"after.nscp"),"Rendering calculation changed simulator");
        auto weights=s.plasticity;begin=std::chrono::steady_clock::now();
        for(int i=0;i<200;++i){weights[0].weight+=1e-6;f.update(c,&weights,view);}
        double weighted=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/200;
        while(!s.done()){check(s.step(),"Large active STDP failed");f.update(c,&s.plasticity,view);}
        size_t changed=0;for(size_t j=0;j<c.synapses.size();++j)changed+=s.plasticity[j].weight!=c.synapses[j].weight;
        check(changed>0&&f.geometry_builds==1,"STDP did not change weights or rebuilt geometry");
        save_checkpoint(s,root/"trained.nscp");auto restored=load_checkpoint(root/"trained.nscp");WeightField second;
        second.update(restored->config,&restored->plasticity,view);
        for(size_t i=0;i<f.cells.size();++i){near(second.cells[i].x,f.cells[i].x,"Restored x");near(second.cells[i].y,f.cells[i].y,"Restored y");}
        auto maximum=large(1000,50);Simulator full(maximum);view.columns=view.rows=64;WeightField maxfield;
        maxfield.update(maximum,&full.plasticity,view);check(maxfield.included_edges==50000,"Maximum graph lost edges");
        std::cout<<"PASS "<<checks<<" checks. 500 neurons / 15000 edges: geometry_ms="<<build<<", cached_frame_ms="<<unchanged
            <<", changed_weights_frame_ms="<<weighted<<", cache_bytes="<<cache<<", trained_edges="<<changed
            <<". 1000 / 50000 / 64x64: geometry_ms="<<maxfield.geometry_ms<<", cache_bytes="<<maxfield.cache_bytes()<<"\n";
        std::cout<<"Artifacts: "<<root.u8string()<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
