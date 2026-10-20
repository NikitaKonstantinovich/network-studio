#include "animat.h"
#include "io.h"
#include "checkpoint_build.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <locale>
namespace studio {
namespace {
std::vector<uint64_t> crossed(const ModelSpace& arena,const AnimatEnvironment& env,double x0,double y0,double x1,double y1,
                              std::vector<double>& fractions) {
    const double dx=x1-x0,dy=y1-y0;std::vector<double> cuts{0,1};
    auto add=[&](double t){if(t>0&&t<1&&std::isfinite(t))cuts.push_back(t);};
    for(const auto& z:env.zones) {
        if(z.shape==ZoneShape::Rectangle) {
            if(dx!=0){add((z.x-x0)/dx);add((z.x+z.width-x0)/dx);}
            if(dy!=0){add((z.y-y0)/dy);add((z.y+z.height-y0)/dy);}
        } else {
            const double a=dx*dx+dy*dy,b=2*((x0-z.x)*dx+(y0-z.y)*dy);
            const double cc=(x0-z.x)*(x0-z.x)+(y0-z.y)*(y0-z.y)-z.width*z.width,disc=b*b-4*a*cc;
            if(a>0&&disc>=0){add((-b-std::sqrt(disc))/(2*a));add((-b+std::sqrt(disc))/(2*a));}
        }
    }
    std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
    std::vector<uint64_t> zones{arena_zone(arena,env,x0,y0)};fractions={0};
    auto append=[&](uint64_t zone,double fraction){if(zone!=zones.back()){zones.push_back(zone);fractions.push_back(fraction);}};
    for(size_t k=1;k<cuts.size();++k){double t=(cuts[k-1]+cuts[k])*.5;append(arena_zone(arena,env,x0+dx*t,y0+dy*t),cuts[k-1]);}
    append(arena_zone(arena,env,x1,y1),1);return zones;
}
std::ostringstream stream(){std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17);return o;}
}
AnimatAnalysis analyze_animat(const Config& c,const AnimatRuntime& a,double end,double from,double to) {
    if(!std::isfinite(from)||!std::isfinite(to)||from<0||to<=from)throw std::runtime_error("Animat: analysis requires finite 0 <= from < to");
    AnimatAnalysis result;result.requested_from=from;result.requested_to=to;
    if(a.rows.empty())return result;
    result.from=std::max(from,a.rows.front().time);result.to=std::min(to,end);
    result.complete=from>=a.rows.front().time&&to<=end;
    if(result.to<=result.from)return result;
    std::map<std::pair<uint64_t,uint64_t>,AnimatZoneStatistics> zs;
    std::map<uint64_t,AnimatStageStatistics> ss;
    auto zstat=[&](uint64_t run,uint64_t z)->AnimatZoneStatistics&{auto& v=zs[{run,z}];v.zone=z;v.stage_run=run;return v;};
    // Residence is a zero-order hold. Controller displacements occur
    // instantaneously at their timestamps; there is no retroactive motion.
    for(size_t i=0;i<a.rows.size();++i) {
        const auto& row=a.rows[i];const double stop=i+1<a.rows.size()?a.rows[i+1].time:end;
        const double dt=std::max(0.,std::min(stop,result.to)-std::max(row.time,result.from));
        auto& stage=ss[row.stage_run];stage.run=row.stage_run;stage.stage=row.stage;
        stage.time+=dt;zstat(row.stage_run,row.zone).time+=dt;
        if(row.time<result.from||row.time>=result.to||i==0)continue;
        const auto& previous=a.rows[i-1];
        const bool motion=row.kind==AnimatRowKind::Tick;
        if(motion){double path=std::hypot(row.x-row.x0,row.y-row.y0);result.path+=path;stage.path+=path;}
        std::vector<double> fractions;
        std::vector<uint64_t> zones;
        if(motion)zones=crossed(c.space,a.environments[row.environment].value,row.x0,row.y0,row.x,row.y,fractions);
        else {zones={previous.zone};fractions={0};if(row.zone!=previous.zone){zones.push_back(row.zone);fractions.push_back(1);}}
        for(size_t k=1;k<zones.size();++k){
            result.crossings.push_back({row.time,fractions[k],zones[k-1],zones[k],row.stage_run,!motion});
            ++zstat(row.stage_run,zones[k-1]).exits;++zstat(row.stage_run,zones[k]).entries;
        }
    }
    // Construct complete visits first, including each geometric transit. Then
    // clip them, preserving censoring instead of inventing visits at the query edge.
    uint64_t zone=a.rows.front().zone,run=a.rows.front().stage_run;
    double started=a.rows.front().time;bool left=false;
    auto visit=[&](double stop,bool right) {
        if((stop>result.from&&started<result.to)||(stop==started&&started>=result.from&&started<result.to))
            result.visits.push_back({zone,run,std::max(started,result.from),std::min(stop,result.to),left||started<result.from,right||stop>=result.to});
    };
    for(size_t i=1;i<a.rows.size()&&a.rows[i].time<result.to;++i) {
        const auto& row=a.rows[i];
        if(row.stage_run!=run){visit(row.time,true);run=row.stage_run;started=row.time;left=true;}
        std::vector<double> fractions;std::vector<uint64_t> route;
        if(row.kind==AnimatRowKind::Tick)route=crossed(c.space,a.environments[row.environment].value,row.x0,row.y0,row.x,row.y,fractions);
        else {route={zone};if(row.zone!=zone)route.push_back(row.zone);}
        for(size_t k=1;k<route.size();++k){visit(row.time,false);zone=route[k];started=row.time;left=false;}
    }
    visit(end,true);
    auto it=std::upper_bound(a.rows.begin(),a.rows.end(),result.from,[](double t,const auto& r){return t<r.time;});
    const auto initial_zone=(it==a.rows.begin()?a.rows.front():*--it).zone;
    if(initial_zone)for(const auto& cross:result.crossings)if(cross.time>result.from&&cross.from==initial_zone&&cross.to!=initial_zone){result.first_exit=cross.time-result.from;break;}
    const double duration=result.to-result.from;result.mean_speed=result.path/duration;
    for(auto& [key,v]:zs){v.fraction=duration>0?v.time/duration:0;v.stage_fraction=ss[v.stage_run].time>0?v.time/ss[v.stage_run].time:0;result.zones.push_back(v);}
    for(auto& [key,v]:ss)if(v.time>0||v.path>0){v.mean_speed=v.time>0?v.path/v.time:0;
        for(const auto& visit:result.visits)if(visit.stage_run==v.run&&visit.end>visit.start){if(visit.zone)for(const auto& crossing:result.crossings)if(crossing.stage_run==v.run&&crossing.time>visit.start&&crossing.from==visit.zone){v.first_exit=crossing.time-visit.start;break;}break;}
        result.stages.push_back(v);}
    return result;
}
void export_animat_analysis(const AnimatAnalysis& a,const std::filesystem::path& prefix) {
    auto zones=stream();zones<<"stage_run,zone_id,residence_s,fraction_of_interval,entries,exits,fraction_of_stage\n";
    for(auto r:a.zones)zones<<r.stage_run<<','<<r.zone<<','<<r.time<<','<<r.fraction<<','<<r.entries<<','<<r.exits<<','<<r.stage_fraction<<'\n';
    auto visits=stream();visits<<"stage_run,zone_id,start_s,end_s,duration_s,left_censored,right_censored\n";
    for(auto v:a.visits)visits<<v.stage_run<<','<<v.zone<<','<<v.start<<','<<v.end<<','<<v.end-v.start<<','<<v.left_censored<<','<<v.right_censored<<'\n';
    auto stages=stream();stages<<"stage_run,stage_index_0based,available_s,path,mean_speed,first_exit_s\n";
    for(auto v:a.stages)stages<<v.run<<','<<v.stage<<','<<v.time<<','<<v.path<<','<<v.mean_speed<<','<<v.first_exit<<'\n';
    auto cross=stream();cross<<"time_s,segment_fraction,from_zone,to_zone,stage_run,teleport_or_environment\n";
    for(auto v:a.crossings)cross<<v.time<<','<<v.fraction<<','<<v.from<<','<<v.to<<','<<v.stage_run<<','<<v.teleport<<'\n';
    auto summary=stream();summary<<"{\"requested_from\":"<<a.requested_from<<",\"requested_to\":"<<a.requested_to
        <<",\"available_from\":"<<a.from<<",\"available_to\":"<<a.to<<",\"complete\":"<<(a.complete?"true":"false")
        <<",\"path\":"<<a.path<<",\"mean_speed\":"<<a.mean_speed<<",\"first_exit_s\":"<<a.first_exit<<"}\n";
    auto write=[&](const char* suffix,const std::string& text){auto p=prefix;p+=suffix;write_atomic(p,text);};
    write("_animat_zones.csv",zones.str());write("_animat_visits.csv",visits.str());write("_animat_stages.csv",stages.str());
    write("_animat_crossings.csv",cross.str());write("_animat_analysis.json",summary.str());
}
std::string animat_csv_header(){return "time_s,x,y,zone_id,stage_index_0based,stage_run,environment,kind,vx,vy,requested_dx,requested_dy,actual_dx,actual_dy,frequency_hz,requested_frequency_hz,apply_time_s,amplitude,start_s,stop_s,width_s,first_delay_s,radius,enabled,learning,learning_rate,marker\n";}
std::string animat_csv_row(const AnimatRow& r){auto o=stream();o<<r.time<<','<<r.x<<','<<r.y<<','<<r.zone<<','<<r.stage<<','<<r.stage_run<<','<<r.environment<<','<<int(r.kind)<<','<<r.vx<<','<<r.vy<<','<<r.requested_x<<','<<r.requested_y<<','<<r.x-r.x0<<','<<r.y-r.y0<<','<<r.frequency<<','<<r.requested_frequency<<','<<r.apply_time<<','<<r.amplitude<<','<<r.start<<','<<r.stop<<','<<r.width<<','<<r.first_delay<<','<<r.radius<<','<<r.enabled<<','<<r.learning<<','<<r.learning_rate<<','<<r.marker<<'\n';return o.str();}
void export_animat(const Simulator& s,const std::filesystem::path& prefix,double from,double to) {
    if(!s.config.animat.enabled)return;
    auto o=stream();o<<animat_csv_header();for(const auto& r:s.animat.rows)o<<animat_csv_row(r);
    auto write=[&](const char* suffix,const std::string& text){auto p=prefix;p+=suffix;write_atomic(p,text);};
    write("_animat.csv",o.str());write("_animat_initial.json",s.animat.initial_config);
    auto saved=stream();saved<<"time_s,stage_run,exit,kind,index,value\n";
    for(const auto& v:s.animat.saved){for(size_t i=0;i<v.state.size();++i)saved<<v.time<<','<<v.stage_run<<','<<v.exit<<",state,"<<i<<','<<v.state[i]<<'\n';for(size_t i=0;i<v.plasticity.size();++i)saved<<v.time<<','<<v.stage_run<<','<<v.exit<<",weight_pre_post_slow,"<<i<<','<<v.plasticity[i]<<'\n';}
    write("_animat_stage_states.csv",saved.str());
    auto actions=s.animat.actions;actions.insert(actions.end(),s.animat.pending.begin(),s.animat.pending.end());
    std::sort(actions.begin(),actions.end(),[](auto& a,auto& b){return a.time!=b.time?a.time<b.time:a.sequence<b.sequence;});
    write("_animat_actions.json",animat_commands_json(actions));
    auto meta=stream();meta<<"{\"document_type\":\"network-studio-animat-experiment\",\"schema\":1,\"solver_build\":\""<<STUDIO_CHECKPOINT_BUILD_ID
        <<"\",\"algorithm\":\""<<CheckpointAlgorithm<<"\",\"compute_threads_requested\":"<<s.compute_threads()<<",\"compute_threads_effective\":"<<s.effective_compute_threads()
        <<",\"time\":"<<s.time<<",\"step_period\":"<<s.animat.period_steps<<",\"config\":"<<serialize(s.config)<<",\"environments\":[";
    for(size_t i=0;i<s.animat.environments.size();++i){if(i)meta<<',';const auto& e=s.animat.environments[i];meta<<"{\"time\":"<<e.time<<",\"value\":"<<animat_environment_json(e.value)<<'}';}
    meta<<"]}\n";write("_animat_experiment.json",meta.str());
    if(s.time>from)export_animat_analysis(analyze_animat(s.config,s.animat,s.time,from,to<0?s.time:to),prefix);
}
}
