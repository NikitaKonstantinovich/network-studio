#include "io.h"
#include "analysis.h"
#include "history_archive.h"
#include "weight_analysis.h"
#include "animat.h"
#include "archive_codec.h"
#include <iostream>
#include <iomanip>
#include <optional>
#include <chrono>
namespace studio { bool package_cli(int argc,char** argv,int& exit_code); }
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
static double parse_number(const std::string& text) {
    size_t used=0;double value=std::stod(text,&used);
    if(used!=text.size())throw std::runtime_error("Invalid numeric argument: "+text);return value;
}
static int run_cli(int argc,char** argv) {
    try {
        int package_exit=0;if(studio::package_cli(argc,argv,package_exit))return package_exit;
        int weights_exit=0;if(studio::weights_cli(argc,argv,weights_exit))return weights_exit;
        int analysis_exit=0;if(studio::analysis_cli(argc,argv,analysis_exit))return analysis_exit;
        studio::Config c=studio::preset(0);std::filesystem::path output="experiment",save,resume,checkpoint;
        bool config_options=false,continue_current=false;std::filesystem::path plan,trained,stimulus_replay,animat_replay;std::string branch_origin;
        std::optional<int> stage,sequence;std::optional<bool> learning;std::optional<double> learning_rate;
        std::optional<double> until,stop,run_for;double animat_from=0,animat_to=-1;
        int compute_threads=1;std::filesystem::path archive;bool archive_append=false,archive_deliveries=false;size_t archive_rows=10000;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") {
                std::cout<<"Animat: --animat-commands file.json; --animat-from seconds --animat-to seconds (statistics; full trajectory always exported)\n";
                std::cout<<"Experiment packages: python experiment_package/package.py --help\n"
                    "  --package-build-info; --package-probe --config file|--resume file [--commands file --profile file --until end --threads mode --output NEW.json]\n"
                    "  --package-records --archive directory --output NEW.jsonl (this segment only; no simulation)\n";
                std::cout<<"Weights: --weights-initial config.json | --weights-from-summary PREFIX_summary.json | --weights-from-checkpoint file.nscp --output file.nsweights\n"
                    "Weights analysis: --weights-analyze --snapshot file.nsweights [--snapshot another.nsweights] --output prefix [--representation signed|absolute --polarity all|E|I --model all|stp|exp|tm --enabled all|on|off --bins 40 --tolerance 1e-6 --reference 0 --target 1]\n";
                std::cout<<"network_studio_cli --analyze-export prefix --analysis-profile file.json --analysis-from seconds --analysis-to seconds --output separate_prefix\n"
                    "network_studio_cli --compare-analysis first_analysis.json second_analysis.json\n"
                    "network_studio_cli [--config file.json | --preset 0..7 | --resume file.nscp]\n"
                    "  [--duration seconds] [--dt seconds] [--sample seconds] [--save file.json]\n"
                    "  [--method adaptive-rk4|rk4|euler] (default adaptive-rk4; nonlinear portion only)\n"
                    "  [--until absolute_end] [--stop-at absolute_time | --run-for seconds]\n"
                    "  [--checkpoint file.nscp] [--output prefix]\n"
                    "  [--archive new_directory | --archive-append directory] [--archive-deliveries yes|no] [--archive-rows 2..1000000]\n"
                    "  --inspect-archive directory (committed range, completion and interrupted tail)\n"
                    "  --analyze-archive directory --analysis-profile file --analysis-from seconds --analysis-to seconds --output prefix\n"
                    "  --export-archive directory --analysis-from seconds --analysis-to seconds --output NEW_prefix\n"
                    "  [--threads "<<studio::compute::choices()<<"] (runtime setting; default 1)\n"
                    "  [--stimulus-commands file.json] (recorded absolute model times)\n"
                    "  [--plan config_with_stages.json] [--stage zero_based | --sequence zero_based]\n"
                    "  [--learning enabled|frozen] [--learning-rate factor] [--fork provenance]\n"
                    "  [--export-trained new_initial_config.json] [--continue-current yes]\n"
                    "Stage durations are relative seconds rounded upward to a completed macro step.\n"
                    "Stage controls preserve state; --plan imports only stages and analysis settings.\n"
                    "All times are seconds. --save writes only the initial configuration.\n"
                    "--dt is the maximum main step: 1..500 us for Izhikevich-only/no-astro networks,\n"
                    "otherwise 1..50 us. Sample/noise intervals must remain >= dt (no automatic changes).\n"
                    "RK4/Euler require Izhikevich only, no astrocytes; exact linear transitions and kinetic/event limits remain active.\n"
                    "--resume restores full state; config/method/dt/sample/duration/--save overrides are forbidden.\n"
                    "--until extends a restored final time (absolute from t=0).\n"
                    "--stop-at / --run-for stop at the first completed step at or after the target,\n"
                    "within the configured final time. --run-for is relative to restored/start time.\n"
                    "--checkpoint writes full dynamic state, retained RAM history and archive anchor (keep the archive directories).\n"
                    "JSON synapses: delay in seconds; stdp_timing = arrival (default) or emission.\n"
                    "Exports: _events.csv contains emissions, _deliveries.csv actual arrivals,\n"
                    "_pending_deliveries.csv includes arrivals beyond the current final time.\n";return 0;
            }
            if(i+1>=argc)throw std::runtime_error("Missing option value.");std::string value=argv[++i];
            if(arg=="--config"){c=studio::load_config(std::filesystem::u8path(value));config_options=true;}
            else if(arg=="--preset") {double index=parse_number(value);if(!std::isfinite(index)||index<0||index>7||index!=std::floor(index))throw std::runtime_error("Preset must be 0..7.");c=studio::preset(int(index));config_options=true;}
            else if(arg=="--duration"){c.duration=parse_number(value);config_options=true;}
            else if(arg=="--dt"){c.dt=parse_number(value);config_options=true;}
            else if(arg=="--method"){c.integration_method=studio::parse_integration_method(value);config_options=true;}
            else if(arg=="--sample"){c.sample=parse_number(value);config_options=true;}
            else if(arg=="--until")until=parse_number(value);
            else if(arg=="--stop-at")stop=parse_number(value);
            else if(arg=="--run-for")run_for=parse_number(value);
            else if(arg=="--threads")compute_threads=studio::compute::parse(std::string_view(value),"--threads");
            else if(arg=="--archive"||arg=="--archive-append"){if(!archive.empty())throw std::runtime_error("Choose one archive operation");archive=std::filesystem::u8path(value);archive_append=arg=="--archive-append";}
            else if(arg=="--archive-deliveries"){if(value!="yes"&&value!="no")throw std::runtime_error("Archive deliveries: yes or no");archive_deliveries=value=="yes";}
            else if(arg=="--archive-rows"){double n=parse_number(value);if(!std::isfinite(n)||n<2||n>studio::MaxSamples||n!=std::floor(n))throw std::runtime_error("Archive rows must be 2..1000000");archive_rows=size_t(n);}
            else if(arg=="--resume")resume=std::filesystem::u8path(value);
            else if(arg=="--checkpoint")checkpoint=std::filesystem::u8path(value);
            else if(arg=="--save")save=std::filesystem::u8path(value);
            else if(arg=="--continue-current"){if(value!="yes")throw std::runtime_error("Use --continue-current yes");continue_current=true;}
            else if(arg=="--animat-commands")animat_replay=std::filesystem::u8path(value);
            else if(arg=="--animat-from")animat_from=parse_number(value);
            else if(arg=="--animat-to")animat_to=parse_number(value);
            else if(arg=="--stimulus-commands")stimulus_replay=std::filesystem::u8path(value);
            else if(arg=="--plan")plan=std::filesystem::u8path(value);
            else if(arg=="--stage"||arg=="--sequence") {double n=parse_number(value);if(n<0||n>255||n!=std::floor(n))throw std::runtime_error("Stage index must be 0..255");(arg=="--stage"?stage:sequence)=int(n);}
            else if(arg=="--learning"){if(value!="enabled"&&value!="frozen")throw std::runtime_error("Learning: enabled or frozen");learning=value=="enabled";}
            else if(arg=="--learning-rate")learning_rate=parse_number(value);
            else if(arg=="--fork")branch_origin=value;
            else if(arg=="--export-trained")trained=std::filesystem::u8path(value);
            else if(arg=="--output")output=std::filesystem::u8path(value);
            else throw std::runtime_error("Unknown option: "+arg);
        }
        if(!resume.empty() && (config_options || !save.empty()))throw std::runtime_error("A checkpoint cannot be combined with configuration overrides or --save; use --until to extend.");
        if(stop && run_for)throw std::runtime_error("Choose --stop-at or --run-for.");
        if(!save.empty()) {
            if(!checkpoint.empty() || stop || run_for || until)throw std::runtime_error("--save cannot be combined with execution/checkpoint options.");
            studio::save_config(c,save);std::cout<<"Configuration saved.\n";return 0;
        }
        if(resume.empty() && until)c.duration=*until;
        if(resume.empty()&&!archive.empty()){c.history_capacity=archive_rows;c.bounded_journals=true;c.journal_capacity=50000;}
        auto simulator=resume.empty()?std::make_unique<studio::Simulator>(c):studio::load_checkpoint(resume);
        auto& s=*simulator;
        s.set_compute_threads(compute_threads);
        if(!s.error.empty())throw std::runtime_error("Snapshot/configuration is not resumable: "+s.error);
        if(!archive.empty())studio::start_archive(s,archive,archive_deliveries,archive_append);
        if(!resume.empty() && until)s.extend_duration(*until);
        if(continue_current){if(s.config.animat.enabled){studio::AnimatAction a;a.time=s.time;a.kind=studio::AnimatActionKind::Continue;s.schedule_animat(a);}else s.continue_current();}
        if(stage&&sequence)throw std::runtime_error("Choose --stage or --sequence");
        if(!plan.empty()){auto p=studio::load_config(plan);auto candidate=s.config;candidate.stages=p.stages;candidate.analysis=p.analysis;if(s.config.animat.enabled&&s.steps&&studio::serialize(candidate)!=studio::serialize(s.config))throw std::runtime_error("Animat stage plan is fixed after the first step");auto err=studio::validate(candidate);if(!err.empty())throw std::runtime_error(err);s.config.stages=p.stages;s.config.analysis=p.analysis;if(s.config.animat.enabled&&!s.steps)s.animat.initial_config=studio::serialize(s.config);}
        if(!branch_origin.empty())s.fork_experiment(branch_origin);
        if(stage||sequence){if(s.config.animat.enabled){studio::AnimatAction a;a.time=s.time;a.kind=studio::AnimatActionKind::Stage;a.stage=stage?*stage:*sequence;a.automatic=bool(sequence);s.schedule_animat(a);}else s.enter_stage(stage?*stage:*sequence,bool(sequence));}
        if(learning||learning_rate){if(s.config.animat.enabled){studio::AnimatAction a;a.time=s.time;a.kind=studio::AnimatActionKind::Learning;a.learning=learning.value_or(s.experiment.learning);a.learning_rate=learning_rate.value_or(s.experiment.learning_rate);s.schedule_animat(a);}else s.set_learning(learning.value_or(s.experiment.learning),learning_rate.value_or(s.experiment.learning_rate));}
        if(!stimulus_replay.empty())studio::load_stimulator_replay(s,stimulus_replay);
        if(!animat_replay.empty())s.replay_animat(studio::read_animat_commands(studio::archive_codec::read(animat_replay,16*1024*1024)));
        double target=s.config.duration;
        if(run_for) {
            if(!std::isfinite(*run_for)||*run_for<0)throw std::runtime_error("--run-for must be finite and nonnegative.");
            target=s.time+*run_for;
        } else if(stop)target=*stop;
        if(!std::isfinite(target)||target<s.time||target>s.config.duration)throw std::runtime_error("Stop target must lie between current and final absolute time; use --until to extend.");
        const auto begin=std::chrono::steady_clock::now();
        auto progress=begin;
        while(!s.done() && (!(stop||run_for)||s.time<target)) {
            if(!s.step())break;
            if(s.steps%256==0&&std::chrono::steady_clock::now()-progress>std::chrono::seconds(30)) {
                progress=std::chrono::steady_clock::now();std::cerr<<"Progress t="<<std::setprecision(10)<<s.time<<" s, steps="<<s.steps<<", events_after="<<s.history.events_after<<", deliveries_after="<<s.history.deliveries_after<<"\n";
            }
        }
        std::cout<<"Wall seconds: "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"\n";
        std::cout<<"Compute threads: "<<(compute_threads==0?"auto":std::to_string(compute_threads))<<" / effective "<<s.effective_compute_threads()<<"\n";
        std::cout<<"Integration method: "<<studio::integration_method_info(s.config.integration_method).argument<<"\n";
        std::cout<<std::setprecision(17)<<"Maximum main step (s): "<<s.config.dt
            <<"\nAccepted internal steps: "<<s.integration.accepted
            <<"\nMean internal step (s): "<<s.integration.mean_step(s.time)
            <<"\nRejected integration attempts: "<<s.integration.rejected
            <<"\nError rejections: "<<(s.config.integration_method==studio::IntegrationMethod::AdaptiveRK4?std::to_string(s.integration.error_rejected):"not applicable")
            <<"\nPhysical-state reductions: "<<s.integration.state_reductions<<"\n";
        studio::flush_archive(s);
        if(!trained.empty())studio::save_config(s.learned_config(),trained);
        if(!checkpoint.empty())studio::save_checkpoint(s,checkpoint);
        if(!s.error.empty())throw std::runtime_error(s.error);
        studio::export_results(s,output);
        if(s.config.animat.enabled&&(animat_from!=0||animat_to>=0))studio::export_animat(s,output,animat_from,animat_to);
        std::cout<<std::setprecision(17)<<(s.done()?"Completed ":"Paused ")<<s.time<<" s / "<<s.steps<<" steps / "<<s.history.time.size()<<" samples\n";
        for(size_t i=0;i<s.spikes.size();++i)std::cout<<"N"<<i+1<<": "<<s.spikes[i]<<" spikes\n";
        for(size_t i=0;i<s.releases.size();++i)std::cout<<"A"<<i+1<<": "<<s.releases[i]<<" releases\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
#ifdef _WIN32
static std::string unicode_argument(const wchar_t* value) {
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,nullptr,0,nullptr,nullptr);
    if(size<=0)throw std::runtime_error("Invalid Unicode command-line argument.");
    std::string result(size,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,result.data(),size,nullptr,nullptr))
        throw std::runtime_error("Cannot convert command-line argument to UTF-8.");
    result.pop_back();return result;
}
int wmain(int argc,wchar_t** argv) {
    try {
        std::vector<std::string> values;std::vector<char*> arguments;
        for(int i=0;i<argc;++i)values.push_back(unicode_argument(argv[i]));
        for(auto& value:values)arguments.push_back(value.data());
        return run_cli(argc,arguments.data());
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
#else
int main(int argc,char** argv) {return run_cli(argc,argv);}
#endif
