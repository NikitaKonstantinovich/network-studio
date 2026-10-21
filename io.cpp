#include "io.h"
#include "generator.h"
#include "analysis.h"
#include "animat.h"
#include "vendor/json/json.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <locale>
#include <chrono>
#include <system_error>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace studio {
using Object = json::jobject;
static constexpr size_t MaxConfigBytes=64*1024*1024;
static void put(Object& o,const char* k,double v) {
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(17)<<v; o.set(k,out.str());
}
static void put(Object& o,const char* k,int v) { put(o,k,double(v)); }
static void put(Object& o,const char* k,bool v) { o.set(k,v?"true":"false"); }
static void put(Object& o,const char* k,const std::string& v) { o.set(k,json::parsing::encode_string(v.c_str())); }
static double number(const Object& o,const char* k) {
    if(!o[k].is_number()) throw std::runtime_error(std::string("Expected a number: ")+k);
    std::string text=o.get(k); std::istringstream in(text); in.imbue(std::locale::classic());
    double v=0; in>>v; if(!in || !std::isfinite(v) || (in>>std::ws,!in.eof())) throw std::runtime_error(std::string("Invalid number: ")+k);
    return v;
}
static int integer(const Object& o,const char* k) {
    double v=number(o,k); if(v!=std::floor(v)||v< -1||v>100000) throw std::runtime_error(std::string("Expected an integer: ")+k);
    return int(v);
}
static uint32_t seed_value(const Object& o) {
    double v=number(o,"seed");
    if(v!=std::floor(v)||v<0||v>4294967295.0)throw std::runtime_error("Seed must be a 32-bit unsigned integer.");
    return static_cast<uint32_t>(v);
}
static bool boolean(const Object& o,const char* k) {
    if(!o[k].is_bool()) throw std::runtime_error(std::string("Expected a boolean: ")+k); return o[k].is_true();
}
static std::string string(const Object& o,const char* k) {
    if(!o[k].is_string()) throw std::runtime_error(std::string("Expected a string: ")+k); return o[k].as_string();
}
static Object object(const Object& o,const char* k,bool array) {
    Object result=Object::parse(o.get(k));
    if(result.is_array()!=array) throw std::runtime_error(std::string("Wrong object/array type: ")+k); return result;
}
static Object element(const Object& array,size_t i) {
    Object o=Object::parse(array.get(i)); if(o.is_array()) throw std::runtime_error("Expected object in array."); return o;
}
static Object index_array(const std::vector<int>& indices) {
    Object array(true);for(int index:indices)array+=json::kvp("",std::to_string(index));return array;
}
static std::vector<int> read_indices(const Object& root,const char* key,size_t limit) {
    const auto array=object(root,key,true);
    if(array.size()>limit)throw std::runtime_error(std::string("Too many recording indices: ")+key);
    std::vector<int> indices;indices.reserve(array.size());
    for(size_t i=0;i<array.size();++i) {
        Object value;value.set("index",array.get(i));indices.push_back(integer(value,"index"));
    }
    return indices;
}
static NeuronModel neuron_model(const Object& o) {
    const auto model=string(o,"model");
    if(model=="hodgkin_huxley")return NeuronModel::HodgkinHuxley;
    if(model=="izhikevich")return NeuronModel::Izhikevich;
    throw std::runtime_error("Unknown neuron model: "+model);
}
static SynapseModel synapse_model(const Object& o) {
    const auto model=string(o,"model");
    if(model=="conductance_stp")return SynapseModel::ConductanceSTP;
    if(model=="exponential_current")return SynapseModel::ExponentialCurrent;
    if(model=="tsodyks_markram")return SynapseModel::TsodyksMarkram;
    throw std::runtime_error("Unknown synapse model: "+model);
}
static StdpRule stdp_rule(const Object& o) {
    const auto rule=string(o,"stdp_rule");
    if(rule=="additive")return StdpRule::Additive;
    if(rule=="multiplicative")return StdpRule::Multiplicative;
    if(rule=="triplet")return StdpRule::Triplet;
    throw std::runtime_error("Unknown STDP rule: "+rule);
}
#define WRITE(field) put(o,#field,v.field)
#define READ(field) v.field=number(o,#field)
#define READINT(field) v.field=integer(o,#field)
#include "stimulator_json.inc"
#include "animat_json.inc"
#include "experiment_json.inc"
#include "analysis_json.inc"
std::string serialize(const Config& c) {
    const auto err=validate(c); if(!err.empty())throw std::runtime_error(err);
    Object root; put(root,"schema",9);put(root,"history_capacity",double(c.history_capacity)); put(root,"name",c.name); put(root,"dt",c.dt);
    put(root,"duration",c.duration); put(root,"sample",c.sample);
    put(root,"noise_sigma",c.noise_sigma);put(root,"noise_interval",c.noise_interval);put(root,"seed",double(c.seed));
    put(root,"record_all",c.record_all);root.set("record_neurons",index_array(c.record_neurons).as_string());
    root.set("record_synapses",index_array(c.record_synapses).as_string());
    write_experiment_config(root,c);root.set("animat",animat_config_object(c.animat).as_string());
    root.set("stimulators",stimulator_array(c.stimulators).as_string());uid(root,"next_stimulator_id",c.next_stimulator_id);put(root,"activity_horizon",c.activity_horizon);
    Object parameters;
    for(int i:SharedParameters)put(parameters,na::parameter_names[i],c.parameters[i]);
    root.set("parameters",parameters.as_string());
    Object neurons(true),astrocytes(true),synapses(true),gaps(true);
    for(const auto& v:c.neurons) {
        Object o; WRITE(name); WRITE(px); WRITE(py); WRITE(voltage); WRITE(current); WRITE(amplitude);
        WRITE(frequency); WRITE(start); WRITE(stop); WRITE(width);
        put(o,"model",std::string(v.model==NeuronModel::Izhikevich?"izhikevich":"hodgkin_huxley"));
        WRITE(izh_a); WRITE(izh_b); WRITE(izh_c); WRITE(izh_d); WRITE(izh_peak); WRITE(izh_u);
        WRITE(equilibrium_u); WRITE(individual_noise); WRITE(noise_sigma); WRITE(inhibitory);
        Object intervals(true);
        for(const auto& interval:v.pulse_intervals) {
            Object entry;put(entry,"start",interval.start);put(entry,"stop",interval.stop);
            intervals+=json::kvp("",entry.as_string());
        }
        o.set("pulse_intervals",intervals.as_string());
        neurons+=json::kvp("",o.as_string());
    }
    for(const auto& v:c.astrocytes) {
        Object o; WRITE(name); WRITE(px); WRITE(py); WRITE(ip3); WRITE(calcium); WRITE(h);
        WRITE(threshold); WRITE(alpha); WRITE(sic); WRITE(feedback); astrocytes+=json::kvp("",o.as_string());
    }
    for(const auto& v:c.synapses) {
        Object o; WRITE(source); WRITE(target); WRITE(astro); WRITE(conductance); WRITE(reversal);
        WRITE(release); WRITE(tau_syn); WRITE(tau_f); WRITE(tau_d); WRITE(spill); WRITE(enabled);
        put(o,"model",std::string(v.model==SynapseModel::ExponentialCurrent?"exponential_current":
            v.model==SynapseModel::TsodyksMarkram?"tsodyks_markram":"conductance_stp"));
        WRITE(weight); WRITE(gain);
        WRITE(stdp); WRITE(a_plus); WRITE(a_minus); WRITE(tau_plus); WRITE(tau_minus);
        WRITE(weight_min); WRITE(weight_max);
        WRITE(tau_slow);
        WRITE(delay);put(o,"stdp_timing",std::string(v.stdp_timing==StdpTiming::Arrival?"arrival":"emission"));
        put(o,"stdp_rule",std::string(v.stdp_rule==StdpRule::Triplet?"triplet":
            v.stdp_rule==StdpRule::Multiplicative?"multiplicative":"additive"));
        synapses+=json::kvp("",o.as_string());
    }
    for(const auto& v:c.gaps) {
        Object o; WRITE(a); WRITE(b); WRITE(ip3); WRITE(calcium); gaps+=json::kvp("",o.as_string());
    }
    root.set("neurons",neurons.as_string()); root.set("astrocytes",astrocytes.as_string());
    root.set("synapses",synapses.as_string()); root.set("gaps",gaps.as_string());
    return root.pretty();
}
std::string legacy_binding_config(const Config& c) {
    auto root=Object::parse(serialize(c));
    Object old;
    for(const auto& key:{"schema","name","dt","duration","sample","noise_sigma","noise_interval","seed","record_all","record_neurons","record_synapses","parameters","neurons","astrocytes","synapses","gaps"})old.set(key,root.get(key));
    put(old,"schema",5);return old.pretty();
}
Config deserialize(const std::string& text) {
    if(text.size()>MaxConfigBytes || text.find('\0')!=std::string::npos)throw std::runtime_error("Configuration exceeds 64 MiB or contains NUL bytes.");
    // Bound recursion before entering the legacy reader, and reject trailing commas.
    int depth=0;bool quoted=false,escaped=false;char previous=0;
    for(unsigned char ch:text) {
        if(quoted) {
            if(ch<32)throw std::runtime_error("Raw control character in JSON string.");
            if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;
        }else {
            if(ch=='"')quoted=true;
            if(ch=='{'||ch=='[') {if(++depth>32)throw std::runtime_error("JSON nesting is too deep.");}
            if(ch=='}'||ch==']') {if(--depth<0||previous==',')throw std::runtime_error("Invalid JSON closing delimiter.");}
            if(ch!=' '&&ch!='\n'&&ch!='\r'&&ch!='\t')previous=char(ch);
        }
    }
    if(depth!=0||quoted)throw std::runtime_error("Unterminated JSON value.");
    auto parsed=json::parsing::parse(text.c_str());
    if(parsed.type!=json::jtype::jobject || std::string(parsed.remainder).find_first_not_of(" \r\n\t")!=std::string::npos)
        throw std::runtime_error("Expected one JSON object, without trailing data.");
    Object root=Object::parse(text);
    const int schema=integer(root,"schema");
    if(schema<1||schema>9)throw std::runtime_error("Unsupported configuration schema.");
    Config c; c.name=string(root,"name"); c.dt=number(root,"dt"); c.duration=number(root,"duration"); c.sample=number(root,"sample");
    if(root.has_key("animat"))c.animat=read_animat_config(object(root,"animat",false));
    if(schema>=8){const int cap=integer(root,"history_capacity");if(cap<0||cap>int(MaxSamples))throw std::runtime_error("Invalid history capacity");c.history_capacity=size_t(cap);}
    if(schema>=2) { c.noise_sigma=number(root,"noise_sigma");c.noise_interval=number(root,"noise_interval");c.seed=seed_value(root); }
    if(schema>=4) {
        c.record_all=boolean(root,"record_all");
        c.record_neurons=read_indices(root,"record_neurons",MaxNeurons);
        c.record_synapses=read_indices(root,"record_synapses",MaxSynapses);
    }
    const auto p=object(root,"parameters",false);
    for(int i:SharedParameters)c.parameters[i]=number(p,na::parameter_names[i]);
    auto neurons=object(root,"neurons",true),astrocytes=object(root,"astrocytes",true),synapses=object(root,"synapses",true),gaps=object(root,"gaps",true);
    if(neurons.size()>MaxNeurons||astrocytes.size()>MaxAstrocytes||synapses.size()>MaxSynapses||gaps.size()>MaxGaps)
        throw std::runtime_error("Network exceeds supported size.");
    for(size_t i=0;i<neurons.size();++i) {
        const auto o=element(neurons,i); Neuron v; v.name=string(o,"name");
        READ(px); READ(py); READ(voltage); READ(current); READ(amplitude); READ(frequency); READ(start); READ(stop); READ(width);
        if(schema>=2) {
            v.model=neuron_model(o); READ(izh_a); READ(izh_b); READ(izh_c); READ(izh_d); READ(izh_peak); READ(izh_u);
            v.equilibrium_u=boolean(o,"equilibrium_u");v.individual_noise=boolean(o,"individual_noise");READ(noise_sigma);
        }
        if(schema>=4)v.inhibitory=boolean(o,"inhibitory");
        if(o.has_key("pulse_intervals")) {
            const auto intervals=object(o,"pulse_intervals",true);
            if(intervals.size()>64)throw std::runtime_error("At most 64 stimulus intervals per period are supported.");
            for(size_t k=0;k<intervals.size();++k) {
                const auto interval=element(intervals,k);
                v.pulse_intervals.push_back({number(interval,"start"),number(interval,"stop")});
            }
        }
        c.neurons.push_back(v);
    }
    for(size_t i=0;i<astrocytes.size();++i) {
        const auto o=element(astrocytes,i); Astrocyte v; v.name=string(o,"name");
        READ(px); READ(py); READ(ip3); READ(calcium); READ(h); READ(threshold); READ(alpha); READ(sic);
        v.feedback=boolean(o,"feedback"); c.astrocytes.push_back(v);
    }
    for(size_t i=0;i<synapses.size();++i) {
        const auto o=element(synapses,i); Synapse v; READINT(source); READINT(target); READINT(astro);
        READ(conductance); READ(reversal); READ(release); READ(tau_syn); READ(tau_f); READ(tau_d); READ(spill);
        if(schema>=2) { v.model=synapse_model(o);READ(weight);READ(gain); }
        if(schema>=3) {
            v.stdp=boolean(o,"stdp");READ(a_plus);READ(a_minus);READ(tau_plus);READ(tau_minus);
            READ(weight_min);READ(weight_max);v.stdp_rule=stdp_rule(o);
            if(o.has_key("tau_slow")){READ(tau_slow);}
        } else if(v.model==SynapseModel::ConductanceSTP) {
            // Schemas 1/2 ignored this field for conductance-based currents.
            // Preserve their dynamics now that all models use the multiplier.
            v.weight=1;
        }
        if(o.has_key("delay")){READ(delay);}
        if(o.has_key("stdp_timing")) {
            const auto timing=string(o,"stdp_timing");
            if(timing=="arrival")v.stdp_timing=StdpTiming::Arrival;
            else if(timing=="emission")v.stdp_timing=StdpTiming::Emission;
            else throw std::runtime_error("Unknown STDP timing: "+timing);
        }
        v.enabled=boolean(o,"enabled"); c.synapses.push_back(v);
    }
    for(size_t i=0;i<gaps.size();++i) {
        const auto o=element(gaps,i); Gap v; READINT(a); READINT(b); READ(ip3); READ(calcium); c.gaps.push_back(v);
    }
    read_experiment_config(root,c);
    if(schema>=7||root.has_key("stimulators")){c.stimulators=read_stimulators(root,"stimulators");c.next_stimulator_id=read_uid(root,"next_stimulator_id");c.activity_horizon=number(root,"activity_horizon");}
    const auto err=validate(c); if(!err.empty())throw std::runtime_error(err); return c;
}
#undef WRITE
#undef READ
#undef READINT
Config load_config(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary); if(!f)throw std::runtime_error("Cannot open configuration.");
    if(std::filesystem::file_size(path)>MaxConfigBytes)throw std::runtime_error("Configuration exceeds 64 MiB.");
    std::ostringstream text; text<<f.rdbuf(); if(f.bad())throw std::runtime_error("Configuration read failed.");
    std::string s=text.str(); if(s.compare(0,3,"\xef\xbb\xbf")==0)s.erase(0,3); return deserialize(s);
}
static void write_file(const std::filesystem::path& path,const std::string& contents) {
    auto temporary=path; temporary+=L".tmp-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        { std::ofstream f(temporary,std::ios::binary); f.exceptions(std::ios::badbit|std::ios::failbit); f<<contents; f.close(); }
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            const auto code=GetLastError();
            throw std::runtime_error("Cannot replace output file '"+path.u8string()+"': "+
                std::system_category().message(int(code))+" (Windows error "+std::to_string(code)+").");
        }
#else
        std::filesystem::rename(temporary,path);
#endif
    } catch(...) { std::error_code ec; std::filesystem::remove(temporary,ec); throw; }
}
void save_config(const Config& c,const std::filesystem::path& path) { write_file(path,serialize(c)+"\n"); }
void write_atomic(const std::filesystem::path& path,const std::string& bytes) {write_file(path,bytes);}
void export_results(const Simulator& s,const std::filesystem::path& prefix) {
    if(s.config.animat.enabled)export_animat(s,prefix);
    if(s.history.time.empty())throw std::runtime_error("No data to export.");
    if(s.history.weights.size()!=s.config.synapses.size()||s.plasticity.size()!=s.config.synapses.size())
        throw std::runtime_error("Weight history does not match the simulated synapses.");
    if(s.history.values.size()!=size_t(dimension(s.config)))throw std::runtime_error("State history has the wrong dimension.");
    std::vector<bool> neuron_mask(s.config.neurons.size(),s.config.record_all),synapse_mask(s.config.synapses.size(),s.config.record_all);
    for(int i:s.config.record_neurons)neuron_mask.at(i)=true;
    for(int i:s.config.record_synapses)synapse_mask.at(i)=true;
    std::vector<const std::vector<double>*> state_columns,weight_columns;
    auto state_column=[&](size_t index,bool selected) {
        const auto& column=s.history.values[index];
        if(column.size()!=(selected?s.history.time.size():0))throw std::runtime_error("State history does not match recording policy/times.");
        if(selected)state_columns.push_back(&column);
    };
    for(size_t i=0;i<s.config.neurons.size();++i)for(int k=0;k<4;++k)state_column(4*i+k,neuron_mask[i]);
    for(size_t i=0;i<s.config.synapses.size();++i) {
        for(int k=0;k<4;++k)state_column(syn_offset(s.config,int(i))+k,synapse_mask[i]);
        const auto& column=s.history.weights[i];
        if(column.size()!=(synapse_mask[i]?s.history.time.size():0))throw std::runtime_error("Weight history does not match recording policy/times.");
        if(synapse_mask[i])weight_columns.push_back(&column);
    }
    for(size_t i=0;i<s.config.astrocytes.size();++i)for(int k=0;k<7;++k)state_column(astro_offset(s.config,int(i))+k,true);
    auto output=[&](const wchar_t* suffix,const std::string& text) { auto path=prefix; path+=suffix; write_file(path,text); };
    std::ostringstream csv; csv.imbue(std::locale::classic()); csv<<std::setprecision(17)<<"time_s";
    for(size_t i=0;i<s.config.neurons.size();++i) {
        if(!neuron_mask[i])continue;
        const std::array<const char*,4> fields=s.config.neurons[i].model==NeuronModel::Izhikevich?
            std::array<const char*,4>{"V_mV","u","reserved_1","reserved_2"}:
            std::array<const char*,4>{"V_mV","m","h","n"};
        for(const char* field:fields)csv<<",N"<<i+1<<"_"<<field;
    }
    for(size_t i=0;i<s.config.synapses.size();++i) {
        if(!synapse_mask[i])continue;
        csv<<",S"<<i+1<<"_u,S"<<i+1<<"_x,S"<<i+1
           <<(s.config.synapses[i].model==SynapseModel::TsodyksMarkram?"_y,S":"_s,S")<<i+1<<"_Y_uM";
    }
    for(size_t i=0;i<s.config.astrocytes.size();++i)for(const char* field:{"GammaA","IP3_uM","Ca_uM","h","xA","GA_uM","GammaS"})csv<<",A"<<i+1<<"_"<<field;
    csv<<"\n";
    for(size_t row=0;row<s.history.time.size();++row) {
        csv<<s.history.time[row];for(const auto* column:state_columns)csv<<","<<(*column)[row];csv<<"\n";
    }
    output(L"_trace.csv",csv.str());
    std::ostringstream weights;weights.imbue(std::locale::classic());weights<<std::setprecision(17)<<"time_s";
    for(size_t i=0;i<s.config.synapses.size();++i)if(synapse_mask[i])weights<<",S"<<i+1<<"_weight";
    weights<<"\n";
    for(size_t row=0;row<s.history.time.size();++row) {
        weights<<s.history.time[row];for(const auto* column:weight_columns)weights<<","<<(*column)[row];weights<<"\n";
    }
    output(L"_weights.csv",weights.str());
    std::ostringstream events; events.imbue(std::locale::classic()); events<<std::setprecision(17)<<"time_s,type,node_index_1based\n";
    for(const auto& e:s.history.events)events<<e.time<<","<<(e.astro?"astro_release":"spike")<<","<<e.index+1<<"\n";
    output(L"_events.csv",events.str()); output(L"_experiment.json",serialize(s.config));
    auto delivery_csv=[&](const std::vector<Delivery>& list) {
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)
            <<"synapse_index_1based,source_index_1based,target_index_1based,t_emit_s,t_arrival_s\n";
        for(const auto& e:list) {const auto& edge=s.config.synapses[e.synapse];
            out<<e.synapse+1<<","<<edge.source+1<<","<<edge.target+1<<","<<e.t_emit<<","<<e.t_arrival<<"\n";}
        return out.str();
    };
    output(L"_deliveries.csv",delivery_csv(s.history.deliveries));
    auto pending=s.pending_deliveries;
    std::sort(pending.begin(),pending.end(),[](const Delivery& a,const Delivery& b){return DeliveryLater{}(b,a);});
    output(L"_pending_deliveries.csv",delivery_csv(pending));
    Object summary; put(summary,"time_s",s.time); put(summary,"steps",double(s.steps));
    Object integration;put(integration,"maximum_main_step_s",s.config.dt);
    uid(integration,"accepted_internal_steps",s.integration.accepted);uid(integration,"rejected_attempts",s.integration.rejected);
    put(integration,"accepted_model_time_s",s.time);put(integration,"mean_internal_step_s",s.integration.mean_step(s.time));
    summary.set("integration_statistics",integration.as_string());
    put(summary,"complete",s.done()&&s.error.empty()); put(summary,"error",s.error);
    put(summary,"integrator",std::string("event-localized RK4 step doubling; exact linear kinetics; input/sample/delivery boundaries"));
    put(summary,"deliveries",int(s.history.deliveries.size()));put(summary,"pending_deliveries",int(s.pending_deliveries.size()));
    put(summary,"record_all",s.config.record_all);put(summary,"recorded_dimension",recorded_dimension(s.config));
    Object recorded_neurons(true),recorded_synapses(true);
    for(size_t i=0;i<neuron_mask.size();++i)if(neuron_mask[i])recorded_neurons+=json::kvp("",std::to_string(i));
    for(size_t i=0;i<synapse_mask.size();++i)if(synapse_mask[i])recorded_synapses+=json::kvp("",std::to_string(i));
    summary.set("recorded_neurons_0based",recorded_neurons.as_string());
    summary.set("recorded_synapses_0based",recorded_synapses.as_string());put(summary,"all_astrocytes_recorded",true);
    Object spikes(true),releases(true),final_weights(true);
    for(auto v:s.spikes)spikes+=json::kvp("",std::to_string(v));
    for(auto v:s.releases)releases+=json::kvp("",std::to_string(v));
    for(const auto& state:s.plasticity) {
        std::ostringstream value;value.imbue(std::locale::classic());value<<std::setprecision(17)<<state.weight;
        final_weights+=json::kvp("",value.str());
    }
    summary.set("neuron_spikes",spikes.as_string()); summary.set("astrocyte_releases",releases.as_string());
    summary.set("final_synapse_weights",final_weights.as_string());
    put(summary,"events_available_after_s",s.history.events_after);put(summary,"deliveries_available_after_s",s.history.deliveries_after);put(summary,"stimuli_available_after_s",s.history.stimuli_after);
    put(summary,"state_history_from_s",s.history.time.front());uid(summary,"state_sample_begin",s.history.sample_begin);put(summary,"rolling_state_history",s.config.history_capacity>0);put(summary,"bounded_journals",s.config.bounded_journals);put(summary,"shared_stimuli_available_after_s",s.history.shared_after);put(summary,"activity_available_after_s",s.activity.available_after);put(summary,"activity_horizon_s",s.config.activity_horizon);
    output(L"_summary.json",summary.pretty());
    output(L"_protocols.json",serialize_runtime(s));
    std::ostringstream stimuli;stimuli.imbue(std::locale::classic());stimuli<<std::setprecision(17)<<"time_s,neuron_1based,protocol_0based,amplitude\n";
    for(const auto& e:s.history.stimuli)stimuli<<e.time<<","<<e.neuron+1<<","<<e.protocol<<","<<e.amplitude<<"\n";
    output(L"_stimuli.csv",stimuli.str());
    std::ostringstream shared;shared.imbue(std::locale::classic());shared<<std::setprecision(17)<<"stimulator_id,pulse_id,start_s,end_s,amplitude,complete,stage_0based,recipients_1based\n";
    for(const auto& p:s.history.shared_pulses){shared<<p.stimulator<<","<<p.pulse<<","<<p.start<<","<<(p.complete?p.end:s.time)<<","<<p.amplitude<<","<<p.complete<<","<<p.stage<<",\"";for(size_t j=0;j<p.recipients.size();++j){if(j)shared<<";";shared<<p.recipients[j]+1;}shared<<"\"\n";}
    output(L"_shared_stimuli.csv",shared.str());output(L"_stimulator_commands.json",serialize_stimulus_runtime(s));

    export_analysis(analyze_network(s.config,s.history,s.time,s.config.analysis),s.config.analysis,prefix);
}
}
