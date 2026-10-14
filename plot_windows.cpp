#include "plot_windows.h"
#include "imgui_internal.h"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>

namespace studio {
namespace {
const char* kinds[]={"Потенциалы","Астроциты","Растр событий","Синапсы","Фазовый портрет","Нейрон / вход","Доставки"};
const ImVec4 teal(.25f,.84f,.72f,1),blue(.38f,.65f,1,1),gold(1,.74f,.36f,1),rose(.93f,.43f,.53f,1);
ImVec4 color(int i) { const ImVec4 colors[]={teal,blue,gold,rose,ImVec4(.72f,.56f,.94f,1)};return colors[i%5]; }
void bit(const char* label,uint32_t& mask,int n) {bool v=(mask&(1u<<n))!=0;if(ImGui::Checkbox(label,&v))mask^=1u<<n;}
bool recorded(const History& h,int column) {return column>=0&&column<int(h.values.size())&&!h.time.empty()&&h.values[column].size()==h.time.size();}
struct Derived { const Simulator* s; int object,kind; };
ImPlotPoint derived(int row,void* ptr) {
    const auto& d=*static_cast<Derived*>(ptr);const auto& s=*d.s;const auto& h=s.history;
    double v=0;
    if(d.kind==0)v=external_current(s.config,d.object,h.time[row]); // immutable run protocol, never App::config
    else {
        const auto& syn=s.config.synapses[d.object];const int b=syn_offset(s.config,d.object);
        if(d.kind==1)v=1-h.values[b+1][row]-h.values[b+2][row];
        else {
            const double scale=syn.model==SynapseModel::ConductanceSTP?syn.conductance*(syn.reversal-h.values[syn.target*4][row]):syn.gain;
            v=syn.enabled?h.weights[d.object][row]*scale*h.values[b+2][row]:0;
        }
    }
    return {h.time[row],v};
}
struct Events {const Simulator* s;int first,object;bool astro,peak;};
ImPlotPoint event_point(int row,void* ptr) {
    const auto& d=*static_cast<Events*>(ptr);const auto& e=d.s->history.events[d.first+row];
    if(e.astro!=d.astro||(d.object>=0&&e.index!=d.object))return {e.time,std::numeric_limits<double>::quiet_NaN()};
    const double peak=d.peak?(d.s->config.neurons[e.index].model==NeuronModel::Izhikevich?d.s->config.neurons[e.index].izh_peak:d.s->config.parameters[na::V_spike]):0;
    return {e.time,d.peak?peak:double(e.index+1+(e.astro?d.s->config.neurons.size():0))};
}
void select_objects(PlotWindowState& w,const Config& c,bool astro) {
    const int count=int(astro?c.astrocytes.size():c.neurons.size());
    if(ImGui::Button("Объекты…"))ImGui::OpenPopup("objects");
    if(ImGui::BeginPopup("objects")) {
        ImGui::TextDisabled("До 32 рядов; выбор независим от инспектора");
        if(ImGui::Button("Первые записанные")) {
            w.objects.clear();for(int i=0;i<count&&w.objects.size()<32;++i)if(astro||records_neuron(c,i))w.objects.push_back(i);
        }
        ImGui::BeginChild("list",{310,210});
        ImGuiListClipper clip;clip.Begin(count);
        while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i) {
            ImGui::PushID(i);bool chosen=std::find(w.objects.begin(),w.objects.end(),i)!=w.objects.end();
            std::string label=(astro?"A":"N")+std::to_string(i+1)+" "+(astro?c.astrocytes[i].name:c.neurons[i].name);
            if(ImGui::Checkbox(label.c_str(),&chosen)) {
                if(chosen&&w.objects.size()<32)w.objects.push_back(i);
                if(!chosen)w.objects.erase(std::remove(w.objects.begin(),w.objects.end(),i),w.objects.end());
                w.fit=true;
            }ImGui::PopID();
        }ImGui::EndChild();ImGui::EndPopup();
    }
}
}
std::string PlotWindowState::window_name() const {return title+"###studio_plot_"+std::to_string(id);}
PlotWindows::PlotWindows() {
    for(int i=0;i<7;++i){embedded[i].kind=PlotKind(i);embedded[i].title=kinds[i];embedded[i].place=false;}
    embedded[1].quantities=6;
}
void PlotWindows::data_changed(const Config& c,bool replacement) {
    binding=config_binding(c);
    for(auto& w:windows) {w.compatible=!replacement&&w.binding==binding;w.fit=true;}
    for(int i=0;i<7;++i) {
        auto& w=embedded[i];w.binding=binding;w.compatible=true;w.fit=true;w.object=0;w.objects.clear();
        if(i==0)for(int n=0;n<int(c.neurons.size())&&w.objects.size()<32;++n)if(records_neuron(c,n))w.objects.push_back(n);
        if(i==1)for(int n=0;n<int(c.astrocytes.size());++n)w.objects.push_back(n);
    }
    if(std::any_of(windows.begin(),windows.end(),[](const auto& w){return !w.compatible;}))
        message="Сеть/протокол изменились. Проверьте и явно перепривяжите отдельные графики.";
}
uint32_t PlotWindows::detach(const PlotWindowState& source) {
    if(windows.size()>=MaxWindows){message="Открыто максимальное число окон: 32.";return 0;}
    if(next_id>=UINT32_MAX-1){message="Исчерпан диапазон ID окон; откройте новое рабочее пространство.";return 0;}
    auto w=source;w.id=next_id++;w.open=true;w.place=true;w.undock=true;w.restore_limits=w.limits_valid;
    w.position={120.f+float(w.id%6)*28,120.f+float(w.id%6)*28};windows.push_back(std::move(w));return windows.back().id;
}
uint32_t PlotWindows::create(PlotKind kind,int object) {
    PlotWindowState w;w.kind=kind;w.title=kinds[int(kind)];w.binding=binding;w.object=object;w.objects={object};
    if(kind==PlotKind::Astrocytes)w.quantities=6;
    return detach(w);
}
PlotWindowState* PlotWindows::find(uint32_t id) {for(auto& w:windows)if(w.id==id)return &w;return nullptr;}
uint32_t PlotWindows::duplicate(uint32_t id) {
    auto* w=find(id);if(!w)return 0;auto copy=*w;
    if(copy.title.size()>230){size_t end=230;while((static_cast<unsigned char>(copy.title[end])&0xc0)==0x80)--end;copy.title.resize(end);}
    copy.title+=" — копия";return detach(copy);
}
bool PlotWindows::close(uint32_t id) {auto old=windows.size();windows.erase(std::remove_if(windows.begin(),windows.end(),[&](const auto& w){return w.id==id;}),windows.end());return windows.size()!=old;}
void PlotWindows::rebind(PlotWindowState& w) {w.binding=binding;w.compatible=true;w.fit=true;}
void PlotWindows::clamp_to_monitors(ImVec2& p,ImVec2& size) {
    const auto& monitors=ImGui::GetPlatformIO().Monitors;
    ImVec2 origin=ImGui::GetMainViewport()->WorkPos,area=ImGui::GetMainViewport()->WorkSize;
    double distance=std::numeric_limits<double>::max();
    for(const auto& m:monitors) {
        double dx=std::max({double(m.WorkPos.x-p.x),0.,double(p.x-m.WorkPos.x-m.WorkSize.x)});
        double dy=std::max({double(m.WorkPos.y-p.y),0.,double(p.y-m.WorkPos.y-m.WorkSize.y)});
        if(dx*dx+dy*dy<distance){distance=dx*dx+dy*dy;origin=m.WorkPos;area=m.WorkSize;}
    }
    if(area.x<100||area.y<100)return; // minimized main viewport in backend-free tests
    size.x=std::min(std::max(360.f,size.x),area.x);size.y=std::min(std::max(240.f,size.y),area.y);
    p.x=std::clamp(p.x,origin.x,origin.x+area.x-size.x);p.y=std::clamp(p.y,origin.y,origin.y+area.y-size.y);
}
void PlotWindows::reset_layout() {
    for(auto& w:windows){w.position={100.f+float(w.id%6)*28,100.f+float(w.id%6)*28};w.size={720,480};w.place=w.undock=true;}
    reset_host=true;
}
void PlotWindows::demo() {
    for(auto kind:{PlotKind::Voltage,PlotKind::Neuron,PlotKind::Raster,PlotKind::Synapse}) {
        auto id=create(kind);if(auto* w=find(id)){if(kind==PlotKind::Synapse)w->view=1;}
    }
    host_open=true;demo_layout=true;
}
void PlotWindows::draw(const Simulator& s) {
    if(!pending_ini.empty()){ImGui::LoadIniSettingsFromMemory(pending_ini.data(),pending_ini.size());pending_ini.clear();}
    const ImGuiID dock=ImHashStr("studio_plot_dock");
    const bool move_host=reset_host;
    if(reset_host){ImGui::DockBuilderRemoveNode(dock);reset_host=false;}
    if(host_open) {
        ImGui::SetNextWindowSize({1320,880},ImGuiCond_FirstUseEver);
        if(move_host){ImVec2 pos{80,80},size{1100,720};clamp_to_monitors(pos,size);ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize(size);}
        if(ImGui::Begin("Рабочая область графиков###studio_plot_host",&host_open,ImGuiWindowFlags_NoDocking)) {
            ImGui::TextDisabled("Перетащите заголовок графика к центру для вкладки или к краю для разделения.");
            if(demo_layout&&windows.size()>=4) {
                ImGui::DockBuilderRemoveNode(dock);ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dock,ImGui::GetContentRegionAvail());
                ImGuiID right, left,bottom_left,bottom_right;
                ImGui::DockBuilderSplitNode(dock,ImGuiDir_Left,.5f,&left,&right);
                ImGui::DockBuilderSplitNode(left,ImGuiDir_Down,.5f,&bottom_left,&left);
                ImGui::DockBuilderSplitNode(right,ImGuiDir_Down,.5f,&bottom_right,&right);
                const ImGuiID nodes[]={left,right,bottom_left,bottom_right};
                for(int i=0;i<4;++i){auto& w=windows[windows.size()-4+i];w.place=w.undock=false;ImGui::DockBuilderDockWindow(w.window_name().c_str(),nodes[i]);}
                ImGui::DockBuilderFinish(dock);demo_layout=false;
            }
            ImGui::DockSpace(dock,{0,0});
        }
        if(!ImGui::IsMouseDown(0)) {
            auto p=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();clamp_to_monitors(p,size);ImGui::SetWindowPos(p);ImGui::SetWindowSize(size);
        }
        ImGui::End();
    }else ImGui::DockSpace(dock,{0,0},ImGuiDockNodeFlags_KeepAliveOnly);
    // Mutations are deferred: no push_back/erase can invalidate the active window.
    std::vector<uint32_t> duplicates;
    for(auto& w:windows) {
        if(w.place){clamp_to_monitors(w.position,w.size);ImGui::SetNextWindowPos(w.position);ImGui::SetNextWindowSize(w.size);w.place=false;}
        if(w.undock){ImGui::SetNextWindowDockID(0,ImGuiCond_Always);w.undock=false;}
        ImGui::SetNextWindowSizeConstraints({360,240},{FLT_MAX,FLT_MAX});
        if(ImGui::Begin(w.window_name().c_str(),&w.open,ImGuiWindowFlags_MenuBar)) {
            if(ImGui::BeginMenuBar()) {
                if(ImGui::BeginMenu("Окно")) {
                    char title[256];std::snprintf(title,sizeof(title),"%s",w.title.c_str());
                    if(ImGui::InputText("Название",title,sizeof(title)))w.title=title;
                    if(ImGui::MenuItem("Дублировать"))duplicates.push_back(w.id);
                    if(ImGui::MenuItem("Закрыть"))w.open=false;
                    ImGui::EndMenu();
                }ImGui::EndMenuBar();
            }
            draw_view(w,s,ImGui::GetContentRegionAvail().y,true);
        }
        if(!ImGui::IsWindowDocked()) {
            w.position=ImGui::GetWindowPos();w.size=ImGui::GetWindowSize();
            auto pos=w.position,size=w.size;clamp_to_monitors(pos,size);
            // Check every frame, including after monitor removal; don't fight active drags.
            if(!ImGui::IsMouseDown(0)&&(pos.x!=w.position.x||pos.y!=w.position.y||size.x!=w.size.x||size.y!=w.size.y)){
                w.position=pos;w.size=size;w.place=true;
            }
        }
        ImGui::End();
    }
    for(auto id:duplicates)duplicate(id);
    windows.erase(std::remove_if(windows.begin(),windows.end(),[](const auto& w){return !w.open;}),windows.end());
}
void PlotWindows::draw_view(PlotWindowState& w,const Simulator& s,float height,bool detached) {
    const auto& c=s.config;const auto& h=s.history;
    const float content_width=ImGui::GetContentRegionAvail().x;
    if(!w.compatible||w.binding!=binding) {
        ImGui::TextWrapped("Привязка несовместима с текущей сетью или протоколом. Индексы не переназначены автоматически.");
        if(ImGui::Button("Привязать к текущей сети")){w.object=0;w.objects={0};rebind(w);}
        return;
    }
    ImGui::Checkbox("Следить за временем",&w.follow);if(content_width>=360)ImGui::SameLine();ImGui::SetNextItemWidth(80);
    const double previous_seconds=w.seconds;
    ImGui::DragScalar("Окно, с",ImGuiDataType_Double,&w.seconds,.05f,nullptr,nullptr,"%.8g");
    w.seconds=std::isfinite(w.seconds)?std::max(w.seconds,.000001):previous_seconds;
    if(!detached)ImGui::SameLine();
    ImGui::Checkbox("Автомасштаб Y",&w.auto_fit);
    if(detached){ImGui::SameLine();if(ImGui::Button("Вписать"))w.fit=true;}
    if(!detached){ImGui::SameLine();if(ImGui::Button("Открыть в отдельном окне"))detach(w);}
    if(h.time.empty()){ImGui::TextWrapped("Нет записанной истории. Проверьте параметры эксперимента.");return;}
    const bool astro=w.kind==PlotKind::Astrocytes||w.kind==PlotKind::AstroPhase;
    const bool syn=w.kind==PlotKind::Synapse;
    if(astro&&c.astrocytes.empty()){ImGui::TextWrapped("В этой сети нет астроцитов.");return;}
    if(w.kind==PlotKind::Voltage||w.kind==PlotKind::Astrocytes)select_objects(w,c,astro);
    else if(syn||astro||w.kind==PlotKind::Neuron) {
        int total=int(syn?c.synapses.size():astro?c.astrocytes.size():c.neurons.size());
        if(total==0){ImGui::TextDisabled("В сети нет объектов этого типа.");return;}
        if(w.object<0||w.object>=total){ImGui::TextWrapped("Выбранный объект отсутствует. Выберите объект явно.");w.object=-1;}
        int n=w.object+1;ImGui::SetNextItemWidth(140);
        if(ImGui::SliderInt(syn?"Синапс":astro?"Астроцит":"Нейрон графика",&n,1,total,syn?"S%d":astro?"A%d":"N%d")){w.object=n-1;w.fit=true;}
        if(w.object<0)return;
    }
    if(syn) {
        if(content_width>=640)ImGui::SameLine();ImGui::SetNextItemWidth(185);const char* views[]={"Ресурсы и ток","Вес w(t)"};
        if(ImGui::Combo("График связи",&w.view,views,2))w.fit=true;
    }
    if(w.kind==PlotKind::Neuron) {
        if(content_width>=640)ImGui::SameLine();ImGui::SetNextItemWidth(185);const char* views[]={"Внешний ток I(t)","Восстановление u(t)","Фаза V / u"};
        if(ImGui::Combo("Представление",&w.view,views,3))w.fit=true;
        if(w.view&&c.neurons[w.object].model!=NeuronModel::Izhikevich){ImGui::TextWrapped("У HH нет переменной восстановления u. Выберите вход или потенциал.");return;}
    }
    if(w.kind==PlotKind::Deliveries) {
        ImGui::Text("Доставлено: %zu · В очереди: %zu",h.deliveries.size(),s.pending_deliveries.size());
        ImGui::TextWrapped("t emit — спайк источника; t arrival — приход по связи. Приход не является новым спайком.");
        if(ImGui::BeginTable("delivery_journal",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,std::max(50.f,ImGui::GetContentRegionAvail().y-4)})) {
            for(const char* label:{"Связь","Источник","Приёмник","t emit, с","t arrival, с"})ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();ImGuiListClipper clip;clip.Begin(int(h.deliveries.size()));
            while(clip.Step())for(int row=clip.DisplayStart;row<clip.DisplayEnd;++row) {
                const auto& e=h.deliveries[row];const auto& edge=c.synapses[e.synapse];
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("S%d",e.synapse+1);
                ImGui::TableNextColumn();ImGui::Text("N%d",edge.source+1);ImGui::TableNextColumn();ImGui::Text("N%d",edge.target+1);
                ImGui::TableNextColumn();ImGui::Text("%.12g",e.t_emit);ImGui::TableNextColumn();ImGui::Text("%.12g",e.t_arrival);
            }ImGui::EndTable();
        }return;
    }
    const bool has_quantities=w.kind==PlotKind::Voltage||w.kind==PlotKind::Astrocytes||w.kind==PlotKind::Raster||(syn&&!w.view);
    if(has_quantities) {
        const float space=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x-ImGui::GetItemRectMax().x;
        if(space>120)ImGui::SameLine();
        if(ImGui::Button("Величины…"))ImGui::OpenPopup("quantities");
    }
    if(ImGui::BeginPopup("quantities")) {
        if(w.kind==PlotKind::Voltage){bit("V, мВ",w.quantities,0);bit("Маркеры спайков",w.quantities,1);}
        else if(w.kind==PlotKind::Astrocytes) {const char* labels[]={"Рецептор Γ","IP3, мкМ","Ca, мкМ","h","Ресурс xA","GA, мкМ","ΓS"};for(int i=0;i<7;++i)bit(labels[i],w.quantities,i);}
        else if(syn&&w.view==0){const char* labels[]={"u","x","s / y","z (TM)","Глутамат Y","Синаптический ток"};for(int i=0;i<6;++i)bit(labels[i],w.quantities,i);}
        else if(w.kind==PlotKind::Raster){bit("Спайки нейронов",w.quantities,0);bit("Выбросы астроцитов",w.quantities,1);}
        else ImGui::TextDisabled("Величина выбрана в представлении графика.");
        ImGui::EndPopup();
    }
    if(w.kind==PlotKind::Neuron&&w.view==0)ImGui::TextDisabled("Вход по сохранённому протоколу расчёта, в моменты записи.");
    bool missing=false;
    if(w.kind==PlotKind::Voltage)for(int i:w.objects)if(!recorded(h,i*4))missing=true;
    if(w.kind==PlotKind::Astrocytes)for(int i:w.objects)if(i<0||i>=int(c.astrocytes.size()))missing=true;
    if(w.kind==PlotKind::Neuron)missing=!recorded(h,w.object*4)||(w.view>0&&!recorded(h,w.object*4+1));
    if(syn)missing=w.object>=int(h.weights.size())||h.weights[w.object].size()!=h.time.size()||!recorded(h,syn_offset(c,w.object)+2);
    if(missing) {ImGui::TextWrapped("Выбранная история не записывалась. Включите запись объекта и повторите расчёт.");if(w.kind!=PlotKind::Voltage)return;}
    bool current_available=!syn||c.synapses[w.object].model!=SynapseModel::ConductanceSTP||recorded(h,c.synapses[w.object].target*4);
    if(syn&&!w.view&&!current_available)ImGui::TextWrapped("Ток недоступен: потенциал приёмника не записывался. Ресурсы доступны.");
    const bool phase=w.kind==PlotKind::AstroPhase||(w.kind==PlotKind::Neuron&&w.view==2);
    const char* y=w.kind==PlotKind::Voltage?"V, мВ":w.kind==PlotKind::Astrocytes?"мкМ / доля":w.kind==PlotKind::Raster?"Номер узла":syn?(w.view?"Вес w":"Ресурсы / Y"):phase?(astro?"Ca, мкМ":"u"):w.view?"u":c.neurons[w.object].model==NeuronModel::Izhikevich?"I, ед. модели":"I, мкА/см²";
    ImGui::PushID(w.view);if(w.fit)ImPlot::SetNextAxesToFit();
    if(ImPlot::BeginPlot("##plot",{-1,std::max(65.f,std::min(height,ImGui::GetContentRegionAvail().y-4))})) {
        ImPlot::SetupLegend(ImPlotLocation_NorthWest,ImPlotLegendFlags_Horizontal);
        ImPlot::SetupAxes(phase?(astro?"IP3, мкМ":"V, мВ"):"Время, с",y,
            phase&&w.auto_fit?ImPlotAxisFlags_AutoFit:0,w.auto_fit?ImPlotAxisFlags_AutoFit:0);
        if(syn&&!w.view)ImPlot::SetupAxis(ImAxis_Y2,"I syn",ImPlotAxisFlags_Opposite|(w.auto_fit?ImPlotAxisFlags_AutoFit:0));
        if(w.restore_limits&&w.limits_valid&&!w.fit) {
            ImPlot::SetupAxisLimits(ImAxis_X1,w.limits[0],w.limits[1],ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1,w.limits[2],w.limits[3],ImPlotCond_Always);
            if(syn&&!w.view)ImPlot::SetupAxisLimits(ImAxis_Y2,w.limits[4],w.limits[5],ImPlotCond_Always);
        }
        if(w.follow&&!phase)ImPlot::SetupAxisLimits(ImAxis_X1,std::max(0.,s.time-w.seconds),std::max(w.seconds,s.time),ImPlotCond_Always);
        const int count=int(h.time.size());
        auto line=[&](const std::string& label,int col,int palette) {if(recorded(h,col)){ImPlot::SetNextLineStyle(color(palette));ImPlot::PlotLine(label.c_str(),h.time.data(),h.values[col].data(),count);}};
        const double start=w.follow?std::max(0.,s.time-w.seconds):0;
        int first=int(std::lower_bound(h.events.begin(),h.events.end(),start,[](const auto& e,double t){return e.time<t;})-h.events.begin());
        if(w.kind==PlotKind::Voltage)for(int i:w.objects)if(i>=0&&i<int(c.neurons.size())&&recorded(h,i*4)) {
            auto label="N"+std::to_string(i+1)+" "+c.neurons[i].name;
            if(w.quantities&1)line(label,i*4,i);
            if(w.quantities&2){Events e{&s,first,i,false,true};ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,2.5f,color(i),1,color(i));ImPlot::PlotScatterG((label+" спайки").c_str(),event_point,&e,int(h.events.size())-first);}
        }
        if(w.kind==PlotKind::Astrocytes)for(int i:w.objects)if(i>=0&&i<int(c.astrocytes.size())) {
            const char* names[]={"Γ","IP3","Ca","h","xA","GA","ΓS"};for(int k=0;k<7;++k)if(w.quantities&(1u<<k))line("A"+std::to_string(i+1)+" "+names[k],astro_offset(c,i)+k,i+k);
        }
        if(w.kind==PlotKind::Raster)for(int a=0;a<2;++a)if(w.quantities&(1u<<a)) {
            Events e{&s,first,-1,a!=0,false};ImPlot::SetNextMarkerStyle(a?ImPlotMarker_Diamond:ImPlotMarker_Cross,3,color(a),1,color(a));
            ImPlot::PlotScatterG(a?"Астроциты (после нейронов)":"Нейроны",event_point,&e,int(h.events.size())-first);
        }
        if(syn) {
            const auto& edge=c.synapses[w.object];int b=syn_offset(c,w.object);
            if(w.view){ImPlot::SetNextLineStyle(teal);ImPlot::PlotLine("Текущий вес",h.time.data(),h.weights[w.object].data(),count);
                double t[]={h.time.front(),std::max(.1,h.time.back())},v[]={edge.weight,edge.weight};ImPlot::PlotLine("Начальный вес",t,v,2);
                if(edge.stdp){double lo[]={edge.weight_min,edge.weight_min},hi[]={edge.weight_max,edge.weight_max};ImPlot::PlotLine("Минимум",t,lo,2);ImPlot::PlotLine("Максимум",t,hi,2);}
            }else {
                if(edge.model!=SynapseModel::ExponentialCurrent){if(w.quantities&1)line("u: использование",b,2);if(w.quantities&2)line("x: ресурс",b+1,0);}
                if(w.quantities&4)line(edge.model==SynapseModel::TsodyksMarkram?"y: активный ресурс":"s: активация",b+2,1);
                if((w.quantities&8)&&edge.model==SynapseModel::TsodyksMarkram){Derived d{&s,w.object,1};ImPlot::PlotLineG("z: неактивный ресурс",derived,&d,count);}
                if(w.quantities&16)line("Глутамат Y",b+3,4);
                if((w.quantities&32)&&current_available){Derived d{&s,w.object,2};ImPlot::SetAxes(ImAxis_X1,ImAxis_Y2);ImPlot::SetNextLineStyle(rose);ImPlot::PlotLineG("Синаптический ток",derived,&d,count);}
            }
        }
        if(phase) {int x=astro?astro_offset(c,w.object)+1:w.object*4;int yy=x+1;
            if(recorded(h,x)&&recorded(h,yy))ImPlot::PlotLine("Траектория",h.values[x].data(),h.values[yy].data(),count);
        }else if(w.kind==PlotKind::Neuron) {
            if(w.view==0){Derived d{&s,w.object,0};ImPlot::SetNextLineStyle(gold);ImPlot::PlotLineG("DC + импульс + шум",derived,&d,count);}
            else line("Переменная восстановления",w.object*4+1,0);
        }
        auto limits=ImPlot::GetPlotLimits(ImAxis_X1,ImAxis_Y1);w.limits[0]=limits.X.Min;w.limits[1]=limits.X.Max;w.limits[2]=limits.Y.Min;w.limits[3]=limits.Y.Max;
        if(syn&&!w.view){auto secondary=ImPlot::GetPlotLimits(ImAxis_X1,ImAxis_Y2);w.limits[4]=secondary.Y.Min;w.limits[5]=secondary.Y.Max;}
        w.limits_valid=true;w.fit=w.restore_limits=false;ImPlot::EndPlot();
    }ImGui::PopID();
}
}
