#include "gui.h"
#include "weight_field_projection.h"
#include <algorithm>
#include <cmath>

namespace studio {
void App::field_controls() {
    auto& v=plot_windows.topology;
    const SimulationReadView current(*simulation);
    const auto& field_config=changed?config:current.config;
    ImGui::Checkbox("Векторное поле весов",&v.field);
    ImGui::SameLine();ImGui::Checkbox("Поле активности",&v.activity);
    ImGui::Checkbox("Обычные связи",&v.edges);ImGui::SameLine();ImGui::Checkbox("Сетка",&v.grid);
    if(ImGui::GetContentRegionAvail().x>180)ImGui::SameLine();
    if(ImGui::SmallButton("Вписать схему")){v.zoom=1;v.pan_x=v.pan_y=0;}
    if(v.field) {
        if(ImGui::Button("Настройки поля…"))ImGui::OpenPopup("weight_field_settings");
        ImGui::SetNextWindowSize({430*ImGui::GetFontSize()/17,0},ImGuiCond_Appearing);
        if(ImGui::BeginPopup("weight_field_settings")) {
            int source=v.current?1:0;const char* sources[]={"Начальные веса","Текущие веса"};
            ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##field_source",&source,sources,2))v.current=source==1;
            ImGui::SetNextItemWidth(100);ImGui::DragInt("Столбцы",&v.columns,.2f,1,64,"%d",ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetNextItemWidth(100);ImGui::DragInt("Строки",&v.rows,.2f,1,64,"%d",ImGuiSliderFlags_AlwaysClamp);
            double lo=1e-6,hi=100;ImGui::SetNextItemWidth(130);const double previous=v.scale;
            ImGui::DragScalar("Масштаб стрелок",ImGuiDataType_Double,&v.scale,.001f,&lo,&hi,"%.5g",ImGuiSliderFlags_AlwaysClamp);
            if(!std::isfinite(v.scale))v.scale=previous;
            ImGui::Checkbox("Автомасштаб поля",&v.auto_scale);
            int polarity=int(v.polarity);const char* polarities[]={"Все","Возбуждающие","Тормозные"};
            ImGui::SetNextItemWidth(230);if(ImGui::Combo("Тип связей",&polarity,polarities,3))v.polarity=FieldPolarity(polarity);
            int model=int(v.model);const char* models[]={"Все модели","Проводимость / STP","Экспоненциальный ток","Цодыкс–Маркрам"};
            ImGui::SetNextItemWidth(230);if(ImGui::Combo("Модель поля",&model,models,4))v.model=FieldModel(model);
            ImGui::Separator();
            ImGui::TextWrapped("Сумма |w| по направлениям связей. Отрицательный вес не разворачивает стрелку. Проводимость, ток и активность в сумму не входят; шкалы разных моделей могут различаться.");
            ImGui::TextWrapped("Масштаб — длина в координатах модели на единицу веса. Автомасштаб меняет общий коэффициент: выключите его для сравнения. Золотой маркер на конце означает ограничение длины ячейкой.");
            ImGui::TextWrapped("Сетка привязана к [0,1] × [0,1]; X вправо, Y вниз. Колесо: масштаб схемы; средняя кнопка: перемещение. Наведите на ячейку для чисел.");
            if(ImGui::Button("Готово"))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        weight_field.update(field_config,changed?nullptr:&current.plasticity,v);
        if(field_config.neurons.empty())ImGui::TextDisabled("Нет нейронов для построения поля.");
        else if(!weight_field.available)ImGui::TextWrapped("Текущие веса недоступны до обновления эксперимента.");
        else if(!weight_field.included_edges)ImGui::TextWrapped("Нет подходящих направленных связей.");
        else {
            ImGui::TextDisabled("%s · %zu связей · max |V| %.3g",v.current?"Текущие":"Начальные",weight_field.included_edges,weight_field.maximum);
            if(weight_field.maximum==0)ImGui::TextDisabled("Нулевое поле: веса нулевые или вклады компенсируются.");
            const double cap=.8/std::max(v.columns,v.rows);
            const double scale=v.auto_scale&&weight_field.maximum>0?cap/weight_field.maximum:v.scale;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x);
            ImGui::TextDisabled("t = %.6g с · k = %.4g (%s) · %.3f мс",v.current?current.time:0,scale,v.auto_scale?"авто":"фикс.",weight_field.update_ms);
            ImGui::PopTextWrapPos();
        }
    }
    if(v.activity) {
        if(ImGui::Button("Настройки активности…"))ImGui::OpenPopup("activity_field_settings");
        if(ImGui::BeginPopup("activity_field_settings")) {
            const char* modes[]={"Доставки по связям","Доставки с последующим ответом"};ImGui::SetNextItemWidth(320);ImGui::Combo("Критерий",&v.activity_mode,modes,2);
            auto number=[](const char* label,double& value,double lo,double hi){const double previous=value;ImGui::SetNextItemWidth(140);ImGui::DragScalar(label,ImGuiDataType_Double,&value,.001f,&lo,&hi,"%.6g",ImGuiSliderFlags_AlwaysClamp);value=std::isfinite(value)?std::clamp(value,lo,hi):previous;};
            number("Окно активности, с",v.activity_window,.000001,10);number("Ответ от, с",v.response_min,0,10);v.response_max=std::max(v.response_max,v.response_min);number("Ответ до, с",v.response_max,v.response_min,10);
            number("Масштаб активности",v.activity_scale,.000001,100);
            int polarity=int(v.activity_polarity);const char* types[]={"Все","Возбуждающие","Тормозные"};if(ImGui::Combo("Тип активных связей",&polarity,types,3))v.activity_polarity=FieldPolarity(polarity);
            int model=int(v.activity_model);const char* models[]={"Все модели","STP","Экспоненциальный ток","TM"};if(ImGui::Combo("Модель активных связей",&model,models,4))v.activity_model=FieldModel(model);
            ImGui::TextWrapped("Ответ после доставки — временное совпадение, не доказательство причинности. Все подходящие доставки могут разделять один постспайк. Одновременные доставка и спайк исключаются. Тормозной эффект этот критерий не измеряет.");
            ImGui::TextWrapped("Каждая активная связь даёт |текущий вес| один раз за окно. Исторический просмотр не подменяется конечными весами: доступен только текущий момент/пауза.");
            ImGui::Text("Глубина записи %.6g с; окно после %.9g с",current.config.activity_horizon,current.activity.available_after);
            if(ImGui::Button("Готово"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(!v.field)weight_field.update(field_config,changed?nullptr:&current.plasticity,v);
        if(!changed&&current.error.empty())activity_field.update(current,v,weight_field);
        else {activity_field.available=false;activity_field.message="Поле активности недоступно до обновления эксперимента.";}
        ImGui::TextColored({1,.6f,.25f,1},"Оранжевое: активность · синее: веса · t=%.6g с · %.3f мс",activity_field.time,activity_field.update_ms);
        if(!activity_field.available)ImGui::TextWrapped("%s",activity_field.message.c_str());
    }
}
void App::field_overlay(ImVec2 base,ImVec2 span,bool tooltip) {
    const auto& v=plot_windows.topology;if((!v.field&&!v.activity)||!weight_field.available)return;
    auto* d=ImGui::GetWindowDrawList();const double cap=.8/std::max(v.columns,v.rows);
    const double scale=v.auto_scale&&weight_field.maximum>0?cap/weight_field.maximum:v.scale;
    const float dpi=ImGui::GetFontSize()/17;
    auto render=[&](const std::vector<WeightCell>& cells,double scale,ImU32 color,float offset){
      for(int y=0;y<v.rows;++y)for(int x=0;x<v.columns;++x) {
        const auto& cell=cells[y*v.columns+x];const double magnitude=cell.magnitude();
        if(magnitude==0)continue;
        const auto arrow=project_weight_arrow(cell,scale,cap,span.x,span.y);
        const ImVec2 center{base.x+float((x+.5)/v.columns)*span.x,base.y+float((y+.5)/v.rows)*span.y+offset};
        const ImVec2 delta{float(arrow.x),float(arrow.y)};
        const float length=std::hypot(delta.x,delta.y);if(length<.5f*dpi)continue;
        const ImVec2 a{center.x-delta.x*.5f,center.y-delta.y*.5f},b{center.x+delta.x*.5f,center.y+delta.y*.5f};
        const ImVec2 unit{delta.x/length,delta.y/length};const float head=std::min(7*dpi,length*.35f);
        d->AddLine(a,b,color,1.7f*dpi);
        d->AddTriangleFilled(b,{b.x-head*unit.x-head*.45f*unit.y,b.y-head*unit.y+head*.45f*unit.x},
            {b.x-head*unit.x+head*.45f*unit.y,b.y-head*unit.y-head*.45f*unit.x},color);
        if(arrow.saturated)d->AddCircle(b,3*dpi,IM_COL32(255,190,90,255),8,1.5f*dpi);
      }
    };
    if(v.field)render(weight_field.cells,scale,IM_COL32(131,188,255,255),v.activity?-2.f:0.f);
    if(v.activity&&activity_field.available&&activity_field.cells.size()==weight_field.cells.size())render(activity_field.cells,v.activity_scale,IM_COL32(255,155,60,255),v.field?2.f:0.f);
    if(tooltip&&!ImGui::IsMouseDragging(ImGuiMouseButton_Middle)&&!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const auto mouse=ImGui::GetIO().MousePos;const double mx=(mouse.x-base.x)/span.x,my=(mouse.y-base.y)/span.y;
        if(mx>=0&&mx<=1&&my>=0&&my<=1) {
            const int x=std::min(v.columns-1,int(mx*v.columns)),y=std::min(v.rows-1,int(my*v.rows));
            const auto& cell=weight_field.cells[y*v.columns+x];
            ImGui::BeginTooltip();ImGui::Text("Ячейка [%d, %d] · Связей: %zu",x+1,y+1,cell.count);
            ImGui::Text("Vx = %.9g · Vy = %.9g",cell.x,cell.y);ImGui::Text("|V| = %.9g",cell.magnitude());
            ImGui::Text("Масштаб = %.6g (%s)",scale,v.auto_scale?"авто":"фиксированный");
            if(v.activity&&activity_field.available&&activity_field.cells.size()==weight_field.cells.size()) {
                const auto& a=activity_field.cells[y*v.columns+x];ImGui::Separator();ImGui::Text("Активных связей %zu · событий %llu",a.count,a.events);
                ImGui::Text("Активность: Vx %.9g · Vy %.9g · |V| %.9g",a.x,a.y,a.magnitude());
            }
            if(cell.magnitude()*scale>cap)ImGui::TextColored({1,.75f,.35f,1},"Длина насыщена; числа приведены без ограничения.");
            ImGui::EndTooltip();
        }
    }
}
}
