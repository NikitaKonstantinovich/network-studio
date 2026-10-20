// Uses the existing strict JSON codec. Scientific field settings only.
static Object animat_environment_object(const AnimatEnvironment& v) {
    Object o;WRITE(outside_frequency);WRITE(amplitude);WRITE(width);WRITE(radius);WRITE(enabled);
    Object zones(true);
    for(const auto& v:v.zones){Object o;uid(o,"id",v.id);WRITE(name);put(o,"shape",int(v.shape));WRITE(priority);
        WRITE(x);WRITE(y);WRITE(width);WRITE(height);WRITE(frequency);zones+=json::kvp("",o.as_string());}
    o.set("zones",zones.as_string());return o;
}
static AnimatEnvironment read_animat_environment(const Object& o) {
    AnimatEnvironment v;READ(outside_frequency);READ(amplitude);READ(width);READ(radius);v.enabled=boolean(o,"enabled");
    auto zones=object(o,"zones",true);if(zones.size()>MaxAnimatZones)throw std::runtime_error("Animat: too many zones");
    for(size_t i=0;i<zones.size();++i){auto z=element(zones,i);ArenaZone a;
        a.id=read_uid(z,"id");a.name=string(z,"name");a.shape=ZoneShape(integer(z,"shape"));a.priority=integer(z,"priority");
        a.x=number(z,"x");a.y=number(z,"y");a.width=number(z,"width");a.height=number(z,"height");a.frequency=number(z,"frequency");v.zones.push_back(std::move(a));}
    return v;
}
std::string animat_environment_json(const AnimatEnvironment& e){return animat_environment_object(e).as_string();}
AnimatEnvironment animat_environment_from_json(const std::string& s){if(s.size()>65536)throw std::runtime_error("Animat environment exceeds 64 KiB");return read_animat_environment(Object::parse(s));}
static Object animat_config_object(const AnimatConfig& v) {
    Object o;WRITE(enabled);uid(o,"stimulator",v.stimulator);WRITE(initial_x);WRITE(initial_y);WRITE(interval);
    WRITE(gain);WRITE(max_displacement);WRITE(zero_threshold);put(o,"boundary",int(v.boundary));put(o,"silence",int(v.silence));
    Object f;const auto& a=v.field;
    put(f,"columns",a.columns);put(f,"rows",a.rows);put(f,"mode",a.activity_mode);put(f,"window",a.activity_window);
    put(f,"response_min",a.response_min);put(f,"response_max",a.response_max);
    put(f,"polarity",int(a.activity_polarity));put(f,"model",int(a.activity_model));
    o.set("field",f.as_string());o.set("environment",animat_environment_object(v.environment).as_string());return o;
}
static AnimatConfig read_animat_config(const Object& o) {
    AnimatConfig v;v.enabled=boolean(o,"enabled");v.stimulator=read_uid(o,"stimulator");READ(initial_x);READ(initial_y);
    READ(interval);READ(gain);READ(max_displacement);READ(zero_threshold);v.boundary=AnimatBoundary(integer(o,"boundary"));v.silence=AnimatSilence(integer(o,"silence"));
    auto f=object(o,"field",false);auto& a=v.field;a.columns=integer(f,"columns");a.rows=integer(f,"rows");
    a.activity_mode=integer(f,"mode");a.activity_window=number(f,"window");a.response_min=number(f,"response_min");a.response_max=number(f,"response_max");
    a.activity_polarity=FieldPolarity(integer(f,"polarity"));a.activity_model=FieldModel(integer(f,"model"));
    v.environment=read_animat_environment(object(o,"environment",false));return v;
}
static Object animat_stage_object(const AnimatStage& v) {
    Object o;WRITE(environment);WRITE(home);WRITE(marker);WRITE(position);WRITE(x);WRITE(y);o.set("value",animat_environment_object(v.value).as_string());return o;
}
static AnimatStage read_animat_stage(const Object& o) {
    AnimatStage v;v.environment=boolean(o,"environment");v.home=boolean(o,"home");v.marker=boolean(o,"marker");if(o.has_key("position")){v.position=boolean(o,"position");READ(x);READ(y);}
    v.value=read_animat_environment(object(o,"value",false));return v;
}
std::string animat_commands_json(const std::vector<AnimatAction>& actions) {
    Object root;put(root,"document_type",std::string("network-studio-animat-actions"));put(root,"schema",1);Object list(true);
    for(const auto& v:actions){Object o;uid(o,"sequence",v.sequence);WRITE(time);put(o,"kind",int(v.kind));WRITE(stage);WRITE(automatic);WRITE(learning);WRITE(clear_traces);WRITE(learning_rate);
        o.set("environment",animat_environment_object(v.environment).as_string());list+=json::kvp("",o.as_string());}
    root.set("actions",list.as_string());return root.pretty();
}
std::vector<AnimatAction> read_animat_commands(const std::string& text) {
    if(text.size()>16*1024*1024)throw std::runtime_error("Animat: command file exceeds 16 MiB");
    auto root=Object::parse(text);if(string(root,"document_type")!="network-studio-animat-actions"||integer(root,"schema")!=1)throw std::runtime_error("Animat: incompatible command document");
    auto list=object(root,"actions",true);if(list.size()>MaxAnimatActions)throw std::runtime_error("Animat: too many actions");
    std::vector<AnimatAction> out;double last=-1;std::set<uint64_t> ids;
    for(size_t i=0;i<list.size();++i){auto o=element(list,i);AnimatAction v;v.sequence=read_uid(o,"sequence");READ(time);
        v.kind=AnimatActionKind(integer(o,"kind"));READINT(stage);v.automatic=boolean(o,"automatic");v.learning=boolean(o,"learning");v.clear_traces=boolean(o,"clear_traces");READ(learning_rate);v.environment=read_animat_environment(object(o,"environment",false));
        if(v.time<last||!v.sequence||!ids.insert(v.sequence).second)throw std::runtime_error("Animat: unordered or duplicate action");last=v.time;out.push_back(std::move(v));}
    return out;
}
