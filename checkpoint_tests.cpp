#include "io.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace studio;
static int checks=0,roundtrips=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F> static void rejects(F f,const char* message){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,message);}
static std::string bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"test file missing");return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path& p,const std::string& b){std::ofstream f(p,std::ios::binary);f.write(b.data(),b.size());check(bool(f),"test write failed");}
static uint64_t get64(const std::string& s,size_t offset){uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t((unsigned char)s.at(offset+i))<<(8*i);return v;}
static void put64(std::string& s,size_t offset,uint64_t v){for(int i=0;i<8;++i)s.at(offset+i)=char(v>>(8*i));}
static void resign(std::string& s){uint64_t h=14695981039346656037ull;for(size_t i=0;i<s.size()-8;++i){h^=(unsigned char)s[i];h*=1099511628211ull;}put64(s,s.size()-8,h);}
struct Offsets {
    size_t algorithm,build,time,steps,sample,suggested,state,thresholds,rows,events;
    explicit Offsets(const std::string& s) {
        size_t p=26+8;algorithm=p; p+=8+get64(s,p);build=p;p+=8+get64(s,p);
        p+=8+get64(s,p);p+=8+8*get64(s,p);time=p;steps=p+8;sample=p+16;suggested=p+24;p+=48;
        p+=8+get64(s,p);state=p;p+=8+8*get64(s,p);p+=8+32*get64(s,p);
        p+=8+8*get64(s,p);p+=8+8*get64(s,p);thresholds=p;p+=8+8*get64(s,p);
        rows=p;p+=8+8*get64(s,p);
        for(int table=0;table<2;++table){auto columns=get64(s,p);p+=8;for(uint64_t j=0;j<columns;++j)p+=8+8*get64(s,p);}
        events=p;
    }
};
static std::vector<Config> fixtures() {
    std::vector<Config> all;
    for(int rule=0;rule<3;++rule) {
        auto c=preset(6);c.name="checkpoint_stdp_"+std::to_string(rule);c.duration=.035;c.sample=.000175;
        c.seed=4294967295u;c.noise_sigma=.45;c.noise_interval=.000073;
        for(size_t i=0;i<c.neurons.size();++i){auto& n=c.neurons[i];n.current=11+3*double(i);n.start=.00005+double(i)*.000025;n.stop=.026025;n.frequency=200;n.pulse_intervals={{0,.00001},{.002,.002031}};}
        c.neurons[1].individual_noise=true;c.neurons[1].noise_sigma=.7;
        c.synapses[0].stdp_rule=StdpRule(rule);c.synapses[0].model=SynapseModel(rule);
        c.synapses[0].gain=3;c.synapses[0].conductance=.1;
        all.push_back(c);
    }
    auto c=preset(2);c.name="checkpoint_hh_stp_astro";c.duration=.035;c.sample=.000225;
    for(auto& n:c.neurons){n.current=10;n.amplitude=1;n.start=.00005;n.stop=.024025;n.width=.00001;}
    c.parameters[na::V_spike]=0;c.parameters[na::O_P]=0;
    c.noise_sigma=.2;c.noise_interval=.0001;
    for(auto& a:c.astrocytes){a.threshold=.100001;a.ip3=.8;a.sic=.2;}
    all.push_back(c);
    c=all[2];c.name="checkpoint_selective_mixed";c.record_all=false;c.record_neurons={1};c.record_synapses={};
    Neuron hh;hh.name="HH hidden";hh.amplitude=0;hh.current=12;c.neurons.push_back(hh);
    c.astrocytes.push_back(Astrocyte{});c.astrocytes[0].threshold=.100001;c.astrocytes[0].ip3=.8;c.parameters[na::O_P]=0;
    c.synapses[0].astro=0;
    Synapse stp;stp.source=2;stp.target=0;stp.astro=0;c.synapses.push_back(stp);
    c.sample=.00015;all.push_back(c);
    return all;
}
static void finish(Simulator& s){while(!s.done())check(s.step(),s.error.c_str());check(s.error.empty(),s.error.c_str());}
static void identical(const Simulator& a,const Simulator& b,const std::filesystem::path& root) {
    save_checkpoint(a,root/"a.nscp");save_checkpoint(b,root/"b.nscp");
    check(bytes(root/"a.nscp")==bytes(root/"b.nscp"),"checkpoint mismatch: exact equality required");
}
static void roundtrip(std::unique_ptr<Simulator>& s,const std::filesystem::path& root) {
    const auto file=root/std::filesystem::u8path("состояние.nscp");save_checkpoint(*s,file);
    auto restored=load_checkpoint(file);identical(*s,*restored,root);s=std::move(restored);++roundtrips;
}
static void comparison_tests(const std::filesystem::path& root) {
    for(const auto& c:fixtures()) {
        Simulator uninterrupted(c),save_only(c);auto resumed=std::make_unique<Simulator>(c);
        roundtrip(resumed,root);
        uint64_t event_checkpoints=0;
        while(!uninterrupted.done()) {
            const size_t old_events=uninterrupted.history.events.size();
            check(uninterrupted.step(),uninterrupted.error.c_str());
            if(uninterrupted.history.events.size()!=old_events)roundtrip(resumed,root);
            check(save_only.step(),save_only.error.c_str());check(resumed->step(),resumed->error.c_str());
            const auto k=uninterrupted.steps;
            // Before/after initial pulse, pulse stop, noise-bin and sample edges;
            // also the safe steps surrounding EVERY observed event.
            if(k<12 || k%47==0 || uninterrupted.history.events.size()!=old_events || (k>=959 && k<=1043)) {
                save_checkpoint(save_only,root/"save_only.nscp");
                roundtrip(resumed,root);
                if(uninterrupted.history.events.size()!=old_events)++event_checkpoints;
            }
        }
        identical(uninterrupted,*resumed,root);identical(uninterrupted,save_only,root);
        check(event_checkpoints>0,"fixture did not exercise spikes/releases");
        export_results(uninterrupted,root/"continuous");export_results(*resumed,root/"resumed");
        for(const char* suffix:{"_trace.csv","_weights.csv","_events.csv","_summary.json","_experiment.json"})
            check(bytes(root/(std::string("continuous")+suffix))==bytes(root/(std::string("resumed")+suffix)),"export mismatch");
        if(!c.synapses.empty()&&c.synapses[0].stdp)check(uninterrupted.plasticity[0].weight!=c.synapses[0].weight,"learning fixture did not change weights");
    }
}
static void detector_tests(const std::filesystem::path& root) {
    for(bool astro:{false,true}) {
        auto c=fixtures()[3];c.record_all=false;c.record_neurons.clear();c.record_synapses.clear();c.sample=.007;
        auto s=std::make_unique<Simulator>(c);const size_t detector=astro?c.neurons.size():0;
        bool found=false;
        while(!s->done()) {
            check(s->step(),"detector fixture failed");save_checkpoint(*s,root/"detector.nscp");auto blob=bytes(root/"detector.nscp");Offsets o(blob);
            if(get64(blob,o.thresholds+8+8*detector)==0 && (astro?s->releases[0]:s->spikes[0])>0 && s->time!=s->history.time.back()) {found=true;break;}
        }
        check(found,"no disarmed detector found");
        // Just below the threshold but within tolerance, with a rising RHS:
        // reconstruction from value alone would incorrectly arm this detector.
        const int coordinate=astro?astro_offset(c,0)+2:0;double peak=astro?c.astrocytes[0].threshold:c.parameters[na::V_spike];
        s->state[coordinate]=std::nextafter(peak,-std::numeric_limits<double>::infinity());
        // Astrocytes are always recorded, but last sample is earlier than time.
        save_checkpoint(*s,root/"detector.nscp");auto restored=load_checkpoint(root/"detector.nscp");
        auto blob=bytes(root/"detector.nscp");Offsets o(blob);put64(blob,o.thresholds+8+8*detector,1);resign(blob);write(root/"armed.nscp",blob);
        auto wrongly_armed=load_checkpoint(root/"armed.nscp");
        auto count=[&](const Simulator& x){return astro?x.releases[0]:x.spikes[0];};auto old=count(*s);
        check(s->step() && restored->step() && wrongly_armed->step(),"detector continuation failed");
        check(count(*s)==old && count(*restored)==old,"disarmed detector repeated an event");
        check(count(*wrongly_armed)>old,"detector regression fixture is not sensitive to lost armed flag");
        identical(*s,*restored,root);
    }
}
static void reliability_tests(const std::filesystem::path& root) {
    auto c=fixtures()[2];c.duration=.001013;c.sample=.00015;
    // Preserve also the non-shared parameter array and signed zero exactly.
    for(size_t i=0;i<c.parameters.size();++i)if(std::find(std::begin(SharedParameters),std::end(SharedParameters),int(i))==std::end(SharedParameters)){c.parameters[i]=std::numeric_limits<double>::denorm_min();break;}
    c.neurons[0].px=-0.;c.neurons[0].amplitude=std::numeric_limits<double>::denorm_min();
    c.neurons[0].py=std::nextafter(.3,1.);c.name="Контроль точных значений";
    Simulator finished(c);finish(finished);save_checkpoint(finished,root/"good.nscp");auto loaded=load_checkpoint(root/"good.nscp");
    identical(finished,*loaded,root);check(loaded->done()&&!loaded->step(),"completed snapshot auto-restarted");
    finished.extend_duration(.003017);loaded->extend_duration(.003017);identical(finished,*loaded,root);
    finish(finished);finish(*loaded);identical(finished,*loaded,root);
    loaded->extend_duration(.005);roundtrip(loaded,root);finish(*loaded);
    auto before=bytes(root/"good.nscp");
    finished.error="Diagnostic test: calculation failed";save_checkpoint(finished,root/"error.nscp");auto failed=load_checkpoint(root/"error.nscp");
    check(failed->error==finished.error && failed->done() && !failed->step(),"failed snapshot resumed");
    rejects([&]{failed->extend_duration(.01);},"failed snapshot extended");
    finished.state[0]=std::numeric_limits<double>::infinity();rejects([&]{save_checkpoint(finished,root/"good.nscp");},"non-finite snapshot accepted");
    check(bytes(root/"good.nscp")==before,"failed save destroyed previous file");
#ifdef _WIN32
    HANDLE lock=CreateFileW((root/"good.nscp").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(lock!=INVALID_HANDLE_VALUE,"could not lock atomic replacement target");
    rejects([&]{save_checkpoint(*loaded,root/"good.nscp");},"locked replacement should fail");CloseHandle(lock);
    check(bytes(root/"good.nscp")==before,"atomic replacement failure destroyed old file");
#endif
    for(const auto& entry:std::filesystem::directory_iterator(root))check(entry.path().filename().string().find(".tmp-")==std::string::npos,"temporary file leaked");
    auto good=bytes(root/"good.nscp");Offsets o(good);
    auto bad=[&](std::string data){write(root/"bad.nscp",data);rejects([&]{load_checkpoint(root/"bad.nscp");},"corrupt checkpoint was accepted");};
    for(size_t length:{size_t(0),size_t(7),good.size()/2,good.size()-1})bad(good.substr(0,length));
    auto corrupt=good;corrupt[corrupt.size()/2]^=1;bad(corrupt);
    for(auto mutation:std::vector<std::pair<size_t,uint64_t>>{{26,3},{o.state,UINT64_MAX},{o.rows,MaxSamples+1},{o.thresholds+8,2},{o.thresholds,10000},{o.steps,(1ull<<53)+1},{o.sample,(1ull<<53)+1},{o.time,0x7ff0000000000000ull},{o.state+8,0x7ff8000000000001ull},{o.state+8+8*(syn_offset(c,0)+1),0x4000000000000000ull}}) {
        auto b=good;put64(b,mutation.first,mutation.second);resign(b);bad(b);
    }
    for(size_t offset:{size_t(0),o.algorithm+8,o.build+8}){auto b=good;b[offset]^=1;resign(b);bad(b);}
    auto trailing=good;trailing.insert(trailing.size()-8,1,'x');resign(trailing);bad(trailing);
    // Valid checksum cannot turn an invalid graph, clock or index into a valid file.
    auto inconsistent=good;put64(inconsistent,o.sample,1);resign(inconsistent);bad(inconsistent);
    const auto huge=root/"oversized.nscp";{std::ofstream f(huge,std::ios::binary);f.seekp(MaxCheckpointBytes);f.put('x');}
    rejects([&]{load_checkpoint(huge);},"oversized file accepted");std::filesystem::remove(huge);
    // Force a genuine resolution failure inside step, then retain the diagnostic.
    auto resolution=good;put64(resolution,o.suggested,1);resign(resolution);write(root/"resolution.nscp",resolution);
    auto broken=load_checkpoint(root/"resolution.nscp");broken->extend_duration(.002);
    check(!broken->step()&&!broken->error.empty(),"clock failure fixture did not fail");
    save_checkpoint(*broken,root/"failed_step.nscp");auto diagnostic=load_checkpoint(root/"failed_step.nscp");
    check(diagnostic->error==broken->error && !diagnostic->step(),"partial failed step resumed");
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--fixtures") {
            std::filesystem::create_directories(argv[2]);int i=0;
            for(auto c:fixtures())save_config(c,std::filesystem::path(argv[2])/("case"+std::to_string(i++)+".json"));return 0;
        }
        auto root=std::filesystem::current_path()/("checkpoint-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        comparison_tests(root);detector_tests(root);reliability_tests(root);
        std::cout<<"PASS: "<<checks<<" checks, "<<roundtrips<<" exact round trips, HH/astro disarmed detectors, failure and atomicity tests. Artifacts: "<<root<<"\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
