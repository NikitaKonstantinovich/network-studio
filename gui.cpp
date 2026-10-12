#include "gui.h"
#include "implot.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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
App::App() { reset(); }
void App::reset() { running=false; simulation=std::make_unique<Simulator>(config); if(!simulation->error.empty())status=simulation->error; }
void App::replace(Config c) {
    config=std::move(c); selected_kind=selected_index=0; preset_index=-1; filename.clear(); dirty=false;
    syn_plot=syn_view=0;
    for(int i=0;i<int(config.synapses.size());++i)if(config.synapses[i].stdp) {
        selected_kind=2;selected_index=syn_plot=i;syn_view=1;break;
    }
    link_source=0; link_target=std::min(1,int(config.neurons.size())-1); reset(); status="Конфигурация загружена.";
}
void App::save(const std::filesystem::path& path) {
    try { save_config(config,path); filename=path; dirty=false; status="Конфигурация сохранена."; }
    catch(const std::exception& e){status=e.what();}
}
void App::load(const std::filesystem::path& path) {
    try { auto c=load_config(path); replace(std::move(c)); filename=path; }
    catch(const std::exception& e){status=e.what();}
}
void App::save_state(const std::filesystem::path& path) {
    if(path.empty())return;
    if(running){status="Приостановите расчёт перед сохранением состояния.";return;}
    try {
        save_checkpoint(*simulation,path);
        status=simulation->error.empty()?"Полное состояние сохранено.":"Сохранён диагностический снимок ошибки; продолжение запрещено.";
    }catch(const std::exception& e){status=e.what();}
}
bool App::restore_state(const std::filesystem::path& path) {
    if(path.empty())return false;
    try {
        auto restored=load_checkpoint(path);Config restored_config=restored->config;
        std::string message="Состояние восстановлено: t = "+std::to_string(restored->time)+" с. ";
        message+=!restored->error.empty()?"Диагностический снимок: продолжение запрещено. "+restored->error:
            restored->done()?"Расчёт завершён; увеличьте конечное время или выполните сброс.":"Пауза. Нажмите «Продолжить».";
        config=std::move(restored_config);simulation=std::move(restored);running=false;dirty=false;
        selected_kind=selected_index=0;preset_index=-1;filename.clear();
        neuron_plot=neuron_view=syn_plot=syn_view=astro_plot=0;drag_kind=drag_index=-1;
        link_source=0;link_target=std::min(1,int(config.neurons.size())-1);generation_preview.reset();status=std::move(message);
        return true;
    }catch(const std::exception& e){status=e.what();return false;}
}
void App::advance() {
    if(!running)return;
    const auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<20000 && !simulation->done();++i) {
        if(!simulation->step())break;
        if((config.neurons.size()>32||i%32==0) && std::chrono::steady_clock::now()-begin>std::chrono::milliseconds(8))break;
    }
    if(simulation->done()) {
        running=false; status=simulation->error.empty()?"Расчёт завершён. Результаты можно экспортировать.":simulation->error;
    }
}
static bool scalar(const char* label,double& v,double lo,double hi,double speed=.01,const char* fmt="%.4g") {
    return ImGui::DragScalar(label,ImGuiDataType_Double,&v,float(speed),&lo,&hi,fmt,ImGuiSliderFlags_AlwaysClamp);
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
void App::network_wizard() {
    if(request_network_wizard) {
        generation_preview.reset();generation_error.clear();
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
        if(ImGui::BeginTabItem("Связи и расчёт")) {
            ImGui::PushItemWidth(240);
            const char* topologies[]={"Случайная сеть","Ближайшие соседи","Однонаправленное кольцо","Без связей"};
            int topology=int(generation.topology);
            if(ImGui::Combo("Топология",&topology,topologies,4)){generation.topology=NetworkTopology(topology);modified=true;}
            if(generation.topology==NetworkTopology::DirectedRing)ImGui::TextWrapped("При двух и более нейронах каждый соединён со следующим, последний — с первым. Один нейрон остаётся без связей. Распределение числа входов не используется.");
            if(generation.topology==NetworkTopology::NearestNeighbors)ImGui::TextWrapped("Источники выбираются по близости на сгенерированной плоскости. Число входов задаётся распределением.");
            modified|=ImGui::InputScalar("Seed генератора",ImGuiDataType_U32,&generation.seed);
            int synapse=generation.synapse.model==SynapseModel::TsodyksMarkram?0:1;
            const char* synapses[]={"Цодыкс–Маркрам (TM)","Экспоненциальный ток"};
            if(ImGui::Combo("Синапсы группы",&synapse,synapses,2)){generation.synapse.model=synapse==0?SynapseModel::TsodyksMarkram:SynapseModel::ExponentialCurrent;modified=true;}
            modified|=scalar("Масштаб g",generation.synapse.gain,0,10000,.1);
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
            modified|=scalar("Расчёт, с",generation.duration,.001,120,.1);
            double sample=generation.sample*1000;
            if(scalar("Запись новой сети, мс",sample,.025,1000,.1)){generation.sample=sample*.001;modified=true;}
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
        ImGui::TextDisabled("Ограничено степеней: %d · повторных выборок: %zu",g.degree_clamped,g.parameter_resamples);
        if(!generation_preview->record_all)ImGui::TextWrapped("Выборочная запись: %zu нейронов и %zu связей; события всех узлов сохраняются. Состав записи меняется в инспекторе.",generation_preview->record_neurons.size(),generation_preview->record_synapses.size());
    } else if(generation_error.empty())ImGui::TextDisabled("Нажмите «Предпросмотр», чтобы проверить сеть и увидеть фактические распределения.");
    ImGui::EndPopup();
}
void App::toolbar() {
    ImGui::TextColored(Teal,"NEURO / ASTRO"); ImGui::SameLine(); ImGui::TextDisabled("Конструктор малых сетей");
    ImGui::SameLine(ImGui::GetWindowWidth()-365); ImGui::TextDisabled("HH / Ижикевич · IP3 / Ca · RK4 · CPU");
    ImGui::Separator();
    ImGui::BeginDisabled(!simulation->error.empty() || (!running && simulation->done()));
    ImGui::PushStyleColor(ImGuiCol_Button,{.10f,.38f,.33f,1});
    if(ImGui::Button(running?"Пауза":simulation->time>0?"Продолжить":"Запуск",{110,30})) {
        running=!running; status=running?"Выполняется расчёт…":"Расчёт приостановлен.";
    } ImGui::PopStyleColor(); ImGui::EndDisabled();
    ImGui::SameLine(); if(ImGui::Button("Сброс",{80,30})) {reset();status="Начальные состояния восстановлены.";}
    ImGui::SameLine(); ImGui::BeginDisabled(running||!simulation->error.empty()||simulation->done());
    if(ImGui::Button("Шаг 50 мс",{110,30})) {
        double target=std::min(simulation->time+.05,config.duration);
        while(simulation->time<target&&!simulation->done())if(!simulation->step())break;
        status=simulation->error.empty()?"Выполнен шаг расчёта.":simulation->error;
    } ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(running);
    if(ImGui::Button("Открыть…",{95,30})&&file_dialog) {auto path=file_dialog(false,false);if(!path.empty())load(path);}
    ImGui::SameLine(); if(ImGui::Button("Сохранить…",{110,30})&&file_dialog) {auto path=file_dialog(true,false);if(!path.empty())save(path);}
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(running||simulation->history.time.size()<2);
    if(ImGui::Button("Экспорт CSV…",{125,30})&&file_dialog) {
        auto path=file_dialog(true,true);
        if(!path.empty()) try { path.replace_extension(); export_results(*simulation,path);status="Экспортированы CSV, события, параметры и сводка JSON."; }
        catch(const std::exception& e){status=e.what();}
    } ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextColored(running?Teal:Muted,"  %.3f / %.3f с",simulation->time,config.duration);
    ImGui::BeginDisabled(running);
    if(ImGui::Button("Сохранить состояние…")&&checkpoint_dialog) {auto path=checkpoint_dialog(true);if(!path.empty())save_state(path);}
    ImGui::SameLine();
    if(ImGui::Button("Восстановить состояние…")&&checkpoint_dialog) {auto path=checkpoint_dialog(false);if(!path.empty())restore_state(path);}
    ImGui::EndDisabled();
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
    if(scalar("Длительность, с",end_time,.001,120,.1)) {
        if(simulation->time==0){config.duration=end_time;edit();}
        else try {simulation->extend_duration(end_time);config.duration=end_time;dirty=true;status="Конечное время продлено; состояния и обучение сохранены.";}
        catch(const std::exception& e){status=e.what();}
    }
    if(simulation->time>0)ImGui::TextDisabled("Конечное время от t=0; можно увеличить.");
    double dt=config.dt*1e6;
    if(scalar("Шаг, мкс",dt,1,50,1,"%.1f")){config.dt=dt*1e-6;edit();}
    double sample=config.sample*1000;
    if(scalar("Запись, мс",sample,config.dt*1000,1000,.05)){config.sample=sample*.001;edit();}
    ImGui::PopItemWidth();
    if(ImGui::Checkbox("Записывать все состояния",&config.record_all))edit();
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
        if(scalar("Конец, с",n.stop,n.start,120,.1))edit();
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
            const double period=n.frequency>0?1/n.frequency:120.;
            if(ImGui::BeginTable("pulse_intervals",3,ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("От, мс");ImGui::TableSetupColumn("До, мс");ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,25);
                ImGui::TableHeadersRow();int remove=-1;
                for(int i=0;i<int(n.pulse_intervals.size());++i) {
                    auto& interval=n.pulse_intervals[i];ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);ImGui::SetNextItemWidth(-1);
                    double start_ms=interval.start*1000;
                    if(scalar(("##pulse_start_"+std::to_string(i)).c_str(),start_ms,0,120000,.1)){interval.start=start_ms*.001;edit();}
                    ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1);
                    double stop_ms=interval.stop*1000;
                    if(scalar(("##pulse_stop_"+std::to_string(i)).c_str(),stop_ms,0,120000,.1)){interval.stop=stop_ms*.001;edit();}
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
                syn_plot=selected_index;syn_view=1;
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
            ImGui::TextWrapped("Следы суммируют прежние спайки. События одного шага не дают взаимный нулевой лаг. Сохранение записывает начальный вес; «Сброс» отменяет обучение.");
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
            ImGui::TextWrapped("I = w · A · s. Каждый спайк увеличивает s на 1, между спайками ток затухает с tau syn. Отрицательный w задаёт торможение.");
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
    ImGui::TextColored(Muted,"ТОПОЛОГИЯ");ImGui::SameLine();
    if(compact) {
        ImGui::Checkbox("Все связи",&show_all_edges);
        ImGui::SameLine();ImGui::TextDisabled("E: зелёный · I: розовый");
        ImGui::TextDisabled("Клик: выбрать · Показаны связи выбранного узла");
    } else ImGui::TextDisabled("Перетащите узел · Shift + клик: связать · ПКМ: добавить");
    ImVec2 origin=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();size.x=std::max(size.x,100.f);size.y=std::max(size.y,100.f);
    ImGui::InvisibleButton("network_canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
    bool hovered=ImGui::IsItemHovered(),active=ImGui::IsItemActive();auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();
    d->PushClipRect(origin,add(origin,size),true); d->AddRectFilled(origin,add(origin,size),IM_COL32(14,22,31,255),8);
    for(float x=20;x<size.x;x+=24)for(float y=20;y<size.y;y+=24)d->AddCircleFilled(add(origin,{x,y}),1,IM_COL32(36,47,61,255));
    auto pos=[&](double x,double y) {return add(origin,{45+float(x)*(size.x-90),42+float(y)*(size.y-84)});};
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
    for(int i=0;i<int(config.gaps.size());++i) {
        const auto& g=config.gaps[i];bool sel=selected_kind==3&&selected_index==i;
        dashed(d,a[g.a],a[g.b],color(sel?Gold:Blue));linehit(a[g.a],a[g.b],3,i);
    }
    for(int i=0;i<int(config.synapses.size());++i) {
        const auto& s=config.synapses[i];bool sel=selected_kind==2&&selected_index==i;
        if(compact&&!show_all_edges&&!sel&&!(selected_kind==0&&(s.source==selected_index||s.target==selected_index)))continue;
        ImVec2 p=n[s.source],q=n[s.target];
        bool inhibitory=s.model==SynapseModel::ConductanceSTP?s.reversal<=-40:s.weight<0;
        ImU32 col=color(sel?Gold:(s.enabled?(inhibitory?ImVec4(.88f,.45f,.52f,1):Teal):Muted));
        ImVec2 center=mul(add(p,q),.5f);
        if(s.source==s.target) {float r=compact?9.f:22.f;d->AddCircle(add(p,{0,-r-7}),r,col,compact?12:28,2);center=add(p,{0,-2*r-7});}
        else { arrow(d,p,q,col,sel?3:(compact?1.f:1.8f),compact?5.f:25.f);linehit(p,q,2,i); }
        if(!compact||sel) {char buf[20];std::snprintf(buf,sizeof(buf),"S%d",i+1);d->AddText(add(center,{4,-19}),col,buf);}
        if(s.astro>=0)dashed(d,center,a[s.astro],IM_COL32(94,144,220,s.enabled?165:60));
    }
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
    if(hovered && !running && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit>=0) {
        if(io.KeyShift && selected_kind==0 && hit_kind==0 && config.synapses.size()<MaxSynapses) {
            config.synapses.push_back(new_synapse(config,selected_index,hit));selected_kind=2;selected_index=int(config.synapses.size())-1;edit();
        } else {selected_kind=hit_kind;selected_index=hit;drag_kind=hit_kind;drag_index=hit;}
    }
    if(active && !running && !io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left,3) && drag_kind>=0 && drag_kind<2) {
        double* x=nullptr;double* y=nullptr;
        if(drag_kind==0){x=&config.neurons[drag_index].px;y=&config.neurons[drag_index].py;}
        else {x=&config.astrocytes[drag_index].px;y=&config.astrocytes[drag_index].py;}
        // Layout changes preserve the numerical trajectory.
        *x=std::clamp((double(io.MousePos.x-origin.x)-45)/std::max(1.f,size.x-90),0.,1.);
        *y=std::clamp((double(io.MousePos.y-origin.y)-42)/std::max(1.f,size.y-84),0.,1.);dirty=true;
        if(drag_kind==0) {simulation->config.neurons[drag_index].px=*x;simulation->config.neurons[drag_index].py=*y;}
        else {simulation->config.astrocytes[drag_index].px=*x;simulation->config.astrocytes[drag_index].py=*y;}
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
    title("РЕЗУЛЬТАТЫ");
    ImGui::Checkbox("Следить за временем",&follow);ImGui::SameLine();ImGui::SetNextItemWidth(110);
    scalar("Окно, с",window_seconds,.05,120,.05);ImGui::SameLine();
    ImGui::TextDisabled("Колесо: масштаб · ЛКМ: панорама · Двойной клик: вписать");
    const auto& s=*simulation; const auto& c=s.config;const auto& h=s.history;
    bool valid=s.error.empty()||!h.time.empty();int count=int(h.time.size());
    if(!valid||count==0) {ImGui::TextWrapped("Исправьте параметры модели для построения графиков.");return;}
    auto axes=[&](const char* y,bool fast=false) {
        ImPlot::SetupAxes("Время, с",y,0,follow?(ImPlotAxisFlags_AutoFit|(fast?ImPlotAxisFlags_RangeFit:0)):0);
        if(follow)ImPlot::SetupAxisLimits(ImAxis_X1,fast?std::max(0.,s.time-window_seconds):0,std::max(fast?window_seconds:.1,s.time),ImPlotCond_Always);
    };
    auto line=[&](const std::string& label,int column,const ImVec4& col) {
        if(column<0||column>=int(h.values.size())||h.values[column].size()!=h.time.size())return;
        ImPlot::SetNextLineStyle(col);ImPlot::PlotLine(label.c_str(),h.time.data(),h.values[column].data(),count);
    };
    float plot_height=std::max(130.f,height-92);
    // Notices and selectors consume different amounts of vertical space.
    // Size the actual plot after those controls, so its axes stay in the panel.
    auto plot_size=[&](float requested) {
        return ImVec2(-1,std::max(65.f,std::min(requested,ImGui::GetContentRegionAvail().y-4)));
    };
    if(ImGui::BeginTabBar("plots")) {
        if(ImGui::BeginTabItem("Потенциалы",nullptr,request_plot_tab==0?ImGuiTabItemFlags_SetSelected:0)) {
            std::vector<int> shown;
            for(int i=0;i<int(c.neurons.size());++i)if(records_neuron(c,i)&&shown.size()<32)shown.push_back(i);
            if(selected_kind==0&&selected_index>=0&&selected_index<int(c.neurons.size())&&records_neuron(c,selected_index)&&
               std::find(shown.begin(),shown.end(),selected_index)==shown.end())shown.push_back(selected_index);
            if(shown.empty())ImGui::TextDisabled("Потенциалы не записывались. Включите запись нейронов в инспекторе и повторите расчёт.");
            else if(!c.record_all)ImGui::TextDisabled("Показаны записанные нейроны. Состав записи меняется в инспекторе перед новым расчётом.");
            else if(c.neurons.size()>32)ImGui::TextDisabled("Первые 32 нейрона и выбранный узел. Для сравнения отдельных узлов настройте запись в инспекторе.");
            std::vector<std::vector<double>> peaks(c.neurons.size());
            std::vector<bool> plotted(c.neurons.size(),false);for(int i:shown)plotted[i]=true;
            for(const auto& e:h.events)if(!e.astro&&plotted[e.index]&&(!follow||e.time>=s.time-window_seconds))peaks[e.index].push_back(e.time);
            if(ImPlot::BeginPlot("##voltages",plot_size(plot_height))) {
                axes("V, мВ",true);
                for(int i:shown) {
                    const auto col=i%3==0?Teal:(i%3==1?Blue:Gold);
                    const auto label="N"+std::to_string(i+1)+" "+c.neurons[i].name;
                    line(label,i*4,col);
                    if(c.neurons[i].model==NeuronModel::Izhikevich) {
                        // Izhikevich state is recorded after reset; event markers retain the peak.
                        const auto& times=peaks[i];std::vector<double> voltage(times.size(),c.neurons[i].izh_peak);
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,2.5f,col,1,col);
                        ImPlot::PlotScatter(label.c_str(),times.data(),voltage.data(),int(times.size()));
                    }
                } ImPlot::EndPlot();
            } ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Астроциты",nullptr,request_plot_tab==1?ImGuiTabItemFlags_SetSelected:0)) {
            if(c.astrocytes.empty())ImGui::TextDisabled("Добавьте астроцит к сети.");
            else if(ImPlot::BeginPlot("##astro",plot_size(plot_height))) {
                axes("Ca / IP3, мкМ");
                for(int i=0;i<int(c.astrocytes.size());++i) {
                    int b=astro_offset(c,i);line("A"+std::to_string(i+1)+" Ca",b+2,i%2?Gold:Blue);
                    line("A"+std::to_string(i+1)+" IP3",b+1,i%2?Teal:ImVec4(.72f,.56f,.94f,1));
                }ImPlot::EndPlot();
            } ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Растр событий",nullptr,request_plot_tab==2?ImGuiTabItemFlags_SetSelected:0)) {
            if(ImPlot::BeginPlot("##raster",plot_size(plot_height))) {
                const double start=follow&&c.neurons.size()>32?std::max(0.,s.time-window_seconds):0.;
                ImPlot::SetupAxes("Время, с","Номер узла");
                if(follow)ImPlot::SetupAxisLimits(ImAxis_X1,start,std::max(start+.1,s.time),ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1,.5,c.neurons.size()+c.astrocytes.size()+.5,ImPlotCond_Always);
                std::vector<double> tx,ny,ta,ay;
                const auto first=std::lower_bound(h.events.begin(),h.events.end(),start,[](const auto& e,double t){return e.time<t;});
                for(auto it=first;it!=h.events.end();++it)if(it->astro){ta.push_back(it->time);ay.push_back(double(c.neurons.size()+it->index+1));}
                    else {tx.push_back(it->time);ny.push_back(it->index+1.);}
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Cross,3,Teal,1,Teal);
                ImPlot::PlotScatter("Нейроны",tx.data(),ny.data(),int(tx.size()));
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond,5,Blue,1,Blue);
                ImPlot::PlotScatter("Астроциты (после нейронов)",ta.data(),ay.data(),int(ta.size()));ImPlot::EndPlot();
            } ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Синапсы",nullptr,request_plot_tab==3?ImGuiTabItemFlags_SetSelected:0)) {
            if(c.synapses.empty())ImGui::TextDisabled("Добавьте синапс к сети.");
            else {
                syn_plot=std::clamp(syn_plot,0,int(c.synapses.size())-1);ImGui::SetNextItemWidth(120);
                int selected=syn_plot+1;ImGui::SliderInt("Синапс",&selected,1,int(c.synapses.size()),"S%d");syn_plot=selected-1;
                ImGui::SameLine();ImGui::SetNextItemWidth(185);
                const char* views[]={"Ресурсы и ток","Вес w(t)"};
                ImGui::Combo("График связи",&syn_view,views,2);
                const bool recorded=records_synapse(c,syn_plot)&&h.weights[syn_plot].size()==h.time.size();
                const auto& selected_synapse=c.synapses[syn_plot];
                const bool current_available=selected_synapse.model!=SynapseModel::ConductanceSTP||h.values[selected_synapse.target*4].size()==h.time.size();
                if(!recorded)ImGui::TextWrapped("Состояние этой связи не записывалось. Включите «Записывать синапс» в инспекторе и запустите расчёт заново.");
                else if(syn_view==0&&!current_available)ImGui::TextWrapped("Для тока проводимости включите запись нейрона-приёмника и повторите расчёт. Ресурсы синапса доступны.");
                if(recorded&&syn_view==1&&ImPlot::BeginPlot("##synapse_weight",plot_size(plot_height-35))) {
                    axes("Вес w");
                    ImPlot::SetNextLineStyle(Teal);
                    ImPlot::PlotLine("Текущий вес",h.time.data(),h.weights[syn_plot].data(),count);
                    const auto& syn=c.synapses[syn_plot];
                    const double span[]={h.time.front(),std::max(.1,h.time.back())};
                    const double initial[]={syn.weight,syn.weight};
                    ImPlot::SetNextLineStyle(Muted);
                    ImPlot::PlotLine("Начальный вес",span,initial,2);
                    if(syn.stdp) {
                        const double minimum[]={syn.weight_min,syn.weight_min},maximum[]={syn.weight_max,syn.weight_max};
                        ImPlot::SetNextLineStyle(Gold);ImPlot::PlotLine("Минимум",span,minimum,2);
                        ImPlot::SetNextLineStyle(Gold);ImPlot::PlotLine("Максимум",span,maximum,2);
                    }
                    ImPlot::EndPlot();
                } else if(recorded&&syn_view==0&&ImPlot::BeginPlot("##synapse",plot_size(plot_height-35))) {
                    const auto& syn=c.synapses[syn_plot];int b=syn_offset(c,syn_plot);
                    axes(syn.model==SynapseModel::TsodyksMarkram?"u, x, y, z":"u, x, s");
                    ImPlot::SetupAxis(ImAxis_Y2,"I syn",ImPlotAxisFlags_Opposite|(follow?ImPlotAxisFlags_AutoFit:0));
                    if(syn.model!=SynapseModel::ExponentialCurrent) {
                        line("u: использование",b,Gold);line("x: ресурс",b+1,Teal);
                    }
                    line(syn.model==SynapseModel::TsodyksMarkram?"y: активный ресурс":"s: активация",b+2,Blue);
                    if(syn.model==SynapseModel::TsodyksMarkram) {
                        std::vector<double> inactive;inactive.reserve(count);
                        for(int k=0;k<count;++k)inactive.push_back(1-h.values[b+1][k]-h.values[b+2][k]);
                        ImPlot::SetNextLineStyle(ImVec4(.72f,.56f,.94f,1));
                        ImPlot::PlotLine("z: неактивный ресурс",h.time.data(),inactive.data(),count);
                    }
                    std::vector<double> current;current.reserve(count);
                    if(current_available)for(int k=0;k<count;++k) {
                        double scale=syn.model==SynapseModel::ConductanceSTP?
                            syn.conductance*(syn.reversal-h.values[syn.target*4][k]):syn.gain;
                        current.push_back(syn.enabled?h.weights[syn_plot][k]*scale*h.values[b+2][k]:0);
                    }
                    if(current_available) {
                        ImPlot::SetAxes(ImAxis_X1,ImAxis_Y2);ImPlot::SetNextLineStyle(ImVec4(.98f,.5f,.58f,1));
                        ImPlot::PlotLine("Синаптический ток",h.time.data(),current.data(),count);
                    }
                    ImPlot::EndPlot();
                }
            } ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Фазовый портрет",nullptr,request_plot_tab==4?ImGuiTabItemFlags_SetSelected:0)) {
            if(c.astrocytes.empty())ImGui::TextDisabled("Добавьте астроцит к сети.");
            else {
                astro_plot=std::clamp(astro_plot,0,int(c.astrocytes.size())-1);ImGui::SetNextItemWidth(120);
                int selected=astro_plot+1;ImGui::SliderInt("Астроцит",&selected,1,int(c.astrocytes.size()),"A%d");astro_plot=selected-1;
                if(ImPlot::BeginPlot("##phase",plot_size(plot_height-35))) {
                    ImPlot::SetupAxes("IP3, мкМ","Ca, мкМ",follow?ImPlotAxisFlags_AutoFit:0,follow?ImPlotAxisFlags_AutoFit:0);
                    int b=astro_offset(c,astro_plot);ImPlot::SetNextLineStyle(Blue);
                    ImPlot::PlotLine("Траектория",h.values[b+1].data(),h.values[b+2].data(),count);ImPlot::EndPlot();
                }
            } ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Нейрон / вход",nullptr,request_plot_tab==5?ImGuiTabItemFlags_SetSelected:0)) {
            neuron_plot=std::clamp(neuron_plot,0,int(c.neurons.size())-1);
            int selected=neuron_plot+1;ImGui::SetNextItemWidth(120);
            ImGui::SliderInt("Нейрон графика",&selected,1,int(c.neurons.size()),"N%d");neuron_plot=selected-1;
            ImGui::SameLine();ImGui::SetNextItemWidth(185);
            const char* views[]={"Внешний ток I(t)","Восстановление u(t)","Фаза V / u"};
            const bool izh=c.neurons[neuron_plot].model==NeuronModel::Izhikevich;
            if(!izh)neuron_view=0;
            ImGui::Combo("Представление",&neuron_view,views,izh?3:1);
            const float available=std::max(100.f,plot_height-35);
            const bool recorded=h.values[neuron_plot*4].size()==h.time.size()&&h.values[neuron_plot*4+1].size()==h.time.size();
            if(neuron_view!=0&&!recorded)ImGui::TextWrapped("Состояние этого нейрона не записывалось. Включите «Записывать нейрон» в инспекторе и запустите расчёт заново.");
            if(neuron_view==0&&ImPlot::BeginPlot("##neuron_input",plot_size(available))) {
                axes(izh?"I, ед. модели":"I, мкА/см²",true);
                std::vector<double> input;input.reserve(count);
                for(double t:h.time)input.push_back(external_current(c,neuron_plot,t));
                ImPlot::SetNextLineStyle(Gold);ImPlot::PlotLine("DC + импульс + шум",h.time.data(),input.data(),count);
                ImPlot::EndPlot();
            } else if(recorded&&neuron_view==1&&ImPlot::BeginPlot("##neuron_recovery",plot_size(available))) {
                axes("u",true);line("Переменная восстановления",neuron_plot*4+1,Teal);ImPlot::EndPlot();
            } else if(recorded&&neuron_view==2&&ImPlot::BeginPlot("##neuron_phase",plot_size(available))) {
                ImPlot::SetupAxes("V, мВ","u",follow?ImPlotAxisFlags_AutoFit:0,follow?ImPlotAxisFlags_AutoFit:0);
                ImPlot::SetNextLineStyle(Teal);
                ImPlot::PlotLine("Траектория V / u",h.values[neuron_plot*4].data(),h.values[neuron_plot*4+1].data(),count);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        } ImGui::EndTabBar();
    }
    request_plot_tab=-1;
}
void App::draw() {
    changed=false;advance();const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Network Studio",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    toolbar();float width=ImGui::GetContentRegionAvail().x,height=ImGui::GetContentRegionAvail().y;
    float left=std::clamp(width*.23f,260.f,320.f),right=std::clamp(width*.25f,300.f,360.f);
    float bottom=std::clamp(height*.40f,260.f,340.f),top=height-bottom-8;
    ImGui::BeginChild("editor",{left,top},ImGuiChildFlags_Borders);editor();ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("canvas",{std::max(220.f,width-left-right-16),top},ImGuiChildFlags_Borders);canvas();ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("inspector",{right,top},ImGuiChildFlags_Borders);inspector();ImGui::EndChild();
    if(changed) {reset();status=simulation->error.empty()?"Параметры изменены. Расчёт сброшен.":simulation->error;}
    ImGui::BeginChild("plots",{-1,bottom-30},ImGuiChildFlags_Borders);plots(bottom-30);ImGui::EndChild();
    ImGui::TextColored(simulation->error.empty()?Muted:ImVec4(1,.45f,.42f,1),"%s%s",dirty?"● ":"",status.c_str());
    network_wizard();
    ImGui::End();
}
}
