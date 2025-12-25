#pragma once

#include <map>
#include <list>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "gfx/gfx.hpp"

namespace ui {

class ThumbCache {
public:
    struct Entry {
        gfx::Texture tex;
    };

    bool ensure(const std::string& mods_root, int gb_game_id, int id, const std::string& url_in, std::string& log);
    bool ensureLarge(const std::string& mods_root, int gb_game_id, int id, const std::string& url_in, std::string& log);
    void clear();
    void clearLarge();

    std::map<int, Entry> small;
    std::map<int, Entry> large;
    std::unordered_set<int> tried;
private:
    static constexpr size_t kMaxSmallEntries = 64;
    static constexpr size_t kMaxLargeEntries = 4;
    std::list<int> small_lru;
    std::list<int> large_lru;
    std::unordered_map<int, std::list<int>::iterator> small_pos;
    std::unordered_map<int, std::list<int>::iterator> large_pos;
    void touchSmall(int id);
    void touchLarge(int id);
    void enforceSmallLimit();
    void enforceLargeLimit();
};

} // namespace ui
