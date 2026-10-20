#include "animat.h"
#include "io.h"
#include <limits>
#include "activity_field.h"
#include "history_archive.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace studio {
void save_animat_stage_result(Simulator& s,bool exit) {
    if(!s.config.animat.enabled)return;
    size_t count=s.state.size()+4*s.plasticity.size();
    for(const auto& v:s.animat.saved)count+=v.state.size()+v.plasticity.size();
    if(s.animat.saved.size()>=512||count>8000000)throw std::runtime_error("Animat: saved stage result budget (8 million doubles / 512 snapshots) exceeded");
    AnimatSavedState v;v.time=s.time;v.stage_run=s.animat.stage_run;v.exit=exit;v.state=s.state;
    for(const auto& p:s.plasticity)for(double x:{p.weight,p.pre_trace,p.post_trace,p.post_slow_trace})v.plasticity.push_back(x);
    s.animat.saved.push_back(std::move(v));
}
namespace {
bool range(double x,double lo,double hi){return std::isfinite(x)&&x>=lo&&x<=hi;}
const Stimulator& source(const Config& c) {
    auto it=std::find_if(c.stimulators.begin(),c.stimulators.end(),[&](const auto& s){return s.id==c.animat.stimulator;});
    if(it==c.stimulators.end())throw std::runtime_error("Animat: linked stimulator is missing");
    return *it;
}
uint64_t period_steps(const Config& c) {
    const double ratio=c.animat.interval/c.dt;
    const double nearest=std::round(ratio);
    // A decimal 20 ms / 25 us need not divide exactly in binary64.
    const double n=std::abs(ratio-nearest)<=8*std::numeric_limits<double>::epsilon()*std::max(1.,ratio)?nearest:std::ceil(ratio);
    if(!range(n,1,1e12))throw std::runtime_error("Animat: invalid controller interval");
    return uint64_t(n);
}
}
std::string validate_animat_environment(const Config& c,const AnimatEnvironment& e) {
    if(e.zones.size()>MaxAnimatZones||!range(e.outside_frequency,0,200)||!range(e.amplitude,-30,100)||
       !range(e.width,0,1)||!range(e.radius,0,std::hypot(c.space.width,c.space.height)))
        return "Animat: invalid environment/stimulation parameters";
    std::set<uint64_t> ids;
    for(const auto& z:e.zones) {
        if(!z.id||z.id==UINT64_MAX||!ids.insert(z.id).second||z.name.empty()||z.name.size()>128||
           z.name.find_first_of("\r\n\t,")!=std::string::npos||int(z.shape)<0||int(z.shape)>1||
           z.priority<0||z.priority>100000||!range(z.frequency,0,200)||
           !range(z.x,0,c.space.width)||!range(z.y,0,c.space.height)||
           !range(z.width,1e-12,2*std::max(c.space.width,c.space.height))||
           !range(z.height,1e-12,2*std::max(c.space.width,c.space.height)))
            return "Animat: invalid zone geometry, name, priority or ID";
        if(z.shape==ZoneShape::Rectangle&&(z.x+z.width>c.space.width||z.y+z.height>c.space.height))
            return "Animat: rectangle must be inside the arena";
        if(z.shape==ZoneShape::Circle&&(z.x-z.width<0||z.y-z.width<0||z.x+z.width>c.space.width||z.y+z.width>c.space.height))
            return "Animat: circle (width = radius) must be inside the arena";
    }
    return {};
}
std::string validate_animat(const Config& c) {
    const auto& a=c.animat;
    if(!a.enabled)return {};
    const double end_steps=c.duration/c.dt;
    if(!std::isfinite(end_steps)||std::abs(end_steps-std::round(end_steps))>8*std::numeric_limits<double>::epsilon()*std::max(1.,end_steps))
        return "Animat: final time must lie on a whole dt boundary (including extensions)";
    if(!range(a.initial_x,0,c.space.width)||!range(a.initial_y,0,c.space.height)||
       !range(a.interval,c.dt,1e6)||!range(a.gain,0,1e6)||!range(a.max_displacement,0,1e9)||
       !range(a.zero_threshold,0,1e6)||int(a.boundary)<0||int(a.boundary)>1||int(a.silence)<0||int(a.silence)>1||
       !valid_topology_view(a.field)||a.field.activity_window>c.activity_horizon)
        return "Animat: invalid controller/field parameters or insufficient activity horizon";
    try {const auto& s=source(c);if(!s.spatial||!s.signal.pulse_intervals.empty())
        return "Animat: select a spatial single-pulse stimulator (tables are not supported by the controller)";period_steps(c);}
    catch(const std::exception& e){return e.what();}
    if(auto e=validate_animat_environment(c,a.environment);!e.empty())return e;
    for(const auto& stage:c.stages) {
        if(stage.animat.position&&(stage.animat.home||!range(stage.animat.x,0,c.space.width)||!range(stage.animat.y,0,c.space.height)))return "Animat: invalid stage position";
        if(std::find(stage.stimulator_ids.begin(),stage.stimulator_ids.end(),a.stimulator)!=stage.stimulator_ids.end())
            return "Animat: configure the linked source through the stage's animat environment";
        if(stage.animat.environment)if(auto e=validate_animat_environment(c,stage.animat.value);!e.empty())return e;
    }
    return {};
}
uint64_t arena_zone(const ModelSpace& arena,const AnimatEnvironment& e,double x,double y) {
    const ArenaZone* selected=nullptr;
    for(const auto& z:e.zones) {
        const bool inside=z.shape==ZoneShape::Circle?std::hypot(x-z.x,y-z.y)<=z.width:
            x>=z.x&&y>=z.y&&(x<z.x+z.width||(x==arena.width&&z.x+z.width==arena.width))&&
            (y<z.y+z.height||(y==arena.height&&z.y+z.height==arena.height));
        if(inside&&(!selected||z.priority>selected->priority||(z.priority==selected->priority&&z.id<selected->id)))selected=&z;
    }
    return selected?selected->id:0;
}
double arena_frequency(const AnimatEnvironment& e,uint64_t id) {
    for(const auto& z:e.zones)if(z.id==id)return z.frequency;
    return e.outside_frequency;
}
AnimatMotion animat_motion(const ModelSpace& arena,const AnimatConfig& a,double x,double y,double vx,double vy,double last_x,double last_y) {
    // The field direction is normalized-plane geometry. Apply the Jacobian
    // diag(width,height) before the physical displacement norm/cap.
    double dx=a.gain*vx*arena.width,dy=a.gain*vy*arena.height;
    if(std::hypot(vx,vy)<=a.zero_threshold) {
        dx=a.silence==AnimatSilence::RepeatLast?last_x:0;
        dy=a.silence==AnimatSilence::RepeatLast?last_y:0;
    }
    const double length=std::hypot(dx,dy);
    if(!std::isfinite(length))throw std::runtime_error("Animat: non-finite requested displacement");
    if(length>a.max_displacement&&length>0){dx*=a.max_displacement/length;dy*=a.max_displacement/length;}
    AnimatMotion result{x+dx,y+dy,dx,dy};
    if(a.boundary==AnimatBoundary::Clamp) {
        result.x=std::clamp(result.x,0.,arena.width);result.y=std::clamp(result.y,0.,arena.height);
    } else if(result.x<0||result.y<0||result.x>arena.width||result.y>arena.height) {result.x=x;result.y=y;}
    return result;
}
void Simulator::animat_row(AnimatRow row) {
    if(animat.rows.size()>=MaxAnimatRows)throw std::runtime_error("Animat: full scientific trajectory reached 1000000 rows; save and end this experiment");
    const auto& e=animat.environments.back().value;const auto& s=source(config);
    row.time=time;row.x=animat.x;row.y=animat.y;row.environment=animat.environments.size()-1;
    row.zone=arena_zone(config.space,e,row.x,row.y);row.stage=experiment.stage_index;row.stage_run=animat.stage_run;
    row.learning=experiment.learning;row.learning_rate=experiment.learning_rate;
    row.frequency=s.signal.frequency;row.requested_frequency=arena_frequency(e,row.zone);row.apply_time=time;
    for(const auto& command:stimulus_commands)if(command.controller&&command.value.id==s.id)row.apply_time=command.time;
    row.amplitude=s.signal.amplitude;row.start=s.signal.start;row.stop=s.signal.stop;row.width=s.signal.width;
    row.first_delay=s.first_delay;row.enabled=s.enabled;row.radius=s.radius;
    animat.rows.push_back(row);
    if(archive)archive->animat(row);
}
void Simulator::initialize_animat() {
    if(!config.animat.enabled)return;
    animat.initial_config=serialize(config);
    animat.x=config.animat.initial_x;animat.y=config.animat.initial_y;
    animat.desired_frequency=source(config).signal.frequency;
    animat.period_steps=period_steps(config);animat.next_step=animat.period_steps;
    animat.environments.push_back({0,config.animat.environment});
    animat_source();
    AnimatRow row;row.x0=animat.x;row.y0=animat.y;animat_row(row);
}
void Simulator::animat_source() {
    struct Guard {bool& flag;Guard(bool& f):flag(f){flag=true;}~Guard(){flag=false;}} guard(animat_updating);
    auto s=source(config);const auto& e=animat.environments.back().value;
    const auto zone=arena_zone(config.space,e,animat.x,animat.y);
    const double frequency=arena_frequency(e,zone);
    // Movement is immediate; frequency uses the existing phase scheduler.
    // A newer zone replaces an older unexecuted controller frequency request.
    const bool changed=frequency!=animat.desired_frequency;
    if(changed)stimulus_commands.erase(std::remove_if(stimulus_commands.begin(),stimulus_commands.end(),
        [&](const auto& c){return c.controller&&c.value.id==s.id;}),stimulus_commands.end());
    animat.desired_frequency=frequency;
    s.x=animat.x;s.y=animat.y;s.radius=e.radius;s.enabled=e.enabled;
    s.signal.amplitude=e.amplitude;s.signal.width=e.width;
    const auto& old=source(config);
    if(s.x!=old.x||s.y!=old.y||s.radius!=old.radius||s.enabled!=old.enabled||
       s.signal.amplitude!=old.signal.amplitude||s.signal.width!=old.signal.width)
        schedule_stimulator(s,time,false,true);
    if(changed&&s.signal.frequency!=frequency){s=source(config);s.signal.frequency=frequency;schedule_stimulator(s,time,false,true);}
}
void Simulator::animat_stage(const AnimatStage& stage) {
    ++animat.stage_run;
    if(stage.environment) {
        if(animat.environments.size()>=MaxAnimatActions)throw std::runtime_error("Animat: environment history is full");
        animat.environments.push_back({time,stage.value});
        if(archive)archive->animat_environment(time,animat_environment_json(stage.value),animat.environments.size()-1);
    }
    AnimatRow row;row.kind=AnimatRowKind::Stage;row.x0=animat.x;row.y0=animat.y;row.marker=stage.marker;
    if(stage.home||stage.position){animat.x=stage.position?stage.x:config.animat.initial_x;animat.y=stage.position?stage.y:config.animat.initial_y;animat.last_x=animat.last_y=0;}
    animat_source();animat_row(row);
    if(stage.marker)save_animat_stage_result(*this,false);
}
void Simulator::apply_animat_action(const AnimatAction& action) {
    if(action.kind==AnimatActionKind::Continue){continue_current();return;}
    if(action.kind==AnimatActionKind::Learning){set_learning(action.learning,action.learning_rate,action.clear_traces);AnimatRow r;r.kind=AnimatRowKind::Stage;r.x0=animat.x;r.y0=animat.y;animat_row(r);return;}
    if(action.kind==AnimatActionKind::Stage){enter_stage(size_t(action.stage),action.automatic);return;}
    AnimatRow row;row.x0=animat.x;row.y0=animat.y;
    if(action.kind==AnimatActionKind::Home) {
        row.kind=AnimatRowKind::Home;animat.x=config.animat.initial_x;animat.y=config.animat.initial_y;animat.last_x=animat.last_y=0;
    } else {
        row.kind=AnimatRowKind::Environment;
        if(animat.environments.size()>=MaxAnimatActions)throw std::runtime_error("Animat: environment history is full");
        animat.environments.push_back({time,action.environment});
        if(archive)archive->animat_environment(time,animat_environment_json(action.environment),animat.environments.size()-1);
    }
    animat_source();animat_row(row);
}
void Simulator::schedule_animat(AnimatAction action) {
    if(stepping||!checkpoint_boundary||!error.empty()||!config.animat.enabled)throw std::runtime_error("Animat: action requires a safe enabled experiment");
    if(!range(action.time,time,1e9)||animat.actions.size()+animat.pending.size()>=MaxAnimatActions||animat.next_action==UINT64_MAX)
        throw std::runtime_error("Animat: invalid time or full action log");
    if(int(action.kind)<0||int(action.kind)>4)throw std::runtime_error("Animat: unknown action");
    if(action.kind==AnimatActionKind::Environment) {
        auto issue=validate_animat_environment(config,action.environment);if(!issue.empty())throw std::runtime_error(issue);
    }
    if(action.kind==AnimatActionKind::Stage&&(action.stage<0||action.stage>=int(config.stages.size())))throw std::runtime_error("Animat: missing stage");
    // Same safe-boundary convention as stages. Store accepted absolute time.
    if(action.time!=time) {
        const double nearest=std::round(action.time/config.dt);
        if(nearest*config.dt!=action.time){const double requested=action.time;const double step=std::ceil(requested/config.dt);
            action.time=step*config.dt;if(action.time<requested)action.time=(step+1)*config.dt;}
    }
    if(!range(action.learning_rate,0,100))throw std::runtime_error("Animat: invalid learning rate");
    if(action.sequence){if(action.sequence==UINT64_MAX)throw std::runtime_error("Animat action ID overflow");for(const auto& v:animat.actions)if(v.sequence==action.sequence)throw std::runtime_error("Animat duplicate action ID");for(const auto& v:animat.pending)if(v.sequence==action.sequence)throw std::runtime_error("Animat duplicate pending ID");animat.next_action=std::max(animat.next_action,action.sequence+1);}
    else action.sequence=animat.next_action++;
    animat.pending.push_back(action);
    std::sort(animat.pending.begin(),animat.pending.end(),[](const auto& a,const auto& b){return a.time!=b.time?a.time<b.time:a.sequence<b.sequence;});
    // An immediate command is applied now, but never performs a controller tick.
    while(!animat.pending.empty()&&animat.pending.front().time<=time) {
        auto v=animat.pending.front();animat.pending.erase(animat.pending.begin());apply_animat_action(v);animat.actions.push_back(v);
    }
}
void Simulator::replay_animat(std::vector<AnimatAction> commands) {
    // Validate every payload and the aggregate budget before scheduling any action.
    if(animat.actions.size()+animat.pending.size()+commands.size()>MaxAnimatActions)throw std::runtime_error("Animat: replay budget exceeded");
    std::set<uint64_t> ids;for(const auto& v:animat.actions)ids.insert(v.sequence);for(const auto& v:animat.pending)ids.insert(v.sequence);
    for(const auto& a:commands) {
        if(!a.sequence||a.sequence==UINT64_MAX||!ids.insert(a.sequence).second||!range(a.learning_rate,0,100))throw std::runtime_error("Animat replay contains invalid/duplicate ID or learning rate");
        if(!range(a.time,time,1e9)||int(a.kind)<0||int(a.kind)>4)throw std::runtime_error("Animat: invalid replay time/action");
        if(a.kind==AnimatActionKind::Environment){auto e=validate_animat_environment(config,a.environment);if(!e.empty())throw std::runtime_error(e);}
        if(a.kind==AnimatActionKind::Stage&&(a.stage<0||a.stage>=int(config.stages.size())))throw std::runtime_error("Animat: invalid replay stage");
    }
    for(auto a:commands)schedule_animat(a);
}
void Simulator::animat_boundary() {
    if(!config.animat.enabled)return;
    if(steps>=animat.next_step) {
    if(steps!=animat.next_step||UINT64_MAX-animat.next_step<animat.period_steps)throw std::runtime_error("Animat: missed controller tick or clock overflow");
    // Use the same field implementation as the display. Rebuilding disposable
    // caches from the retained event window makes checkpoint continuation exact.
    WeightField geometry;ActivityField field;auto settings=config.animat.field;settings.current=true;
    geometry.update(config,&plasticity,settings);
    field.update(config,plasticity,activity,time,this,0,settings,geometry);
    if(!field.available)throw std::runtime_error("Animat: activity window is incomplete; increase retained horizon or reduce event load");
    const auto v=field.vector_at(config,animat.x,animat.y);
    const auto motion=animat_motion(config.space,config.animat,animat.x,animat.y,v.x,v.y,animat.last_x,animat.last_y);
    AnimatRow row;row.kind=AnimatRowKind::Tick;row.x0=animat.x;row.y0=animat.y;row.vx=v.x;row.vy=v.y;
    row.requested_x=motion.requested_x;row.requested_y=motion.requested_y;
    if(std::hypot(v.x,v.y)>config.animat.zero_threshold){animat.last_x=motion.requested_x;animat.last_y=motion.requested_y;}
    animat.x=motion.x;animat.y=motion.y;
    animat.next_step+=animat.period_steps;
    animat_source();animat_row(row);
    }
    while(!animat.pending.empty()&&animat.pending.front().time<=time) {
        auto a=animat.pending.front();animat.pending.erase(animat.pending.begin());
        if(a.time!=time)throw std::runtime_error("Animat: missed an action boundary");
        apply_animat_action(a);animat.actions.push_back(a);
    }
}
double animat_input(const Config& c,const AnimatRuntime& a,int neuron,double at) {
    if(a.rows.empty())return 0;
    auto it=std::upper_bound(a.rows.begin(),a.rows.end(),at,[](double t,const auto& r){return t<r.time;});
    if(it==a.rows.begin())return std::numeric_limits<double>::quiet_NaN();
    const auto& r=*--it;auto s=source(c);
    s.x=r.x;s.y=r.y;s.radius=r.radius;s.enabled=r.enabled;s.signal.amplitude=r.amplitude;s.signal.frequency=r.frequency;
    s.signal.start=r.start;s.signal.stop=r.stop;s.signal.width=r.width;s.first_delay=r.first_delay;
    const auto recipients=stimulator_recipients(c,s);
    return std::find(recipients.begin(),recipients.end(),neuron)!=recipients.end()?pulse_current(stimulator_signal(s),at):0;
}
}
