#pragma once

#include <string>
#include <vector>
#include <atomic>

namespace gb {

struct ModItem {
    int id=0;
    std::string name;
    std::string profile;
    std::string thumb;
};

struct CreditAuthor {
    std::string role;
    std::string name;
};

struct CreditGroup {
    std::string title;
    std::vector<CreditAuthor> authors;
};

struct GalleryImage {
    int key=0;
    std::string url;
};

struct FileEntry {
    int id=0;
    std::string name;
    std::string description;
    std::string url;
    long long size_bytes=0;
    long long timestamp=0;
    bool has_contents=false;
};

struct DlProg {
    std::atomic<long long> now{0};
    std::atomic<long long> total{0};
    std::atomic<bool>      cancel{false};
};

std::string json_unescape(const std::string& s);
std::string normalize_url(std::string u);
bool http_get(const std::string& url, std::string& body, std::string& log);
std::vector<ModItem> fetch_mods_index_page(std::string& log, int limit=10, int page=1, int game_id=23582);
bool fetch_mod_thumb_url(int mod_id, std::string& out_url, std::string& log);
bool http_get_bytes_ref(const std::string& in_url,
                        const std::string& referer,
                        std::vector<unsigned char>& out,
                        std::string& log);
bool fetch_mod_description(int mod_id,
                           std::string& out_desc,
                           std::vector<CreditGroup>* out_credits,
                           std::vector<GalleryImage>* out_images,
                           std::vector<FileEntry>* out_files,
                           std::string& log);
bool download_primary_zip(const ModItem& item,
                          const std::string& mods_root,
                          std::string& saved_path,
                          std::string& log,
                          DlProg* prog=nullptr,
                          const FileEntry* preferred_file=nullptr);

} // namespace gb
