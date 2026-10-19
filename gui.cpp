#include "gui.h"
#include "implot.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace studio {
static const ImVec4 Teal(.25f,.84f,.72f,1),Blue(.38f,.65f,1,1),Gold(1,.74f,.36f,1),Muted(.58f,.65f,.74f,1),Rose(.93f,.43f,.53f,1);
static ImU32 color(ImVec4 v) { return ImGui::ColorConvertFloat4ToU32(v); }
static ImVec2 add(ImVec2 a,ImVec2 b) { return {a.x+b.x,a.y+b.y}; }
static ImVec2 sub(ImVec2 a,ImVec2 b) { return {a.x-b.x,a.y-b.y}; }
static ImVec2 mul(ImVec2 a,float s) { return {a.x*s,a.y*s}; }
void apply_style() {
    ImGui::StyleColorsDark(); auto& s=ImGui::GetStyle();
    s.WindowRounding=0; s.ChildRounding=10; s.FrameRounding=5; s.PopupRounding=7;
    s.FramePadding={8,5}; s.ItemSpacing={8,8}; s.WindowPadding={16,12}; s.ScrollbarSize=12;
    s.Colors[ImGuiCol_WindowBg]={.055f,.073f,.10f,1}; s.Colors[ImGuiCol_ChildBg]={.078f,.105f,.145f,1};
    s.Colors[ImGuiCol_FrameBg]={.12f,.16f,.21f,1}; s.Colors[ImGuiCol_Button]={.14f,.21f,.27f,1};
    s.Colors[ImGuiCol_ButtonHovered]={.17f,.34f,.37f,1}; s.Colors[ImGuiCol_ButtonActive]={.14f,.43f,.40f,1};
    s.Colors[ImGuiCol_Header]={.13f,.29f,.31f,1}; s.Colors[ImGuiCol_HeaderHovered]={.14f,.36f,.39f,1};
    s.Colors[ImGuiCol_CheckMark]=Teal; s.Colors[ImGuiCol_SliderGrab]=Teal; s.Colors[ImGuiCol_TextDisabled]=Muted;
    s.Colors[ImGuiCol_PlotHistogram]=Teal;
    ImPlot::StyleColorsDark(); auto& p=ImPlot::GetStyle(); p.PlotPadding={12,10}; p.LineWeight=1.6f;
    p.Colors[ImPlotCol_PlotBg]={.055f,.078f,.108f,1}; p.Colors[ImPlotCol_FrameBg]={.078f,.105f,.145f,1};
}
App::App() {
    simulation=&display;
    plot_windows.analysis_export_dialog=[this](){return file_dialog?file_dialog(true,true):std::filesystem::path{};};
    plot_windows.analysis_export=[this](const AnalysisSettings& settings,const std::filesystem::path& path){RunnerCommand c;c.kind=RunnerCommand::Kind::ExportAnalysis;c.analysis=settings;c.path=path;command(std::move(c));};
    runner=std::make_unique<SimulationRunner>(config);synchronize();
}
App::~App() {if(runner)runner->shutdown();}
void App::set_display_hz(double value) {display_hz=float(value);runner->set_display_hz(value);}
void App::set_compute_threads(int requested,bool wait) {
    RunnerCommand c;c.kind=RunnerCommand::Kind::ComputeThreads;c.index=requested;
    auto result=command(std::move(c));
    if(wait&&!drawing){const auto out=wait_command(result);if(!out.ok)throw std::runtime_error(out.error);}
}
std::shared_future<RunnerResult> App::command(RunnerCommand value,bool wait) {
    const auto kind=value.kind;auto result=runner->submit(std::move(value));
    pending.push_back({result,kind});
    if(kind==RunnerCommand::Kind::Reset||kind==RunnerCommand::Kind::LoadConfig||kind==RunnerCommand::Kind::Restore)replacing=true;
    // Editing is enabled only after the controller acknowledges the boundary.
    running=true;
    if(wait&&!drawing)wait_command(result);
    return result;
}
RunnerResult App::wait_command(std::shared_future<RunnerResult> result) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(10);
    while(result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
        advance();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Controller acknowledgement timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto out=result.get();advance();
    while(out.snapshot_sequence && (display.generation<out.generation || (display.generation==out.generation&&display.sequence<out.snapshot_sequence))){
        advance();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Display acknowledgement timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}
void App::synchronize() {
    while(!pending.empty()){auto f=pending.front().result;wait_command(f);}
    RunnerCommand c;c.kind=RunnerCommand::Kind::Barrier;
    auto r=wait_command(command(std::move(c)));
    if(!r.ok)throw std::runtime_error(r.error);
}
void App::run(){RunnerCommand c;c.kind=RunnerCommand::Kind::Run;command(std::move(c));status="Выполняется расчёт…";}
void App::pause(){RunnerCommand c;c.kind=RunnerCommand::Kind::Pause;command(std::move(c));status="Ожидание завершения текущего шага…";}
void App::advance_for(double duration){RunnerCommand c;c.kind=RunnerCommand::Kind::Advance;c.value=duration;auto r=wait_command(command(std::move(c)));synchronize();if(!r.ok)throw std::runtime_error(r.error);}
void App::reset() {
    RunnerCommand c;c.kind=RunnerCommand::Kind::Reset;c.config=config;command(std::move(c));
    if(!drawing)synchronize();
}
void App::replace(Config c) {
    config=std::move(c); selected_kind=selected_index=0; preset_index=-1; filename.clear(); dirty=false;
    syn_plot=syn_view=0;
    for(int i=0;i<int(config.synapses.size());++i)if(config.synapses[i].stdp) {selected_kind=2;selected_index=syn_plot=i;syn_view=1;break;}
    link_source=0; link_target=std::min(1,int(config.neurons.size())-1); reset();
    plot_windows.data_changed(config,true);plot_windows.embedded[3].object=syn_plot;plot_windows.embedded[3].view=syn_view;
    status="Конфигурация загружена.";
}
void App::save(const std::filesystem::path& path) {
    try {save_config(config,path);filename=path;dirty=false;status="Конфигурация сохранена.";}catch(const std::exception& e){status=e.what();}
}
void App::load(const std::filesystem::path& path) {
    RunnerCommand c;c.kind=RunnerCommand::Kind::LoadConfig;c.path=path;command(std::move(c));filename=path;
    if(!drawing)synchronize();
}
void App::save_state(const std::filesystem::path& path) {
    if(path.empty())return;RunnerCommand c;c.kind=RunnerCommand::Kind::SaveCheckpoint;c.path=path;command(std::move(c));if(!drawing)synchronize();
}
bool App::restore_state(const std::filesystem::path& path) {
    if(path.empty())return false;RunnerCommand c;c.kind=RunnerCommand::Kind::Restore;c.path=path;auto f=command(std::move(c));
    if(drawing)return true;
    const auto result=wait_command(f);synchronize();return result.ok;
}
void App::export_data(const std::filesystem::path& path) {
    RunnerCommand c;c.kind=RunnerCommand::Kind::Export;c.path=path;command(std::move(c));if(!drawing)synchronize();
}
void App::advance() {
    const bool was_running=display.metrics.running;
    while(auto packet=runner->take_snapshot()) {
        const auto previous=display.generation;
        if(!display.apply(packet))throw std::runtime_error("Display snapshot sequence mismatch");
        if(display_config!=packet->config) {display_config=packet->config;config=display.config;}
        if(previous!=display.generation) {
            activity_field=ActivityField{};stimulator_draft.id=0;drag_kind=drag_index=-1;generation_preview.reset();
            auto binding=config;if(!display.experiment.protocols.empty())binding.neurons=display.experiment.protocols.front().neurons;
            plot_windows.data_changed(binding);plot_windows.embedded[7].analysis=config.analysis;
        }
    }
    for(auto it=pending.begin();it!=pending.end();) {
        if(it->result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){++it;continue;}
        const auto result=it->result.get();const auto kind=it->kind;
        if(result.snapshot_sequence&&(display.generation<result.generation||(display.generation==result.generation&&display.sequence<result.snapshot_sequence))){++it;continue;}
        if(!result.ok){status=result.error;config=display.config;}
        else if(kind==RunnerCommand::Kind::Pause)status="Расчёт приостановлен; текущий шаг завершён.";
        else if(kind==RunnerCommand::Kind::Advance)status="Шаг расчёта завершён.";
        else if(kind==RunnerCommand::Kind::SaveCheckpoint)status="Полное состояние сохранено.";
        else if(kind==RunnerCommand::Kind::ComputeThreads)status="Число вычислительных потоков применено на границе шага.";
        else if(kind==RunnerCommand::Kind::Export||kind==RunnerCommand::Kind::ExportAnalysis||kind==RunnerCommand::Kind::ExportTrained)status="Экспорт завершён.";
        else if(kind==RunnerCommand::Kind::Restore||kind==RunnerCommand::Kind::LoadConfig) {
            if(result.ok){selected_kind=selected_index=0;preset_index=-1;dirty=false;filename.clear();neuron_plot=neuron_view=syn_plot=syn_view=astro_plot=0;link_source=0;link_target=std::min(1,int(config.neurons.size())-1);status="Состояние загружено. Пауза.";}
        } else if(kind==RunnerCommand::Kind::Stimulator||kind==RunnerCommand::Kind::MoveStimulator)status="Команда источника: t="+std::to_string(result.applied_time)+" с.";
        it=pending.erase(it);
    }
    replacing=std::any_of(pending.begin(),pending.end(),[](const Pending& p){return p.kind==RunnerCommand::Kind::Reset||p.kind==RunnerCommand::Kind::LoadConfig||p.kind==RunnerCommand::Kind::Restore;});
    running=display.metrics.running||!pending.empty();
    if(!display.error.empty())status=display.error;
    else if(was_running&&!running&&display.done())status="Расчёт завершён. Результаты можно экспортировать.";
}
static bool scalar(const char* label,double& v,double lo,double hi,double speed=.01,const char* fmt="%.4g") {
    return ImGui::DragScalar(label,ImGuiDataType_Double,&v,float(speed),&lo,&hi,fmt,ImGuiSliderFlags_AlwaysClamp);
}
static bool scalar_min(const char* label,double& v,double lo,double speed=.01) {
    const double before=v;
    if(!ImGui::DragScalar(label,ImGuiDataType_Double,&v,float(speed),&lo,nullptr,"%.8g",ImGuiSliderFlags_AlwaysClamp))return false;
    v=std::isfinite(v)?std::max(lo,v):before;
    return v!=before;
}
static bool delay_settings(Synapse& s) {
    bool changed=false;double ms=s.delay*1000;
    if(scalar("Задержка, мс",ms,0,120000,.1)){s.delay=ms*.001;changed=true;}
    int mode=int(s.stdp_timing);const char* modes[]={"arrival — при приходе","emission — при генерации"};
    if(ImGui::Combo("Время пре-STDP",&mode,modes,2)){s.stdp_timing=StdpTiming(mode);changed=true;}
    ImGui::TextWrapped("arrival: пре-след при доставке; emission: при спайке источника. Пост-время — спайк приёмника. Доставка в обоих режимах: t emit + задержка.");
    return changed;
}
static bool text(const char* label,std::string& value,size_t limit=64) {
    char buf[256]{}; std::snprintf(buf,sizeof(buf),"%s",value.c_str());
    if(ImGui::InputText(label,buf,std::min(sizeof(buf),limit+1))) {value=buf; return true;} return false;
}
static void title(const char* label) { ImGui::TextColored(Muted,"%s",label); ImGui::Separator(); }
static void set_recorded(Config& c,bool neuron,int index,bool enabled) {
    if(c.record_all) {
        c.record_neurons.clear();c.record_synapses.clear();
        for(int i=0;i<int(c.neurons.size());++i)c.record_neurons.push_back(i);
        for(int i=0;i<int(c.synapses.size());++i)c.record_synapses.push_back(i);
        c.record_all=false;
    }
    auto& indices=neuron?c.record_neurons:c.record_synapses;
    indices.erase(std::remove(indices.begin(),indices.end(),index),indices.end());
    if(enabled)indices.push_back(index);
    std::sort(indices.begin(),indices.end());
}
static Synapse new_synapse(const Config& c,int source,int target,int astro=-1) {
    Synapse s;s.source=source;s.target=target;s.astro=astro;
    if(c.neurons[source].inhibitory)s.reversal=-80;
    return s;
}
static bool combo(const char* label,int& value,const Config& c,bool astro=false,bool none=false) {
    const int count=int(astro?c.astrocytes.size():c.neurons.size());
    if(value>=count)value=none?-1:std::max(0,count-1);
    std::string preview=value<0?"Без астроцита":(value<count?(astro?c.astrocytes[value].name:c.neurons[value].name):"—");
    bool change=false;
    if(ImGui::BeginCombo(label,preview.c_str())) {
        if(none && ImGui::Selectable("Без астроцита",value<0)){value=-1;change=true;}
        ImGuiListClipper clipper;clipper.Begin(count);
        while(clipper.Step())for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) {
            ImGui::PushID(i); const auto& name=astro?c.astrocytes[i].name:c.neurons[i].name;
            if(ImGui::Selectable(name.c_str(),value==i)){value=i;change=true;} ImGui::PopID();
        } ImGui::EndCombo();
    } return change;
}
void App::preview_network(const NetworkGeneration& settings,bool append) {
    auto preview=std::make_unique<Config>(generate_network(config,settings,append,&generation_summary));
    generation=settings;generation_append=append;generation_preview=std::move(preview);generation_error.clear();
}
void App::network_wizard() {
    if(request_network_wizard) {
        generation_error.clear();
        ImGui::OpenPopup("Мастер сети Ижикевича");request_network_wizard=false;
    }
    const auto area=ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize({std::min(960.f,area.x-40),std::min(820.f,area.y-40)},ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});
    if(!ImGui::BeginPopupModal("Мастер сети Ижикевича",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    bool modified=false;
    ImGui::TextWrapped("Задайте популяции и распределения. Предпросмотр создаёт точный образец с выбранным Seed; эксперимент изменится только после нажатия «Создать».");
    if(ImGui::RadioButton("Заменить текущую сеть",!generation_append)){generation_append=false;modified=true;}
    ImGui::SameLine();if(ImGui::RadioButton("Добавить отдельную группу",generation_append)){generation_append=true;modified=true;}
    ImGui::TextDisabled(generation_append?"Существующие узлы и связи сохранятся. Между группами связи не создаются.":"Новая сеть заменит текущую конфигурацию; результаты расчёта будут сброшены.");
    ImGui::BeginChild("wizard_settings",{0,std::max(230.f,ImGui::GetContentRegionAvail().y-155)},ImGuiChildFlags_Borders);
    if(ImGui::BeginTabBar("wizard_tabs")) {
        if(ImGui::BeginTabItem("Популяции и распределения")) {
            if(ImGui::BeginTable("populations",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV)) {
                auto population=[&](const char* id,const char* heading,PopulationGeneration& p,ImVec4 col) {
                    ImGui::TableNextColumn();ImGui::PushID(id);ImGui::TextColored(col,"%s",heading);
                    ImGui::SetNextItemWidth(120);
                    std::string count_label=std::string("Число ")+id;
                    modified|=ImGui::DragInt(count_label.c_str(),&p.count,1,0,MaxNeurons,"%d",ImGuiSliderFlags_AlwaysClamp);
                    if(ImGui::BeginTable("gaussians",3,ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Параметр",ImGuiTableColumnFlags_WidthFixed,92);
                        ImGui::TableSetupColumn("Среднее");ImGui::TableSetupColumn("Отклонение σ");ImGui::TableHeadersRow();
                        auto distribution=[&](const char* label,const char* key,GaussianParameter& g,double lo,double hi,double speed) {
                            ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(label);
                            ImGui::TableNextColumn();ImGui::SetNextItemWidth(-1);
                            std::string mean_id="##"+std::string(id)+"_"+key+"_mean",sd_id="##"+std::string(id)+"_"+key+"_sd";
                            modified|=scalar(mean_id.c_str(),g.mean,lo,hi,speed);
                            ImGui::TableNextColumn();ImGui::SetNextItemWidth(-1);
                            modified|=scalar(sd_id.c_str(),g.sd,0,hi-lo,speed);
                        };
                        distribution("a","a",p.a,.0001,1,.001);
                        distribution("b","b",p.b,-2,2,.001);
                        distribution("c, мВ","c",p.c,-100,0,.1);
                        distribution("d","d",p.d,-100,100,.1);
                        distribution("Входов k","degree",p.in_degree,0,MaxNeurons-1,1);
                        distribution("|Вес|","weight",p.weight,0,1,.01);
                        ImGui::EndTable();
                    }
                    ImGui::PopID();
                };
                population("E","Возбуждающие (E)",generation.excitatory,Teal);
                population("I","Тормозные (I)",generation.inhibitory,Rose);
                ImGui::EndTable();
            }
            ImGui::TextWrapped("σ = 0 задаёт постоянное значение. Число входов относится к нейрону-приёмнику; модуль веса — к типу источника. Веса исходящих E-связей положительные, I-связей отрицательные.");
            if(ImGui::Button("Входы 15 ± 2")){generation.excitatory.in_degree=generation.inhibitory.in_degree={15,2};modified=true;}
            ImGui::SameLine();if(ImGui::Button("Входы 30 ± 3")){generation.excitatory.in_degree=generation.inhibitory.in_degree={30,3};modified=true;}
            ImGui::TextWrapped("Нормальное распределение ограничено допустимыми значениями: недопустимый параметр выбирается заново. Число входов округляется и ограничивается размером группы. Самосвязи и дубликаты исключены.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Связи и расчёт",nullptr,request_wizard_tab==1?ImGuiTabItemFlags_SetSelected:0)) {
            ImGui::PushItemWidth(240);
            const char* topologies[]={"Случайная сеть","Ближайшие соседи","Однонаправленное кольцо","Без связей","Связи в пределах радиуса"};
            int topology=int(generation.topology);
            if(ImGui::Combo("Топология",&topology,topologies,5)){generation.topology=NetworkTopology(topology);modified=true;}
            if(generation.topology==NetworkTopology::DirectedRing)ImGui::TextWrapped("При двух и более нейронах каждый соединён со следующим, последний — с первым. Один нейрон остаётся без связей. Распределение числа входов не используется.");
            if(generation.topology==NetworkTopology::NearestNeighbors)ImGui::TextWrapped("Источники выбираются по близости на сгенерированной плоскости. Число входов задаётся распределением.");
            modified|=scalar("Ширина пространства",generation.space.width,1e-9,1e9,.1);
            modified|=scalar("Высота пространства",generation.space.height,1e-9,1e9,.1);
            modified|=text("Единицы модели",generation.space.unit,32);
            if(generation.topology==NetworkTopology::WithinRadius)modified|=scalar("Радиус",generation.radius,0,1e9,.01);
            modified|=ImGui::Checkbox("Разрешить самосвязи (кроме кольца)",&generation.allow_self);
            ImGui::TextWrapped("X = px × ширина, Y = py × высота. Границы открытые, без заворачивания. При нехватке соседей используются доступные; дальние связи не добавляются.");
            modified|=ImGui::InputScalar("Seed генератора",ImGuiDataType_U32,&generation.seed);
            int synapse=generation.synapse.model==SynapseModel::TsodyksMarkram?0:1;
            const char* synapses[]={"Цодыкс–Маркрам (TM)","Экспоненциальный ток"};
            if(ImGui::Combo("Синапсы группы",&synapse,synapses,2)){generation.synapse.model=synapse==0?SynapseModel::TsodyksMarkram:SynapseModel::ExponentialCurrent;modified=true;}
            modified|=scalar("Масштаб g",generation.synapse.gain,0,10000,.1);
            modified|=delay_settings(generation.synapse);
            double tau=generation.synapse.tau_syn*1000;
            if(scalar("Затухание, мс",tau,1,10000,.1)){generation.synapse.tau_syn=tau*.001;modified=true;}
            if(generation.synapse.model==SynapseModel::TsodyksMarkram) {
                modified|=scalar("Использование U",generation.synapse.release,0,1,.01);
                tau=generation.synapse.tau_d*1000;
                if(scalar("Восстановление, мс",tau,1,10000,1)){generation.synapse.tau_d=tau*.001;modified=true;}
                tau=generation.synapse.tau_f*1000;
                if(scalar("Фасилитация, мс",tau,1,10000,1)){generation.synapse.tau_f=tau*.001;modified=true;}
            }
            modified|=ImGui::Checkbox("STDP на возбуждающих связях",&generation.synapse.stdp);
            if(generation.synapse.stdp) {
                const char* rules[]={"Аддитивная","Мультипликативная","Триплетная (минимальная)"};int rule=int(generation.synapse.stdp_rule);
                if(ImGui::Combo("Правило STDP группы",&rule,rules,3)){generation.synapse.stdp_rule=StdpRule(rule);modified=true;}
                modified|=scalar("STDP A+",generation.synapse.a_plus,0,1,.001);
                modified|=scalar("STDP A−",generation.synapse.a_minus,0,1,.001);
                tau=generation.synapse.tau_plus*1000;
                if(scalar("STDP tau+, мс",tau,1,10000,.5)){generation.synapse.tau_plus=tau*.001;modified=true;}
                tau=generation.synapse.tau_minus*1000;
                if(scalar("STDP tau−, мс",tau,1,10000,.5)){generation.synapse.tau_minus=tau*.001;modified=true;}
                if(generation.synapse.stdp_rule==StdpRule::Triplet) {
                    tau=generation.synapse.tau_slow*1000;
                    if(scalar("STDP tau slow, мс",tau,1,10000,1)){generation.synapse.tau_slow=tau*.001;modified=true;}
                    ImGui::TextWrapped("Минимальная триплетная: LTP зависит от прежнего медленного следа постспайков; LTD — парная.");
                }
                modified|=scalar("STDP минимум w",generation.synapse.weight_min,0,generation.synapse.weight_max-1e-6,.01);
                modified|=scalar("STDP максимум w",generation.synapse.weight_max,generation.synapse.weight_min+1e-6,10,.01);
                ImGui::TextWrapped("Обучаются только E-связи. Для мультипликативного правила изменение масштабируется расстоянием до границы веса. Начальные E-веса должны лежать между выбранными границами.");
            } else ImGui::TextDisabled(generation.synapse.model==SynapseModel::TsodyksMarkram?"Обучение весов выключено; короткая пластичность TM сохраняется.":"Обучение весов выключено.");
            ImGui::Separator();ImGui::BeginDisabled(generation_append);
            modified|=text("Имя новой сети",generation.name,120);
            modified|=scalar_min("Расчёт, с",generation.duration,.001,.1);
            double sample=generation.sample*1000;
            if(scalar_min("Запись новой сети, мс",sample,.025,.1)){generation.sample=sample*.001;modified=true;}
            ImGui::EndDisabled();ImGui::PopItemWidth();
            ImGui::TextWrapped(generation_append?"Новая группа создаётся без импульсных входов; общий шум и параметры текущего расчёта сохраняются. Добавьте стимулы нейронам в инспекторе.":"Новая сеть создаётся без внешних стимулов и шума. Добавьте вход нейрону в инспекторе.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if(modified){generation_preview.reset();generation_error.clear();}
    if(ImGui::Button("Предпросмотр",{150,0})) {
        try {generation_preview=std::make_unique<Config>(generate_network(config,generation,generation_append,&generation_summary));generation_error.clear();}
        catch(const std::exception& e){generation_preview.reset();generation_error=e.what();}
    }
    ImGui::SameLine();ImGui::BeginDisabled(!generation_preview);
    if(ImGui::Button("Создать",{120,0})) {
        auto c=std::move(*generation_preview);generation_preview.reset();replace(std::move(c));dirty=true;
        status=generation_append?"Группа добавлена. Настройте стимул в инспекторе.":"Сеть создана. Настройте стимул в инспекторе.";
        ImGui::CloseCurrentPopup();
    }ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Отмена",{120,0})){generation_preview.reset();generation_error.clear();ImGui::CloseCurrentPopup();}
    if(!generation_error.empty())ImGui::TextWrapped("%s",generation_error.c_str());
    if(generation_preview) {
        const auto& g=generation_summary;
        ImGui::Text("Предпросмотр: %d E + %d I · %d связей · всего %zu нейронов",g.excitatory_count,g.inhibitory_count,g.new_synapses,generation_preview->neurons.size());
        ImGui::Text("Входы: %.2f ± %.2f [%.0f; %.0f] · |вес|: %.3f ± %.3f [%.3f; %.3f]",g.degrees.mean,g.degrees.sd,g.degrees.min,g.degrees.max,g.absolute_weights.mean,g.absolute_weights.sd,g.absolute_weights.min,g.absolute_weights.max);
        ImGui::Text("Не хватило соседей: %d · длина: %.3g ± %.3g [%.3g; %.3g]",g.insufficient_neighbors,g.lengths.mean,g.lengths.sd,g.lengths.min,g.lengths.max);
        ImGui::Text("R=%.4g · %.4g × %.4g %s · seed=%u",generation.radius,generation.space.width,generation.space.height,generation.space.unit.c_str(),generation.seed);
        if(ImGui::Button("Распределения образца…"))ImGui::OpenPopup("generated_distributions");
        if(ImGui::BeginPopup("generated_distributions")) {
            if(ImPlot::BeginPlot("Число входов",{600,200})){ImPlot::PlotHistogram("Входы",g.in_degrees.data(),int(g.in_degrees.size()),20);ImPlot::EndPlot();}
            if(ImPlot::BeginPlot("Длины и веса",{600,220})){ImPlot::PlotHistogram("Длины",g.edge_lengths.data(),int(g.edge_lengths.size()),30);ImPlot::PlotHistogram("Веса (со знаком)",g.signed_weights.data(),int(g.signed_weights.size()),30);ImPlot::EndPlot();}
            ImGui::Text("E: %.4g ± %.4g; I: %.4g ± %.4g",g.excitatory_weights.mean,g.excitatory_weights.sd,g.inhibitory_weights.mean,g.inhibitory_weights.sd);ImGui::EndPopup();
        }
        ImGui::TextDisabled("Ограничено степеней: %d · повторных выборок: %zu",g.degree_clamped,g.parameter_resamples);
        if(!generation_preview->record_all)ImGui::TextWrapped("Выборочная запись: %zu нейронов и %zu связей; события всех узлов сохраняются. Состав записи меняется в инспекторе.",generation_preview->record_neurons.size(),generation_preview->record_synapses.size());
    } else if(generation_error.empty())ImGui::TextDisabled("Нажмите «Предпросмотр», чтобы проверить сеть и увидеть фактические распределения.");
    request_wizard_tab=-1;ImGui::EndPopup();
}
void App::toolbar() {
    ImGui::TextColored(Teal,"NEURO / ASTRO"); ImGui::SameLine(); ImGui::TextDisabled("Конструктор малых сетей");
    ImGui::SameLine(ImGui::GetWindowWidth()-365); ImGui::TextDisabled("HH / Ижикевич · IP3 / Ca · RK4 · CPU");
    ImGui::Separator();
    ImGui::BeginDisabled(!simulation->error.empty() || (!running && simulation->done()));
    ImGui::PushStyleColor(ImGuiCol_Button,{.10f,.38f,.33f,1});
    if(ImGui::Button(running?"Пауза":simulation->time>0?"Продолжить":"Запуск",{110,30})) {
        if(running)pause();else run();
    } ImGui::PopStyleColor(); ImGui::EndDisabled();
    ImGui::SameLine(); if(ImGui::Button("Сброс",{80,30})) {RunnerCommand c;c.kind=RunnerCommand::Kind::Reset;c.flag=true;command(std::move(c));status="Восстанавливаются начальные состояния…";}
    ImGui::SameLine(); ImGui::BeginDisabled(running||!simulation->error.empty()||simulation->done());
    if(ImGui::Button("Шаг 50 мс",{110,30})) {
        RunnerCommand c;c.kind=RunnerCommand::Kind::Advance;c.value=.05;command(std::move(c));status="Выполняется шаг 50 мс…";
    } ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(running);
    if(ImGui::Button("Открыть…",{95,30})&&file_dialog) {auto path=file_dialog(false,false);if(!path.empty())load(path);}
    ImGui::SameLine(); if(ImGui::Button("Сохранить…",{110,30})&&file_dialog) {auto path=file_dialog(true,false);if(!path.empty())save(path);}
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(running||simulation->sample_begin+simulation->history.time.size()<2);
    if(ImGui::Button("Экспорт CSV…",{125,30})&&file_dialog) {
        auto path=file_dialog(true,true);
        if(!path.empty()) try { path.replace_extension(); export_data(path); }
        catch(const std::exception& e){status=e.what();}
    } ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextColored(running?Teal:Muted,"  %.3f / %.3f с",simulation->time,config.duration);
    ImGui::BeginDisabled(running);
    if(ImGui::Button("Сохранить состояние…")&&checkpoint_dialog) {auto path=checkpoint_dialog(true);if(!path.empty())save_state(path);}
    ImGui::SameLine();
    if(ImGui::Button("Восстановить состояние…")&&checkpoint_dialog) {auto path=checkpoint_dialog(false);if(!path.empty())restore_state(path);}
    ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Этапы эксперимента…"))show_stages=true;
    ImGui::SameLine();if(ImGui::Button("Стимуляторы…"))show_stimulators=true;
    const auto& m=simulation->metrics;
    ImGui::Text("Скорость %.5g модельных с/с · расчёт %.1f с · осталось ≈ %.1f с",m.model_per_second,m.elapsed_seconds,m.eta_seconds);
    ImGui::SameLine();ImGui::SetNextItemWidth(115);
    if(ImGui::SliderFloat("Обновление, Гц",&display_hz,1,20,"%.0f"))set_display_hz(display_hz);
    const auto& modes=compute::ThreadModes;
    const char* thread_names[compute::ThreadModes.size()];
    int thread_choice=0;for(size_t i=0;i<modes.size();++i){thread_names[i]=modes[i].count?modes[i].argument:"Авто";if(modes[i].count==m.compute_threads)thread_choice=int(i);}
    const bool threads_pending=std::any_of(pending.begin(),pending.end(),[](const Pending& p){return p.kind==RunnerCommand::Kind::ComputeThreads;});
    ImGui::SetNextItemWidth(95);ImGui::BeginDisabled(threads_pending);
    if(ImGui::Combo("Потоки расчёта",&thread_choice,thread_names,IM_ARRAYSIZE(thread_names)))set_compute_threads(modes[thread_choice].count);
    ImGui::EndDisabled();
    ImGui::SameLine();ImGui::TextDisabled("Фактически: %d%s",m.effective_compute_threads,m.effective_compute_threads==1&&m.compute_threads!=1?" · последовательный режим":"");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Число включает поток управления решателем. Малые сети и неподдерживаемые конфигурации используют последовательный режим.");
    if(running&&!m.running)ImGui::TextDisabled("Ожидание подтверждения вычислителя…");
    ImGui::ProgressBar(float(simulation->time/config.duration),{-1,4},"");
}
void App::editor() {
    title("ЭКСПЕРИМЕНТ"); ImGui::BeginDisabled(running); ImGui::PushItemWidth(-115);
    if(ImGui::Button("Создать сеть…",{-1,0}))request_network_wizard=true;
    const char* names[]={"2 → 1 + астроцит","Без обратной связи","4 нейрона + 2 астроцита","Пустой эксперимент","Ижикевич · RS","Ижикевич · синапс TM","STDP: пре → пост","STDP: пост → пре"};
    if(ImGui::BeginCombo("Пресет",preset_index>=0?names[preset_index]:"Загруженная сеть")) {
        for(int i=0;i<IM_ARRAYSIZE(names);++i)if(ImGui::Selectable(names[i],preset_index==i)){replace(preset(i));preset_index=i;}
        ImGui::EndCombo();
    }
    if(text("Название",config.name,120))edit();
    double end_time=config.duration;
    if(scalar_min("Длительность, с",end_time,.001,.1)) {
        if(simulation->time==0){config.duration=end_time;edit();}
        else try {RunnerCommand c;c.kind=RunnerCommand::Kind::Extend;c.value=end_time;command(std::move(c));dirty=true;}
        catch(const std::exception& e){status=e.what();}
    }
    if(simulation->time>0)ImGui::TextDisabled("Конечное время от t=0; можно увеличить.");
    double dt=config.dt*1e6;
    if(scalar("Шаг, мкс",dt,1,50,1,"%.1f")){config.dt=dt*1e-6;edit();}
    double sample=config.sample*1000;
    if(scalar_min("Запись, мс",sample,config.dt*1000,.05)){config.sample=sample*.001;edit();}
    ImGui::PopItemWidth();
    if(ImGui::Checkbox("Записывать все состояния",&config.record_all))edit();
    const auto rows=planned_samples(config.duration,config.sample);
    if(rows<=MaxSamples)ImGui::TextDisabled("История: %zu / 1 000 000 отсчётов",rows);
    else ImGui::TextDisabled("История: более 1 000 000 отсчётов");
    bool bounded=config.bounded_journals;
    if(ImGui::Checkbox("Ограниченные журналы",&bounded)) {
        RunnerCommand c;c.kind=RunnerCommand::Kind::BoundedJournals;c.flag=bounded;command(std::move(c));
    }
    ImGui::TextWrapped(config.bounded_journals?"Старые события удаляются; доступная глубина видна в анализе и журнале этапов.":"Полные журналы: лимит 2 млн записей каждого типа. Для длинных опытов включите ограниченные журналы заранее.");
    if(!config.record_all)ImGui::TextDisabled("Запись: %zu нейронов, %zu связей; события всех узлов",config.record_neurons.size(),config.record_synapses.size());
    ImGui::TextDisabled("%zu нейронов · %zu астроцитов · %zu синапсов",config.neurons.size(),config.astrocytes.size(),config.synapses.size());
    ImGui::Spacing();
    if(ImGui::BeginTabBar("objects")) {
        if(ImGui::BeginTabItem("Узлы")) {
            ImGui::BeginDisabled(config.neurons.size()>=MaxNeurons);
            if(ImGui::Button("+ Нейрон")) {
                Neuron n; n.name="Neuron "+std::to_string(config.neurons.size()+1);n.amplitude=0;
                n.px=.3+.08*(config.neurons.size()%5);n.py=.35+.1*(config.neurons.size()%3);
                config.neurons.push_back(n);selected_kind=0;selected_index=int(config.neurons.size())-1;edit();
            } ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(config.astrocytes.size()>=MaxAstrocytes);
            if(ImGui::Button("+ Астроцит")) {
                Astrocyte a;a.name="Astro "+std::to_string(config.astrocytes.size()+1);a.px=.4+.1*(config.astrocytes.size()%4);
                config.astrocytes.push_back(a);selected_kind=1;selected_index=int(config.astrocytes.size())-1;edit();
            } ImGui::EndDisabled();
            title("НЕЙРОНЫ");
            const bool long_nodes=config.neurons.size()>20;
            if(long_nodes)ImGui::BeginChild("neuron_list",{0,180},ImGuiChildFlags_Borders);
            ImGuiListClipper neurons;neurons.Begin(int(config.neurons.size()));
            while(neurons.Step())for(int i=neurons.DisplayStart;i<neurons.DisplayEnd;++i) {
                ImGui::PushID(i);std::string label="N"+std::to_string(i+1)+"   "+config.neurons[i].name+(config.neurons[i].inhibitory?"  I":"  E");
                if(ImGui::Selectable(label.c_str(),selected_kind==0&&selected_index==i)){selected_kind=0;selected_index=i;}
                ImGui::PopID();
            }
            if(long_nodes)ImGui::EndChild();
            title("АСТРОЦИТЫ");
            for(int i=0;i<int(config.astrocytes.size());++i) {
                ImGui::PushID(100+i);std::string label="A"+std::to_string(i+1)+"   "+config.astrocytes[i].name;
                if(ImGui::Selectable(label.c_str(),selected_kind==1&&selected_index==i)){selected_kind=1;selected_index=i;}
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Связи")) {
            ImGui::PushItemWidth(-95);
            combo("От нейрона",link_source,config); combo("К нейрону",link_target,config);
            combo("Астроцит",link_astro,config,true,true);
            ImGui::PopItemWidth(); ImGui::BeginDisabled(config.synapses.size()>=MaxSynapses);
            if(ImGui::Button("+ Синапс",{-1,0})) {
                config.synapses.push_back(new_synapse(config,link_source,link_target,link_astro));selected_kind=2;selected_index=int(config.synapses.size())-1;edit();
            } ImGui::EndDisabled();
            const bool long_synapses=config.synapses.size()>20;
            if(long_synapses)ImGui::BeginChild("synapse_list",{0,180},ImGuiChildFlags_Borders);
            ImGuiListClipper synapses;synapses.Begin(int(config.synapses.size()));
            while(synapses.Step())for(int i=synapses.DisplayStart;i<synapses.DisplayEnd;++i) {
                const auto& s=config.synapses[i]; ImGui::PushID(i);
                std::string label="S"+std::to_string(i+1)+"   N"+std::to_string(s.source+1)+" → N"+std::to_string(s.target+1);
                if(s.astro>=0)label+=" / A"+std::to_string(s.astro+1);
                if(!s.enabled)label+=" (выкл.)";
                if(ImGui::Selectable(label.c_str(),selected_kind==2&&selected_index==i)){selected_kind=2;selected_index=i;} ImGui::PopID();
            }
            if(long_synapses)ImGui::EndChild();
            title("ОБМЕН МЕЖДУ АСТРОЦИТАМИ");
            ImGui::BeginDisabled(config.astrocytes.size()<2||config.gaps.size()>=MaxGaps);
            if(ImGui::Button("+ Диффузионная связь",{-1,0})) {
                bool added=false;
                for(int a=0;a<int(config.astrocytes.size())&&!added;++a)for(int b=a+1;b<int(config.astrocytes.size())&&!added;++b) {
                    bool found=false; for(const auto& g:config.gaps)if((g.a==a&&g.b==b)||(g.a==b&&g.b==a))found=true;
                    if(!found){config.gaps.push_back({a,b});selected_kind=3;selected_index=int(config.gaps.size())-1;edit();added=true;}
                } if(!added)status="Все пары астроцитов уже связаны.";
            } ImGui::EndDisabled();
            for(int i=0;i<int(config.gaps.size());++i) {
                auto& g=config.gaps[i]; ImGui::PushID(200+i);
                std::string label="A"+std::to_string(g.a+1)+" ↔ A"+std::to_string(g.b+1);
                if(ImGui::Selectable(label.c_str(),selected_kind==3&&selected_index==i)){selected_kind=3;selected_index=i;} ImGui::PopID();
            } ImGui::EndTabItem();
        } ImGui::EndTabBar();
    }
    ImGui::Spacing();
    if(ImGui::CollapsingHeader("Шумовой вход")) {
        ImGui::PushItemWidth(-115);
        if(scalar("σ общий",config.noise_sigma,0,100,.1))edit();
        double interval=config.noise_interval*1000;
        if(scalar("Интервал, мс",interval,config.dt*1000,1000,.1)){config.noise_interval=interval*.001;edit();}
        if(ImGui::InputScalar("Seed",ImGuiDataType_U32,&config.seed))edit();
        ImGui::TextWrapped("Независимый гауссов шум для каждого нейрона. Значение удерживается на интервале. Seed обеспечивает воспроизводимость; σ можно переопределить в инспекторе.");
        ImGui::PopItemWidth();
    }
    if(ImGui::CollapsingHeader("Общие параметры модели")) {
        ImGui::TextWrapped("Биофизика HH и астроцитов. Параметры Ижикевича, входов, синапсов и обратной связи задаются в инспекторе.");
        ImGui::PushItemWidth(105);
        for(int i:SharedParameters)if(ImGui::InputDouble(na::parameter_names[i],&config.parameters[i],0,0,"%.6g"))edit();
        ImGui::PopItemWidth();
    } ImGui::EndDisabled();
}
void App::inspector() {
    title("ИНСПЕКТОР"); ImGui::BeginDisabled(running); ImGui::PushItemWidth(-132);
    if(selected_kind==0 && selected_index>=0 && selected_index<int(config.neurons.size())) {
        auto& n=config.neurons[selected_index]; ImGui::TextColored(Teal,"N%d · Нейрон",selected_index+1);
        const char* models[]={"Hodgkin–Huxley","Ижикевич"};int model=int(n.model);
        if(ImGui::Combo("Модель нейрона",&model,models,2)){n.model=NeuronModel(model);edit();}
        if(text("Имя",n.name))edit();
        if(ImGui::Checkbox("Тип для новых связей: I",&n.inhibitory))edit();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Тип задаёт знак новых исходящих связей. Уже созданные связи сохраняют свои веса и потенциалы реверсии.");
        bool record=records_neuron(config,selected_index);
        if(ImGui::Checkbox("Записывать нейрон",&record)){set_recorded(config,true,selected_index,record);edit();}
        title("НАЧАЛЬНОЕ СОСТОЯНИЕ");
        if(scalar("V, мВ",n.voltage,-100,60,.1))edit();
        if(n.model==NeuronModel::Izhikevich) {
            if(ImGui::Checkbox("u(0) = b · V(0)",&n.equilibrium_u))edit();
            if(!n.equilibrium_u&&scalar("u(0)",n.izh_u,-1000,1000,.1))edit();
            title("ПАРАМЕТРЫ ИЖИКЕВИЧА");
            if(scalar("a",n.izh_a,.0001,1,.001))edit();
            if(scalar("b",n.izh_b,-2,2,.001))edit();
            if(scalar("c, мВ",n.izh_c,-100,0,.1))edit();
            if(scalar("d",n.izh_d,-100,100,.1))edit();
            if(scalar("Пик, мВ",n.izh_peak,0,100,.1))edit();
            ImGui::TextWrapped("При достижении пика: V ← c, u ← u + d. Ток задан в единицах модели. Уравнения модели записаны для времени в миллисекундах.");
        } else ImGui::TextDisabled("m, h, n: стационарные при V(0)");
        title("ВНЕШНИЙ ТОК");
        if(scalar(n.model==NeuronModel::Izhikevich?"DC, ед. модели":"DC, мкА/см²",n.current,-30,100,.1))edit();
        if(scalar(n.model==NeuronModel::Izhikevich?"Импульс, ед. модели":"Импульс, мкА/см²",n.amplitude,-30,100,.1))edit();
        if(scalar("Частота, Гц",n.frequency,0,200,.5))edit();
        if(scalar("Начало, с",n.start,0,n.stop,.1))edit();
        if(scalar_min("Конец, с",n.stop,n.start,.1))edit();
        double width=n.width*1000;
        ImGui::BeginDisabled(!n.pulse_intervals.empty());
        if(scalar("Ширина, мс",width,0,1000,.1)){n.width=width*.001;edit();}
        ImGui::EndDisabled();
        bool interval_table=!n.pulse_intervals.empty();
        if(ImGui::Checkbox("Таблица импульсов",&interval_table)) {
            if(interval_table)n.pulse_intervals.push_back({0,std::min(std::max(n.width,.000001),n.frequency>0?1/n.frequency:1.)});
            else n.pulse_intervals.clear();
            edit();
        }
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Интервалы внутри каждого периода, относительно поля «Начало, с».\nГраницы [от, до); амплитуда общая. Пустая таблица использует поле «Ширина».\nНапример: Начало=0, частота=1 Гц, интервалы 50–53 и 70–73 мс.");
        if(interval_table) {
            ImGui::TextWrapped("Интервалы от начала периода, мс. Упорядочены, не пересекаются.");
            const double period=n.frequency>0?1/n.frequency:std::numeric_limits<double>::max();
            if(ImGui::BeginTable("pulse_intervals",3,ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("От, мс");ImGui::TableSetupColumn("До, мс");ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,25);
                ImGui::TableHeadersRow();int remove=-1;
                for(int i=0;i<int(n.pulse_intervals.size());++i) {
                    auto& interval=n.pulse_intervals[i];ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);ImGui::SetNextItemWidth(-1);
                    double start_ms=interval.start*1000;
                    if(scalar_min(("##pulse_start_"+std::to_string(i)).c_str(),start_ms,0,.1)){interval.start=start_ms*.001;edit();}
                    ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);
                    double stop_ms=interval.stop*1000;
                    if(scalar_min(("##pulse_stop_"+std::to_string(i)).c_str(),stop_ms,0,.1)){interval.stop=stop_ms*.001;edit();}
                    ImGui::TableSetColumnIndex(2);
                    if(ImGui::SmallButton(("×##pulse_delete_"+std::to_string(i)).c_str()))remove=i;
                }
                ImGui::EndTable();
                if(remove>=0){n.pulse_intervals.erase(n.pulse_intervals.begin()+remove);edit();}
            }
            const double after=n.pulse_intervals.empty()?0:n.pulse_intervals.back().stop;
            const double next_start=std::min(after+.010,(after+period)*.5);
            ImGui::BeginDisabled(n.pulse_intervals.size()>=64 || next_start>=period);
            if(ImGui::Button("+ Интервал")) {
                n.pulse_intervals.push_back({next_start,std::min(next_start+.003,period)});edit();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();ImGui::TextDisabled("Период %.3g мс",period*1000);
        }
        ImGui::TextWrapped("Частота 0 отключает импульсы. Постоянный ток DC остаётся активным.");
        if(ImGui::Checkbox("Индивидуальный шум",&n.individual_noise))edit();
        if(n.individual_noise) {if(scalar("σ нейрона",n.noise_sigma,0,100,.1))edit();}
        else ImGui::TextDisabled("σ общий = %.3g",config.noise_sigma);
        ImGui::Spacing(); ImGui::BeginDisabled(config.neurons.size()==1);
        if(!config.stimulators.empty()) {
            ImGui::TextWrapped("Общие источники: HH — мкА/см²; Ижикевич — единицы модели. DC, индивидуальный протокол и шум добавляются отдельно.");
            for(const auto& source:config.stimulators){auto list=stimulator_recipients(config,source);if(std::find(list.begin(),list.end(),selected_index)!=list.end())ImGui::TextWrapped("#%llu %s: %.6g",source.id,source.name.c_str(),pulse_current(stimulator_signal(source),simulation->time));}
        }
        if(ImGui::Button("Удалить нейрон",{-1,0})) {remove_neuron(config,selected_index);selected_kind=selected_index=0;edit();}
        ImGui::EndDisabled();
    } else if(selected_kind==1 && selected_index<int(config.astrocytes.size())) {
        auto& a=config.astrocytes[selected_index];ImGui::TextColored(Blue,"A%d · IP3 / Li–Rinzel",selected_index+1);
        if(text("Имя",a.name))edit();
        title("НАЧАЛЬНОЕ СОСТОЯНИЕ");
        if(scalar("IP3, мкМ",a.ip3,0,10,.01))edit();
        double maxCa=config.parameters[na::C_T]/(1+config.parameters[na::rho_A]);
        if(scalar("Ca, мкМ",a.calcium,0,maxCa,.005))edit();
        if(scalar("hA",a.h,0,1,.01))edit();
        title("ГЛИОТРАНСМИССИЯ");
        if(scalar("Порог Ca, мкМ",a.threshold,1e-6,maxCa,.005))edit();
        if(ImGui::Checkbox("Обратная связь",&a.feedback))edit();
        if(scalar("alpha",a.alpha,0,1,.01))edit();
        if(scalar("g SIC, мСм/см²",a.sic,0,1,.001))edit();
        ImGui::TextWrapped("alpha задаёт вероятность выброса при полной активации рецепторов. g SIC добавляет медленный ток в нейроны, получающие связанные с астроцитом синапсы.");
        if(ImGui::Button("Удалить астроцит",{-1,0})) {remove_astrocyte(config,selected_index);selected_kind=selected_index=0;edit();}
    } else if(selected_kind==2 && selected_index<int(config.synapses.size())) {
        auto& s=config.synapses[selected_index];ImGui::TextColored(Gold,"S%d · Синапс",selected_index+1);
        bool record=records_synapse(config,selected_index);
        if(ImGui::Checkbox("Записывать синапс",&record)){set_recorded(config,false,selected_index,record);edit();}
        const char* models[]={"Проводимость + STP","Экспоненциальный ток","Цодыкс–Маркрам"};int model=int(s.model);
        if(ImGui::Combo("Модель синапса",&model,models,3)){s.model=SynapseModel(model);edit();}
        if(combo("Источник",s.source,config))edit(); if(combo("Приёмник",s.target,config))edit();
        if(combo("Астроцит",s.astro,config,true,true))edit(); if(ImGui::Checkbox("Включён",&s.enabled))edit();
        if(delay_settings(s))edit();
        if(scalar("Начальный вес w",s.weight,s.stdp?s.weight_min:(s.model==SynapseModel::ConductanceSTP?0:-10),s.stdp?s.weight_max:10,.01))edit();
        if(!changed&&selected_index<int(simulation->plasticity.size()))
            ImGui::TextColored(Teal,"Текущий вес: %.6g",simulation->plasticity[selected_index].weight);
        if(s.model==SynapseModel::ConductanceSTP) {
            if(scalar("g, мСм/см²",s.conductance,0,5,.01))edit();
            if(scalar("E rev, мВ",s.reversal,-100,50,1))edit();
        } else {
            if(scalar("Масштаб A",s.gain,0,10000,.1))edit();
        }
        const bool learnable=s.source!=s.target&&s.weight>=0&&
            (s.model!=SynapseModel::ConductanceSTP||s.reversal>-40);
        ImGui::BeginDisabled(!learnable&&!s.stdp);
        if(ImGui::Checkbox("STDP: обучение веса",&s.stdp)) {
            if(s.stdp) {
                s.weight_min=std::min(s.weight_min,s.weight);s.weight_max=std::max(s.weight_max,s.weight);
                syn_plot=selected_index;syn_view=1;plot_windows.embedded[3].object=syn_plot;plot_windows.embedded[3].view=1;
            }
            edit();
        }
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("STDP доступна для возбуждающих связей между разными нейронами.");
        if(s.stdp) {
            const char* rules[]={"Аддитивная","Мультипликативная","Триплетная (минимальная)"};int rule=int(s.stdp_rule);
            if(ImGui::Combo("Правило STDP",&rule,rules,3)){s.stdp_rule=StdpRule(rule);edit();}
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Аддитивная: A × след. Мультипликативная: ещё × расстояние до границы.\nТриплетная минимальная (пособие, с.9, (12)):\nLTP = A+ × (w max − w) × пре-след × прежний медленный пост-след.\nLTD остаётся парной; отдельного парного LTP нет.");
            if(scalar("A+",s.a_plus,0,1,.001))edit();
            if(scalar("A−",s.a_minus,0,1,.001))edit();
            double tau=s.tau_plus*1000;
            if(scalar("tau+, мс",tau,1,10000,.5)){s.tau_plus=tau*.001;edit();}
            tau=s.tau_minus*1000;
            if(scalar("tau−, мс",tau,1,10000,.5)){s.tau_minus=tau*.001;edit();}
            if(s.stdp_rule==StdpRule::Triplet) {
                tau=s.tau_slow*1000;
                if(scalar("tau slow, мс",tau,1,10000,1)){s.tau_slow=tau*.001;edit();}
                ImGui::TextWrapped("LTP использует медленный след до текущего постспайка. Изолированная первая пара не даёт LTP без прежней постактивности.");
            }
            if(scalar("Минимум w",s.weight_min,0,std::min(s.weight,s.weight_max-1e-6),.01))edit();
            if(scalar("Максимум w",s.weight_max,std::max(s.weight,s.weight_min+1e-6),10,.01))edit();
            ImGui::TextWrapped("Δt = t пост − t пре выбранного режима. Одновременные события используют прежние следы, без взаимной пары с нулевым лагом. Изменение параметров сбрасывает опыт; сохранение конфигурации записывает начальный вес.");
        }
        if(s.model!=SynapseModel::ExponentialCurrent&&scalar("U base",s.release,0,1,.01))edit();
        double tau=s.tau_syn*1000; if(scalar("tau syn, мс",tau,1,10000,.1)){s.tau_syn=tau*.001;edit();}
        if(s.model!=SynapseModel::ExponentialCurrent) {
            tau=s.tau_f*1000; if(scalar("tau f, мс",tau,1,10000,1)){s.tau_f=tau*.001;edit();}
            tau=s.tau_d*1000; if(scalar("tau d, мс",tau,1,10000,1)){s.tau_d=tau*.001;edit();}
        }
        if(scalar("Spillover",s.spill,0,1,.001))edit();
        if(s.model==SynapseModel::ConductanceSTP)
            ImGui::TextWrapped("E rev ≤ −40 мВ: тормозная связь, без выброса глутамата. Синапс может быть прикреплён к одному астроциту.");
        else if(s.model==SynapseModel::ExponentialCurrent)
            ImGui::TextWrapped("I = w · A · s. Каждая доставка увеличивает s на 1; между доставками s затухает с tau syn. Текущий обученный w влияет и на существующий хвост тока. Отрицательный w задаёт торможение.");
        else ImGui::TextWrapped("I = w · A · y. Ресурсы: x — доступные, y — активные, z = 1 − x − y. u задаёт использование ресурса; tau f — фасилитация, tau d — восстановление.");
        if(ImGui::Button("Удалить синапс",{-1,0})) {
            auto& indices=config.record_synapses;
            indices.erase(std::remove(indices.begin(),indices.end(),selected_index),indices.end());
            for(int& i:indices)if(i>selected_index)--i;
            config.synapses.erase(config.synapses.begin()+selected_index);selected_kind=selected_index=0;edit();
        }
    } else if(selected_kind==3 && selected_index<int(config.gaps.size())) {
        auto& g=config.gaps[selected_index];ImGui::TextColored(Blue,"Диффузионная связь");
        if(combo("Астроцит A",g.a,config,true))edit(); if(combo("Астроцит B",g.b,config,true))edit();
        if(scalar("D IP3, с⁻¹",g.ip3,0,10,.01))edit(); if(scalar("D Ca, с⁻¹",g.calcium,0,10,.01))edit();
        ImGui::TextWrapped("Симметричный обмен D·(X_B − X_A) между компартментами одинакового объёма.");
        if(ImGui::Button("Удалить связь",{-1,0})) {config.gaps.erase(config.gaps.begin()+selected_index);selected_kind=selected_index=0;edit();}
    } else { selected_kind=selected_index=0; }
    ImGui::PopItemWidth(); ImGui::EndDisabled();
    title("АКТИВНОСТЬ");
    if(!changed && simulation->error.empty()) {
        if(selected_kind==0 && selected_index<int(simulation->spikes.size())) {
            ImGui::Text("V = %.2f мВ",simulation->state[selected_index*4]);
            if(simulation->config.neurons[selected_index].model==NeuronModel::Izhikevich)
                ImGui::Text("u = %.3f",simulation->state[selected_index*4+1]);
            ImGui::Text("Спайков: %llu",static_cast<unsigned long long>(simulation->spikes[selected_index]));
        } else if(selected_kind==1 && selected_index<int(simulation->releases.size())) {
            int b=astro_offset(simulation->config,selected_index);
            ImGui::Text("Ca = %.4f мкМ",simulation->state[b+2]);ImGui::Text("IP3 = %.4f мкМ",simulation->state[b+1]);
            ImGui::Text("Выбросов: %llu",static_cast<unsigned long long>(simulation->releases[selected_index]));
        }
    }
    ImGui::Spacing(); ImGui::TextDisabled("Изменение модели сбрасывает расчёт.");
}
static void arrow(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 c,float weight=1.8f,float radius=25) {
    ImVec2 delta=sub(b,a); float len=std::sqrt(delta.x*delta.x+delta.y*delta.y);if(len<1)return;
    ImVec2 unit=mul(delta,1/len),normal={-unit.y,unit.x}; a=add(a,mul(unit,radius));b=sub(b,mul(unit,radius));
    d->AddLine(a,b,c,weight); d->AddTriangleFilled(b,add(sub(b,mul(unit,9)),mul(normal,4)),sub(sub(b,mul(unit,9)),mul(normal,4)),c);
}
static void dashed(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 c) {
    ImVec2 v=sub(b,a);float len=std::sqrt(v.x*v.x+v.y*v.y);if(len<1)return;v=mul(v,1/len);
    for(float t=0;t<len;t+=10)d->AddLine(add(a,mul(v,t)),add(a,mul(v,std::min(t+4,len))),c,1.3f);
}
void App::canvas() {
    const bool compact=config.neurons.size()>32;
    auto& view=plot_windows.topology;
    ImGui::TextColored(Muted,"ТОПОЛОГИЯ");ImGui::SameLine();
    if(compact) {
        ImGui::Checkbox("Все связи",&view.all_edges);
        ImGui::SameLine();ImGui::TextDisabled("E: зелёный · I: розовый");
        ImGui::TextDisabled("Клик: выбрать · Показаны связи выбранного узла");
    } else ImGui::TextDisabled("Перетащите узел · Shift + клик: связать · ПКМ: добавить");
    field_controls();
    ImVec2 origin=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();size.x=std::max(size.x,100.f);size.y=std::max(size.y,100.f);
    ImGui::InvisibleButton("network_canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    bool hovered=ImGui::IsItemHovered(),active=ImGui::IsItemActive();auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();
    d->PushClipRect(origin,add(origin,size),true); d->AddRectFilled(origin,add(origin,size),IM_COL32(14,22,31,255),8);
    const float dpi=ImGui::GetFontSize()/17;const ImVec2 pad{45*dpi,42*dpi},extent{std::max(1.f,size.x-90*dpi),std::max(1.f,size.y-84*dpi)};
    if(hovered&&io.MouseWheel!=0) {
        const double qx=(io.MousePos.x-origin.x-pad.x)/extent.x,qy=(io.MousePos.y-origin.y-pad.y)/extent.y;
        const double x=(qx-.5-view.pan_x)/view.zoom,y=(qy-.5-view.pan_y)/view.zoom;
        view.zoom=std::clamp(view.zoom*std::pow(1.15,io.MouseWheel),.25,8.);
        view.pan_x=std::clamp(qx-.5-x*view.zoom,-8.,8.);view.pan_y=std::clamp(qy-.5-y*view.zoom,-8.,8.);
    }
    if(active&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        view.pan_x=std::clamp(view.pan_x+io.MouseDelta.x/extent.x,-8.,8.);
        view.pan_y=std::clamp(view.pan_y+io.MouseDelta.y/extent.y,-8.,8.);
    }
    const ImVec2 base=add(origin,{pad.x+extent.x*float(.5-.5*view.zoom+view.pan_x),pad.y+extent.y*float(.5-.5*view.zoom+view.pan_y)});
    const ImVec2 span=mul(extent,float(view.zoom));
    auto pos=[&](double x,double y) {return add(base,{float(x)*span.x,float(y)*span.y});};
    if(view.grid) {
        for(int x=0;x<=view.columns;++x)d->AddLine(pos(double(x)/view.columns,0),pos(double(x)/view.columns,1),IM_COL32(36,47,61,200));
        for(int y=0;y<=view.rows;++y)d->AddLine(pos(0,double(y)/view.rows),pos(1,double(y)/view.rows),IM_COL32(36,47,61,200));
    }
    std::vector<ImVec2> n,a;
    for(const auto& v:config.neurons)n.push_back(pos(v.px,v.py));for(const auto& v:config.astrocytes)a.push_back(pos(v.px,v.py));
    int hit_kind=-1,hit=-1;float nearest=compact?8.f:26.f;
    auto test=[&](ImVec2 p,int kind,int index) {auto v=sub(io.MousePos,p);float len=std::sqrt(v.x*v.x+v.y*v.y);if(len<nearest){nearest=len;hit_kind=kind;hit=index;}};
    for(int i=0;i<int(n.size());++i)test(n[i],0,i);for(int i=0;i<int(a.size());++i)test(a[i],1,i);
    auto linehit=[&](ImVec2 p,ImVec2 q,int kind,int index) {
        if(hit_kind>=0 && hit_kind<2)return;
        ImVec2 v=sub(q,p),w=sub(io.MousePos,p);float l=v.x*v.x+v.y*v.y;
        float t=l>0?std::clamp((w.x*v.x+w.y*v.y)/l,0.f,1.f):0;
        ImVec2 delta=sub(io.MousePos,add(p,mul(v,t)));float dist=std::sqrt(delta.x*delta.x+delta.y*delta.y);
        if(dist<8){hit_kind=kind;hit=index;}
    };
    if(view.edges)for(int i=0;i<int(config.gaps.size());++i) {
        const auto& g=config.gaps[i];bool sel=selected_kind==3&&selected_index==i;
        dashed(d,a[g.a],a[g.b],color(sel?Gold:Blue));linehit(a[g.a],a[g.b],3,i);
    }
    if(view.edges)for(int i=0;i<int(config.synapses.size());++i) {
        const auto& s=config.synapses[i];bool sel=selected_kind==2&&selected_index==i;
        if(compact&&!view.all_edges&&!sel&&!(selected_kind==0&&(s.source==selected_index||s.target==selected_index)))continue;
        ImVec2 p=n[s.source],q=n[s.target];
        bool inhibitory=s.model==SynapseModel::ConductanceSTP?s.reversal<=-40:s.weight<0;
        ImU32 col=color(sel?Gold:(s.enabled?(inhibitory?ImVec4(.88f,.45f,.52f,1):Teal):Muted));
        ImVec2 center=mul(add(p,q),.5f);
        if(s.source==s.target) {float r=compact?9.f:22.f;d->AddCircle(add(p,{0,-r-7}),r,col,compact?12:28,2);center=add(p,{0,-2*r-7});}
        else { arrow(d,p,q,col,sel?3:(compact?1.f:1.8f),compact?5.f:25.f);linehit(p,q,2,i); }
        if(!compact||sel) {char buf[20];std::snprintf(buf,sizeof(buf),"S%d",i+1);d->AddText(add(center,{4,-19}),col,buf);}
        if(s.astro>=0)dashed(d,center,a[s.astro],IM_COL32(94,144,220,s.enabled?165:60));
    }
    field_overlay(base,span,hovered&&hit_kind<0);
    const bool source_hit=stimulator_overlay(base,span,hovered,active);
    for(int i=0;i<int(n.size());++i) {
        bool sel=selected_kind==0&&selected_index==i;double v=-65;
        const auto node_color=config.neurons[i].inhibitory?Rose:Teal;
        if(!changed&&simulation->error.empty()&&i<int(simulation->config.neurons.size()))v=simulation->state[i*4];
        const float radius=compact?4.5f:25.f;
        d->AddCircleFilled(n[i],radius,compact?color(node_color):IM_COL32(17,35,42,255),compact?10:36);
        d->AddCircle(n[i],radius+(compact&&sel?2:0),color(sel?Gold:node_color),compact?12:36,sel?2.5f:1.5f);
        if(v>0)d->AddCircle(n[i],radius+4,IM_COL32(98,245,204,180),compact?12:36,2);
        if(!compact||sel) {
            char id[20];std::snprintf(id,sizeof(id),"N%d",i+1);d->AddText(add(n[i],compact?ImVec2(8,-10):ImVec2(-10,-8)),color(node_color),id);
            if(!compact) {
                d->AddText(add(n[i],{-25,32}),IM_COL32(214,227,241,255),config.neurons[i].name.c_str());
                char voltage[30];std::snprintf(voltage,sizeof(voltage),"%.1f mV",v);d->AddText(add(n[i],{-25,52}),color(Muted),voltage);
            }
        }
    }
    for(int i=0;i<int(a.size());++i) {
        bool sel=selected_kind==1&&selected_index==i;
        d->AddQuadFilled(add(a[i],{0,-27}),add(a[i],{27,0}),add(a[i],{0,27}),add(a[i],{-27,0}),IM_COL32(24,38,63,255));
        d->AddQuad(add(a[i],{0,-27}),add(a[i],{27,0}),add(a[i],{0,27}),add(a[i],{-27,0}),color(sel?Gold:Blue),sel?3:1.8f);
        char id[20];std::snprintf(id,sizeof(id),"A%d",i+1);d->AddText(add(a[i],{-10,-8}),color(Blue),id);
        d->AddText(add(a[i],{-28,32}),IM_COL32(214,227,241,255),config.astrocytes[i].name.c_str());
        if(!changed&&simulation->error.empty()&&i<int(simulation->config.astrocytes.size())) {
            char ca[30];std::snprintf(ca,sizeof(ca),"Ca %.3f uM",simulation->state[astro_offset(simulation->config,i)+2]);
            d->AddText(add(a[i],{-35,52}),color(Muted),ca);
        }
    }
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {drag_kind=-1;drag_index=-1;}
    if(hovered && !source_hit && !running && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit>=0) {
        if(io.KeyShift && selected_kind==0 && hit_kind==0 && config.synapses.size()<MaxSynapses) {
            config.synapses.push_back(new_synapse(config,selected_index,hit));selected_kind=2;selected_index=int(config.synapses.size())-1;edit();
        } else {selected_kind=hit_kind;selected_index=hit;drag_kind=hit_kind;drag_index=hit;}
    }
    if(active && !source_hit && !running && !io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left,3) && drag_kind>=0 && drag_kind<2) {
        double* x=nullptr;double* y=nullptr;
        if(drag_kind==0){x=&config.neurons[drag_index].px;y=&config.neurons[drag_index].py;}
        else {x=&config.astrocytes[drag_index].px;y=&config.astrocytes[drag_index].py;}
        // Layout changes preserve the numerical trajectory.
        *x=std::clamp(double(io.MousePos.x-base.x)/span.x,0.,1.);
        *y=std::clamp(double(io.MousePos.y-base.y)/span.y,0.,1.);dirty=true;
        RunnerCommand c;c.kind=drag_kind==0?RunnerCommand::Kind::MoveNeuron:RunnerCommand::Kind::MoveAstro;c.index=drag_index;c.value=*x;c.second=*y;command(std::move(c));
    }
    d->PopClipRect();
    if(hovered&&hit_kind==0&&hit>=0&&!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const auto& node=config.neurons[hit];
        ImGui::BeginTooltip();ImGui::Text("N%d · %s · %s",hit+1,node.name.c_str(),node.inhibitory?"I / тормозный":"E / возбуждающий");
        if(!changed&&simulation->error.empty()&&hit<int(simulation->spikes.size()))ImGui::Text("V %.3f мВ · Спайков %llu",simulation->state[hit*4],static_cast<unsigned long long>(simulation->spikes[hit]));
        ImGui::TextDisabled(records_neuron(config,hit)?"Состояние записывается":"Состояние не записывается; события сохраняются");ImGui::EndTooltip();
    }
    if(hovered&&!running&&ImGui::IsMouseClicked(ImGuiMouseButton_Right))ImGui::OpenPopup("canvas_menu");
    if(ImGui::BeginPopup("canvas_menu")) {
        if(ImGui::MenuItem("Добавить нейрон",nullptr,false,config.neurons.size()<MaxNeurons)) {
            Neuron v;v.name="Neuron "+std::to_string(n.size()+1);v.amplitude=0;config.neurons.push_back(v);edit();
        }
        if(ImGui::MenuItem("Добавить астроцит",nullptr,false,config.astrocytes.size()<MaxAstrocytes)) {
            Astrocyte v;v.name="Astro "+std::to_string(a.size()+1);config.astrocytes.push_back(v);edit();
        } ImGui::EndPopup();
    }
}
void App::plots(float height) {
    const char* names[]={"Потенциалы","Астроциты","Растр событий","Синапсы","Фазовый портрет","Нейрон / вход","Доставки","Сеть / стимуляция"};
    if(ImGui::Button("Новый график"))ImGui::OpenPopup("new_plot");
    if(ImGui::BeginPopup("new_plot")) {
        for(int i=0;i<8;++i)if(ImGui::MenuItem(names[i]))plot_windows.create(PlotKind(i));
        ImGui::EndPopup();
    }
    ImGui::SameLine();if(ImGui::Button("Рабочее пространство…"))ImGui::OpenPopup("workspace_menu");
    if(ImGui::BeginPopup("workspace_menu")) {
        ImGui::MenuItem("Область docking",nullptr,&plot_windows.host_open);
        if(ImGui::MenuItem("Пример: четыре графика"))plot_windows.demo();
        if(ImGui::MenuItem("Сохранить рабочее пространство…")&&workspace_dialog)plot_windows.save(workspace_dialog(true));
        if(ImGui::MenuItem("Открыть рабочее пространство…")&&workspace_dialog)plot_windows.load(workspace_dialog(false));
        if(ImGui::MenuItem("Сбросить расположение окон"))plot_windows.reset_layout();
        if(!plot_windows.message.empty())ImGui::TextWrapped("%s",plot_windows.message.c_str());
        ImGui::EndPopup();
    }
    ImGui::SameLine();ImGui::TextDisabled("Колесо: масштаб · ЛКМ: панорама · Двойной клик: вписать");
    if(ImGui::BeginTabBar("plots")) {
        for(int i=0;i<8;++i)if(ImGui::BeginTabItem(names[i],nullptr,request_plot_tab==i?ImGuiTabItemFlags_SetSelected:0)) {
            ImGui::PushID(i);plot_windows.draw_view(plot_windows.embedded[i],*simulation,height-60,false);ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    request_plot_tab=-1;
}
void App::draw() {
    changed=false;advance();drawing=true;const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Network Studio",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDocking);
    toolbar();
    if(replacing){ImGui::TextUnformatted("Применяется конфигурация…");ImGui::End();drawing=false;return;}
    float width=ImGui::GetContentRegionAvail().x,height=ImGui::GetContentRegionAvail().y;
    float left=std::clamp(width*.23f,260.f,320.f),right=std::clamp(width*.25f,300.f,360.f);
    float bottom=std::clamp(height*.40f,260.f,440.f),top=height-bottom-8;
    ImGui::BeginChild("editor",{left,top},ImGuiChildFlags_Borders);editor();ImGui::EndChild();
    if(replacing){ImGui::End();drawing=false;return;}
    ImGui::SameLine();
    ImGui::BeginChild("canvas",{std::max(220.f,width-left-right-16),top},ImGuiChildFlags_Borders);canvas();ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("inspector",{right,top},ImGuiChildFlags_Borders);inspector();ImGui::EndChild();
    if(changed) {reset();status=simulation->error.empty()?"Параметры изменены. Расчёт сброшен.":simulation->error;}
    ImGui::BeginChild("plots",{-1,bottom-30},ImGuiChildFlags_Borders);if(!replacing)plots(bottom-30);ImGui::EndChild();
    ImGui::TextColored(simulation->error.empty()?Muted:ImVec4(1,.45f,.42f,1),"%s%s",dirty?"● ":"",status.c_str());
    network_wizard();
    ImGui::End();
    if(replacing){drawing=false;return;}
    plot_windows.draw(*simulation);
    const auto& analysis=plot_windows.embedded[7].analysis;
    // Send presentation settings only when edited; never copy into the live solver.
    if(analysis_settings_json(config.analysis)!=analysis_settings_json(analysis)) {
        config.analysis=analysis;RunnerCommand c;c.kind=RunnerCommand::Kind::Analysis;c.analysis=analysis;command(std::move(c));
    }
    stage_manager();stimulator_manager();drawing=false;
}
}
