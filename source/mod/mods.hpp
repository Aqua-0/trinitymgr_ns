#pragma once

#include <string>
#include <vector>
#include <atomic>

namespace mods {

enum class Game {
    ZA,
    SV,
};

struct ModEntry{
    std::string name;
    std::string path;
    std::string romfs;
    std::string exefs;
    std::vector<std::string> files; // "romfs/<rel>" or "exefs/<rel>"
    bool selected=false;
};

std::vector<ModEntry> scan_root(const std::string& root, std::string& log, Game game=Game::ZA);
bool clear_target_known(const std::string& target_romfs, std::string& log, Game game=Game::ZA);
std::vector<std::pair<std::string, std::vector<int>>> compute_conflicts(const std::vector<ModEntry>& mods);
bool apply_copy_progress(const std::vector<ModEntry>& mods,
                         const std::string& target_romfs,
                         std::string& log,
                         struct ApplyProg* prog);

bool save_selection_cache(const std::vector<ModEntry>& mods,
                          const std::string& cache_path,
                          std::string& log);
bool load_selection_cache(const std::string& cache_path,
                          std::vector<ModEntry>& mods,
                          std::string& log);
struct ProfileInfo {
    std::string id;
    std::string label;
};
bool list_selection_profiles(const std::string& dir,
                             std::vector<ProfileInfo>& out,
                             std::string& log);
bool save_selection_profile(const std::vector<ModEntry>& mods,
                            const std::string& dir,
                            const std::string& display_name,
                            std::string& log);
bool load_selection_profile(const std::string& dir,
                            const std::string& id,
                            std::vector<ModEntry>& mods,
                            std::string& log);
bool delete_selection_profile(const std::string& dir,
                              const std::string& id,
                              std::string& log);

struct ApplyProg {
    std::atomic<size_t> done{0};
    std::atomic<size_t> total{0};
    std::atomic<long long> bytes{0};
    std::atomic<bool> cancel{false};
};

} // namespace mods
