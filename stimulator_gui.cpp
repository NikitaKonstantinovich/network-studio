#include "gui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace studio {
namespace {
bool scalar(const char* label,double& v,double lo,double hi) {ImGui::SetNextItemWidth(160);return ImGui::DragScalar(label,ImGuiDataType_Double,&v,.01f,&lo,&hi,"%.8g",ImGuiSliderFlags_AlwaysClamp);}
}
void App::stimulator_manager() {
    if(!show_stimulators)return;
    ImGui::SetNextWindowSize({900,850},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});
    if(!ImGui::Begin("Стимуляторы###shared_stimulators",&show_stimulators)){ImGui::End();return;}
    auto& sim=*simulation;
    ImGui::Text("t = %.9g с · команд ожидает %zu",sim.time,sim.stimulus_commands.size());
    ImGui::TextWrapped("DC + индивидуальные импульсы + общие источники + шум суммируются. HH: мкА/см²; Ижикевич: единицы тока модели. Одинаковые числа не означают физически одинаковый ток.");
    ImGui::BeginDisabled(!sim.error.empty());
    if(ImGui::Button("Добавить стимулятор")&&config.stimulators.size()<64)try {
        Stimulator value;value.name="Stimulator "+std::to_string(config.next_stimulator_id);value.x=config.space.width*.5;value.y=config.space.height*.5;value.radius=std::min(config.space.width,config.space.height)*.1;
        value.signal.start=sim.time;value.signal.stop=std::max(sim.time+1,config.duration);value.signal.frequency=10;value.signal.width=.003;value.signal.amplitude=30;
        value.recipients={std::clamp(selected_kind==0?selected_index:0,0,int(config.neurons.size())-1)};
        selected_stimulator=config.next_stimulator_id;RunnerCommand c;c.kind=RunnerCommand::Kind::Stimulator;c.stimulator=value;c.value=-1;c.flag2=true;command(std::move(c));stimulator_draft.id=0;dirty=true;
    }catch(const std::exception& e){status=e.what();}
    ImGui::BeginChild("stimulator_list",{0,90},ImGuiChildFlags_Borders);
    for(const auto& s:config.stimulators)if(ImGui::Selectable(("#"+std::to_string(s.id)+" "+s.name+"###source_"+std::to_string(s.id)).c_str(),selected_stimulator==s.id)){selected_stimulator=s.id;stimulator_draft=s;}
    ImGui::EndChild();
    auto selected=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==selected_stimulator;});
    if(selected!=config.stimulators.end()) {
        if(stimulator_draft.id!=selected->id)stimulator_draft=*selected;
        auto& v=stimulator_draft;
        char name[256];std::snprintf(name,sizeof(name),"%s",v.name.c_str());ImGui::SetNextItemWidth(300);if(ImGui::InputText("Название источника",name,sizeof(name)))v.name=name;
        ImGui::Checkbox("Источник включён",&v.enabled);ImGui::SameLine();ImGui::Checkbox("Пространственная область",&v.spatial);
        if(ImGui::BeginTabBar("stimulator_tabs")) {
            if(ImGui::BeginTabItem("Сигнал и получатели")) {
                ImGui::BeginChild("source_settings",{0,360},ImGuiChildFlags_Borders);
                scalar("Начало, с",v.signal.start,0,1e12);ImGui::SameLine();scalar("Окончание, с",v.signal.stop,v.signal.start,1e12);
                scalar("Первый импульс через, с",v.first_delay,0,std::max(0.,v.signal.stop-v.signal.start));
                scalar("Частота источника, Гц",v.signal.frequency,0,200);ImGui::SameLine();scalar("Амплитуда источника",v.signal.amplitude,-30,100);
                double width=v.signal.width*1000;if(scalar("Ширина импульса, мс",width,0,1000))v.signal.width=width*.001;
                if(v.spatial) {
                    ImGui::Text("Пространство %.6g × %.6g %s (без скрытого пересчёта)",config.space.width,config.space.height,config.space.unit.c_str());
                    scalar("Центр X",v.x,0,config.space.width);ImGui::SameLine();scalar("Центр Y",v.y,0,config.space.height);
                    scalar("Радиус области",v.radius,0,1e9);
                    const char* types[]={"Все нейроны","Возбуждающие","Тормозные"};int filter=int(v.filter);ImGui::SetNextItemWidth(220);if(ImGui::Combo("Фильтр области",&filter,types,3))v.filter=StimulatorFilter(filter);
                    ImGui::TextWrapped("Равная амплитуда внутри круга, граница включена. Центр можно перетаскивать на схеме без сброса сети и фазы.");
                } else {
                    if(ImGui::Button("Выбрать всех получателей")){v.recipients.clear();for(int i=0;i<int(config.neurons.size());++i)v.recipients.push_back(i);}
                    ImGui::SameLine();if(ImGui::Button("Снять получателей"))v.recipients.clear();
                    ImGui::BeginChild("source_recipients",{0,115},ImGuiChildFlags_Borders);ImGuiListClipper clip;clip.Begin(int(config.neurons.size()));
                    while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){ImGui::PushID(i);auto it=std::find(v.recipients.begin(),v.recipients.end(),i);bool on=it!=v.recipients.end();
                        if(ImGui::Checkbox(("N"+std::to_string(i+1)+" "+config.neurons[i].name).c_str(),&on)){if(on)v.recipients.push_back(i);else v.recipients.erase(it);}ImGui::PopID();}
                    ImGui::EndChild();
                }
                if(ImGui::TreeNode("Таблица импульсов внутри периода")) {
                    if(ImGui::Button("Добавить интервал")&&v.signal.pulse_intervals.size()<64) {double start=v.signal.pulse_intervals.empty()?0:v.signal.pulse_intervals.back().stop+.001;v.signal.pulse_intervals.push_back({start,start+.001});}
                    if(ImGui::Button("Очистить таблицу"))v.signal.pulse_intervals.clear();
                    for(int i=0;i<int(v.signal.pulse_intervals.size());++i){ImGui::PushID(i);auto& p=v.signal.pulse_intervals[i];double a=p.start*1000,b=p.stop*1000;
                        if(scalar("От, мс",a,0,1e6))p.start=a*.001;ImGui::SameLine();if(scalar("До, мс",b,0,1e6))p.stop=b*.001;ImGui::PopID();}ImGui::TreePop();
                }
                ImGui::EndChild();ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Журнал импульсов")) {
                ImGui::TextWrapped("Один ID импульса на весь источник. Получатели и амплитуда в таблице — при начале; изменения во время импульса сохраняются в версиях протоколов/журнале команд.");
                if(ImGui::BeginTable("shared_pulses",6,ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY,{0,300})) {
                    for(auto label:{"Источник:импульс","Начало, с","Конец, с","A","Получателей","Этап"})ImGui::TableSetupColumn(label);ImGui::TableHeadersRow();
                    ImGuiListClipper clip;clip.Begin(int(sim.history.shared_pulses.size()));while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){const auto& p=sim.history.shared_pulses[i];ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%llu:%llu",p.stimulator,p.pulse);ImGui::TableNextColumn();ImGui::Text("%.9g",p.start);ImGui::TableNextColumn();ImGui::Text("%.9g%s",p.complete?p.end:sim.time,p.complete?"":"…");ImGui::TableNextColumn();ImGui::Text("%.4g",p.amplitude);ImGui::TableNextColumn();ImGui::Text("%zu",p.recipients.size());ImGui::TableNextColumn();ImGui::Text("%d",p.stage+1);}ImGui::EndTable();
                }ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Команды и история")) {
                ImGui::BeginChild("source_commands",{0,340},ImGuiChildFlags_Borders);
                for(const auto& c:sim.stimulus_commands)ImGui::Text("Ожидает t=%.9g · #%llu · %s",c.time,c.value.id,c.restart?"перезапуск":"без перезапуска");
                for(const auto& c:sim.stimulus_changes)ImGui::TextWrapped("t=%.9g · команда %llu · источник #%llu · X=%.6g Y=%.6g R=%.6g · f=%.6g A=%.6g · %zu получателей · этап %d",c.time,c.sequence,c.value.id,c.value.x,c.value.y,c.value.radius,c.value.signal.frequency,c.value.signal.amplitude,c.recipients.size(),c.stage+1);
                ImGui::EndChild();ImGui::EndTabItem();
            }ImGui::EndTabBar();
        }
        ImGui::Text("Получателей по черновику: %zu · сейчас вход источника %.5g",stimulator_recipients(config,v).size(),pulse_current(stimulator_signal(*selected),sim.time));
        ImGui::TextWrapped("Применение не сбрасывает сеть. Положение/получатели меняются сейчас. Новая частота — со следующей границы текущего периода; перезапуск фазы — отдельное действие.");
        auto apply=[&](bool restart){try{RunnerCommand c;c.kind=RunnerCommand::Kind::Stimulator;c.stimulator=v;c.value=-1;c.flag=restart;command(std::move(c));dirty=true;}catch(const std::exception& e){status=e.what();}};
        if(ImGui::Button("Применить без сброса"))apply(false);
        ImGui::SameLine();if(ImGui::Button("Применить с новой фазой"))apply(true);
        ImGui::SameLine();if(ImGui::Button("Перечитать параметры")) {auto it=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==selected_stimulator;});if(it!=config.stimulators.end())v=*it;}
        if(ImGui::Button("Удалить стимулятор"))try{RunnerCommand c;c.kind=RunnerCommand::Kind::RemoveStimulator;c.id=selected_stimulator;command(std::move(c));if(plot_windows.embedded[7].analysis.reference_stimulator==selected_stimulator)plot_windows.embedded[7].analysis.reference_stimulator=0;selected_stimulator=0;dirty=true;}catch(const std::exception& e){status=e.what();}
    }
    ImGui::EndDisabled();ImGui::TextWrapped("%s",status.c_str());ImGui::End();
}
bool App::stimulator_overlay(ImVec2 base,ImVec2 span,bool hovered,bool active) {
    auto* draw=ImGui::GetWindowDrawList();const auto& io=ImGui::GetIO();uint64_t hit=0;
    for(const auto& s:config.stimulators)if(s.spatial) {
        const ImVec2 center{base.x+float(s.x/config.space.width)*span.x,base.y+float(s.y/config.space.height)*span.y};
        const auto color=s.id==selected_stimulator?IM_COL32(255,192,80,240):IM_COL32(245,165,80,140);
        for(int k=0;k<=80;++k){const double angle=k*6.283185307179586/80;draw->PathLineTo({center.x+float(s.radius/config.space.width*std::cos(angle))*span.x,center.y+float(s.radius/config.space.height*std::sin(angle))*span.y});}draw->PathStroke(color,ImDrawFlags_Closed,1.5f);
        draw->AddRectFilled({center.x-7,center.y-7},{center.x+7,center.y+7},color,2);draw->AddText({center.x+10,center.y-20},color,("Stim #"+std::to_string(s.id)).c_str());
        if(hovered&&std::hypot(io.MousePos.x-center.x,io.MousePos.y-center.y)<14)hit=s.id;
        if(s.id==selected_stimulator)for(int i:stimulator_recipients(config,s)){const auto& n=config.neurons[i];draw->AddCircle({base.x+float(n.px)*span.x,base.y+float(n.py)*span.y},config.neurons.size()>32?8.f:30.f,color,24,2);}
    }
    if(!ImGui::IsMouseDown(ImGuiMouseButton_Left))drag_stimulator=0;
    if(hit&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){selected_stimulator=drag_stimulator=hit;for(auto& s:config.stimulators)if(s.id==hit)stimulator_draft=s;}
    if(hit&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))show_stimulators=true;
    if(active&&drag_stimulator&&ImGui::IsMouseDragging(ImGuiMouseButton_Left,3))try {
        const double x=std::clamp(double(io.MousePos.x-base.x)/span.x,0.,1.)*config.space.width,y=std::clamp(double(io.MousePos.y-base.y)/span.y,0.,1.)*config.space.height;
        auto current=std::find_if(config.stimulators.begin(),config.stimulators.end(),[&](auto& s){return s.id==drag_stimulator;});
        if(current!=config.stimulators.end()&&current->x==x&&current->y==y)return true;
        RunnerCommand c;c.kind=RunnerCommand::Kind::MoveStimulator;c.id=drag_stimulator;c.value=x;c.second=y;c.third=-1;command(std::move(c));
        if(current!=config.stimulators.end()){current->x=x;current->y=y;}
        for(auto& s:config.stimulators)if(s.id==drag_stimulator)stimulator_draft=s;dirty=true;
    }catch(const std::exception& e){status=e.what();}
    return hit||drag_stimulator;
}
}
