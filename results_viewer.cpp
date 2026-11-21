#include "results_viewer.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace studio {

void ResultsViewer::open(const std::filesystem::path& file) {
    if(file.empty()||loading())return;
    failure.clear();
    visible=focus=true;
    job=std::async(std::launch::async,[file]{return load_saved_results(file);});
}

void ResultsViewer::poll(bool wait) {
    if(!loading()||(!wait&&job.wait_for(std::chrono::seconds(0))!=std::future_status::ready))return;
    try {
        auto replacement=std::make_unique<SavedResults>(job.get());
        PlotWindows replacement_plots;
        replacement_plots.data_changed(replacement->view.config,true);
        for(auto& w:replacement_plots.embedded)w.follow=false;
        replacement_plots.embedded[int(PlotKind::Neuron)].view=1;
        if(!replacement->view.config.record_neurons.empty())
            replacement_plots.embedded[int(PlotKind::Neuron)].object=replacement->view.config.record_neurons.front();
        if(!replacement->view.config.record_synapses.empty())
            replacement_plots.embedded[int(PlotKind::Synapse)].object=replacement->view.config.record_synapses.front();
        plots=std::move(replacement_plots);
        result=std::move(replacement);
        const double end=result->view.time;
        shown=draft={std::max(0.,end-10),end};
        const auto& channels=result->view.plot_channels;
        select(channels.events?PlotKind::Raster:channels.states?PlotKind::Voltage:
            channels.weights?PlotKind::Synapse:channels.deliveries?PlotKind::Deliveries:PlotKind::Network);
        if(!channels.states&&channels.weights)plots.embedded[int(PlotKind::Synapse)].view=1;
        failure.clear();
    } catch(const std::exception& e) {
        failure=e.what();
    }
}

void ResultsViewer::select(PlotKind kind) {requested_tab=int(kind);}

bool ResultsViewer::set_interval(PlotInterval value) {
    if(!result||!std::isfinite(value.from)||!std::isfinite(value.to)||value.from<0||
       value.to<=value.from||value.to>result->view.time)return false;
    shown=draft=value;
    for(auto& w:plots.embedded){w.fit=true;w.restore_limits=false;}
    return true;
}

void ResultsViewer::draw() {
    poll();
    if(!visible)return;
    const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x+40,viewport->WorkPos.y+40},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(1180.f,viewport->WorkSize.x-55),std::min(720.f,viewport->WorkSize.y-55)},ImGuiCond_FirstUseEver);
    if(focus){ImGui::SetNextWindowFocus();focus=false;}
    if(ImGui::Begin("Сохранённые результаты###saved_results",&visible,ImGuiWindowFlags_NoSavedSettings)) {
        if(loading())ImGui::TextUnformatted("Чтение комплекта результатов…");
        if(!failure.empty())ImGui::TextWrapped("Не удалось открыть результаты. %s",failure.c_str());
        if(result) {
            ImGui::TextWrapped("%s · %zu нейронов · до %.9g с",result->prefix.filename().u8string().c_str(),result->view.config.neurons.size(),result->view.time);
            if(result->view.time<=0)ImGui::TextWrapped("В комплекте только начальное состояние. Для графика нужен интервал времени.");
            else {
                ImGui::SetNextItemWidth(95);ImGui::InputDouble("От, с",&draft.from,0,0,"%.9g");
                ImGui::SameLine();ImGui::SetNextItemWidth(95);ImGui::InputDouble("До, с",&draft.to,0,0,"%.9g");
                ImGui::SameLine();
                const bool valid=std::isfinite(draft.from)&&std::isfinite(draft.to)&&draft.from>=0&&draft.to>draft.from&&draft.to<=result->view.time;
                ImGui::BeginDisabled(!valid);
                if(ImGui::Button("Показать интервал"))set_interval(draft);
                ImGui::EndDisabled();
                ImGui::SameLine();if(ImGui::Button("Весь расчёт"))set_interval({0,result->view.time});
                if(!valid)ImGui::TextWrapped("Требуется 0 ≤ От < До ≤ %.9g с.",result->view.time);
                ImGui::TextDisabled("Показано %.9g–%.9g с · интервал общий для вкладок",shown.from,shown.to);
                if(ImGui::BeginTabBar("saved_result_plots",ImGuiTabBarFlags_FittingPolicyScroll)) {
                    for(int i=0;i<int(plots.embedded.size());++i) {
                        auto& w=plots.embedded[i];
                        const auto flags=requested_tab==i?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None;
                        if(ImGui::BeginTabItem(plot_kind_title(w.kind),nullptr,flags)) {
                            ImGui::PushID(i);
                            plots.draw_view(w,result->view,ImGui::GetContentRegionAvail().y,true,&shown);
                            ImGui::PopID();
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                    requested_tab=-1;
                }
            }
        }
    }
    ImGui::End();
}

}
