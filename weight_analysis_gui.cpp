#include "gui.h"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace studio {
void App::add_weight_snapshot(WeightSnapshot s){if(weight_snapshots.size()>=16)throw std::runtime_error("Не более 16 сохранённых снимков; сохраните файл и удалите лишний снимок из списка.");validate_weights(s);weight_snapshots.push_back(std::make_shared<const WeightSnapshot>(std::move(s)));weight_saved_selection=int(weight_snapshots.size())-1;weight_target=weight_saved_selection+(weights_include_live?1:0);weights_dirty=true;weight_message="Снимок добавлен и выбран для сравнения. История выбранных связей не использовалась.";}
void App::capture_weight_snapshot(){RunnerCommand c;c.kind=RunnerCommand::Kind::CaptureWeights;c.text=weight_name.data();command(std::move(c));}
void App::import_weight_snapshot(const std::filesystem::path& path){if(path.empty())return;WeightSnapshot s;
    if(path.extension()==L".nscp"){auto sim=load_checkpoint(path);if(!sim->error.empty())throw std::runtime_error("Checkpoint содержит ошибку расчёта.");s=capture_weights(sim->config,sim->plasticity,sim->time,sim->steps,path.stem().u8string());s.origin="Validated checkpoint: "+path.u8string();}
    else if(path.filename().u8string().find("_summary.json")!=std::string::npos)s=load_weight_summary(path);
    else s=load_weights(path);add_weight_snapshot(std::move(s));
}
namespace {
void statistics(const char* label,const WeightStatistics& s){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(label);ImGui::TableNextColumn();ImGui::Text("%zu",s.count);const double values[]={s.mean,s.median,s.stddev,s.minimum,s.maximum,s.sum};for(int i=0;i<6;++i){ImGui::TableNextColumn();if(s.count||i==5)ImGui::Text("%.9g",values[i]);else ImGui::TextUnformatted("—");}ImGui::TableNextColumn();if(s.learning_count)ImGui::Text("%.3f%% / %.3f%% (N=%zu)",100.*s.at_lower/s.learning_count,100.*s.at_upper/s.learning_count,s.learning_count);else ImGui::TextUnformatted("н/д: нет STDP");}
void histogram_plot(const char* title,const std::vector<double>& edges,const std::vector<std::vector<double>>& counts,const std::vector<std::string>& labels,const char* axis){if(edges.size()<2)return;std::vector<double> centers;for(size_t i=0;i+1<edges.size();++i)centers.push_back(edges[i]+(edges[i+1]-edges[i])/2);if(ImPlot::BeginPlot(title,{-1,225})){ImPlot::SetupAxes(axis,"Число связей");ImPlot::SetupAxisLimits(ImAxis_X1,edges.front(),edges.back(),ImGuiCond_Always);for(size_t i=0;i<counts.size();++i){ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL,.25f);ImPlot::PlotBars(labels[i].c_str(),centers.data(),counts[i].data(),int(counts[i].size()),(edges[1]-edges[0])*.95);}ImPlot::EndPlot();}}
}
void App::weights_manager(){if(!show_weights)return;ImGui::SetNextWindowSize({1100,900},ImGuiCond_FirstUseEver);if(!ImGui::Begin("Веса всей сети###weight_analysis",&show_weights)){ImGui::End();return;}
    ImGui::TextWrapped("Все текущие веса из согласованного снимка вычислителя, включая незаписываемые связи. Это отдельные снимки, не временная история.");
    bool dirty=ImGui::Checkbox("Показывать текущий снимок",&weights_include_live);
    if(dirty){const int shift=weights_include_live?1:-1;weight_reference=std::max(0,weight_reference+shift);weight_target=std::max(0,weight_target+shift);}
    ImGui::SameLine();ImGui::Text("t = %.9g с; шаг %llu",simulation->time,(unsigned long long)simulation->steps);
    ImGui::SetNextItemWidth(220);ImGui::InputText("Название нового снимка",weight_name.data(),weight_name.size());
    ImGui::BeginDisabled(weight_snapshots.size()>=16);
    if(ImGui::Button("Зафиксировать текущие веса"))capture_weight_snapshot();ImGui::SameLine();
    if(ImGui::Button("Добавить исходные веса"))try{add_weight_snapshot(initial_weights(simulation->config));}catch(const std::exception& e){weight_message=e.what();}
    ImGui::SameLine();if(ImGui::Button("Загрузить снимок весов…")&&weight_dialog)try{auto p=weight_dialog(false);if(!p.empty())import_weight_snapshot(p);}catch(const std::exception& e){weight_message=e.what();}ImGui::EndDisabled();
    ImGui::TextWrapped("Импорт: .nsweights, совместимый .nscp или PREFIX_summary.json с парным PREFIX_experiment.json. Старые checkpoint не обходятся; парность старой сводки не подтверждена хэшем.");
    if(!weight_snapshots.empty()){
        weight_saved_selection=std::clamp(weight_saved_selection,0,int(weight_snapshots.size())-1);ImGui::SetNextItemWidth(350);
        if(ImGui::BeginCombo("Сохранённые снимки",weight_snapshots[weight_saved_selection]->name.c_str())){for(int i=0;i<int(weight_snapshots.size());++i){ImGui::PushID(i);if(ImGui::Selectable(weight_snapshots[i]->name.c_str(),i==weight_saved_selection))weight_saved_selection=i;ImGui::PopID();}ImGui::EndCombo();}
        ImGui::SameLine();if(ImGui::Button("Сохранить снимок весов…")&&weight_dialog)try{auto p=weight_dialog(true);if(!p.empty()){save_weights(*weight_snapshots[weight_saved_selection],p);weight_message="Снимок весов сохранён.";}}catch(const std::exception& e){weight_message=e.what();}
        ImGui::SameLine();if(ImGui::Button("Убрать снимок")){
            const int removed=weight_saved_selection+(weights_include_live?1:0);
            for(int* selection:{&weight_reference,&weight_target}){if(*selection==removed)*selection=0;else if(*selection>removed)--*selection;}
            weight_snapshots.erase(weight_snapshots.begin()+weight_saved_selection);weights_dirty=true;weight_message="Снимок удалён из списка. Проверьте выбор базы и цели сравнения.";
        }
    }
    ImGui::Separator();ImGui::SetNextItemWidth(110);dirty|=ImGui::Combo("Тип связи",&weight_settings.polarity,"Все\0E\0I\0");ImGui::SameLine();ImGui::SetNextItemWidth(185);dirty|=ImGui::Combo("Модель связи",&weight_settings.model,"Все модели\0Conductance STP\0Exponential current\0Tsodyks–Markram\0");ImGui::SameLine();ImGui::SetNextItemWidth(130);dirty|=ImGui::Combo("Включение",&weight_settings.enabled,"Все\0Включённые\0Выключенные\0");
    int representation=weight_settings.absolute?1:0;ImGui::SetNextItemWidth(220);if(ImGui::Combo("Представление веса",&representation,"Хранимый w (со знаком)\0Модуль |w|\0")){weight_settings.absolute=representation!=0;dirty=true;}
    ImGui::SameLine();ImGui::SetNextItemWidth(85);dirty|=ImGui::InputInt("Столбцов",&weight_settings.bins);weight_settings.bins=std::clamp(weight_settings.bins,1,512);
    ImGui::SetNextItemWidth(130);dirty|=ImGui::InputDouble("Допуск у границ (абсолютный)",&weight_settings.boundary_tolerance,0,0,"%.9g");
    ImGui::TextWrapped("E/I: у проводимостной модели — по реверсии; у токовых — по исходному знаку/правилу STDP. Доли у границ считаются только среди выбранных связей с STDP, по исходному w. σ — генеральное отклонение (делитель N).");
    if(weights_include_live&&(weight_sequence!=simulation->sequence||weight_generation!=simulation->generation||!weights_live))try{weights_live=std::make_shared<const WeightSnapshot>(capture_weights(simulation->config,simulation->plasticity,simulation->time,simulation->steps,"Текущий"));weight_sequence=simulation->sequence;weight_generation=simulation->generation;dirty=true;}catch(const std::exception& e){weights_live.reset();weight_message=e.what();}
    std::vector<const WeightSnapshot*> frames;if(weights_include_live&&weights_live)frames.push_back(weights_live.get());for(auto& s:weight_snapshots)frames.push_back(s.get());
    if(frames.empty()){ImGui::TextUnformatted("Нет снимков. Включите текущий или загрузите сохранённый.");ImGui::TextWrapped("%s",weight_message.c_str());ImGui::End();return;}
    auto choose=[&](const char* label,int& index){index=std::clamp(index,0,int(frames.size())-1);ImGui::SetNextItemWidth(260);std::string display=frames[index]->name+" @ "+std::to_string(frames[index]->time)+" с";if(ImGui::BeginCombo(label,display.c_str())){for(int i=0;i<int(frames.size());++i){ImGui::PushID(i);std::string item=frames[i]->name+" @ "+std::to_string(frames[i]->time)+" с";if(ImGui::Selectable(item.c_str(),index==i)){index=i;dirty=true;}ImGui::PopID();}ImGui::EndCombo();}};
    choose("Базовый снимок",weight_reference);ImGui::SameLine();choose("Сравниваемый снимок",weight_target);
    if(dirty||weights_dirty)try{weight_result=analyze_weights(frames,weight_settings,weight_reference,weight_target);weights_dirty=false;}catch(const std::exception& e){weight_result={};weight_message=e.what();weights_dirty=true;}
    if(ImGui::Button("Экспорт распределений и чисел…")&&file_dialog)try{auto p=file_dialog(true,true);if(!p.empty()){p.replace_extension();export_weights(frames,weight_settings,weight_reference,weight_target,p);weight_message="Сохранены исходные веса, статистика, общие столбцы, Δ и таблица изменений.";}}catch(const std::exception& e){weight_message=e.what();}
    ImGui::TextWrapped("%s",weight_message.c_str());
    if(weight_result.distributions.size()==frames.size()){
        const float stats_height=std::min(240.f,(frames.size()+1)*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetStyle().ScrollbarSize+12);
        if(ImGui::BeginTable("weight_statistics",9,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp,{-1,stats_height},1250)){
            for(auto title:{"Снимок / t, с","N","Среднее","Медиана","σ (N)","Min","Max","Сумма","У min / max обучения"})ImGui::TableSetupColumn(title);ImGui::TableHeadersRow();for(size_t i=0;i<frames.size();++i){auto label=frames[i]->name+" / "+std::to_string(frames[i]->time);statistics(label.c_str(),weight_result.distributions[i].stats);}ImGui::EndTable();}
        std::vector<std::vector<double>> counts;std::vector<std::string> labels;for(size_t i=0;i<frames.size();++i){counts.push_back(weight_result.distributions[i].counts);labels.push_back(frames[i]->name+" @ "+std::to_string(frames[i]->time)+"###weight_"+std::to_string(i));}
        histogram_plot("Распределения — общие границы###weights_hist",weight_result.edges,counts,labels,weight_settings.absolute?"|w|":"w");
        const auto& delta=weight_result.difference;ImGui::TextWrapped("Δ = сравниваемый − базовый; для модуля: |w₂| − |w₁|. В пару входят связи, проходящие фильтры в обоих снимках.");
        if(!delta.error.empty())ImGui::TextWrapped("%s",delta.error.c_str());else {
            if(delta.stats.count)ImGui::Text("Пар: %zu; точно без изменения исходного w: %zu; среднее Δ: %.9g; σΔ: %.9g",delta.stats.count,delta.unchanged,delta.stats.mean,delta.stats.stddev);
            else ImGui::TextUnformatted("Нет пар, проходящих фильтры в обоих снимках. Статистики Δ не определены.");
            histogram_plot("Распределение изменений###delta_weights",delta.edges,{delta.counts},{"Δ"},weight_settings.absolute?"Δ|w|":"Δw");
            if(ImGui::BeginTable("largest_changes",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{-1,210})){
                for(auto t:{"Связь (база → цель)","Источник → приёмник","w₁","w₂","Δw","Δ представления"})ImGui::TableSetupColumn(t);ImGui::TableHeadersRow();for(size_t i=0;i<std::min<size_t>(100,delta.changes.size());++i){const auto& x=delta.changes[i];const auto& a=*frames[weight_reference];const auto& b=*frames[weight_target];const auto& e=a.config.synapses[x.before];ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%zu → %zu",x.before+1,x.after+1);ImGui::TableNextColumn();ImGui::Text("%s → %s",a.config.neurons[e.source].name.c_str(),a.config.neurons[e.target].name.c_str());for(double v:{a.weights[x.before],b.weights[x.after],x.raw_delta,x.delta}){ImGui::TableNextColumn();ImGui::Text("%.9g",v);}}ImGui::EndTable();}
        }
    }ImGui::End();
}
}
