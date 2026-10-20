#include "gui.h"
#include "imgui_internal.h"
#include "implot.h"
#include <fstream>
#include <iostream>
#include <chrono>

namespace {
int checks=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
std::string bytes(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void frame(studio::App& app){ImGui::NewFrame();app.draw();ImGui::Render();app.synchronize();}
void equal(studio::App& a,const studio::Simulator& b,const std::filesystem::path& dir) {
    a.save_state(dir/L"a.nscp");studio::save_checkpoint(b,dir/L"b.nscp");
    require(bytes(dir/L"a.nscp")==bytes(dir/L"b.nscp"),"Plot operations changed full checkpoint bytes (including queue, learning, integrator and history).");
}
}
int main() {
    try {
        ImGui::CreateContext();ImPlot::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={1600,1000};io.IniFilename=nullptr;
        io.DeltaTime=1.f/60;io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
        ImGuiPlatformMonitor m;m.MainPos=m.WorkPos={0,0};m.MainSize=m.WorkSize={1600,1000};m.DpiScale=1;ImGui::GetPlatformIO().Monitors.push_back(m);
        unsigned char* data;int width,height;io.Fonts->GetTexDataAsRGBA32(&data,&width,&height);io.Fonts->SetTexID(ImTextureID(1));studio::apply_style();
        auto dir=std::filesystem::current_path()/"verification"/std::filesystem::u8path("plot-tests-кириллица-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        studio::App app;auto c=studio::preset(6);c.duration=.22;c.sample=.00025;c.noise_sigma=.1;c.synapses[0].delay=.12;c.record_all=false;c.record_neurons={0};c.record_synapses={0};
        c.analysis.bin=.001;c.analysis.burst_window=.002;c.analysis.start_rate=100;c.analysis.end_rate=0;c.analysis.min_neurons=1;c.analysis.min_gap=0;c.analysis.response_max=.01;
        app.replace(c);studio::Simulator reference(c);
        while(reference.time<.12)require(reference.step(),"Fixture failed");app.advance_for(reference.time);
        require(!app.simulation->pending_deliveries.empty(),"The invariance fixture needs a pending delivery.");
        auto& plots=app.plot_windows;auto first=plots.create(studio::PlotKind::Voltage,0);
        auto* original=plots.find(first);original->title="Потенциал α";original->follow=false;original->auto_fit=false;
        original->seconds=.025;original->limits={.03,.13,-80,40,-2,2};original->limits_valid=original->restore_limits=true;original->fit=false;
        auto clone=plots.duplicate(first);
        require(clone!=first&&plots.find(clone)->objects==plots.find(first)->objects,"Duplicate lost selected objects or reused ID.");
        // Reacquire after vector mutation; clients must not retain references across create().
        plots.find(clone)->seconds=3600;plots.find(clone)->objects={1};plots.find(clone)->title="Другой нейрон";
        require(plots.find(first)->seconds==.025&&plots.find(first)->objects==std::vector<int>{0},"View settings are shared between duplicates.");
        auto input=plots.create(studio::PlotKind::Neuron);auto weight=plots.create(studio::PlotKind::Synapse);plots.find(weight)->view=1;
        plots.create(studio::PlotKind::Raster);plots.create(studio::PlotKind::Deliveries);
        auto network=plots.create(studio::PlotKind::Network),network2=plots.create(studio::PlotKind::Network);
        plots.find(network)->analysis_interval={.02,.12,false};
        require(plots.create_analysis_profile(*plots.find(network),u8"Общий профиль"),"Profile creation failed");
        require(!plots.create_analysis_profile(*plots.find(network),u8"Общий профиль"),"Duplicate profile silently overwritten");
        plots.find(network2)->analysis.start_rate+=10;
        require(plots.load_analysis_profile(*plots.find(network2),0),"Profile load failed");
        plots.apply_analysis_to_all(*plots.find(network));frame(app);
        require(plots.find(network2)->analysis_interval.to==.12&&plots.embedded[7].analysis_interval.from==.02,"Apply to all did not copy explicit interval");
        auto result=[&](uint32_t id){auto* w=plots.find(id);return studio::analyze_network(app.simulation->config,app.simulation->history,app.simulation->time,w->analysis,w->analysis_interval);};
        require(studio::analysis_result_json(result(network),plots.analysis_profiles[0])==studio::analysis_result_json(result(network2),plots.analysis_profiles[0]),"Identical windows produce different statistics");
        auto expected=result(network);studio::export_analysis(expected,plots.find(network)->analysis,dir/"window",plots.analysis_profiles[0].name);
        require(!expected.bursts.empty()&&!expected.responses.empty()&&expected.responses.front().latency>=0,"Window/export equivalence fixture must contain real bursts and matched responses");
        studio::RunnerCommand export_command;export_command.kind=studio::RunnerCommand::Kind::ExportAnalysis;export_command.analysis=plots.find(network)->analysis;export_command.analysis_interval=plots.find(network)->analysis_interval;export_command.text=plots.analysis_profiles[0].name;export_command.path=dir/"runner";
        require(app.wait_command(app.command(export_command)).ok,"Analysis runner export failed");
        for(auto suffix:{"_analysis.json","_activity.csv","_bursts.csv","_responses.csv"})require(bytes(dir/(std::string("window")+suffix))==bytes(dir/(std::string("runner")+suffix)),"Window and runner export differ with identical coverage/settings");
        plots.find(network)->seconds=.001;plots.find(network2)->seconds=300;plots.find(network2)->follow=false;frame(app);
        require(studio::analysis_result_json(result(network),plots.analysis_profiles[0])==studio::analysis_result_json(result(network2),plots.analysis_profiles[0]),"Zoom changed interval statistics");
        equal(app,reference,dir);
        const auto* values=app.simulation->history.values[0].data();const auto* times=app.simulation->history.time.data();
        for(int i=0;i<5;++i){app.selected_kind=i%3;app.selected_index=i%2;frame(app);}
        require(plots.find(first)->objects==std::vector<int>{0},"Inspector selection changed an independent graph.");
        require(values==app.simulation->history.values[0].data()&&times==app.simulation->history.time.data(),"Drawing replaced shared history storage.");
        equal(app,reference,dir);
        auto id=ImGui::FindWindowByName(plots.find(first)->window_name().c_str())->ID;
        plots.find(first)->title="Переименован###вложенный ID";frame(app);
        require(ImGui::FindWindowByName(plots.find(first)->window_name().c_str())->ID==id,"Editable title changed stable ImGui ID.");
        require(plots.save(dir/L"вид.nsworkspace"),"Workspace save failed.");
        const auto saved=bytes(dir/L"вид.nsworkspace");const auto count=plots.windows.size();
        plots.close(input);require(plots.windows.size()==count-1,"Closing a window failed.");
        plots.automatic_save_allowed=false; // A corrupt startup file must not block an explicit valid replacement forever.
        require(plots.load(dir/L"вид.nsworkspace"),"Workspace load failed.");frame(app);frame(app);
        require(plots.automatic_save_allowed,"Explicit valid replacement did not re-enable workspace autosave.");
        require(plots.windows.size()==count&&plots.find(first)->seconds==.025&&!plots.find(first)->follow,"Workspace lost settings.");
        require(plots.analysis_profiles.size()==1&&plots.analysis_profiles[0].name==u8"Общий профиль"&&plots.find(network)->analysis_interval.from==.02&&plots.find(network)->analysis_interval.to==.12&&!plots.find(network)->analysis_interval.current_end&&plots.embedded[7].analysis_profile==u8"Общий профиль","Workspace lost profiles / independent interval");
        require(plots.find(clone)->seconds==3600,"Long plot time window was clamped or lost on restore.");
        require(plots.find(first)->limits[0]==.03&&plots.find(first)->limits[1]==.13,"Manual axis range was not restored.");
        studio::write_atomic(dir/L"truncated.nsworkspace",saved.substr(0,saved.size()-3));
        require(!plots.load(dir/L"truncated.nsworkspace")&&plots.windows.size()==count,"Truncated workspace mutated current views.");
        auto damaged=saved;damaged[55]^=1;studio::write_atomic(dir/L"damaged.nsworkspace",damaged);
        require(!plots.load(dir/L"damaged.nsworkspace")&&plots.find(first)->seconds==.025,"Corrupt workspace mutated current views.");
        auto incompatible=saved;
        const size_t version=8+std::string("Network Studio plot workspace").size();incompatible[version]=99;
        uint64_t hash=14695981039346656037ull;for(size_t i=0;i<incompatible.size()-8;++i){hash^=static_cast<unsigned char>(incompatible[i]);hash*=1099511628211ull;}
        for(int i=0;i<8;++i)incompatible[incompatible.size()-8+i]=char(hash>>(8*i));studio::write_atomic(dir/L"version.nsworkspace",incompatible);
        require(!plots.load(dir/L"version.nsworkspace"),"Unknown workspace schema was accepted.");
        studio::write_atomic(dir/L"oversize.nsworkspace",std::string(studio::PlotWindows::MaxWorkspaceBytes+1,'x'));
        require(!plots.load(dir/L"oversize.nsworkspace"),"Workspace size limit failed.");
        require(!plots.load({})&&plots.windows.size()==count,"Cancelled load changed windows.");
        require(!plots.save(dir/L"missing"/L"file.nsworkspace")&&bytes(dir/L"вид.nsworkspace")==saved,"Failed save destroyed previous workspace.");
        plots.find(first)->position={50000,-50000};plots.find(first)->place=true;frame(app);
        auto pos=plots.find(first)->position;require(pos.x>=0&&pos.x<1600&&pos.y>=0&&pos.y<1000,"Off-screen window was not clamped.");
        plots.reset_layout();frame(app);equal(app,reference,dir);
        for(int i=0;i<200;++i) {
            require(reference.step(),"Continuation failed.");studio::RunnerCommand next;next.kind=studio::RunnerCommand::Kind::RunTo;next.value=reference.time;require(app.wait_command(app.command(next)).ok,"Runner continuation failed.");
            if(i%20==0){auto copy=plots.duplicate(first);frame(app);plots.close(copy);}
        }
        equal(app,reference,dir);
        app.save_state(dir/L"resume.nscp");app.reset();frame(app);
        require(plots.find(first)->compatible&&app.simulation->time==0,"Reset invalidated compatible views.");
        require(plots.find(network)->analysis_interval.to==.12&&plots.analysis_profiles.size()==1,"Reset lost presentation profile/interval");
        require(app.restore_state(dir/L"resume.nscp"),"Restore checkpoint failed.");frame(app);
        require(plots.find(first)->compatible,"Same-protocol checkpoint lost binding.");equal(app,reference,dir);
        require(plots.find(network)->analysis_interval.from==.02&&plots.analysis_profiles.size()==1,"Checkpoint replaced workspace profiles");
        // Even an identical JSON replacement is a new graph, and requires explicit consent to rebind.
        app.replace(c);frame(app);require(!plots.find(first)->compatible,"Replacement silently retargeted old indices.");
        plots.rebind(*plots.find(first));require(plots.find(first)->compatible,"Explicit rebind failed.");
        studio::remove_neuron(app.config,0);app.reset();frame(app);require(!plots.find(first)->compatible,"Deletion silently retargeted another neuron at index 0.");
        require(plots.load(dir/L"вид.nsworkspace")&&!plots.find(first)->compatible,"Incompatible saved binding was silently accepted.");
        // Larger recorded network, 8 simultaneous views share one retained display history.
        studio::NetworkGeneration generation;generation.excitatory.count=80;generation.inhibitory.count=20;
        generation.excitatory.in_degree=generation.inhibitory.in_degree={4,0};generation.duration=.5;generation.sample=.00025;
        auto large=studio::generate_network(studio::preset(3),generation,false);large.record_all=true;
        for(auto& n:large.neurons)n.current=8;app.replace(large);app.advance_for(app.config.duration);require(app.simulation->done()&&app.simulation->error.empty(),"Large fixture failed.");
        plots.windows.clear();for(int i=0;i<8;++i)plots.create(i%2?studio::PlotKind::Synapse:studio::PlotKind::Voltage,i);
        const auto* storage=app.simulation->history.values[0].data();const auto display_samples=app.simulation->history.time.size();auto before=std::chrono::steady_clock::now();
        for(int i=0;i<20;++i)frame(app);
        auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
        require(storage==app.simulation->history.values[0].data()&&app.simulation->history.time.size()==display_samples,"Multiple views changed retained display history.");
        require(app.simulation->truncated&&display_samples<2001&&app.simulation->history.time.back()==large.duration,"Large display history was not explicitly bounded at the latest sample.");
        app.save_state(dir/L"large-full.nscp");require(studio::load_checkpoint(dir/L"large-full.nscp")->history.time.size()==2001,"Display retention truncated scientific history.");
        std::cout<<"PASS: "<<checks<<" plot/workspace checks; exact checkpoints match; 100 neurons / 400 edges / 8 windows / 20 frames: "<<elapsed<<" ms. Artifacts: "<<dir.u8string()<<"\n";
        ImPlot::DestroyContext();ImGui::DestroyContext();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
