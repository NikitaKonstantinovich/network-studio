// Included inside io.cpp's namespace: shares strict JSON primitives.
static Object analysis_object(const AnalysisSettings& v) {
    Object o;put(o,"population",int(v.population));o.set("neurons",index_array(v.neurons).as_string());
    WRITE(bin);WRITE(burst_window);WRITE(start_rate);WRITE(end_rate);WRITE(min_neurons);WRITE(min_fraction);WRITE(min_gap);WRITE(burst_rate_window);
    WRITE(reference_neuron);WRITE(response_min);WRITE(response_max);uid(o,"reference_stimulator",v.reference_stimulator);return o;
}
static AnalysisSettings read_analysis(const Object& o) {
    AnalysisSettings v;v.population=AnalysisPopulation(integer(o,"population"));v.neurons=read_indices(o,"neurons",MaxNeurons);
    READ(bin);READ(burst_window);READ(start_rate);READ(end_rate);READINT(min_neurons);READ(min_fraction);READ(min_gap);READ(burst_rate_window);
    READINT(reference_neuron);READ(response_min);READ(response_max);if(o.has_key("reference_stimulator"))v.reference_stimulator=read_uid(o,"reference_stimulator");return v;
}
static Object stage_object(const ExperimentStage& v) {
    Object o;WRITE(name);WRITE(duration);WRITE(learning);WRITE(learning_rate);WRITE(clear_traces);
    o.set("neurons",index_array(v.neurons).as_string());WRITE(stimulus_enabled);WRITE(amplitude);WRITE(frequency);WRITE(width);WRITE(first_pulse);o.set("stimulator_ids",uid_array(v.stimulator_ids).as_string());return o;
}
static ExperimentStage read_stage(const Object& o) {
    ExperimentStage v;v.name=string(o,"name");READ(duration);v.learning=boolean(o,"learning");READ(learning_rate);v.clear_traces=boolean(o,"clear_traces");
    v.neurons=read_indices(o,"neurons",MaxNeurons);v.stimulus_enabled=boolean(o,"stimulus_enabled");READ(amplitude);READ(frequency);READ(width);READ(first_pulse);if(o.has_key("stimulator_ids"))v.stimulator_ids=read_uids(o,"stimulator_ids",64);return v;
}
static void write_experiment_config(Object& root,const Config& c) {
    Object space;put(space,"width",c.space.width);put(space,"height",c.space.height);put(space,"unit",c.space.unit);root.set("space",space.as_string());
    Object generated(true);for(const auto& g:c.generation){Object o;put(o,"settings_json",g.settings_json);put(o,"first",g.first);put(o,"count",g.count);generated+=json::kvp("",o.as_string());}
    root.set("generation",generated.as_string());Object stages(true);for(const auto& s:c.stages)stages+=json::kvp("",stage_object(s).as_string());
    root.set("stages",stages.as_string());root.set("analysis",analysis_object(c.analysis).as_string());
    put(root,"bounded_journals",c.bounded_journals);put(root,"journal_capacity",double(c.journal_capacity));
}
static void read_experiment_config(const Object& root,Config& c) {
    if(root.has_key("space")){auto o=object(root,"space",false);c.space={number(o,"width"),number(o,"height"),string(o,"unit")};}
    if(root.has_key("analysis"))c.analysis=read_analysis(object(root,"analysis",false));
    if(root.has_key("generation")) {
        auto list=object(root,"generation",true);if(list.size()>1000)throw std::runtime_error("Too many generation records");
        for(size_t i=0;i<list.size();++i){auto o=element(list,i);auto text=string(o,"settings_json");if(text.size()>65536)throw std::runtime_error("Generation metadata too large");c.generation.push_back({text,integer(o,"first"),integer(o,"count")});}
    }
    if(root.has_key("stages")) {
        auto list=object(root,"stages",true);if(list.size()>256)throw std::runtime_error("Too many stages");
        for(size_t i=0;i<list.size();++i)c.stages.push_back(read_stage(element(list,i)));
    }
    if(root.has_key("bounded_journals"))c.bounded_journals=boolean(root,"bounded_journals");
    if(root.has_key("journal_capacity")){double value=number(root,"journal_capacity");if(value<50000||value>MaxEvents||value!=std::floor(value))throw std::runtime_error("Invalid journal capacity");c.journal_capacity=size_t(value);}
}
std::string generation_json(const NetworkGeneration& g) {
    Object o;put(o,"version",1);put(o,"seed",double(g.seed));put(o,"topology",int(g.topology));put(o,"radius",g.radius);put(o,"allow_self",g.allow_self);
    put(o,"width",g.space.width);put(o,"height",g.space.height);put(o,"unit",g.space.unit);put(o,"name",g.name);put(o,"duration",g.duration);put(o,"sample",g.sample);
    auto pop=[&](const PopulationGeneration& p){Object o;put(o,"count",p.count);auto gauss=[&](const char* key,const GaussianParameter& v){Object pair;put(pair,"mean",v.mean);put(pair,"sd",v.sd);o.set(key,pair.as_string());};
        gauss("a",p.a);gauss("b",p.b);gauss("c",p.c);gauss("d",p.d);gauss("in_degree",p.in_degree);gauss("weight",p.weight);return o;};
    o.set("excitatory",pop(g.excitatory).as_string());o.set("inhibitory",pop(g.inhibitory).as_string());
    Config template_config;template_config.neurons.resize(2);auto syn=g.synapse;syn.source=0;syn.target=1;syn.astro=-1;syn.weight=.5;syn.stdp=false;template_config.synapses.push_back(syn);
    o.set("synapse_template",Object::parse(serialize(template_config)).get("synapses"));put(o,"stdp",g.synapse.stdp);return o.pretty();
}
std::string serialize_runtime(const Simulator& s) {
    const auto& v=s.experiment;Object o;WRITE(learning);WRITE(learning_rate);WRITE(automatic);WRITE(stage_active);WRITE(stage_end);WRITE(stage_index);WRITE(origin);
    Object protocols(true);for(const auto& p:v.protocols) {
        Object entry;put(entry,"time",p.time);put(entry,"end",p.end);put(entry,"name",p.name);put(entry,"learning",p.learning);put(entry,"cleared",p.cleared);put(entry,"learning_rate",p.learning_rate);
        Object neurons(true);for(const auto& v:p.neurons) {
            Object o;WRITE(current);WRITE(amplitude);WRITE(frequency);WRITE(start);WRITE(stop);WRITE(width);WRITE(individual_noise);WRITE(noise_sigma);
            Object intervals(true);for(auto pulse:v.pulse_intervals){Object x;put(x,"start",pulse.start);put(x,"stop",pulse.stop);intervals+=json::kvp("",x.as_string());}
            o.set("pulse_intervals",intervals.as_string());neurons+=json::kvp("",o.as_string());
        }entry.set("neurons",neurons.as_string());entry.set("stimulators",stimulator_array(p.stimulators).as_string());protocols+=json::kvp("",entry.as_string());
    }o.set("protocols",protocols.as_string());return o.as_string();
}
void deserialize_runtime(Simulator& s,const std::string& text) {
    const auto o=Object::parse(text);ExperimentRuntime v;
    v.learning=boolean(o,"learning");READ(learning_rate);v.automatic=boolean(o,"automatic");v.stage_active=boolean(o,"stage_active");READ(stage_end);READINT(stage_index);v.origin=string(o,"origin");
    auto list=object(o,"protocols",true);if(list.size()==0||list.size()>4096)throw std::runtime_error("Invalid protocol count");
    double previous=0;
    for(size_t i=0;i<list.size();++i) {
        auto o=element(list,i);ProtocolVersion p;p.time=number(o,"time");p.end=number(o,"end");p.name=string(o,"name");p.learning=boolean(o,"learning");p.cleared=boolean(o,"cleared");p.learning_rate=number(o,"learning_rate");
        if(p.time<previous||p.time>s.time||p.end<p.time||p.end>s.time||p.learning_rate<0||p.learning_rate>100||p.name.size()>120||(i==0&&p.time!=0))throw std::runtime_error("Invalid protocol history");
        if(i&&v.protocols.back().end!=p.time)throw std::runtime_error("Protocol history gap");previous=p.time;
        auto nodes=object(o,"neurons",true);if(nodes.size()!=s.config.neurons.size())throw std::runtime_error("Protocol graph mismatch");
        p.neurons=s.config.neurons;
        for(size_t j=0;j<nodes.size();++j){auto o=element(nodes,j);auto& v=p.neurons[j];READ(current);READ(amplitude);READ(frequency);READ(start);READ(stop);READ(width);v.individual_noise=boolean(o,"individual_noise");READ(noise_sigma);
            auto pulses=object(o,"pulse_intervals",true);if(pulses.size()>64)throw std::runtime_error("Too many protocol intervals");v.pulse_intervals.clear();for(size_t k=0;k<pulses.size();++k){auto x=element(pulses,k);v.pulse_intervals.push_back({number(x,"start"),number(x,"stop")});}}
        p.stimulators=read_stimulators(o,"stimulators");
        Config check=s.config;check.neurons=p.neurons;check.stimulators=p.stimulators;check.stages.clear();check.analysis.reference_stimulator=0;if(!validate(check).empty())throw std::runtime_error("Invalid historical protocol");v.protocols.push_back(std::move(p));
    }
    if(v.origin.size()>4096||v.learning_rate<0||v.learning_rate>100||v.stage_end<0||(v.stage_active&&(v.stage_index<0||v.stage_index>=int(s.config.stages.size())||v.stage_end>s.config.duration))||(!v.stage_active&&v.automatic))throw std::runtime_error("Invalid execution state");
    s.experiment=std::move(v);
}

std::string analysis_settings_json(const AnalysisSettings& a){return analysis_object(a).as_string();}
AnalysisSettings analysis_settings_from_json(const std::string& text){if(text.size()>65536)throw std::runtime_error("Analysis settings too large");return read_analysis(Object::parse(text));}
