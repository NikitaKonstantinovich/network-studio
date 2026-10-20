#pragma once
#include "analysis.h"
#include <functional>
namespace studio {
// Archive I/O is synchronous, owned by the solver controller. It never runs model steps.
enum class ArchiveRecord : uint64_t { Sample=1,Event=2,Delivery=3,Stimulus=4,Pulse=5,Protocol=6 };
struct ArchiveInfo {
    std::string id,parent,parent_anchor,status="open",diagnostic;
    uint64_t blocks=0,records=0,steps=0,signature=0,config_hash=0;
    double from=0,until=0;
    bool deliveries=false,orphan_tail=false;
};
class HistoryArchive {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    static constexpr size_t BufferBytes=4*1024*1024,MaxRecordBytes=64*1024*1024;
    HistoryArchive(const std::filesystem::path& directory,const Simulator& s,bool deliveries,bool append);
    ~HistoryArchive();
    void sample(const Simulator& s);
    void event(const Event& e);
    void delivery(const Delivery& e);
    void stimulus(const StimulusEvent& e);
    void pulse(const SharedPulse& p,double observed=-1);
    void protocol(const Simulator& s);
    void boundary(const Simulator& s,bool force=false);
    std::string anchor() const;
    ArchiveInfo info() const;
    size_t buffered_bytes() const;
};
// Explicit attach: new directory at t=0, or branch at a restored time; append only
// when the checkpoint anchor equals the archive's latest committed boundary.
void start_archive(Simulator& s,const std::filesystem::path& directory,bool deliveries=false,bool append=false);
void flush_archive(const Simulator& s);
ArchiveInfo inspect_archive(const std::filesystem::path& directory);
void scan_archive(const std::filesystem::path& directory,const std::function<void(ArchiveRecord,const std::string&)>& visitor,const ArchiveInfo* snapshot=nullptr);
NetworkAnalysis analyze_archive(const std::filesystem::path& directory,const AnalysisSettings& settings,const AnalysisInterval& interval);
// Bounded interval export of all recorded channels, including selected states.
void export_archive(const std::filesystem::path& directory,double from,double to,const std::filesystem::path& prefix);
}
