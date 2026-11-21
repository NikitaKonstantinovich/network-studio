#pragma once
#include "plot_windows.h"
#include "saved_results.h"
#include <future>
#include <memory>

namespace studio {

// Owns a single export and its plot settings independently of the active experiment.
class ResultsViewer {
public:
    void open(const std::filesystem::path& file);
    void poll(bool wait=false);
    void draw();
    bool loading() const {return job.valid();}
    const SavedResults* data() const {return result.get();}
    const std::string& error() const {return failure;}
    void select(PlotKind kind);
    const PlotWindowState& plot(PlotKind kind) const {return plots.embedded[int(kind)];}
    PlotInterval interval() const {return shown;}
    bool set_interval(PlotInterval value);
private:
    std::unique_ptr<SavedResults> result;
    std::future<SavedResults> job;
    std::string failure;
    PlotWindows plots;
    PlotInterval shown{0,0},draft{0,0};
    int requested_tab=-1;
    bool visible=false,focus=false;
};

}
