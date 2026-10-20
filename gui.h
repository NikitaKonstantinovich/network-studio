#pragma once
#include "io.h"
#include "simulation_runner.h"
#include "generator.h"
#include "imgui.h"
#include "plot_windows.h"
#include "activity_field.h"
#include <memory>
#include <functional>
namespace studio {
class App {
public:
    Config config = preset(0);
    const SimulationView* simulation = nullptr;
    ~App();
    std::shared_future<RunnerResult> command(RunnerCommand value,bool wait=false);
    RunnerResult wait_command(std::shared_future<RunnerResult> result);
    void synchronize();
    void run();
    void pause();
    void advance_for(double duration);
    void export_data(const std::filesystem::path& path);
    void set_display_hz(double value);
    void set_compute_threads(int requested,bool wait=false);
    bool running = false, dirty = false;
    std::string status = "Готово. Выберите узел, настройте параметры и запустите расчёт.";
    int selected_kind = 0, selected_index = 0;
    int request_plot_tab = -1; // Used by the renderer smoke test; -1 leaves user selection alone.
    bool request_network_wizard = false;
    int request_wizard_tab=-1;
    void preview_network(const NetworkGeneration& settings,bool append=false);
    bool show_stages=false;
    bool show_weights=false,weights_include_live=true,weights_dirty=true;
    WeightSettings weight_settings;
    std::vector<std::shared_ptr<const WeightSnapshot>> weight_snapshots;
    std::shared_ptr<const WeightSnapshot> weights_live;
    WeightAnalysis weight_result;
    std::array<char,512> weight_name{};
    std::string weight_message;
    int weight_reference=0,weight_target=1,weight_saved_selection=0;
    uint64_t weight_sequence=0,weight_generation=0;
    std::function<std::filesystem::path(bool)> weight_dialog;
    void add_weight_snapshot(WeightSnapshot snapshot);
    void capture_weight_snapshot();
    void import_weight_snapshot(const std::filesystem::path& path);
    bool show_archive=false,archive_deliveries=false;
    int archive_rows=10000;
    std::array<char,2048> archive_path{};
    AnalysisInterval archive_interval{0,1,false};
    AnalysisProfile archive_profile;
    NetworkAnalysis archive_result;
    std::future<NetworkAnalysis> archive_job;
    std::string archive_message,archive_analysis_source,archive_info_label;
    bool show_stimulators=false;
    uint64_t selected_stimulator=0;
    Stimulator stimulator_draft;
    std::function<std::filesystem::path(bool,bool)> file_dialog;
    std::function<std::filesystem::path(bool)> checkpoint_dialog;
    std::function<std::filesystem::path(bool)> workspace_dialog;
    PlotWindows plot_windows;
    WeightField weight_field;
    ActivityField activity_field;
    App();
    void draw();
    void advance();
    void reset();
    void replace(Config c);
    void save(const std::filesystem::path& path);
    void load(const std::filesystem::path& path);
    void save_state(const std::filesystem::path& path);
    bool restore_state(const std::filesystem::path& path);
private:
    SimulationView display;
    std::unique_ptr<SimulationRunner> runner;
    struct Pending {std::shared_future<RunnerResult> result;RunnerCommand::Kind kind;};
    std::vector<Pending> pending;
    std::shared_ptr<const Config> display_config;
    bool drawing=false,replacing=false;
    float display_hz=10;
    bool changed = false;
    int preset_index = 0, link_source = 0, link_target = 1, link_astro = 0;
    int syn_plot = 0, syn_view = 0, astro_plot = 0;
    int neuron_plot = 0, neuron_view = 0;
    int drag_kind = -1, drag_index = -1;
    NetworkGeneration generation;
    GenerationSummary generation_summary;
    std::unique_ptr<Config> generation_preview;
    bool generation_append = false;
    std::string generation_error;
    std::filesystem::path filename;
    void toolbar();
    void editor();
    void inspector();
    void canvas();
    void field_controls();
    void field_overlay(ImVec2 base,ImVec2 span,bool tooltip);
    void plots(float height);
    void network_wizard();
    void archive_manager();
    void weights_manager();
    void stage_manager();
    void stimulator_manager();
    bool stimulator_overlay(ImVec2 base,ImVec2 span,bool hovered,bool active);
    uint64_t drag_stimulator=0;
    void edit() { changed = true; dirty = true; }
};
void apply_style();
}
