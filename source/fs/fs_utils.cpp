#include "fs_utils.hpp"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>

namespace fsx {

bool isdir(const std::string& p){
    struct stat st{};
    return stat(p.c_str(), &st)==0 && S_ISDIR(st.st_mode);
}

bool isfile(const std::string& p){
    struct stat st{};
    return stat(p.c_str(), &st)==0 && S_ISREG(st.st_mode);
}

long file_size(const std::string& p){
    struct stat st{};
    return stat(p.c_str(), &st)==0 ? (long)st.st_size : -1;
}

time_t file_mtime(const std::string& p){
    struct stat st{};
    return stat(p.c_str(), &st)==0 ? st.st_mtime : 0;
}

bool makedirs(const std::string& path){
    if(path.empty()) return false;
    size_t start=0; std::string cur;
    if(path.rfind("sdmc:/",0)==0){cur="sdmc:/"; start=6;}
    else if(path.rfind("romfs:/",0)==0){cur="romfs:/"; start=7;}
    else if(path.rfind("host:/",0)==0){cur="host:/"; start=6;}
    else if(path[0]=='/'){cur="/"; start=1;} else return false;
    size_t i=start;
    while(i<=path.size()){
        size_t j=path.find('/',i);
        std::string token=(j==std::string::npos)?path.substr(i):path.substr(i,j-i);
        if(!token.empty()){
            if(!cur.empty() && cur.back()!='/') cur.push_back('/');
            cur+=token;
            if(!isdir(cur) && mkdir(cur.c_str(),0777)!=0 && errno!=EEXIST) return false;
        }
        if(j==std::string::npos) break;
        i=j+1;
    }
    return true;
}

bool rmtree(const std::string& root){
    if(!isdir(root)) return true;
    DIR* d=opendir(root.c_str()); if(!d) return false;
    struct dirent* e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.') continue;
        std::string p=root+"/"+e->d_name;
        struct stat st{}; if(stat(p.c_str(),&st)!=0) continue;
        if(S_ISDIR(st.st_mode)){ if(!rmtree(p)) { closedir(d); return false; } }
        else { if(unlink(p.c_str())!=0) { closedir(d); return false; } }
    }
    closedir(d);
    return rmdir(root.c_str())==0;
}

std::vector<DirEnt> list_dirents(const std::string& root){
    std::vector<DirEnt> out; DIR* d=opendir(root.c_str()); if(!d) return out;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string p=root+"/"+e->d_name; struct stat st{};
        if(stat(p.c_str(),&st)!=0) continue;
        out.push_back({p, S_ISDIR(st.st_mode), S_ISREG(st.st_mode)});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEnt& a, const DirEnt& b){ return a.path < b.path; });
    return out;
}

void list_files_rec(const std::string& root,const std::string& rel,std::vector<std::string>& out){
    std::string here=root+(rel.empty()?"":"/"+rel); DIR* d=opendir(here.c_str()); if(!d) return;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string name=e->d_name; std::string child_rel=rel.empty()?name:rel+"/"+name; std::string child_abs=root+"/"+child_rel;
        struct stat st{}; if(stat(child_abs.c_str(),&st)==0){
            if(S_ISDIR(st.st_mode)) list_files_rec(root,child_rel,out);
            else if(S_ISREG(st.st_mode)) out.push_back(child_rel);
        }
    } closedir(d);
}

} // namespace fsx
