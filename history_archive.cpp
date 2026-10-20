#include "history_archive.h"
#include "archive_codec.h"
#include "io.h"
#include "checkpoint_build.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace studio {
using namespace archive_codec;
namespace {
std::filesystem::path block_path(const std::filesystem::path& dir,uint64_t n){std::ostringstream s;s<<std::setw(16)<<std::setfill('0')<<n<<".nshb";return dir/s.str();}
std::string manifest(const ArchiveInfo& i){Writer w;w.text("NETWORK-STUDIO-HISTORY");w.u(1);w.text(STUDIO_CHECKPOINT_BUILD_ID);w.text(i.id);w.text(i.parent);w.text(i.parent_anchor);w.text(i.status);w.text(i.diagnostic);w.u(i.blocks);w.u(i.records);w.u(i.steps);w.u(i.signature);w.u(i.config_hash);w.d(i.from);w.d(i.until);w.u(i.deliveries);return w.sealed();}
ArchiveInfo parse_manifest(const std::filesystem::path& dir){auto b=verified(dir/"manifest.nsh",32768);Reader r{b};check(r.text(64)=="NETWORK-STUDIO-HISTORY"&&r.u()==1,"unsupported document or schema");r.text(256);ArchiveInfo i;i.id=r.text(128);i.parent=r.text(16384);i.parent_anchor=r.text(16384);i.status=r.text(32);i.diagnostic=r.text(4096);i.blocks=r.u();i.records=r.u();i.steps=r.u();i.signature=r.u();i.config_hash=r.u();i.from=r.d();i.until=r.d();auto deliveries=r.u();check(deliveries<=1&&i.from>=0&&i.until>=i.from&&i.blocks<=1000000000,"invalid manifest");i.deliveries=deliveries!=0;r.end();return i;}
std::string anchor_for(const std::filesystem::path& dir,const ArchiveInfo& i){Writer w;w.text(std::filesystem::absolute(dir).u8string());w.text(i.id);w.u(i.blocks);w.u(i.records);w.u(i.steps);w.d(i.until);w.u(i.signature);return w.bytes;}
uint64_t signature(const Simulator& s){Writer w;auto c=s.config; // duration extension is intentionally allowed AFTER attachment
    w.text(serialize(c));w.text(serialize_runtime(s));w.text(serialize_stimulus_runtime(s));w.d(s.time);w.u(s.steps);
    for(double v:s.state)w.d(v);for(auto p:s.plasticity){w.d(p.weight);w.d(p.pre_trace);w.d(p.post_trace);w.d(p.post_slow_trace);}
    for(auto n:s.spikes)w.u(n);for(auto n:s.releases)w.u(n);auto pending=s.pending_deliveries;std::sort(pending.begin(),pending.end(),[](const auto& a,const auto& b){return DeliveryLater{}(b,a);});for(auto d:pending){w.d(d.t_emit);w.d(d.t_arrival);w.u(d.synapse);}return hash(w.bytes);
}
std::string new_id(){std::random_device rd;std::ostringstream s;s<<std::hex<<std::chrono::high_resolution_clock::now().time_since_epoch().count()<<rd()<<rd();return s.str();}
}
struct HistoryArchive::Impl {
    std::filesystem::path directory;ArchiveInfo committed,current;std::string buffer;uint64_t count=0,last_commit_step=0;bool failed=false;
#ifdef _WIN32
    HANDLE lock=INVALID_HANDLE_VALUE;
#endif
    ~Impl(){
#ifdef _WIN32
        if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
#endif
    }
    void acquire(){
#ifdef _WIN32
        lock=CreateFileW((directory/L"writer.lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(lock!=INVALID_HANDLE_VALUE,"another writer owns this archive or lock cannot be opened");
#endif
    }
    void flush(){if(buffer.empty())return;check(current.blocks<1000000000&&count<=UINT64_MAX-current.records,"archive block/record counter limit reached");Writer w;w.text("NSH-BLOCK");w.u(1);w.text(current.id);w.u(current.blocks+1);w.u(count);w.text(buffer);auto path=block_path(directory,current.blocks+1);check(!std::filesystem::exists(path),"block already exists; source archive will not be overwritten");write_atomic(path,w.sealed());++current.blocks;current.records+=count;buffer.clear();count=0;}
    void put(ArchiveRecord type,const std::string& payload){check(!failed,"writer has failed; restore a checkpoint into a new archive");
        try{check(payload.size()<=HistoryArchive::MaxRecordBytes,"one record exceeds 64 MiB; recording stopped");if(buffer.size()+payload.size()+16>HistoryArchive::BufferBytes)flush();Writer w;w.u(uint64_t(type));w.text(payload);buffer+=w.bytes;++count;if(buffer.size()>=HistoryArchive::BufferBytes)flush();}catch(...){failed=true;throw;}}
};
HistoryArchive::HistoryArchive(const std::filesystem::path& dir,const Simulator& s,bool deliveries,bool append):impl(std::make_unique<Impl>()) {
    check(!s.stepping&&s.checkpoint_boundary,"recording requires a completed step boundary");
    impl->directory=std::filesystem::absolute(dir).lexically_normal();
    if(append){
        impl->acquire();auto info=inspect_archive(dir);
        check(!info.orphan_tail,"uncommitted tail exists; analyze committed prefix and resume into a NEW branch directory");
        check(info.status!="failed"&&s.archive_anchor==anchor_for(impl->directory,info),"checkpoint is not the latest committed boundary of this archive; create a new branch");
        check(signature(s)==info.signature,"checkpoint state/protocol changed before archive attachment");
        check(deliveries==info.deliveries,"delivery recording mode cannot change when appending");
        impl->committed=impl->current=info;impl->current.status="open";impl->last_commit_step=s.steps;
    } else {
        check(!std::filesystem::exists(dir),"new archive directory already exists; refusing to overwrite");
        check(std::filesystem::create_directory(dir),"cannot create archive directory");impl->acquire();
        auto& i=impl->current;i.id=new_id();i.deliveries=deliveries;i.from=i.until=s.time;i.steps=s.steps;
        if(!s.archive_anchor.empty()){i.parent_anchor=s.archive_anchor;Reader r{s.archive_anchor};
            // Sequenced reads: operator+ operands may be evaluated in either order.
            // Reading the path with the ID's 128-byte limit broke long/UTF-8 paths.
            const auto parent_path=r.text(8192);const auto parent_id=r.text(128);
            i.parent=parent_path+" @ "+parent_id+"; checkpoint t="+std::to_string(s.time);}
        auto config=serialize(s.config);i.config_hash=hash(config);write_atomic(dir/"config.json",config);
        protocol(s);if(s.history.time.back()==s.time)sample(s);
        // A branch begins at the checkpoint boundary. Existing open pulses are
        // context, not new onsets: consumers filter their start against from.
        for(const auto& p:s.history.shared_pulses)if(!p.complete)pulse(p,s.time);
        boundary(s,true);
    }
}
HistoryArchive::~HistoryArchive()=default;
void HistoryArchive::sample(const Simulator& s){Writer w;w.d(s.time);w.u(s.history.sample_begin+s.history.time.size()-1);for(const auto& col:s.history.values)if(!col.empty())w.d(col.back());for(const auto& col:s.history.weights)if(!col.empty())w.d(col.back());impl->put(ArchiveRecord::Sample,w.bytes);}
void HistoryArchive::event(const Event& e){Writer w;w.d(e.time);w.u(e.index);w.u(e.astro);impl->put(ArchiveRecord::Event,w.bytes);}
void HistoryArchive::delivery(const Delivery& e){if(!impl->current.deliveries)return;Writer w;w.d(e.t_arrival);w.d(e.t_emit);w.u(e.synapse);impl->put(ArchiveRecord::Delivery,w.bytes);}
void HistoryArchive::stimulus(const StimulusEvent& e){Writer w;w.d(e.time);w.d(e.amplitude);w.u(e.neuron);w.u(e.protocol);impl->put(ArchiveRecord::Stimulus,w.bytes);}
void HistoryArchive::pulse(const SharedPulse& p,double observed){Writer w;w.d(observed>=0?observed:p.complete?p.end:p.start);w.u(p.stimulator);w.u(p.pulse);w.d(p.start);w.d(p.end);w.d(p.amplitude);w.u(p.complete);w.u(p.stage+1);w.u(p.recipients.size());for(int i:p.recipients)w.u(i);w.u(observed>=0);impl->put(ArchiveRecord::Pulse,w.bytes);}
void HistoryArchive::protocol(const Simulator& s){Writer w;w.d(s.time);w.text(serialize(s.config));w.text(serialize_runtime(s));w.text(serialize_stimulus_runtime(s));impl->put(ArchiveRecord::Protocol,w.bytes);}
void HistoryArchive::boundary(const Simulator& s,bool force){
    check(!s.stepping&&s.checkpoint_boundary,"cannot commit an unfinished step");
    check(!impl->failed,"recording failed; no further data accepted");
    if(!force&&!s.done()&&s.steps-impl->last_commit_step<4096&&impl->current.blocks==impl->committed.blocks)return;
    try{impl->flush();auto i=impl->current;i.until=s.time;i.steps=s.steps;i.signature=signature(s);i.status=s.error.empty()?(s.time>=s.config.duration?"complete":"open"):"failed";i.diagnostic=s.error.substr(0,4096);
        write_atomic(impl->directory/"manifest.nsh",manifest(i));impl->current=impl->committed=i;impl->last_commit_step=s.steps;
    }catch(...){impl->failed=true;throw;}
}
std::string HistoryArchive::anchor()const{return anchor_for(impl->directory,impl->committed);}
ArchiveInfo HistoryArchive::info()const{auto i=impl->committed;if(impl->failed){i.status="failed";i.diagnostic="Writer failed; committed prefix retained. Restore checkpoint into a new branch.";}return i;}
size_t HistoryArchive::buffered_bytes()const{return impl->buffer.size();}
void start_archive(Simulator& s,const std::filesystem::path& dir,bool deliveries,bool append){
    check(!s.archive&&s.error.empty(),"archive is already active or simulation failed");
    check(s.steps==0||!s.archive_anchor.empty(),"enable before the first step, or restore an archived checkpoint to branch");
    check(s.config.history_capacity>0&&s.config.bounded_journals,"enable rolling state history and bounded journals before recording");
    auto writer=std::make_shared<HistoryArchive>(dir,s,deliveries,append);s.archive=std::move(writer);s.archive_anchor=s.archive->anchor();
}
void flush_archive(const Simulator& s){if(s.archive&&s.error.empty())s.archive->boundary(s,true);}
ArchiveInfo inspect_archive(const std::filesystem::path& dir){auto i=parse_manifest(dir);i.orphan_tail=std::filesystem::exists(block_path(dir,i.blocks+1));for(auto& e:std::filesystem::directory_iterator(dir)){
        auto name=e.path().filename().u8string();if(name.find(".tmp")!=std::string::npos)i.orphan_tail=true;
        if(e.path().extension()==L".nshb"){try{size_t used=0;auto n=std::stoull(e.path().stem().u8string(),&used);if(used!=16||n>i.blocks||n==0)i.orphan_tail=true;}catch(...){i.orphan_tail=true;}}
    }return i;}
void scan_archive(const std::filesystem::path& dir,const std::function<void(ArchiveRecord,const std::string&)>& visitor,const ArchiveInfo* snapshot){
    const auto info=snapshot?*snapshot:parse_manifest(dir);check(hash(read(dir/"config.json"))==info.config_hash,"configuration checksum mismatch");uint64_t total=0;
    for(uint64_t n=1;n<=info.blocks;++n){auto b=verified(block_path(dir,n));Reader r{b};check(r.text(64)=="NSH-BLOCK"&&r.u()==1&&r.text(128)==info.id&&r.u()==n,"invalid block identity/order");auto count=r.u();auto body=r.text(HistoryArchive::MaxRecordBytes+16);r.end();Reader records{body};check(count<=body.size()/16,"invalid record count");
        for(uint64_t k=0;k<count;++k){auto type=records.u();check(type>=1&&type<=6,"unknown record type");auto data=records.text();visitor(ArchiveRecord(type),data);}records.end();total+=count;
    }check(total==info.records,"manifest record count mismatch");
}
}
