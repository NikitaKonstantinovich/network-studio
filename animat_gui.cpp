#include "gui.h"
#include "animat.h"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace studio {
namespace {
bool scalar(const char* label,double& v,double lo,double hi) {
    ImGui::SetNextItemWidth(150);
    return ImGui::DragScalar(label,ImGuiDataType_Double,&v,.01f,&lo,&hi,"%.7g",ImGuiSliderFlags_AlwaysClamp);
}
}
bool edit_animat_environment(AnimatEnvironment& e,const ModelSpace& space) {
    const auto before=animat_environment_json(e);
    ImGui::Checkbox("Источник включён",&e.enabled);
    scalar("Вне зон, Гц",e.outside_frequency,0,200);ImGui::SameLine();scalar("Амплитуда источника",e.amplitude,-30,100);
    scalar("Радиус получателей",e.radius,0,std::hypot(space.width,space.height));ImGui::SameLine();
    double ms=e.width*1000;if(scalar("Ширина импульса, мс",ms,0,1000))e.width=ms*.001;
    if(ImGui::Button("+ Прямоугольник")&&e.zones.size()<MaxAnimatZones) {
        ArenaZone z;for(auto& old:e.zones)z.id=std::max(z.id,old.id+1);
        z.name="Зона "+std::to_string(z.id);z.width=space.width*.4;z.height=space.height*.5;e.zones.push_back(z);
    }
    ImGui::SameLine();
    if(ImGui::Button("+ Круг")&&e.zones.size()<MaxAnimatZones) {
        ArenaZone z;for(auto& old:e.zones)z.id=std::max(z.id,old.id+1);
        z.name="Зона "+std::to_string(z.id);z.shape=ZoneShape::Circle;z.x=space.width*.5;z.y=space.height*.5;
        z.width=std::min(space.width,space.height)*.2;z.height=z.width;e.zones.push_back(z);
    }
    int removed=-1;
    for(int i=0;i<int(e.zones.size());++i) {
        auto& z=e.zones[i];ImGui::PushID(i);
        if(ImGui::TreeNode((z.name+" #"+std::to_string(z.id)).c_str())) {
            char name[160];std::snprintf(name,sizeof(name),"%s",z.name.c_str());if(ImGui::InputText("Название",name,sizeof(name)))z.name=name;
            ImGui::InputInt("Приоритет",&z.priority);scalar("Частота зоны, Гц",z.frequency,0,200);
            scalar("X",z.x,0,space.width);ImGui::SameLine();scalar("Y",z.y,0,space.height);
            scalar(z.shape==ZoneShape::Circle?"Радиус":"Ширина",z.width,1e-9,std::max(space.width,space.height));
            if(z.shape==ZoneShape::Rectangle){ImGui::SameLine();scalar("Высота",z.height,1e-9,space.height);}
            if(ImGui::Button("Удалить зону"))removed=i;
            ImGui::TreePop();
        }ImGui::PopID();
    }
    if(removed>=0)e.zones.erase(e.zones.begin()+removed);
    ImGui::TextWrapped("Больший приоритет выигрывает; при равенстве выигрывает меньший ID. Прямоугольник [x,x+w) × [y,y+h), внешний край арены включён. Граница круга включена.");
    return before!=animat_environment_json(e);
}
void App::animat_manager() {
    if(!show_animat)return;
    ImGui::SetNextWindowSize({1100,820},ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("Нейроанимат###neuroanimat",&show_animat)){ImGui::End();return;}
    const auto& sim=*simulation;const auto& a=sim.animat;
    if(animat_generation!=sim.generation) {
        animat_generation=sim.generation;animat_draft=config.animat;animat_space=config.space;
        animat_environment_draft=a.environments.empty()?config.animat.environment:a.environments.back().value;
        animat_display_after=-1;animat_result={};animat_comparisons.clear();
    }
    if(animat_analysis_job.valid()&&animat_analysis_job.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=animat_analysis_job.get();if(result.ok&&result.animat_analysis)animat_result=*result.animat_analysis;
        else animat_message=result.error;animat_analysis_job={};
    }
    ImGui::Text("t = %.9g с · этап %d · следующий тик %.9g с · период %.9g с · %s",sim.time,sim.experiment.stage_index+1,a.next_step*config.dt,a.period_steps*config.dt,running?"расчёт":"пауза");
    if(ImGui::Button(running?"Пауза нейроанимата":"Продолжить нейроанимата")){if(running)pause();else run();}
    ImGui::SameLine();if(ImGui::Button("Этапы…"))show_stages=true;
    ImGui::SameLine();if(ImGui::Button("Архив…"))show_archive=true;
    ImGui::SameLine();if(ImGui::Button("Сохранить checkpoint…")&&checkpoint_dialog){auto p=checkpoint_dialog(true);if(!p.empty())save_state(p);}
    ImGui::SameLine();if(ImGui::Button("Восстановить checkpoint…")&&checkpoint_dialog){auto p=checkpoint_dialog(false);if(!p.empty())restore_state(p);}
    if(ImGui::BeginTabBar("animat_tabs")) {
        if(ImGui::BeginTabItem("Арена и траектория",nullptr,request_animat_tab==0?ImGuiTabItemFlags_SetSelected:0)) {
            if(!config.animat.enabled)ImGui::TextWrapped("Контроллер выключен. Откройте настройки или загрузите пример. Включение контроллера сбрасывает эксперимент.");
            if(ImGui::Button("Очистить только рисунок"))animat_display_after=sim.time;
            ImGui::SameLine();if(ImGui::Button("Показать доступную траекторию"))animat_display_after=-1;
            ImGui::SameLine();ImGui::BeginDisabled(running||!config.animat.enabled);
            if(ImGui::Button("Вернуть в начальную точку")) {RunnerCommand c;c.kind=RunnerCommand::Kind::AnimatAction;c.animat_action.kind=AnimatActionKind::Home;c.animat_action.time=sim.time;command(c);}
            ImGui::EndDisabled();
            ImGui::SameLine();
            if(ImGui::Button("Добавить траекторию из checkpoint…")&&checkpoint_dialog) {
                auto path=checkpoint_dialog(false);
                if(!path.empty())try {
                    auto saved=load_checkpoint(path);
                    if(!saved->config.animat.enabled||saved->config.space.width!=config.space.width||saved->config.space.height!=config.space.height||saved->config.space.unit!=config.space.unit)
                        throw std::runtime_error("Несовместимые арены: сравнение требует одинаковых размеров и единиц.");
                    if(animat_comparisons.size()>=4)throw std::runtime_error("Не более четырёх сравнительных траекторий.");
                    auto rows=std::move(saved->animat.rows);if(rows.size()>20000)rows.erase(rows.begin(),rows.end()-20000);
                    animat_comparisons.push_back({path.filename().u8string(),std::move(rows)});
                    animat_message="Сравнение показывает не более 20000 последних записей каждого checkpoint; полный экспорт не обрезается.";
                }catch(const std::exception& e){animat_message=e.what();}
            }
            const auto& space=config.space;
            const float inner_height=std::max(30.f,std::min(265.f,float((ImGui::GetContentRegionAvail().x-animat_plot_padding.x)*space.height/space.width)));
            const ImVec2 arena_size{float(inner_height*space.width/space.height)+animat_plot_padding.x,inner_height+animat_plot_padding.y};
            if(ImPlot::BeginPlot("Арена###animat_arena",arena_size,ImPlotFlags_NoInputs)) {
                ImPlot::SetupAxes(("X, "+space.unit).c_str(),("Y, "+space.unit+" (вниз)").c_str(),ImPlotAxisFlags_Lock,ImPlotAxisFlags_Lock|ImPlotAxisFlags_Invert);
                ImPlot::SetupAxesLimits(0,space.width,0,space.height,ImGuiCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_North,ImPlotLegendFlags_Outside|ImPlotLegendFlags_Horizontal);
                ImPlot::SetupFinish();auto pixels=ImPlot::GetPlotSize();
                animat_plot_padding={arena_size.x-pixels.x,arena_size.y-pixels.y};
                animat_render_scale_x=pixels.x/space.width;animat_render_scale_y=pixels.y/space.height;
                const auto& env=a.environments.empty()?config.animat.environment:a.environments.back().value;
                auto zones=env.zones;std::sort(zones.begin(),zones.end(),[](auto& x,auto& y){return x.priority!=y.priority?x.priority<y.priority:x.id>y.id;});
                ImPlot::PushPlotClipRect();auto* draw=ImPlot::GetPlotDrawList();
                for(const auto& z:zones) {
                    const ImU32 fill=z.frequency>env.outside_frequency?IM_COL32(245,130,90,65):IM_COL32(60,205,165,55);
                    if(z.shape==ZoneShape::Rectangle) {auto p=ImPlot::PlotToPixels(z.x,z.y),q=ImPlot::PlotToPixels(z.x+z.width,z.y+z.height);draw->AddRectFilled(p,q,fill);draw->AddRect(p,q,IM_COL32(180,195,205,180));}
                    else {auto p=ImPlot::PlotToPixels(z.x,z.y),q=ImPlot::PlotToPixels(z.x+z.width,z.y);draw->AddCircleFilled(p,std::abs(q.x-p.x),fill,64);draw->AddCircle(p,std::abs(q.x-p.x),IM_COL32(180,195,205,180),64);}
                    auto p=ImPlot::PlotToPixels(z.x,z.y);draw->AddText(p,IM_COL32_WHITE,z.name.c_str());
                }ImPlot::PopPlotClipRect();
                auto trace=[&](const char* name,const std::vector<AnimatRow>& rows,double after) {
                    std::vector<double> x,y;for(const auto& r:rows)if(r.time>after){x.push_back(r.x);y.push_back(r.y);}
                    if(!x.empty())ImPlot::PlotLine(name,x.data(),y.data(),int(x.size()));
                };
                trace("Текущая",a.rows,animat_display_after);
                for(const auto& other:animat_comparisons)trace(other.first.c_str(),other.second,-1);
                double x=a.x,y=a.y;if(config.animat.enabled)ImPlot::PlotScatter("Робот",&x,&y,1);
                x=config.animat.initial_x;y=config.animat.initial_y;ImPlot::PlotScatter("Начальная точка",&x,&y,1);
                ImPlot::EndPlot();
            }
            if(!a.rows.empty()){const auto& r=a.rows.back();ImGui::Text("X %.7g · Y %.7g · зона #%llu · поле (%.6g, %.6g)",a.x,a.y,r.zone,r.vx,r.vy);
                ImGui::Text("Источник #%llu: %.7g Гц · запрос %.7g Гц · применение %.9g с",config.animat.stimulator,r.frequency,r.requested_frequency,r.apply_time);
                for(const auto& source:config.stimulators)if(source.id==config.animat.stimulator){const double next=next_stimulator_pulse(source,sim.stimulus_commands,sim.time);if(std::isfinite(next))ImGui::Text("Следующий импульс: %.9g с",next);else ImGui::TextUnformatted(source.enabled?"Новых импульсов не запланировано / источник завершён":"Источник выключен");}
                ImGui::Text("Экран: [%.7g; %.7g] с, %zu записей. Полные данные и статистика находятся у вычислителя.",a.rows.front().time,a.rows.back().time,a.rows.size());}
            if(ImPlot::BeginPlot("Координаты и время###animat_coordinates",{-1,160})) {
                ImPlot::SetupAxes("Время, с",config.space.unit.c_str());std::vector<double> t,x,y;
                for(const auto& r:a.rows){t.push_back(r.time);x.push_back(r.x);y.push_back(r.y);}
                if(!t.empty()){ImPlot::PlotStairs("X",t.data(),x.data(),int(t.size()));ImPlot::PlotStairs("Y",t.data(),y.data(),int(t.size()));}ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Настройки контроллера",nullptr,request_animat_tab==1?ImGuiTabItemFlags_SetSelected:0)) {
            ImGui::BeginDisabled(running);
            if(ImGui::Button("Минимальный пример (сброс)"))replace(animat_example(false));
            ImGui::SameLine();if(ImGui::Button("Пространственная сеть (сброс)"))replace(animat_example(true));
            ImGui::Checkbox("Включить нейроанимата",&animat_draft.enabled);
            scalar("Ширина арены",animat_space.width,1e-6,1e9);ImGui::SameLine();scalar("Высота арены",animat_space.height,1e-6,1e9);
            char unit[64];std::snprintf(unit,sizeof(unit),"%s",animat_space.unit.c_str());if(ImGui::InputText("Единицы",unit,sizeof(unit)))animat_space.unit=unit;
            scalar("Начальный X",animat_draft.initial_x,0,animat_space.width);ImGui::SameLine();scalar("Начальный Y",animat_draft.initial_y,0,animat_space.height);
            scalar("Интервал контроллера, с",animat_draft.interval,config.dt,1e6);
            ImGui::TextDisabled("Принимается целое число шагов решателя, округление вверх; dt = %.7g с.",config.dt);
            scalar("Коэффициент перемещения",animat_draft.gain,0,1e6);scalar("Макс. перемещение за обновление",animat_draft.max_displacement,0,1e9);
            scalar("Порог нулевого вектора",animat_draft.zero_threshold,0,1e6);
            int boundary=int(animat_draft.boundary),silence=int(animat_draft.silence);
            if(ImGui::Combo("Граница арены",&boundary,"Обрезать компоненты\0Отменить выходящее перемещение\0"))animat_draft.boundary=AnimatBoundary(boundary);
            if(ImGui::Combo("Нет активности",&silence,"Оставаться на месте\0Повторить последнее смещение (доп. режим)\0"))animat_draft.silence=AnimatSilence(silence);
            auto& f=animat_draft.field;ImGui::SliderInt("Столбцы поля",&f.columns,1,64);ImGui::SliderInt("Строки поля",&f.rows,1,64);
            ImGui::Combo("Критерий активности",&f.activity_mode,"Доставки\0Доставки с последующим ответом\0");
            scalar("Окно активности, с",f.activity_window,1e-6,config.activity_horizon);
            scalar("Ответ от, с",f.response_min,0,1);ImGui::SameLine();scalar("Ответ до, с",f.response_max,f.response_min,1);
            int polarity=int(f.activity_polarity),model=int(f.activity_model);
            if(ImGui::Combo("Фильтр E/I",&polarity,"Все\0Возбуждающие\0Тормозные\0"))f.activity_polarity=FieldPolarity(polarity);
            if(ImGui::Combo("Модель связей",&model,"Все\0STP\0Экспоненциальная\0TM\0"))f.activity_model=FieldModel(model);
            if(ImGui::BeginCombo("Связанный пространственный источник",std::to_string(animat_draft.stimulator).c_str())) {
                for(const auto& s:config.stimulators)if(s.spatial&&ImGui::Selectable((std::to_string(s.id)+" "+s.name).c_str(),s.id==animat_draft.stimulator))animat_draft.stimulator=s.id;
                ImGui::EndCombo();
            }
            if(ImGui::Button("Применить настройки и сбросить опыт"))try {
                Config candidate=config;candidate.space=animat_space;candidate.animat=animat_draft;
                candidate.animat.environment=animat_environment_draft;auto issue=validate(candidate);if(!issue.empty())throw std::runtime_error(issue);config=std::move(candidate);reset();dirty=true;
            }catch(const std::exception& e){animat_message=e.what();}
            ImGui::EndDisabled();
            ImGui::TextWrapped("Нормализованный вектор преобразуется (W*vx, H*vy), затем умножается на коэффициент и ограничивается по физической длине. Масштаб стрелок, камера и частота GUI не участвуют. Совпадение доставки и ответа не доказывает причинность.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Среда",nullptr,request_animat_tab==2?ImGuiTabItemFlags_SetSelected:0)) {
            ImGui::BeginDisabled(running);
            if(ImGui::Button("Загрузить действующую среду"))animat_environment_draft=a.environments.empty()?config.animat.environment:a.environments.back().value;
            edit_animat_environment(animat_environment_draft,animat_space);
            if(ImGui::Button("Применить среду без сброса")) {
                RunnerCommand c;c.kind=RunnerCommand::Kind::AnimatAction;c.animat_action.kind=AnimatActionKind::Environment;
                c.animat_action.time=sim.time;c.animat_action.environment=animat_environment_draft;command(c);
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped("Смена частоты завершает текущий период. До первого импульса его время сохраняется. Более новый запрос зоны заменяет неисполненный запрос контроллера; никаких дополнительных импульсов при перемещении нет.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Статистика и экспорт",nullptr,request_animat_tab==3?ImGuiTabItemFlags_SetSelected:0)) {
            scalar("От, с",animat_from,0,1e9);ImGui::SameLine();scalar("До, с",animat_to,0,1e9);
            if(ImGui::Button("До текущего времени"))animat_to=sim.time;
            ImGui::BeginDisabled(!config.animat.enabled||animat_to<=animat_from||animat_analysis_job.valid());
            if(ImGui::Button("Рассчитать по полной траектории")) {RunnerCommand c;c.kind=RunnerCommand::Kind::AnalyzeAnimat;c.value=animat_from;c.second=animat_to;animat_analysis_job=command(c);}
            ImGui::SameLine();
            if(ImGui::Button("Экспорт траектории и анализа…")&&file_dialog) {
                auto p=file_dialog(true,true);if(!p.empty()){if(p.extension()==L".csv")p.replace_extension();RunnerCommand c;c.kind=RunnerCommand::Kind::ExportAnimat;c.path=p;c.value=animat_from;c.second=animat_to;command(c);}
            }ImGui::EndDisabled();
            const auto& r=animat_result;
            ImGui::Text("Запрошено [%.7g; %.7g), доступно [%.7g; %.7g) · %s",r.requested_from,r.requested_to,r.from,r.to,r.complete?"полный интервал":"НЕПОЛНЫЙ / не рассчитан");
            ImGui::Text("Путь %.7g · средняя скорость %.7g · первый выход %.7g с (-1: нет)",r.path,r.mean_speed,r.first_exit);
            if(ImGui::BeginTable("animat_stage_stats",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
                for(const char* h:{"Вход в этап","Этап","Время","Путь","Скорость","Первый выход, с"})ImGui::TableSetupColumn(h);ImGui::TableHeadersRow();
                for(const auto& s:r.stages){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%llu",s.run);ImGui::TableNextColumn();ImGui::Text("%d",s.stage+1);ImGui::TableNextColumn();ImGui::Text("%.7g",s.time);ImGui::TableNextColumn();ImGui::Text("%.7g",s.path);ImGui::TableNextColumn();ImGui::Text("%.7g",s.mean_speed);ImGui::TableNextColumn();ImGui::Text("%.7g",s.first_exit);}
                ImGui::EndTable();
            }
            if(ImGui::BeginTable("animat_zone_stats",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
                for(const char* h:{"Вход в этап","Зона","Время","Доля интервала","Входы","Выходы"})ImGui::TableSetupColumn(h);ImGui::TableHeadersRow();
                for(const auto& z:r.zones){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%llu",z.stage_run);ImGui::TableNextColumn();ImGui::Text("%llu",z.zone);ImGui::TableNextColumn();ImGui::Text("%.7g",z.time);ImGui::TableNextColumn();ImGui::Text("%.5g",z.fraction);ImGui::TableNextColumn();ImGui::Text("%llu",z.entries);ImGui::TableNextColumn();ImGui::Text("%llu",z.exits);}ImGui::EndTable();
            }
            ImGui::TextWrapped("Положение удерживается между обновлениями. Перемещение мгновенное: пересечённые транзитом зоны имеют нулевое время пребывания. Посещения на границах интервала помечаются в CSV как усечённые. Возврат в начало - телепортация без добавления пути.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextWrapped("%s",animat_message.c_str());ImGui::TextWrapped("%s",status.c_str());ImGui::End();
}
}
