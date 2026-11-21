#include "saved_results.h"
#include "io.h"
#include "vendor/json/json.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <string_view>

namespace studio {
namespace {
constexpr auto MiB=1024u*1024u;
constexpr std::wstring_view Suffixes[]={
    L"_experiment.json",L"_summary.json",L"_trace.csv",L"_weights.csv",
    L"_events.csv",L"_deliveries.csv",L"_pending_deliveries.csv",
    L"_stimuli.csv",L"_shared_stimuli.csv",L"_analysis.json",
    L"_activity.csv",L"_bursts.csv",L"_responses.csv"
};

void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
std::filesystem::path sibling(std::filesystem::path prefix,std::wstring_view suffix) {
    prefix+=suffix;return prefix;
}
std::filesystem::path export_prefix(const std::filesystem::path& selected) {
    const auto name=selected.filename().wstring();
    for(auto suffix:Suffixes)if(name.size()>suffix.size()&&name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0)
        return selected.parent_path()/name.substr(0,name.size()-suffix.size());
    throw std::runtime_error("Выберите файл комплекта Network Studio, например *_summary.json или *_events.csv.");
}
template<class T> T numeric(std::string_view text) {
    T value{};
    require(!text.empty(),"пустое числовое поле");
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    require(result.ec==std::errc{}&&result.ptr==text.data()+text.size(),"некорректное число");
    return value;
}
double real(std::string_view text) {
    const double value=numeric<double>(text);
    require(std::isfinite(value),"неконечное число");
    return value;
}
int object_index(std::string_view text,size_t size) {
    const auto i=numeric<size_t>(text);
    require(i>=1&&i<=size,"номер объекта вне границ сети");
    return int(i-1);
}

class Fields {
public:
    explicit Fields(std::string_view line):rest(line){}
    bool more() const {return !finished;}
    std::string_view next() {
        require(more(),"не хватает столбцов");
        const auto end=rest.find(',');
        if(end==rest.npos){finished=true;return rest;}
        auto cell=rest.substr(0,end);rest.remove_prefix(end+1);return cell;
    }
    void end() const {require(!more(),"лишние столбцы");}
private:
    std::string_view rest;
    bool finished=false;
};

template<class Header,class Row>
bool csv(const std::filesystem::path& path,size_t file_limit,size_t line_limit,size_t row_limit,Header header,Row row) {
    if(!std::filesystem::exists(path))return false;
    size_t line_number=0;
    try {
        require(std::filesystem::is_regular_file(path),"ожидался обычный файл");
        require(std::filesystem::file_size(path)<=file_limit,"файл превышает лимит просмотра");
        std::ifstream input(path,std::ios::binary);
        require(bool(input),"не удалось открыть файл");
        std::vector<char> line(line_limit+1);
        while(input.getline(line.data(),std::streamsize(line.size()))) {
            size_t length=size_t(input.gcount())-(input.eof()?0:1);
            std::string_view text(line.data(),length);
            if(!text.empty()&&text.back()=='\r')text.remove_suffix(1);
            if(++line_number==1) {
                if(text.substr(0,3)=="\xef\xbb\xbf")text.remove_prefix(3);
                header(text);
            } else {
                require(line_number-1<=row_limit,"слишком много записей");
                Fields fields(text);row(fields,line_number-2);fields.end();
            }
        }
        require(input.eof()&&!input.bad()&&input.gcount()==0,"повреждённая или слишком длинная строка");
        require(line_number!=0,"отсутствует заголовок CSV");
    } catch(const std::exception& e) {
        throw std::runtime_error(path.filename().u8string()+", строка "+std::to_string(line_number?line_number:1)+": "+e.what());
    }
    return true;
}
auto exact_header(std::string_view expected) {
    return [=](std::string_view header){require(header==expected,"неизвестный заголовок CSV");};
}
void check_time(double value,double first,double last) {
    require(value>=first&&value<=last,"время вне границ журнала");
}
int state_column(std::string_view field,const Config& c) {
    const auto sep=field.find('_');
    require(sep!=field.npos&&sep>=2,"неизвестный столбец состояний");
    const auto id=field.substr(1,sep-1),suffix=field.substr(sep+1);
    switch(field.front()) {
    case 'N': {
        const int i=object_index(id,c.neurons.size());
        const auto kind=c.neurons[i].model;
        const std::string_view names[]={ "V_mV", kind==NeuronModel::Izhikevich?"u":"m",
            kind==NeuronModel::Izhikevich?"reserved_1":"h",kind==NeuronModel::Izhikevich?"reserved_2":"n" };
        for(int j=0;j<4;++j)if(suffix==names[j])return i*4+j;
        break;
    }
    case 'S': {
        const int i=object_index(id,c.synapses.size());
        const std::string_view names[]={ "u","x",c.synapses[i].model==SynapseModel::TsodyksMarkram?"y":"s","Y_uM" };
        for(int j=0;j<4;++j)if(suffix==names[j])return syn_offset(c,i)+j;
        break;
    }
    case 'A': {
        const int i=object_index(id,c.astrocytes.size());
        const std::string_view names[]={"GammaA","IP3_uM","Ca_uM","h","xA","GA_uM","GammaS"};
        for(int j=0;j<7;++j)if(suffix==names[j])return astro_offset(c,i)+j;
        break;
    }
    }
    throw std::runtime_error("неизвестный столбец состояний");
}
int weight_column(std::string_view field,const Config& c) {
    const auto sep=field.find('_');
    require(sep!=field.npos&&sep>=2&&field.front()=='S'&&field.substr(sep)=="_weight","неизвестный столбец весов");
    return object_index(field.substr(1,sep-1),c.synapses.size());
}
bool load_samples(const std::filesystem::path& path,SimulationView& view,double first,bool weights,bool check_grid,size_t& stored) {
    auto& h=view.history;
    auto& table=weights?h.weights:h.values;
    std::vector<int> columns;
    const bool present=csv(path,1024u*MiB,8*MiB,MaxSamples,[&](std::string_view line){
        Fields headers(line);require(headers.next()=="time_s","отсутствует столбец time_s");
        std::vector<bool> seen(table.size());
        while(headers.more()) {
            auto cell=headers.next();
            const int column=weights?weight_column(cell,view.config):state_column(cell,view.config);
            require(!seen[column],"повтор столбца");seen[column]=true;columns.push_back(column);
        }
    },[&](Fields& fields,size_t row) {
        const double time=real(fields.next());check_time(time,first,view.time);
        if(check_grid) require(row<h.time.size()&&time==h.time[row],"сетка времени не совпадает с состояниями");
        else {
            require(row?time>h.time.back():time==first,"неверная или неупорядоченная сетка времени");
            h.time.push_back(time);
        }
        require(columns.size()<=MaxStoredValues-stored,"превышен лимит записанных значений");
        for(int column:columns)table[column].push_back(real(fields.next()));
        stored+=columns.size();
    });
    if(present){
        require(!h.time.empty(),"в сохранённой истории нет отсчётов");
        for(int column:columns)require(table[column].size()==h.time.size(),"неполная история отсчётов");
    }
    return present;
}

bool load_deliveries(const std::filesystem::path& path,SimulationView& view,bool pending) {
    auto& h=view.history;
    return csv(path,256u*MiB,512,MaxEvents,exact_header("synapse_index_1based,source_index_1based,target_index_1based,t_emit_s,t_arrival_s"),
        [&](Fields& row,size_t) {
            const int i=object_index(row.next(),view.config.synapses.size());
            const auto& edge=view.config.synapses[i];
            require(object_index(row.next(),view.config.neurons.size())==edge.source&&object_index(row.next(),view.config.neurons.size())==edge.target,"связь не соответствует топологии");
            const double emit=real(row.next()),arrival=real(row.next());
            check_time(emit,0,view.time);
            require(arrival>=emit,"доставка раньше спайка");
            if(pending) {
                require(arrival>=view.time,"прошедшая доставка в очереди");
                ++view.pending_deliveries.count;
            } else {
                check_time(arrival,h.deliveries_after,view.time);
                require(h.deliveries.empty()||arrival>=h.deliveries.back().t_arrival,"доставки не упорядочены");
                h.deliveries.push_back({emit,arrival,i});
            }
        });
}

void load_events_and_stimuli(const std::filesystem::path& prefix,SimulationView& view) {
    auto& h=view.history;
    const auto& c=view.config;
    auto& channels=view.plot_channels;
    channels.events=csv(sibling(prefix,L"_events.csv"),256u*MiB,256,MaxEvents,exact_header("time_s,type,node_index_1based"),
        [&](Fields& row,size_t) {
            const double time=real(row.next());check_time(time,h.events_after,view.time);
            require(h.events.empty()||time>=h.events.back().time,"события не упорядочены");
            const auto type=row.next();require(type=="spike"||type=="astro_release","неизвестный тип события");
            const bool astro=type=="astro_release";
            h.events.push_back({time,object_index(row.next(),astro?c.astrocytes.size():c.neurons.size()),astro});
        });
    channels.stimuli=csv(sibling(prefix,L"_stimuli.csv"),256u*MiB,512,MaxEvents,exact_header("time_s,neuron_1based,protocol_0based,amplitude"),
        [&](Fields& row,size_t) {
            const double time=real(row.next());check_time(time,h.stimuli_after,view.time);
            require(h.stimuli.empty()||time>=h.stimuli.back().time,"стимулы не упорядочены");
            const int neuron=object_index(row.next(),c.neurons.size());
            const int protocol=numeric<int>(row.next());require(protocol>=0,"недопустимый номер протокола");
            h.stimuli.push_back({time,real(row.next()),neuron,protocol});
        });
    size_t recipients=0;
    channels.shared_stimuli=csv(sibling(prefix,L"_shared_stimuli.csv"),256u*MiB,16*1024,MaxEvents,
        exact_header("stimulator_id,pulse_id,start_s,end_s,amplitude,complete,stage_0based,recipients_1based"),
        [&](Fields& row,size_t) {
            SharedPulse p;
            p.stimulator=numeric<uint64_t>(row.next());p.pulse=numeric<uint64_t>(row.next());
            require(p.stimulator>0&&p.pulse>0,"недопустимый номер стимулятора или импульса");
            p.start=real(row.next());p.end=real(row.next());p.amplitude=real(row.next());
            check_time(p.start,0,view.time);check_time(p.end,p.start,view.time);
            const int complete=numeric<int>(row.next());require(complete==0||complete==1,"неверный флаг завершения импульса");
            p.complete=complete==1;p.stage=numeric<int>(row.next());
            auto nodes=row.next();
            require(nodes.size()>=2&&nodes.front()=='"'&&nodes.back()=='"',"неверный список получателей");
            nodes.remove_prefix(1);nodes.remove_suffix(1);
            while(!nodes.empty()) {
                const auto end=nodes.find(';');
                require(++recipients<=MaxStoredValues&&p.recipients.size()<c.neurons.size(),"слишком много получателей");
                const int i=object_index(nodes.substr(0,end),c.neurons.size());
                require(p.recipients.empty()||i>p.recipients.back(),"повтор или неверный порядок получателей");
                p.recipients.push_back(i);
                if(end==nodes.npos)break;
                nodes.remove_prefix(end+1);require(!nodes.empty(),"отсутствует номер получателя");
            }
            h.shared_pulses.push_back(std::move(p));
        });
    std::stable_sort(h.shared_pulses.begin(),h.shared_pulses.end(),[](const auto& a,const auto& b){return a.start<b.start;});
}
}

SavedResults load_saved_results(const std::filesystem::path& selected_file) {
    SavedResults result;
    result.prefix=export_prefix(selected_file);
    auto& view=result.view;
    auto& h=view.history;
    const auto& prefix=result.prefix;
    view.config=load_config(sibling(prefix,L"_experiment.json"));
    const auto summary_path=sibling(prefix,L"_summary.json");
    require(std::filesystem::is_regular_file(summary_path)&&std::filesystem::file_size(summary_path)<=16u*MiB,"нужен корректный файл *_summary.json");
    std::ifstream input(summary_path,std::ios::binary);
    require(bool(input),"не удалось открыть сводку");
    const std::string contents((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    const auto summary=json::jobject::parse(contents);
    view.time=real(summary.get("time_s"));
    require(view.time>=0&&view.time<=view.config.duration,"время результатов не соответствует конфигурации");
    auto coverage=[&](const char* key) {
        const double t=real(summary.get(key));
        check_time(t,0,view.time);return t;
    };
    h.events_after=coverage("events_available_after_s");
    h.deliveries_after=coverage("deliveries_available_after_s");
    h.stimuli_after=coverage("stimuli_available_after_s");
    h.shared_after=coverage("shared_stimuli_available_after_s");
    const double first=coverage("state_history_from_s");
    h.values.resize(dimension(view.config));h.weights.resize(view.config.synapses.size());
    size_t stored=0;
    auto& channels=view.plot_channels;
    channels.states=load_samples(sibling(prefix,L"_trace.csv"),view,first,false,false,stored);
    channels.weights=load_samples(sibling(prefix,L"_weights.csv"),view,first,true,channels.states,stored);
    load_events_and_stimuli(prefix,view);
    channels.deliveries=load_deliveries(sibling(prefix,L"_deliveries.csv"),view,false);
    load_deliveries(sibling(prefix,L"_pending_deliveries.csv"),view,true);
    // The export contains sampled states and event times, not sampled external current.
    channels.input=false;
    require(channels.states||channels.weights||channels.events||channels.deliveries||channels.stimuli||channels.shared_stimuli,"в комплекте нет файлов данных для графиков");

    // Object selection follows the CSV columns, including partial exports.
    auto& c=view.config;
    c.record_all=false;c.record_neurons.clear();c.record_synapses.clear();
    for(size_t i=0;i<c.neurons.size();++i)if(!h.values[i*4].empty())c.record_neurons.push_back(int(i));
    for(size_t i=0;i<c.synapses.size();++i)if(!h.values[syn_offset(c,int(i))+2].empty()||!h.weights[i].empty())c.record_synapses.push_back(int(i));
    return result;
}
}
