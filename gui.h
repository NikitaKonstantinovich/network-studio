#pragma once
#include "io.h"
#include "generator.h"
#include "imgui.h"
#include "plot_windows.h"
#include <memory>
#include <functional>
namespace studio {
class App {
public:
    Config config = preset(0);
    std::unique_ptr<Simulator> simulation;
    bool running = false, dirty = false;
    std::string status = "Готово. Выберите узел, настройте параметры и запустите расчёт.";
    int selected_kind = 0, selected_index = 0;
    int request_plot_tab = -1; // Used by the renderer smoke test; -1 leaves user selection alone.
    bool request_network_wizard = false;
    int request_wizard_tab=-1;
    void preview_network(const NetworkGeneration& settings,bool append=false);
    bool show_stages=false;
    std::function<std::filesystem::path(bool,bool)> file_dialog;
    std::function<std::filesystem::path(bool)> checkpoint_dialog;
    std::function<std::filesystem::path(bool)> workspace_dialog;
    PlotWindows plot_windows;
    WeightField weight_field;
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
    void stage_manager();
    void edit() { changed = true; dirty = true; }
};
void apply_style();
}
