#include "mods.hpp"

#include "fs/fs_utils.hpp"
#include "zip/zip_utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <unordered_set>
#include <strings.h>
#include <cstdio>
#include <unistd.h>

namespace mods {

namespace {
static const char* kKnownTopDirsLA[]{
    "ai_influence","avalon","field","ik_ai_behavior","ik_chara","ik_demo","ik_effect",
    "ik_event","ik_message","ik_pokemon","light","param_ai","param_chr","script",
    "system_resource","system","ui","world","arc","shaders"
};
// TODO: SV list is currently identical; adjust as needed for SV-specific layouts.
static const char* kKnownTopDirsSV[]{
    "ai_influence","avalon","field","ik_ai_behavior","ik_chara","ik_demo","ik_effect",
    "ik_event","ik_message","ik_pokemon","light","param_ai","param_chr","script",
    "system_resource","system","ui","world","arc","shaders"
};
static const char* const* known_dirs_for(Game game){
    return (game==Game::SV) ? kKnownTopDirsSV : kKnownTopDirsLA;
}
static size_t known_dirs_count_for(Game game){
    return (game==Game::SV) ? (sizeof(kKnownTopDirsSV)/sizeof(kKnownTopDirsSV[0]))
                            : (sizeof(kKnownTopDirsLA)/sizeof(kKnownTopDirsLA[0]));
}
static bool is_known_top(const std::string& n, Game game){
    const char* const* arr = known_dirs_for(game);
    size_t count = known_dirs_count_for(game);
    for(size_t i=0;i<count;i++) if(strcasecmp(n.c_str(), arr[i])==0) return true;
    return false;
}

static bool has_known_child(const std::string& dir, Game game){
    DIR* d=opendir(dir.c_str()); if(!d) return false;
    bool ok=false; while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string c=e->d_name; std::string p=dir+"/"+c; struct stat st{};
        if(stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode) && is_known_top(c, game)){ ok=true; break; }
    } closedir(d); return ok;
}
static bool find_romfs_root_rec(const std::string& start, std::string& out, Game game, int depth_limit=6){
    if(depth_limit<0) return false;
    if(fsx::isdir(start+"/romfs")){ out=start+"/romfs"; return true; }
    if(has_known_child(start, game)){ out=start; return true; }
    DIR* d=opendir(start.c_str()); if(!d) return false;
    bool found=false;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string c=e->d_name; std::string p=start+"/"+c; struct stat st{};
        if(stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode)){
            if(find_romfs_root_rec(p, out, game, depth_limit-1)){ found=true; break; }
        }
    } closedir(d); return found;
}
static bool find_first_dir_named(const std::string& start, const char* name, std::string& out, int depth_limit=6){
    if(depth_limit<0) return false;
    std::string candidate = start + "/" + name;
    if(fsx::isdir(candidate)){ out=candidate; return true; }
    DIR* d=opendir(start.c_str()); if(!d) return false;
    bool found=false;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string c=e->d_name; std::string p=start+"/"+c; struct stat st{};
        if(stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode)){
            if(find_first_dir_named(p, name, out, depth_limit-1)){ found=true; break; }
        }
    } closedir(d); return found;
}

static std::string strip_ext(const std::string& p){
    auto s=p.find_last_of('.'); if(s==std::string::npos) return p; return p.substr(0,s);
}

static std::string base_name(const std::string& p){
    auto s=p.find_last_of('/'); return (s==std::string::npos)?p:p.substr(s+1);
}

static bool ends_with_ci(const std::string& s, const char* suffix){
    size_t n=s.size(), m=strlen(suffix); if(m>n) return false;
    return strcasecmp(s.c_str()+n-m, suffix)==0;
}

static std::vector<std::pair<std::string, std::vector<int>>> compute_conflicts_internal(const std::vector<ModEntry>& mods){
    std::map<std::string, std::vector<int>> m;
    for(size_t i=0;i<mods.size();++i){
        if(!mods[i].selected) continue;
        for(const auto& rel : mods[i].files){
            size_t slash = rel.find('/'); if(slash==std::string::npos) continue;
            std::string space = rel.substr(0, slash);
            std::string sub   = rel.substr(slash+1);
            std::string key   = space + ":" + sub;
            auto& vec = m[key];
            if(vec.empty() || vec.back()!=(int)i) vec.push_back((int)i);
        }
    }
    std::vector<std::pair<std::string, std::vector<int>>> out;
    for(auto& kv : m) if(kv.second.size()>=2) out.push_back(kv);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    return out;
}

} // namespace

std::vector<ModEntry> scan_root(const std::string& root, std::string& log, Game game){
    std::vector<ModEntry> out;
    if(!fsx::isdir(root)){ log+="[scan] mods_root missing: "+root+"\n"; return out; }
    const std::string cache_root = root + "/_unzipped";
    auto ents=fsx::list_dirents(root);
    for(auto& de: ents){
        std::string base = base_name(de.path);
        if(de.is_dir && strcasecmp(base.c_str(), "_unzipped")==0) continue;
        std::string display_name, candidate_path;
        if(de.is_file && ends_with_ci(de.path, ".zip")){
            long zsize = fsx::file_size(de.path);
            time_t zmt  = fsx::file_mtime(de.path);
            std::string zipname = base_name(de.path);
            std::string out_dir = cache_root + "/" + strip_ext(zipname);
            long osz=0; time_t omt=0;
            bool up_to_date = fsx::isdir(out_dir) && zipx::read_stamp(out_dir, osz, omt) && (osz==zsize) && (omt==zmt);
            if(up_to_date){ log += "[zip] cache hit: " + out_dir + "\n"; }
            else{
                if(fsx::isdir(out_dir)){
                    if(!fsx::rmtree(out_dir)){
                        log += "[zip] clean fail: " + out_dir + "\n"; continue;
                    }
                }
                log += "[zip] extracting: " + de.path + " -> " + out_dir + "\n";
                if(!zipx::unzip_to(de.path, out_dir, log)){
                    log += "[zip] extract fail: " + de.path + "\n";
                    continue;
                }
                zipx::write_stamp(out_dir, zsize, zmt);
            }
            display_name = strip_ext(zipname);
            candidate_path = out_dir;
        } else if(de.is_dir){
            display_name = base;
            candidate_path = de.path;
        } else {
            continue;
        }

        std::string romfs_root;
        std::string exefs_root;
        find_romfs_root_rec(candidate_path, romfs_root, game, 8);
        find_first_dir_named(candidate_path, "exefs", exefs_root, 8);

        bool fallback_mode=false;
        std::vector<std::string> fallback_entries;
        if(romfs_root.empty() && exefs_root.empty()){
            std::vector<std::string> rels;
            fsx::list_files_rec(candidate_path,"",rels);
            for(auto& rel : rels){
                std::string norm=rel; std::replace(norm.begin(),norm.end(),'\\','/');
                size_t slash = norm.find('/');
                std::string top = (slash==std::string::npos)?norm:norm.substr(0,slash);
                if(is_known_top(top, game)){
                    fallback_entries.push_back("romfs/"+norm);
                }
            }
            if(fallback_entries.empty()){
                log += "[scan] skip (no romfs or exefs): " + candidate_path + "\n";
                continue;
            }
            fallback_mode = true;
        }
        ModEntry m{};
        m.name  = display_name;
        m.path  = de.path;
        m.romfs = romfs_root;
        m.exefs = exefs_root;
        m.selected = true;

        m.files.clear();
        if(!fallback_mode && !m.romfs.empty()){
            std::vector<std::string> rels; fsx::list_files_rec(m.romfs,"",rels);
            for(auto& r: rels){
                std::string norm=r; std::replace(norm.begin(),norm.end(),'\\','/');
                if(norm=="arc/data.trpfd" || norm=="arc/data.trpfd.csv") continue;
                auto pos=norm.find('/'); std::string top=pos==std::string::npos?norm:norm.substr(0,pos);
                if(!is_known_top(top, game)) continue;
                m.files.push_back("romfs/"+norm);
            }
        }
        if(!m.exefs.empty()){
            std::vector<std::string> rels; fsx::list_files_rec(m.exefs,"",rels);
            for(auto& r: rels){ std::string norm=r; std::replace(norm.begin(),norm.end(),'\\','/'); m.files.push_back("exefs/"+norm); }
        }
        if(fallback_mode){
            m.romfs = candidate_path;
            m.files.insert(m.files.end(), fallback_entries.begin(), fallback_entries.end());
        }
        std::sort(m.files.begin(),m.files.end());
        m.files.erase(std::unique(m.files.begin(), m.files.end()), m.files.end());
        out.push_back(std::move(m));
    }
    return out;
}

bool clear_target_known(const std::string& target_romfs, std::string& log, Game game){
    size_t removed=0, failed=0;
    const char* const* arr = known_dirs_for(game);
    size_t count = known_dirs_count_for(game);
    for(size_t i=0;i<count;i++){
        std::string path = target_romfs + "/" + arr[i];
        if(fsx::isdir(path)){
            if(fsx::rmtree(path)) removed++;
            else { failed++; log += std::string("[clear] fail: ") + path + "\n"; }
        }
    }
    std::string content_root = target_romfs;
    if(content_root.size()>=6 && strcasecmp(content_root.c_str()+content_root.size()-6, "/romfs")==0) content_root.resize(content_root.size()-6);
    std::string exefs_dir = content_root + "/exefs";
    if(fsx::isdir(exefs_dir)){ if(fsx::rmtree(exefs_dir)) removed++; else { failed++; log += std::string("[clear] fail: ") + exefs_dir + "\n"; } }
    log += "[clear] removed="+std::to_string(removed)+" failed="+std::to_string(failed)+"\n";
    return failed==0;
}

std::vector<std::pair<std::string, std::vector<int>>> compute_conflicts(const std::vector<ModEntry>& mods){
    return compute_conflicts_internal(mods);
}

static bool copy_file(const std::string& src,const std::string& dst,size_t& out_bytes){
    out_bytes=0;
    FILE* in=fopen(src.c_str(),"rb"); if(!in) return false;
    auto slash=dst.find_last_of('/'); if(slash!=std::string::npos){ std::string d=dst.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)){ fclose(in); return false; } }
    FILE* out=fopen(dst.c_str(),"wb"); if(!out){ fclose(in); return false; }
    std::vector<char> buf(1<<20); // 1 MiB chunks to reduce sdcard seeks
    while(true){ size_t n=fread(buf.data(),1,buf.size(),in); if(n==0) break; if(fwrite(buf.data(),1,n,out)!=n){ fclose(in); fclose(out); return false; } out_bytes+=n; }
    fclose(in); fclose(out); return true;
}

bool apply_copy_progress(const std::vector<ModEntry>& mods_in,
                                const std::string& target_romfs,
                                std::string& log,
                                ApplyProg* prog){
    if(prog){ prog->done=0; prog->total=0; prog->bytes=0; prog->cancel=false; }

    bool any=false; for(auto& m:mods_in) any|=m.selected;
    if(!any){ log+="[apply] no mods selected\n"; return false; }
    if(!fsx::makedirs(target_romfs)){ log+="[apply] cannot create: "+target_romfs+"\n"; return false; }

    std::string content_root = target_romfs;
    if(content_root.size()>=6 && strcasecmp(content_root.c_str()+content_root.size()-6, "/romfs")==0) content_root.resize(content_root.size()-6);
    std::string target_exefs = content_root + "/exefs";

    auto classify_rel = [](const std::string& rel, std::string& sub)->int{
        size_t slash = rel.find('/');
        if(slash==std::string::npos) return 0;
        std::string space = rel.substr(0, slash);
        sub = rel.substr(slash+1);
        if(space=="romfs") return 1;
        if(space=="exefs") return 2;
        return 0;
    };

    size_t total_jobs=0;
    std::string rel_sub;
    for(const auto& m:mods_in) if(m.selected){
        for(const auto& rel: m.files){
            if(classify_rel(rel, rel_sub)){
                total_jobs++;
            }
        }
    }
    if(prog) prog->total = total_jobs;

    size_t copied=0, failed=0; long long bytes=0;
    size_t completed=0;
    bool canceled=false;
    std::string src_path, dst_path;
    std::string sub_path;
    for(const auto& m:mods_in){
        if(!m.selected) continue;
        for(const auto& rel: m.files){
            int space = classify_rel(rel, sub_path);
            if(space==0) continue;
            if(space==1){
                src_path = (m.romfs.empty()?std::string():m.romfs) + "/" + sub_path;
                dst_path = target_romfs + "/" + sub_path;
            }else{
                src_path = (m.exefs.empty()?std::string():m.exefs) + "/" + sub_path;
                dst_path = target_exefs + "/" + sub_path;
            }
            if(prog && prog->cancel.load()){ canceled=true; break; }
            size_t outb=0;
            if(copy_file(src_path, dst_path, outb)){ copied++; bytes += outb; }
            else { failed++; log += "[copy] fail "+src_path+" -> "+dst_path+"\n"; }
            completed++;
            if(prog){ prog->done = completed; prog->bytes = bytes; }
        }
        if(canceled) break;
    }

    char msg[160]; snprintf(msg,sizeof(msg),"[apply] files=%zu bytes=%lld failed=%zu\n", copied, bytes, failed);
    log += msg;
    if(prog && prog->cancel.load()) return false;
    return failed==0 && !canceled;
}

static std::string parent_dir(const std::string& path){
    auto slash = path.find_last_of('/');
    if(slash == std::string::npos) return {};
    return path.substr(0, slash);
}

static bool read_selection_file(const std::string& cache_path,
                                std::unordered_set<std::string>& keep,
                                std::string* display_name=nullptr){
    FILE* f = fopen(cache_path.c_str(), "r");
    if(!f) return false;
    char buf[1024];
    if(display_name) display_name->clear();
    while(fgets(buf, sizeof(buf), f)){
        std::string line(buf);
        while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
        if(line.empty()) continue;
        if(line.rfind("#name:", 0)==0){
            if(display_name){
                *display_name = line.substr(6);
                while(!display_name->empty() && ((*display_name)[0]==' ')) display_name->erase(display_name->begin());
            }
            continue;
        }
        if(line[0]=='#') continue;
        keep.insert(line);
    }
    fclose(f);
    return true;
}

bool save_selection_cache(const std::vector<ModEntry>& mods,
                          const std::string& cache_path,
                          std::string& log){
    if(cache_path.empty()) return false;
    std::string dir = parent_dir(cache_path);
    if(!dir.empty() && !fsx::isdir(dir)){
        if(!fsx::makedirs(dir)){
            log += "[mods] save_selection_cache: failed to create " + dir + "\n";
            return false;
        }
    }
    FILE* f = fopen(cache_path.c_str(), "w");
    if(!f){
        log += "[mods] save_selection_cache: fopen failed\n";
        return false;
    }
    size_t count=0;
    for(const auto& m : mods){
        if(!m.selected) continue;
        fputs(m.path.c_str(), f);
        fputc('\n', f);
        count++;
    }
    fclose(f);
    (void)count;
    return true;
}

bool load_selection_cache(const std::string& cache_path,
                          std::vector<ModEntry>& mods,
                          std::string& log){
    if(cache_path.empty()) return false;
    std::unordered_set<std::string> keep;
    if(!read_selection_file(cache_path, keep, nullptr)) return false;
    size_t hits=0;
    for(auto& m : mods){
        bool sel = keep.find(m.path)!=keep.end();
        m.selected = sel;
        if(sel) hits++;
    }
    char msg[128];
    snprintf(msg,sizeof(msg),"[mods] restored selection (%zu of %zu)\n", hits, mods.size());
    log += msg;
    return true;
}

static std::string sanitize_profile_id(const std::string& name){
    std::string out;
    out.reserve(name.size());
    for(char c : name){
        unsigned char uc = static_cast<unsigned char>(c);
        if((uc>='0' && uc<='9') || (uc>='a' && uc<='z')){
            out.push_back(static_cast<char>(tolower(uc)));
        }else if(uc>='A' && uc<='Z'){
            out.push_back(static_cast<char>(tolower(uc)));
        }else if(c==' ' || c=='-' || c=='_'){
            out.push_back('_');
        }
    }
    if(out.empty()) out = "profile";
    if(out.size()>48) out.resize(48);
    // collapse duplicate underscores
    std::string collapsed;
    collapsed.reserve(out.size());
    bool lastUnderscore=false;
    for(char ch : out){
        if(ch=='_'){
            if(lastUnderscore) continue;
            lastUnderscore=true;
        }else{
            lastUnderscore=false;
        }
        collapsed.push_back(ch);
    }
    while(!collapsed.empty() && collapsed.back()=='_') collapsed.pop_back();
    if(collapsed.empty()) collapsed = "profile";
    return collapsed;
}

static std::string profile_path_for(const std::string& dir, const std::string& id){
    if(dir.empty()) return id;
    std::string clean = id;
    std::string path = dir;
    if(!path.empty() && path.back()!='/') path.push_back('/');
    path += clean;
    path += ".txt";
    return path;
}

bool save_selection_profile(const std::vector<ModEntry>& mods,
                            const std::string& dir,
                            const std::string& display_name,
                            std::string& log){
    if(dir.empty()) return false;
    bool any=false;
    for(const auto& m:mods) if(m.selected){ any=true; break; }
    if(!any){
        log += "[mods] save profile: no mods selected\n";
        return false;
    }
    if(!fsx::isdir(dir)){
        if(!fsx::makedirs(dir)){
            log += "[mods] save profile: cannot create dir\n";
            return false;
        }
    }
    std::string id = sanitize_profile_id(display_name);
    std::string path = profile_path_for(dir, id);
    FILE* f = fopen(path.c_str(), "w");
    if(!f){
        log += "[mods] save profile: fopen failed\n";
        return false;
    }
    std::string label = display_name;
    if(label.empty()) label = id;
    fprintf(f, "#name:%s\n", label.c_str());
    for(const auto& m : mods){
        if(!m.selected) continue;
        fputs(m.path.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    char msg[128];
    snprintf(msg,sizeof(msg),"[mods] profile saved: %s (%s)\n", label.c_str(), id.c_str());
    log += msg;
    return true;
}

bool load_selection_profile(const std::string& dir,
                            const std::string& id,
                            std::vector<ModEntry>& mods,
                            std::string& log){
    if(dir.empty() || id.empty()) return false;
    std::string path = profile_path_for(dir, id);
    std::unordered_set<std::string> keep;
    if(!read_selection_file(path, keep, nullptr)){
        log += "[mods] load profile: missing " + id + "\n";
        return false;
    }
    size_t hits=0;
    for(auto& m : mods){
        bool sel = keep.find(m.path)!=keep.end();
        m.selected = sel;
        if(sel) hits++;
    }
    char msg[128];
    snprintf(msg,sizeof(msg),"[mods] profile loaded (%zu selected)\n", hits);
    log += msg;
    return true;
}

bool delete_selection_profile(const std::string& dir,
                              const std::string& id,
                              std::string& log){
    if(dir.empty() || id.empty()) return false;
    std::string path = profile_path_for(dir, id);
    if(unlink(path.c_str())==0){
        log += "[mods] profile deleted: " + id + "\n";
        return true;
    }
    log += "[mods] profile delete failed: " + id + "\n";
    return false;
}

bool list_selection_profiles(const std::string& dir,
                             std::vector<ProfileInfo>& out,
                             std::string& log){
    out.clear();
    if(dir.empty()) return false;
    if(!fsx::isdir(dir)){
        fsx::makedirs(dir);
        return true;
    }
    auto ents = fsx::list_dirents(dir);
    for(const auto& de : ents){
        if(!de.is_file) continue;
        auto slash = de.path.find_last_of('/');
        std::string base = (slash==std::string::npos)?de.path:de.path.substr(slash+1);
        auto dot = base.find_last_of('.');
        std::string id = (dot==std::string::npos)?base:base.substr(0,dot);
        std::unordered_set<std::string> dummy;
        std::string display;
        read_selection_file(de.path, dummy, &display);
        if(display.empty()){
            display = id;
            std::replace(display.begin(), display.end(), '_', ' ');
        }
        out.push_back(ProfileInfo{ id, display });
    }
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b){
        return a.label < b.label;
    });
    char msg[128];
    snprintf(msg,sizeof(msg),"[mods] profiles listed: %zu\n", out.size());
    log += msg;
    return true;
}

} // namespace mods
