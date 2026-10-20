// Included by io.cpp: reuse the existing AnalysisSettings JSON model and parser.
static void validate_profile(const AnalysisProfile& p) {
    if(p.name.size()>128||p.name.find_first_of("\r\n\t")!=std::string::npos||p.name.find('\0')!=std::string::npos)throw std::runtime_error("Invalid analysis profile name (128 UTF-8 bytes maximum)");
    Config probe;probe.neurons.resize(MaxNeurons);probe.analysis=p.settings;probe.analysis.reference_stimulator=0;
    auto error=validate_experiment(probe);if(!error.empty())throw std::runtime_error(error);
}
std::string analysis_profile_json(const AnalysisProfile& p) {
    validate_profile(p);Object o;put(o,"document_type",std::string("network_studio_analysis_profile"));put(o,"schema_version",1);
    put(o,"name",p.name);o.set("settings",analysis_settings_json(p.settings));return o.as_string();
}
AnalysisProfile analysis_profile_from_json(const std::string& text) {
    if(text.size()>65536)throw std::runtime_error("Analysis profile too large");auto o=Object::parse(text);
    if(string(o,"document_type")!="network_studio_analysis_profile"||integer(o,"schema_version")!=1)throw std::runtime_error("Incompatible analysis profile");
    AnalysisProfile p{string(o,"name"),analysis_settings_from_json(object(o,"settings",false).as_string())};validate_profile(p);return p;
}
std::string analysis_interval_json(const AnalysisInterval& i) {
    if(!valid_analysis_interval(i))throw std::runtime_error("Invalid analysis interval");Object o;put(o,"from_s",i.from);put(o,"to_s",i.to);put(o,"current_end",i.current_end);return o.as_string();
}
AnalysisInterval analysis_interval_from_json(const std::string& text) {
    if(text.size()>1024)throw std::runtime_error("Analysis interval too large");auto o=Object::parse(text);
    AnalysisInterval i{number(o,"from_s"),number(o,"to_s"),boolean(o,"current_end")};if(!valid_analysis_interval(i))throw std::runtime_error("Invalid analysis interval");return i;
}
std::string analysis_result_json(const NetworkAnalysis& r,const AnalysisProfile& p) {
    Object o;put(o,"document_type",std::string("network_studio_analysis_result"));put(o,"schema_version",1);put(o,"algorithm",std::string(AnalysisAlgorithm));
    o.set("profile",analysis_profile_json(p));
    put(o,"requested_from_s",r.requested_from);put(o,"requested_until_s",r.requested_until);
    put(o,"available_from_s",r.available_from);put(o,"available_until_s",r.available_until);
    put(o,"effective_from_s",r.from);put(o,"effective_until_s",r.until);put(o,"full_interval",r.full_interval);
    put(o,"prehistory_complete",r.prehistory_complete);put(o,"detector_certain_from_s",r.detector_certain_from);
    put(o,"population_size",double(r.population_size));put(o,"detected_bursts",double(r.bursts.size()));
    put(o,"complete_bursts",double(std::count_if(r.bursts.begin(),r.bursts.end(),[](const auto& b){return b.complete;})));
    put(o,"stimulus_hz",r.stimulus_hz);put(o,"burst_hz",r.response_hz);put(o,"eligible_stimuli",double(r.responses.size()));
    put(o,"matched",double(r.responses.size()-r.missed));put(o,"missed",double(r.missed));put(o,"extra",double(r.extra));put(o,"incomplete_stimuli",double(r.incomplete_stimuli));
    put(o,"response_fraction",r.responses.empty()?0.:double(r.responses.size()-r.missed)/r.responses.size());
    put(o,"interval_rule",std::string("[from,to); burst start owns interval; complete requires end+min_gap+W<=to; response window closed and strictly before to"));
    return o.as_string();
}
static std::string analysis_read_file(const std::filesystem::path& path,size_t limit) {
    auto n=std::filesystem::file_size(path);if(n>limit)throw std::runtime_error("Analysis input exceeds size limit");
    std::ifstream in(path,std::ios::binary);std::string text(size_t(n),'\0');if(!in.read(text.data(),std::streamsize(n)))throw std::runtime_error("Cannot read analysis input");return text;
}
std::string compare_analysis_files(const std::filesystem::path& ap,const std::filesystem::path& bp) {
    auto read=[](const std::filesystem::path& path) {
        auto o=Object::parse(analysis_read_file(path,1024*1024));
        if(string(o,"document_type")!="network_studio_analysis_result"||integer(o,"schema_version")!=1||string(o,"algorithm")!=AnalysisAlgorithm)throw std::runtime_error("Incompatible analysis result format/algorithm");
        auto profile=analysis_profile_from_json(object(o,"profile",false).as_string());NetworkAnalysis r;
        r.requested_from=number(o,"requested_from_s");r.requested_until=number(o,"requested_until_s");r.available_from=number(o,"available_from_s");r.available_until=number(o,"available_until_s");
        r.from=number(o,"effective_from_s");r.until=number(o,"effective_until_s");r.full_interval=boolean(o,"full_interval");r.prehistory_complete=boolean(o,"prehistory_complete");r.detector_certain_from=number(o,"detector_certain_from_s");
        auto n=number(o,"population_size");if(n<1||n>MaxNeurons||std::floor(n)!=n)throw std::runtime_error("Invalid analysis population");r.population_size=size_t(n);
        if(r.requested_from<0||r.requested_until<=r.requested_from||r.available_from<0||r.available_until<r.available_from||r.until<=r.from||r.from!=std::max(r.requested_from,r.available_from)||r.until!=std::min(r.requested_until,r.available_until)||r.full_interval!=(r.from==r.requested_from&&r.until==r.requested_until)||r.detector_certain_from<0||r.prehistory_complete!=(r.detector_certain_from<=r.from))throw std::runtime_error("Inconsistent analysis coverage");
        return std::make_pair(r,profile.settings);
    };
    auto a=read(ap),b=read(bp);return compare_analysis(a.first,a.second,b.first,b.second);
}
NetworkAnalysis analyze_export(const std::filesystem::path& prefix,const AnalysisSettings& settings,const AnalysisInterval& interval) {
    auto file=[&](const wchar_t* suffix){auto p=prefix;p+=suffix;return p;};
    auto config=load_config(file(L"_experiment.json"));auto summary=Object::parse(analysis_read_file(file(L"_summary.json"),16*1024*1024));History h;
    const double until=number(summary,"time_s");h.events_after=number(summary,"events_available_after_s");h.stimuli_after=number(summary,"stimuli_available_after_s");h.shared_after=number(summary,"shared_stimuli_available_after_s");
    if(until<0||std::min({h.events_after,h.stimuli_after,h.shared_after})<0||std::max({h.events_after,h.stimuli_after,h.shared_after})>until)throw std::runtime_error("Invalid exported history coverage");
    auto csv=[&](const wchar_t* suffix,const char* header,auto row) {
        auto path=file(suffix);if(std::filesystem::file_size(path)>256*1024*1024)throw std::runtime_error("Analysis CSV exceeds 256 MiB");
        std::ifstream in(path);in.imbue(std::locale::classic());std::string line;std::getline(in,line);if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line!=header)throw std::runtime_error("Unexpected analysis CSV header");size_t count=0;
        while(std::getline(in,line)){if(++count>MaxEvents||line.size()>65536)throw std::runtime_error("Analysis CSV resource limit");std::istringstream fields(line);fields.imbue(std::locale::classic());row(fields);}
        if(!in.eof())throw std::runtime_error("Cannot read analysis CSV");
    };
    auto real=[](std::istringstream& in){std::string token;std::getline(in,token,',');std::istringstream s(token);s.imbue(std::locale::classic());double n;s>>n;if(!s||!std::isfinite(n)||(s>>std::ws,!s.eof()))throw std::runtime_error("Invalid CSV number");return n;};
    auto index=[&](std::istringstream& in,int low,int high){double n=real(in);if(n<low||n>high||n!=std::floor(n))throw std::runtime_error("Invalid CSV index");return int(n);};
    csv(L"_events.csv","time_s,type,node_index_1based",[&](auto& in){Event e;e.time=real(in);std::string type;std::getline(in,type,',');if(type!="spike"&&type!="astro_release")throw std::runtime_error("Invalid event type");e.astro=type=="astro_release";e.index=index(in,1,int(e.astro?config.astrocytes.size():config.neurons.size()))-1;
        if(e.time<h.events_after||e.time>until||(!h.events.empty()&&e.time<h.events.back().time))throw std::runtime_error("Invalid event time/order");h.events.push_back(e);});
    if(settings.reference_stimulator)csv(L"_shared_stimuli.csv","stimulator_id,pulse_id,start_s,end_s,amplitude,complete,stage_0based,recipients_1based",[&](auto& in){
        auto id=[&](){std::string v;std::getline(in,v,',');if(v.empty()||v.find_first_not_of("0123456789")!=v.npos)throw std::runtime_error("Invalid CSV id");return std::stoull(v);};
        SharedPulse p;p.stimulator=id();p.pulse=id();p.start=real(in);p.end=real(in);p.amplitude=real(in);p.complete=index(in,0,1)!=0;p.stage=index(in,-1,255);
        if(p.start<h.shared_after||p.start>until||p.end<p.start||(!h.shared_pulses.empty()&&p.start<h.shared_pulses.back().start))throw std::runtime_error("Invalid shared stimulus time/order");h.shared_pulses.push_back(p);
    });
    else csv(L"_stimuli.csv","time_s,neuron_1based,protocol_0based,amplitude",[&](auto& in){StimulusEvent e;e.time=real(in);e.neuron=index(in,1,int(config.neurons.size()))-1;e.protocol=index(in,0,100000);e.amplitude=real(in);
        if(e.time<h.stimuli_after||e.time>until||(!h.stimuli.empty()&&e.time<h.stimuli.back().time))throw std::runtime_error("Invalid stimulus time/order");h.stimuli.push_back(e);});
    return analyze_network(config,h,until,settings,interval);
}
