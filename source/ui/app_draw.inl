#pragma once

    void drawMenuBar(){
        SDL_Rect nav{0,0,NAV_W,WIN_H}; fill(themeNavBg(),nav);
        int y=40;
        text("TrinityMgr", 24, y, textTitle()); y+=36;
        text(
#ifdef APP_DEBUG_BUILD
            "DEBUG BUILD"
#else
            "RELEASE BUILD"
#endif
            , 24, y, textAccent(), true);
        y += 28;
        for(int i=0;i<(int)menu.size();++i){
            SDL_Color col = (i==menu_cursor)?textAccent():C(180,190,200);
            SDL_Rect hi{0,y-6,NAV_W,36}; if(i==menu_cursor) fill(themeHighlight(),hi);
            text(menu[i], 24, y, col); y+=42;
        }
        int footY = WIN_H - 56;
        text("L/R tabs  |  Touch nav", 16, footY, textMuted(), true);
        text("ZL/ZR page", 16, footY+18, textMuted(), true);
#ifdef APP_DEBUG_BUILD
        text("Minus swaps build", 16, footY+36, textMuted(), true);
#else
        text("Minus = Profiles", 16, footY+36, textMuted(), true);
#endif
    }

    void drawMods(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Mods", NAV_W+20, 24, textTitle());
        std::string hintLeft = "[A] toggle  [X] toggle all  [Y] rescan  [ZL/ZR] page  [Touch] toggle   ";
        text(hintLeft, NAV_W+20, 60, textMuted(), true);
        SDL_Color conflictColor = C(0xE0, 0x58, 0x58);
        int hintX = NAV_W+20 + textw(hintLeft, true);
        text("X = conflict", hintX, 60, conflictColor, true);
        int item_h=40, top=100, max_vis=(WIN_H-top-20)/item_h;
        std::vector<char> conflictFlags(modlist.size(), 0);
        if(!modlist.empty()){
            const auto& conflicts = getConflicts();
            for(const auto& c : conflicts){
                for(int idx : c.second){
                    if(idx>=0 && idx<(int)conflictFlags.size()){
                        conflictFlags[idx] = 1;
                    }
                }
            }
        }
        if(mod_cursor<mod_scroll) mod_scroll=mod_cursor;
        if(mod_cursor>=mod_scroll+max_vis) mod_scroll=mod_cursor-max_vis+1;
        for(int i=0;i<max_vis;i++){
            int idx = mod_scroll+i; if(idx>=(int)modlist.size()) break;
            auto& m=modlist[idx];
            SDL_Rect card{NAV_W+12, top+i*item_h-6, WIN_W-NAV_W-24, item_h};
            SDL_Rect shadow = card; shadow.x +=2; shadow.y +=2;
            fill(C(0,0,0,80), shadow);
            fillRounded(themeCanvasBg(), card, 14);
            SDL_Rect rowInner{card.x+6, card.y+4, card.w-12, card.h-8};
            if(idx==mod_cursor) fillRounded(themeHighlight(), rowInner, 10);
            std::string line = std::string(m.selected?"[X] ":"[ ] ")+m.name+" ("+std::to_string(m.files.size())+")";
            int textY = rowInner.y+6;
            if(idx < (int)conflictFlags.size() && conflictFlags[idx]){
                text("X", rowInner.x, textY, conflictColor, true);
            }
            text(line, rowInner.x + (idx < (int)conflictFlags.size() && conflictFlags[idx] ? 18 : 8), textY, textPrimary());
        }
    }

    void drawTarget(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Target", NAV_W+20, 24, textTitle());
        text(target_romfs, NAV_W+20, 70, textPrimary());
        text("[A] append /romfs if missing   [Y] mkdir -p", NAV_W+20, 110, textMuted(), true);
    }

    void drawApply(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Apply", NAV_W+20, 24, textTitle());
        size_t sel=0, files=0; for(auto& m:modlist) if(m.selected){ sel++; files+=m.files.size(); }
        text("Selected mods: "+std::to_string(sel), NAV_W+20, 70, textPrimary());
        text("Total files:   "+std::to_string(files), NAV_W+20, 104, textPrimary());
        text("Target:  "+target_romfs, NAV_W+20, 138, textPrimary());
        text(std::string("Clear target before copy: ")+(clear_target_before_apply?"ON":"OFF")+"  [X] toggle", NAV_W+20, 172, textPrimary());

        const auto& conflicts = getConflicts();
        SDL_Rect sep{NAV_W+20, 210, WIN_W-NAV_W-40, 2}; fill(themeNavBg(), sep);
        SDL_Color conflictColor = C(0xE0, 0x58, 0x58);
        text("Conflicts: "+std::to_string(conflicts.size()), NAV_W+20, 220, conflictColor);
        int y=256; int shown=0;
        for(auto& c : conflicts){
            if(shown>=12) break;
            std::string line = " - " + c.first;
            if(textw(line,true) > (WIN_W-NAV_W-40)){ line = line.substr(0, 64) + "..."; }
            text(line, NAV_W+20, y, textPrimary(), true);
            y+=26; shown++;
        }
        if((int)conflicts.size()>shown) text("... +" + std::to_string(conflicts.size()-shown) + " more", NAV_W+20, y, textMuted(), true);
        text("[A] Start copy", NAV_W+20, WIN_H-40, textMuted(), true);
        text("[Y] Launch game now", NAV_W+20, WIN_H-68, textMuted(), true);
    }

    void drawBrowse(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        std::string title = "GameBanana - Page " + std::to_string(gb_page+1) + " (ZL/ZR switch)";
        text(title, NAV_W+20, 24, textTitle());
        text("TopSubs - " + std::to_string(GB_PER_PAGE) + " per page  |  [Y] refresh  [A] download",
            NAV_W+20, 60, textMuted(), true);
        int cardGap = 16;
        int availableHeight = WIN_H - 140 - cardGap;
        int rowH = availableHeight / GB_PER_PAGE;
        if(rowH < 120) rowH = 120;
        const int thH=80; const int thW=120;
        if(gb_fetching.load()){
            int y=100;
            text("Loading...", NAV_W+20, y, textPrimary());
            return;
        }
        if(gb_items.empty()){
            int y=100;
            text("No items. Press Y.", NAV_W+20, y, textPrimary()); return;
        }
        int y=100;
        for(int idx=0; idx<(int)gb_items.size(); ++idx){
            const auto& it = gb_items[idx];
            SDL_Rect card{NAV_W+12, y-6, WIN_W-NAV_W-24, rowH-16};
            SDL_Rect shadow=card; shadow.x+=2; shadow.y+=2;
            fill(C(0,0,0,80), shadow);
            fillRounded(themeCanvasBg(), card, 18);
            SDL_Rect row{card.x+6, card.y+4, card.w-12, card.h-8};
            if(idx==browse_cursor) fillRounded(themeHighlight(), row, 12);

            thumb_cache.ensure(mods_root, it.id, it.thumb, log);
            auto itex = thumb_cache.small.find(it.id);
            if(itex!=thumb_cache.small.end() && itex->second.tex.valid()){
                SDL_Rect dst{ row.x+8, row.y+8, thW, thH };
                canvas.blitScaled(itex->second.tex, dst.x, dst.y, dst.w, dst.h);
            }else{
                SDL_Rect ph{ row.x+8, row.y+8, thW, thH }; fill(themeCardBg(), ph);
                text("no img", ph.x+16, ph.y+22, textMuted(), true);
            }
            text(it.name, row.x+thW+20, row.y+10, textPrimary());
            text(it.profile, row.x+thW+20, row.y+48, textMuted(), true);
            std::string meta = "ID " + std::to_string(it.id);
            text(meta, row.x+thW+20, row.y+78, textMuted(), true);

            y += rowH;
            if(y > WIN_H-40) break;
        }

        SDL_Rect topBar{NAV_W, 88, WIN_W-NAV_W, 6}; fill(themeNavBg(), topBar);
        SDL_Rect botBar{NAV_W, WIN_H-40, WIN_W-NAV_W, 6}; fill(themeNavBg(), botBar);
    }

    void drawModInfoOverlay(){
        if(!info_overlay || info_mod_index<0 || info_mod_index>=(int)gb_items.size()) return;
        SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,180), dim);
        int panelW = WIN_W * 4 / 5;
        int panelH = WIN_H * 4 / 5;
        int px = (WIN_W - panelW)/2;
        int py = (WIN_H - panelH)/2;
        SDL_Rect panel{px,py,panelW,panelH}; fillRounded(themeNavBg(), panel, 20);
        SDL_Rect inner{px+8, py+8, panelW-16, panelH-16}; fillRounded(themePanelBg(), inner, 18);
        const auto& item = gb_items[info_mod_index];
        text(item.name, px+24, py+28, textTitle());
        text(item.profile, px+24, py+60, textMuted(), true);

        int descWidth = (panelW * 3)/5;
        int descX = px + 24;
        int descY = py + 100;
        SDL_Rect descBox{descX-4, descY-8, descWidth+8, panelH-180};
        fillRounded(themeCanvasBg(), descBox, 16);

        std::string descText = info_desc;
        if(descText.empty()){
            if(info_loading.load()) descText = "Loading description...";
            else if(info_failed) descText = "Description unavailable.";
            else descText = "No description provided.";
        }
        bool smallFont = font_small.ready();
        std::vector<std::string> descLines;
        gfx::wrapTextLines(descText, descWidth-16, font, font_small, smallFont, descLines);
        const int lineH = smallFont ? 22 : 28;
        int usableHeight = descBox.h - 16;
        if(usableHeight <= 0) usableHeight = descBox.h;
        int visibleLines = std::max(1, usableHeight / lineH);
        info_desc_visible_lines = visibleLines;
        info_desc_total_lines = (int)descLines.size();
        if(info_desc_total_lines == 0){
            descLines.push_back(descText);
            info_desc_total_lines = (int)descLines.size();
        }
        int maxScroll = std::max(0, info_desc_total_lines - info_desc_visible_lines);
        if(info_desc_scroll > maxScroll) info_desc_scroll = maxScroll;
        if(info_desc_scroll < 0) info_desc_scroll = 0;

        int ty = descY;
        int endLine = std::min(info_desc_scroll + visibleLines, info_desc_total_lines);
        for(int i=info_desc_scroll; i<endLine; ++i){
            text(descLines[i], descX, ty, textPrimary(), smallFont);
            ty += lineH;
        }
        if(info_desc_total_lines > visibleLines){
            SDL_Rect track{ descBox.x + descBox.w - 8, descBox.y + 6, 4, descBox.h - 12 };
            fill(themeHighlight(), track);
            float ratio = (float)visibleLines / (float)info_desc_total_lines;
            int thumbH = std::max(12, (int)(track.h * ratio));
            int thumbY = track.y;
            int range = track.h - thumbH;
            if(range > 0 && info_desc_total_lines > visibleLines){
                float pos = (float)info_desc_scroll / (float)(info_desc_total_lines - visibleLines);
                thumbY += (int)(range * pos);
            }
            SDL_Rect thumb{ track.x, thumbY, track.w, thumbH };
            fill(textAccent(), thumb);
        }

        int imageX = descX + descWidth + 32;
        int imageW = panelW - (imageX - px) - 32;

        int authorsHeight = 150;
        SDL_Rect authorsBox{imageX, descY, imageW, authorsHeight};
        fillRounded(themeCanvasBg(), authorsBox, 16);
        text("Authors", authorsBox.x, authorsBox.y - 22, textTitle(), true);
        std::string authorsText;
        if(info_authors.empty()){
            authorsText = info_loading.load() ? "Loading author information..." : "No author information.";
        }else{
            for(const auto& group : info_authors){
                if(!group.title.empty()){
                    authorsText += group.title + ":\n";
                }
                for(const auto& author : group.authors){
                    authorsText += "  - ";
                    if(!author.role.empty()){
                        authorsText += author.role;
                        if(!author.name.empty()) authorsText += ": ";
                    }
                    authorsText += author.name;
                    authorsText.push_back('\n');
                }
                authorsText.push_back('\n');
            }
        }
        bool useSmall = font_small.ready();
        std::vector<std::string> authorLines;
        gfx::wrapTextLines(authorsText, authorsBox.w - 16, font, font_small, useSmall, authorLines);
        const int authorLineH = useSmall ? 22 : 28;
        int authorHeight = std::max(1, (authorsBox.h - 16) / authorLineH);
        info_authors_visible_lines = authorHeight;
        info_authors_total_lines = (int)authorLines.size();
        if(info_authors_total_lines == 0){
            authorLines.push_back(authorsText);
            info_authors_total_lines = (int)authorLines.size();
        }
        int maxAuthScroll = std::max(0, info_authors_total_lines - info_authors_visible_lines);
        if(info_authors_scroll > maxAuthScroll) info_authors_scroll = maxAuthScroll;
        if(info_authors_scroll < 0) info_authors_scroll = 0;
        int ay = authorsBox.y + 8;
        int authorEnd = std::min(info_authors_scroll + info_authors_visible_lines, info_authors_total_lines);
        for(int i=info_authors_scroll; i<authorEnd; ++i){
            text(authorLines[i], authorsBox.x + 8, ay, textPrimary(), useSmall);
            ay += authorLineH;
        }
        if(info_authors_total_lines > info_authors_visible_lines){
            SDL_Rect track{ authorsBox.x + authorsBox.w - 8, authorsBox.y + 6, 4, authorsBox.h - 12 };
            fill(themeHighlight(), track);
            float ratio = (float)info_authors_visible_lines / (float)info_authors_total_lines;
            int thumbH = std::max(12, (int)(track.h * ratio));
            int thumbY = track.y;
            int range = track.h - thumbH;
            if(range > 0){
                float pos = (float)info_authors_scroll / (float)(info_authors_total_lines - info_authors_visible_lines);
                thumbY += (int)(range * pos);
            }
            SDL_Rect thumb{ track.x, thumbY, track.w, thumbH };
            fill(textAccent(), thumb);
        }

        int imageY = authorsBox.y + authorsBox.h + 16;
        int imageH = panelH - (imageY - py) - 80;
        if(imageH < 120) imageH = 120;
        SDL_Rect imgBox{imageX, imageY, imageW, imageH};
        fillRounded(themeCardBg(), imgBox, 18);
        info_image_rect = imgBox;
        int currentKey = item.id;
        std::string currentUrl = item.thumb;
        if(!info_gallery_images.empty()){
            if(info_gallery_index < 0 || info_gallery_index >= (int)info_gallery_images.size()){
                info_gallery_index = 0;
            }
            if(!info_gallery_images.empty()){
                currentKey = info_gallery_images[info_gallery_index].key;
                currentUrl = info_gallery_images[info_gallery_index].url.empty() ? item.thumb : info_gallery_images[info_gallery_index].url;
            }
        }
        thumb_cache.ensure(mods_root, currentKey, currentUrl, log);
        thumb_cache.ensureLarge(mods_root, currentKey, currentUrl, log);
        auto texIt = thumb_cache.large.find(currentKey);
        if(texIt!=thumb_cache.large.end() && texIt->second.tex.valid()){
            int tw = texIt->second.tex.w;
            int th = texIt->second.tex.h;
            float scale = std::min((float)imageW / tw, (float)imageH / th);
            int dw = std::max(1, (int)(tw * scale));
            int dh = std::max(1, (int)(th * scale));
            int dx = imageX + (imageW - dw)/2;
            int dy = imageY + (imageH - dh)/2;
            canvas.blitScaled(texIt->second.tex, dx, dy, dw, dh);
        }else{
            text("No image available", imageX + 16, imageY + 24, textMuted(), true);
        }
        if(info_gallery_images.size() > 1){
            std::string hint = "[X] View Gallery (" + std::to_string(info_gallery_index+1) + "/" + std::to_string(info_gallery_images.size()) + ")";
            text(hint, imgBox.x, imgBox.y - 22, textMuted(), true);
        }

        std::string instruct = "[A] Download    [B] Close";
        int tw = textw(instruct, true);
        text(instruct, px + panelW/2 - tw/2, py + panelH - 40, textAccent(), true);
            if(info_gallery_active){
                SDL_Rect overlay{0,0,WIN_W,WIN_H};
                fill(C(0,0,0,200), overlay);
                if(!info_gallery_images.empty()){
                int cur = info_gallery_index;
                if(cur < 0 || cur >= (int)info_gallery_images.size()) cur = 0;
                int key = info_gallery_images[cur].key;
                std::string url = info_gallery_images[cur].url;
                thumb_cache.ensure(mods_root, key, url, log);
                thumb_cache.ensureLarge(mods_root, key, url, log);
                auto gtex = thumb_cache.large.find(key);
                int gW = WIN_W * 3 / 4;
                int gH = WIN_H * 3 / 4;
                    SDL_Rect frame{ (WIN_W - gW)/2, (WIN_H - gH)/2, gW, gH };
                    fillRounded(themePanelBg(), frame, 24);
                if(gtex!=thumb_cache.large.end() && gtex->second.tex.valid()){
                    int tw = gtex->second.tex.w;
                    int th = gtex->second.tex.h;
                    float scale = std::min((float)frame.w / tw, (float)frame.h / th);
                    int dw = std::max(1, (int)(tw * scale));
                    int dh = std::max(1, (int)(th * scale));
                    int dx = frame.x + (frame.w - dw)/2;
                    int dy = frame.y + (frame.h - dh)/2;
                    canvas.blitScaled(gtex->second.tex, dx, dy, dw, dh);
                }else{
                    text("Loading image...", frame.x + 20, frame.y + 20, textPrimary(), true);
                }
                std::string idxStr = std::to_string(cur+1) + " / " + std::to_string(info_gallery_images.size());
                text(idxStr, frame.x + frame.w - textw(idxStr,true) - 12, frame.y + frame.h - 36, textAccent(), true);
            }else{
                text("No gallery images", WIN_W/2 - 80, WIN_H/2, textPrimary(), true);
            }
            text("[L/R][< >] navigate   [B] close", WIN_W/2 - 150, WIN_H - 60, textAccent(), true);
        }
    }

    void drawFilePickerOverlay(){
        if(!file_picker_open) return;
        SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,200), dim);
        int panelW = WIN_W * 3 / 5;
        int panelH = WIN_H * 3 / 5;
        if(panelW < 600) panelW = 600;
        if(panelH < 360) panelH = 360;
        int px = (WIN_W - panelW)/2;
        int py = (WIN_H - panelH)/2;
        file_picker_panel_rect = SDL_Rect{px,py,panelW,panelH};
        SDL_Rect panel{px,py,panelW,panelH}; fillRounded(themeNavBg(), panel, 20);
        SDL_Rect inner{px+8, py+8, panelW-16, panelH-16}; fillRounded(themePanelBg(), inner, 18);
        text("Select download file", px+24, py+36, textTitle());
        std::string subtitle = "Choose one of the files provided for this mod.";
        text(subtitle, px+24, py+68, textMuted(), true);
        std::string instruct = "[A] Download  [B] Cancel  [Touch] Tap entry";
        text(instruct, px+24, py + panelH - 40, textAccent(), true);
        file_picker_list_rect = SDL_Rect{px+24, py+96, panelW-48, panelH-152};
        int rowH = 96;
        int visible = file_picker_list_rect.h / rowH;
        if(visible < 1) visible = 1;
        int count = (int)file_picker_files.size();
        if(count == 0){
            text("No downloadable files were returned for this mod.", px+24, py + panelH/2, textPrimary(), true);
            return;
        }
        if(file_picker_cursor < file_picker_scroll) file_picker_scroll = file_picker_cursor;
        if(file_picker_cursor >= file_picker_scroll + visible) file_picker_scroll = std::max(0, file_picker_cursor - visible + 1);
        for(int i=0; i<visible; ++i){
            int idx = file_picker_scroll + i;
            if(idx >= count) break;
            SDL_Rect row{ file_picker_list_rect.x, file_picker_list_rect.y + i*rowH, file_picker_list_rect.w, rowH-8 };
            SDL_Rect shadow = row; shadow.x +=2; shadow.y +=2;
            fill(C(0,0,0,60), shadow);
            fillRounded(idx==file_picker_cursor ? themeHighlight() : themeCanvasBg(), row, 14);
            auto& file = file_picker_files[idx];
            std::string name = file.name.empty() ? "Download " + std::to_string(idx+1) : file.name;
            if((int)name.size() > 48) name = name.substr(0, 45) + "...";
            text(name, row.x + 12, row.y + 18, textTitle());
            std::string meta = formatFileSize(file.size_bytes) + "  |  " + formatFileDate(file.timestamp);
            text(meta, row.x + 12, row.y + 46, textMuted(), true);
            std::string desc = file.description.empty() ? "No description provided." : file.description;
            std::vector<std::string> lines;
            gfx::wrapTextLines(desc, row.w - 24, font, font_small, font_small.ready(), lines);
            if(!lines.empty()){
                text(lines[0], row.x + 12, row.y + 68, textPrimary(), true);
            }
        }
        if(count > visible){
            int trackH = file_picker_list_rect.h;
            SDL_Rect track{ file_picker_list_rect.x + file_picker_list_rect.w - 8, file_picker_list_rect.y, 4, trackH };
            fill(themeHighlight(), track);
            int thumbH = std::max(12, trackH * visible / count);
            int range = trackH - thumbH;
            int thumbY = track.y;
            if(range > 0){
                float pos = (float)file_picker_scroll / (float)(count - visible);
                thumbY += (int)(range * pos);
            }
            SDL_Rect thumb{ track.x, thumbY, track.w, thumbH };
            fill(textAccent(), thumb);
        }
    }

    void drawProfiles(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Profiles", NAV_W+20, 24, textTitle());
        text("[A] load  [X] overwrite  [Y] save as new  [B] delete  [-] jump here", NAV_W+20, 60, textMuted(), true);
        int item_h=34, top=100, max_vis=profileVisibleCount();
        if(profile_cursor<profile_scroll) profile_scroll=profile_cursor;
        if(profile_cursor>=profile_scroll+max_vis) profile_scroll=std::max(0,profile_cursor-max_vis+1);
        if(profile_list.empty()){
            text("No profiles saved. Press Y to create one.", NAV_W+20, top, textPrimary());
        }else{
            for(int i=0;i<max_vis;i++){
                int idx = profile_scroll+i; if(idx >= (int)profile_list.size()) break;
                const auto& prof = profile_list[idx];
                SDL_Rect card{NAV_W+12, top+i*item_h-2, WIN_W-NAV_W-24, item_h+6};
            SDL_Rect shadow=card; shadow.x+=2; shadow.y+=2;
            fill(C(0,0,0,80), shadow);
            fillRounded(themeCanvasBg(), card, 12);
                SDL_Rect row{card.x+6, card.y+3, card.w-12, card.h-6};
                if(idx==profile_cursor) fillRounded(themeHighlight(), row, 10);
                text(prof.label, row.x+8, row.y+6, textPrimary());
                text("ID: " + prof.id, row.x+280, row.y+6, textMuted(), true);
            }
        }
        if(!profile_message.empty()){
            text(profile_message, NAV_W+20, WIN_H-36, textAccent(), true);
        }
    }

    void drawSettings(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Settings", NAV_W+20, 24, textTitle());
        int y = 80;
        std::string touchState = touch_controls_enabled ? "ENABLED" : "DISABLED";
        std::string touchLine = std::string("Touch controls: ") + touchState + "  [A] toggle";
        text(touchLine, NAV_W+20, y, textPrimary());
        y += 30;
        text("When disabled, touch input is ignored and only buttons work.", NAV_W+20, y, textMuted(), true);

        y += 56;
        text("Legacy mods copy", NAV_W+20, y, textTitle());
        y += 32;
        std::string copyLine = std::string("[X] Copy once from ") + MODS_ROOT_LEGACY;
        text(copyLine, NAV_W+20, y, textPrimary(), true);
        y += 28;
        text(std::string("to ") + MODS_ROOT_NEW, NAV_W+20, y, textPrimary(), true);
        y += 28;
        text("Existing files in the new folder will be overwritten. Use to migrate old installs.", NAV_W+20, y, textMuted(), true);

        y += 56;
        text("Updates", NAV_W+20, y, textTitle());
        y += 32;
        text("[Y] Check for updates", NAV_W+20, y, textPrimary(), true);
        y += 28;
        if(update_checking){
            text("Checking remote feed...", NAV_W+20, y, textMuted(), true);
            y += 26;
        }else if(!update_status.empty()){
            std::vector<std::string> lines;
            gfx::wrapTextLines(update_status, WIN_W - NAV_W - 40, font, font_small, true, lines);
            int step = font_small.ready() ? 22 : 26;
            for(const auto& line : lines){
                text(line, NAV_W+20, y, textPrimary(), true);
                y += step;
            }
        }
        y += 28;
        if(update_available){
            text("[ZL] Download & install latest release", NAV_W+20, y, textPrimary(), true);
        }else{
            text("No pending download. Run check above first.", NAV_W+20, y, textMuted(), true);
        }

        if(!settings_message.empty()){
            y = WIN_H - 48;
            text(settings_message, NAV_W+20, y, textAccent(), true);
        }
    }

    void drawLog(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("Log", NAV_W+20, 24, textTitle());

        const int topY = 64;
        const int lineH = 20;
        const int max_vis = (WIN_H - topY - 48) / lineH;
        int total_lines = (int)log_lines.size() + (log_partial_line.empty() ? 0 : 1);
        int max_scroll = std::max(0, total_lines - max_vis);
        if(log_scroll > max_scroll) log_scroll = max_scroll;

        int start = std::max(0, total_lines - max_vis - log_scroll);
        int y = topY;
        int current = 0;
        int drawn = 0;
        for(const auto& line : log_lines){
            if(current >= start){
                text(line, NAV_W+20, y, textPrimary(), true);
                y += lineH;
                drawn++;
                if(drawn >= max_vis) break;
            }
            current++;
        }
        if(drawn < max_vis && !log_partial_line.empty() && current >= start){
            text(log_partial_line, NAV_W+20, y, textPrimary(), true);
            y += lineH;
            drawn++;
        }

        char info[96];
        snprintf(info, sizeof(info), "[Up/Down] scroll  [ZL/ZR] page  [X] bottom  [Y] clear   %d/%d",
                 total_lines - log_scroll, total_lines);
        text(info, NAV_W+20, WIN_H-28, textMuted(), true);
    }

    void drawAbout(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(themePanelBg(),panel);
        text("About", NAV_W+20, 24, textTitle());
        std::string aboutText = "Switch-side Trinity mod merger. ZIPs supported. Conflicts preview. romfs + exefs. Browse with thumbnails.";
        std::vector<std::string> lines;
        gfx::wrapTextLines(aboutText, WIN_W - NAV_W - 40, font, font_small, true, lines);
        int y = 70;
        for(const auto& line : lines){
            text(line, NAV_W+20, y, textPrimary(), true);
            y += 26;
        }
    }

    void render(){
        canvas.clear(themeCanvasBg());
        drawMenuBar();
        switch(screen){
            case Screen::Mods:   drawMods(); break;
            case Screen::Target: drawTarget(); break;
            case Screen::Apply:  drawApply(); break;
            case Screen::Browse: drawBrowse(); break;
            case Screen::Profiles: drawProfiles(); break;
            case Screen::Settings: drawSettings(); break;
            case Screen::Log:    drawLog(); break;
            case Screen::About:  drawAbout(); break;
        }
        if(info_overlay) drawModInfoOverlay();
        if(file_picker_open) drawFilePickerOverlay();
#ifdef APP_DEBUG_BUILD
        const float mb = 1024.0f * 1024.0f;
        int ty = 8;
        u64 procTotal=0, procUsed=0;
        if(R_SUCCEEDED(svcGetInfo(&procTotal, InfoType_TotalNonSystemMemorySize, CUR_PROCESS_HANDLE, 0)) &&
           R_SUCCEEDED(svcGetInfo(&procUsed,  InfoType_UsedNonSystemMemorySize,  CUR_PROCESS_HANDLE, 0)) &&
           procTotal>0){
            float usedMB = procUsed / mb;
            float totalMB = procTotal / mb;
            char heapInfo[96];
            snprintf(heapInfo, sizeof(heapInfo), "Heap: %.1f/%.1f MB", usedMB, totalMB);
            int textWidth = textw(heapInfo, true);
            int tx = WIN_W - textWidth - 12;
            SDL_Rect bg{tx-8, ty-6, textWidth+16, 26};
            fill(C(0,0,0,160), bg);
            text(heapInfo, tx, ty, textAccent(), true);
            ty += 26;
        }
        u64 sysTotal=0, sysUsed=0;
        if(R_SUCCEEDED(svcGetSystemInfo(&sysTotal, SystemInfoType_TotalPhysicalMemorySize, 0, 0)) &&
           R_SUCCEEDED(svcGetSystemInfo(&sysUsed,  SystemInfoType_UsedPhysicalMemorySize,  0, 0)) &&
           sysTotal>0){
            float sysUsedMB = sysUsed / mb;
            float sysTotalMB = sysTotal / mb;
            char sysInfo[96];
            snprintf(sysInfo, sizeof(sysInfo), "Sys: %.1f/%.1f MB", sysUsedMB, sysTotalMB);
            int sysWidth = textw(sysInfo, true);
            int tx = WIN_W - sysWidth - 12;
            SDL_Rect bg2{tx-8, ty-6, sysWidth+16, 26};
            fill(C(0,0,0,160), bg2);
            text(sysInfo, tx, ty, textAccent(), true);
        }
#endif
        if (applying.load()){
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);

            text("Applying mods...  [B] cancel", WIN_W/2 - 220, WIN_H/2 - 80, textTitle());
            size_t done = app_prog.done.load(), total = app_prog.total.load();
            long long bytes = app_prog.bytes.load();

            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(themeCardBg(), bar);
            int pw = (total>0) ? (int)((done * w) / total) : 0;
            if(pw<0) pw=0;
            if(pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(120,200,120), prog);

            char buf[128];
            double pct = (total>0)? (100.0 * (double)done / (double)total) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%zu / %zu)  %lld KB", pct, done, total, bytes/1024);
            text(buf, bar.x, bar.y + 34, textPrimary(), true);
        }
        if (downloading.load()){
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);

            text("Downloading...  [B] cancel", WIN_W/2 - 220, WIN_H/2 - 80, textTitle());
            long long now = dl_prog.now.load();
            long long tot = dl_prog.total.load();
            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(themeCardBg(), bar);
            int pw = (tot>0) ? (int)((now * w) / tot) : 0;
            if (pw<0) pw=0;
            if (pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(80,180,255), prog);

            char buf[128]; double pct = (tot>0)? (100.0 * (double)now / (double)tot) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%lld / %lld KB)", pct, now/1024, tot/1024);
            text(buf, bar.x, bar.y + 34, textPrimary(), true);
        }
        if(update_downloading.load()){
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);

            text("Downloading update...  [B] cancel", WIN_W/2 - 260, WIN_H/2 - 80, textTitle());
            long long bytesNow = update_prog.now.load();
            long long bytesTot = update_prog.total.load();
            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(themeCardBg(), bar);
            int pw = (bytesTot>0) ? (int)((bytesNow * w) / bytesTot) : 0;
            if(pw<0) pw=0;
            if(pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(140,200,255), prog);

            char buf[160];
            double pct = (bytesTot>0) ? (100.0 * (double)bytesNow / (double)bytesTot) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%s / %s)", pct,
                     formatFileSize(bytesNow).c_str(), formatFileSize(bytesTot).c_str());
            text(buf, bar.x, bar.y + 34, textPrimary(), true);
        }
        if(legacy_copying.load()){
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);
            text("Copying legacy mods...  [B] cancel", WIN_W/2 - 260, WIN_H/2 - 80, textTitle());
            long long bytesNow = legacy_prog.bytes_done.load();
            long long bytesTot = legacy_prog.bytes_total.load();
            size_t filesNow = legacy_prog.files_done.load();
            size_t filesTot = legacy_prog.files_total.load();
            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(themeCardBg(), bar);
            int pw = (bytesTot>0) ? (int)((bytesNow * w) / bytesTot) : 0;
            if(pw<0) pw=0;
            if(pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(200,200,120), prog);
            char buf[160];
            double pct = (bytesTot>0) ? (100.0 * (double)bytesNow / (double)bytesTot) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%s / %s)", pct,
                formatFileSize(bytesNow).c_str(), formatFileSize(bytesTot).c_str());
            text(buf, bar.x, bar.y + 34, textPrimary(), true);
            char files[96];
            snprintf(files,sizeof(files),"Files: %zu / %zu", filesNow, filesTot);
            text(files, bar.x, bar.y + 58, textPrimary(), true);
        }
        presenter.present(canvas.data().data(), canvas.data().size() * sizeof(uint32_t));
    }
