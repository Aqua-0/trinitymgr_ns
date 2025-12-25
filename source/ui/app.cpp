// path: source/ui/app.cpp
#include <switch.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <map>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <malloc.h>
#include <utility>
#include <ctime>
#include <curl/curl.h>

#include "gfx/gfx.hpp"
#include "fs/fs_utils.hpp"
#include "net/netlog.hpp"
#include "gb/gamebanana.hpp"
#include "mod/mods.hpp"

#include "ui/thumb_cache.hpp"

#ifndef STBI_NO_THREAD_LOCALS
#define STBI_NO_THREAD_LOCALS
#endif
#include "stb_image.h"

struct SDL_Rect { int x, y, w, h; };
using SDL_Color = gfx::Color;

// ---------------- fs helpers ----------------

static const char* AppletTypeName(AppletType t){
    switch(t){
        case AppletType_None: return "None";
        case AppletType_Default: return "Default";
        case AppletType_SystemApplet: return "SystemApplet";
        case AppletType_LibraryApplet: return "LibraryApplet";
        case AppletType_OverlayApplet: return "OverlayApplet";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_Application: return "Application";
        default: return "Unknown";
    }
}

static const char* FocusStateName(AppletFocusState s){
    switch(s){
        case AppletFocusState_InFocus: return "InFocus";
        case AppletFocusState_OutOfFocus: return "OutOfFocus";
        case AppletFocusState_Background: return "Background";
        default: return "Unknown";
    }
}

namespace ui {
enum class Screen{ Mods, Target, Apply, Browse, Profiles, Settings, Log, About };

struct App {
    static std::string tailLines(const std::string& s, int maxLines){
        if(maxLines <= 0 || s.empty()) return {};
        int lines = 0;
        for(size_t i=s.size(); i>0; --i){
            if(s[i-1] == '\n'){
                lines++;
                if(lines >= maxLines){
                    return s.substr(i);
                }
            }
        }
        return s;
    }
    static constexpr int WIN_W=1280, WIN_H=720;
    static constexpr int NAV_W=280;
    static constexpr u64 ZA_TID = 0x0100F43008C44000ULL;
    static constexpr u64 SV_VIOLET_TID  = 0x01008F6008C5E000ULL;
    static constexpr u64 SV_SCARLET_TID = 0x0100A3D008C5C000ULL;
    static constexpr int GB_GAME_ZA = 23582;
    static constexpr int GB_GAME_SV = 17220;

    static constexpr const char* SETTINGS_PATH = "sdmc:/switch/trinitymgr_ns/settings.txt";
    static constexpr const char* SELECTION_CACHE_PATH_ZA = "sdmc:/switch/trinitymgr_ns/last_modset.txt";
    static constexpr const char* PROFILE_DIR_ZA = "sdmc:/switch/trinitymgr_ns/profiles";
    static constexpr const char* SELECTION_CACHE_PATH_SV = "sdmc:/switch/trinitymgr_ns/last_modset_sv.txt";
    static constexpr const char* PROFILE_DIR_SV = "sdmc:/switch/trinitymgr_ns/profiles_sv";

    static constexpr const char* MODS_PARENT_DIR = "sdmc:/switch/trinitymgr_ns";
    static constexpr const char* MODS_ROOT_NEW   = "sdmc:/switch/trinitymgr_ns/PLZAMods";
    static constexpr const char* MODS_ROOT_LEGACY = "sdmc:/switch/PLZAMods";
    static constexpr const char* MODS_ROOT_SV = "sdmc:/switch/trinitymgr_ns/SVMods";
    static constexpr const char* SELF_NRO_PATH = "sdmc:/switch/trinitymgr_ns/trinitymgr_ns.nro";
    static constexpr const char* UPDATE_ASSET_HINT = "trinitymgr_ns";
    static constexpr const char* DEFAULT_UPDATE_FEED = "https://api.github.com/repos/Aqua-0/trinitymgr_ns/releases/latest";
    static constexpr const char* APP_VERSION_STRING = "2.0";

    enum class ActiveGame { ZA, SV };
    enum class SvTitle { Violet, Scarlet };
    ActiveGame active_game = ActiveGame::ZA;
    SvTitle sv_title = SvTitle::Violet;

    int gb_game_id = GB_GAME_ZA;
    std::string mods_root    = MODS_ROOT_NEW;
    std::string self_nro_path = SELF_NRO_PATH;
    std::string target_romfs;
    std::string selection_cache_path = SELECTION_CACHE_PATH_ZA;
    std::string profile_dir = PROFILE_DIR_ZA;
    std::vector<mods::ModEntry> modlist;
    int menu_cursor=0;
    int mod_cursor=0, mod_scroll=0;
    int gb_page = 0;          // current 0-based page
    int browse_cursor = 0; 
    int log_scroll = 0;
    std::atomic<bool> downloading{false};
    std::atomic<bool> dl_done{false};
    std::atomic<bool> dl_failed{false};
    gb::DlProg dl_prog{};
    Thread  dl_thread{};            // libnx thread
    bool    dl_thread_alive = false;
    void*   dl_stack = nullptr;     // allocated stack
    Thread  app_thread{}; bool app_thread_alive=false; void* app_stack=nullptr;
    static constexpr size_t APP_STACK_SZ = 256 * 1024;
    std::atomic<bool> applying{false};
    std::atomic<bool> app_done{false};
    std::atomic<bool> app_failed{false};
    mods::ApplyProg app_prog{};
    static constexpr int GB_PER_PAGE = 5;
    static constexpr size_t DL_STACK_SZ = 256 * 1024; // 256 KiB
    static constexpr size_t RESCAN_STACK_SZ = 256 * 1024;
    static constexpr size_t GB_STACK_SZ = 256 * 1024;
    static constexpr size_t INFO_STACK_SZ = 128 * 1024;
    static constexpr size_t LEGACY_COPY_STACK_SZ = 256 * 1024;
    static constexpr size_t UPDATE_STACK_SZ = 256 * 1024;
    gb::ModItem dl_item{};
    std::string dl_saved;          // final path
    std::string dl_thread_log;
    gb::DlProg update_prog{};
    std::string app_thread_log;
    std::vector<mods::ModEntry> app_mods_snapshot;
    Screen screen=Screen::Mods;
    bool running=true;
    bool clear_target_before_apply=false;
    bool rescan_pending=false;
    std::string log;

    std::vector<gb::ModItem> gb_items;
    std::vector<mods::ModEntry> rescan_result;
    std::string rescan_thread_log;
    Thread rescan_thread{}; bool rescan_thread_alive=false; void* rescan_stack=nullptr;
    std::atomic<bool> rescanning{false};
    std::atomic<bool> rescan_done{false};
    std::atomic<bool> rescan_failed{false};
    mods::Game rescan_game = mods::Game::ZA;
    std::string rescan_root;

    Thread gb_thread{}; bool gb_thread_alive=false; void* gb_stack=nullptr;
    std::atomic<bool> gb_fetching{false};
    std::atomic<bool> gb_done{false};
    std::atomic<bool> gb_failed{false};
    int gb_request_page=0;
    int gb_request_game_id = GB_GAME_ZA;
    int gb_result_page = 0;
    int gb_result_game_id = GB_GAME_ZA;
    int gb_pending_page=-1;
    std::string gb_thread_log;
    std::string gb_status;
    std::vector<gb::ModItem> gb_thread_items;
    struct ModInfoCacheEntry{
        std::string desc;
        std::vector<gb::CreditGroup> credits;
        std::vector<gb::GalleryImage> images;
        std::vector<gb::FileEntry> files;
    };
    struct LegacyCopyOp{
        std::string src;
        std::string dst;
        long long size=0;
    };
    struct LegacyCopyProg{
        std::atomic<size_t> files_total{0};
        std::atomic<size_t> files_done{0};
        std::atomic<long long> bytes_total{0};
        std::atomic<long long> bytes_done{0};
        std::atomic<bool> cancel{false};
    };
    std::unordered_map<int,ModInfoCacheEntry> gb_info_cache;
    std::deque<int> gb_info_lru;
    static constexpr size_t GB_INFO_CACHE_LIMIT = 64;

    ThumbCache thumb_cache;

    gfx::Canvas   canvas;
    gfx::Font     font;
    gfx::Font     font_small;
    gfx::Texture  boot_bg_tex;
    bool          boot_bg_ready=false;
    std::string   boot_bg_label;
    gfx::DekoPresenter presenter;
    int nxlink_sock=-1;
    u64 log_frame_counter=0;
    u64 log_last_flush_frame=0;
    size_t log_lines_bytes=0;
    std::deque<std::string> log_lines;
    std::string log_partial_line;
    std::string log_disk_buffer;
    std::vector<std::pair<std::string, std::vector<int>>> conflict_cache;
    bool conflicts_dirty=true;
    AppletHookCookie hookCookie{};
    AppletFocusState lastFocusState=AppletFocusState_InFocus;
    AppletOperationMode lastOpMode=AppletOperationMode_Handheld;
    bool touch_active=false;
    float touch_last_x=0.0f;
    float touch_last_y=0.0f;
    bool touch_controls_enabled=true;
    int dpad_repeat_dir=0;
    int dpad_repeat_timer=0;
    int tab_stick_dir=0;
    int tab_stick_timer=0;

#ifdef APP_DEBUG_BUILD
    std::vector<std::string> menu={"Mods","Target","Apply","Browse","Profiles","Settings","Log","About","Exit"};
    static constexpr int LOG_TAB_INDEX = 6;
#else
    std::vector<std::string> menu={"Mods","Target","Apply","Browse","Profiles","Settings","About","Exit"};
    static constexpr int LOG_TAB_INDEX = -1;
#endif
    static constexpr int APPLY_TAB_INDEX = 2;
    int profile_tab_index = 4;
    int profile_cursor=0, profile_scroll=0;
    std::vector<mods::ProfileInfo> profile_list;
    std::string profile_message;
    bool info_overlay=false;
    int info_mod_index=-1;
    int info_mod_id=0;
    std::atomic<bool> info_loading{false};
    bool info_failed=false;
    std::string info_desc;
    std::string info_thread_log;
    Thread info_thread{}; bool info_thread_alive=false; void* info_stack=nullptr;
    std::vector<gb::CreditGroup> info_authors;
    std::vector<gb::GalleryImage> info_gallery_images;
    std::vector<gb::FileEntry> info_files;
    int info_gallery_index=0;
    bool info_gallery_active=false;
    SDL_Rect info_image_rect{0,0,0,0};
    int info_desc_scroll=0;
    int info_desc_visible_lines=0;
    int info_desc_total_lines=0;
    int info_desc_scroll_hold_dir=0;
    int info_desc_scroll_hold_timer=0;
    int info_authors_scroll=0;
    int info_authors_visible_lines=0;
    int info_authors_total_lines=0;
    static constexpr int INFO_SCROLL_REPEAT_FRAMES = 6;
    static constexpr int DPAD_REPEAT_DELAY_FRAMES = 12;
    static constexpr int DPAD_REPEAT_RATE_FRAMES = 3;
    static constexpr int TAB_STICK_DEADZONE = 14000;
    static constexpr int TAB_STICK_REPEAT_FRAMES = 12;
    bool file_picker_open=false;
    int file_picker_cursor=0;
    int file_picker_scroll=0;
    SDL_Rect file_picker_panel_rect{0,0,0,0};
    SDL_Rect file_picker_list_rect{0,0,0,0};
    std::vector<gb::FileEntry> file_picker_files;
    int file_picker_target_index=-1;
    bool dl_has_file_choice=false;
    gb::FileEntry dl_file_choice;
    bool update_checking=false;
    bool update_available=false;
    std::string update_status;
    std::string update_download_url;
    std::string update_download_error;
    std::atomic<bool> update_downloading{false};
    std::atomic<bool> update_download_done{false};
    std::atomic<bool> update_download_failed{false};
    Thread update_thread{}; bool update_thread_alive=false; void* update_stack=nullptr;
    LegacyCopyProg legacy_prog;
    std::vector<LegacyCopyOp> legacy_copy_plan;
    std::atomic<bool> legacy_copying{false};
    std::atomic<bool> legacy_copy_done{false};
    std::atomic<bool> legacy_copy_failed{false};
    std::atomic<bool> legacy_copy_cancelled{false};
    std::string legacy_copy_error;
    Thread legacy_thread{}; bool legacy_thread_alive=false; void* legacy_stack=nullptr;
    std::string settings_message;
    static constexpr size_t LOG_WINDOW_BYTES = 256 * 1024;
    static constexpr size_t LOG_FLUSH_CHUNK_BYTES = 32 * 1024;
    static constexpr size_t LOG_MAX_FILE_BYTES = 3 * 1024 * 1024;
    static constexpr u64 LOG_FLUSH_INTERVAL_FRAMES = 1800;
    static constexpr int firstTab=0;
    static void s_dl_entry(void* arg){
        static_cast<App*>(arg)->dl_worker();
    }
    static void s_rescan_entry(void* arg){
        static_cast<App*>(arg)->rescan_worker();
    }
    static void s_gb_entry(void* arg){
        static_cast<App*>(arg)->gb_worker();
    }
    static void s_info_entry(void* arg){
        static_cast<App*>(arg)->info_worker();
    }
    static void s_legacy_copy_entry(void* arg){
        static_cast<App*>(arg)->legacy_copy_worker();
    }
    static void s_update_entry(void* arg){
        static_cast<App*>(arg)->update_download_worker();
    }

    #include "ui/app_logic.inl"
    #include "ui/app_draw.inl"
    static bool launch_title_id(u64 tid, std::string& log){
        // Works only when running in Application mode (title override / forwarder NSP).
        AppletStorage args{};
        Result rc = appletCreateStorage(&args, 0);  // no user args
        if (R_FAILED(rc)){
            char b[96]; snprintf(b,sizeof(b),"[launch] appletCreateStorage rc=0x%08x\n", rc);
            log += b; return false;
        }

        rc = appletRequestLaunchApplication(tid, &args);
        appletStorageClose(&args);

        if (R_SUCCEEDED(rc)) return true;

        char b[96]; snprintf(b,sizeof(b),"[launch] appletRequestLaunchApplication rc=0x%08x\n", rc);
        log += b;
        log += "[launch] hint: run as Application (Album applet cannot launch titles)\n";
        return false;
    }

    #include "ui/app_input.inl"
    void loop(){
        PadState pad{}; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
        while (running && appletMainLoop()){
            log_frame_counter++;
            pollTouch();
            padUpdate(&pad);
            u64 kdown = padGetButtonsDown(&pad);
            u64 kheld = padGetButtons(&pad);
            HidAnalogStickState leftStick = padGetStickPos(&pad, 0);
            handlePad(kdown, kheld, leftStick);
            if (dl_done.exchange(false)) {
                if (!dl_thread_log.empty()) { log += dl_thread_log; dl_thread_log.clear(); }
                if (!dl_saved.empty()) {
                    log += "[gb] download complete: " + dl_saved + "\n";
                    requestRescan();
                }
            }
            if (dl_failed.exchange(false)) {
                if (!dl_thread_log.empty()) { log += dl_thread_log; dl_thread_log.clear(); }
                log += "[gb] download failed or canceled\n";
            }
            if(update_download_done.exchange(false)){
                log += "[update] download+install complete\n";
                update_available = false;
                settings_message = "Update installed. Restart to use new version.";
                update_status = settings_message;
            }
            if(update_download_failed.exchange(false)){
                std::string msg = update_download_error.empty() ? "Update download failed." : update_download_error;
                log += "[update] " + msg + "\n";
                settings_message = msg;
            }
            // apply completion
            if (app_done.exchange(false))  {
                if(!app_thread_log.empty()){ log += app_thread_log; app_thread_log.clear(); }
                log += "[apply] complete\n";
                releaseApplySnapshot();
            }
            if (app_failed.exchange(false)){
                if(!app_thread_log.empty()){ log += app_thread_log; app_thread_log.clear(); }
                log += "[apply] failed or canceled\n";
                releaseApplySnapshot();
            }
            if (rescan_done.exchange(false)){
                if(!rescan_thread_log.empty()){ log += rescan_thread_log; rescan_thread_log.clear(); }
                if(rescan_game != activeModsGame() || rescan_root != mods_root){
                    log += "[scan] stale result discarded\n";
                    rescan_result.clear();
                }else{
                    modlist = std::move(rescan_result);
                    for(auto& m:modlist) m.selected = true;
                    mod_cursor = 0;
                    mod_scroll = 0;
                    restoreSelectionCache();
                    markConflictsDirty();
                }
            }
            if (rescan_failed.exchange(false)){
                // message already logged in startRescanThread
            }
            if(rescan_pending && !rescanning.load() && !rescan_thread_alive && !downloadsActive()){
                rescan_pending = false;
                startRescanThread();
            }
            if (gb_done.exchange(false)){
                std::string tlog;
                tlog.swap(gb_thread_log);
                if(!tlog.empty()){
                    gb_status = tailLines(tlog, 4);
                    log += tlog;
                }
                if(gb_result_game_id != gb_game_id){
                    log += "[gb] stale page discarded\n";
                    gb_thread_items.clear();
                }else{
                    gb_page = gb_result_page;
                    gb_items = std::move(gb_thread_items);
                    browse_cursor = gb_items.empty() ? 0 : std::min(browse_cursor, (int)gb_items.size()-1);
                }
                if(gb_pending_page >= 0 && !gb_fetching.load() && !gb_thread_alive){
                    int next = gb_pending_page;
                    gb_pending_page = -1;
                    startGbThread(next);
                }
            }
            if (gb_failed.exchange(false)){
                if(gb_status.empty()) gb_status = "[gb] request failed";
                if(gb_pending_page >= 0 && !gb_fetching.load() && !gb_thread_alive){
                    int next = gb_pending_page;
                    gb_pending_page = -1;
                    startGbThread(next);
                }
            }
            if(!info_thread_alive && !info_thread_log.empty()){
                log += info_thread_log;
                info_thread_log.clear();
            }
            if (!app_thread_alive && app_thread.handle){
                threadClose(&app_thread);
                memset(&app_thread, 0, sizeof(app_thread));
                if(app_stack){ free(app_stack); app_stack=nullptr; }
            }
            // after handling dl_done/dl_failed:
            if (!dl_thread_alive && dl_thread.handle){
                threadClose(&dl_thread);
                memset(&dl_thread, 0, sizeof(dl_thread));
                if(dl_stack){ free(dl_stack); dl_stack=nullptr; }
            }
            if (!rescan_thread_alive && rescan_thread.handle){
                threadClose(&rescan_thread);
                memset(&rescan_thread, 0, sizeof(rescan_thread));
                if(rescan_stack){ free(rescan_stack); rescan_stack=nullptr; }
            }
            if (!gb_thread_alive && gb_thread.handle){
                threadClose(&gb_thread);
                memset(&gb_thread, 0, sizeof(gb_thread));
                if(gb_stack){ free(gb_stack); gb_stack=nullptr; }
            }
            if (!info_thread_alive && info_thread.handle){
                threadClose(&info_thread);
                memset(&info_thread, 0, sizeof(info_thread));
                if(info_stack){ free(info_stack); info_stack=nullptr; }
            }
            if (!update_thread_alive && update_thread.handle){
                threadClose(&update_thread);
                memset(&update_thread, 0, sizeof(update_thread));
                if(update_stack){ free(update_stack); update_stack=nullptr; }
            }
            drainLogInput();
            maybeFlushLogToFile();
            render();
        }
    }
};
} // namespace ui

int main(int argc, char** argv){
    ui::App app;
    if(argc > 0 && argv && argv[0]){
        app.overrideSelfNroPath(argv[0]);
    }
    if(!app.init()) return 0;
    app.loop();
    app.shutdown();
    return 0;
}
