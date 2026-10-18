#include "gui.h"
#include "implot.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace studio;
namespace {
int checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void frame(App& app){ImGui::NewFrame();app.draw();ImGui::Render();check(ImGui::GetDrawData()->TotalVtxCount>100,"Empty render");}
std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void equal(const Simulator& a,const Simulator& b,const std::filesystem::path& dir) {
    save_checkpoint(a,dir/"a.nscp");save_checkpoint(b,dir/"b.nscp");
    check(bytes(dir/"a.nscp")==bytes(dir/"b.nscp"),"Field controls changed simulation/checkpoint/queue/history");
}
void resign(std::string& s) {
    uint64_t hash=14695981039346656037ull;for(size_t i=0;i<s.size()-8;++i){hash^=static_cast<unsigned char>(s[i]);hash*=1099511628211ull;}
    for(int k=0;k<8;++k)s[s.size()-8+k]=char(hash>>(8*k));
}
}
int main() {
    try {
        ImGui::CreateContext();ImPlot::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={1800,1200};io.IniFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
        ImGuiPlatformMonitor monitor;monitor.MainPos=monitor.WorkPos={0,0};monitor.MainSize=monitor.WorkSize=io.DisplaySize;monitor.DpiScale=1;ImGui::GetPlatformIO().Monitors.push_back(monitor);
        io.DeltaTime=1.f/60;io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
        unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);io.Fonts->SetTexID(ImTextureID(1));apply_style();
        auto dir=std::filesystem::current_path()/"field-gui"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(dir);
        App app;auto c=preset(6);c.duration=.3;c.synapses[0].delay=.12;app.replace(c);Simulator reference(c);
        while(app.simulation->time<.12){check(app.simulation->step()&&reference.step(),"Fixture failed");}
        check(!app.simulation->pending_deliveries.empty(),"Queue not covered");
        auto& v=app.plot_windows.topology;v.field=true;v.columns=8;v.rows=6;v.scale=.04;
        frame(app);const auto builds=app.weight_field.geometry_builds;const auto sums=app.weight_field.sum_updates;
        auto old=app.weight_field.cells;
        for(int i=0;i<8;++i) {
            io.DisplaySize=i%2?ImVec2(1200,850):ImVec2(1800,1200);io.FontGlobalScale=i%2?1.5f:1;
            v.grid=i%2;v.edges=i%2;v.zoom=1+i*.25;v.pan_x=.03*i;v.pan_y=-.02*i;v.scale=.01*(i+1);v.auto_scale=i%2;frame(app);
            for(size_t j=0;j<old.size();++j)check(old[j].x==app.weight_field.cells[j].x&&old[j].y==app.weight_field.cells[j].y,"Viewport size/DPI/camera changed numerical field");
        }
        check(app.weight_field.geometry_builds==builds&&app.weight_field.sum_updates==sums,"Display settings rebuilt numerical cache");equal(*app.simulation,reference,dir);
        v.current=false;frame(app);v.current=true;frame(app);equal(*app.simulation,reference,dir);
        app.running=true;frame(app);app.running=false;while(reference.steps<app.simulation->steps)check(reference.step(),"Reference advance");equal(*app.simulation,reference,dir);
        check(app.weight_field.geometry_builds==builds,"Running STDP rebuilt geometry");
        save_checkpoint(*app.simulation,dir/"restore.nscp");app.reset();frame(app);
        WeightField initial;initial.update(c,&app.simulation->plasticity,v);
        for(size_t i=0;i<initial.cells.size();++i)check(initial.cells[i].x==app.weight_field.cells[i].x,"Reset retained trained field");
        check(app.restore_state(dir/"restore.nscp"),"Checkpoint restore failed");frame(app);equal(*app.simulation,reference,dir);
        v.polarity=FieldPolarity::Inhibitory;v.model=FieldModel::TsodyksMarkram;
        check(app.plot_windows.save(dir/"view.nsworkspace"),"Workspace save");const auto saved=bytes(dir/"view.nsworkspace");
        v=TopologyViewState{};check(app.plot_windows.load(dir/"view.nsworkspace"),"Workspace load");
        check(v.field&&v.columns==8&&v.rows==6&&v.polarity==FieldPolarity::Inhibitory&&v.model==FieldModel::TsodyksMarkram&&v.zoom==2.75,"Workspace lost topology settings");
        // Schema 1 has the same window/ini prefix and no 112-byte topology block.
        auto legacy=saved;legacy.erase(legacy.size()-8-112,112);const size_t version=8+std::string("Network Studio plot workspace").size();legacy[version]=1;resign(legacy);
        write_atomic(dir/"legacy.nsworkspace",legacy);check(app.plot_windows.load(dir/"legacy.nsworkspace")&&!v.field&&v.zoom==1,"Schema 1 compatibility/defaults");
        auto corrupt=saved;corrupt[corrupt.size()-8-112+6*8]=0;resign(corrupt);write_atomic(dir/"invalid.nsworkspace",corrupt);
        check(!app.plot_windows.load(dir/"invalid.nsworkspace")&&!v.field,"Invalid grid partially replaced workspace");
        auto replacement=preset(2);replacement.duration=.01;replacement.sample=.001;replacement.neurons[0].model=NeuronModel::Izhikevich;
        save_config(replacement,dir/"mixed.json");app.load(dir/"mixed.json");v.field=true;frame(app);
        check(app.weight_field.geometry_builds>builds&&app.weight_field.included_edges==replacement.synapses.size(),"Loading mixed astro network kept old field");
        remove_neuron(app.config,0);app.reset();frame(app);check(app.weight_field.included_edges==app.config.synapses.size(),"Deletion retained stale indices");
        NetworkGeneration g;g.excitatory.count=400;g.inhibitory.count=100;g.excitatory.in_degree=g.inhibitory.in_degree={30,0};
        g.synapse.stdp=true;g.duration=.03;auto large=generate_network(preset(3),g,false);app.replace(large);v.field=true;v.edges=false;
        io.DisplaySize={1800,1200};io.FontGlobalScale=1;frame(app);const auto gc=app.weight_field.geometry_builds;
        auto begin=std::chrono::steady_clock::now();for(int i=0;i<100;++i)frame(app);
        const double enabled=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/100;
        v.field=false;begin=std::chrono::steady_clock::now();for(int i=0;i<100;++i)frame(app);
        const double disabled=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/100;
        check(app.weight_field.geometry_builds==gc&&app.simulation->steps==0,"Idle frames rebuilt geometry or stepped simulator");
        std::cout<<"PASS "<<checks<<" GUI checks; exact checkpoints; 500/15000 ImGui frame: enabled_ms="<<enabled<<", disabled_ms="<<disabled<<"\n";
        ImPlot::DestroyContext();ImGui::DestroyContext();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
