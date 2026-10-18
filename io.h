#pragma once
#include "network.h"
#include <filesystem>
#include <memory>
namespace studio {
std::string serialize(const Config& c);
Config deserialize(const std::string& text);
Config load_config(const std::filesystem::path& path);
void save_config(const Config& c,const std::filesystem::path& path);
std::string analysis_settings_json(const AnalysisSettings& settings);
AnalysisSettings analysis_settings_from_json(const std::string& text);
std::string legacy_binding_config(const Config& config);
std::string serialize_runtime(const Simulator& s);
void deserialize_runtime(Simulator& s,const std::string& text);
std::string serialize_stimulus_runtime(const Simulator& s);
void deserialize_stimulus_runtime(Simulator& s,const std::string& text);
void load_stimulator_replay(Simulator& s,const std::filesystem::path& path);
void export_results(const Simulator& s,const std::filesystem::path& prefix);
// Separate binary document, safe only outside step(); errors restore read-only.
inline constexpr char CheckpointAlgorithm[]="event-rk4-doubling-exact-linear-localized-stimulators-v4";
inline constexpr size_t MaxCheckpointBytes=512*1024*1024;
void save_checkpoint(const Simulator& s,const std::filesystem::path& path);
std::unique_ptr<Simulator> load_checkpoint(const std::filesystem::path& path);
// Shared atomic sibling-file replacement for configs, exports and checkpoints.
void write_atomic(const std::filesystem::path& path,const std::string& bytes);
}
