#include "plot_windows.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <set>
#include <sstream>
#include <iomanip>
#include <limits>

namespace studio {
namespace {
uint64_t hash(const std::string& s) {uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
struct Writer {
    std::string bytes;
    void u64(uint64_t v){for(int i=0;i<8;++i)bytes.push_back(char(v>>(i*8)));}
    void real(double v){uint64_t n;static_assert(sizeof(n)==sizeof(v));std::memcpy(&n,&v,8);u64(n);}
    void str(const std::string& v){u64(v.size());bytes+=v;}
};
struct Reader {
    const std::string& bytes;size_t cursor=0;
    uint64_t u64(){if(bytes.size()-cursor<8)throw std::runtime_error("Усечённое рабочее пространство.");uint64_t n=0;for(int i=0;i<8;++i)n|=uint64_t(static_cast<unsigned char>(bytes[cursor++]))<<(8*i);return n;}
    size_t bounded(size_t max){auto n=u64();if(n>max)throw std::runtime_error("Недопустимый размер/индекс рабочего пространства.");return size_t(n);}
    bool flag(){return bounded(1)!=0;}
    double real(double lo=-1e12,double hi=1e12){uint64_t n=u64();double v;std::memcpy(&v,&n,8);if(!std::isfinite(v)||v<lo||v>hi)throw std::runtime_error("Недопустимое число рабочего пространства.");return v;}
    std::string str(size_t max){size_t n=bounded(max);if(n>bytes.size()-cursor)throw std::runtime_error("Усечённая строка рабочего пространства.");auto s=bytes.substr(cursor,n);cursor+=n;if(s.find('\0')!=s.npos)throw std::runtime_error("Недопустимая строка рабочего пространства.");return s;}
};
}
std::string PlotWindows::config_binding(const Config& c) {
    // Deliberately conservative: indices alone cannot establish object identity.
    // Ignore only screen layout and the extendable end time, not scientific inputs.
    Config normalized=c;normalized.duration=1;
    normalized.stages.clear();normalized.analysis=AnalysisSettings{};normalized.generation.clear();
    // A short, fully recorded large graph may fit its actual history budget
    // but not a fictitious one-second run. Keep old bindings when possible;
    // use the minimum legal duration only for previously unopenable graphs.
    if(!validate(normalized).empty())normalized.duration=.001;
    for(auto& n:normalized.neurons)n.px=n.py=0;
    for(auto& a:normalized.astrocytes)a.px=a.py=0;
    Writer w;w.str(legacy_binding_config(normalized));for(double p:c.parameters)w.real(p);
    std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash(w.bytes);return out.str();
}
bool PlotWindows::save(const std::filesystem::path& path) {
    if(path.empty())return false;
    try {
        if(!valid_topology_view(topology))throw std::runtime_error("Недопустимые настройки топологии.");
        Writer w;w.str("Network Studio plot workspace");w.u64(5);w.u64(next_id);w.u64(host_open);w.u64(windows.size());
        for(const auto& p:windows) {
            if(p.title.size()>255||!std::isfinite(p.seconds)||p.seconds<.000001)throw std::runtime_error("Недопустимые настройки окна; рабочее пространство не заменено.");
            for(int k=0;k<6;++k)if(!std::isfinite(p.limits[k])||std::abs(p.limits[k])>1e12)throw std::runtime_error("Диапазон оси слишком велик для рабочего пространства.");
            for(int k=0;k<6;k+=2)if(p.limits[k]>=p.limits[k+1])throw std::runtime_error("Недопустимый диапазон оси.");
            w.u64(p.id);w.str(p.title);w.str(p.binding);w.u64(p.compatible);w.u64(int(p.kind));w.u64(p.follow);w.u64(p.auto_fit);
            w.real(p.seconds);w.u64(p.object+1);w.u64(p.view);w.u64(p.quantities);w.u64(p.objects.size());for(int n:p.objects)w.u64(n);
            w.u64(p.limits_valid);for(double v:p.limits)w.real(v);
            w.real(p.position.x);w.real(p.position.y);w.real(p.size.x);w.real(p.size.y);
        }
        size_t n=0;const char* ini=ImGui::SaveIniSettingsToMemory(&n);
        if(n>1024*1024)throw std::runtime_error("Расположение окон превышает лимит 1 МиБ.");w.str(std::string(ini,n));
        w.u64(topology.field);w.u64(topology.edges);w.u64(topology.grid);w.u64(topology.all_edges);w.u64(topology.current);w.u64(topology.auto_scale);
        w.u64(topology.columns);w.u64(topology.rows);w.real(topology.scale);w.real(topology.zoom);w.real(topology.pan_x);w.real(topology.pan_y);
        w.u64(int(topology.polarity));w.u64(int(topology.model));
        w.str(analysis_settings_json(embedded[7].analysis));
        w.u64(windows.size());for(const auto& p:windows)w.str(analysis_settings_json(p.analysis));
        w.u64(topology.activity);w.u64(topology.activity_mode);w.real(topology.activity_window);w.real(topology.response_min);w.real(topology.response_max);w.real(topology.activity_scale);w.u64(int(topology.activity_polarity));w.u64(int(topology.activity_model));
        if(analysis_profiles.size()>64)throw std::runtime_error("Too many analysis profiles");
        w.u64(analysis_profiles.size());std::set<std::string> names;
        for(const auto& p:analysis_profiles){if(p.name.empty()||!names.insert(p.name).second)throw std::runtime_error("Empty/duplicate profile name");w.str(analysis_profile_json(p));}
        auto selection=[&](const PlotWindowState& p){w.str(analysis_interval_json(p.analysis_interval));w.str(p.analysis_profile);};
        selection(embedded[7]);for(const auto& p:windows)selection(p);
        w.u64(hash(w.bytes));if(w.bytes.size()>MaxWorkspaceBytes)throw std::runtime_error("Рабочее пространство превышает 4 МиБ.");
        write_atomic(path,w.bytes);workspace_path=path;automatic_save_allowed=true;message="Рабочее пространство графиков сохранено.";return true;
    }catch(const std::exception& e){message=e.what();return false;}
}
bool PlotWindows::load(const std::filesystem::path& path) {
    if(path.empty())return false;
    try {
        auto size=std::filesystem::file_size(path);if(size<32||size>MaxWorkspaceBytes)throw std::runtime_error("Недопустимый размер рабочего пространства (лимит 4 МиБ).");
        std::ifstream in(path,std::ios::binary);std::string bytes(size,'\0');if(!in.read(bytes.data(),std::streamsize(size)))throw std::runtime_error("Не удалось прочитать рабочее пространство.");
        Reader checksum{bytes,bytes.size()-8};if(checksum.u64()!=hash(bytes.substr(0,bytes.size()-8)))throw std::runtime_error("Повреждённое рабочее пространство: контрольная сумма.");
        bytes.resize(bytes.size()-8);Reader r{bytes};
        if(r.str(64)!="Network Studio plot workspace")throw std::runtime_error("Несовместимый тип рабочего пространства.");
        const auto version=r.u64();if(version!=1&&version!=2&&version!=3&&version!=4&&version!=5)throw std::runtime_error("Несовместимая версия рабочего пространства.");
        auto next=uint32_t(r.bounded(UINT32_MAX-1));bool host=r.flag();size_t count=r.bounded(MaxWindows);
        std::vector<PlotWindowState> parsed;std::set<uint32_t> ids;bool mismatch=false;
        for(size_t i=0;i<count;++i) {
            PlotWindowState p;p.id=uint32_t(r.bounded(UINT32_MAX-2));if(!p.id||p.id>=next||!ids.insert(p.id).second)throw std::runtime_error("Повторный или недопустимый ID окна.");
            p.title=r.str(255);p.binding=r.str(16);p.compatible=r.flag();p.kind=PlotKind(r.bounded(version>=3?7:6));p.follow=r.flag();p.auto_fit=r.flag();
            p.seconds=r.real(.000001,std::numeric_limits<double>::max());p.object=int(r.bounded(MaxSynapses))-1;p.view=int(r.bounded(p.kind==PlotKind::Network?4:2));p.quantities=uint32_t(r.bounded(127));
            if(p.kind==PlotKind::Synapse&&p.view>1)throw std::runtime_error("Недопустимое представление синапса.");
            auto objects=r.bounded(32);std::set<int> selected;for(size_t k=0;k<objects;++k){int n=int(r.bounded(MaxNeurons-1));if(!selected.insert(n).second)throw std::runtime_error("Повторный объект графика.");p.objects.push_back(n);}
            p.limits_valid=r.flag();for(auto& v:p.limits)v=r.real();
            for(int k=0;k<6;k+=2)if(p.limits[k]>=p.limits[k+1])throw std::runtime_error("Некорректный диапазон оси.");
            p.position.x=float(r.real(-1e7,1e7));p.position.y=float(r.real(-1e7,1e7));p.size.x=float(r.real(1,16384));p.size.y=float(r.real(1,16384));
            p.compatible=p.compatible&&p.binding==binding;mismatch|=!p.compatible;p.restore_limits=p.limits_valid;p.fit=false;p.place=true;parsed.push_back(std::move(p));
        }
        auto ini=r.str(1024*1024);TopologyViewState view;
        if(version>=2) {
            view.field=r.flag();view.edges=r.flag();view.grid=r.flag();view.all_edges=r.flag();view.current=r.flag();view.auto_scale=r.flag();
            view.columns=int(r.bounded(64));view.rows=int(r.bounded(64));view.scale=r.real(1e-6,100);view.zoom=r.real(.25,8);
            view.pan_x=r.real(-8,8);view.pan_y=r.real(-8,8);view.polarity=FieldPolarity(r.bounded(2));view.model=FieldModel(r.bounded(3));
            if(!valid_topology_view(view))throw std::runtime_error("Недопустимые настройки топологии.");
        }
        AnalysisSettings analysis;
        if(version>=3) {
            analysis=analysis_settings_from_json(r.str(65536));
            if(r.bounded(MaxWindows)!=parsed.size())throw std::runtime_error("Analysis window count mismatch");
            for(auto& p:parsed)p.analysis=analysis_settings_from_json(r.str(65536));
            Config probe;probe.neurons.resize(MaxNeurons);probe.analysis=analysis;probe.analysis.reference_stimulator=0;if(!validate_experiment(probe).empty())throw std::runtime_error("Invalid analysis settings");
            for(auto& p:parsed){probe.analysis=p.analysis;probe.analysis.reference_stimulator=0;if(!validate_experiment(probe).empty())throw std::runtime_error("Invalid window analysis settings");}
        }
        if(version>=4){view.activity=r.flag();view.activity_mode=int(r.bounded(1));view.activity_window=r.real(1e-6,10);view.response_min=r.real(0,10);view.response_max=r.real(0,10);view.activity_scale=r.real(1e-6,100);view.activity_polarity=FieldPolarity(r.bounded(2));view.activity_model=FieldModel(r.bounded(3));if(!valid_topology_view(view))throw std::runtime_error("Invalid activity field settings");}
        std::vector<AnalysisProfile> profiles;AnalysisInterval interval;std::string profile;
        if(version>=5) {
            size_t count=r.bounded(64);std::set<std::string> names;
            for(size_t i=0;i<count;++i){auto p=analysis_profile_from_json(r.str(65536));if(p.name.empty()||!names.insert(p.name).second)throw std::runtime_error("Empty/duplicate profile name");profiles.push_back(std::move(p));}
            interval=analysis_interval_from_json(r.str(1024));profile=r.str(128);
            for(auto& p:parsed){p.analysis_interval=analysis_interval_from_json(r.str(1024));p.analysis_profile=r.str(128);}
        }
        if(r.cursor!=bytes.size()||!next)throw std::runtime_error("Лишние данные рабочего пространства.");
        // Commit only after validating the complete document, including ini checksum.
        windows=std::move(parsed);next_id=next;host_open=host;pending_ini=std::move(ini);workspace_path=path;
        topology=view;analysis_profiles=std::move(profiles);embedded[7].analysis_interval=interval;embedded[7].analysis_profile=profile;embedded[7].analysis=analysis;embedded[7].analysis_at=-1;
        automatic_save_allowed=true;
        message=mismatch?"Рабочее пространство открыто; есть несовместимые привязки. Проверьте окна и перепривяжите их явно.":"Рабочее пространство графиков восстановлено.";
        return true;
    }catch(const std::exception& e){message=e.what();return false;}
}
}
