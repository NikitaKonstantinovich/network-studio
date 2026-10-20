#pragma once
#include "io.h"
#include "imgui.h"
#include "weight_field.h"
#include "analysis.h"
#include "simulation_view.h"
#include <array>
#include <functional>

namespace studio {
enum class PlotKind { Voltage, Astrocytes, Raster, Synapse, AstroPhase, Neuron, Deliveries, Network };
struct PlotWindowState {
    uint32_t id=0;
    std::string title="График", binding;
    PlotKind kind=PlotKind::Voltage;
    bool open=true, compatible=true, follow=true, auto_fit=true, fit=true;
    double seconds=1;
    int object=0, view=0;
    // Explicit indices, never pointers into the graph/history. Empty means no selection.
    std::vector<int> objects;
    uint32_t quantities=127;
    bool limits_valid=false, restore_limits=false, place=true, undock=false;
    std::array<double,6> limits{{0,1,-1,1,-1,1}};
    ImVec2 position{120,120}, size{720,480};
    AnalysisSettings analysis;
    AnalysisInterval analysis_interval;
    std::string analysis_profile;
    std::array<char,129> profile_draft{};
    NetworkAnalysis analyzed;
    double analysis_at=-1;
    std::string analysis_key;
    const void* analysis_identity=nullptr;
    uint64_t analysis_generation=0;
    std::string window_name() const;
};
// Presentation only. Reads the frame's stable display view without owning a solver.
class PlotWindows {
public:
    static constexpr size_t MaxWindows=32, MaxWorkspaceBytes=4*1024*1024;
    std::vector<PlotWindowState> windows;
    std::array<PlotWindowState,8> embedded;
    std::vector<AnalysisProfile> analysis_profiles;
    TopologyViewState topology;
    std::string binding, message;
    std::filesystem::path workspace_path;
    bool host_open=false;
    std::function<std::filesystem::path()> analysis_export_dialog;
    std::function<void(const AnalysisSettings&,const AnalysisInterval&,const std::string&,const std::filesystem::path&)> analysis_export;
    bool automatic_save_allowed=true;
    PlotWindows();
    void data_changed(const Config& config,bool replacement=false);
    uint32_t create(PlotKind kind,int object=0);
    uint32_t duplicate(uint32_t id);
    uint32_t detach(const PlotWindowState& source);
    bool close(uint32_t id);
    PlotWindowState* find(uint32_t id);
    void rebind(PlotWindowState& window);
    void draw(const SimulationReadView& simulation);
    void draw_view(PlotWindowState& window,const SimulationReadView& simulation,float height,bool detached);
    void reset_layout();
    void demo();
    bool save(const std::filesystem::path& path);
    bool load(const std::filesystem::path& path);
    static std::string config_binding(const Config& config);
    static void clamp_to_monitors(ImVec2& position,ImVec2& size);
    void draw_network(PlotWindowState& window,const SimulationReadView& simulation);
    bool create_analysis_profile(PlotWindowState& window,const std::string& name);
    bool load_analysis_profile(PlotWindowState& window,size_t index);
    void apply_analysis_to_all(const PlotWindowState& source);
private:
    uint32_t next_id=1;
    bool reset_host=false, demo_layout=false;
    std::string pending_ini;
};
}
