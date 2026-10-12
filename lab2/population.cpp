#include "population.h"
#include "io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace studio::population {
std::string validate_base(const Config& base,int neurons,const std::vector<Edge>& edges) {
    const auto invalid=studio::validate(base);
    if(!invalid.empty())return "Invalid base configuration: "+invalid;
    if(neurons<1 || neurons>2000)return "Population size must be 1..2000.";
    if(base.neurons.size()!=1 || base.neurons[0].model!=NeuronModel::Izhikevich ||
       !base.astrocytes.empty() || !base.gaps.empty() || base.synapses.size()!=1)
        return "Base requires exactly one Izhikevich neuron, one template synapse, and no astrocytes or gaps.";
    const auto& s=base.synapses[0];
    if(s.delay!=0)return "Conduction delays require network_studio_cli; the aggregated population solver supports only delay=0.";
    if(s.source!=0 || s.target!=0 || s.astro!=-1 || !s.enabled || s.stdp || s.weight!=1 ||
       (s.model!=SynapseModel::TsodyksMarkram && s.model!=SynapseModel::ExponentialCurrent))
        return "Template must be an enabled self-synapse: TM or exponential current, weight=1, STDP off, astro=-1.";
    if(edges.size()>2000000)return "At most 2000000 edges are supported.";
    for(const auto& e:edges)
        if(e.source<0 || e.target<0 || e.source>=neurons || e.target>=neurons ||
           !std::isfinite(e.weight) || e.weight< -10 || e.weight>10)
            return "Edge endpoint is outside the population or weight is outside [-10,10].";
    return {};
}
Population::Population(const Config& base,int neurons,const std::vector<Edge>& edges):config(base) {
    error=validate_base(base,neurons,edges); if(!error.empty())return;
    synapse=base.synapses[0];
    config.neurons.assign(neurons,base.neurons[0]);
    config.synapses.clear(); // Template is not an actual population self-connection.
    state.resize(neurons);next.resize(neurons);input.resize(neurons);spikes.resize(neurons);outgoing.resize(neurons);
    fired.reserve(neurons);
    flow=numerical::Flow(7*neurons);suggested_step=base.dt;
    for(int i=0;i<neurons;++i)thresholds.push_back({7*i,base.neurons[0].izh_peak,true});
    for(auto& n:state) {
        n.v=base.neurons[0].voltage;
        n.u=base.neurons[0].equilibrium_u ? base.neurons[0].izh_b*n.v : base.neurons[0].izh_u;
    }
    for(const auto& e:edges)outgoing[e.source].push_back(e);
    const auto& n=base.neurons[0];
    constant_input=n.amplitude==0 || n.frequency==0 || n.width==0;
}
#include "population_integrator.inc"
}

namespace {
using namespace studio;
using namespace studio::population;
void require(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
void assert_close(double actual,double expected,const std::string& what) {
    if(std::abs(actual-expected)>2e-8*(1+std::abs(expected))) {
        std::ostringstream text;text<<std::setprecision(17)<<what<<": "<<actual<<" != "<<expected;
        throw std::runtime_error(text.str());
    }
}
Config test_base(SynapseModel model) {
    auto c=preset(4);c.neurons[0].amplitude=0;c.neurons[0].current=8;
    c.duration=.153013;c.sample=.0007;
    Synapse s;s.source=s.target=0;s.model=model;s.gain=2;s.release=.5;
    s.tau_syn=.01;s.tau_f=1;s.tau_d=.05;c.synapses={s};return c;
}
void self_test() {
    int cases=0;std::uint64_t checked_steps=0,checked_spikes=0;
    for(auto model:{SynapseModel::TsodyksMarkram,SynapseModel::ExponentialCurrent})
    for(double dt:{.00005,.000025,.0000125})for(int mode=0;mode<4;++mode) {
        auto base=test_base(model);base.dt=dt;
        if(mode==1){base.noise_sigma=3;base.neurons[0].current=4;}
        if(mode==2) {
            base.noise_sigma=2;base.neurons[0].individual_noise=true;base.neurons[0].noise_sigma=1.5;
            base.neurons[0].current=0;base.neurons[0].amplitude=30;base.neurons[0].start=.01;
            base.neurons[0].stop=.2;base.neurons[0].frequency=20;base.neurons[0].width=.001;
            base.neurons[0].equilibrium_u=false;base.neurons[0].izh_u=-12;
        }
        if(mode==3){base.noise_sigma=3;base.neurons[0].current=4;base.seed=101;}
        std::vector<Edge> edges;
        for(int i=0;i<5;++i) {
            edges.push_back({i,(i+1)%5,i==4 ? -.3 : .2});
            if(mode)edges.push_back({i,(i+3)%5,(i%2 ? -.17 : .13)});
        }
        if(mode==2){edges.push_back({0,0,.05});edges.push_back({0,1,-.04});edges.push_back({3,2,0});}
        if(mode==3) {
            // Fixed PRNG specifies a reproducible non-ring directed graph.
            // Integer LCG avoids standard-library distribution differences.
            edges.clear();std::uint32_t random=192837;
            auto draw=[&](){random=random*1664525u+1013904223u;return random;};
            for(int source=0;source<5;++source)for(int target=0;target<5;++target)
                if(draw()%3==0) {
                    const double weight=(double(draw()%4001)-2000)/10000;
                    edges.push_back({source,target,weight});
                }
            require(!edges.empty(),"Random fixture has no edges.");
        }
        Population aggregate(base,5,edges);require(aggregate.error.empty(),aggregate.error);
        auto native_config=base;native_config.neurons.assign(5,base.neurons[0]);native_config.synapses.clear();
        for(const auto& edge:edges) {
            auto syn=base.synapses[0];syn.source=edge.source;syn.target=edge.target;syn.weight=edge.weight;
            native_config.synapses.push_back(syn);
        }
        Simulator native(native_config);require(native.error.empty(),native.error);
        auto compare=[&] {
            assert_close(aggregate.time,native.time,"time");
            for(int i=0;i<5;++i) {
                assert_close(aggregate.state[i].v,native.state[i*4],"V");assert_close(aggregate.state[i].u,native.state[i*4+1],"recovery u");
                double sum=0;
                for(int j=0;j<int(edges.size());++j)if(edges[j].target==i)
                    sum+=edges[j].weight*base.synapses[0].gain*native.state[syn_offset(native_config,j)+2];
                assert_close(aggregate.state[i].current,sum,"aggregate current");
            }
            for(int j=0;j<int(edges.size());++j) {
                const auto& src=aggregate.state[edges[j].source];const int offset=syn_offset(native_config,j);
                assert_close(src.tm_u,native.state[offset],"TM u");assert_close(src.tm_x,native.state[offset+1],"TM x");
                assert_close(src.tm_y,native.state[offset+2],"TM y");
                assert_close(edges[j].weight>0 ? src.glutamate : 0,native.state[offset+3],"glutamate");
            }
        };
        compare();size_t previous_events=0,records=1;
        while(!native.done()) {
            require(native.step(),native.error);require(aggregate.step(),aggregate.error);++checked_steps;
            compare(); // Includes every sample and every intermediate step, even the shortened last step.
            require(native.history.events.size()-previous_events==aggregate.fired.size(),"Different spike count within step.");
            for(size_t i=0;i<aggregate.fired.size();++i) {
                const auto& event=native.history.events[previous_events+i];
                require(!event.astro && event.index==aggregate.fired[i] && std::abs(event.time-aggregate.events[i].time)<2e-10,"Spike identity/time mismatch.");
                ++checked_spikes;
            }
            previous_events=native.history.events.size();
            records+=aggregate.samples.size();
            require(records==native.history.time.size(),"Sample cadence mismatch.");
        }
        require(aggregate.done() && aggregate.time==base.duration,"Final partial step missing.");++cases;
    }
    auto base=test_base(SynapseModel::TsodyksMarkram);
    require(!validate_base(base,0,{}).empty(),"Invalid population count accepted.");
    require(!validate_base(base,5,{{5,0,1}}).empty(),"Invalid endpoint accepted.");
    require(!validate_base(base,5,{{0,0,std::numeric_limits<double>::quiet_NaN()}}).empty(),"NaN weight accepted.");
    base.synapses[0].stdp=true;require(!validate_base(base,5,{}).empty(),"STDP accepted.");
    base.synapses[0].stdp=false;base.synapses[0].weight=.5;require(!validate_base(base,5,{}).empty(),"Ignored template weight accepted.");
    std::cout<<"PASS population equivalence: "<<cases<<" cases, "<<checked_steps<<" steps, "<<checked_spikes<<" spikes; strict bounds checked.\n";
}
std::string trim(std::string text) {
    const auto first=text.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};
    return text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);
}
int integer(const std::string& input) {
    auto value=trim(input);require(!value.empty() && value.find_first_not_of("0123456789")==std::string::npos,"Expected a nonnegative decimal integer: "+input);
    size_t used=0;auto number=std::stoull(value,&used);require(used==value.size() && number<=2000000,"Integer argument is too large.");return int(number);
}
double number(const std::string& input) {
    std::istringstream stream(trim(input));stream.imbue(std::locale::classic());double value;stream>>value;
    require(bool(stream) && stream.peek()==std::char_traits<char>::eof() && std::isfinite(value),"Expected a finite number: "+input);return value;
}
std::vector<Edge> read_edges(const std::filesystem::path& path) {
    std::ifstream file(path);require(bool(file),"Cannot read edge file: "+path.u8string());
    std::string line;require(bool(std::getline(file,line)) && trim(line)=="source,target,weight","Edge CSV header must be source,target,weight.");
    std::vector<Edge> edges;size_t row=1;
    while(std::getline(file,line)) {
        ++row;const auto first=line.find(','),last=line.rfind(',');
        require(first!=std::string::npos && first!=last && line.find(',',first+1)==last,"Expected three fields on edge CSV row "+std::to_string(row));
        edges.push_back({integer(line.substr(0,first)),integer(line.substr(first+1,last-first-1)),number(line.substr(last+1))});
        require(edges.size()<=2000000,"At most 2000000 edges are supported.");
    }
    require(file.eof(),"Error while reading edge CSV.");return edges;
}
std::vector<int> parse_record(const std::string& value,int neurons) {
    std::vector<int> result;std::set<int> unique;std::istringstream stream(value);std::string item;
    require(!value.empty() && value.back()!=',',"--record requires comma-separated neuron indices.");
    while(std::getline(stream,item,',')) {
        int index=integer(item);require(index<neurons && unique.insert(index).second,"Recorded index is missing or duplicated.");result.push_back(index);
    }
    require(!result.empty() && result.size()<=16,"Record 1..16 neurons.");return result;
}
std::string quote(const std::string& value) {
    std::string result="\"";
    for(unsigned char ch:value) {
        if(ch=='"' || ch=='\\'){result+='\\';result+=char(ch);}
        else if(ch<32){std::ostringstream s;s<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(ch);result+=s.str();}
        else result+=char(ch);
    }
    return result+'"';
}
std::filesystem::path suffix(const std::filesystem::path& prefix,const char* tail) {auto p=prefix;p+=tail;return p;}
void record(std::ofstream& file,double time,const std::vector<NodeState>& state,const std::vector<int>& selected) {
    file<<time;
    for(int i:selected){const auto& n=state[i];file<<','<<n.v<<','<<n.u<<','<<n.current<<','<<n.tm_u<<','<<n.tm_x<<','<<n.tm_y;}
    file<<'\n';
}
int run_cli(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--self-test"){self_test();return 0;}
        std::filesystem::path base_path,edges_path,output;std::string selected_text;int neurons=0;std::set<std::string> options;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--help") {
                std::cout<<"network_studio_population --base config.json --edges edges.csv --neurons N --output prefix [--record 0,1,2,3]\n"
                    "Homogeneous Izhikevich + TM/exponential population; all neurons integrated, selected trace only.\n"
                    "Base: one Izh neuron + one enabled self-synapse template, weight=1, STDP off, no astrocytes.\n"
                    "Edge header: source,target,weight; indices zero-based, signed weights replace template weight.\n"
                    "No output file may already exist. --self-test compares against the native Simulator.\n";return 0;
            }
            require(options.insert(arg).second,"Duplicate option: "+arg);require(i+1<argc,"Missing option value: "+arg);
            const std::string value=argv[++i];
            if(arg=="--base")base_path=std::filesystem::u8path(value);
            else if(arg=="--edges")edges_path=std::filesystem::u8path(value);
            else if(arg=="--neurons")neurons=integer(value);
            else if(arg=="--output")output=std::filesystem::u8path(value);
            else if(arg=="--record")selected_text=value;
            else throw std::runtime_error("Unknown option: "+arg);
        }
        require(!base_path.empty() && !edges_path.empty() && !output.empty() && neurons>0,"Specify --base, --edges, --neurons and --output; see --help.");
        const auto base=load_config(base_path);const auto edges=read_edges(edges_path);Population population(base,neurons,edges);
        require(population.error.empty(),population.error);
        std::vector<int> selected;
        if(!selected_text.empty())selected=parse_record(selected_text,neurons);
        else for(int i=0;i<std::min(4,neurons);++i)selected.push_back(i);
        for(const auto* tail:{"_events.csv","_trace.csv","_summary.json","_base.json"})
            require(!std::filesystem::exists(suffix(output,tail)),"Output already exists: "+suffix(output,tail).u8string());
        if(!output.parent_path().empty())std::filesystem::create_directories(output.parent_path());
        save_config(base,suffix(output,"_base.json"));
        std::ofstream events(suffix(output,"_events.csv")),trace(suffix(output,"_trace.csv"));
        require(bool(events) && bool(trace),"Cannot create result CSV files.");
        events.imbue(std::locale::classic());trace.imbue(std::locale::classic());events<<std::setprecision(17);trace<<std::setprecision(17);
        events<<"time_s,type,node_index_1based\n";trace<<"time_s";
        for(int i:selected)for(const char* field:{"v_mV","u_model","current_model","tm_u","tm_x","tm_y"})trace<<",N"<<i+1<<'_'<<field;
        trace<<'\n';record(trace,population.time,population.state,selected);std::uint64_t samples=1,total=0;
        const auto begin=std::chrono::steady_clock::now();
        while(!population.done()) {
            if(!population.step())break;
            for(const auto& event:population.events){events<<event.time<<",spike,"<<event.index+1<<'\n';++total;}
            for(const auto& sample:population.samples){record(trace,sample.first,sample.second,selected);++samples;}
            if(!events || !trace){population.error="Result CSV write failed.";break;}
        }
        events.flush();trace.flush();if(!events || !trace)population.error="Result CSV flush failed.";
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::ofstream summary(suffix(output,"_summary.json"));summary.imbue(std::locale::classic());summary<<std::setprecision(17);
        const auto& syn=base.synapses[0];
        summary<<"{\n  \"complete\": "<<(population.error.empty() && population.time==base.duration ? "true" : "false")
            <<",\n  \"error\": "<<quote(population.error)<<",\n  \"neurons\": "<<neurons<<",\n  \"edges\": "<<edges.size()
            <<",\n  \"time_s\": "<<population.time<<",\n  \"duration_s\": "<<base.duration<<",\n  \"dt_s\": "<<base.dt
            <<",\n  \"sample_s\": "<<base.sample<<",\n  \"steps\": "<<population.steps<<",\n  \"samples\": "<<samples
            <<",\n  \"wall_seconds\": "<<elapsed<<",\n  \"seed\": "<<base.seed<<",\n  \"noise_interval_s\": "<<base.noise_interval
            <<",\n  \"model\": \"izhikevich\",\n  \"synapse_model\": "<<quote(syn.model==SynapseModel::TsodyksMarkram ? "tsodyks_markram" : "exponential_current")
            <<",\n  \"integrator\": \"event-localized RK4 step doubling with exact linear kinetics and input boundaries\",\n  \"aggregation\": \"identical per-source pools; linear per-target signed current\""
            <<",\n  \"current_model_column\": \"synaptic current only; external current/noise excluded\""
            <<",\n  \"stdp\": false,\n  \"astrocytes\": 0,\n  \"total_spikes\": "<<total<<",\n  \"neuron_spikes\": [";
        for(size_t i=0;i<population.spikes.size();++i){if(i)summary<<',';summary<<population.spikes[i];}
        summary<<"],\n  \"recorded_neurons_0based\": [";
        for(size_t i=0;i<selected.size();++i){if(i)summary<<',';summary<<selected[i];}
        summary<<"],\n  \"base_path\": "<<quote(std::filesystem::absolute(base_path).u8string())
            <<",\n  \"edges_path\": "<<quote(std::filesystem::absolute(edges_path).u8string())<<"\n}\n";
        summary.flush();require(bool(summary),"Cannot write summary JSON.");require(population.error.empty(),population.error);
        std::cout<<"Completed "<<population.time<<" s / "<<population.steps<<" steps / "<<neurons<<" neurons / "<<edges.size()
                 <<" edges / "<<total<<" spikes in "<<elapsed<<" wall seconds.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    try {
        std::vector<std::string> values;std::vector<char*> arguments;
        for(int i=0;i<argc;++i) {
            const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
            require(size>0,"Invalid Unicode argument.");std::string value(size,'\0');
            require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),size,nullptr,nullptr)>0,"Cannot convert Unicode argument.");
            value.pop_back();values.push_back(value);
        }
        for(auto& value:values)arguments.push_back(value.data());return run_cli(argc,arguments.data());
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#else
int main(int argc,char** argv) {return run_cli(argc,argv);}
#endif
