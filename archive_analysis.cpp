#include "history_archive.h"
#include "archive_codec.h"
#include "io.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <locale>
#include <sstream>
#include <atomic>
#include <cwctype>
namespace studio {
using namespace archive_codec;
namespace {
std::string compact_json(const std::string& input){std::string out;out.reserve(input.size());bool quoted=false,escaped=false;for(unsigned char ch:input){if(quoted){out.push_back(char(ch));if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;}else if(ch=='"'){quoted=true;out.push_back(char(ch));}else if(ch!=' '&&ch!='\n'&&ch!='\r'&&ch!='\t')out.push_back(char(ch));}return out;}
std::vector<int> columns(const Config& c,bool weights){std::vector<int> out;if(!weights){for(int i=0;i<int(c.neurons.size());++i)if(records_neuron(c,i))for(int k=0;k<4;++k)out.push_back(i*4+k);for(int i=0;i<int(c.synapses.size());++i)if(records_synapse(c,i))for(int k=0;k<4;++k)out.push_back(syn_offset(c,i)+k);for(int i=0;i<int(c.astrocytes.size());++i)for(int k=0;k<7;++k)out.push_back(astro_offset(c,i)+k);}else for(int i=0;i<int(c.synapses.size());++i)if(records_synapse(c,i))out.push_back(i);return out;}
struct Decoded {
    double at=0;Event event{};Delivery delivery{};StimulusEvent stimulus{};SharedPulse pulse{};
    uint64_t sample=0;bool pulse_context=false;std::vector<double> values;std::string config,runtime,stimulators;
};
Decoded decode(const Config& c,const ArchiveInfo& info,ArchiveRecord type,const std::string& b){Reader r{b};Decoded d;d.at=r.d();check(d.at>=info.from&&d.at<=info.until,"record outside committed range");
    switch(type){
    case ArchiveRecord::Event:{auto index=r.u(),astro=r.u();check(astro<=1&&index<(astro?c.astrocytes.size():c.neurons.size()),"invalid event index");d.event={d.at,int(index),astro!=0};break;}
    case ArchiveRecord::Delivery:{double emit=r.d();auto edge=r.u();check(info.deliveries&&edge<c.synapses.size()&&emit>=0&&emit<=d.at&&emit+c.synapses[edge].delay==d.at,"invalid delivery");d.delivery={emit,d.at,int(edge)};break;}
    case ArchiveRecord::Stimulus:{double amplitude=r.d();auto neuron=r.u(),protocol=r.u();check(neuron<c.neurons.size()&&protocol<4096&&amplitude!=0,"invalid stimulus");d.stimulus={d.at,amplitude,int(neuron),int(protocol)};break;}
    case ArchiveRecord::Sample:{d.sample=r.u();auto n=recorded_dimension(c);check(b.size()==16+size_t(n)*8,"sample dimension mismatch");d.values.reserve(n);for(int k=0;k<n;++k)d.values.push_back(r.d());break;}
    case ArchiveRecord::Pulse:{auto& p=d.pulse;p.stimulator=r.u();p.pulse=r.u();p.start=r.d();p.end=r.d();p.amplitude=r.d();auto complete=r.u(),stage=r.u(),count=r.u();check(p.stimulator>0&&p.pulse>0&&p.start>=0&&p.end>=p.start&&p.end<=info.until&&complete<=1&&stage<=256&&count<=c.neurons.size(),"invalid shared pulse");p.complete=complete!=0;p.stage=int(stage)-1;for(size_t k=0;k<count;++k){auto i=r.u();check(i<c.neurons.size(),"invalid pulse recipient");p.recipients.push_back(int(i));}auto context=r.u();check(context<=1,"invalid pulse context flag");d.pulse_context=context!=0;break;}
    case ArchiveRecord::Protocol:d.config=r.text();d.runtime=r.text();d.stimulators=r.text();break;
    }r.end();return d;
}
struct Segment {std::filesystem::path path;ArchiveInfo info;};
std::vector<Segment> segments(const std::filesystem::path& path,const ArchiveInfo& info){
    std::vector<Segment> out;std::vector<std::string> seen;
    std::function<void(const std::filesystem::path&,ArchiveInfo)> visit=[&](const std::filesystem::path& at,ArchiveInfo entry){
        check(seen.size()<16&&std::find(seen.begin(),seen.end(),entry.id)==seen.end(),"archive lineage exceeds 16 segments or contains a cycle");seen.push_back(entry.id);
        if(!entry.parent_anchor.empty()){
            Reader r{entry.parent_anchor};auto parent=std::filesystem::u8path(r.text(8192));auto id=r.text(128);auto blocks=r.u(),records=r.u(),steps=r.u();auto until=r.d();auto signature=r.u();r.end();
            check(parent.is_absolute(),"parent archive path must be absolute");
#ifdef _WIN32
            check(parent.root_name().native().size()==2&&parent.root_name().native()[1]==L':',"network parent archive paths are not opened automatically");
#endif
            auto prefix=inspect_archive(parent);check(prefix.id==id&&prefix.blocks>=blocks&&prefix.records>=records&&prefix.until>=until&&until==entry.from,"missing or incompatible parent archive prefix");
            if(prefix.blocks==blocks)check(prefix.signature==signature,"parent checkpoint signature mismatch");
            prefix.blocks=blocks;prefix.records=records;prefix.steps=steps;prefix.until=until;prefix.signature=signature;visit(parent,std::move(prefix));
        }
        out.push_back({at,std::move(entry)});
    };visit(path,info);return out;
}
struct PagedEvents {
    std::filesystem::path path;std::ofstream output;std::ifstream input;size_t count=0;uint64_t tick=0;
    static constexpr size_t Rows=2048;
    struct Page {size_t start=SIZE_MAX;uint64_t age=0;std::vector<Event> data;};std::array<Page,8> pages;
    PagedEvents(){static std::atomic<uint64_t> serial{0};auto base=std::filesystem::temp_directory_path();for(int i=0;i<100;++i){path=base/("network-analysis-"+std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial)+".tmp");if(!std::filesystem::exists(path))break;}output.open(path,std::ios::binary|std::ios::trunc);check(bool(output),"cannot create analysis spool");}
    ~PagedEvents(){output.close();input.close();std::error_code ec;std::filesystem::remove(path,ec);}
    void push(Event e){Writer w;w.d(e.time);w.u(e.index);output.write(w.bytes.data(),16);check(bool(output),"analysis spool write failed");++count;}
    void ready(){output.close();check(bool(output),"analysis spool flush failed");input.open(path,std::ios::binary);check(bool(input),"cannot read analysis spool");}
    Event get(size_t index){check(index<count,"analysis cursor out of range");size_t start=index/Rows*Rows;Page* page=nullptr;for(auto& p:pages)if(p.start==start){page=&p;break;}
        if(!page){page=&*std::min_element(pages.begin(),pages.end(),[](const Page& a,const Page& b){return a.age<b.age;});page->start=start;size_t n=std::min(Rows,count-start);std::string b(n*16,'\0');input.clear();input.seekg(std::streamoff(start)*16);input.read(b.data(),b.size());check(bool(input),"analysis spool read failed");Reader r{b};page->data.clear();page->data.reserve(n);for(size_t k=0;k<n;++k){double t=r.d();auto neuron=r.u();page->data.push_back({t,int(neuron),false});}}
        page->age=++tick;return page->data[index-start];}
};
}
NetworkAnalysis analyze_archive(const std::filesystem::path& directory,const AnalysisSettings& settings,const AnalysisInterval& interval){
    const auto info=inspect_archive(directory);const auto parts=segments(directory,info);auto c=load_config(parts.front().path/"config.json");History context;context.events_after=context.stimuli_after=context.shared_after=parts.front().info.from;
    const double until=interval.current_end?info.until:std::min(info.until,interval.to);PagedEvents events;double previous=-1;
    for(const auto& part:parts)scan_archive(part.path,[&](ArchiveRecord type,const std::string& bytes){auto d=decode(c,part.info,type,bytes);
        if(type==ArchiveRecord::Protocol){auto changed=deserialize(d.config);check(changed.neurons.size()==c.neurons.size()&&changed.synapses.size()==c.synapses.size()&&changed.astrocytes.size()==c.astrocytes.size(),"protocol changes graph dimensions");c.stimulators=std::move(changed.stimulators);c.next_stimulator_id=changed.next_stimulator_id;}
        else if(type==ArchiveRecord::Event){auto e=d.event;check(e.time>=previous,"unordered spikes");previous=e.time;if(e.astro||e.time>=until)return;const auto& n=c.neurons[e.index];auto p=settings.population;
            if(p==AnalysisPopulation::All||(p==AnalysisPopulation::Excitatory&&!n.inhibitory)||(p==AnalysisPopulation::Inhibitory&&n.inhibitory)||(p==AnalysisPopulation::Selected&&std::find(settings.neurons.begin(),settings.neurons.end(),e.index)!=settings.neurons.end()))events.push(e);
        }else if(type==ArchiveRecord::Stimulus&&!settings.reference_stimulator&&d.stimulus.neuron==settings.reference_neuron&&d.at>=interval.from&&d.at<until){check(context.stimuli.size()<MaxEvents,"too many stimuli in requested interval; shorten it");context.stimuli.push_back(d.stimulus);
        }else if(type==ArchiveRecord::Pulse&&d.pulse.stimulator==settings.reference_stimulator&&!d.pulse.complete&&!d.pulse_context&&d.pulse.start>=std::max(part.info.from,interval.from)&&d.pulse.start<until){check(context.shared_pulses.size()<MaxEvents,"too many shared stimuli in interval");context.shared_pulses.push_back(std::move(d.pulse));}
    },&part.info);events.ready();return analyze_event_source(c,context,info.until,settings,interval,events.count,[&](size_t index){return events.get(index);});
}
void export_archive(const std::filesystem::path& directory,double from,double to,const std::filesystem::path& prefix){
    check(std::isfinite(from)&&std::isfinite(to)&&from>=0&&to>from,"invalid export interval");auto info=inspect_archive(directory);const auto parts=segments(directory,info);auto c=load_config(parts.front().path/"config.json");
    const auto source=std::filesystem::absolute(directory).lexically_normal();const auto destination=std::filesystem::absolute(prefix).lexically_normal();
    auto canonical=[](const std::filesystem::path& path){auto text=std::filesystem::weakly_canonical(path).generic_wstring();
#ifdef _WIN32
        std::transform(text.begin(),text.end(),text.begin(),[](wchar_t ch){return wchar_t(std::towlower(ch));});
#endif
        return text;};
    auto target=canonical(destination);for(const auto& part:parts){auto base=canonical(part.path);check(target!=base&&target.compare(0,base.size()+1,base+L"/")!=0,"export must be outside source archives");}
    auto metadata_path=prefix;metadata_path+="_archive.txt";check(!std::filesystem::exists(metadata_path),"export metadata exists; choose a new prefix");
    const std::array<const char*,6> suffix={"_states.csv","_events.csv","_deliveries.csv","_stimuli.csv","_shared_updates.csv","_protocols.jsonl"};
    std::array<std::filesystem::path,6> paths,temp;std::array<std::ofstream,6> files;
    for(size_t i=0;i<files.size();++i){paths[i]=prefix;paths[i]+=suffix[i];temp[i]=paths[i];temp[i]+=".tmp";check(!std::filesystem::exists(paths[i])&&!std::filesystem::exists(temp[i]),"output exists; choose a new export prefix");files[i].open(temp[i],std::ios::binary);check(bool(files[i]),"cannot open export");files[i].imbue(std::locale::classic());files[i]<<std::setprecision(17);}
    files[0]<<"time_s,sample_index";for(int i:columns(c,false))files[0]<<",state_"<<i;for(int i:columns(c,true))files[0]<<",weight_"<<i;files[0]<<"\n";
    files[1]<<"time_s,index,astro\n";files[2]<<"arrival_s,emission_s,synapse,source,target\n";files[3]<<"time_s,amplitude,neuron,protocol\n";files[4]<<"update_s,stimulator,pulse,start_s,end_s,amplitude,complete,stage,recipients\n";
    try{for(const auto& part:parts)scan_archive(part.path,[&](ArchiveRecord type,const std::string& bytes){auto d=decode(c,part.info,type,bytes);if(d.at<from||d.at>=to)return;
        if(part.info.from>parts.front().info.from&&((type==ArchiveRecord::Sample&&d.at==part.info.from)||(type==ArchiveRecord::Pulse&&d.pulse_context)))return;auto& f=files[size_t(type)-1];
        switch(type){
        case ArchiveRecord::Sample:f<<d.at<<","<<d.sample;for(double v:d.values)f<<","<<v;f<<"\n";break;
        case ArchiveRecord::Event:f<<d.at<<","<<d.event.index<<","<<d.event.astro<<"\n";break;
        case ArchiveRecord::Delivery:{auto& e=c.synapses[d.delivery.synapse];f<<d.at<<","<<d.delivery.t_emit<<","<<d.delivery.synapse<<","<<e.source<<","<<e.target<<"\n";break;}
        case ArchiveRecord::Stimulus:f<<d.at<<","<<d.stimulus.amplitude<<","<<d.stimulus.neuron<<","<<d.stimulus.protocol<<"\n";break;
        case ArchiveRecord::Pulse:{auto& p=d.pulse;f<<d.at<<","<<p.stimulator<<","<<p.pulse<<","<<p.start<<","<<p.end<<","<<p.amplitude<<","<<p.complete<<","<<p.stage<<",";for(int i:p.recipients)f<<i<<";";f<<"\n";break;}
        case ArchiveRecord::Protocol:f<<"{\"time_s\":"<<d.at<<",\"config\":"<<compact_json(d.config)<<",\"runtime\":"<<compact_json(d.runtime)<<",\"stimulators\":"<<compact_json(d.stimulators)<<"}\n";break;
        }check(bool(f),"export write failed");},&part.info);
        for(auto& f:files){f.close();check(bool(f),"export flush failed");}for(size_t i=0;i<files.size();++i)std::filesystem::rename(temp[i],paths[i]);
        std::ostringstream metadata;metadata<<std::setprecision(17)<<"document_type=network_studio_history_export\nschema=1\narchive="<<info.id<<"\nrequested="<<from<<","<<to<<"\navailable="<<parts.front().info.from<<","<<info.until<<"\nstatus="<<info.status<<"\norphan_tail="<<info.orphan_tail<<"\ndeliveries_recorded="<<info.deliveries<<"\nexport_complete=true\n";for(const auto& part:parts)metadata<<"segment="<<part.info.id<<";from="<<part.info.from<<";until="<<part.info.until<<";deliveries="<<part.info.deliveries<<"\n";auto p=prefix;p+="_archive.txt";check(!std::filesystem::exists(p),"metadata already exists");write_atomic(p,metadata.str());
    }catch(...){for(auto& f:files)f.close();throw;} // .tmp files explicitly identify an incomplete export.
}
}
