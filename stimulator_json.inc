// Shared by Config, protocol versions and controller command persistence.
static void uid(Object& o,const char* key,uint64_t value){put(o,key,std::to_string(value));}
static uint64_t read_uid(const Object& o,const char* key) {
    auto text=string(o,key);uint64_t value=0;
    if(text.empty()||text.size()>20||(text.size()>1&&text[0]=='0'))throw std::runtime_error("Invalid uint64 string");
    for(char c:text){if(c<'0'||c>'9'||value>(UINT64_MAX-uint64_t(c-'0'))/10)throw std::runtime_error("Invalid uint64 string");value=value*10+uint64_t(c-'0');}return value;
}
static Object uid_array(const std::vector<uint64_t>& list){Object o(true);for(auto x:list)o+=json::kvp("",json::parsing::encode_string(std::to_string(x).c_str()));return o;}
static std::vector<uint64_t> read_uids(const Object& o,const char* key,size_t max) {
    auto array=object(o,key,true);if(array.size()>max)throw std::runtime_error("Too many IDs");std::vector<uint64_t> out;
    for(size_t i=0;i<array.size();++i){Object item;item.set("id",array.get(i));out.push_back(read_uid(item,"id"));}return out;
}
static Object stimulator_object(const Stimulator& v) {
    Object o;uid(o,"id",v.id);WRITE(name);WRITE(enabled);WRITE(spatial);WRITE(x);WRITE(y);WRITE(radius);WRITE(first_delay);put(o,"filter",int(v.filter));
    o.set("recipients",index_array(v.recipients).as_string());
    const auto& n=v.signal;Object signal;put(signal,"start",n.start);put(signal,"stop",n.stop);put(signal,"amplitude",n.amplitude);put(signal,"frequency",n.frequency);put(signal,"width",n.width);
    Object table(true);for(auto p:n.pulse_intervals){Object x;put(x,"start",p.start);put(x,"stop",p.stop);table+=json::kvp("",x.as_string());}
    signal.set("pulse_intervals",table.as_string());o.set("signal",signal.as_string());return o;
}
static Stimulator read_stimulator(const Object& o) {
    Stimulator v;v.id=read_uid(o,"id");v.name=string(o,"name");v.enabled=boolean(o,"enabled");v.spatial=boolean(o,"spatial");READ(x);READ(y);READ(radius);READ(first_delay);
    v.filter=StimulatorFilter(integer(o,"filter"));v.recipients=read_indices(o,"recipients",MaxNeurons);
    auto signal=object(o,"signal",false);auto& n=v.signal;n.start=number(signal,"start");n.stop=number(signal,"stop");n.amplitude=number(signal,"amplitude");n.frequency=number(signal,"frequency");n.width=number(signal,"width");
    auto table=object(signal,"pulse_intervals",true);if(table.size()>64)throw std::runtime_error("Shared pulse table too large");
    for(size_t k=0;k<table.size();++k){auto p=element(table,k);n.pulse_intervals.push_back({number(p,"start"),number(p,"stop")});}return v;
}
static Object stimulator_array(const std::vector<Stimulator>& list){Object o(true);for(auto& s:list)o+=json::kvp("",stimulator_object(s).as_string());return o;}
static std::vector<Stimulator> read_stimulators(const Object& o,const char* key) {
    auto array=object(o,key,true);if(array.size()>64)throw std::runtime_error("At most 64 shared stimulators");std::vector<Stimulator> result;
    for(size_t k=0;k<array.size();++k)result.push_back(read_stimulator(element(array,k)));return result;
}
std::string serialize_stimulus_runtime(const Simulator& s) {
    Object o;put(o,"document_type",std::string("network-studio-stimulator-commands"));put(o,"schema",1);uid(o,"next_command",s.next_stimulus_command);Object commands(true),changes(true),latches(true);
    for(const auto& c:s.stimulus_commands){Object x;uid(x,"sequence",c.sequence);put(x,"time",c.time);put(x,"restart",c.restart);x.set("value",stimulator_object(c.value).as_string());commands+=json::kvp("",x.as_string());}
    for(const auto& c:s.stimulus_changes){Object x;uid(x,"sequence",c.sequence);put(x,"time",c.time);put(x,"stage",c.stage);put(x,"restart",c.restart);x.set("value",stimulator_object(c.value).as_string());x.set("recipients",index_array(c.recipients).as_string());changes+=json::kvp("",x.as_string());}
    for(const auto& l:s.stimulus_latches){Object x;uid(x,"id",l.id);uid(x,"next_pulse",l.next_pulse);uid(x,"active_pulse",l.active_pulse);latches+=json::kvp("",x.as_string());}
    o.set("commands",commands.as_string());o.set("changes",changes.as_string());o.set("latches",latches.as_string());return o.as_string();
}
void deserialize_stimulus_runtime(Simulator& s,const std::string& text) {
    auto o=Object::parse(text);s.next_stimulus_command=read_uid(o,"next_command");if(!s.next_stimulus_command)throw std::runtime_error("Invalid command counter");
    auto commands=object(o,"commands",true),changes=object(o,"changes",true),latches=object(o,"latches",true);
    if(commands.size()+changes.size()>4096||latches.size()>4096)throw std::runtime_error("Stimulator runtime budget exceeded");
    std::set<uint64_t> sequences,ids;
    auto check_value=[&](const Stimulator& value){Config c=s.config;c.stimulators={value};c.stages.clear();c.analysis.reference_stimulator=0;auto issue=validate_stimulators(c);if(!issue.empty())throw std::runtime_error(issue);};
    auto check_sequence=[&](uint64_t sequence){if(!sequence||sequence>=s.next_stimulus_command||!sequences.insert(sequence).second)throw std::runtime_error("Invalid/duplicate command sequence");};
    double previous=s.time;uint64_t previous_sequence=0;
    for(size_t i=0;i<commands.size();++i){auto x=element(commands,i);StimulatorCommand c{read_uid(x,"sequence"),number(x,"time"),read_stimulator(object(x,"value",false)),boolean(x,"restart")};
        check_sequence(c.sequence);check_value(c.value);if((s.error.empty()?c.time<=s.time:c.time<s.time)||c.time<previous||(c.time==previous&&c.sequence<=previous_sequence))throw std::runtime_error("Unordered command queue");previous=c.time;previous_sequence=c.sequence;s.stimulus_commands.push_back(c);}
    previous=0;
    for(size_t i=0;i<changes.size();++i){auto x=element(changes,i);StimulatorChange c{read_uid(x,"sequence"),number(x,"time"),integer(x,"stage"),read_stimulator(object(x,"value",false)),read_indices(x,"recipients",MaxNeurons),boolean(x,"restart")};
        check_sequence(c.sequence);check_value(c.value);if(c.time<previous||c.time>s.time||c.stage< -1||c.stage>255)throw std::runtime_error("Invalid command history time");previous=c.time;
        for(int n:c.recipients)if(n<0||n>=int(s.config.neurons.size()))throw std::runtime_error("Invalid command recipient");s.stimulus_changes.push_back(c);}
    Config future=s.config;
    for(const auto& command:s.stimulus_commands){auto it=std::find_if(future.stimulators.begin(),future.stimulators.end(),[&](auto& v){return v.id==command.value.id;});if(it==future.stimulators.end())future.stimulators.push_back(command.value);else *it=command.value;std::sort(future.stimulators.begin(),future.stimulators.end(),[](auto& a,auto& b){return a.id<b.id;});auto issue=validate_stimulators(future);if(!issue.empty())throw std::runtime_error(issue);}
    for(size_t i=0;i<latches.size();++i){auto x=element(latches,i);StimulatorLatch l{read_uid(x,"id"),read_uid(x,"next_pulse"),read_uid(x,"active_pulse")};
        if(!l.id||l.id>=s.config.next_stimulator_id||!l.next_pulse||l.active_pulse>=l.next_pulse||!ids.insert(l.id).second)throw std::runtime_error("Invalid stimulator latch");s.stimulus_latches.push_back(l);}
}

void load_stimulator_replay(Simulator& s,const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);if(size>16*1024*1024)throw std::runtime_error("Stimulator replay exceeds 16 MiB");
    std::ifstream input(path,std::ios::binary);std::string text(size,'\0');if(!input.read(text.data(),std::streamsize(size)))throw std::runtime_error("Cannot read stimulator replay");
    auto o=Object::parse(text);if(string(o,"document_type")!="network-studio-stimulator-commands"||integer(o,"schema")!=1)throw std::runtime_error("Unsupported stimulator replay format");
    std::vector<StimulatorCommand> commands;
    for(const char* key:{"changes","commands"}){auto array=object(o,key,true);if(commands.size()+array.size()>4096)throw std::runtime_error("Replay command limit");for(size_t i=0;i<array.size();++i){auto c=element(array,i);commands.push_back({read_uid(c,"sequence"),number(c,"time"),read_stimulator(object(c,"value",false)),boolean(c,"restart")});}}
    s.replay_stimulators(std::move(commands));
}
