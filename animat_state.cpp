#include "animat.h"
#include "io.h"
#include "archive_codec.h"
#include <algorithm>
#include <set>
namespace studio {
namespace {
using archive_codec::Writer;
using archive_codec::Reader;
void require(bool value,const char* message){if(!value)throw std::runtime_error(std::string("Animat state: ")+message);}
size_t count(Reader& r,size_t maximum,size_t bytes) {
    auto n=r.u();require(n<=maximum&&n<=(r.bytes.size()-r.pos)/bytes,"count exceeds budget or remaining bytes");return size_t(n);
}
void row_write(Writer& w,const AnimatRow& r) {
    for(double v:{r.time,r.x0,r.y0,r.x,r.y,r.vx,r.vy,r.requested_x,r.requested_y,r.frequency,r.requested_frequency,r.apply_time,
        r.amplitude,r.start,r.stop,r.width,r.first_delay,r.radius,r.learning_rate})w.d(v);
    w.u(r.zone);w.u(r.environment);w.u(r.stage_run);w.u(r.stage+1);w.u(uint64_t(r.kind));w.u(r.enabled);w.u(r.learning);w.u(r.marker);
}
AnimatRow row_read(Reader& r) {
    AnimatRow v;for(double* x:{&v.time,&v.x0,&v.y0,&v.x,&v.y,&v.vx,&v.vy,&v.requested_x,&v.requested_y,&v.frequency,
        &v.requested_frequency,&v.apply_time,&v.amplitude,&v.start,&v.stop,&v.width,&v.first_delay,&v.radius,&v.learning_rate})*x=r.d();
    v.zone=r.u();v.environment=r.u();v.stage_run=r.u();auto stage=r.u(),kind=r.u(),enabled=r.u(),learning=r.u(),marker=r.u();
    require(stage<=256&&kind<=5&&enabled<=1&&learning<=1&&marker<=1,"invalid row flags");
    v.stage=int(stage)-1;v.kind=AnimatRowKind(kind);v.enabled=enabled!=0;v.learning=learning!=0;v.marker=marker!=0;return v;
}
}
std::string encode_animat_row(const AnimatRow& r){Writer w;row_write(w,r);return std::move(w.bytes);}
AnimatRow decode_animat_row(const std::string& b){Reader r{b};auto v=row_read(r);r.end();return v;}
std::string serialize_animat(const Simulator& s) {
    const auto& a=s.animat;Writer w;w.text("ANIMAT-STATE-1");
    w.d(a.x);w.d(a.y);w.d(a.last_x);w.d(a.last_y);w.d(a.desired_frequency);
    w.u(a.next_step);w.u(a.period_steps);w.u(a.stage_run);w.u(a.next_action);
    w.text(a.initial_config);w.u(a.saved.size());for(const auto& v:a.saved){w.d(v.time);w.u(v.stage_run);w.u(v.exit);w.u(v.state.size());for(double x:v.state)w.d(x);w.u(v.plasticity.size());for(double x:v.plasticity)w.d(x);}
    w.u(a.environments.size());for(const auto& e:a.environments){w.d(e.time);w.text(animat_environment_json(e.value));}
    w.text(animat_commands_json(a.actions));w.text(animat_commands_json(a.pending));
    w.u(a.rows.size());for(const auto& row:a.rows)row_write(w,row);return std::move(w.bytes);
}
void deserialize_animat(Simulator& s,const std::string& b) {
    Reader r{b};require(r.text(64)=="ANIMAT-STATE-1","unsupported runtime schema");AnimatRuntime a;
    a.x=r.d();a.y=r.d();a.last_x=r.d();a.last_y=r.d();a.desired_frequency=r.d();
    a.next_step=r.u();a.period_steps=r.u();a.stage_run=r.u();a.next_action=r.u();
    a.initial_config=r.text(16*1024*1024);
    size_t total=0;auto saved=count(r,512,40);for(size_t i=0;i<saved;++i){AnimatSavedState v;v.time=r.d();v.stage_run=r.u();auto flag=r.u();require(flag<=1,"invalid result flag");v.exit=flag!=0;auto n=count(r,MaxState,8);total+=n;require(total<=8000000,"saved result budget exceeded");for(size_t k=0;k<n;++k)v.state.push_back(r.d());n=count(r,MaxSynapses*4,8);total+=n;require(total<=8000000,"saved result budget exceeded");for(size_t k=0;k<n;++k)v.plasticity.push_back(r.d());a.saved.push_back(std::move(v));}
    auto n=count(r,MaxAnimatActions,16);
    for(size_t i=0;i<n;++i){const double at=r.d();auto json=r.text(65536);a.environments.push_back({at,animat_environment_from_json(json)});}
    a.actions=read_animat_commands(r.text(16*1024*1024));a.pending=read_animat_commands(r.text(16*1024*1024));
    n=count(r,MaxAnimatRows,216);a.rows.reserve(n);for(size_t i=0;i<n;++i)a.rows.push_back(row_read(r));
    r.end();s.animat=std::move(a);validate_animat_runtime(s);
}
void validate_animat_runtime(const Simulator& s) {
    const auto& a=s.animat;const auto& c=s.config;
    require(a.rows.size()<=MaxAnimatRows&&a.environments.size()<=MaxAnimatActions&&a.actions.size()+a.pending.size()<=MaxAnimatActions,"runtime exceeds limits");
    if(!c.animat.enabled){require(a.rows.empty()&&a.environments.empty()&&a.pending.empty()&&a.actions.empty(),"disabled controller has active history");return;}
    require(a.period_steps>0&&a.next_step>=a.period_steps&&a.next_step%a.period_steps==0&&(!s.error.empty()||(a.next_step>s.steps&&a.next_step-a.period_steps<=s.steps)),"next tick inconsistent with completed steps");
    require(std::isfinite(a.last_x)&&std::isfinite(a.last_y)&&std::isfinite(a.desired_frequency)&&a.desired_frequency>=0&&a.desired_frequency<=200,"invalid controller memory");
    const auto initial=deserialize(a.initial_config);require(initial.animat.enabled&&initial.neurons.size()==c.neurons.size()&&initial.synapses.size()==c.synapses.size()&&initial.animat.stimulator==c.animat.stimulator,"invalid initial configuration");
    for(const auto& source:c.stimulators)if(source.id==c.animat.stimulator)require(source.x==a.x&&source.y==a.y,"source/robot positions differ");
    require(a.x>=0&&a.y>=0&&a.x<=c.space.width&&a.y<=c.space.height&&std::isfinite(a.x)&&std::isfinite(a.y),"position outside arena");
    require(!a.rows.empty()&&!a.environments.empty()&&a.environments.front().time==0&&a.rows.front().time==0,"missing initial context");
    double at=0;for(const auto& e:a.environments){require(e.time>=at&&e.time<=s.time&&validate_animat_environment(c,e.value).empty(),"invalid environment history");at=e.time;}
    at=0;for(const auto& r:a.rows) {
        for(double v:{r.time,r.x0,r.y0,r.x,r.y,r.vx,r.vy,r.requested_x,r.requested_y,r.frequency,r.requested_frequency,
            r.apply_time,r.amplitude,r.start,r.stop,r.width,r.first_delay,r.radius,r.learning_rate})require(std::isfinite(v),"non-finite row");
        require(r.time>=at&&r.time<=s.time&&r.x>=0&&r.x<=c.space.width&&r.y>=0&&r.y<=c.space.height&&
            r.x0>=0&&r.x0<=c.space.width&&r.y0>=0&&r.y0<=c.space.height&&r.environment<a.environments.size()&&r.stage_run<=a.stage_run&&
            r.stage>=-1&&r.stage<int(c.stages.size())&&int(r.kind)>=0&&int(r.kind)<=5,"invalid trajectory");
        require(r.frequency>=0&&r.frequency<=200&&r.requested_frequency>=0&&r.requested_frequency<=200&&r.apply_time>=r.time&&
            r.width>=0&&r.width<=1&&r.amplitude>=-30&&r.amplitude<=100&&r.radius>=0&&r.learning_rate>=0&&r.learning_rate<=100,"invalid source history");
        require(r.zone==arena_zone(c.space,a.environments[r.environment].value,r.x,r.y),"zone/position mismatch");
        at=r.time;
    }
    for(const auto& v:a.saved){for(double x:v.state)require(std::isfinite(x),"non-finite saved state");for(double x:v.plasticity)require(std::isfinite(x),"non-finite saved learning");require(v.time>=0&&v.time<=s.time&&v.stage_run<=a.stage_run&&v.state.size()==s.state.size()&&v.plasticity.size()==4*s.plasticity.size(),"invalid saved stage result");}
    require(a.rows.back().x==a.x&&a.rows.back().y==a.y,"position/history mismatch");
    std::set<uint64_t> ids;
    for(int pending=0;pending<2;++pending) {
        at=0;
        for(const auto& v:pending?a.pending:a.actions) {
            require(v.sequence>0&&v.sequence<a.next_action&&ids.insert(v.sequence).second&&v.time>=at&&
                (pending?v.time>s.time:v.time<=s.time)&&int(v.kind)>=0&&int(v.kind)<=4,"invalid queued/logged action");
            if(v.kind==AnimatActionKind::Environment)require(validate_animat_environment(c,v.environment).empty(),"invalid action environment");
            if(v.kind==AnimatActionKind::Stage)require(v.stage>=0&&v.stage<int(c.stages.size()),"invalid action stage");
            at=v.time;
        }
    }
}
}
