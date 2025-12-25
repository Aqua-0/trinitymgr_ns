#pragma once

    void dumpModsDirListing(){
        auto dump = [&](const std::string& dir){
            log += std::string("[mods] listing ") + dir + "\n";
            if(!fsx::isdir(dir)){
                log += "  (missing)\n";
                return;
            }
            auto ents = fsx::list_dirents(dir);
            if(ents.empty()){
                log += "  (empty)\n";
                return;
            }
            for(const auto& e : ents){
                log += "  - " + e.path + (e.is_dir?"/":"") + "\n";
            }
        };
        dump(mods_root);
        dump(mods_root + "/_unzipped");
    }

    void gotoMenuIndex(int idx){
        idx = std::max(0, std::min(idx, (int)menu.size()-1));
        menu_cursor = idx;
        if(idx != 3 && info_overlay) closeModInfo();
        if(menu_cursor == (int)menu.size()-1){ running=false; return; } // Exit
#ifdef APP_DEBUG_BUILD
        switch(menu_cursor){
            case 0: screen=Screen::Mods; break;
            case 1: screen=Screen::Target; break;
            case 2: screen=Screen::Apply; break;
            case 3: {
                screen = Screen::Browse;
                if (gb_items.empty() && !gb_fetching.load()) {
                    requestGbFetch(gb_page, true);
                    browse_cursor = 0;
                }
                break;
            }
            case 4:
                screen = Screen::Profiles;
                refreshProfileList();
                break;
            case 5: screen=Screen::Settings; break;
            case 6: screen=Screen::Log; break;
            case 7: screen=Screen::About; break;
            default: screen=Screen::Mods; break;
        }
#else
        switch(menu_cursor){
            case 0: screen=Screen::Mods; break;
            case 1: screen=Screen::Target; break;
            case 2: screen=Screen::Apply; break;
            case 3: {
                screen = Screen::Browse;
                if (gb_items.empty() && !gb_fetching.load()) {
                    requestGbFetch(gb_page, true);
                    browse_cursor = 0;
                }
                break;
            }
            case 4:
                screen = Screen::Profiles;
                refreshProfileList();
                break;
            case 5: screen=Screen::Settings; break;
            case 6: screen=Screen::About; break;
            default: screen=Screen::Mods; break;
        }
#endif
    }
    void gotoLogTabOrFallback(){
#ifdef APP_DEBUG_BUILD
        gotoMenuIndex(LOG_TAB_INDEX);
#else
        gotoMenuIndex(APPLY_TAB_INDEX);
#endif
    }
    void moveTab(int delta){
        int idx = menu_cursor;
        int maxIdx = (int)menu.size() - 2;
        if(idx > maxIdx) idx = maxIdx;
        idx += delta;
        if(idx < 0) idx = maxIdx;
        if(idx > maxIdx) idx = 0;
        gotoMenuIndex(idx);
    }

    void handlePad(u64 down, u64 held, const HidAnalogStickState& leftStick){
        if(down&HidNpadButton_Plus){ running=false; return; }
#ifdef APP_DEBUG_BUILD
        if((held & HidNpadButton_ZL) && (held & HidNpadButton_ZR) && (down & HidNpadButton_StickR)){
            for(int i=0;i<512;i++){
                char buf[128];
                snprintf(buf, sizeof(buf), "[stress] frame=%llu burst=%d", (unsigned long long)log_frame_counter, i);
                log += buf;
                log += "\n";
            }
            return;
        }
#endif
        if((held & HidNpadButton_ZL) && (held & HidNpadButton_ZR) && (down & HidNpadButton_Y)){
            dumpModsDirListing();
            return;
        }
        if(update_downloading.load() && (down & HidNpadButton_B)){
            update_prog.cancel = true;
            settings_message = "Canceling update download...";
            return;
        }
        if(legacy_copying.load() && (down & HidNpadButton_B)){
            legacy_prog.cancel = true;
            settings_message = "Canceling legacy copy...";
            return;
        }

        if(!info_overlay){
            int heldDir = 0;
            if(held & HidNpadButton_Up) heldDir -= 1;
            if(held & HidNpadButton_Down) heldDir += 1;
            if(heldDir != 0){
                if(heldDir != dpad_repeat_dir){
                    dpad_repeat_dir = heldDir;
                    dpad_repeat_timer = 0;
                }else{
                    dpad_repeat_timer++;
                    if(dpad_repeat_timer >= DPAD_REPEAT_DELAY_FRAMES){
                        down |= (heldDir < 0) ? HidNpadButton_Up : HidNpadButton_Down;
                        dpad_repeat_timer = DPAD_REPEAT_DELAY_FRAMES - DPAD_REPEAT_RATE_FRAMES;
                        if(dpad_repeat_timer < 0) dpad_repeat_timer = 0;
                    }
                }
            }else{
                dpad_repeat_dir = 0;
                dpad_repeat_timer = 0;
            }
        }else{
            dpad_repeat_dir = 0;
            dpad_repeat_timer = 0;
        }

        auto resetTabStick = [&](){ tab_stick_dir = 0; tab_stick_timer = 0; };
        if(file_picker_open){
            resetTabStick();
            int count = (int)file_picker_files.size();
            const int rowH = 96;
            int visible = file_picker_list_rect.h > 0 ? file_picker_list_rect.h / rowH : 4;
            if(visible < 1) visible = 4;
            if(down & HidNpadButton_B){
                cancelFilePicker();
                return;
            }
            if((down & HidNpadButton_A) && count>0){
                confirmFilePickerSelection();
                return;
            }
            if((down & HidNpadButton_Up) && count>0){
                if(file_picker_cursor > 0){
                    file_picker_cursor--;
                    if(file_picker_cursor < file_picker_scroll) file_picker_scroll = file_picker_cursor;
                }
                return;
            }
            if((down & HidNpadButton_Down) && count>0){
                if(file_picker_cursor + 1 < count){
                    file_picker_cursor++;
                    int maxVis = file_picker_scroll + visible;
                    if(file_picker_cursor >= maxVis) file_picker_scroll = file_picker_cursor - visible + 1;
                }
                return;
            }
            if((down & HidNpadButton_ZL) && count>0){
                file_picker_cursor = std::max(0, file_picker_cursor - visible);
                if(file_picker_cursor < file_picker_scroll) file_picker_scroll = file_picker_cursor;
                return;
            }
            if((down & HidNpadButton_ZR) && count>0){
                file_picker_cursor = std::min(count-1, file_picker_cursor + visible);
                int maxVis = file_picker_scroll + visible;
                if(file_picker_cursor >= maxVis) file_picker_scroll = std::max(0, file_picker_cursor - visible + 1);
                return;
            }
            return;
        }
        if(info_overlay){
            resetTabStick();
            if(info_gallery_active){
                if(down & HidNpadButton_B){
                    closeImageGallery();
                    return;
                }
                if(down & (HidNpadButton_Left | HidNpadButton_L)){
                    galleryStep(-1);
                    return;
                }
                if(down & (HidNpadButton_Right | HidNpadButton_R)){
                    galleryStep(+1);
                    return;
                }
                return;
            }
            if(down&HidNpadButton_B){
                closeModInfo();
                return;
            }
            bool consumed=false;
            if(down&HidNpadButton_Up){ scrollInfoDesc(-1); consumed=true; }
            if(down&HidNpadButton_Down){ scrollInfoDesc(+1); consumed=true; }
            if(down&HidNpadButton_ZL){
                scrollInfoDesc(-std::max(1, info_desc_visible_lines-1));
                consumed=true;
            }
            if(down&HidNpadButton_ZR){
                scrollInfoDesc(std::max(1, info_desc_visible_lines-1));
                consumed=true;
            }
            if((down&HidNpadButton_X) && !info_loading.load()){
                openImageGallery();
                return;
            }
            if((down&HidNpadButton_A) && !info_loading.load()){
                int idx = info_mod_index;
                startDownloadFlow(idx);
                return;
            }
            int heldDir = 0;
            if(held & HidNpadButton_Up) heldDir -= 1;
            if(held & HidNpadButton_Down) heldDir += 1;
            if(heldDir != 0 && info_desc_total_lines > info_desc_visible_lines){
                if(heldDir != info_desc_scroll_hold_dir){
                    info_desc_scroll_hold_dir = heldDir;
                    info_desc_scroll_hold_timer = 0;
                }else{
                    info_desc_scroll_hold_timer++;
                    if(info_desc_scroll_hold_timer >= INFO_SCROLL_REPEAT_FRAMES){
                        scrollInfoDesc(heldDir);
                        info_desc_scroll_hold_timer = INFO_SCROLL_REPEAT_FRAMES - 2;
                    }
                }
            }else{
                info_desc_scroll_hold_dir = 0;
                info_desc_scroll_hold_timer = 0;
            }
            if(consumed) return;
            return;
        }
        int stickY = leftStick.y;
        if(stickY >= TAB_STICK_DEADZONE || stickY <= -TAB_STICK_DEADZONE){
            int dir = (stickY > 0) ? -1 : 1;
            if(dir != tab_stick_dir){
                tab_stick_dir = dir;
                tab_stick_timer = 0;
                moveTab(dir);
            }else{
                tab_stick_timer++;
                if(tab_stick_timer >= TAB_STICK_REPEAT_FRAMES){
                    moveTab(dir);
                    tab_stick_timer = TAB_STICK_REPEAT_FRAMES - 2;
                }
            }
        }else if(tab_stick_dir != 0){
            resetTabStick();
        }

        if(down&HidNpadButton_Minus){
            if(screen==Screen::Profiles){
                gotoMenuIndex(0);
            }else{
                gotoMenuIndex(profile_tab_index);
            }
            return;
        }
        if(down&HidNpadButton_L) moveTab(-1);
        if(down&HidNpadButton_R) moveTab(+1);

        if(screen==Screen::Mods){
            int item_h=32, top=100, max_vis=(WIN_H-top-20)/item_h;
            int page = std::max(1, max_vis - 1);
            if(modlist.empty()){
                mod_cursor = 0;
                mod_scroll = 0;
                if(down&HidNpadButton_Y){
                    requestRescan();
                }
                return;
            }
            if(down&HidNpadButton_ZL) mod_cursor=std::max(0,mod_cursor-page);
            if(down&HidNpadButton_ZR) mod_cursor=std::min((int)modlist.size()-1,mod_cursor+page);
            if(down&HidNpadButton_Up)   mod_cursor=std::max(0,mod_cursor-1);
            if(down&HidNpadButton_Down) mod_cursor=std::min((int)modlist.size()-1,mod_cursor+1);
            bool busy = applying.load();
            if(down&HidNpadButton_A){
                if(busy){
                    log += "[apply] busy; cannot change selection\n";
                }else{
                    mod_cursor = std::max(0, std::min(mod_cursor, (int)modlist.size()-1));
                    modlist[mod_cursor].selected=!modlist[mod_cursor].selected;
                    persistSelectionCache();
                    markConflictsDirty();
                }
            }
            if(down&HidNpadButton_X){
                if(busy){
                    log += "[apply] busy; cannot toggle all\n";
                }else{
                    bool any=false; for(auto& m:modlist) any|=m.selected; for(auto& m:modlist) m.selected=!any;
                    persistSelectionCache();
                    markConflictsDirty();
                }
            }
            if(down&HidNpadButton_Y){
                requestRescan();
            }
        } else if(screen==Screen::Target){
            if(down&HidNpadButton_A){ if(target_romfs.size()<6 || strcasecmp(target_romfs.c_str()+target_romfs.size()-6,"/romfs")!=0) target_romfs+="/romfs"; }
            if(down&HidNpadButton_Y){ if(fsx::makedirs(target_romfs)) log+="[mkdir] ok "+target_romfs+"\n"; else log+="[mkdir] fail "+target_romfs+"\n"; }
        } else if(screen==Screen::Apply){
            if(down&HidNpadButton_X) clear_target_before_apply=!clear_target_before_apply;
            if(down & HidNpadButton_A){
                if(applying.load()){
                    log += "[apply] already running\n";
                    gotoLogTabOrFallback();
                    return;
                }
                log += "[apply] target="+target_romfs+"\n";
                if(!fsx::makedirs(target_romfs)){ log+="[apply] cannot create target\n"; gotoLogTabOrFallback(); return; }
                if(clear_target_before_apply) mods::clear_target_known(target_romfs, log, activeModsGame());

                applying = true; app_done = false; app_failed = false;
                app_prog.done = 0; app_prog.total = 0; app_prog.bytes = 0; app_prog.cancel = false;

                app_stack = memalign(0x1000, APP_STACK_SZ);
                if(!app_stack){ applying=false; log += "[thread] apply memalign failed\n"; gotoLogTabOrFallback(); return; }
                Result rc = threadCreate(&app_thread, s_app_entry, this, app_stack, APP_STACK_SZ, 0x2C, -2);
                if(R_FAILED(rc)){
                    applying=false;
                    char buf[96]; snprintf(buf,sizeof(buf),"[thread] apply create failed rc=0x%08X\n", rc); log+=buf;
                    free(app_stack); app_stack=nullptr; gotoLogTabOrFallback(); return;
                }
                app_mods_snapshot = modlist;
                app_thread_alive=true;
                threadStart(&app_thread);
                gotoLogTabOrFallback(); // send to Log during apply
            }
            if (down & HidNpadButton_Y) {
                log += "[launch] requesting start...\n";
                launch_title_id(activeTitleId(), log);
                // Usually system will switch away; if it returns, show Log
                gotoLogTabOrFallback();
            }
            if((down & HidNpadButton_B) && applying.load()){ app_prog.cancel = true; }
        } else if(screen==Screen::Browse){
            if(down&HidNpadButton_Y){
                browse_cursor = 0;
                requestGbFetch(gb_page, true);
            }
            auto prevPage = [&](){
                if(gb_page>0){
                    browse_cursor = 0;
                    requestGbFetch(gb_page-1);
                }
            };
            auto nextPage = [&](){
                browse_cursor = 0;
                requestGbFetch(gb_page+1);
            };
            if(down&HidNpadButton_ZL){
                prevPage();
            }
            if(down&HidNpadButton_ZR){
                nextPage();
            }
            if(down&HidNpadButton_Left){
                prevPage();
            }
            if(down&HidNpadButton_Right){
                nextPage();
            }
            if(down&HidNpadButton_Up){
                if(browse_cursor>0) browse_cursor--;
            }
            if(down&HidNpadButton_Down){
                if(browse_cursor+1 < (int)gb_items.size()) browse_cursor++;
            }
            if((down & HidNpadButton_A) && !gb_items.empty()){
                openModInfo(browse_cursor);
            }
            if ((down & HidNpadButton_B) && downloading.load()){
                dl_prog.cancel = 1; // worker sees this and aborts
            }
        } else if(screen==Screen::Profiles){
            int count = (int)profile_list.size();
            int max_vis = profileVisibleCount();
            int page = std::max(1, max_vis-1);
            if(count==0){
                if(down&HidNpadButton_Y) createProfilePrompt();
                return;
            }
            if(down&HidNpadButton_ZL) profile_cursor = std::max(0, profile_cursor - page);
            if(down&HidNpadButton_ZR) profile_cursor = std::min(count-1, profile_cursor + page);
            if(down&HidNpadButton_Up) profile_cursor = std::max(0, profile_cursor-1);
            if(down&HidNpadButton_Down) profile_cursor = std::min(count-1, profile_cursor+1);
            if(profile_cursor < profile_scroll) profile_scroll = profile_cursor;
            if(profile_cursor >= profile_scroll + max_vis) profile_scroll = std::max(0, profile_cursor - max_vis + 1);
            if(down&HidNpadButton_A) loadProfileAtCursor();
            if(down&HidNpadButton_X) overwriteProfileAtCursor();
            if(down&HidNpadButton_Y) createProfilePrompt();
            if(down&HidNpadButton_B) deleteProfileAtCursor();
        } else if(screen==Screen::Settings){
            if(down & HidNpadButton_A){
                touch_controls_enabled = !touch_controls_enabled;
                touch_active = false;
                settings_message = touch_controls_enabled ? "Touch controls enabled." : "Touch controls disabled.";
            }
            if(down & HidNpadButton_B){
                active_game = (active_game==ActiveGame::SV) ? ActiveGame::ZA : ActiveGame::SV;
                saveSettings();
                applyGameSelection(true);
                settings_message = std::string("Switched to ") + activeGameName() + ".";
            }
            if((down & HidNpadButton_ZR) && active_game==ActiveGame::SV){
                sv_title = (sv_title==SvTitle::Scarlet) ? SvTitle::Violet : SvTitle::Scarlet;
                saveSettings();
                setDefaultTargetRomfs();
                settings_message = std::string("SV title set to ") + activeSvTitleName() + ".";
            }
            if(down & HidNpadButton_X){
                copyLegacyModsOnce();
            }
            if((down & HidNpadButton_ZL) && update_available){
                startUpdateDownload();
            }
            if(down & HidNpadButton_Y){
                checkForUpdates();
            }
        } else if(screen==Screen::Log){
            // match drawLog() counts so scroll math aligns
            int total_lines = (int)log_lines.size() + (log_partial_line.empty() ? 0 : 1);
            const int vis =  (WIN_H - 64 - 48) / 20;
            int max_scroll = std::max(0, total_lines - vis);

            if(down & HidNpadButton_Up)    log_scroll = std::min(max_scroll, log_scroll + 1);
            if(down & HidNpadButton_Down)  log_scroll = std::max(0, log_scroll - 1);
            if(down & HidNpadButton_ZL)    log_scroll = std::min(max_scroll, log_scroll + vis - 1);
            if(down & HidNpadButton_ZR)    log_scroll = std::max(0, log_scroll - (vis - 1));
            if(down & HidNpadButton_X)     log_scroll = 0;               // jump to latest
            if(down & HidNpadButton_Y){
                log_lines.clear();
                log_lines_bytes = 0;
                log_partial_line.clear();
                log_scroll = 0;
            }
        }
    }

    void pollTouch(){
        if(!touch_controls_enabled){
            touch_active = false;
            return;
        }
        HidTouchScreenState state{};
        hidGetTouchScreenStates(&state, 1);
        if(state.count > 0){
            const auto& t = state.touches[0];
            touch_last_x = std::min(std::max(t.x / 1280.0f, 0.0f), 1.0f);
            touch_last_y = std::min(std::max(t.y / 720.0f, 0.0f), 1.0f);
            touch_active = true;
        }else if(touch_active){
            handleTouch(touch_last_x, touch_last_y);
            touch_active = false;
        }
    }

    void handleTouch(float nx, float ny){
        int x = (int)(nx * WIN_W);
        int y = (int)(ny * WIN_H);
        if(file_picker_open){
            handleFilePickerTouchPoint(x, y);
            return;
        }
        if(info_overlay){
            handleInfoOverlayTouchPoint(x, y);
            return;
        }

        // left navigation rail
        if (x < NAV_W){
            int y0 = 40 + 40;
            int idx = (y - y0) / 42;
            if (idx >= 0 && idx < (int)menu.size()) gotoMenuIndex(idx);
            return;
        }

        if (screen == Screen::Mods){
            int item_h = 32, top = 100, max_vis = (WIN_H - top - 20) / item_h;
            if (y >= top){
                int row = (y - top) / item_h;
                if (row >= 0 && row < max_vis){
                    int idx = mod_scroll + row;
                    if (idx >= 0 && idx < (int)modlist.size()){
                        modlist[idx].selected = !modlist[idx].selected;
                        mod_cursor = idx;
                        persistSelectionCache();
                        markConflictsDirty();
                    }
                }
            }
            return;
        }

        if (screen == Screen::Apply){
            if (y >= 160 && y <= 200) clear_target_before_apply = !clear_target_before_apply;
            return;
        }

        if (screen == Screen::Browse){
            // tap near top => previous page
            if (y < 88 + 16){
                if (gb_page > 0){
                    browse_cursor = 0;
                    requestGbFetch(gb_page-1);
                }
                return;
            }
            // tap near bottom => next page
            if (y > WIN_H - 48){
                browse_cursor = 0;
                requestGbFetch(gb_page+1);
                return;
            }
            // row selection
            const int startY = 100;
            const int rowH   = 88;
            if (y >= startY){
                int idx = (y - startY) / rowH;
                if (idx >= 0 && idx < (int)gb_items.size()){
                    browse_cursor = idx;
                    openModInfo(idx);
                }
            }
            return;
        }
    }
