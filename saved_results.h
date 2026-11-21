#pragma once
#include "simulation_view.h"
#include <filesystem>

namespace studio {

struct SavedResults {
    std::filesystem::path prefix;
    SimulationView view;
};

// Read the files from "Export CSV" without constructing or stepping a Simulator.
SavedResults load_saved_results(const std::filesystem::path& selected_file);

}
