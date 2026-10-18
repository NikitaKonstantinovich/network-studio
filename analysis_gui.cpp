#include "plot_windows.h"
#include "implot.h"
#include <algorithm>
#include <cstdio>
namespace studio {
namespace {
bool number(const char* label,double& v,double lo,double hi) {return ImGui::DragScalar(label,ImGuiDataType_Double,&v,.001f,&lo,&hi,"%.6g",ImGuiSliderFlags_AlwaysClamp);}
}
void PlotWindows::draw_network(PlotWindowState& w,const Simulator& s) {
    const char* views[]={"Популяционная активность","Частота пачек","Задержки ответов","Журнал стимулов","Сетевые пачки"};
    ImGui::SetNextItemWidth(235);ImGui::Combo("Представление сети",&w.view,views,5);ImGui::SameLine();
    if(ImGui::Button("Параметры анализа…"))ImGui::OpenPopup("network_analysis_settings");
    auto& a=w.analysis;
    if(ImGui::BeginPopup("network_analysis_settings")) {
        const char* populations[]={"Все нейроны","Возбуждающие","Тормозные","Выбранная группа"};int p=int(a.population);
        if(ImGui::Combo("Популяция",&p,populations,4))a.population=AnalysisPopulation(p);
        if(a.population==AnalysisPopulation::Selected) {
            ImGui::BeginChild("selected_population",{340,140},ImGuiChildFlags_Borders);ImGuiListClipper clip;clip.Begin(int(s.config.neurons.size()));
            while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){auto it=std::find(a.neurons.begin(),a.neurons.end(),i);bool on=it!=a.neurons.end();ImGui::PushID(i);
                if(ImGui::Checkbox(s.config.neurons[i].name.c_str(),&on)){if(on)a.neurons.push_back(i);else a.neurons.erase(it);}ImGui::PopID();}ImGui::EndChild();
        }
        number("Окно частоты, с",a.bin,1e-6,1e6);number("Окно детектора, с",a.burst_window,1e-6,1e6);
        number("Начало, спайков/с группы",a.start_rate,1e-9,1e12);number("Завершение, спайков/с группы",a.end_rate,0,a.start_rate);
        ImGui::SliderInt("Минимум участников",&a.min_neurons,1,int(s.config.neurons.size()));number("Минимальная доля",a.min_fraction,0,1);
        number("Промежуток между пачками, с",a.min_gap,0,1e6);number("Окно частоты пачек, с",a.burst_rate_window,1e-6,1e6);
        int ref=a.reference_neuron+1;if(ImGui::SliderInt("Опорный поезд нейрона",&ref,1,int(s.config.neurons.size()),"N%d"))a.reference_neuron=ref-1;
        number("Ответ не раньше, с",a.response_min,0,1e6);number("Ответ не позже, с",a.response_max,a.response_min,1e6);
        ImGui::TextWrapped("Один опорный получатель — один поезд стимулов. Пачка сопоставляется только одному стимулу, по порядку. Незавершённые окна ответа не считаются пропусками. Равенство частот не доказывает синхронизацию.");
        if(ImGui::Button("Готово"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    const auto key=analysis_settings_json(a);
    if(key!=w.analysis_key||w.analysis_at<0||s.time<w.analysis_at||s.time-w.analysis_at>=.1||s.done()) {
        if(key!=w.analysis_key||s.time!=w.analysis_at){w.analyzed=analyze_network(s.config,s.history,s.time,a);w.analysis_at=s.time;w.analysis_key=key;}
    }
    if(analysis_export_dialog) {
        ImGui::SameLine();if(ImGui::Button("Экспорт этого анализа…")) {
            auto path=analysis_export_dialog();if(!path.empty())try{path.replace_extension();auto fresh=analyze_network(s.config,s.history,s.time,a);export_analysis(fresh,a,path);message="Анализ экспортирован.";}catch(const std::exception& e){message=e.what();}
        }
    }
    const auto& r=w.analyzed;
    ImGui::Text("Анализ [%.6g; %.6g] с · N=%zu · пропуски %zu · лишние пачки %zu",r.from,r.until,r.population_size,r.missed,r.extra);
    if(!r.error.empty()){ImGui::TextWrapped("%s",r.error.c_str());return;}
    ImGui::Text("С ответом: %zu / %zu · стимулы %.4g Гц · пачки %.4g Гц · неполные окна %zu",r.responses.size()-r.missed,r.responses.size(),r.stimulus_hz,r.response_hz,r.incomplete_stimuli);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Окна [a,b); неполное окно нормируется своей длительностью. Детектор: (t-W,t]. Частоты сами по себе не означают синхронизацию.");
    if(w.view==3||w.view==4) {
        if(ImGui::BeginTable("network_journal",w.view==3?4:6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,std::max(70.f,ImGui::GetContentRegionAvail().y-4)})) {
            if(w.view==3) {
                for(auto label:{"t, с","Нейрон","Протокол","Амплитуда"})ImGui::TableSetupColumn(label);ImGui::TableHeadersRow();
                ImGuiListClipper clip;clip.Begin(int(s.history.stimuli.size()));while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){const auto& e=s.history.stimuli[i];
                    ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%.9g",e.time);ImGui::TableNextColumn();ImGui::Text("N%d",e.neuron+1);ImGui::TableNextColumn();ImGui::Text("%d",e.protocol);ImGui::TableNextColumn();ImGui::Text("%.6g",e.amplitude);}
            }else {
                for(auto label:{"Начало, с","Конец, с","Длительность, с","Спайков","Участников","Завершена"})ImGui::TableSetupColumn(label);ImGui::TableHeadersRow();
                ImGuiListClipper clip;clip.Begin(int(r.bursts.size()));while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){const auto& b=r.bursts[i];ImGui::TableNextRow();
                    for(double v:{b.start,b.end,b.end-b.start,double(b.spikes),double(b.participants),double(b.complete)}){ImGui::TableNextColumn();ImGui::Text("%.9g",v);}}
            }ImGui::EndTable();
        }return;
    }
    if(w.fit)ImPlot::SetNextAxesToFit();
    if(ImPlot::BeginPlot("##network_metrics",{-1,std::max(75.f,ImGui::GetContentRegionAvail().y-4)})) {
        ImPlot::SetupAxes(w.view==2?"Задержка, с":"Время, с",w.view==0?"Спайков/с группы":w.view==1?"Пачек/с":"Число ответов",0,w.auto_fit?ImPlotAxisFlags_AutoFit:0);
        if(w.view==0)ImPlot::SetupAxis(ImAxis_Y2,"Спайков/(нейрон·с)",ImPlotAxisFlags_Opposite|(w.auto_fit?ImPlotAxisFlags_AutoFit:0));
        if(w.follow&&w.view!=2)ImPlot::SetupAxisLimits(ImAxis_X1,std::max(r.from,r.until-w.seconds),std::max(r.until,r.from+w.seconds),ImPlotCond_Always);
        if(w.restore_limits&&w.limits_valid&&!w.fit){ImPlot::SetupAxisLimits(ImAxis_X1,w.limits[0],w.limits[1],ImPlotCond_Always);ImPlot::SetupAxisLimits(ImAxis_Y1,w.limits[2],w.limits[3],ImPlotCond_Always);}
        if(w.view==2){std::vector<double> latencies;for(auto x:r.responses)if(x.latency>=0)latencies.push_back(x.latency);if(!latencies.empty())ImPlot::PlotHistogram("Ответы",latencies.data(),int(latencies.size()),20);}
        else {
            if(w.view==0) {ImPlot::PlotLine("Группа",r.time.data(),r.group_hz.data(),int(r.time.size()));ImPlot::SetAxes(ImAxis_X1,ImAxis_Y2);ImPlot::PlotLine("На нейрон",r.time.data(),r.neuron_hz.data(),int(r.time.size()));ImPlot::SetAxes(ImAxis_X1,ImAxis_Y1);}
            else ImPlot::PlotLine("Число начал пачек / доступную длину окна",r.time.data(),r.burst_hz.data(),int(r.time.size()));
            std::vector<double> stimuli,stages,bursts;
            for(auto e:s.history.stimuli)if(e.neuron==a.reference_neuron&&e.time>=r.from)stimuli.push_back(e.time);
            for(const auto& p:s.experiment.protocols)if(p.time>0)stages.push_back(p.time);
            for(auto b:r.bursts)bursts.push_back(b.start);
            ImPlot::SetNextLineStyle({1,.7f,.2f,.45f});ImPlot::PlotInfLines("Стимулы",stimuli.data(),int(stimuli.size()));
            ImPlot::SetNextLineStyle({.4f,.9f,.5f,.6f});ImPlot::PlotInfLines("Начала пачек",bursts.data(),int(bursts.size()));
            ImPlot::SetNextLineStyle({.9f,.4f,.7f,.65f},2);ImPlot::PlotInfLines("Этапы",stages.data(),int(stages.size()));
        }
        auto lim=ImPlot::GetPlotLimits();w.limits[0]=lim.X.Min;w.limits[1]=lim.X.Max;w.limits[2]=lim.Y.Min;w.limits[3]=lim.Y.Max;
        w.limits_valid=true;w.fit=w.restore_limits=false;ImPlot::EndPlot();
    }
}
}
