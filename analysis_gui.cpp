#include "plot_windows.h"
#include "implot.h"
#include <algorithm>
#include <cstdio>
namespace studio {
namespace {
bool number(const char* label,double& v,double lo,double hi) {return ImGui::DragScalar(label,ImGuiDataType_Double,&v,.001f,&lo,&hi,"%.6g",ImGuiSliderFlags_AlwaysClamp);}
}
bool PlotWindows::create_analysis_profile(PlotWindowState& w,const std::string& name) {
    try {
        if(name.empty()||analysis_profiles.size()>=64)throw std::runtime_error("Укажите имя; допускается до 64 профилей.");
        for(const auto& p:analysis_profiles)if(p.name==name)throw std::runtime_error("Имя уже занято. Выберите новое имя профиля.");
        AnalysisProfile profile{name,w.analysis};analysis_profile_json(profile);analysis_profiles.push_back(profile);w.analysis_profile=name;
        message="Профиль создан; сохраните рабочее пространство.";return true;
    }catch(const std::exception& e){message=e.what();return false;}
}
bool PlotWindows::load_analysis_profile(PlotWindowState& w,size_t index) {
    if(index>=analysis_profiles.size())return false;const auto& p=analysis_profiles[index];w.analysis=p.settings;w.analysis_profile=p.name;w.analysis_at=-1;return true;
}
void PlotWindows::apply_analysis_to_all(const PlotWindowState& source) {
    // Copy first: source can be one of the destinations. No window insertion/solver command.
    const auto a=source.analysis;const auto interval=source.analysis_interval;const auto name=source.analysis_profile;
    auto apply=[&](PlotWindowState& w){w.analysis=a;w.analysis_interval=interval;w.analysis_profile=name;w.analysis_at=-1;};
    apply(embedded[7]);for(auto& w:windows)if(w.kind==PlotKind::Network)apply(w);
    message="Параметры профиля и интервал применены ко всем окнам сетевого анализа.";
}
void PlotWindows::draw_network(PlotWindowState& w,const SimulationReadView& s) {
    const char* views[]={"Популяционная активность","Частота пачек","Задержки ответов","Журнал стимулов","Сетевые пачки"};
    ImGui::SetNextItemWidth(235);ImGui::Combo("Представление сети",&w.view,views,5);ImGui::SameLine();
    if(ImGui::Button("Параметры анализа…"))ImGui::OpenPopup("network_analysis_settings");
    auto& a=w.analysis;
    ImGui::SameLine();if(ImGui::Button("Профили…"))ImGui::OpenPopup("analysis_profiles");
    if(ImGui::BeginPopup("analysis_profiles")) {
        ImGui::TextWrapped("Профиль хранит популяцию, детектор, опорный источник и окно ответа. Интервал задаётся отдельно; масштаб графика на статистику не влияет.");
        ImGui::InputText("Название профиля",w.profile_draft.data(),w.profile_draft.size());
        if(ImGui::Button("Создать из текущих настроек"))create_analysis_profile(w,w.profile_draft.data());
        if(ImGui::BeginCombo("Загрузить профиль",w.analysis_profile.empty()?"Выберите профиль":w.analysis_profile.c_str())) {
            for(size_t i=0;i<analysis_profiles.size();++i){ImGui::PushID(int(i));if(ImGui::Selectable(analysis_profiles[i].name.c_str()))load_analysis_profile(w,i);ImGui::PopID();}ImGui::EndCombo();
        }
        if(ImGui::Button("Применить профиль и интервал ко всем окнам"))apply_analysis_to_all(w);
        ImGui::Separator();ImGui::TextUnformatted("Проверить перед сравнением с окном:");
        auto compare=[&](PlotWindowState& other) {
            auto own=analyze_network(s.config,s.history,s.time,a,w.analysis_interval);
            auto result=analyze_network(s.config,s.history,s.time,other.analysis,other.analysis_interval);
            auto reason=compare_analysis(own,a,result,other.analysis);
            message=!other.compatible?"Несовместимая привязка другого окна.":reason.empty()?"Профили и интервалы совпадают; данные полные. Результаты сопоставимы.":reason;
        };
        if(&w!=&embedded[7]&&ImGui::Selectable("Встроенный сетевой анализ"))compare(embedded[7]);
        for(auto& other:windows)if(&other!=&w&&other.kind==PlotKind::Network){ImGui::PushID(int(other.id));if(ImGui::Selectable(other.title.c_str()))compare(other);ImGui::PopID();}
        if(!message.empty())ImGui::TextWrapped("%s",message.c_str());
        ImGui::TextWrapped("Профили сохраняются в рабочем пространстве; экспорт статистики содержит точную копию профиля и интервала.");
        if(ImGui::Button("Закрыть профили"))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
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
        std::string source=a.reference_stimulator?"Стимулятор #"+std::to_string(a.reference_stimulator):"Индивидуальный вход нейрона";
        if(ImGui::BeginCombo("Опорный источник",source.c_str())) {
            if(ImGui::Selectable("Индивидуальный вход нейрона",!a.reference_stimulator))a.reference_stimulator=0;
            for(const auto& stim:s.config.stimulators)if(ImGui::Selectable(("#"+std::to_string(stim.id)+" "+stim.name).c_str(),a.reference_stimulator==stim.id))a.reference_stimulator=stim.id;
            ImGui::EndCombo();
        }
        if(!a.reference_stimulator){int ref=a.reference_neuron+1;if(ImGui::SliderInt("Опорный поезд нейрона",&ref,1,int(s.config.neurons.size()),"N%d"))a.reference_neuron=ref-1;}

        number("Ответ не раньше, с",a.response_min,0,1e6);number("Ответ не позже, с",a.response_max,a.response_min,1e6);
        ImGui::TextWrapped("Один общий источник учитывается один раз независимо от числа получателей. Для прежних входов выбирается один опорный нейрон. Пачка сопоставляется только одному стимулу, по порядку. Незавершённые окна ответа не считаются пропусками. Равенство частот не доказывает синхронизацию.");
        if(ImGui::Button("Готово"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    ImGui::SetNextItemWidth(115);ImGui::InputDouble("От, с##analysis",&w.analysis_interval.from,0,0,"%.9g");ImGui::SameLine();
    ImGui::BeginDisabled(w.analysis_interval.current_end);ImGui::SetNextItemWidth(115);ImGui::InputDouble("До, с##analysis",&w.analysis_interval.to,0,0,"%.9g");ImGui::EndDisabled();ImGui::SameLine();
    ImGui::Checkbox("До текущего времени",&w.analysis_interval.current_end);
    std::string profile_label=w.analysis_profile.empty()?"без имени":w.analysis_profile;
    if(!w.analysis_profile.empty()) {
        auto found=std::find_if(analysis_profiles.begin(),analysis_profiles.end(),[&](const auto& p){return p.name==w.analysis_profile;});
        if(found==analysis_profiles.end()||!same_analysis_settings(a,found->settings))profile_label+=" (изменён)";
    }
    ImGui::Text("Профиль: %s · Интервал статистики не зависит от масштаба графика",profile_label.c_str());
    if(!valid_analysis_interval(w.analysis_interval)){ImGui::TextWrapped("Интервал: нужны конечные неотрицательные числа, До > От.");return;}
    const auto key=analysis_settings_json(a)+analysis_interval_json(w.analysis_interval);
    const bool invalid=w.analysis_identity!=s.identity||w.analysis_generation!=s.generation||key!=w.analysis_key||w.analysis_at<0||s.time<w.analysis_at;
    if(invalid||(s.time!=w.analysis_at&&(s.time-w.analysis_at>=.1||s.done()))) {
        w.analyzed=analyze_network(s.config,s.history,s.time,a,w.analysis_interval);w.analysis_at=s.time;w.analysis_key=key;
        w.analysis_identity=s.identity;w.analysis_generation=s.generation;
    }
    if(analysis_export_dialog) {
        if(ImGui::Button(analysis_export?"Экспорт анализа расчёта…":"Экспорт этого анализа…")) {
            auto path=analysis_export_dialog();if(!path.empty())try {
                path.replace_extension();
                if(analysis_export){analysis_export(a,w.analysis_interval,w.analysis_profile,path);message="Экспорт по журналам расчёта: их глубина может превышать экранную. Фактическое покрытие записано в _analysis.json.";}
                else {auto fresh=analyze_network(s.config,s.history,s.time,a,w.analysis_interval);export_analysis(fresh,a,path,w.analysis_profile);message=s.truncated?"Экспортирован анализ ограниченного окна отображения.":"Анализ экспортирован.";}
            }catch(const std::exception& e){message=e.what();}
        }
    }
    const auto& r=w.analyzed;
    ImGui::TextWrapped("Запрошено [%.9g; %.9g) с · Данные [%.9g; %.9g) с",r.requested_from,r.requested_until,r.available_from,r.available_until);
    if(!r.error.empty()){ImGui::TextWrapped("Анализ недоступен: %s",r.error.c_str());return;}
    if(!r.full_interval)ImGui::TextColored({1,.65f,.2f,1},"НЕПОЛНЫЙ интервал: показана только доступная часть.");
    if(!r.prehistory_complete)ImGui::TextWrapped("Недостаточная предыстория: состояние детектора известно только с %.9g с. Полное сравнение недоступно.",r.detector_certain_from);
    ImGui::Text("Фактически [%.9g; %.9g) с · N=%zu · пропуски %zu · лишние %zu",r.from,r.until,r.population_size,r.missed,r.extra);
    ImGui::Text("С ответом: %zu / %zu · стимулы %.4g Гц · пачки %.4g Гц · неполные окна %zu",r.responses.size()-r.missed,r.responses.size(),r.stimulus_hz,r.response_hz,r.incomplete_stimuli);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Окна [a,b); неполное окно нормируется своей длительностью. Детектор: (t-W,t]. Частоты сами по себе не означают синхронизацию.");
    ImGui::Text("Пачки: обнаружено %zu, подтверждено %zu",r.bursts.size(),size_t(std::count_if(r.bursts.begin(),r.bursts.end(),[](const auto& b){return b.complete;})));
    if(w.view==3||w.view==4) {
        if(ImGui::BeginTable("network_journal",w.view==3?4:6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,std::max(70.f,ImGui::GetContentRegionAvail().y-4)})) {
            if(w.view==3&&a.reference_stimulator) {
                for(auto label:{"Начало, с","Конец, с","ID импульса","Получателей"})ImGui::TableSetupColumn(label);ImGui::TableHeadersRow();
                for(const auto& e:s.history.shared_pulses)if(e.stimulator==a.reference_stimulator&&e.start>=r.from&&e.start<r.until){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%.9g",e.start);ImGui::TableNextColumn();ImGui::Text("%.9g%s",e.complete?e.end:s.time,e.complete?"":" (идёт)");ImGui::TableNextColumn();ImGui::Text("%llu",e.pulse);ImGui::TableNextColumn();ImGui::Text("%zu",e.recipients.size());}
            }else if(w.view==3) {
                for(auto label:{"t, с","Нейрон","Протокол","Амплитуда"})ImGui::TableSetupColumn(label);ImGui::TableHeadersRow();
                std::vector<const StimulusEvent*> rows;for(const auto& e:s.history.stimuli)if(e.neuron==a.reference_neuron&&e.time>=r.from&&e.time<r.until)rows.push_back(&e);
                ImGuiListClipper clip;clip.Begin(int(rows.size()));while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){const auto& e=*rows[i];
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
            else ImPlot::PlotLine("Начала пачек (включая неподтверждённые) / длина окна",r.time.data(),r.burst_hz.data(),int(r.time.size()));
            std::vector<double> stimuli,stages,bursts;
            if(a.reference_stimulator){for(const auto& e:s.history.shared_pulses)if(e.stimulator==a.reference_stimulator&&e.start>=r.from&&e.start<r.until)stimuli.push_back(e.start);}
            else for(auto e:s.history.stimuli)if(e.neuron==a.reference_neuron&&e.time>=r.from&&e.time<r.until)stimuli.push_back(e.time);
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
