#include "gui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"
#include <map>
#include <iostream>
#include <cmath>
#include <chrono>
#include <fstream>

static std::map<ImGuiID,ImRect> bounds;
static std::map<ImGuiID,ImGuiWindow*> owners;
static std::map<ImGuiID,ImGuiID> scopes;
static std::map<std::string,ImRect> items;
static std::map<ImGuiID,std::string> labels;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx,ImGuiID id,const ImRect& bb,const ImGuiLastItemData*) {bounds[id]=bb;owners[id]=ctx->CurrentWindow;scopes[id]=ctx->CurrentWindow->IDStack.back();}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID id,const char* label,ImGuiItemStatusFlags) {
    labels[id]=label;
    if(bounds.count(id))items[label]=bounds[id];
}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID id) {return labels[id].c_str();}
static int checks=0;
static void require(bool ok,const char* msg) {++checks;if(!ok)throw std::runtime_error(msg);}
static std::string bytes(const std::filesystem::path& p) { std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}}; }
static void frame(studio::App& app) {
    bounds.clear();owners.clear();items.clear();scopes.clear();ImGui::NewFrame();app.draw();ImGui::Render();app.synchronize();
    require(ImGui::GetDrawData()->TotalVtxCount>100,"GUI emitted no geometry.");
}
static void click_point(studio::App& app,ImVec2 p) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(p.x,p.y);frame(app);
    io.AddMouseButtonEvent(0,true);frame(app);io.AddMouseButtonEvent(0,false);frame(app);
}
static void click(studio::App& app,const char* label,bool activate=true) {
    for(int attempt=0;attempt<6;++attempt) {
        frame(app);
        // This fork omits ItemInfo for combo boxes and tabs. Resolve their real
        // ImGui IDs as well as labels, then click only within the visible panel.
        std::vector<ImGuiID> candidates;
        for(const auto& entry:labels)if(entry.second==label)candidates.push_back(entry.first);
        for(const auto& entry:scopes)if(ImHashStr(label,0,entry.second)==entry.first)candidates.push_back(entry.first);
        for(auto* window:GImGui->Windows)candidates.push_back(window->GetID(label));
        for(int i=0;i<GImGui->TabBars.GetBufSize();++i) {
            candidates.push_back(ImHashStr(label,0,GImGui->TabBars.Buf[i].ID));
            for(const auto& tab:GImGui->TabBars.Buf[i].Tabs)candidates.push_back(ImHashStr(label,0,tab.ID));
        }
        bool found=false;
        for(ImGuiID id:candidates) {
            auto b=bounds.find(id);
            if(b!=bounds.end()) {
                auto* window=owners[id];const auto center=b->second.GetCenter();found=true;
                // A center at the panel edge can lie inside InnerRect but
                // outside the actual content clip rect. Reveal the whole row.
                const auto& clip=window->InnerClipRect;
                if(clip.Contains(center)&&b->second.Min.y>=clip.Min.y&&b->second.Max.y<=clip.Max.y) {
                    if(activate)click_point(app,center);return;
                }
                // Center the target instead of jumping a full long inspector.
                float wheel=(window->InnerRect.GetCenter().y-center.y)/(5.f*ImGui::GetFontSize());
                auto p=window->InnerRect.GetCenter();auto& io=ImGui::GetIO();io.AddMousePosEvent(p.x,p.y);frame(app);
                io.AddMouseWheelEvent(0,wheel);
                frame(app);frame(app);break;
            }
        }
        if(!found)break;
    }
    throw std::runtime_error(std::string("Interaction target is missing: ")+label);
}
static void reveal(studio::App& app,const char* label) {click(app,label,false);}
static void enter_scalar(studio::App& app,const char* label,const char* value) {
    auto& io=ImGui::GetIO();
    // Bring the field into view before holding Ctrl: ImGui intentionally
    // ignores ordinary wheel scrolling while that modifier is down.
    click(app,label);
    io.AddKeyEvent(ImGuiMod_Ctrl,true);frame(app);click(app,label);
    io.AddKeyEvent(ImGuiMod_Ctrl,false);frame(app);
    require(GImGui->TempInputId!=0,(std::string("Ctrl-click did not open scalar text entry: ")+label).c_str());
    io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame(app);
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame(app);
    io.AddInputCharactersUTF8(value);frame(app);
    io.AddKeyEvent(ImGuiKey_Enter,true);frame(app);io.AddKeyEvent(ImGuiKey_Enter,false);frame(app);
}
int main() {
    try {
        ImGui::CreateContext();ImPlot::CreateContext();auto& io=ImGui::GetIO();
        io.DisplaySize={1462,953};io.DeltaTime=1.f/60;io.IniFilename=nullptr;io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
        ImGuiPlatformMonitor monitor;monitor.MainPos=monitor.WorkPos={0,0};monitor.MainSize=monitor.WorkSize=io.DisplaySize;monitor.DpiScale=1;
        ImGui::GetPlatformIO().Monitors.push_back(monitor);GImGui->TestEngineHookItems=true;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",17,nullptr,io.Fonts->GetGlyphRangesCyrillic());
        unsigned char* pixels=nullptr;int w=0,h=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(ImTextureID(1));
        studio::apply_style();studio::App app;frame(app);frame(app);
        auto step_config=studio::preset(4);step_config.sample=.001;step_config.duration=.01;
        app.replace(step_config);frame(app);enter_scalar(app,"Шаг, мкс","500");
        require(app.config.dt==.0005&&app.config.sample==.001&&app.config.noise_interval==.001,"GUI expanded step/unchanged dependent intervals");
        app.advance_for(.002);frame(app);
        require(app.simulation->integration.accepted>0&&app.simulation->integration.mean_step(app.simulation->time)>0,"GUI snapshot lacks internal statistics");
        enter_scalar(app,"Шаг, мкс","250");require(app.simulation->time==0&&app.simulation->integration.accepted==0,"Step edit did not reset statistics/model");
        enter_scalar(app,"Шаг, мкс","500");app.advance_for(.002);const auto before_invalid=app.simulation->state;const auto time_before_invalid=app.simulation->time;click(app,"+ Астроцит");
        require(app.config.dt==.0005&&!app.draft_error.empty()&&app.config.astrocytes.size()==1,"Adding astrocyte silently changed step or allowed run");
        click(app,"Продолжить");require(!app.running&&app.simulation->time==time_before_invalid&&app.simulation->state==before_invalid,"Invalid composition was allowed to run or changed the model");
        click(app,"Уменьшить шаг до 50 мкс");
        require(app.config.dt==50e-6&&app.draft_error.empty()&&app.simulation->error.empty()&&app.simulation->time==0,"Explicit astrocyte step correction failed");
        step_config.dt=.0005;app.replace(step_config);frame(app);
        click(app,"Модель нейрона");click(app,"Hodgkin–Huxley");
        require(app.config.dt==.0005&&!app.draft_error.empty(),"HH switch did not retain explicit invalid step");
        click(app,"Уменьшить шаг до 50 мкс");require(app.simulation->error.empty(),"HH correction failed");
        step_config.dt=25e-6;step_config.sample=100e-6;app.replace(step_config);frame(app);
        enter_scalar(app,"Шаг, мкс","500");
        require(app.config.sample==100e-6&&!app.draft_error.empty(),"GUI silently adjusted sample interval");
        app.replace(studio::preset(0));frame(app);frame(app);
        click(app,"+ Нейрон");require(app.config.neurons.size()==4&&app.selected_kind==0,"Add neuron did not work.");
        click(app,"+ Астроцит");require(app.config.astrocytes.size()==2&&app.selected_kind==1,"Add astrocyte did not work.");
        click(app,"Связи");click(app,"+ Синапс");require(app.config.synapses.size()==3&&app.selected_kind==2,"Add synapse did not work.");
        click(app,"Включён");require(!app.config.synapses.back().enabled,"Synapse toggle did not work.");
        click(app,"Запуск");require(app.running,"Run button did not start simulation.");
        click(app,"Пауза");require(!app.running&&app.simulation->time>0,"Pause did not preserve the trajectory.");
        double paused=app.simulation->time;const auto paused_state=app.simulation->state;const auto paused_generation=app.simulation->generation;
        for(auto mode:studio::compute::ThreadModes) {
            click(app,"Потоки расчёта");click(app,mode.count?mode.argument:"Авто");
            require(app.simulation->metrics.compute_threads==mode.count&&app.simulation->metrics.effective_compute_threads==1,
                "Compute thread combo did not acknowledge requested count and serial fallback.");
            require(app.simulation->time==paused&&app.simulation->generation==paused_generation&&app.simulation->state==paused_state,
                "Runtime thread selection reset or advanced the paused experiment.");
        }
        click(app,"Потоки расчёта");click(app,"1");
        require(app.simulation->metrics.compute_threads==1&&!app.running,"Compute thread combo did not return to serial mode on acknowledgement.");
        click(app,"Шаг 50 мс");require(app.simulation->time>=paused+.05-1e-5,"Step button did not advance time.");
        click(app,"Удалить синапс");require(app.config.synapses.size()==2&&app.simulation->time==0,"Delete did not reset simulation.");
        click(app,"+ Диффузионная связь");require(app.config.gaps.size()==1,"Add astrocyte diffusion link did not work.");
        click(app,"Удалить связь");require(app.config.gaps.empty(),"Delete diffusion link did not work.");
        app.replace(studio::preset(0));click(app,"Шаг 50 мс");frame(app);
        auto canvas=items.at("network_canvas");
        auto point=[&](double x,double y) {return ImVec2(canvas.Min.x+45+float(x)*(canvas.GetWidth()-90),canvas.Min.y+42+float(y)*(canvas.GetHeight()-84));};
        auto before=app.config.neurons[0];double recorded=app.simulation->time;
        click_point(app,point(before.px,before.py));require(app.selected_kind==0&&app.selected_index==0,"Canvas selection failed.");
        io.AddMouseButtonEvent(0,true);frame(app);io.AddMousePosEvent(point(.3,.3).x,point(.3,.3).y);frame(app);
        io.AddMouseButtonEvent(0,false);frame(app);
        require(std::abs(app.config.neurons[0].px-.3)<.02&&app.simulation->time==recorded,"Node drag failed or cleared results.");
        double moved=app.config.neurons[0].px;
        click_point(app,point(.9,.1));io.AddMouseButtonEvent(0,true);frame(app);
        io.AddMousePosEvent(point(.85,.15).x,point(.85,.15).y);frame(app);io.AddMouseButtonEvent(0,false);frame(app);
        require(app.config.neurons[0].px==moved,"Dragging empty canvas moved the selected node.");
        click_point(app,point(.3,.3));io.AddKeyEvent(ImGuiMod_Shift,true);frame(app);
        click_point(app,point(app.config.neurons[1].px,app.config.neurons[1].py));io.AddKeyEvent(ImGuiMod_Shift,false);frame(app);
        require(app.config.synapses.size()==3&&app.config.synapses.back().source==0&&app.config.synapses.back().target==1,"Shift-click connection failed.");
        click(app,"Пресет");click(app,"Ижикевич · RS");
        require(app.config.neurons.size()==1&&app.config.neurons[0].model==studio::NeuronModel::Izhikevich,"Izhikevich preset selector failed.");
        click(app,"Пресет");click(app,"Ижикевич · синапс TM");
        require(app.config.synapses.size()==1&&app.config.synapses[0].model==studio::SynapseModel::TsodyksMarkram,"Tsodyks-Markram preset selector failed.");
        app.replace(studio::preset(3));
        click(app,"Модель нейрона");click(app,"Ижикевич");
        require(app.config.neurons[0].model==studio::NeuronModel::Izhikevich,"Neuron model combo did not select Izhikevich.");
        // The inspector is scrollable; toolbar controls reduce its viewport.
        // Reveal the whole four-field group before checking rendered widgets.
        reveal(app,"d");
        require(items.count("a")&&items.count("b")&&items.count("c, мВ")&&items.count("d"),"Izhikevich parameters are missing.");
        click(app,"u(0) = b · V(0)");
        require(!app.config.neurons[0].equilibrium_u&&items.count("u(0)"),"Explicit recovery initial state is unavailable.");
        click(app,"Индивидуальный шум");
        require(app.config.neurons[0].individual_noise,"Individual neuron noise toggle did not work.");
        app.config.neurons[0].noise_sigma=2.5;
        auto saved=studio::serialize(app.config);app.replace(studio::deserialize(saved));
        require(app.config.neurons[0].model==studio::NeuronModel::Izhikevich&&app.config.neurons[0].individual_noise&&app.config.neurons[0].noise_sigma==2.5,"GUI model configuration did not round trip.");
        app.replace(studio::preset(5));app.selected_kind=2;app.selected_index=0;
        click(app,"Модель синапса");click(app,"Экспоненциальный ток");
        require(app.config.synapses[0].model==studio::SynapseModel::ExponentialCurrent,"Synapse model combo did not select exponential current.");
        reveal(app,"Масштаб A");
        require(items.count("Начальный вес w")&&items.count("Масштаб A")&&!items.count("tau f, мс"),"Exponential current inspector shows the wrong parameters.");
        click(app,"Модель синапса");click(app,"Цодыкс–Маркрам");
        require(app.config.synapses[0].model==studio::SynapseModel::TsodyksMarkram,"Synapse model combo did not select Tsodyks-Markram.");
        enter_scalar(app,"Задержка, мс","1.375");
        click(app,"Время пре-STDP");click(app,"emission — при генерации");
        require(app.config.synapses[0].delay==1.375*.001&&app.config.synapses[0].stdp_timing==studio::StdpTiming::Emission,"Delay/timing inspector or ms conversion failed.");
        click(app,"Шаг 50 мс");
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(app.simulation->time>0&&!app.simulation->history.deliveries.empty(),"GUI delayed delivery did not run.");
        app.request_plot_tab=6;frame(app);frame(app);
        require(GImGui->Tables.GetAliveCount()>0,"Delivery journal did not render.");
        enter_scalar(app,"Задержка, мс","100");
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(!app.simulation->pending_deliveries.empty(),"GUI pending-queue fixture empty.");
        const auto delayed_state=app.simulation->state;const auto delayed_pending=app.simulation->pending_deliveries;
        const auto delayed_file=std::filesystem::current_path()/("gui-delayed-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".nscp");
        app.checkpoint_dialog=[&](bool){return delayed_file;};
        click(app,"Сохранить состояние…");const auto delayed_bytes=bytes(delayed_file);click(app,"Сброс");click(app,"Восстановить состояние…");
        require(!app.running&&app.simulation->state==delayed_state&&app.simulation->pending_deliveries.size()==delayed_pending.size(),"GUI restore lost pending deliveries.");
        app.save_state(delayed_file);require(bytes(delayed_file)==delayed_bytes,"GUI restore changed full checkpoint, including delivery clock.");
        std::filesystem::remove(delayed_file);app.selected_kind=2;app.selected_index=0;
        click(app,"Время пре-STDP");click(app,"arrival — при приходе");
        require(app.simulation->time==0&&app.simulation->pending_deliveries.empty(),"Timing edit failed to reset experiment.");
        enter_scalar(app,"Задержка, мс","2.5");
        require(app.simulation->time==0&&app.simulation->pending_deliveries.empty(),"Editing delay failed to reset experiment.");
        saved=studio::serialize(app.config);app.replace(studio::deserialize(saved));
        require(app.config.synapses[0].model==studio::SynapseModel::TsodyksMarkram,"Synapse model configuration did not round trip.");
        auto c=studio::preset(2);c.duration=3;c.sample=.001;app.replace(c);
        app.advance_for(app.config.duration);require(app.simulation->done()&&app.simulation->error.empty(),"Plot test simulation failed.");
        for(int tab=0;tab<6;++tab) {
            app.request_plot_tab=tab;frame(app);app.request_plot_tab=tab;frame(app);frame(app);
        }
        require(GImPlot->Plots.GetAliveCount()==6,"Not all plot tabs were built.");
        c=studio::preset(5);c.duration=.3;c.sample=.0001;app.replace(c);
        app.advance_for(app.config.duration);require(app.simulation->done()&&app.simulation->error.empty(),"Izhikevich plot test simulation failed.");
        app.request_plot_tab=0;frame(app);app.request_plot_tab=3;frame(app);
        app.request_plot_tab=5;frame(app);frame(app);
        click(app,"Представление");click(app,"Восстановление u(t)");
        click(app,"Представление");click(app,"Фаза V / u");
        require(GImPlot->Plots.GetAliveCount()==8,"Recovery or phase portrait plot is missing.");
        click(app,"Пресет");click(app,"STDP: пост → пре");
        require(app.config.synapses[0].stdp&&app.config.neurons[1].start<app.config.neurons[0].start,"LTD preset selector failed.");
        click(app,"Пресет");click(app,"STDP: пре → пост");
        require(app.config.synapses[0].stdp&&app.selected_kind==2,"STDP preset did not select the learning synapse.");
        click(app,"STDP: обучение веса");require(!app.config.synapses[0].stdp,"STDP checkbox did not disable learning.");
        click(app,"STDP: обучение веса");require(app.config.synapses[0].stdp,"STDP checkbox did not enable learning.");
        click(app,"Правило STDP");click(app,"Мультипликативная");
        require(app.config.synapses[0].stdp_rule==studio::StdpRule::Multiplicative,"STDP rule combo did not select multiplicative learning.");
        enter_scalar(app,"A+","0.035");
        enter_scalar(app,"tau+, мс","30");
        require(std::abs(app.config.synapses[0].a_plus-.035)<1e-12&&std::abs(app.config.synapses[0].tau_plus-.03)<1e-12,"STDP parameter editing or millisecond conversion failed.");
        const double initial_weight=app.config.synapses[0].weight;
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(app.simulation->plasticity[0].weight>initial_weight&&app.config.synapses[0].weight==initial_weight,"Training did not increase live weight independently of initial configuration.");
        require(app.simulation->history.weights[0].size()==app.simulation->history.time.size(),"Weight plot samples are not aligned with time.");
        click(app,"Синапсы");frame(app);frame(app);
        require(GImPlot->Plots.GetAliveCount()==9,"STDP weight plot was not built by the preset's default view.");
        click(app,"График связи");click(app,"Ресурсы и ток");
        click(app,"График связи");click(app,"Вес w(t)");
        // Sandboxed Windows can deny atomic replacement in its redirected TEMP.
        // CTest's working directory is the writable build tree.
        const auto saved_path=std::filesystem::current_path()/("network_studio_gui_stdp_"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
        app.file_dialog=[&](bool,bool){return saved_path;};
        auto checkpoint_path=saved_path;checkpoint_path.replace_extension(".nscp");
        app.checkpoint_dialog=[&](bool){return checkpoint_path;};
        const auto checkpoint_time=app.simulation->time;
        const auto checkpoint_weight=app.simulation->plasticity[0].weight;
        const auto checkpoint_values=app.simulation->history.values;
        const auto checkpoint_state=app.simulation->state;
        click(app,"Сохранить состояние…");
        require(std::filesystem::is_regular_file(checkpoint_path),("Checkpoint button failed: "+app.status).c_str());
        require(app.simulation->time==checkpoint_time&&app.simulation->state==checkpoint_state,"Saving checkpoint altered state.");
        click(app,"Сброс");click(app,"Восстановить состояние…");
        require(!app.running&&app.simulation->time==checkpoint_time&&app.simulation->state==checkpoint_state&&
            app.simulation->plasticity[0].weight==checkpoint_weight&&app.simulation->history.values==checkpoint_values,"GUI restore lost trajectory/training/history.");
        const auto checkpoint_instance=app.simulation->generation;
        app.checkpoint_dialog=[](bool){return std::filesystem::path{};};
        click(app,"Восстановить состояние…");
        require(app.simulation->generation==checkpoint_instance&&app.simulation->time==checkpoint_time,"Cancelled load replaced experiment.");
        app.checkpoint_dialog=[&](bool){return checkpoint_path.parent_path()/"missing-checkpoint.nscp";};
        click(app,"Восстановить состояние…");
        require(app.simulation->generation==checkpoint_instance&&app.simulation->history.values==checkpoint_values,"Failed load replaced experiment.");
        click(app,"Продолжить");require(app.running&&app.simulation->generation==checkpoint_instance,"Continue constructed a new simulator.");
        click(app,"Пауза");require(app.simulation->time>checkpoint_time,"Restored experiment did not advance.");
        app.restore_state(checkpoint_path);
        std::filesystem::remove(checkpoint_path);
        click(app,"Сохранить…");
        require(std::filesystem::is_regular_file(saved_path),("GUI save did not write the STDP configuration: "+app.status).c_str());
        auto saved_config=studio::load_config(saved_path);
        require(saved_config.synapses[0].stdp&&saved_config.synapses[0].stdp_rule==studio::StdpRule::Multiplicative&&saved_config.synapses[0].weight==initial_weight&&
            std::abs(saved_config.synapses[0].a_plus-.035)<1e-12&&std::abs(saved_config.synapses[0].tau_plus-.03)<1e-12,
            "Saving a trained run did not preserve initial weight and STDP settings.");
        click(app,"Сброс");
        require(app.simulation->time==0&&app.simulation->plasticity[0].weight==initial_weight&&
            app.simulation->plasticity[0].pre_trace==0&&app.simulation->plasticity[0].post_trace==0&&
            app.simulation->history.weights[0].size()==1,"Reset did not clear training and restore the initial weight.");
        app.replace(studio::preset(0));click(app,"Открыть…");
        require(app.config.synapses[0].stdp&&app.config.synapses[0].stdp_rule==studio::StdpRule::Multiplicative&&app.config.synapses[0].a_plus==saved_config.synapses[0].a_plus&&
            app.simulation->plasticity[0].weight==initial_weight,"GUI load did not restore the STDP configuration.");
        std::filesystem::remove(saved_path);
        c=studio::preset(6);c.duration=.3;app.replace(c);
        click(app,"Правило STDP");click(app,"Триплетная (минимальная)");
        require(app.config.synapses[0].stdp_rule==studio::StdpRule::Triplet,"Triplet rule is unavailable in the real inspector combo.");
        enter_scalar(app,"tau slow, мс","125");
        require(std::abs(app.config.synapses[0].tau_slow-.125)<1e-12,"Triplet slow time constant lost millisecond conversion.");
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(app.simulation->plasticity[0].post_slow_trace>0&&app.simulation->plasticity[0].weight==1,
            "First isolated pre-post pair incorrectly potentiated the minimal triplet rule.");
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(app.simulation->plasticity[0].weight!=1,"Triplet history did not change the later weight.");
        saved=studio::serialize(app.config);app.replace(studio::deserialize(saved));
        require(app.config.synapses[0].stdp_rule==studio::StdpRule::Triplet&&app.config.synapses[0].tau_slow==.125&&
            app.simulation->plasticity[0].post_slow_trace==0,"Triplet GUI round trip/reset retained history or lost parameters.");
        c=studio::preset(4);c.duration=.2;c.neurons[0].start=0;c.neurons[0].stop=.2;c.neurons[0].frequency=1;app.replace(c);
        click(app,"Таблица импульсов");
        enter_scalar(app,"##pulse_stop_0","53");enter_scalar(app,"##pulse_start_0","50");
        click(app,"+ Интервал");
        enter_scalar(app,"##pulse_stop_1","73");enter_scalar(app,"##pulse_start_1","70");
        require(app.config.neurons[0].pulse_intervals.size()==2&&
            std::abs(app.config.neurons[0].pulse_intervals[0].start-.05)<1e-12&&
            std::abs(app.config.neurons[0].pulse_intervals[1].stop-.073)<1e-12,
            "Stimulus interval table edits failed.");
        saved=studio::serialize(app.config);app.replace(studio::deserialize(saved));
        click(app,"Шаг 50 мс");click(app,"Шаг 50 мс");
        require(app.simulation->spikes[0]>0&&app.simulation->error.empty(),"A GUI-created multi-pulse train did not run.");
        click(app,"Таблица импульсов");
        require(app.config.neurons[0].pulse_intervals.empty()&&app.simulation->time==0,
            "Disabling the interval table did not restore legacy pulses and reset results.");
        c=studio::preset(0);c.synapses[0].reversal=-80;app.replace(c);app.selected_kind=2;
        click(app,"STDP: обучение веса");
        require(!app.config.synapses[0].stdp,"GUI enabled STDP for an inhibitory conductance synapse.");
        app.replace(studio::preset(3));click(app,"Шаг 50 мс");
        const auto before_wizard=studio::serialize(app.config);const double before_wizard_time=app.simulation->time;
        click(app,"Создать сеть…");
        enter_scalar(app,"Число E","6");enter_scalar(app,"Число I","2");
        enter_scalar(app,"##E_a_mean","0.03");enter_scalar(app,"##E_a_sd","0.001");
        enter_scalar(app,"##E_degree_mean","2");enter_scalar(app,"##E_degree_sd","0");
        enter_scalar(app,"##I_degree_mean","2");enter_scalar(app,"##I_degree_sd","0");
        click(app,"Предпросмотр");
        require(studio::serialize(app.config)==before_wizard&&app.simulation->time==before_wizard_time,"Wizard preview mutated the experiment or its results.");
        click(app,"Отмена");
        require(studio::serialize(app.config)==before_wizard&&app.simulation->time==before_wizard_time,"Cancelling the wizard changed the experiment.");
        click(app,"Создать сеть…");click(app,"Предпросмотр");click(app,"Создать");
        require(app.config.neurons.size()==8&&app.config.synapses.size()==16&&app.simulation->time==0&&app.dirty,"Wizard did not apply the exact preview as a new network.");
        for(int i=0;i<8;++i)require(app.config.neurons[i].model==studio::NeuronModel::Izhikevich&&app.config.neurons[i].inhibitory==(i>=6),"Generated E/I neuron type is wrong.");
        require(app.config.neurons[0].izh_a!=.02&&app.config.neurons[0].izh_a!=app.config.neurons[1].izh_a,"Gaussian mean/SD edits were ignored.");
        for(const auto& syn:app.config.synapses)require((syn.weight<0)==app.config.neurons[syn.source].inhibitory&&!syn.stdp,"Generated outgoing polarity or default learning is wrong.");
        const auto first_group=app.config;
        click(app,"Создать сеть…");click(app,"Добавить отдельную группу");
        enter_scalar(app,"Число E","3");enter_scalar(app,"Число I","1");
        click(app,"Связи и расчёт");click(app,"Топология");click(app,"Однонаправленное кольцо");
        click(app,"Синапсы группы");click(app,"Экспоненциальный ток");
        enter_scalar(app,"Задержка, мс","3.125");
        click(app,"Время пре-STDP");click(app,"emission — при генерации");
        click(app,"STDP на возбуждающих связях");
        click(app,"Предпросмотр");click(app,"Создать");
        require(app.config.neurons.size()==12&&app.config.synapses.size()==20,"Wizard append or ring topology has the wrong size.");
        for(int i=0;i<8;++i)require(app.config.neurons[i].izh_a==first_group.neurons[i].izh_a&&app.config.neurons[i].px==first_group.neurons[i].px,"Appending a group changed an existing neuron.");
        for(int i=0;i<16;++i)require(app.config.synapses[i].source==first_group.synapses[i].source&&app.config.synapses[i].weight==first_group.synapses[i].weight,"Appending a group changed an existing connection.");
        for(int i=16;i<20;++i)require(app.config.synapses[i].source>=8&&app.config.synapses[i].target>=8&&app.config.synapses[i].model==studio::SynapseModel::ExponentialCurrent&&app.config.synapses[i].stdp==!app.config.neurons[app.config.synapses[i].source].inhibitory,"Append connected separate groups or ignored the synapse/STDP selector.");
        for(int i=16;i<20;++i)require(app.config.synapses[i].delay==3.125*.001&&app.config.synapses[i].stdp_timing==studio::StdpTiming::Emission,"Wizard did not propagate delay/timing.");
        app.selected_kind=0;app.selected_index=0;click(app,"Записывать нейрон");
        require(!studio::records_neuron(app.config,0)&&!app.config.record_all,"Neuron recording checkbox did not switch to selective recording.");
        click(app,"Записывать нейрон");require(studio::records_neuron(app.config,0),"Neuron recording could not be restored.");
        const auto outgoing_before=app.config.synapses[0];click(app,"Тип для новых связей: I");
        require(app.config.neurons[0].inhibitory&&app.config.synapses[0].weight==outgoing_before.weight,"Changing the default neuron type silently rewrote existing weights.");
        // Selectively recorded conductance without its target voltage must not
        // dereference the empty target column while drawing synaptic currents.
        c=studio::preset(0);c.record_all=false;c.record_neurons={0};c.record_synapses={0};c.duration=.001;c.sample=.0001;
        app.replace(c);app.advance_for(app.config.duration);require(app.simulation->done()&&app.simulation->error.empty(),"Selective GUI fixture failed.");
        require(app.simulation->history.values[c.synapses[0].target*4].empty(),"Fixture target neuron was unexpectedly recorded.");
        app.selected_kind=2;app.selected_index=0;app.request_plot_tab=3;frame(app);frame(app);
        click(app,"Записывать синапс");require(!studio::records_synapse(app.config,0)&&app.simulation->time==0,"Synapse recording checkbox did not reset for a rerun.");
        app.request_plot_tab=3;frame(app);app.request_plot_tab=5;frame(app);
        // Full supported graph size, with only two node/edge traces retained.
        c=studio::preset(3);c.neurons.clear();c.synapses.clear();c.duration=.001;c.sample=.0005;
        c.record_all=false;c.record_neurons={0,999};c.record_synapses={0,49999};
        for(int i=0;i<1000;++i) {
            studio::Neuron node;node.model=studio::NeuronModel::Izhikevich;node.amplitude=0;node.inhibitory=i>=800;
            node.name="Large "+std::to_string(i+1);node.px=(i%40+.5)/40.;node.py=(i/40+.5)/25.;c.neurons.push_back(node);
        }
        for(int i=0;i<50000;++i) {
            studio::Synapse syn;syn.source=i/50;syn.target=(syn.source+i%50+1)%1000;syn.model=studio::SynapseModel::TsodyksMarkram;
            syn.weight=c.neurons[syn.source].inhibitory?-.5:.5;c.synapses.push_back(syn);
        }
        app.replace(c);require(app.simulation->error.empty(),("Large graph GUI fixture is invalid: "+app.simulation->error).c_str());
        app.advance_for(app.config.duration);require(app.simulation->done()&&app.simulation->error.empty(),"Large graph simulation failed.");
        for(int tab=0;tab<6;++tab){app.request_plot_tab=tab;frame(app);frame(app);}
        require(app.simulation->history.values[0].size()==3&&app.simulation->history.values[4].empty(),"Large graph recording selection was ignored.");
        require(ImGui::GetDrawData()->TotalVtxCount<250000,"Large graph drew all edge labels instead of a compact selection.");
        app.selected_kind=2;app.selected_index=49999;app.request_plot_tab=3;frame(app);
        app.selected_kind=0;app.selected_index=1;app.request_plot_tab=5;frame(app);
        enter_scalar(app,"Нейрон графика","2");
        click(app,"Представление");click(app,"Восстановление u(t)");
        click(app,"Представление");click(app,"Фаза V / u");
        for(int i=0;i<GImPlot->Plots.GetBufSize();++i) {
            const auto& plot=GImPlot->Plots.Buf[i];
            for(int axis:{ImAxis_X1,ImAxis_Y1}) {
                auto r=plot.Axes[axis].Range;
                require(std::isfinite(r.Min)&&std::isfinite(r.Max)&&r.Max>r.Min,"Plot has a degenerate or non-finite axis.");
            }
        }
        io.DisplaySize={1120,720};frame(app);frame(app);
        require(items.count("Продолжить")>0&&items.count("network_canvas")>0&&items.count("Восстановить состояние…")>0,"Controls missing at minimum size.");
        const auto completed_state=app.simulation->state;const auto completed_time=app.simulation->time;
        click(app,"Продолжить");require(app.simulation->time==completed_time&&!app.running,"Completed run silently restarted.");
        enter_scalar(app,"Длительность, с","0.002");
        require(app.simulation->state==completed_state&&app.simulation->time==completed_time&&app.config.duration==.002,"Duration extension reset state.");
        click(app,"Шаг 50 мс");require(app.simulation->time==.002,"Completed run did not extend.");
        io.DisplaySize={1600,1000};frame(app);frame(app);
        const auto plot_state=app.simulation->state;const auto plot_steps=app.simulation->steps;
        click(app,"Новый график");frame(app);
        require(items.count("Потенциалы")!=0,"New plot popup has no voltage action.");
        click_point(app,items.at("Потенциалы").GetCenter());frame(app);
        require(app.plot_windows.windows.size()==1,"New plot menu did not create an independent window.");
        const auto plot_id=app.plot_windows.windows.front().id;
        auto copy_id=app.plot_windows.duplicate(plot_id);frame(app);
        app.plot_windows.find(copy_id)->follow=false;app.plot_windows.find(copy_id)->objects={999};frame(app);
        require(app.plot_windows.find(plot_id)->follow&&app.plot_windows.find(plot_id)->objects==std::vector<int>{0},"Duplicate changes propagated to source plot.");
        app.plot_windows.close(copy_id);frame(app);
        require(app.plot_windows.windows.size()==1&&app.simulation->state==plot_state&&app.simulation->steps==plot_steps,"Window commands advanced or reset simulation.");
        app.plot_windows.windows.clear();app.replace(studio::preset(0));frame(app);
        enter_scalar(app,"Длительность, с","200");
        require(app.config.duration==200&&app.simulation->error.empty(),"GUI still caps 200 s experiment or its 800001-row history.");
        enter_scalar(app,"Запись, мс","2000");
        enter_scalar(app,"Длительность, с","3600");
        require(app.config.sample==2&&app.config.duration==3600&&app.simulation->error.empty(),"GUI caps sparse sampling or long experiments.");
        app.selected_kind=0;app.selected_index=0;frame(app);
        enter_scalar(app,"Конец, с","3500");enter_scalar(app,"Начало, с","1000");
        require(app.config.neurons[0].start==1000&&app.config.neurons[0].stop==3500,"GUI caps stimulus times.");
        app.request_plot_tab=0;frame(app);enter_scalar(app,"Окно, с","3600");
        require(app.plot_windows.embedded[0].seconds==3600,"Plot time window remains capped.");
        const auto config_before_field=studio::serialize(app.config);const auto steps_before_field=app.simulation->steps;
        click(app,"Векторное поле весов");click(app,"Обычные связи");click(app,"Сетка");click(app,"Настройки поля…");
        enter_scalar(app,"Столбцы","8");enter_scalar(app,"Строки","6");enter_scalar(app,"Масштаб стрелок","0.08");
        click(app,"##field_source");click(app,"Начальные веса");
        click(app,"Тип связей");click(app,"Тормозные");click(app,"Модель поля");click(app,"Экспоненциальный ток");
        click(app,"Автомасштаб поля");
        const auto& view=app.plot_windows.topology;
        require(view.field&&!view.edges&&!view.grid&&!view.current&&view.columns==8&&view.rows==6&&view.scale==.08&&view.auto_scale&&
            view.polarity==studio::FieldPolarity::Inhibitory&&view.model==studio::FieldModel::ExponentialCurrent,"Field controls did not apply.");
        click(app,"Готово");frame(app);
        const auto field_canvas=items.at("network_canvas");const auto focus=field_canvas.GetCenter();
        io.AddMousePosEvent(focus.x,focus.y);frame(app);frame(app);io.AddMouseWheelEvent(0,1);frame(app);frame(app);
        require(view.zoom>1,"Topology wheel zoom did not work");click(app,"Вписать схему");require(view.zoom==1,"Topology reset view failed");
        require(studio::serialize(app.config)==config_before_field&&app.simulation->steps==steps_before_field,"Field controls edited the model or advanced simulation");
        app.replace(studio::preset(6));app.advance_for(20*app.config.dt);require(app.simulation->error.empty(),"Stage GUI fixture failed");
        const auto stage_state=app.simulation->state;const auto stage_time=app.simulation->time;const auto stage_weight=app.simulation->plasticity[0].weight;
        click(app,"Этапы эксперимента…");click(app,"Обучение включено");
        require(!app.simulation->experiment.learning&&app.simulation->state==stage_state&&app.simulation->time==stage_time&&app.simulation->plasticity[0].weight==stage_weight,"Freeze GUI reset dynamics or weights");
        click(app,"Очистить следы STDP");click(app,"Добавить этап");click(app,"1. Этап 1");
        enter_scalar(app,"Длительность этапа, с","0.02");click(app,"Войти в этап (пауза)");
        require(app.simulation->experiment.stage_active&&app.simulation->time==stage_time&&app.simulation->state==stage_state&&!app.running,"Manual stage GUI did not retain paused state");
        app.show_stages=false;app.request_plot_tab=7;frame(app);frame(app);
        click(app,"Параметры анализа…");enter_scalar(app,"Окно частоты, с","0.025");click(app,"Готово");
        require(app.config.analysis.bin==.025&&app.simulation->time==stage_time&&app.simulation->state==stage_state,"Analysis GUI altered solver");
        auto metric=app.plot_windows.create(studio::PlotKind::Network);auto metric_copy=app.plot_windows.duplicate(metric);
        app.plot_windows.find(metric_copy)->analysis.bin=.1;frame(app);
        require(app.plot_windows.find(metric)->analysis.bin==.025&&app.plot_windows.find(metric_copy)->analysis.bin==.1,"Independent metric settings are shared");
        app.plot_windows.windows.clear();frame(app);click(app,"Профили…");click(app,"Название профиля");
        io.AddInputCharactersUTF8("GUI analysis profile");frame(app);click(app,"Создать из текущих настроек");
        require(app.plot_windows.analysis_profiles.size()==1&&app.plot_windows.embedded[7].analysis_profile=="GUI analysis profile","Profile creation controls failed");
        app.plot_windows.embedded[7].analysis.bin=.05;click(app,"Загрузить профиль");click(app,"GUI analysis profile");
        require(app.plot_windows.embedded[7].analysis.bin==.025,"Profile load control did not restore detector settings");
        click(app,"Применить профиль и интервал ко всем окнам");
        click(app,"Закрыть профили");
        click(app,"До текущего времени");click(app,"От, с##analysis");
        io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame(app);io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddInputCharactersUTF8("0.005");frame(app);io.AddKeyEvent(ImGuiKey_Enter,true);frame(app);io.AddKeyEvent(ImGuiKey_Enter,false);frame(app);
        require(app.plot_windows.embedded[7].analysis_interval.from==.005&&!app.plot_windows.embedded[7].analysis_interval.current_end,"Explicit analysis interval controls failed");
        require(app.simulation->time==stage_time&&app.simulation->state==stage_state,"Profiles/interval UI changed network state");
        app.plot_windows.windows.clear();app.request_network_wizard=true;app.request_wizard_tab=1;frame(app);frame(app);
        click(app,"Топология");click(app,"Связи в пределах радиуса");enter_scalar(app,"Радиус","0.15");click(app,"Предпросмотр");
        require(app.simulation->time==stage_time&&app.simulation->state==stage_state,"Radius preview changed live model");click(app,"Отмена");
        app.plot_windows.windows.clear();app.replace(studio::preset(6));app.config.space={100,100,"um"};app.reset();
        app.advance_for(200*app.config.dt);require(app.simulation->error.empty(),"Stimulator GUI fixture");
        auto source_state=app.simulation->state;const auto source_time=app.simulation->time;const auto source_steps=app.simulation->steps;
        click(app,"Стимуляторы…");click(app,"Добавить стимулятор");click(app,"Выбрать всех получателей");click(app,"Применить без сброса");
        require(app.config.stimulators.size()==1&&app.config.stimulators[0].recipients.size()==app.config.neurons.size(),"Shared source/recipients GUI");
        require(app.simulation->state==source_state&&app.simulation->time==source_time&&app.simulation->steps==source_steps,"Source GUI reset simulation");
        click(app,"Пространственная область");enter_scalar(app,"Радиус области","40");enter_scalar(app,"Центр X","30");click(app,"Применить без сброса");
        require(app.config.stimulators[0].spatial&&app.config.stimulators[0].radius==40&&app.config.stimulators[0].x==30,"Spatial GUI lost physical units");
        app.show_stimulators=false;app.plot_windows.topology=studio::TopologyViewState{};frame(app);
        click(app,"Поле активности");click(app,"Настройки активности…");click(app,"Критерий");click(app,"Доставки с последующим ответом");
        enter_scalar(app,"Окно активности, с","0.125");enter_scalar(app,"Ответ до, с","0.03");enter_scalar(app,"Масштаб активности","0.07");click(app,"Готово");
        require(app.plot_windows.topology.activity_mode==1&&app.plot_windows.topology.activity_window==.125&&app.plot_windows.topology.activity_scale==.07,"Activity GUI settings");
        require(app.simulation->state==source_state&&app.simulation->time==source_time&&app.simulation->steps==source_steps,"Activity GUI altered dynamics");
        frame(app);const auto source_canvas=items.at("network_canvas");
        auto source_point=[&](double x,double y){return ImVec2(source_canvas.Min.x+45+float(x)*(source_canvas.GetWidth()-90),source_canvas.Min.y+42+float(y)*(source_canvas.GetHeight()-84));};
        io.AddMousePosEvent(source_point(.3,.5).x,source_point(.3,.5).y);frame(app);io.AddMouseButtonEvent(0,true);frame(app);
        // Keep the inspector closed during the canvas drag so it cannot cover the target.
        app.show_stimulators=false;io.AddMousePosEvent(source_point(.4,.6).x,source_point(.4,.6).y);frame(app);
        io.AddMouseButtonEvent(0,false);app.show_stimulators=false;frame(app);
        if(std::abs(app.config.stimulators[0].x-40)>=2)std::cerr<<"Drag x="<<app.config.stimulators[0].x<<" y="<<app.config.stimulators[0].y<<" status="<<app.status<<"\n";
        require(std::abs(app.config.stimulators[0].x-40)<2&&app.simulation->state==source_state&&app.simulation->time==source_time,"Spatial drag reset state or failed");
        app.show_stimulators=true;frame(app);click(app,"Удалить стимулятор");require(app.config.stimulators.empty()&&app.simulation->state==source_state,"Source delete reset dynamics");
        // The editor must show normalized, acknowledged parameters, while the
        // current protocol remains distinct from a queued frequency command.
        auto timing=studio::preset(6);timing.duration=.25;timing.sample=.001;
        studio::Stimulator clock;clock.id=1;clock.signal.start=0;clock.first_delay=.05;
        clock.signal.stop=.25;clock.signal.frequency=10;clock.signal.width=.02;clock.recipients={0};
        timing.stimulators={clock};timing.next_stimulator_id=2;app.replace(timing);
        app.selected_stimulator=1;app.show_stimulators=true;frame(app);
        auto initial=app.simulation->state;const auto initial_steps=app.simulation->steps;
        enter_scalar(app,"Частота источника, Гц","20");click(app,"Применить без сброса");
        require(app.stimulator_draft.signal.frequency==20&&app.stimulator_draft.first_delay==.05&&app.config.stimulators[0].first_delay==.05,"GUI pre-first frequency loses delay");
        require(app.simulation->state==initial&&app.simulation->steps==initial_steps,"Source editing advanced network");
        app.advance_for(.06);frame(app);auto active_state=app.simulation->state;const auto active_steps=app.simulation->steps;
        const auto active_weights=app.simulation->history.weights;const auto active_events=app.simulation->history.events.size();const auto deliveries=app.simulation->pending_deliveries.size();
        enter_scalar(app,"Частота источника, Гц","10");click(app,"Применить без сброса");
        require(app.simulation->stimulus_commands.size()==1&&app.config.stimulators[0].signal.frequency==20,"GUI lost distinction between pending/current");
        const auto accepted=app.simulation->stimulus_commands.front();
        require(app.stimulator_draft.signal.start==accepted.value.signal.start&&app.stimulator_draft.first_delay==0&&app.stimulator_draft.signal.frequency==10,"GUI retained unnormalized draft after acknowledgement");
        require(app.simulation->state==active_state&&app.simulation->steps==active_steps&&app.simulation->history.weights==active_weights&&app.simulation->history.events.size()==active_events&&app.simulation->pending_deliveries.size()==deliveries,"GUI command changed dynamics/events/weights");
        app.advance_for(.05);frame(app);require(app.config.stimulators[0].signal.frequency==10&&app.simulation->stimulus_commands.empty(),"GUI pending command not promoted");
        enter_scalar(app,"Начало, с","0.2");enter_scalar(app,"Первый импульс через, с","0.007");
        const auto applied=app.simulation->time;const auto unchanged=app.simulation->state;
        click(app,"Применить с новой фазой");
        require(app.stimulator_draft.signal.start==applied&&app.config.stimulators[0].signal.start==applied&&app.stimulator_draft.first_delay==.007,"GUI restart displays rejected start instead of accepted time");
        require(app.simulation->state==unchanged&&app.simulation->time==applied,"GUI restart reset network");
        const auto gui_checkpoint_dir=std::filesystem::current_path()/("gui-stimulator-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(gui_checkpoint_dir);
        app.save_state(gui_checkpoint_dir/"stimulator_phase.nscp");app.stimulator_draft.signal.frequency=123;
        require(app.restore_state(gui_checkpoint_dir/"stimulator_phase.nscp"),"GUI source restore failed");app.selected_stimulator=1;frame(app);
        require(app.stimulator_draft.signal.start==applied&&app.stimulator_draft.signal.frequency==10,"GUI restore retained stale source draft");
        app.show_stimulators=false;auto archive_config=studio::preset(6);archive_config.duration=.06;archive_config.sample=.0001;archive_config.history_capacity=32;archive_config.bounded_journals=true;app.replace(archive_config);
        const auto archive_root=std::filesystem::current_path()/("gui-archive-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(archive_root);
        const auto archive_path=(archive_root/"history").u8string();std::snprintf(app.archive_path.data(),app.archive_path.size(),"%s",archive_path.c_str());app.archive_rows=32;
        click(app,"История на диске…");const auto before_archive=app.simulation->state;
        click(app,"Включить запись до запуска");require(app.simulation->metrics.archive_active&&app.simulation->state==before_archive,"Archive GUI attach changed model");
        app.advance_for(.03);frame(app);require(app.simulation->metrics.disk_until==app.simulation->time&&app.simulation->metrics.memory_samples_from>0,"Archive GUI range mismatch");
        app.save_state(archive_root/"pause.nscp");require(app.restore_state(archive_root/"pause.nscp"),"Archive GUI restore");frame(app);
        require(!app.simulation->metrics.archive_active&&app.simulation->metrics.archive_required,"Archive resumed implicitly");click(app,"Продолжить прежний архив из checkpoint");app.advance_for(.03);
        app.archive_interval={0,.06,false};frame(app);click(app,"Рассчитать по архиву");
        for(int i=0;i<200&&app.archive_result.time.empty();++i){frame(app);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        require(app.archive_result.error.empty()&&!app.archive_result.time.empty()&&app.archive_result.full_interval,"Archive GUI background analysis failed");
        app.show_archive=false;
        auto weights_config=studio::preset(2);weights_config.duration=.02;weights_config.record_all=false;weights_config.record_synapses={0};app.replace(weights_config);app.advance_for(.003);
        const auto weights_dir=std::filesystem::current_path()/("gui-weights-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(weights_dir);
        app.save_state(weights_dir/"before.nscp");const auto weight_state=app.simulation->state;const auto weight_time=app.simulation->time;const auto weight_generation=app.simulation->generation;
        click(app,"Веса всей сети…");click(app,"Зафиксировать текущие веса");
        require(app.weight_snapshots.size()==1&&app.weight_snapshots[0]->weights.size()==app.config.synapses.size(),"GUI capture omitted unrecorded weights");
        click(app,"Добавить исходные веса");require(app.weight_snapshots.size()==2,"GUI initial snapshot missing");
        click(app,"Включение");click(app,"Выключенные");require(app.weight_result.distributions[0].stats.count==0,"GUI empty filter retained old population");click(app,"Включение");click(app,"Все");require(app.weight_result.distributions[0].stats.count==app.config.synapses.size(),"GUI filter did not restore full population");
        click(app,"Представление веса");click(app,"Модуль |w|");require(app.weight_settings.absolute,"GUI signed/absolute selector failed");
        app.weight_dialog=[&](bool){return weights_dir/std::filesystem::u8path("веса.nsweights");};click(app,"Сохранить снимок весов…");click(app,"Загрузить снимок весов…");require(app.weight_snapshots.size()==3,"GUI snapshot save/load failed");
        app.file_dialog=[&](bool,bool){return weights_dir/"export.csv";};click(app,"Экспорт распределений и чисел…");require(std::filesystem::exists(weights_dir/"export_statistics.json"),"GUI weights export missing");
        click(app,"Убрать снимок");require(app.weight_snapshots.size()==2,"GUI snapshot removal failed");
        app.weight_dialog=[](bool){return std::filesystem::path{};};click(app,"Загрузить снимок весов…");require(app.weight_snapshots.size()==2,"Cancelled weight dialog changed collection");
        studio::write_atomic(weights_dir/"broken.nsweights","{");app.weight_dialog=[&](bool){return weights_dir/"broken.nsweights";};click(app,"Загрузить снимок весов…");require(app.weight_snapshots.size()==2,"Broken weight file changed collection");
        app.weight_reference=1;app.weight_target=2;app.weights_dirty=true;click(app,"Показывать текущий снимок");require(app.weight_reference==0&&app.weight_target==1,"Hiding live weights rebound saved selections");click(app,"Показывать текущий снимок");require(app.weight_reference==1&&app.weight_target==2,"Showing live weights rebound saved selections");
        require(app.simulation->state==weight_state&&app.simulation->time==weight_time&&app.simulation->generation==weight_generation,"Weights UI edited or advanced model");
        app.save_state(weights_dir/"after.nscp");{std::ifstream a(weights_dir/"before.nscp",std::ios::binary),b(weights_dir/"after.nscp",std::ios::binary);require(std::string(std::istreambuf_iterator<char>(a),{})==std::string(std::istreambuf_iterator<char>(b),{}),"Weights GUI changed checkpoint state");}
        app.weight_reference=0;app.weight_target=1;app.weights_dirty=true;frame(app);require(app.weight_result.difference.error.empty(),"Matching GUI snapshots rejected");
        auto incompatible=weights_config;incompatible.neurons[0].name="Different identity";app.replace(incompatible);frame(app);require(!app.weight_result.difference.error.empty(),"Weights GUI silently rebound old snapshot to new graph");
        app.show_weights=false;
        app.replace(studio::animat_example(false));app.advance_for(.08);app.show_animat=true;frame(app);frame(app);
        frame(app);frame(app);require(std::abs(app.animat_render_scale_x-app.animat_render_scale_y)<1,"arena physical x/y scale differs");
        const auto robot_dir=std::filesystem::current_path()/("gui-animat-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(robot_dir);
        app.save_state(robot_dir/"before.nscp");const auto robot_state=app.simulation->state;const auto robot_time=app.simulation->time;
        click(app,"Очистить только рисунок");require(app.animat_display_after==robot_time,"clear trajectory did not change display filter");
        click(app,"Показать доступную траекторию");click(app,"Статистика и экспорт");click(app,"До текущего времени");click(app,"Рассчитать по полной траектории");frame(app);
        require(app.animat_result.complete&&app.animat_result.to==robot_time,"full animat analysis failed");
        app.save_state(robot_dir/"after.nscp");require(bytes(robot_dir/"before.nscp")==bytes(robot_dir/"after.nscp"),"view/analysis changed animat experiment");
        click(app,"Арена и траектория");click(app,"Вернуть в начальную точку");
        require(app.simulation->state==robot_state&&app.simulation->time==robot_time&&app.simulation->animat.x==app.config.animat.initial_x,"home changed network/time or failed to move robot");
        require(app.restore_state(robot_dir/"before.nscp"),"animat GUI restore");app.synchronize();frame(app);
        require(app.simulation->time==robot_time&&app.simulation->animat.actions.empty(),"restore retained later home command");
        app.reset();app.synchronize();frame(app);require(app.simulation->time==0&&app.simulation->animat.rows.size()==1,"animat reset");
        app.advance_for(.08);studio::RunnerCommand environment;environment.kind=studio::RunnerCommand::Kind::AnimatAction;
        environment.animat_action.kind=studio::AnimatActionKind::Environment;environment.animat_action.time=app.simulation->time;environment.animat_action.environment=app.config.animat.environment;
        for(auto& z:environment.animat_action.environment.zones)z.frequency=1;app.command(environment,true);app.advance_for(.1);
        require(app.config.stimulators.front().signal.start>0,"reset phase fixture lacks a phase change");
        click(app,"Настройки контроллера");click(app,"Применить настройки и сбросить опыт");
        require(app.simulation->time==0&&app.config.stimulators.front().signal.start==0,"controller settings reset retained runtime stimulus phase");
        app.show_animat=false;
        ImPlot::DestroyContext();ImGui::DestroyContext();
        std::cout<<"PASS: "<<checks<<" GUI checks, including network wizard/append/cancel, 1000 nodes/50000 edges, selective recording, STDP and real ImGui input.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
