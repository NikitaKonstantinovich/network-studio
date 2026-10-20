#include "gui.h"
#include "history_archive.h"
#include "implot.h"
#include <chrono>
#include <algorithm>
namespace studio {
void App::archive_manager(){
    if(!show_archive)return;
    ImGui::SetNextWindowSize({850,820},ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("История на диске###history_archive",&show_archive)){ImGui::End();return;}
    const auto& m=simulation->metrics;const auto& h=simulation->history;
    ImGui::TextWrapped("Экран: состояния [%.9g; %.9g] с, события после %.9g с",h.time.empty()?simulation->time:h.time.front(),simulation->time,h.events_after);
    ImGui::TextWrapped("ОЗУ вычислителя: состояния [%.9g; %.9g] с, события после %.9g с, стимулы после %.9g с",m.memory_samples_from,simulation->time,m.memory_events_from,m.memory_stimuli_from);
    if(m.archive_active)ImGui::TextWrapped("Диск (подтверждено): [%.9g; %.9g] с · %s · %s",m.disk_from,m.disk_until,m.archive_status.c_str(),m.archive_id.c_str());
    else ImGui::TextWrapped(m.archive_required?"Диск: checkpoint содержит привязку. Выберите продолжение архива либо новую ветвь.":"Диск: запись выключена.");
    ImGui::TextWrapped("Видимый масштаб отдельного графика не ограничивает архив. Последний буфер подтверждается при паузе, checkpoint и штатном завершении.");
    ImGui::Separator();ImGui::InputText("Каталог архива (UTF-8)",archive_path.data(),archive_path.size());
    if(m.archive_active){archive_deliveries=m.archive_deliveries;archive_rows=int(config.history_capacity);}
    ImGui::BeginDisabled(running||m.archive_active);
    ImGui::Checkbox("Записывать доставки по связям",&archive_deliveries);
    ImGui::InputInt("Отсчётов состояний в ОЗУ",&archive_rows);archive_rows=std::clamp(archive_rows,2,1000000);
    ImGui::TextWrapped("Остальные каналы включены всегда: спайки, выбросы астроцитов, стимулы, изменения протокола, выбранные состояния и веса. Доставки могут занимать существенно больше места.");
    if(ImGui::Button(m.archive_required?"Новая ветвь в новом каталоге":"Включить запись до запуска")){
        RunnerCommand c;c.kind=RunnerCommand::Kind::StartArchive;c.path=std::filesystem::u8path(archive_path.data());c.flag=archive_deliveries;c.index=archive_rows;command(std::move(c));
    }
    ImGui::BeginDisabled(!m.archive_required);
    if(ImGui::Button("Продолжить прежний архив из checkpoint")){RunnerCommand c;c.kind=RunnerCommand::Kind::StartArchive;c.path=std::filesystem::u8path(archive_path.data());c.flag=archive_deliveries;c.flag2=true;c.index=archive_rows;command(std::move(c));}
    ImGui::EndDisabled();ImGui::EndDisabled();
    if(m.archive_active&&ImGui::Button("Подтвердить буфер на диске")){RunnerCommand c;c.kind=RunnerCommand::Kind::FlushArchive;command(std::move(c));}
    ImGui::Separator();ImGui::TextWrapped("Анализ архива использует профиль встроенного окна «Сеть / стимуляция». Укажите абсолютный интервал. Файл читается в фоне без загрузки всего опыта в ОЗУ.");
    ImGui::SetNextItemWidth(150);ImGui::InputDouble("Архив: от, с",&archive_interval.from,0,0,"%.9g");ImGui::SameLine();ImGui::SetNextItemWidth(150);ImGui::InputDouble("Архив: до, с",&archive_interval.to,0,0,"%.9g");archive_interval.current_end=false;
    const bool busy=archive_job.valid();ImGui::BeginDisabled(busy);
    if(ImGui::Button("Рассчитать по архиву")){
        const auto path=std::filesystem::u8path(archive_path.data());const auto settings=plot_windows.embedded[7].analysis;const auto interval=archive_interval;
        archive_info_label.clear();archive_analysis_source=path.u8string();archive_profile={plot_windows.embedded[7].analysis_profile,settings};archive_message="Чтение подтверждённых блоков…";
        archive_job=std::async(std::launch::async,[path,settings,interval](){return analyze_archive(path,settings,interval);});
    }ImGui::EndDisabled();
    if(archive_job.valid()&&archive_job.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
        try{archive_result=archive_job.get();archive_message=archive_result.error.empty()?"Анализ завершён. Профиль и интервал зафиксированы в результате.":archive_result.error;}
        catch(const std::exception& e){archive_result={};archive_result.error=e.what();archive_message=e.what();}
    }
    ImGui::TextWrapped("%s",archive_message.c_str());
    if(!archive_analysis_source.empty()&&!archive_job.valid()&&archive_info_label.empty())try{
        const auto info=inspect_archive(std::filesystem::u8path(archive_analysis_source));archive_info_label="Источник: "+info.id+" · статус "+info.status+(info.orphan_tail?" · НЕПОДТВЕРЖДЁННЫЙ ХВОСТ ИСКЛЮЧЁН":"");
    }catch(const std::exception& e){archive_info_label=e.what();}
    if(!archive_info_label.empty())ImGui::TextWrapped("%s",archive_info_label.c_str());
    const auto& r=archive_result;
    if(r.error.empty()&&!r.time.empty()){
        ImGui::Text("Запрос [%.9g; %.9g) · доступно [%.9g; %.9g)",r.requested_from,r.requested_until,r.available_from,r.available_until);
        ImGui::Text("Фактически [%.9g; %.9g) · пачки %zu · ответ %zu / %zu",r.from,r.until,r.bursts.size(),r.responses.size()-r.missed,r.responses.size());
        if(!r.full_interval||!r.prehistory_complete)ImGui::TextColored({1,.65f,.2f,1},"НЕПОЛНОЕ покрытие или недостаточная предыстория; полное сравнение недоступно.");
        if(ImGui::Button("Экспорт результата дискового анализа…")&&file_dialog){auto path=file_dialog(true,true);if(!path.empty())try{path.replace_extension();export_analysis(r,archive_profile.settings,path,archive_profile.name);archive_message="Статистика, профиль и интервал экспортированы.";}catch(const std::exception& e){archive_message=e.what();}}
        if(ImPlot::BeginPlot("Активность из архива###archive_activity",{-1,200})){ImPlot::SetupAxes("Время, с","Спайки/с группы");ImPlot::PlotLine("Групповая частота",r.time.data(),r.group_hz.data(),int(r.time.size()));ImPlot::EndPlot();}
    }
    ImGui::End();
}
}
