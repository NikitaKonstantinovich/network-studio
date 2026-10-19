#include "gui.h"
#include <algorithm>
#include <cstdio>
#include <tuple>
namespace studio {
namespace {
bool number(const char* label,double& v,double lo,double hi) {ImGui::SetNextItemWidth(180);return ImGui::DragScalar(label,ImGuiDataType_Double,&v,.01f,&lo,&hi,"%.6g",ImGuiSliderFlags_AlwaysClamp);}
void group(std::vector<int>& indices,const Config& c) {
    if(ImGui::Button("Все нейроны")){indices.clear();for(int i=0;i<int(c.neurons.size());++i)indices.push_back(i);}
    ImGui::SameLine();if(ImGui::Button("Очистить группу"))indices.clear();
    ImGui::Text("Получателей: %zu",indices.size());
    ImGui::BeginChild("stage_neurons",{0,125},ImGuiChildFlags_Borders);
    ImGuiListClipper clip;clip.Begin(int(c.neurons.size()));while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i) {
        ImGui::PushID(i);auto it=std::find(indices.begin(),indices.end(),i);bool chosen=it!=indices.end();
        if(ImGui::Checkbox(("N"+std::to_string(i+1)+" "+c.neurons[i].name).c_str(),&chosen)){if(chosen)indices.push_back(i);else indices.erase(it);}ImGui::PopID();
    }ImGui::EndChild();
}
}
void App::stage_manager() {
    if(!show_stages)return;
    ImGui::SetNextWindowSize({960,800},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});
    if(!ImGui::Begin("Этапы эксперимента###experiment_stages",&show_stages)){ImGui::End();return;}
    auto& sim=*simulation;auto& runtime=sim.experiment;
    ImGui::Text("Время %.9g с · этап %d · до %.9g с · %s",sim.time,runtime.stage_index+1,runtime.stage_end,runtime.automatic?"последовательность":"ручное управление");
    ImGui::TextWrapped("Переход сохраняет динамику, веса, очередь и историю. Длительность округляется вверх к ближайшей границе шага. Старый импульс выбранной группы обрывается; новый поезд начинается через заданную задержку от перехода. DC и шум сохраняются.");
    if(running){if(ImGui::Button("Приостановить для перехода"))pause();}
    ImGui::BeginDisabled(running||!sim.error.empty());
    if(ImGui::Button("Продолжить текущее состояние без этапа"))try {RunnerCommand c;c.kind=RunnerCommand::Kind::ContinueCurrent;command(std::move(c));status="Ограничение этапа снято. При необходимости увеличьте конечное время и нажмите Продолжить; протоколы и веса сохранены.";}catch(const std::exception& e){status=e.what();}
    bool learning=runtime.learning;double rate=runtime.learning_rate;
    bool change=ImGui::Checkbox("Обучение включено",&learning);ImGui::SameLine();change|=number("Общая скорость обучения",rate,0,100);
    bool clear=ImGui::Button("Очистить следы STDP");if(change||clear){RunnerCommand c;c.kind=RunnerCommand::Kind::Learning;c.flag=learning;c.value=rate;c.flag2=clear;command(std::move(c));}
    ImGui::SameLine();ImGui::TextDisabled("При заморозке следы продолжают обновляться.");
    if(ImGui::BeginTabBar("stage_tabs")) {
        if(ImGui::BeginTabItem("План и переходы")) {
            const auto previous=config.stages;
            if(ImGui::Button("Добавить этап")&&config.stages.size()<256){ExperimentStage s;s.name="Этап "+std::to_string(config.stages.size()+1);config.stages.push_back(s);dirty=true;}
            int remove=-1;
            ImGui::BeginChild("stage_plan",{0,360},ImGuiChildFlags_Borders);
            for(int i=0;i<int(config.stages.size());++i) {
                auto& s=config.stages[i];ImGui::PushID(i);
                if(ImGui::CollapsingHeader((std::to_string(i+1)+". "+s.name).c_str())) {
                    char name[256];std::snprintf(name,sizeof(name),"%s",s.name.c_str());if(ImGui::InputText("Имя",name,sizeof(name)))s.name=name;
                    number("Длительность этапа, с",s.duration,config.dt,1e9);ImGui::Checkbox("Обучать",&s.learning);ImGui::SameLine();number("Коэффициент",s.learning_rate,0,100);
                    ImGui::Checkbox("Очистить следы при входе",&s.clear_traces);group(s.neurons,config);
                    ImGui::TextDisabled("Общие источники: выбранные начнут новый поезд при входе в этап.");
                    for(const auto& stim:config.stimulators){auto at=std::find(s.stimulator_ids.begin(),s.stimulator_ids.end(),stim.id);bool chosen=at!=s.stimulator_ids.end();if(ImGui::Checkbox(("#"+std::to_string(stim.id)+" "+stim.name).c_str(),&chosen)){if(chosen)s.stimulator_ids.push_back(stim.id);else s.stimulator_ids.erase(at);}}
                    ImGui::Checkbox("Импульсы включены",&s.stimulus_enabled);number("Амплитуда",s.amplitude,-30,100);number("Частота, Гц",s.frequency,0,200);
                    double ms=s.width*1000;if(number("Ширина, мс",ms,0,1000))s.width=ms*.001;
                    number("Первый импульс через, с",s.first_pulse,0,s.duration);
                    ImGui::TextDisabled("Пустая группа: протоколы не меняются. Группа использует одинаковый новый поезд.");
                    if(ImGui::Button("Войти в этап (пауза)"))try{const auto err=validate_experiment(config);if(!err.empty())throw std::runtime_error(err);RunnerCommand c;c.kind=RunnerCommand::Kind::EnterStage;c.stages=config.stages;c.index=i;command(std::move(c));status="Этап применён; продолжите расчёт.";}catch(const std::exception& e){status=e.what();}
                    ImGui::SameLine();if(ImGui::Button("Выполнить отсюда до конца"))try{const auto err=validate_experiment(config);if(!err.empty())throw std::runtime_error(err);RunnerCommand c;c.kind=RunnerCommand::Kind::EnterStage;c.stages=config.stages;c.index=i;c.flag=c.flag2=true;command(std::move(c));}catch(const std::exception& e){status=e.what();}
                    if(!runtime.stage_active&&ImGui::Button("Удалить этап"))remove=i;
                }ImGui::PopID();
            }ImGui::EndChild();
            if(remove>=0)config.stages.erase(config.stages.begin()+remove);
            auto same=[](const ExperimentStage& a,const ExperimentStage& b){
                return std::tie(a.name,a.duration,a.learning,a.clear_traces,a.learning_rate,a.neurons,a.stimulus_enabled,a.amplitude,a.frequency,a.width,a.first_pulse,a.stimulator_ids)==std::tie(b.name,b.duration,b.learning,b.clear_traces,b.learning_rate,b.neurons,b.stimulus_enabled,b.amplitude,b.frequency,b.width,b.first_pulse,b.stimulator_ids);
            };
            if((previous.size()!=config.stages.size()||!std::equal(previous.begin(),previous.end(),config.stages.begin(),same))&&validate_experiment(config).empty()) {
                RunnerCommand c;c.kind=RunnerCommand::Kind::Plan;c.stages=config.stages;command(std::move(c));dirty=true;
            }
            ImGui::TextWrapped("Войти в этап — применить действия без запуска. «Продолжить» запускает текущий этап. По завершении последнего этапа расчёт остановится. Обычный «Сброс» начинает сеть с исходных весов и состояний.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("История и сохранение")) {
            bool bounded=config.bounded_journals;
            if(ImGui::Checkbox("Ограниченные журналы (старые события удаляются)",&bounded)) {
                RunnerCommand c;c.kind=RunnerCommand::Kind::BoundedJournals;c.flag=bounded;command(std::move(c));
            }
            ImGui::TextWrapped("Потенциалы имеют отдельный лимит. Очередь будущих доставок не сокращается. Полные журналы ограничены 2 млн записей; для длинных опытов выберите ограниченный режим заранее. Удалённые события восстановить из checkpoint нельзя.");
            ImGui::Text("В окне отображения: спайки после %.6g с; доставки после %.6g с; стимулы после %.6g с",sim.history.events_after,sim.history.deliveries_after,sim.history.stimuli_after);
            ImGui::Text("На экране: %zu / %zu / %zu; лимит научных журналов %zu",sim.history.events.size(),sim.history.deliveries.size(),sim.history.stimuli.size(),config.journal_capacity);
            if(ImGui::Button("Сохранить полное состояние…")&&checkpoint_dialog)save_state(checkpoint_dialog(true));
            if(ImGui::Button("Независимое продолжение из состояния…")&&checkpoint_dialog) {
                auto path=checkpoint_dialog(false);if(!path.empty())try {
                    RunnerCommand c;c.kind=RunnerCommand::Kind::Restore;c.path=path;c.text=path.u8string();command(std::move(c));
                    ImGui::EndTabItem();ImGui::EndTabBar();ImGui::EndDisabled();ImGui::End();return;
                }catch(const std::exception& e){status=e.what();}
            }
            if(ImGui::Button("Экспорт весов как начальной конфигурации…")&&file_dialog) {
                auto path=file_dialog(true,false);if(!path.empty())try{RunnerCommand c;c.kind=RunnerCommand::Kind::ExportTrained;c.path=path;command(std::move(c));}catch(const std::exception& e){status=e.what();}
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Журнал переходов")) {
            ImGui::TextWrapped("Происхождение: %s",runtime.origin.empty()?"Новая конфигурация":runtime.origin.c_str());
            for(size_t k=0;k<runtime.protocols.size();++k) {
                const auto& p=runtime.protocols[k];ImGui::PushID(int(k));
                ImGui::Text("t=%.9g · %s · обучение %s × %.4g · очистка %s",p.time,p.name.c_str(),p.learning?"да":"нет",p.learning_rate,p.cleared?"да":"нет");
                if(ImGui::TreeNode("Применённые входные протоколы")) {
                    ImGui::BeginChild("protocol_values",{0,160},ImGuiChildFlags_Borders);
                    ImGuiListClipper clip;clip.Begin(int(p.neurons.size()));
                    while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i) {
                        const auto& n=p.neurons[i];
                        ImGui::Text("N%d · DC %.4g · A %.4g · %.6g Гц · ширина %.6g мс · [%.9g, %.9g) с · таблица %zu",i+1,n.current,n.amplitude,n.frequency,n.width*1000,n.start,n.stop,n.pulse_intervals.size());
                    }
                    for(const auto& stim:p.stimulators)ImGui::Text("#%llu · %s · %.6g Гц · получателей %zu",stim.id,stim.name.c_str(),stim.signal.frequency,stim.recipients.size());
                    ImGui::EndChild();ImGui::TreePop();
                }ImGui::PopID();
            }
            ImGui::TextDisabled("Полные версии стимулов включены в checkpoint и экспорт _protocols.json.");ImGui::EndTabItem();
        }ImGui::EndTabBar();
    }
    ImGui::EndDisabled();ImGui::TextWrapped("%s",status.c_str());ImGui::End();
}
}
