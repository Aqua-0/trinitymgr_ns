#pragma once

    void overrideSelfNroPath(const char* raw){
        if(!raw || !*raw) return;
        std::string path(raw);
        for(char& c : path){
            if(c=='\\') c='/';
        }
        if(path.rfind("sdmc:/", 0) != 0) return;
        if(path.find(".nro") == std::string::npos) return;
        self_nro_path = path;
    }

    void dl_worker(){
        std::string tlog, saved;
        const gb::FileEntry* choice = dl_has_file_choice ? &dl_file_choice : nullptr;
        bool ok = gb::download_primary_zip(dl_item, mods_root, saved, tlog, &dl_prog, choice);
        dl_has_file_choice = false;
        dl_thread_log = std::move(tlog);
        if (ok) { dl_saved = saved; dl_done = true; }
        else    { dl_failed = true; }
        downloading = false;
        dl_thread_alive = false; // signal finished
    }
    void rescan_worker(){
        std::string tlog;
        tlog += "[scan] root=" + mods_root + "\n";
        auto mods = mods::scan_root(mods_root, tlog);
        tlog += "[scan] found=" + std::to_string(mods.size()) + "\n";
        rescan_result = std::move(mods);
        rescan_thread_log = std::move(tlog);
        rescan_done = true;
        rescanning = false;
        rescan_thread_alive = false;
    }
    void gb_worker(){
        std::string tlog;
        gb_thread_items = gb::fetch_mods_index_page(tlog, GB_PER_PAGE, gb_request_page + 1);
        gb_thread_log = std::move(tlog);
        gb_done = true;
        gb_fetching = false;
        gb_thread_alive = false;
    }
    void info_worker(){
        int target = info_mod_id;
        std::string tlog;
        std::string desc;
        std::vector<gb::CreditGroup> credits;
        std::vector<gb::GalleryImage> images;
        std::vector<gb::FileEntry> files;
        bool ok = gb::fetch_mod_description(target, desc, &credits, &images, &files, tlog);
        info_thread_log = std::move(tlog);
        if(ok){
            ModInfoCacheEntry entry;
            entry.desc = desc;
            entry.credits = credits;
            entry.images = images;
            entry.files = files;
            gb_info_cache[target] = entry;
            gb_info_lru.push_back(target);
            while(gb_info_cache.size() > GB_INFO_CACHE_LIMIT && !gb_info_lru.empty()){
                int evict = gb_info_lru.front();
                gb_info_lru.pop_front();
                if(evict == target) continue;
                auto it = gb_info_cache.find(evict);
                if(it != gb_info_cache.end()){
                    gb_info_cache.erase(it);
                    break;
                }
            }
            if(info_mod_id == target){
                info_desc = desc;
                info_authors = credits;
                info_gallery_images = images;
                info_files = files;
                if(info_gallery_images.empty()){
                    if(info_mod_index >=0 && info_mod_index < (int)gb_items.size()){
                        gb::GalleryImage gi;
                        gi.key = gb_items[info_mod_index].id;
                        gi.url = gb_items[info_mod_index].thumb;
                        info_gallery_images.push_back(std::move(gi));
                    }
                }
                info_failed = false;
                resetInfoDescScroll();
            }
        }else{
            if(info_mod_id == target){
                info_desc.clear();
                info_authors.clear();
                info_gallery_images.clear();
                info_files.clear();
                info_failed = true;
                resetInfoDescScroll();
            }
        }
        info_loading = false;
        info_thread_alive = false;
    }
    void legacy_copy_worker(){
        std::vector<LegacyCopyOp> plan;
        plan.swap(legacy_copy_plan);
        bool canceled=false;
        bool ok=true;
        std::string err;
        for(size_t i=0;i<plan.size();++i){
            if(legacy_prog.cancel.load(std::memory_order_relaxed)){
                canceled = true;
                break;
            }
            auto res = copy_file_with_progress(plan[i], err);
            if(res == LegacyCopyResult::Canceled){
                canceled = true;
                break;
            }
            if(res == LegacyCopyResult::Error){
                ok=false;
                break;
            }
            legacy_prog.files_done.fetch_add(1, std::memory_order_relaxed);
        }
        if(canceled){
            legacy_copy_cancelled = true;
        }else if(!ok){
            legacy_copy_error = err;
            legacy_copy_failed = true;
        }else{
            legacy_copy_done = true;
            log += "[mods] copied legacy PLZAMods into new root\n";
        }
        legacy_copying = false;
        legacy_thread_alive = false;
    }
    void update_download_worker(){
        std::string tmp = std::string(MODS_PARENT_DIR) + "/trinity_update.tmp";
        std::string err;
        bool ok = downloadUpdateFile(update_download_url, tmp, err);
        if(ok){
            if(!installDownloadedUpdate(tmp, err)){
                ok = false;
            }
        }
        if(!ok){
            update_download_error = err.empty() ? "Update download failed." : err;
            update_download_failed = true;
            unlink(tmp.c_str());
        }else{
            update_download_done = true;
        }
        update_downloading = false;
        update_thread_alive = false;
    }
    static void s_app_entry(void* arg){ static_cast<App*>(arg)->app_worker(); }
    static void s_applet_hook(AppletHookType type, void* param){
        static_cast<App*>(param)->handleAppletHook(type);
    }
    void handleAppletHook(AppletHookType type){
        if(type==AppletHookType_OnFocusState){
            AppletFocusState st = appletGetFocusState();
            if(st!=lastFocusState){
                lastFocusState = st;
                char buf[96];
                snprintf(buf,sizeof(buf),"[applet] focus=%s\n", FocusStateName(st));
                log += buf;
            }
        }else if(type==AppletHookType_OnOperationMode){
            AppletOperationMode mode = appletGetOperationMode();
            if(mode!=lastOpMode){
                lastOpMode = mode;
                char buf[96];
                snprintf(buf,sizeof(buf),"[applet] operationMode=%d\n", static_cast<int>(mode));
                log += buf;
            }
        }
    }
    void app_worker(){
        std::string tlog;
        bool ok = mods::apply_copy_progress(app_mods_snapshot, target_romfs, tlog, &app_prog);
        app_thread_log = std::move(tlog);
        if(ok) app_done = true; else app_failed = true;
        applying = false; app_thread_alive = false;
    }
    void startRescanThread(){
        rescanning = true;
        rescan_done = false;
        rescan_failed = false;
        rescan_thread_log.clear();
        rescan_result.clear();
        rescan_stack = memalign(0x1000, RESCAN_STACK_SZ);
        if(!rescan_stack){
            rescanning = false;
            rescan_failed = true;
            log += "[scan] memalign failed\n";
            return;
        }
        Result rc = threadCreate(&rescan_thread, s_rescan_entry, this, rescan_stack, RESCAN_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            rescanning = false;
            rescan_failed = true;
            char buf[96]; snprintf(buf,sizeof(buf),"[scan] threadCreate failed rc=0x%08X\n", rc);
            log += buf;
            free(rescan_stack); rescan_stack=nullptr;
            return;
        }
        rescan_thread_alive = true;
        threadStart(&rescan_thread);
    }
    bool downloadsActive() const {
        return downloading.load(std::memory_order_relaxed) || update_downloading.load(std::memory_order_relaxed);
    }
    void requestRescan(){
        if(downloadsActive()){
            rescan_pending = true;
            log += "[scan] download in progress; rescan deferred\n";
            return;
        }
        if(rescanning.load() || rescan_thread_alive){
            rescan_pending = true;
            log += "[scan] already running; queued another\n";
            return;
        }
        rescan_pending = false;
        log += "[scan] starting rescan\n";
        startRescanThread();
    }
    void startGbThread(int page){
        if(downloadsActive()){
            gb_fetching = false;
            gb_pending_page = page;
            log += "[gb] deferred fetch until downloads finish\n";
            return;
        }
        gb_fetching = true;
        gb_done = false;
        gb_failed = false;
        gb_thread_log.clear();
        gb_thread_items.clear();
        gb_request_page = page;
        gb_pending_page = -1;
        gb_stack = memalign(0x1000, GB_STACK_SZ);
        if(!gb_stack){
            gb_fetching = false;
            gb_failed = true;
            log += "[gb] memalign failed for fetch\n";
            return;
        }
        Result rc = threadCreate(&gb_thread, s_gb_entry, this, gb_stack, GB_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            gb_fetching = false;
            gb_failed = true;
            char buf[96]; snprintf(buf,sizeof(buf),"[gb] threadCreate failed rc=0x%08X\n", rc);
            log += buf;
            free(gb_stack); gb_stack=nullptr;
            return;
        }
        gb_thread_alive = true;
        threadStart(&gb_thread);
    }
    void startInfoThread(int mod_id){
        if(info_thread_alive){
            threadWaitForExit(&info_thread);
            threadClose(&info_thread);
            info_thread_alive=false;
        }else if(info_thread.handle){
            threadClose(&info_thread);
            memset(&info_thread, 0, sizeof(info_thread));
        }
        if(info_stack){ free(info_stack); info_stack=nullptr; }
        info_mod_id = mod_id;
        info_loading = true;
        info_failed = false;
        info_thread_log.clear();
        info_desc.clear();
        info_authors.clear();
        info_gallery_images.clear();
        resetInfoDescScroll();
        info_stack = memalign(0x1000, INFO_STACK_SZ);
        if(!info_stack){
            info_loading = false;
            info_failed = true;
            log += "[info] memalign failed\n";
            return;
        }
        Result rc = threadCreate(&info_thread, s_info_entry, this, info_stack, INFO_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            info_loading = false;
            info_failed = true;
            char buf[96]; snprintf(buf,sizeof(buf),"[info] threadCreate failed rc=0x%08X\n", rc); log+=buf;
            free(info_stack); info_stack=nullptr;
            return;
        }
        info_thread_alive = true;
        threadStart(&info_thread);
    }
    void requestGbFetch(int page, bool force_refresh=false){
        if(page < 0) page = 0;
        bool busy = gb_fetching.load() || gb_thread_alive;
        gb_page = page;
        if(downloadsActive()){
            gb_pending_page = page;
            log += "[gb] download in progress; fetch deferred\n";
            return;
        }
        if(busy){
            gb_pending_page = page;
            return;
        }
        gb_items.clear();
        startGbThread(page);
    }
    bool init(){
        socketInitializeDefault();
        Result rc=fsdevMountSdmc(); if(R_FAILED(rc)) printf("[warn] fsdevMountSdmc failed: 0x%x\n",rc);
        nxlink_sock = netlog::connectWithCache(log);
        romfsInit();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        hidInitializeTouchScreen();
        hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard);
        appletSetScreenShotPermission(AppletScreenShotPermission_Enable);
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
        AppletType atype = appletGetAppletType();
        AppletOperationMode opmode = appletGetOperationMode();
        AppletFocusState fstate = appletGetFocusState();
        lastOpMode = opmode;
        lastFocusState = fstate;
        appletHook(&hookCookie, s_applet_hook, this);
        char appInfo[160];
        snprintf(appInfo, sizeof(appInfo), "[applet] type=%s isApp=%d opMode=%d focus=%s\n",
                 AppletTypeName(atype),
                 atype==AppletType_Application || atype==AppletType_SystemApplication,
                 static_cast<int>(opmode),
                 FocusStateName(fstate));
        log += appInfo;
#ifdef APP_DEBUG_BUILD
        log += "[build] DEBUG palette\n";
#else
        log += "[build] RELEASE palette\n";
#endif
        NWindow* win = nwindowGetDefault();
        canvas.init(WIN_W, WIN_H);
        if(!presenter.init(win, WIN_W, WIN_H)){
            log += "[gfx] presenter init failed\n";
            return false;
        }
        presenter.setLogBuffer(&log);
        canvas.clear(themeCanvasBg());
        presenter.present(canvas.data().data(), canvas.data().size() * sizeof(uint32_t));
        const char* candidates[]={"romfs:/ui/Roboto-Regular.ttf","romfs:/ui/OpenSans-Regular.ttf","romfs:/ui/DejaVuSans.ttf","romfs:/font.ttf",nullptr};
        bool ok=false;
        for(int i=0;candidates[i];++i){
            if(font.load(candidates[i], 28, log)){ ok=true; break; }
        }
        if(!ok){
            log += "[font] unable to load primary font\n";
            return false;
        }
        if(!font_small.load("romfs:/ui/Roboto-Medium.ttf", 22, log)){
            font_small = font;
        }
        ensureModsRoot();
        loadBootBackgroundImage();
        showBlockingMessage("Loading Mods...");
        rescanMods();
        refreshProfileList();
        return true;
    }
    void shutdown(){
        persistSelectionCache();
        thumb_cache.clear();
        drainLogInput();
        if (downloading.load()){
            dl_prog.cancel = true;
        }
        if (dl_thread_alive){
            threadWaitForExit(&dl_thread);
            threadClose(&dl_thread);
            dl_thread_alive = false;
        }else if (dl_thread.handle){
            threadClose(&dl_thread);
        }
        if(dl_stack){ free(dl_stack); dl_stack=nullptr; }
        if (applying.load()) app_prog.cancel = true;
        if (app_thread_alive){
            threadWaitForExit(&app_thread);
            threadClose(&app_thread);
            app_thread_alive=false;
        }else if (app_thread.handle){
            threadClose(&app_thread);
        }
        if(app_stack){ free(app_stack); app_stack=nullptr; }
        releaseApplySnapshot();
        if (rescan_thread_alive){
            threadWaitForExit(&rescan_thread);
            threadClose(&rescan_thread);
            rescan_thread_alive=false;
        }else if(rescan_thread.handle){
            threadClose(&rescan_thread);
        }
        if(rescan_stack){ free(rescan_stack); rescan_stack=nullptr; }
        if (gb_thread_alive){
            threadWaitForExit(&gb_thread);
            threadClose(&gb_thread);
            gb_thread_alive=false;
        }else if(gb_thread.handle){
            threadClose(&gb_thread);
        }
        if (info_thread_alive){
            threadWaitForExit(&info_thread);
            threadClose(&info_thread);
            info_thread_alive=false;
        }else if(info_thread.handle){
            threadClose(&info_thread);
        }
        if(info_stack){ free(info_stack); info_stack=nullptr; }
        if(gb_stack){ free(gb_stack); gb_stack=nullptr; }
        drainLogInput();
        maybeFlushLogToFile(true);
        presenter.shutdown();
        gfx::ShutdownFreeType();
        romfsExit();
        fsdevUnmountAll();
        appletUnhook(&hookCookie);
        netlog::shutdown(nxlink_sock);
        socketExit();
    }

    void ensureModsRoot(){
        const std::string desired = MODS_ROOT_NEW;
        const std::string legacy  = MODS_ROOT_LEGACY;
        const std::string parent  = MODS_PARENT_DIR;
        if(!fsx::isdir(parent)){
            fsx::makedirs(parent);
        }
        if(fsx::isdir(desired)){
            mods_root = desired;
        }else if(fsx::isdir(legacy)){
            if(::rename(legacy.c_str(), desired.c_str())==0){
                mods_root = desired;
                log += "[mods] moved legacy PLZAMods into " + desired + "\n";
            }else{
                char buf[192];
                snprintf(buf, sizeof(buf), "[mods] move legacy -> %s failed (errno=%d)\n", desired.c_str(), errno);
                log += buf;
                mods_root = legacy;
            }
        }else{
            if(fsx::makedirs(desired)){
                log += "[mods] created mods root: " + desired + "\n";
            }else{
                log += "[mods] failed to create mods root: " + desired + "\n";
            }
            mods_root = desired;
        }
        if(!fsx::isdir(mods_root)){
            if(fsx::makedirs(mods_root)){
                log += "[mods] created missing mods root path\n";
            }
        }
        log += "[mods] using mods root: " + mods_root + "\n";
    }

    void rescanMods(){
        rescan_pending = false;
        log+="[scan] root="+mods_root+"\n";
        modlist=mods::scan_root(mods_root,log);
        for(auto& m:modlist) m.selected=true;
        mod_cursor=0; mod_scroll=0;
        log+="[scan] found="+std::to_string(modlist.size())+"\n";
        markConflictsDirty();
        restoreSelectionCache();
    }

    void refreshProfileList(){
        mods::list_selection_profiles(PROFILE_DIR, profile_list, log);
        if(profile_cursor >= (int)profile_list.size()){
            profile_cursor = std::max(0, (int)profile_list.size()-1);
        }
        if(profile_cursor < 0) profile_cursor = 0;
        if(profile_cursor < profile_scroll) profile_scroll = profile_cursor;
        int max_vis = profileVisibleCount();
        if(max_vis > 0 && profile_cursor >= profile_scroll + max_vis){
            profile_scroll = std::max(0, profile_cursor - max_vis + 1);
        }
    }
    int profileVisibleCount() const {
        const int top = 100;
        const int item_h = 34;
        return std::max(1, (WIN_H - top - 60) / item_h);
    }
    void setProfileMessage(const std::string& msg){
        profile_message = msg;
    }
    void loadProfileAtCursor(){
        if(profile_list.empty()) return;
        const auto& p = profile_list[profile_cursor];
        if(mods::load_selection_profile(PROFILE_DIR, p.id, modlist, log)){
            mod_cursor = 0; mod_scroll = 0;
            persistSelectionCache();
            markConflictsDirty();
            setProfileMessage("Loaded " + p.label);
        }else{
            setProfileMessage("Load failed");
        }
    }
    void overwriteProfileAtCursor(){
        if(profile_list.empty()) return;
        const auto& p = profile_list[profile_cursor];
        if(mods::save_selection_profile(modlist, PROFILE_DIR, p.label, log)){
            refreshProfileList();
            setProfileMessage("Updated " + p.label);
        }else{
            setProfileMessage("Save failed");
        }
    }
    std::string promptProfileName(const std::string& initial){
        SwkbdConfig kbd{};
        Result rc = swkbdCreate(&kbd, 0);
        if(R_FAILED(rc)) return {};
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetGuideText(&kbd, "Profile name");
        swkbdConfigSetInitialText(&kbd, initial.c_str());
        swkbdConfigSetStringLenMax(&kbd, 48);
        char out[64]{};
        rc = swkbdShow(&kbd, out, sizeof(out));
        swkbdClose(&kbd);
        if(R_FAILED(rc)) return {};
        return std::string(out);
    }
    void createProfilePrompt(){
        std::string seed = (profile_cursor >=0 && profile_cursor < (int)profile_list.size())
            ? profile_list[profile_cursor].label : "New profile";
        std::string name = promptProfileName(seed);
        if(name.empty()){
            setProfileMessage("Save canceled");
            return;
        }
        if(mods::save_selection_profile(modlist, PROFILE_DIR, name, log)){
            refreshProfileList();
            setProfileMessage("Saved " + name);
        }else{
            setProfileMessage("Save failed");
        }
    }
    void deleteProfileAtCursor(){
        if(profile_list.empty()) return;
        const auto p = profile_list[profile_cursor];
        if(mods::delete_selection_profile(PROFILE_DIR, p.id, log)){
            refreshProfileList();
            setProfileMessage("Deleted " + p.label);
        }else{
            setProfileMessage("Delete failed");
        }
    }

    void restoreSelectionCache(){
        if(modlist.empty()) return;
        mods::load_selection_cache(SELECTION_CACHE_PATH, modlist, log);
        markConflictsDirty();
    }
    void persistSelectionCache(){
        if(modlist.empty()) return;
        mods::save_selection_cache(modlist, SELECTION_CACHE_PATH, log);
    }
    void markConflictsDirty(){
        conflicts_dirty = true;
    }
    const std::vector<std::pair<std::string, std::vector<int>>>& getConflicts(){
        if(conflicts_dirty){
            conflict_cache = mods::compute_conflicts(modlist);
            conflicts_dirty = false;
        }
        return conflict_cache;
    }
    void releaseApplySnapshot(){
        if(!app_mods_snapshot.empty()){
            std::vector<mods::ModEntry>().swap(app_mods_snapshot);
        }
    }

    SDL_Color C(uint8_t r,uint8_t g,uint8_t b,uint8_t a=255) const { return SDL_Color{r,g,b,a}; }
    void fill(SDL_Color c, const SDL_Rect& r){ canvas.fillRect(r.x, r.y, r.w, r.h, c); }
    void fillRounded(SDL_Color c, const SDL_Rect& r, int radius=12){ canvas.fillRoundedRect(r.x, r.y, r.w, r.h, radius, c); }
    void text(const std::string& s, int x,int y, SDL_Color c, bool small=false){
        const gfx::Font& f = small ? font_small : font;
        if(!f.ready()) return;
        f.draw(canvas, s, x, y, c);
    }
    int textw(const std::string& s, bool small=false){
        const gfx::Font& f = small ? font_small : font;
        if(!f.ready()) return 0;
        return f.textWidth(s);
    }
    bool readFileAllBytes(const std::string& path, std::vector<unsigned char>& out){
        FILE* f = fopen(path.c_str(), "rb");
        if(!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if(sz <= 0){
            fclose(f);
            return false;
        }
        out.resize(sz);
        size_t rd = fread(out.data(),1,sz,f);
        fclose(f);
        return rd == static_cast<size_t>(sz);
    }
    void loadBootBackgroundImage(){
        boot_bg_ready = false;
        boot_bg_tex = gfx::Texture();
        const std::string dir = "romfs:/bg";
        std::string chosen;
        auto entries = fsx::list_dirents(dir);
        for(const auto& ent : entries){
            if(ent.is_file){
                chosen = ent.path;
                break;
            }
        }
        if(chosen.empty()){
            std::string fallback = dir + "/Opening.png";
            if(fsx::isfile(fallback)) chosen = fallback;
        }
        if(chosen.empty()) return;
        std::vector<unsigned char> bytes;
        if(!readFileAllBytes(chosen, bytes)){
            log += "[bg] read failed: " + chosen + "\n";
            return;
        }
        int w=0,h=0,n=0;
        stbi_uc* decoded = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
        if(!decoded){
            log += "[bg] decode failed: " + chosen + "\n";
            return;
        }
        if(w<=0 || h<=0){
            stbi_image_free(decoded);
            return;
        }
        boot_bg_tex.w = w;
        boot_bg_tex.h = h;
        boot_bg_tex.pixels.resize(static_cast<size_t>(w) * h);
        for(int y=0; y<h; ++y){
            for(int x=0; x<w; ++x){
                size_t idx = static_cast<size_t>(y) * w + x;
                stbi_uc* p = decoded + idx*4;
                boot_bg_tex.pixels[idx] = (uint32_t(p[3])<<24) | (uint32_t(p[0])<<16) |
                                           (uint32_t(p[1])<<8)  | uint32_t(p[2]);
            }
        }
        stbi_image_free(decoded);
        boot_bg_ready = true;
        size_t slash = chosen.find_last_of("/\\");
        boot_bg_label = (slash == std::string::npos) ? chosen : chosen.substr(slash+1);
    }
    void showBlockingMessage(const std::string& msg){
        canvas.clear(themeCanvasBg());
        if(boot_bg_ready && boot_bg_tex.valid()){
            int imgW = boot_bg_tex.w;
            int imgH = boot_bg_tex.h;
            if(imgW>0 && imgH>0){
                float scale = std::min((float)WIN_W / imgW, (float)WIN_H / imgH);
                scale *= 0.8f;
                if(scale <= 0.f) scale = 1.f;
                int drawW = std::max(1, static_cast<int>(imgW * scale));
                int drawH = std::max(1, static_cast<int>(imgH * scale));
                int dx = (WIN_W - drawW)/2;
                int dy = (WIN_H - drawH)/2;
                canvas.blitScaled(boot_bg_tex, dx, dy, drawW, drawH);
            }
        }
        SDL_Rect wash{0,0,WIN_W,WIN_H};
        fill(C(0,0,0,120), wash);
        int msgWidth = textw(msg);
        int x = (WIN_W - msgWidth) / 2;
        if(x < NAV_W + 20) x = NAV_W + 20;
        int y = WIN_H - 80;
        text(msg, x, y, textTitle());
        presenter.present(canvas.data().data(), canvas.data().size() * sizeof(uint32_t));
    }
    bool copy_file_simple(const std::string& src, const std::string& dst, std::string& err){
        FILE* in = fopen(src.c_str(), "rb");
        if(!in){ err = "[copy] open src failed"; return false; }
        auto slash = dst.find_last_of('/');
        if(slash!=std::string::npos){
            std::string dir = dst.substr(0, slash);
            if(!fsx::isdir(dir) && !fsx::makedirs(dir)){
                fclose(in);
                err = "[copy] mkdir failed";
                return false;
            }
        }
        FILE* out = fopen(dst.c_str(), "wb");
        if(!out){ fclose(in); err = "[copy] open dst failed"; return false; }
        std::vector<unsigned char> buf(1<<16);
        bool ok=true;
        while(true){
            size_t n = fread(buf.data(),1,buf.size(),in);
            if(n>0){
                if(fwrite(buf.data(),1,n,out)!=n){ ok=false; err="[copy] write failed"; break; }
            }
            if(n < buf.size()){
                if(feof(in)) break;
                ok=false; err="[copy] read failed"; break;
            }
        }
        fclose(in); fclose(out);
        return ok;
    }
    bool copy_dir_recursive(const std::string& src, const std::string& dst, std::string& err){
        if(!fsx::isdir(src)){
            err = "[copy] source missing";
            return false;
        }
        if(!fsx::isdir(dst) && !fsx::makedirs(dst)){
            err = "[copy] cannot create destination";
            return false;
        }
        auto entries = fsx::list_dirents(src);
        for(const auto& entry : entries){
            std::string rel = entry.path.substr(src.size());
            if(!rel.empty() && (rel[0]=='/' || rel[0]=='\\')) rel.erase(0,1);
            std::string dstPath = dst + "/" + rel;
            if(entry.is_dir){
                if(!copy_dir_recursive(entry.path, dstPath, err)) return false;
            }else if(entry.is_file){
                if(!copy_file_simple(entry.path, dstPath, err)) return false;
            }
        }
        return true;
    }
    bool buildLegacyCopyPlan(const std::string& srcRoot, const std::string& dstRoot,
                             std::vector<LegacyCopyOp>& plan, long long& totalBytes){
        std::function<bool(const std::string&)> walk = [&](const std::string& cur)->bool{
            auto entries = fsx::list_dirents(cur);
            for(const auto& entry : entries){
                std::string rel = entry.path.substr(srcRoot.size());
                if(!rel.empty() && (rel[0]=='/' || rel[0]=='\\')) rel.erase(0,1);
                std::string dstPath = dstRoot;
                if(!rel.empty()) dstPath += "/" + rel;
                for(char& ch : dstPath) if(ch=='\\') ch = '/';
                if(entry.is_dir){
                    if(!fsx::isdir(dstPath)) fsx::makedirs(dstPath);
                    if(!walk(entry.path)) return false;
                }else if(entry.is_file){
                    long sz = fsx::file_size(entry.path);
                    LegacyCopyOp op;
                    op.src = entry.path;
                    op.dst = dstPath;
                    op.size = sz>0 ? sz : 0;
                    plan.push_back(std::move(op));
                    if(sz>0) totalBytes += sz;
                }
            }
            return true;
        };
        plan.clear();
        totalBytes = 0;
        return walk(srcRoot);
    }
    enum class LegacyCopyResult{ Success, Error, Canceled };
    LegacyCopyResult copy_file_with_progress(const LegacyCopyOp& op, std::string& err){
        FILE* in = fopen(op.src.c_str(), "rb");
        if(!in){ err = "[copy] open src failed"; return LegacyCopyResult::Error; }
        auto slash = op.dst.find_last_of('/');
        if(slash != std::string::npos){
            std::string dir = op.dst.substr(0, slash);
            if(!fsx::isdir(dir) && !fsx::makedirs(dir)){
                fclose(in);
                err = "[copy] mkdir failed";
                return LegacyCopyResult::Error;
            }
        }
        FILE* out = fopen(op.dst.c_str(), "wb");
        if(!out){ fclose(in); err = "[copy] open dst failed"; return LegacyCopyResult::Error; }
        std::vector<unsigned char> buf(1<<16);
        LegacyCopyResult result = LegacyCopyResult::Success;
        while(true){
            size_t n = fread(buf.data(),1,buf.size(),in);
            if(n>0){
                if(fwrite(buf.data(),1,n,out)!=n){
                    err = "[copy] write failed";
                    result = LegacyCopyResult::Error;
                    break;
                }
                legacy_prog.bytes_done.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
            }
            if(legacy_prog.cancel.load(std::memory_order_relaxed)){
                result = LegacyCopyResult::Canceled;
                break;
            }
            if(n < buf.size()){
                if(feof(in)) break;
                err = "[copy] read failed";
                result = LegacyCopyResult::Error;
                break;
            }
        }
        fclose(in);
        fclose(out);
        if(result == LegacyCopyResult::Canceled){
            unlink(op.dst.c_str());
        }
        return result;
    }
    bool copyLegacyModsOnce(){
        if(legacy_copying.load()){
            settings_message = "Legacy copy already running.";
            return false;
        }
        const std::string legacy = MODS_ROOT_LEGACY;
        const std::string target = MODS_ROOT_NEW;
        if(!fsx::isdir(legacy)){
            settings_message = "Legacy folder not found.";
            return false;
        }
        if(!fsx::isdir(target) && !fsx::makedirs(target)){
            settings_message = "Failed to create new mods folder.";
            return false;
        }
        legacy_copy_plan.clear();
        long long totalBytes = 0;
        if(!buildLegacyCopyPlan(legacy, target, legacy_copy_plan, totalBytes)){
            settings_message = "Legacy copy plan failed.";
            return false;
        }
        if(legacy_copy_plan.empty()){
            settings_message = "Legacy folder is empty.";
            return false;
        }
        legacy_prog.files_total = legacy_copy_plan.size();
        legacy_prog.files_done = 0;
        legacy_prog.bytes_total = totalBytes;
        legacy_prog.bytes_done = 0;
        legacy_prog.cancel = false;
        legacy_copy_error.clear();
        legacy_copy_done = false;
        legacy_copy_failed = false;
        legacy_copy_cancelled = false;
        legacy_stack = memalign(0x1000, LEGACY_COPY_STACK_SZ);
        if(!legacy_stack){
            settings_message = "Legacy copy memalign failed.";
            return false;
        }
        Result rc = threadCreate(&legacy_thread, s_legacy_copy_entry, this, legacy_stack, LEGACY_COPY_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            char buf[128];
            snprintf(buf, sizeof(buf), "[copy] threadCreate failed rc=0x%08X\n", rc);
            log += buf;
            settings_message = "Legacy copy thread failed.";
            free(legacy_stack);
            legacy_stack=nullptr;
            return false;
        }
        legacy_copying = true;
        legacy_thread_alive = true;
        threadStart(&legacy_thread);
        settings_message = "Legacy copy started...";
        return true;
    }
    std::string formatFileSize(long long bytes) const {
        const double kb = 1024.0;
        const double mb = kb * 1024.0;
        const double gb = mb * 1024.0;
        char buf[64];
        if(bytes >= (long long)gb){
            snprintf(buf, sizeof(buf), "%.2f GB", bytes / gb);
        }else if(bytes >= (long long)mb){
            snprintf(buf, sizeof(buf), "%.2f MB", bytes / mb);
        }else if(bytes >= (long long)kb){
            snprintf(buf, sizeof(buf), "%.1f KB", bytes / kb);
        }else{
            snprintf(buf, sizeof(buf), "%lld B", bytes);
        }
        return buf;
    }
    std::string formatFileDate(long long ts) const {
        if(ts <= 0) return "Unknown date";
        time_t t = static_cast<time_t>(ts);
        struct tm out{};
#ifdef _WIN32
        gmtime_s(&out, &t);
#else
        gmtime_r(&t, &out);
#endif
        char buf[64];
        if(strftime(buf, sizeof(buf), "%Y-%m-%d", &out)==0) return "Unknown date";
        return buf;
    }
    static inline std::string trim_copy(const std::string& s){
        size_t a=0, b=s.size();
        while(a<b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while(b>a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
        return s.substr(a, b-a);
    }
    std::string normalizeVersionLabel(const std::string& in) const {
        std::string t = trim_copy(in);
        if(!t.empty() && (t[0]=='v' || t[0]=='V')) t.erase(0,1);
        return t;
    }
    std::string resolveUpdateFeedUrl() const {
        const char* overridePath = "sdmc:/switch/trinitymgr_ns/update_url.txt";
        FILE* f = fopen(overridePath, "rb");
        if(!f) return DEFAULT_UPDATE_FEED;
        char buf[512];
        char* res = fgets(buf, sizeof(buf), f);
        fclose(f);
        if(!res) return DEFAULT_UPDATE_FEED;
        std::string line = trim_copy(res);
        if(line.empty()) return DEFAULT_UPDATE_FEED;
        return line;
    }
    bool extractJsonStringValue(const std::string& body, const std::string& key, std::string& out){
        std::string pat = "\"" + key + "\"";
        size_t pos = body.find(pat);
        if(pos == std::string::npos) return false;
        pos = body.find(':', pos + pat.size());
        if(pos == std::string::npos) return false;
        pos = body.find('"', pos);
        if(pos == std::string::npos) return false;
        ++pos;
        std::string result;
        bool esc=false;
        for(; pos < body.size(); ++pos){
            char c = body[pos];
            if(esc){ result.push_back(c); esc=false; continue; }
            if(c=='\\'){ esc=true; continue; }
            if(c=='"') break;
            result.push_back(c);
        }
        out = result;
        return true;
    }
    void checkForUpdates(){
        if(update_checking) return;
        update_checking = true;
        std::string url = resolveUpdateFeedUrl();
        std::string body;
        settings_message = "Checking updates...";
        if(!gb::http_get(url, body, log)){
            update_status = "Update check failed.";
            settings_message = update_status;
            update_checking = false;
            return;
        }
        std::string tag;
        std::string html;
        extractJsonStringValue(body, "tag_name", tag);
        extractJsonStringValue(body, "html_url", html);
        std::string remoteNorm = normalizeVersionLabel(tag);
        std::string localNorm = normalizeVersionLabel(APP_VERSION_STRING);
        update_available = false;
        update_download_url.clear();
        if(!remoteNorm.empty() && !localNorm.empty() && remoteNorm != localNorm){
            update_status = "Update available: " + (tag.empty()?remoteNorm:tag) + " (current " + APP_VERSION_STRING + ")";
            std::string assetUrl;
            if(findNroAssetUrl(body, assetUrl)){
                update_available = true;
                update_download_url = assetUrl;
                update_status += "  Press [ZL] to install.";
            }else{
                update_status += "  (No .nro asset found.)";
            }
        }else{
            update_status = std::string("Up to date (") + APP_VERSION_STRING + ")";
        }
        if(!html.empty()) update_status += "  " + html;
        settings_message = update_status;
        update_checking = false;
    }
    bool findNroAssetUrl(const std::string& body, std::string& out){
        const std::string key = "\"browser_download_url\"";
        size_t pos = 0;
        std::string fallback;
        while((pos = body.find(key, pos)) != std::string::npos){
            size_t colon = body.find(':', pos + key.size());
            if(colon == std::string::npos){ pos += key.size(); continue; }
            size_t q = body.find('"', colon);
            if(q == std::string::npos){ pos = colon; continue; }
            ++q;
            std::string raw;
            bool esc=false;
            for(; q < body.size(); ++q){
                char c = body[q];
                if(esc){ raw.push_back(c); esc=false; continue; }
                if(c=='\\'){ esc=true; continue; }
                if(c=='"') break;
                raw.push_back(c);
            }
            if(raw.empty()){ pos = q; continue; }
            std::string decoded = gb::json_unescape(raw);
            if(decoded.find(".nro") == std::string::npos){
                pos = q;
                continue;
            }
            if(decoded.find(UPDATE_ASSET_HINT) != std::string::npos){
                out = decoded;
                return true;
            }
            if(fallback.empty()) fallback = decoded;
            pos = q;
        }
        if(!fallback.empty()){
            out = fallback;
            return true;
        }
        return false;
    }
    static size_t update_write_cb(void* ptr, size_t sz, size_t nm, void* userdata){
        FILE* f = static_cast<FILE*>(userdata);
        if(!f) return 0;
        return fwrite(ptr, sz, nm, f);
    }
    static int update_prog_cb(void* clientp, double dltotal, double dlnow, double, double){
        auto* prog = static_cast<gb::DlProg*>(clientp);
        if(!prog) return 0;
        prog->total.store((long long)dltotal, std::memory_order_relaxed);
        prog->now.store((long long)dlnow, std::memory_order_relaxed);
        return prog->cancel.load(std::memory_order_relaxed) ? 1 : 0;
    }
    bool downloadUpdateFile(const std::string& url, const std::string& outPath, std::string& err){
        CURL* curl = curl_easy_init();
        if(!curl){
            err = "[update] curl init failed";
            return false;
        }
        FILE* out = fopen(outPath.c_str(), "wb");
        if(!out){
            err = "[update] open temp file failed";
            curl_easy_cleanup(curl);
            return false;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr-Updater/1.0 (+Switch)");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &App::update_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef CURLOPT_XFERINFOFUNCTION
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &App::update_prog_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &update_prog);
#endif
        curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &App::update_prog_cb);
        curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &update_prog);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
        CURLcode rc = curl_easy_perform(curl);
        fclose(out);
        curl_easy_cleanup(curl);
        if(rc != CURLE_OK){
            unlink(outPath.c_str());
            char buf[160];
            snprintf(buf,sizeof(buf),"[update] download failed: %s\n", curl_easy_strerror(rc));
            log += buf;
            err = "[update] download failed.";
            return false;
        }
        return true;
    }
    bool installDownloadedUpdate(const std::string& tmpPath, std::string& err){
        std::string dest = self_nro_path.empty() ? SELF_NRO_PATH : self_nro_path;
        if(dest.empty()){
            err = "[update] install path unknown.";
            return false;
        }
        size_t slash = dest.find_last_of('/');
        if(slash == std::string::npos){
            err = "[update] install path invalid.";
            return false;
        }
        std::string parent = dest.substr(0, slash);
        if(!parent.empty() && !fsx::isdir(parent)){
            if(!fsx::makedirs(parent)){
                err = "[update] cannot create install dir.";
                return false;
            }
        }
        if(fsx::isfile(dest)){
            std::string backup = dest + ".bak";
            std::string bkErr;
            if(!copy_file_simple(dest, backup, bkErr)){
                log += bkErr.empty() ? "[update] backup failed.\n" : ("[update] " + bkErr + "\n");
            }
            unlink(dest.c_str());
        }
        if(!copy_file_simple(tmpPath, dest, err)){
            return false;
        }
        unlink(tmpPath.c_str());
        log += "[update] installed to " + dest + "\n";
        return true;
    }
    void startUpdateDownload(){
        if(update_downloading.load()){
            settings_message = "Update download already running.";
            return;
        }
        if(update_download_url.empty()){
            settings_message = "No update package available.";
            return;
        }
        update_prog.now = 0;
        update_prog.total = 0;
        update_prog.cancel = false;
        update_download_done = false;
        update_download_failed = false;
        update_download_error.clear();
        update_stack = memalign(0x1000, UPDATE_STACK_SZ);
        if(!update_stack){
            settings_message = "Update download memalign failed.";
            return;
        }
        Result rc = threadCreate(&update_thread, s_update_entry, this, update_stack, UPDATE_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            char buf[128];
            snprintf(buf,sizeof(buf),"[update] threadCreate failed rc=0x%08X\n", rc);
            log += buf;
            settings_message = "Update download start failed.";
            free(update_stack);
            update_stack=nullptr;
            return;
        }
        update_downloading = true;
        update_thread_alive = true;
        threadStart(&update_thread);
        settings_message = "Downloading update...";
    }
    void closeFilePicker(){
        file_picker_open = false;
        file_picker_files.clear();
        file_picker_target_index = -1;
        file_picker_cursor = 0;
        file_picker_scroll = 0;
        file_picker_panel_rect = SDL_Rect{0,0,0,0};
        file_picker_list_rect = SDL_Rect{0,0,0,0};
    }
    const std::vector<gb::FileEntry>* filesForModIndex(int idx){
        if(idx<0 || idx >= (int)gb_items.size()) return nullptr;
        if(info_overlay && info_mod_index == idx && !info_files.empty()) return &info_files;
        int mod_id = gb_items[idx].id;
        auto it = gb_info_cache.find(mod_id);
        if(it != gb_info_cache.end() && !it->second.files.empty()) return &it->second.files;
        return nullptr;
    }
    void startDownloadFlow(int idx){
        if(idx<0 || idx >= (int)gb_items.size()) return;
        const auto* files = filesForModIndex(idx);
        if((!files || files->empty()) && info_loading.load()){
            log += "[gb] still loading file list; please wait.\n";
            return;
        }
        if(!files || files->empty()){
            closeModInfo();
            beginDownload(idx);
            return;
        }
        if(files->size()==1){
            gb::FileEntry chosen = (*files)[0];
            closeModInfo();
            beginDownload(idx, &chosen);
            return;
        }
        file_picker_files = *files;
        file_picker_target_index = idx;
        file_picker_cursor = 0;
        file_picker_scroll = 0;
        file_picker_open = true;
    }
    void confirmFilePickerSelection(){
        if(!file_picker_open) return;
        if(file_picker_cursor < 0 || file_picker_cursor >= (int)file_picker_files.size()) return;
        if(file_picker_target_index < 0 || file_picker_target_index >= (int)gb_items.size()) return;
        gb::FileEntry chosen = file_picker_files[file_picker_cursor];
        int target = file_picker_target_index;
        closeFilePicker();
        closeModInfo();
        beginDownload(target, &chosen);
    }
    void cancelFilePicker(){
        closeFilePicker();
    }

    SDL_Color themeCanvasBg() const {
#ifdef APP_DEBUG_BUILD
        return C(12, 12, 10);
#else
        return C(2, 8, 32);
#endif
    }
    SDL_Color themeNavBg() const {
#ifdef APP_DEBUG_BUILD
        return C(48, 30, 12);
#else
        return C(0x11, 0x1a, 0x23);
#endif
    }
    SDL_Color themePanelBg() const {
#ifdef APP_DEBUG_BUILD
        return C(36, 24, 12);
#else
        return C(0x0b, 0x12, 0x1a);
#endif
    }
    SDL_Color themeHighlight() const {
#ifdef APP_DEBUG_BUILD
        return C(64, 40, 10);
#else
        return C(0x22, 0x4a, 0x73);
#endif
    }
    SDL_Color themeCardBg() const {
#ifdef APP_DEBUG_BUILD
        return C(56, 32, 12);
#else
        return C(6, 48, 160);
#endif
    }

    SDL_Color textPrimary() const { return C(200,210,220); }
    SDL_Color textTitle() const { return C(220,230,240); }
    SDL_Color textMuted() const { return C(150,160,170); }
    SDL_Color textAccent() const {
#ifdef APP_DEBUG_BUILD
        return C(255,160,90);
#else
        return C(0x78,0xb4,0xe2);
#endif
    }

    void beginDownload(int idx, const gb::FileEntry* file_choice=nullptr){
        if(idx<0 || idx >= (int)gb_items.size()) return;
        if(downloading.load()){
            log += "[gb] download already running\n";
            return;
        }
        dl_item = gb_items[idx];
        if(file_choice){
            dl_has_file_choice = true;
            dl_file_choice = *file_choice;
        }else{
            dl_has_file_choice = false;
        }
        downloading = true;
        dl_done = false; dl_failed = false;
        dl_saved.clear(); dl_thread_log.clear();
        dl_prog.now = 0; dl_prog.total = 0; dl_prog.cancel = false;

        dl_stack = memalign(0x1000, DL_STACK_SZ);
        if(!dl_stack){
            downloading = false;
            log += "[thread] memalign failed\n";
            return;
        }
        Result rc = threadCreate(&dl_thread, s_dl_entry, this, dl_stack, DL_STACK_SZ, 0x2C, -2);
        if(R_FAILED(rc)){
            downloading = false;
            char buf[96]; snprintf(buf,sizeof(buf),"[thread] create failed rc=0x%08X\n", rc);
            log += buf;
            free(dl_stack); dl_stack = nullptr;
            return;
        }
        dl_thread_alive = true;
        threadStart(&dl_thread);
    }

    void openModInfo(int idx){
        if(idx<0 || idx >= (int)gb_items.size()) return;
        info_overlay = true;
        closeFilePicker();
        info_mod_index = idx;
        info_failed = false;
        resetInfoDescScroll();
        int mod_id = gb_items[idx].id;
        info_mod_id = mod_id;
        auto it = gb_info_cache.find(mod_id);
        if(it != gb_info_cache.end()){
            info_desc = it->second.desc;
            info_authors = it->second.credits;
            info_gallery_images = it->second.images;
            info_files = it->second.files;
            if(info_gallery_images.empty()){
                gb::GalleryImage gi;
                gi.key = gb_items[idx].id;
                gi.url = gb_items[idx].thumb;
                info_gallery_images.push_back(std::move(gi));
            }
            info_loading = false;
            resetInfoDescScroll();
        }else{
            startInfoThread(mod_id);
        }
    }
    void closeModInfo(){
        info_overlay = false;
        info_mod_index = -1;
        info_gallery_active = false;
        closeFilePicker();
        info_files.clear();
        resetInfoDescScroll();
        thumb_cache.clearLarge();
    }

    void resetInfoDescScroll(){
        info_desc_scroll = 0;
        info_desc_visible_lines = 0;
        info_desc_total_lines = 0;
        info_desc_scroll_hold_dir = 0;
        info_desc_scroll_hold_timer = 0;
        info_authors_scroll = 0;
        info_authors_visible_lines = 0;
        info_authors_total_lines = 0;
        info_gallery_index = 0;
        info_gallery_active = false;
        info_image_rect = SDL_Rect{0,0,0,0};
    }
    void scrollInfoDesc(int delta){
        if(delta==0) return;
        if(info_desc_visible_lines <= 0) return;
        if(info_desc_total_lines <= info_desc_visible_lines) return;
        int maxScroll = info_desc_total_lines - info_desc_visible_lines;
        info_desc_scroll = std::max(0, std::min(info_desc_scroll + delta, maxScroll));
        if(info_authors_visible_lines > 0 && info_authors_total_lines > info_authors_visible_lines){
            int maxAuth = info_authors_total_lines - info_authors_visible_lines;
            info_authors_scroll = std::max(0, std::min(info_authors_scroll + delta, maxAuth));
        }
    }
    void openImageGallery(){
        if(info_gallery_images.empty()) return;
        if(info_gallery_index < 0 || info_gallery_index >= (int)info_gallery_images.size()) info_gallery_index=0;
        info_gallery_active = true;
    }
    void closeImageGallery(){
        info_gallery_active = false;
    }
    void galleryStep(int delta){
        if(info_gallery_images.empty()) return;
        int count = (int)info_gallery_images.size();
        if(count <= 0) return;
        info_gallery_index = (info_gallery_index + count + delta) % count;
    }

    void handleInfoOverlayTouchPoint(int x, int y){
        int panelW = WIN_W * 4 / 5;
        int panelH = WIN_H * 4 / 5;
        int px = (WIN_W - panelW)/2;
        int py = (WIN_H - panelH)/2;
        int descWidth = (panelW * 3)/5;
        int descX = px + 24;
        int descY = py + 100;
        SDL_Rect descBox{descX-4, descY-8, descWidth+8, panelH-180};
        if(x >= descBox.x && x <= descBox.x + descBox.w &&
           y >= descBox.y && y <= descBox.y + descBox.h){
            if(info_desc_total_lines > info_desc_visible_lines && info_desc_visible_lines>0){
                int delta = (y < descBox.y + descBox.h/2) ? -std::max(1, info_desc_visible_lines/2)
                                                         :  std::max(1, info_desc_visible_lines/2);
                scrollInfoDesc(delta);
            }
        }
        if(info_image_rect.w > 0 && info_image_rect.h > 0 &&
           x >= info_image_rect.x && x <= info_image_rect.x + info_image_rect.w &&
           y >= info_image_rect.y && y <= info_image_rect.y + info_image_rect.h){
            openImageGallery();
        }
    }
    void handleFilePickerTouchPoint(int x, int y){
        if(!file_picker_open) return;
        if(file_picker_list_rect.h <= 0){
            cancelFilePicker();
            return;
        }
        if(x < file_picker_panel_rect.x || x > file_picker_panel_rect.x + file_picker_panel_rect.w ||
           y < file_picker_panel_rect.y || y > file_picker_panel_rect.y + file_picker_panel_rect.h){
            cancelFilePicker();
            return;
        }
        if(y >= file_picker_list_rect.y && y <= file_picker_list_rect.y + file_picker_list_rect.h){
            const int rowH = 96;
            int row = (y - file_picker_list_rect.y) / rowH;
            int idx = file_picker_scroll + row;
            if(idx >=0 && idx < (int)file_picker_files.size()){
                file_picker_cursor = idx;
                confirmFilePickerSelection();
            }
        }
    }

    void enforceLogWindow(){
        while(log_lines_bytes > LOG_WINDOW_BYTES && !log_lines.empty()){
            log_lines_bytes -= log_lines.front().size();
            log_lines.pop_front();
        }
    }
    void appendLogLine(const std::string& line){
        log_lines.emplace_back(line);
        log_lines_bytes += line.size();
        log_disk_buffer.append(line);
        log_disk_buffer.push_back('\n');
        enforceLogWindow();
    }
    void drainLogInput(){
        if(log.empty()) return;
        std::string cleaned;
        cleaned.reserve(log.size());
        for(char c : log){
            if(c!='\r') cleaned.push_back(c);
        }
        size_t pos = 0;
        while(true){
            size_t nl = cleaned.find('\n', pos);
            if(nl == std::string::npos) break;
            log_partial_line.append(cleaned, pos, nl - pos);
            appendLogLine(log_partial_line);
            log_partial_line.clear();
            pos = nl + 1;
        }
        if(pos < cleaned.size()){
            log_partial_line.append(cleaned, pos, cleaned.size() - pos);
        }
        log.clear();
    }
    void flushLogToFile(bool force=false){
        if(log_disk_buffer.empty() && !(force && !log_partial_line.empty())) return;
        constexpr const char* kLogPath = "sdmc:/switch/trinitymgr_ns/log.txt";
        fsx::makedirs("sdmc:/switch/trinitymgr_ns");
        bool truncate = false;
        long existing = fsx::file_size(kLogPath);
        if(existing >= 0 && static_cast<size_t>(existing) > LOG_MAX_FILE_BYTES){
            truncate = true;
        }
        FILE* fp = fopen(kLogPath, truncate ? "wb" : "ab");
        if(!fp){
            log_disk_buffer.clear();
            return;
        }
        if(truncate){
            const char* note = "[log] truncated (exceeded 3 MB)\n";
            fwrite(note, 1, strlen(note), fp);
        }
        if(!log_disk_buffer.empty()){
            fwrite(log_disk_buffer.data(), 1, log_disk_buffer.size(), fp);
            log_disk_buffer.clear();
        }
        if(force && !log_partial_line.empty()){
            fwrite(log_partial_line.data(), 1, log_partial_line.size(), fp);
            fputc('\n', fp);
        }
        fclose(fp);
        log_last_flush_frame = log_frame_counter;
    }
    void maybeFlushLogToFile(bool force=false){
        if(!force && downloadsActive()){
            return;
        }
        if(force){
            flushLogToFile(true);
            return;
        }
        if(log_disk_buffer.size() >= LOG_FLUSH_CHUNK_BYTES){
            flushLogToFile(false);
            return;
        }
        if(!log_disk_buffer.empty() && (log_frame_counter - log_last_flush_frame) >= LOG_FLUSH_INTERVAL_FRAMES){
            flushLogToFile(false);
        }
    }
