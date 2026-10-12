#pragma once
#include "network.h"
#include <filesystem>
namespace studio {
std::string serialize(const Config& c);
Config deserialize(const std::string& text);
Config load_config(const std::filesystem::path& path);
void save_config(const Config& c,const std::filesystem::path& path);
void export_results(const Simulator& s,const std::filesystem::path& prefix);
}
