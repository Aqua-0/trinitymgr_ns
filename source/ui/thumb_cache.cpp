#include "thumb_cache.hpp"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <iterator>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include "fs/fs_utils.hpp"
#include "gb/gamebanana.hpp"

namespace ui {
namespace {

constexpr int kMaxTextureDimension = 1024;
constexpr size_t kMaxTexturePixels = static_cast<size_t>(kMaxTextureDimension) * kMaxTextureDimension;

bool readFileBytes(const std::string& path, std::vector<unsigned char>& out){
    FILE* f = fopen(path.c_str(), "rb");
    if(!f) return false;
    fseek(f,0,SEEK_END);
    long sz = ftell(f);
    fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); return false; }
    out.resize(sz);
    size_t rd = fread(out.data(),1,sz,f);
    fclose(f);
    return rd == (size_t)sz;
}

std::string sniff_image_ext(const std::vector<unsigned char>& b){
    if(b.size()>=3 && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) return ".jpg";
    if(b.size()>=8 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') return ".png";
    if(b.size()>=12 && !memcmp(b.data(),"RIFF",4) && !memcmp(b.data()+8,"WEBP",4)) return ".webp";
    if(b.size()>=2 && b[0]=='B' && b[1]=='M') return ".bmp";
    return ".jpg";
}

bool decodeTextureFromBytes(const std::vector<unsigned char>& bytes, gfx::Texture& tex, std::string& log){
    int w=0,h=0,n=0;
    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if(!decoded){ log += "[thumb] decode fail\n"; return false; }
    if(w<=0 || h<=0){
        stbi_image_free(decoded);
        return false;
    }
    auto neededRatio = [&](int srcW, int srcH)->float{
        double pixels = static_cast<double>(srcW) * static_cast<double>(srcH);
        double dimRatio = std::max(srcW / static_cast<double>(kMaxTextureDimension),
                                   srcH / static_cast<double>(kMaxTextureDimension));
        double pixRatio = std::sqrt(std::max(1.0, pixels / static_cast<double>(kMaxTexturePixels)));
        double desired = std::max({1.0, dimRatio, pixRatio});
        return static_cast<float>(desired);
    };
    int targetW = w;
    int targetH = h;
    float ratio = neededRatio(w,h);
    if(ratio > 1.01f){
        targetW = std::max(1, static_cast<int>(std::round(w / ratio)));
        targetH = std::max(1, static_cast<int>(std::round(h / ratio)));
        char buf[160];
        snprintf(buf,sizeof(buf),"[thumb] downscale %dx%d -> %dx%d\n", w, h, targetW, targetH);
        log += buf;
    }
    tex.w = targetW;
    tex.h = targetH;
    tex.pixels.resize(static_cast<size_t>(targetW)*targetH);
    const float stepX = static_cast<float>(w) / targetW;
    const float stepY = static_cast<float>(h) / targetH;
    for(int y=0;y<targetH;++y){
        int sy = std::min(h-1, static_cast<int>(y * stepY));
        for(int x=0;x<targetW;++x){
            int sx = std::min(w-1, static_cast<int>(x * stepX));
            size_t srcIdx = static_cast<size_t>(sy)*w + sx;
            stbi_uc* p = decoded + srcIdx*4;
            tex.pixels[static_cast<size_t>(y)*targetW + x] =
                (uint32_t(p[3])<<24) | (uint32_t(p[0])<<16) | (uint32_t(p[1])<<8) | uint32_t(p[2]);
        }
    }
    stbi_image_free(decoded);
    return true;
}

bool fetchThumbBytes(int id, const std::string& mods_root, const std::string& url_in,
                     std::vector<unsigned char>& bytes, std::unordered_set<int>& tried, std::string& log){
    const std::string dir = mods_root + "/_thumbs";
    fsx::makedirs(dir);
    const std::string base = dir + "/" + std::to_string(id);
    const char* exts[] = {".jpg",".jpeg",".png",".webp",".bmp",".gif",".img"};
    for(const char* e : exts){
        std::string cand = base + e;
        if(fsx::isfile(cand)){
            if(readFileBytes(cand, bytes)) return true;
            unlink(cand.c_str());
        }
    }
    if(tried.count(id)) return false;
    std::string u = gb::normalize_url(url_in);
    if(u.empty()){
        std::string resolved;
        if(!gb::fetch_mod_thumb_url(id, resolved, log)){
            log += "[thumb] no url resolved id=" + std::to_string(id) + "\n";
            tried.insert(id);
            return false;
        }
        u = gb::normalize_url(resolved);
    }
    if(u.empty()){
        log += "[thumb] normalize_url empty\n";
        tried.insert(id);
        return false;
    }
    std::vector<unsigned char> data;
    if(!gb::http_get_bytes_ref(u, "https://gamebanana.com/games/23582", data, log)){
        log += "[thumb] download fail: " + u + "\n";
        tried.insert(id);
        return false;
    }
    std::string ext = sniff_image_ext(data);
    std::string path = base + ext;
    if(FILE* f=fopen(path.c_str(),"wb")){
        fwrite(data.data(),1,data.size(),f);
        fclose(f);
    }
    bytes.swap(data);
    return true;
}

} // namespace

void ThumbCache::touchSmall(int id){
    auto it = small_pos.find(id);
    if(it != small_pos.end()){
        small_lru.splice(small_lru.end(), small_lru, it->second);
        return;
    }
    small_lru.push_back(id);
    small_pos[id] = std::prev(small_lru.end());
}

void ThumbCache::touchLarge(int id){
    auto it = large_pos.find(id);
    if(it != large_pos.end()){
        large_lru.splice(large_lru.end(), large_lru, it->second);
        return;
    }
    large_lru.push_back(id);
    large_pos[id] = std::prev(large_lru.end());
}

void ThumbCache::enforceSmallLimit(){
    while(small.size() > kMaxSmallEntries && !small_lru.empty()){
        int evict = small_lru.front();
        small_lru.pop_front();
        small_pos.erase(evict);
        small.erase(evict);
    }
}

void ThumbCache::enforceLargeLimit(){
    while(large.size() > kMaxLargeEntries && !large_lru.empty()){
        int evict = large_lru.front();
        large_lru.pop_front();
        large_pos.erase(evict);
        large.erase(evict);
    }
}

bool ThumbCache::ensure(const std::string& mods_root, int id, const std::string& url_in, std::string& log){
    auto smallIt = small.find(id);
    if(smallIt != small.end() && smallIt->second.tex.valid()){
        touchSmall(id);
        return true;
    }
    std::vector<unsigned char> bytes;
    if(!fetchThumbBytes(id, mods_root, url_in, bytes, tried, log)) return false;
    gfx::Texture decoded;
    if(!decodeTextureFromBytes(bytes, decoded, log)){
        tried.insert(id);
        return false;
    }
    gfx::Texture scaled = std::move(decoded);
    scaled.scaleTo(96,64);
    small[id].tex = std::move(scaled);
    touchSmall(id);
    enforceSmallLimit();
    return true;
}

bool ThumbCache::ensureLarge(const std::string& mods_root, int id, const std::string& url_in, std::string& log){
    auto largeIt = large.find(id);
    if(largeIt != large.end() && largeIt->second.tex.valid()){
        touchLarge(id);
        return true;
    }
    std::vector<unsigned char> bytes;
    if(!fetchThumbBytes(id, mods_root, url_in, bytes, tried, log)) return false;
    gfx::Texture decoded;
    if(!decodeTextureFromBytes(bytes, decoded, log)){
        tried.insert(id);
        return false;
    }
    large[id].tex = decoded;
    touchLarge(id);
    enforceLargeLimit();
    return true;
}

void ThumbCache::clear(){
    small.clear();
    clearLarge();
    tried.clear();
    small_lru.clear();
    small_pos.clear();
}

void ThumbCache::clearLarge(){
    large.clear();
    large_lru.clear();
    large_pos.clear();
}

} // namespace ui
