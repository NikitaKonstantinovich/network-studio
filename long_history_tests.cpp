#include "io.h"
#include "generator.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>

using namespace studio;
namespace {
int checks=0;
void check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
Config quiet() {
    auto c=preset(4);c.neurons[0].amplitude=0;c.neurons[0].current=0;c.dt=.00005;
    return c;
}
void finish(Simulator& s) {while(!s.done())if(!s.step())break;check(s.error.empty(),s.error.c_str());}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--fixtures") {
            auto c=quiet();c.duration=200;c.sample=2;c.neurons[0].start=150;c.neurons[0].stop=190;
            c.neurons[0].amplitude=30;c.neurons[0].frequency=.5;
            save_config(c,std::filesystem::path(argv[2])/"long.json");return 0;
        }
        check(MaxSamples==1000000 && MaxStoredValues==32000000,"Unexpected history budgets");
        for(double sample:{.000001,.000025,.00025,.003,2.})for(int k=1;k<100;++k) {
            const double grid=double(k)*sample;
            for(double end:{std::nextafter(grid,0.),grid,std::nextafter(grid,INFINITY)}) {
                // Independent enumeration of the actual multiplied sampling grid.
                size_t rows=1;double last=0;
                for(size_t n=1;double(n)*sample<=end;++n){last=double(n)*sample;++rows;}
                if(last<end)++rows;
                check(planned_samples(end,sample)==rows,"Sample estimate differs at a floating-point boundary");
            }
        }
        auto c=preset(0);c.duration=200;
        check(planned_samples(c.duration,c.sample)==800001 && recorded_dimension(c)==29 && validate(c).empty(),
              "Screenshot fixture (200 s, 0.25 ms, all states) is still rejected");
        c.duration=double(MaxSamples-1)*c.sample;
        check(planned_samples(c.duration,c.sample)==MaxSamples && validate(c).empty(),"Exactly one million rows rejected");
        c.duration=std::nextafter(c.duration,INFINITY);
        check(!validate(c).empty(),"Terminal row beyond one million accepted");
        c=preset(0);c.duration=200;c.neurons.resize(6,c.neurons[0]);
        check(!validate(c).empty(),"Scalar memory budget ignored");
        c=quiet();c.duration=3600;c.sample=2;c.neurons[0].start=1000;c.neurons[0].stop=3500;
        check(validate(c).empty(),"Long experiment/stimulus or sparse sampling rejected");
        check(serialize(deserialize(serialize(c)))==serialize(c),"Long JSON roundtrip failed");
        c.neurons[0].frequency=0;c.neurons[0].pulse_intervals={{200,201}};
        check(validate(c).empty(),"Disabled train retains an artificial 120 s table limit");
        for(double invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),1e300,1e14}) {
            auto bad=c;bad.duration=invalid;bad.sample=invalid;
            check(!validate(bad).empty(),"Non-finite or unresolvable clock accepted");
        }
        NetworkGeneration g;g.excitatory.count=2;g.inhibitory.count=0;g.topology=NetworkTopology::NoConnections;
        g.duration=3600;g.sample=2;auto generated=generate_network(preset(3),g,false);
        check(generated.duration==3600 && validate(generated).empty(),"Long network generation failed");
        auto root=std::filesystem::current_path()/"long-history"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        c=quiet();c.dt=c.sample=.000001;c.duration=double(MaxSamples-1)*c.sample;
        Simulator full(c);finish(full);
        check(full.history.time.size()==MaxSamples && full.history.time.back()==c.duration,"Actual million-row recording failed");
        save_checkpoint(full,root/"million.nscp");auto loaded=load_checkpoint(root/"million.nscp");
        check(loaded->history.time==full.history.time && loaded->history.values==full.history.values &&
              loaded->state==full.state && loaded->steps==full.steps,"Million-row checkpoint changed history/state");
        loaded->extend_duration(c.duration); // Same endpoint is a no-op, even at the limit.
        bool rejected=false;try{loaded->extend_duration(c.duration+c.sample);}catch(const std::exception&){rejected=true;}
        check(rejected && loaded->config.duration==c.duration,"Oversized extension changed the experiment");
        save_checkpoint(*loaded,root/"loaded.nscp");
        check(bytes(root/"million.nscp")==bytes(root/"loaded.nscp"),"Checkpoint roundtrip is not byte-identical");
        c=quiet();c.dt=.000001;c.sample=.000003;c.duration=.0010051;
        Simulator extended(c);finish(extended);extended.extend_duration(.0010171);finish(extended);
        check(extended.history.time.size()==planned_samples(extended.time,c.sample)+1,"Off-grid extension estimate wrong");
        save_checkpoint(extended,root/"extended.nscp");auto continued=load_checkpoint(root/"extended.nscp");
        check(continued->history.time==extended.history.time,"Extended sampling grid rejected by checkpoint");
        std::cout<<"PASS: "<<checks<<" long-history checks; 1,000,000 actual samples, exact checkpoint roundtrip. Artifacts: "<<root.u8string()<<"\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
