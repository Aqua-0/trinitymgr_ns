// path: source/main_sdl.cpp
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <unordered_set>
#include <algorithm>
#include <curl/curl.h>
#include <atomic>
#include <mutex>
#include <malloc.h>  
#include <functional>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <zlib.h>

// ---------------- fs helpers ----------------
namespace fsx {
static bool isdir(const std::string& p){ struct stat st{}; return stat(p.c_str(), &st)==0 && S_ISDIR(st.st_mode); }
static bool isfile(const std::string& p){ struct stat st{}; return stat(p.c_str(), &st)==0 && S_ISREG(st.st_mode); }
static long file_size(const std::string& p){ struct stat st{}; return stat(p.c_str(), &st)==0 ? (long)st.st_size : -1; }
static time_t file_mtime(const std::string& p){ struct stat st{}; return stat(p.c_str(), &st)==0 ? st.st_mtime : 0; }

static bool makedirs(const std::string& path){
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

static bool rmtree(const std::string& root){
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

struct DirEnt { std::string path; bool is_dir; bool is_file; };
static std::vector<DirEnt> list_dirents(const std::string& root){
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

static void list_files_rec(const std::string& root,const std::string& rel,std::vector<std::string>& out){
    std::string here=root+(rel.empty()?"":"/"+rel); DIR* d=opendir(here.c_str()); if(!d) return;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string name=e->d_name; std::string child_rel=rel.empty()?name:rel+"/"+name; std::string child_abs=root+"/"+child_rel;
        struct stat st{}; if(stat(child_abs.c_str(),&st)==0){ if(S_ISDIR(st.st_mode)) list_files_rec(root,child_rel,out); else if(S_ISREG(st.st_mode)) out.push_back(child_rel); }
    } closedir(d);
}
} // ns fsx

// ---------------- simple ZIP (STORE/DEFLATE) ----------------
namespace zipx {
static inline uint16_t rd16(const unsigned char* p){ return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }
static inline uint32_t rd32(const unsigned char* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

struct CentralEntry { uint32_t lh_ofs; uint16_t method; uint32_t comp_size; uint32_t uncomp_size; std::string name; };

static bool sanitize(const std::string& name, std::string& out){
    out.clear();
    if(name.find(':')!=std::string::npos) return false;
    if(!name.empty() && (name[0]=='/' || name[0]=='\\')) return false;
    std::string tmp=name; std::replace(tmp.begin(), tmp.end(), '\\', '/');
    std::vector<std::string> parts;
    size_t i=0; while(i<=tmp.size()){
        size_t j=tmp.find('/', i);
        std::string seg=(j==std::string::npos)?tmp.substr(i):tmp.substr(i, j-i);
        if(seg==".." ) return false;
        if(seg!="." && !seg.empty()) parts.push_back(seg);
        if(j==std::string::npos) break;
        i = j + 1; // fixed indentation warning
    }
    out.reserve(tmp.size());
    for(size_t k=0;k<parts.size();++k){ if(k) out.push_back('/'); out+=parts[k]; }
    return !out.empty();
}

static bool find_eocd(FILE* f, long& cd_ofs, long& entries){
    if(fseek(f,0,SEEK_END)!=0) return false;
    long size=ftell(f); if(size<22) return false;
    long max_back = std::min<long>(size, 0x10000 + 22);
    for(long back=22; back<=max_back; ++back){
        if(fseek(f, size - back, SEEK_SET)!=0) return false;
        unsigned char buf[22];
        if(fread(buf,1,22,f)!=22) return false;
        if(rd32(buf)==0x06054b50){
            long cd_size = rd32(buf+12);
            long cd_offs = rd32(buf+16);
            entries = rd16(buf+10);
            cd_ofs = cd_offs;
            return cd_offs + cd_size <= size;
        }
    }
    return false;
}

static bool read_central(FILE* f, long cd_ofs, long entries, std::vector<CentralEntry>& out){
    out.clear();
    if(fseek(f, cd_ofs, SEEK_SET)!=0) return false;
    for(long i=0;i<entries;++i){
        unsigned char hdr[46];
        if(fread(hdr,1,46,f)!=46) return false;
        if(rd32(hdr)!=0x02014b50) return false;
        uint16_t fnlen = rd16(hdr+28);
        uint16_t xlen  = rd16(hdr+30);
        uint16_t clen  = rd16(hdr+32);
        CentralEntry ce{};
        ce.method      = rd16(hdr+10);
        ce.comp_size   = rd32(hdr+20);
        ce.uncomp_size = rd32(hdr+24);
        ce.lh_ofs      = rd32(hdr+42);
        if(fnlen>0){
            std::string name; name.resize(fnlen);
            if(fread(&name[0],1,fnlen,f)!=fnlen) return false;
            ce.name = name;
        }
        if(fseek(f, xlen + clen, SEEK_CUR)!=0) return false;
        out.push_back(std::move(ce));
    }
    return true;
}

static bool extract_one(FILE* f, const CentralEntry& ce, const std::string& out_root, std::string& log){
    if(fseek(f, ce.lh_ofs, SEEK_SET)!=0) return false;
    unsigned char lhdr[30];
    if(fread(lhdr,1,30,f)!=30) return false;
    if(rd32(lhdr)!=0x04034b50) return false;
    uint16_t fnlen = rd16(lhdr+26);
    uint16_t xlen  = rd16(lhdr+28);
    if(fseek(f, fnlen + xlen, SEEK_CUR)!=0) return false;
    long data_pos = ftell(f);

    if(!ce.name.empty() && (ce.name.back()=='/' || ce.name.back()=='\\')){
        std::string safe; if(!sanitize(ce.name, safe)) return false;
        return fsx::makedirs(out_root + "/" + safe);
    }

    std::string safe;
    if(!sanitize(ce.name, safe)){ log += "[zip] skip unsafe path: " + ce.name + "\n"; return true; }
    std::string out_path = out_root + "/" + safe;

    if(ce.method == 0){
        auto slash=out_path.find_last_of('/'); if(slash!=std::string::npos){ std::string d=out_path.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)) return false; }
        FILE* out=fopen(out_path.c_str(),"wb"); if(!out) return false;
        const size_t CHUNK=1<<16; std::vector<unsigned char> buf(CHUNK);
        size_t remain=ce.comp_size;
        while(remain>0){
            size_t want=std::min(remain, CHUNK);
            if(fseek(f, data_pos, SEEK_SET)!=0){ fclose(out); return false; }
            size_t got=fread(buf.data(),1,want,f);
            if(got!=want){ fclose(out); return false; }
            data_pos += got; remain -= got;
            if(fwrite(buf.data(),1,got,out)!=got){ fclose(out); return false; }
        }
        fclose(out); return true;
    } else if(ce.method == 8){
        auto slash=out_path.find_last_of('/'); if(slash!=std::string::npos){ std::string d=out_path.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)) return false; }
        FILE* out=fopen(out_path.c_str(),"wb"); if(!out) return false;
        const size_t CHUNK = 1<<15;
        std::vector<unsigned char> inbuf(CHUNK), outbuf(CHUNK);
        z_stream strm{};
        if(inflateInit2(&strm, -MAX_WBITS)!=Z_OK){ fclose(out); return false; }
        size_t remain = ce.comp_size; long pos=data_pos; bool ok=true;
        while(remain>0 && ok){
            size_t want=std::min(remain, CHUNK);
            if(fseek(f, pos, SEEK_SET)!=0){ ok=false; break; }
            size_t got=fread(inbuf.data(),1,want,f);
            if(got!=want){ ok=false; break; }
            pos += got; remain -= got;
            strm.next_in=inbuf.data(); strm.avail_in=(uInt)got;
            while(strm.avail_in>0){
                strm.next_out=outbuf.data(); strm.avail_out=CHUNK;
                int zr=inflate(&strm, Z_NO_FLUSH);
                if(zr!=Z_OK && zr!=Z_STREAM_END){ ok=false; break; }
                size_t have = CHUNK - strm.avail_out;
                if(have && fwrite(outbuf.data(),1,have,out)!=have){ ok=false; break; }
            }
        }
        inflateEnd(&strm); fclose(out); return ok;
    } else {
        log += "[zip] unsupported method for " + ce.name + "\n";
        return true;
    }
}

static bool unzip_to(const std::string& zip_path, const std::string& out_root, std::string& log){
    FILE* f=fopen(zip_path.c_str(),"rb");
    if(!f){ log += "[zip] open fail: " + zip_path + "\n"; return false; }
    long cd_ofs=0, entries=0;
    if(!find_eocd(f, cd_ofs, entries)){ fclose(f); log += "[zip] EOCD not found: " + zip_path + "\n"; return false; }
    std::vector<CentralEntry> cen;
    if(!read_central(f, cd_ofs, entries, cen)){ fclose(f); log += "[zip] central read fail\n"; return false; }
    if(!fsx::isdir(out_root) && !fsx::makedirs(out_root)){ fclose(f); log += "[zip] mkdir fail: " + out_root + "\n"; return false; }
    size_t okc=0, skipc=0;
    for(const auto& ce: cen){ if(extract_one(f, ce, out_root, log)) okc++; else skipc++; }
    fclose(f);
    char msg[128]; snprintf(msg,sizeof(msg),"[zip] extracted ok=%zu skip=%zu -> %s\n", okc, skipc, out_root.c_str()); log += msg;
    return true;
}

static bool write_stamp(const std::string& dir, long size, time_t mtime){
    std::string p=dir+"/.zipstamp";
    auto slash=p.find_last_of('/'); if(slash!=std::string::npos){ std::string d=p.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)) return false; }
    FILE* f=fopen(p.c_str(),"wb"); if(!f) return false;
    fprintf(f, "%ld %lld\n", size, (long long)mtime);
    fclose(f); return true;
}
static bool read_stamp(const std::string& dir, long& size, time_t& mtime){
    std::string p=dir+"/.zipstamp"; FILE* f=fopen(p.c_str(),"rb"); if(!f) return false;
    long long mt=0; long sz=0; if(fscanf(f, "%ld %lld", &sz, &mt)!=2){ fclose(f); return false; } fclose(f);
    size=sz; mtime=(time_t)mt; return true;
}
} // ns zipx

// ---------------- GameBanana API (apiv11) ----------------
namespace gb {
struct ModItem {
    int id=0;
    std::string name;
    std::string profile;
    std::string thumb;
};

static size_t match_bracket(const std::string& s, size_t open_idx){
    int depth=0; bool in_str=false, esc=false;
    for(size_t i=open_idx;i<s.size();++i){
        char c=s[i];
        if(in_str){ if(esc) esc=false; else if(c=='\\') esc=true; else if(c=='"') in_str=false; continue; }
        if(c=='"'){ in_str=true; continue; }
        if(c=='['){ depth++; continue; }
        if(c==']'){ depth--; if(depth==0) return i; continue; }
    }
    return std::string::npos;
}
static bool extract_kv_in_slice(const std::string& s, size_t beg, size_t end, const char* key, std::string& out){
    std::string pat="\""; pat+=key; pat+="\"";
    size_t p=s.find(pat, beg); if(p==std::string::npos || p>=end) return false;
    p=s.find(':', p+pat.size()); if(p==std::string::npos || p>=end) return false;
    while(p<end && (s[p]==':'||s[p]==' '||s[p]=='\t'||s[p]=='\r'||s[p]=='\n')) ++p;
    if(p<end && s[p]=='"'){ size_t a=++p; bool esc=false; for(; p<end; ++p){ char c=s[p]; if(esc){esc=false; continue;} if(c=='\\'){esc=true; continue;} if(c=='"'){ out=s.substr(a,p-a); return true; } } return false; }
    size_t a=p; while(a<end && (s[a]=='-'||(s[a]>='0'&&s[a]<='9'))) ++a; out=s.substr(p,a-p); return !out.empty();
}

static size_t curl_write_str(void* ptr, size_t sz, size_t nm, void* userdata){
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), sz*nm);
    return sz*nm;
}
static size_t curl_write_vec(void* ptr, size_t sz, size_t nm, void* userdata){
    auto* out = static_cast<std::vector<unsigned char>*>(userdata);
    size_t n = sz*nm;
    const unsigned char* p = static_cast<const unsigned char*>(ptr);
    out->insert(out->end(), p, p+n);
    return n;
}

static size_t curl_write_file(void* ptr, size_t sz, size_t nm, void* userdata){
    FILE* f = static_cast<FILE*>(userdata);
    if(!f) return 0;
    return fwrite(ptr, sz, nm, f);
}
// --- image helpers ---
static inline std::string sniff_image_ext(const std::vector<unsigned char>& b){
    if(b.size()>=3 && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) return ".jpg";            // JPEG
    if(b.size()>=8 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') return ".png"; // PNG
    if(b.size()>=12 && !memcmp(b.data(),"RIFF",4) && !memcmp(b.data()+8,"WEBP",4)) return ".webp"; // WEBP
    if(b.size()>=2 && b[0]=='B' && b[1]=='M') return ".bmp";                            // BMP
    return ".img";
}
static std::string json_unescape(const std::string& s){
    std::string out; out.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        char c=s[i];
        if(c!='\\'){ out.push_back(c); continue; }
        if(++i>=s.size()){ out.push_back('\\'); break; }
        char n=s[i];
        switch(n){
            case '\\': out.push_back('\\'); break;
            case '"':  out.push_back('"');  break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                if(i+4<s.size()){
                    unsigned code=0; 
                    for(int k=0;k<4;k++){ char h=s[i+1+k];
                        code<<=4; if(h>='0'&&h<='9') code+=h-'0';
                        else if(h>='a'&&h<='f') code+=10+h-'a';
                        else if(h>='A'&&h<='F') code+=10+h-'A';
                        else { code=0; break; }
                    }
                    i+=4; if(code<128) out.push_back((char)code);
                }
            } break;
            default: out.push_back(n); break;
        }
    }
    return out;
}
// Extract first quoted string after a JSON key token (very lightweight)
static bool extract_first_string_after_key(const std::string& s, const char* key, std::string& out){
    size_t p = s.find(key);
    if(p == std::string::npos) return false;
    p = s.find('"', p + strlen(key)); if(p==std::string::npos) return false;
    size_t a = p + 1; bool esc = false;
    for(size_t i=a; i<s.size(); ++i){
        char c = s[i];
        if(esc){ esc=false; continue; }
        if(c=='\\'){ esc=true; continue; }
        if(c=='"'){ out = s.substr(a, i-a); return true; }
    }
    return false;
}
static inline std::string normalize_url(std::string u){
    u = json_unescape(u);
    // protocol-relative
    if(!u.empty() && u.rfind("//",0)==0) u = "https:" + u;
    // stray backslashes (seen in some GB responses)
    for(char& c: u) if(c=='\\') c = '/';
    // basic sanity
    if(u.rfind("http://",0)!=0 && u.rfind("https://",0)!=0) return {};
    return u;
}
// ADD: GET binary with optional Referer (follows redirects, gzip ok)
// GET binary with optional Referer (follows redirects, JPEG/PNG bytes)
static bool http_get_bytes_ref(const std::string& in_url,
                               const std::string& referer,
                               std::vector<unsigned char>& out,
                               std::string& log){
    std::string url = normalize_url(in_url);

    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }
    out.clear();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.1 (+Switch)");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Important on Switch: keep it simple
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");  // avoid brotli surprises

    if(!referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    // conservative timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 8L);

    CURLcode rc = curl_easy_perform(curl);
    long http=0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if(rc!=CURLE_OK){
        log += std::string("[thumb] curl: ") + curl_easy_strerror(rc) + "\n";
    }
    curl_easy_cleanup(curl);

    char buf[128]; snprintf(buf,sizeof(buf),"[thumb] http=%ld size=%zu url=%s\n", http, out.size(), url.c_str());
    log += buf;

    return (rc==CURLE_OK) && http==200 && !out.empty();
}

static bool http_get(const std::string& url, std::string& body, std::string& log){
    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }

    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // enable gzip/deflate -> auto-decode
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_str);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    // TEMP (if TLS trust fails on Switch, uncomment these two):
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // extra curl logs to nxlink

    log += "[gb] url=" + url + "\n";
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if(rc != CURLE_OK){
        log += std::string("[net] curl: ") + curl_easy_strerror(rc) + "\n";
        return false;
    }
    char buf[96];
    snprintf(buf,sizeof(buf),"[gb] http=%ld bytes=%zu\n", http_code, (size_t)body.size());
    log += buf;

    // dump first bytes for inspection
    if(!body.empty()){
        size_t n = std::min<size_t>(body.size(), 512);
        log.append(body.data(), n);
        log.push_back('\n');
    }
    return http_code==200 && !body.empty();
}
// Resolve a usable thumbnail URL for a single Mod id
static bool fetch_mod_thumb_url(int mod_id, std::string& out_url, std::string& log){
    char url[256];
    snprintf(url, sizeof(url),
        "https://gamebanana.com/apiv11/Mod/%d?_csvProperties=_aPreviewMedia", mod_id);


    std::string body;
    if(!http_get(url, body, log)){
        log += "[thumb] fetch_mod_thumb_url: GET failed\n";
        return false;
    }

    // 1) Prefer direct _sFileUrl anywhere in payload
    std::string u;
    if(extract_first_string_after_key(body, "\"_sFileUrl\"", u) && !u.empty()){
        out_url = json_unescape(u);
        return true;
    }

    // 2) Fallback: find first _sBaseUrl and then _sFile that follows
    std::string base, file;
    if(extract_first_string_after_key(body, "\"_sBaseUrl\"", base)){
        size_t p = body.find("\"_sBaseUrl\"");
        // search _sFile after the base occurrence to stay inside same image object
        if(p != std::string::npos){
            std::string tail = body.substr(p);
            if(extract_first_string_after_key(tail, "\"_sFile\"", file) && !file.empty()){
                base = json_unescape(base);
                file = json_unescape(file);
                if(!base.empty()){
                    if(base.back()!='/' && !file.empty() && file.front()!='/') base.push_back('/');
                    out_url = base + file;
                    return true;
                }
            }
        }
    }

    log += "[thumb] fetch_mod_thumb_url: no preview url in payload\n";
    return false;
}
static bool http_get_bytes(const std::string& url, std::vector<unsigned char>& out, std::string& log){
    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    // Optional TLS relax:
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(curl);
    if(rc != CURLE_OK){
        log += std::string("[net] curl: ") + curl_easy_strerror(rc) + "\n";
        curl_easy_cleanup(curl);
        return false;
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return http_code==200 && !out.empty();
}

// -------- minimal JSON helpers (depth-1 reader) --------
static size_t match_brace(const std::string& s, size_t open_idx){
    int depth = 0; bool in_str=false, esc=false;
    for(size_t i=open_idx; i<s.size(); ++i){
        char c=s[i];
        if(in_str){ if(esc) esc=false; else if(c=='\\') esc=true; else if(c=='"') in_str=false; continue; }
        if(c=='"'){ in_str=true; continue; }
        if(c=='{'){ depth++; continue; }
        if(c=='}'){ depth--; if(depth==0) return i; continue; }
    }
    return std::string::npos;
}
static bool kv_top_at_depth1(const std::string& s, size_t beg, size_t end, const char* key, std::string& out){
    size_t i=beg; int depth=0;
    while(i<end){
        char c=s[i++];
        if(c=='"'){
            std::string k; bool esc=false;
            while(i<end){
                char d=s[i++];
                if(esc){ k.push_back(d); esc=false; continue; }
                if(d=='\\'){ esc=true; continue; }
                if(d=='"') break;
                k.push_back(d);
            }
            if(depth!=1) continue;
            while(i<end && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
            if(i>=end || s[i++]!=':') continue;
            while(i<end && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;

            if(strcmp(k.c_str(), key)==0){
                if(i<end && s[i]=='"'){
                    ++i; std::string v; bool esc2=false;
                    while(i<end){
                        char d=s[i++];
                        if(esc2){ v.push_back(d); esc2=false; continue; }
                        if(d=='\\'){ esc2=true; continue; }
                        if(d=='"') break;
                        v.push_back(d);
                    }
                    out=v; return true;
                }else{
                    size_t j=i; while(j<end && ((s[j]>='0'&&s[j]<='9')||s[j]=='-')) j++;
                    if(j>i){ out=s.substr(i,j-i); return true; }
                }
            }else{
                // skip value for non-matching key
                if(i<end && s[i]=='"'){
                    ++i; bool esc2=false;
                    while(i<end){
                        char d=s[i++];
                        if(esc2){ esc2=false; continue; }
                        if(d=='\\'){ esc2=true; continue; }
                        if(d=='"') break;
                    }
                }else if(i<end && (s[i]=='{'||s[i]=='[')){
                    char open=s[i], close=(open=='{'?'}':']'); int d=1; ++i;
                    bool inS=false, e=false;
                    while(i<end && d>0){
                        char ch=s[i++];
                        if(inS){ if(e) e=false; else if(ch=='\\') e=true; else if(ch=='"') inS=false; continue; }
                        if(ch=='"'){ inS=true; continue; }
                        if(ch==open) d++; else if(ch==close) d--;
                    }
                }else{
                    while(i<end && s[i]!=',' && s[i]!='}') i++;
                }
            }
        }else if(c=='{') depth++;
        else if(c=='}') depth--;
    }
    return false;
}
// ADD: minimal JSON unescape (handles \" \\ \/ \b \f \n \r \t and basic \uXXXX -> ASCII)

// ADD: progress sink
struct DlProg {
    std::atomic<long long> now{0};
    std::atomic<long long> total{0};
    std::atomic<bool>      cancel{false};
};

// ADD: cURL xferinfo callback
static int curl_xfer_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t){
    auto* p = static_cast<DlProg*>(clientp);
    if(!p) return 0;
    p->total.store((long long)dltotal, std::memory_order_relaxed);
    p->now.store((long long)dlnow,     std::memory_order_relaxed);
    return p->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}
static int curl_prog_cb(void* clientp, double dltotal, double dlnow, double, double){
    auto* p = static_cast<DlProg*>(clientp);
    if(!p) return 0;
    p->total.store((long long)dltotal, std::memory_order_relaxed);
    p->now.store((long long)dlnow,     std::memory_order_relaxed);
    return p->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}
static void set_progress_opts(CURL* curl, DlProg* prog){
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef CURLOPT_XFERINFOFUNCTION
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curl_xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,    prog);
#endif
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &curl_prog_cb);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA,     prog);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity"); // no brotli
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.1 (+Switch)");
}
// ADD: GET bytes with optional Referer + progress
static bool http_get_bytes_ref_progress(const std::string& url,
                                        const std::string& referer,
                                        std::vector<unsigned char>& out,
                                        std::string& log,
                                        DlProg* prog){
    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate decode
    if(!referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    if(prog){
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curl_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, prog);
    }
    CURLcode rc = curl_easy_perform(curl);
    long http=0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if(rc!=CURLE_OK){ log += std::string("[gb] curl err: ")+curl_easy_strerror(rc)+"\n"; }
    curl_easy_cleanup(curl);
    char buf[96]; snprintf(buf,sizeof(buf),"[gb] GET bytes http=%ld size=%zu\n", http, out.size()); log += buf;
    return (rc==CURLE_OK) && http==200 && !out.empty();
}
static bool resolve_primary_zip_via_api(int mod_id, std::string& out_url, std::string& out_name, std::string& log) {
    // 1) get file ids from the Mod
    char url1[512];
    snprintf(url1, sizeof(url1),
             "https://api.gamebanana.com/Core/Item/Data?"
             "itemtype=Mod&itemid=%d&fields=Files().aFiles()._idRow", mod_id);

    std::string body1;
    if(!http_get(url1, body1, log)){ log += "[gb] Core/Item/Data(Mod) failed\n"; return false; }

    // very light parse: look for numbers after "_idRow":
    std::vector<int> file_ids;
    {
        size_t p = 0;
        while((p = body1.find("_idRow", p)) != std::string::npos){
            size_t q = body1.find_first_of("0123456789", p+6);
            if(q==std::string::npos) break;
            int id = atoi(body1.c_str()+q);
            if(id>0) file_ids.push_back(id);
            p = q+1;
        }
    }
    if(file_ids.empty()){ log += "[gb] no file IDs in Mod\n"; return false; }

    // 2) query File entries in multicall for the first 8 candidates (keep it small)
    std::string url2 = "https://api.gamebanana.com/Core/Item/Data?";
    int cap = (int)std::min<size_t>(file_ids.size(), 8);
    for(int i=0;i<cap;i++){
        if(i) url2 += '&';
        url2 += "itemtype[]=File&itemid[]=" + std::to_string(file_ids[i]) +
                "&fields[]=_sDownloadUrl,_sFile,_nFilesize";
    }

    std::string body2;
    if(!http_get(url2, body2, log)){ log += "[gb] Core/Item/Data(File) failed\n"; return false; }

    // pick best: prefer _sDownloadUrl present + largest size
    std::string best_url, best_name; long long best_size=-1;
    size_t p = 0;
    while((p = body2.find("{", p)) != std::string::npos){
        size_t q = body2.find("}", p); if(q==std::string::npos) break;
        std::string slice = body2.substr(p, q-p+1);

        auto find_val = [&](const char* key)->std::string{
            size_t k = slice.find(key);
            if(k==std::string::npos) return {};
            k = slice.find('"', k + strlen(key)); if(k==std::string::npos) return {};
            size_t e = slice.find('"', k+1); if(e==std::string::npos) return {};
            return slice.substr(k+1, e-(k+1));
        };
        std::string f_url  = find_val("_sDownloadUrl");
        std::string f_name = find_val("_sFile");
        long long   f_sz   = -1;

        // find numeric _nFilesize (unquoted)
        size_t ksz = slice.find("_nFilesize");
        if(ksz!=std::string::npos){
            size_t n = slice.find_first_of("0123456789", ksz);
            if(n!=std::string::npos) f_sz = atoll(slice.c_str()+n);
        }

        if(!f_url.empty()){
            if(f_sz > best_size){ best_size=f_sz; best_url=f_url; best_name=f_name; }
        }
        p = q + 1;
    }

    if(best_url.empty()){ log += "[gb] no _sDownloadUrl in File list\n"; return false; }
    if(best_name.empty()) best_name = "mod_" + std::to_string(mod_id) + ".zip";
    out_url = best_url; out_name = best_name;
    return true;
}
// Robust pager: iterate objects inside _aRecords array
static std::vector<ModItem> fetch_mods_index_page(std::string& log, int limit=10, int page=1){
    std::vector<ModItem> out;
    std::unordered_set<int> seen;

    char url[512];
    snprintf(url,sizeof(url),
        "https://gamebanana.com/apiv11/Mod/Index"
        "?_aFilters%%5BGeneric_Game%%5D=23582&_nPerpage=%d&_nPage=%d&_sSort=Generic_Newest",
        limit, page);
    log += std::string("[gb] index url=") + url + "\n";

    std::string body;
    if(!http_get(url, body, log)){ log += "[gb] index fetch failed\n"; return out; }
    if(body.size()<2){ log += "[gb] empty/short body\n"; return out; }

    // 1) locate the records array boundaries
    size_t recs = body.find("\"_aRecords\"");
    if(recs == std::string::npos){ log += "[gb] _aRecords not found\n"; return out; }
    size_t arr = body.find('[', recs);
    if(arr == std::string::npos){ log += "[gb] _aRecords '[' missing\n"; return out; }
    size_t arr_end = match_bracket(body, arr);
    if(arr_end == std::string::npos){ log += "[gb] _aRecords not closed\n"; return out; }

    // 2) iterate each object { ... } within the array
    size_t p = arr;
    while(out.size() < (size_t)limit){
        p = body.find('{', p);
        if(p == std::string::npos || p >= arr_end) break;
        size_t q = match_brace(body, p);
        if(q == std::string::npos || q > arr_end) break;

        std::string model,idS,name,profile,thumb;
        extract_kv_in_slice(body, p, q+1, "_sModelName", model);
        if(model != "Mod"){ p = q + 1; continue; }

        extract_kv_in_slice(body, p, q+1, "_idRow", idS);
        extract_kv_in_slice(body, p, q+1, "_sName", name);
        extract_kv_in_slice(body, p, q+1, "_sProfileUrl", profile);
        if(!extract_kv_in_slice(body, p, q+1, "_sThumbnailUrl", thumb)){
            // fallback: first preview image inside this record slice
            size_t imgs = body.find("\"_sFileUrl\"", p);
            if(imgs != std::string::npos && imgs < q){
                size_t t1 = body.find('"', imgs+12);
                t1 = (t1==std::string::npos||t1>q)?std::string::npos:body.find('"', t1+1);
                if(t1!=std::string::npos && t1<q){
                    size_t a = t1+1; bool esc=false;
                    for(size_t i=a;i<q;++i){
                        char c=body[i];
                        if(esc){ esc=false; continue; }
                        if(c=='\\'){ esc=true; continue; }
                        if(c=='"'){ thumb = body.substr(a, i-a); break; }
                    }
                }
            }
        }
        if (!thumb.empty()) thumb = gb::json_unescape(thumb);
        if(!idS.empty() && !name.empty() && !profile.empty()){
            int id = atoi(idS.c_str());
            if(seen.insert(id).second)
                out.push_back(ModItem{ id, name, profile, thumb });
        }
        p = q + 1;
    }

    char buf[96]; snprintf(buf,sizeof(buf),"[gb] fetched %d (limit=%d page=%d)\n",(int)out.size(),limit,page); log+=buf;
    return out;
}


static std::string ext_from_url(const std::string& u){
    auto q=u.find('?'); std::string s=(q==std::string::npos)?u:u.substr(0,q);
    auto d=s.find_last_of('.'); if(d==std::string::npos) return ".zip";
    std::string e=s.substr(d); for(char& c:e) c=tolower(c);
    if(e==".jpeg") e=".jpg";
    if(e==".7z"||e==".rar"||e==".zip"||e==".tar"||e==".gz"||e==".xz"||e==".bz2") return e;
    if(e==".png"||e==".jpg"||e==".webp") return ".zip"; // images as thumbnails, not mods
    return ".zip";
}
static std::string safe_name(std::string s){
    for(char& c: s){ if(!( (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c==' ')) c='_'; }
    while(!s.empty() && (s.back()=='.'||s.back()==' ')) s.pop_back();
    if(s.empty()) s = "mod";
    return s;
}

// Try to find first downloadable file URL from Mod/<id> payload.
// We scan for any of these keys: "_sDownloadUrl", "_sFile", "_sUrl".
static bool find_first_download_url(const std::string& body, std::string& url){
    const char* keys[] = { "\"_sDownloadUrl\"", "\"_sFile\"", "\"_sUrl\"" };
    for(const char* k : keys){
        size_t p = body.find(k);
        if(p==std::string::npos) continue;
        p = body.find('"', p + strlen(k)); if(p==std::string::npos) continue; // first quote after :
        p = body.find('"', p+1); if(p==std::string::npos) continue;          // open value
        size_t a = p+1; bool esc=false;
        for(size_t i=a;i<body.size();++i){
            char c=body[i];
            if(esc){ esc=false; continue; }
            if(c=='\\'){ esc=true; continue; }
            if(c=='"'){ url = body.substr(a, i-a); return true; }
        }
    }
    return false;
}
static bool http_download_to_file(const std::string& url,
                                  const std::string& referer,
                                  const std::string& out_tmp_path,
                                  std::string& log,
                                  DlProg* prog){
    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }

    FILE* f = fopen(out_tmp_path.c_str(), "wb");
    if(!f){ log += "[net] fopen failed for tmp\n"; curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate decode
    if(!referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    // progress
    if (prog) { set_progress_opts(curl, prog); }
    else      { curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L); }

    // sane timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 8L); // bytes/sec
    CURLcode rc = curl_easy_perform(curl);
    long http=0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    fflush(f); fclose(f);

    if(rc!=CURLE_OK || http!=200){
        remove(out_tmp_path.c_str());
        char buf[128]; snprintf(buf,sizeof(buf),"[gb] http_download_to_file rc=%d http=%ld\n",(int)rc,http);
        log += buf;
        return false;
    }
    return true;
}
// REPLACE: choose best archive from _aFiles, get _sDownloadUrl, download with Referer
// streams to <mods_root>/<safe_name>.<ext> via <file>.part
static bool download_primary_zip(const ModItem& item, const std::string& mods_root, std::string& saved_path, std::string& log, DlProg* prog=nullptr){
    // 1) Files list
    std::string j;
    {
        std::string url = "https://gamebanana.com/apiv11/Mod/" + std::to_string(item.id) + "?_csvProperties=_aFiles";
        if(!http_get(url, j, log)){ log += "[gb] files list fetch failed\n"; return false; }
    }

    // 2) Pick best file (archives first, else largest), accept when _bHasContents is missing
    auto ext_rank = [](const std::string& name)->int{
        std::string n=name; for(char& c:n) c=tolower(c);
        if(n.find(".zip")!=std::string::npos) return 4;
        if(n.find(".7z") !=std::string::npos) return 4;
        if(n.find(".rar")!=std::string::npos) return 4;
        if(n.find(".tar.gz")!=std::string::npos || n.find(".tgz")!=std::string::npos) return 3;
        return 1;
    };
    auto get_ext = [](std::string e)->std::string{
        for(char& c:e) c=tolower(c);
        if(e==".jpeg") return ".jpg";
        return e.empty()? ".zip" : e;
    };

    size_t af = j.find("\"_aFiles\"");
    if(af==std::string::npos){ log += "[gb] files list empty\n"; }
    size_t arr = (af==std::string::npos)?std::string::npos:j.find('[', af);
    size_t arr_end = (arr==std::string::npos)?std::string::npos:match_bracket(j, arr);

    int best_id=-1, best_rank=-1; long best_size=-1;
    std::string best_name, best_url;

    if(arr!=std::string::npos && arr_end!=std::string::npos){
        for(size_t p=arr; ; ){
            p = j.find('{', p); if(p==std::string::npos || p>=arr_end) break;
            size_t q = match_brace(j, p); if(q==std::string::npos || q>arr_end) break;

            std::string idS, nameS, sizeS, hasS, urlS;
            extract_kv_in_slice(j, p, q+1, "_idRow",       idS);
            extract_kv_in_slice(j, p, q+1, "_sFile",       nameS);
            extract_kv_in_slice(j, p, q+1, "_nFilesize",   sizeS);
            extract_kv_in_slice(j, p, q+1, "_bHasContents",hasS);     // may be missing
            extract_kv_in_slice(j, p, q+1, "_sDownloadUrl",urlS);     // often present

            int  id = idS.empty()? -1 : atoi(idS.c_str());
            nameS = json_unescape(nameS);
            urlS  = json_unescape(urlS);
            long sz = sizeS.empty()? -1 : strtol(sizeS.c_str(), nullptr, 10);

            // If _bHasContents is missing, treat as true
            bool has = hasS.empty() || hasS=="true" || hasS=="1";

            if(id>0 && has && !nameS.empty()){
                int rk = ext_rank(nameS);
                if(rk>best_rank || (rk==best_rank && sz>best_size)){
                    best_id=id; best_name=nameS; best_size=sz; best_rank=rk;
                    best_url=urlS; // may be empty; we'll fall back if so
                }
            }
            p = q + 1;
        }
    }

    // 3) Prefer direct URL from _aFiles; otherwise use resolver
    std::string dl_url, dl_name;
    if(!best_url.empty()){
        dl_url  = best_url;
        // if _sFile didn’t carry an extension, derive one from URL
        std::string ext = ".zip";
        size_t dot = best_name.find_last_of('.');
        if(dot!=std::string::npos && dot+1<best_name.size()) ext = get_ext(best_name.substr(dot));
        else{
            auto q=dl_url.find('?'); std::string s=(q==std::string::npos)?dl_url:dl_url.substr(0,q);
            auto d=s.find_last_of('.'); if(d!=std::string::npos) ext=get_ext(s.substr(d));
        }
        dl_name = best_name.empty()? ("mod_"+std::to_string(item.id)+ext) : best_name;
    } else {
        // fall back to official Core/Item resolver
        if(!gb::resolve_primary_zip_via_api(item.id, dl_url, dl_name, log)){
            log += "[gb] no valid _aFiles url and resolver failed\n";
            return false;
        }
    }
    log += "[gb] download url: " + dl_url + "\n";


    // 4) Prepare final & temp paths
    fsx::makedirs(mods_root);
    std::string base = safe_name(item.name);
    std::string ext  = ".zip";
    size_t dot = best_name.find_last_of('.');
    if(dot!=std::string::npos && dot+1<best_name.size()) ext = get_ext(best_name.substr(dot));
    std::string final_path = mods_root + "/" + base + ext;
    for(int n=1; fsx::isfile(final_path); ++n) final_path = mods_root + "/" + base + "-" + std::to_string(n) + ext;
    std::string tmp_path = final_path + ".part";
    log += "[gb] download url: " + dl_url + "\n";
    // 5) Stream download to .part file with progress (no big RAM)
    if(!http_download_to_file(dl_url, item.profile, tmp_path, log, prog)){
        remove(tmp_path.c_str());
        return false;
    }

    // 6) finalize
    if(rename(tmp_path.c_str(), final_path.c_str()) != 0){
        remove(tmp_path.c_str());
        log += "[gb] rename failed\n";
        return false;
    }

    saved_path = final_path;
    char buf[160]; snprintf(buf,sizeof(buf),"[gb] saved -> %s (file_id=%d size~%ld)\n", final_path.c_str(), best_id, best_size);
    log += buf;
    return true;
}
} // ns gb

// ---------------- mods scan/apply ----------------
namespace mods {
static const char* kKnownTopDirsArr[]={
    "ai_influence","avalon","field","ik_ai_behavior","ik_chara","ik_demo","ik_effect",
    "ik_event","ik_message","ik_pokemon","light","param_ai","param_chr","script",
    "system_resource","system","ui","world","arc","shaders"
};
static bool is_known_top(const std::string& n){ for(auto* s:kKnownTopDirsArr) if(strcasecmp(n.c_str(),s)==0) return true; return false; }

static bool has_known_child(const std::string& dir){
    DIR* d=opendir(dir.c_str()); if(!d) return false;
    bool ok=false; while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string c=e->d_name; std::string p=dir+"/"+c; struct stat st{};
        if(stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode) && is_known_top(c)){ ok=true; break; }
    } closedir(d); return ok;
}
static bool find_romfs_root_rec(const std::string& start, std::string& out, int depth_limit=6){
    if(depth_limit<0) return false;
    if(fsx::isdir(start+"/romfs")){ out=start+"/romfs"; return true; }
    if(has_known_child(start)){ out=start; return true; }
    DIR* d=opendir(start.c_str()); if(!d) return false;
    bool found=false;
    while(auto* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string c=e->d_name; std::string p=start+"/"+c; struct stat st{};
        if(stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode)){
            if(find_romfs_root_rec(p, out, depth_limit-1)){ found=true; break; }
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

struct ModEntry{
    std::string name;
    std::string path;
    std::string romfs;
    std::string exefs;
    std::vector<std::string> files; // "romfs/<rel>" or "exefs/<rel>"
    bool selected=false;
};

static void scan_mod(ModEntry& m){
    m.files.clear();
    if(!m.romfs.empty()){
        std::vector<std::string> rels; fsx::list_files_rec(m.romfs,"",rels);
        for(auto& r: rels){
            std::string norm=r; std::replace(norm.begin(),norm.end(),'\\','/');
            if(norm=="arc/data.trpfd" || norm=="arc/data.trpfd.csv") continue;
            auto pos=norm.find('/'); std::string top=pos==std::string::npos?norm:norm.substr(0,pos);
            if(!is_known_top(top)) continue;
            m.files.push_back("romfs/"+norm);
        }
    }
    if(!m.exefs.empty()){
        std::vector<std::string> rels; fsx::list_files_rec(m.exefs,"",rels);
        for(auto& r: rels){ std::string norm=r; std::replace(norm.begin(),norm.end(),'\\','/'); m.files.push_back("exefs/"+norm); }
    }
    std::sort(m.files.begin(),m.files.end());
}

static bool ends_with_ci(const std::string& s, const char* suffix){
    size_t n=s.size(), m=strlen(suffix); if(m>n) return false;
    return strcasecmp(s.c_str()+n-m, suffix)==0;
}
static std::string base_name(const std::string& p){
    auto s=p.find_last_of('/'); return (s==std::string::npos)?p:p.substr(s+1);
}
static std::string strip_ext(const std::string& p){
    auto s=p.find_last_of('.'); if(s==std::string::npos) return p; return p.substr(0,s);
}

static std::string ensure_unzipped(const std::string& zip_path, const std::string& cache_root, std::string& log){
    long zsize = fsx::file_size(zip_path);
    time_t zmt  = fsx::file_mtime(zip_path);
    std::string zipname = base_name(zip_path);
    std::string out_dir = cache_root + "/" + strip_ext(zipname);
    long osz=0; time_t omt=0;
    bool up_to_date = fsx::isdir(out_dir) && zipx::read_stamp(out_dir, osz, omt) && (osz==zsize) && (omt==zmt);
    if(up_to_date){ log += "[zip] cache hit: " + out_dir + "\n"; return out_dir; }
    if(fsx::isdir(out_dir)){ if(!fsx::rmtree(out_dir)){ log += "[zip] clean fail: " + out_dir + "\n"; return std::string(); } }
    if(!zipx::unzip_to(zip_path, out_dir, log)) return std::string();
    zipx::write_stamp(out_dir, zsize, zmt);
    return out_dir;
}

static std::vector<ModEntry> scan_root(const std::string& root,std::string& log){
    std::vector<ModEntry> out;
    if(!fsx::isdir(root)){ log+="[scan] mods_root missing: "+root+"\n"; return out; }
    const std::string cache_root = root + "/_unzipped";
    auto ents=fsx::list_dirents(root);
    for(auto& de: ents){
        std::string base = base_name(de.path);
        if(de.is_dir && strcasecmp(base.c_str(), "_unzipped")==0) continue;
        std::string display_name, candidate_path;
        if(de.is_file && ends_with_ci(de.path, ".zip")){
            std::string unz = ensure_unzipped(de.path, cache_root, log);
            if(unz.empty()) continue;
            candidate_path = unz;
            display_name = strip_ext(base);
        } else if(de.is_dir){
            candidate_path = de.path;
            display_name = base;
        } else continue;

        std::string romfs_root, exefs_root;
        find_romfs_root_rec(candidate_path, romfs_root, 8);
        find_first_dir_named(candidate_path, "exefs", exefs_root, 8);

        if(romfs_root.empty() && exefs_root.empty()){
            log += "[scan] skip (no romfs or exefs): " + candidate_path + "\n";
            continue;
        }
        ModEntry m{};
        m.name  = display_name;
        m.path  = de.path;
        m.romfs = romfs_root;
        m.exefs = exefs_root;
        scan_mod(m);
        out.push_back(std::move(m));
    }
    return out;
}

static bool clear_target_known(const std::string& target_romfs, std::string& log){
    size_t removed=0, failed=0;
    for(auto* d: kKnownTopDirsArr){
        std::string path = target_romfs + "/" + d;
        if(fsx::isdir(path)){ if(fsx::rmtree(path)) removed++; else { failed++; log += std::string("[clear] fail: ") + path + "\n"; } }
    }
    std::string content_root = target_romfs;
    if(content_root.size()>=6 && strcasecmp(content_root.c_str()+content_root.size()-6, "/romfs")==0) content_root.resize(content_root.size()-6);
    std::string exefs_dir = content_root + "/exefs";
    if(fsx::isdir(exefs_dir)){ if(fsx::rmtree(exefs_dir)) removed++; else { failed++; log += std::string("[clear] fail: ") + exefs_dir + "\n"; } }
    log += "[clear] removed="+std::to_string(removed)+" failed="+std::to_string(failed)+"\n";
    return failed==0;
}

static std::vector<std::pair<std::string, std::vector<int>>> compute_conflicts(const std::vector<ModEntry>& mods){
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

static bool copy_file(const std::string& src,const std::string& dst,size_t& out_bytes){
    out_bytes=0;
    FILE* in=fopen(src.c_str(),"rb"); if(!in) return false;
    auto slash=dst.find_last_of('/'); if(slash!=std::string::npos){ std::string d=dst.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)){ fclose(in); return false; } }
    FILE* out=fopen(dst.c_str(),"wb"); if(!out){ fclose(in); return false; }
    std::vector<char> buf(1<<16);
    while(true){ size_t n=fread(buf.data(),1,buf.size(),in); if(n==0) break; if(fwrite(buf.data(),1,n,out)!=n){ fclose(in); fclose(out); return false; } out_bytes+=n; }
    fclose(in); fclose(out); return true;
}

static bool apply_copy(const std::vector<ModEntry>& mods,const std::string& target_romfs,std::string& log){
    bool any=false; for(auto& m:mods) any|=m.selected; if(!any){ log+="[apply] no mods selected\n"; return false; }
    if(!fsx::makedirs(target_romfs)){ log+="[apply] cannot create: "+target_romfs+"\n"; return false; }

    std::string content_root = target_romfs;
    if(content_root.size()>=6 && strcasecmp(content_root.c_str()+content_root.size()-6, "/romfs")==0) content_root.resize(content_root.size()-6);
    std::string target_exefs = content_root + "/exefs";

    size_t copied=0, bytes=0, failed=0;
    for(const auto& m:mods) if(m.selected){
        for(const auto& rel:m.files){
            size_t slash = rel.find('/'); if(slash==std::string::npos) continue;
            std::string space = rel.substr(0, slash);
            std::string sub   = rel.substr(slash+1);
            std::string src, dst;
            if(space=="romfs"){ src = (m.romfs.empty()?std::string():m.romfs) + "/" + sub; dst = target_romfs + "/" + sub; }
            else if(space=="exefs"){ src = (m.exefs.empty()?std::string():m.exefs) + "/" + sub; dst = target_exefs + "/" + sub; }
            else continue;
            size_t outb=0;
            if(copy_file(src,dst,outb)){copied++; bytes+=outb;} else {failed++; log+="[copy] fail "+src+" -> "+dst+"\n";}
        }
    }
    log+="[apply] files="+std::to_string(copied)+" bytes="+std::to_string(bytes)+" failed="+std::to_string(failed)+"\n";
    return failed==0;
}
struct ApplyProg {
    std::atomic<size_t> done{0};
    std::atomic<size_t> total{0};
    std::atomic<long long> bytes{0};
    std::atomic<bool> cancel{false};
};

// Same copy, but reports progress and can be cancelled.
// Gathers the full queue first so we know totals.
static bool apply_copy_progress(const std::vector<ModEntry>& mods_in,
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

    // Build flat queue
    struct Job{ std::string src,dst; };
    std::vector<Job> jobs; jobs.reserve(4096);
    for(const auto& m:mods_in) if(m.selected){
        for(const auto& rel: m.files){
            size_t slash = rel.find('/'); if(slash==std::string::npos) continue;
            std::string space = rel.substr(0, slash);
            std::string sub   = rel.substr(slash+1);
            Job j{};
            if(space=="romfs"){ j.src=(m.romfs.empty()?std::string():m.romfs)+"/"+sub; j.dst=target_romfs+"/"+sub; }
            else if(space=="exefs"){ j.src=(m.exefs.empty()?std::string():m.exefs)+"/"+sub; j.dst=target_exefs+"/"+sub; }
            else continue;
            jobs.push_back(std::move(j));
        }
    }
    if(prog) prog->total = jobs.size();

    size_t copied=0, failed=0; long long bytes=0;
    for(size_t i=0;i<jobs.size();++i){
        if(prog && prog->cancel.load()) break;
        size_t outb=0;
        if(copy_file(jobs[i].src, jobs[i].dst, outb)){ copied++; bytes += outb; }
        else { failed++; log += "[copy] fail "+jobs[i].src+" -> "+jobs[i].dst+"\n"; }
        if(prog){ prog->done = i+1; prog->bytes = bytes; }
    }

    char msg[160]; snprintf(msg,sizeof(msg),"[apply] files=%zu bytes=%lld failed=%zu\n", copied, bytes, failed);
    log += msg;
    return failed==0 && !(prog && prog->cancel.load());
}
} // ns mods

// ---------------- SDL UI ----------------
namespace ui {
enum class Screen{ Mods, Target, Apply, Browse, Log, About };

struct App {
    static constexpr int WIN_W=1280, WIN_H=720;
    static constexpr int NAV_W=280;
    static constexpr u64 TRINITY_TID = 0x0100F43008C44000ULL;
    std::string mods_root    = "sdmc:/switch/PLZAMods";
    std::string target_romfs = "sdmc:/atmosphere/contents/0100F43008C44000/romfs";
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
    static constexpr int GB_PER_PAGE = 6;
    static constexpr size_t DL_STACK_SZ = 256 * 1024; // 256 KiB
    gb::ModItem dl_item{};    
    std::string dl_saved;          // final path
    std::string dl_thread_log; 
    Screen screen=Screen::Mods;
    bool running=true;
    bool clear_target_before_apply=false;
    std::string log;

    std::vector<gb::ModItem> gb_items;

    // thumbnail cache (id -> texture)
    struct Tex{ SDL_Texture* tex=nullptr; int w=0; int h=0; };
    std::map<int, Tex> thumb_tex;
    std::unordered_set<int> thumb_tried;

    SDL_Window*   win=nullptr;
    SDL_Renderer* ren=nullptr;
    TTF_Font*     font=nullptr;
    TTF_Font*     font_small=nullptr;

    std::vector<std::string> menu={"Mods","Target","Apply","Browse","Log","About","Exit"};
    static constexpr int firstTab=0, lastTab=5;
    static void s_dl_entry(void* arg){
        static_cast<App*>(arg)->dl_worker();
    }

    void dl_worker(){
        std::string tlog, saved;
        bool ok = gb::download_primary_zip(dl_item, mods_root, saved, tlog, &dl_prog);
        dl_thread_log = std::move(tlog);
        if (ok) { dl_saved = saved; dl_done = true; }
        else    { dl_failed = true; }
        downloading = false;
        dl_thread_alive = false; // signal finished
    }
    static void s_app_entry(void* arg){ static_cast<App*>(arg)->app_worker(); }
    void app_worker(){
        std::string tlog; // optional: append to log directly instead
        bool ok = mods::apply_copy_progress(modlist, target_romfs, log, &app_prog);
        if(ok) app_done = true; else app_failed = true;
        applying = false; app_thread_alive = false;
    }
    bool init(){
        socketInitializeDefault(); nxlinkStdio();
        Result rc=fsdevMountSdmc(); if(R_FAILED(rc)) printf("[warn] fsdevMountSdmc failed: 0x%x\n",rc);
        romfsInit();
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)!=0) return false;
        if(TTF_Init()!=0) return false;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0) return false; // need PNG/JPG
        win = SDL_CreateWindow("TrinityMgr", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
        if (!win) {
            SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
            return false;
        }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if (!ren) {
            SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
            return false;
        }
        const char* candidates[]={"romfs:/ui/DejaVuSans.ttf","romfs:/font.ttf",nullptr};
        for(int i=0;candidates[i];++i){ font=TTF_OpenFont(candidates[i], 28); if(font) break; }
        if(!font) return false;
        font_small = TTF_OpenFontIndex("romfs:/ui/DejaVuSans.ttf", 20, 0);
        if(!font_small) font_small=font;
        rescanMods();
        return true;
    }
    void shutdown(){
        for(auto& kv: thumb_tex){ if(kv.second.tex) SDL_DestroyTexture(kv.second.tex); }
        if(font_small && font_small!=font) TTF_CloseFont(font_small);
        if(font) TTF_CloseFont(font);
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
        if (applying.load()) app_prog.cancel = true;
        if (app_thread_alive){
            threadWaitForExit(&app_thread);
            threadClose(&app_thread);
            app_thread_alive=false;
        }else if (app_thread.handle){
            threadClose(&app_thread);
        }
        if(app_stack){ free(app_stack); app_stack=nullptr; }
        if(ren) SDL_DestroyRenderer(ren);
        if(win) SDL_DestroyWindow(win);
        IMG_Quit();
        TTF_Quit(); SDL_Quit();
        romfsExit();
        fsdevUnmountAll();
        socketExit();
    }

    void rescanMods(){
        log+="[scan] root="+mods_root+"\n";
        modlist=mods::scan_root(mods_root,log);
        for(auto& m:modlist) m.selected=true;
        mod_cursor=0; mod_scroll=0;
        log+="[scan] found="+std::to_string(modlist.size())+"\n";
    }

    SDL_Color C(uint8_t r,uint8_t g,uint8_t b,uint8_t a=255){ return SDL_Color{r,g,b,a}; }
    void fill(SDL_Color c, const SDL_Rect& r){ SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,c.a); SDL_RenderFillRect(ren,&r); }
    void text(const std::string& s, int x,int y, SDL_Color c, bool small=false){
        TTF_Font* f = small?font_small:font;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if(!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
        SDL_Rect dst{ x, y, surf->w, surf->h };
        SDL_FreeSurface(surf);
        if(tex){ SDL_RenderCopy(ren, tex, nullptr, &dst); SDL_DestroyTexture(tex); }
    }
    int textw(const std::string& s, bool small=false){ int w,h; TTF_SizeUTF8(small?font_small:font, s.c_str(), &w,&h); return w; }

    // ------ thumbnails ------
    static std::string lower_ext_from_url(const std::string& u){
        auto q=u.find('?'); std::string s=(q==std::string::npos)?u:u.substr(0,q);
        auto d=s.find_last_of('.'); if(d==std::string::npos) return ".img";
        std::string ext=s.substr(d); for(char& c:ext) c=tolower(c);
        if(ext==".jpeg") ext=".jpg";
        if(ext==".webp"||ext==".png"||ext==".jpg"||ext==".gif") return ext;
        return ".img";
    }
    // REPLACE the whole function with this
    bool ensureThumbTexture(int id, const std::string& url_in){
        if(thumb_tex.count(id)) return true;
        if(thumb_tried.count(id)) return false;
        thumb_tried.insert(id);

        const std::string dir = mods_root + "/_thumbs";
        fsx::makedirs(dir);
        const std::string base = dir + "/" + std::to_string(id);

        // Try cache first (any of these)
        const char* exts[] = {".jpg",".jpeg",".png",".webp",".bmp",".gif",".img"};
        std::string path;
        for(const char* e: exts){ std::string cand=base+e; if(fsx::isfile(cand)){ path=cand; break; } }

        std::vector<unsigned char> bytes;

        if(!path.empty()){
            if(FILE* f=fopen(path.c_str(),"rb")){
                fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                if(n>0){ bytes.resize(n); fread(bytes.data(),1,n,f); }
                fclose(f);
            }
            if(bytes.empty()){ unlink(path.c_str()); path.clear(); }
        }

        // No cache: resolve a valid URL and download
        if(bytes.empty()){
            std::string u = gb::normalize_url(url_in);
            if(u.empty()){
                std::string resolved;
                if(!gb::fetch_mod_thumb_url(id, resolved, log)){
                    log += "[thumb] no url resolved for id=" + std::to_string(id) + "\n";
                    return false;
                }
                u = gb::normalize_url(resolved);
            }
            if(u.empty()){
                log += "[thumb] normalize_url produced empty string\n";
                return false;
            }

            if(!gb::http_get_bytes_ref(u, "https://gamebanana.com/games/23582", bytes, log)){
                log += "[thumb] download fail for: " + u + "\n";
                return false;
            }
            // sniff and save with correct extension
            auto sniff_ext = [](const std::vector<unsigned char>& b)->std::string{
                if(b.size()>=3 && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) return ".jpg";
                if(b.size()>=8 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') return ".png";
                if(b.size()>=12 && !memcmp(b.data(),"RIFF",4) && !memcmp(b.data()+8,"WEBP",4)) return ".webp";
                if(b.size()>=2 && b[0]=='B' && b[1]=='M') return ".bmp";
                return ".jpg";
            };
            path = base + sniff_ext(bytes);
            if(FILE* f=fopen(path.c_str(),"wb")){ fwrite(bytes.data(),1,bytes.size(),f); fclose(f); }
        }

        SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
        if(!rw){ log += "[thumb] RWFromMem fail\n"; return false; }
        SDL_Surface* surf = IMG_Load_RW(rw, 1);
        if(!surf){ log += "[thumb] IMG_Load_RW fail\n"; return false; }

        // scale to 96x64
        const int dstW=96, dstH=64;
        SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, dstW, dstH, 32, SDL_PIXELFORMAT_RGBA32);
        if(scaled){
            SDL_Rect dst{0,0,dstW,dstH};
            SDL_BlitScaled(surf, nullptr, scaled, &dst);
            SDL_FreeSurface(surf); surf = scaled;
        }

        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
        if(!tex){ SDL_FreeSurface(surf); log += "[thumb] CreateTexture fail\n"; return false; }
        Tex t{tex, surf->w, surf->h}; SDL_FreeSurface(surf);
        thumb_tex[id] = t;
        return true;
    }


    void drawMenuBar(){
        SDL_Rect nav{0,0,NAV_W,WIN_H}; fill(C(18,26,36),nav);
        int y=40;
        text("TrinityMgr", 24, y, C(200,220,240)); y+=40;
        for(int i=0;i<(int)menu.size();++i){
            SDL_Color col = (i==menu_cursor)?C(80,180,255):C(180,190,200);
            SDL_Rect hi{0,y-6,NAV_W,36}; if(i==menu_cursor) fill(C(30,45,60),hi);
            text(menu[i], 24, y, col); y+=42;
        }
        text("L/R tabs • ZL/ZR page • Touch nav", 16, WIN_H-28, C(140,150,160), true);
    }

    void drawMods(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        text("Mods", NAV_W+20, 24, C(220,230,240));
        text("[A] toggle  [X] toggle all  [Y] rescan  [ZL/ZR] page  [Touch] toggle", NAV_W+20, 60, C(150,160,170), true);
        int item_h=32, top=100, max_vis=(WIN_H-top-20)/item_h;
        if(mod_cursor<mod_scroll) mod_scroll=mod_cursor;
        if(mod_cursor>=mod_scroll+max_vis) mod_scroll=mod_cursor-max_vis+1;
        for(int i=0;i<max_vis;i++){
            int idx = mod_scroll+i; if(idx>=(int)modlist.size()) break;
            auto& m=modlist[idx];
            SDL_Rect row{NAV_W, top+i*item_h, WIN_W-NAV_W, item_h};
            if(idx==mod_cursor) fill(C(24,36,52), row);
            std::string line = std::string(m.selected?"[X] ":"[ ] ")+m.name+" ("+std::to_string(m.files.size())+")";
            text(line, NAV_W+20, top+i*item_h+6, C(220,230,240));
        }
    }

    void drawTarget(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        text("Target", NAV_W+20, 24, C(220,230,240));
        text(target_romfs, NAV_W+20, 70, C(200,210,220));
        text("[A] append /romfs if missing   [Y] mkdir -p", NAV_W+20, 110, C(150,160,170), true);
    }

    void drawApply(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        text("Apply", NAV_W+20, 24, C(220,230,240));
        size_t sel=0, files=0; for(auto& m:modlist) if(m.selected){ sel++; files+=m.files.size(); }
        text("Selected mods: "+std::to_string(sel), NAV_W+20, 70, C(200,210,220));
        text("Total files:   "+std::to_string(files), NAV_W+20, 104, C(200,210,220));
        text("Target:  "+target_romfs, NAV_W+20, 138, C(200,210,220));
        text(std::string("Clear target before copy: ")+(clear_target_before_apply?"ON":"OFF")+"  [X] toggle", NAV_W+20, 172, C(220,230,240));

        auto conflicts = mods::compute_conflicts(modlist);
        text("Conflicts: "+std::to_string(conflicts.size()), NAV_W+20, 220, C(255,180,120));
        int y=256; int shown=0;
        for(auto& c : conflicts){
            if(shown>=12) break;
            std::string line = " - " + c.first;
            if(textw(line,true) > (WIN_W-NAV_W-40)){ line = line.substr(0, 64) + "..."; }
            text(line, NAV_W+20, y, C(200,210,220), true);
            y+=26; shown++;
        }
        if((int)conflicts.size()>shown) text("... +" + std::to_string(conflicts.size()-shown) + " more", NAV_W+20, y, C(160,170,180), true);
        text("[A] Start copy", NAV_W+20, WIN_H-40, C(180,190,200), true);
        text("[Y] Launch game now", NAV_W+20, WIN_H-70, C(180,190,200), true);
    }

    void drawBrowse(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        std::string title = "GameBanana  —  Page " + std::to_string(gb_page+1) + "  (ZL/ZR to change)";
        text(title, NAV_W+20, 24, C(220,230,240));
        text("TopSubs • " + std::to_string(GB_PER_PAGE) + " per page  |  [Y] refresh  [A] download",
            NAV_W+20, 60, C(150,160,170), true);
        int y=100; const int rowH=88; const int thH=64; const int thW=96;
        if(gb_items.empty()){
            text("No items. Press Y.", NAV_W+20, y, C(200,210,220)); return;
        }
        for(int idx=0; idx<(int)gb_items.size(); ++idx){
            const auto& it = gb_items[idx];
            SDL_Rect row{NAV_W, y-8, WIN_W-NAV_W, rowH};
            if(idx==browse_cursor) fill(C(24,36,52), row);

            ensureThumbTexture(it.id, it.thumb);
            auto itex = thumb_tex.find(it.id);
            if(itex!=thumb_tex.end() && itex->second.tex){
                SDL_Rect dst{ NAV_W+20, y+8, thW, thH };
                SDL_RenderCopy(ren, itex->second.tex, nullptr, &dst);
            }else{
                SDL_Rect ph{ NAV_W+20, y+8, thW, thH }; fill(C(30,45,60), ph);
                text("no img", ph.x+16, ph.y+22, C(140,150,160), true);
            }
            text(std::to_string(it.id) + "  " + it.name, NAV_W+20+thW+14, y+8, C(220,230,240));
            text(it.profile, NAV_W+20+thW+14, y+36, C(150,160,170), true);

            y += rowH;
            if(y > WIN_H-40) break;
        }

        // touch paging hint bars
        SDL_Rect topBar{NAV_W, 88, WIN_W-NAV_W, 6}; fill(C(18,26,36), topBar);
        SDL_Rect botBar{NAV_W, WIN_H-40, WIN_W-NAV_W, 6}; fill(C(18,26,36), botBar);
    }

    void drawLog(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        text("Log", NAV_W+20, 24, C(220,230,240));

        // split into lines
        std::vector<std::string> lines;
        lines.reserve(1024);
        {
            std::string cur;
            for(char c: log){
                if(c=='\r') continue;
                if(c=='\n'){ lines.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            if(!cur.empty()) lines.push_back(cur);
        }

        const int topY = 64;
        const int lineH = 20;
        const int max_vis = (WIN_H - topY - 48) / lineH;
        int max_scroll = std::max(0, (int)lines.size() - max_vis);
        if(log_scroll > max_scroll) log_scroll = max_scroll;

        int start = std::max(0, (int)lines.size() - max_vis - log_scroll);
        int y = topY;
        for(int i=0; i<max_vis && (start+i) < (int)lines.size(); ++i){
            text(lines[start+i], NAV_W+20, y, C(190,200,210), true);
            y += lineH;
        }

        // footer
        char info[96];
        snprintf(info, sizeof(info), "[Up/Down] scroll  [ZL/ZR] page  [X] bottom  [MINUS] top  [Y] clear   %d/%d",
                 (int)lines.size() - log_scroll, (int)lines.size());
        text(info, NAV_W+20, WIN_H-28, C(160,170,180), true);
    }

    void drawAbout(){
        SDL_Rect panel{NAV_W,0,WIN_W-NAV_W,WIN_H}; fill(C(12,18,26),panel);
        text("About", NAV_W+20, 24, C(220,230,240));
        text("Switch-side Trinity mod merger. ZIPs supported. Conflicts preview. romfs + exefs. Browse with thumbnails.", NAV_W+20, 70, C(200,210,220), true);
    }
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

    void render(){
            if (!ren) { return; }
SDL_SetRenderDrawColor(ren, 8,12,18,255);
        SDL_RenderClear(ren);
        drawMenuBar();
        switch(screen){
            case Screen::Mods:   drawMods(); break;
            case Screen::Target: drawTarget(); break;
            case Screen::Apply:  drawApply(); break;
            case Screen::Browse: drawBrowse(); break;
            case Screen::Log:    drawLog(); break;
            case Screen::About:  drawAbout(); break;
        }
        if (applying.load()){
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);

            text("Applying mods...  [B] cancel", WIN_W/2 - 220, WIN_H/2 - 80, C(230,230,240));
            size_t done = app_prog.done.load(), total = app_prog.total.load();
            long long bytes = app_prog.bytes.load();

            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(C(40,60,80), bar);
            int pw = (total>0) ? (int)((done * w) / total) : 0;
            if(pw<0) pw=0; if(pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(120,200,120), prog);

            char buf[128];
            double pct = (total>0)? (100.0 * (double)done / (double)total) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%zu / %zu)  %lld KB", pct, done, total, bytes/1024);
            text(buf, bar.x, bar.y + 34, C(200,210,220), true);

            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        }
        if (downloading.load()){
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_Rect dim{0,0,WIN_W,WIN_H}; fill(C(0,0,0,160), dim);

            text("Downloading...  [B] cancel", WIN_W/2 - 220, WIN_H/2 - 80, C(230,230,240));
            long long now = dl_prog.now.load();
            long long tot = dl_prog.total.load();
            int w=600, h=26; SDL_Rect bar{ (WIN_W-w)/2, WIN_H/2 - 20, w, h };
            fill(C(40,60,80), bar);
            int pw = (tot>0) ? (int)((now * w) / tot) : 0;
            if (pw<0) pw=0; if (pw>w) pw=w;
            SDL_Rect prog{ bar.x, bar.y, pw, h }; fill(C(80,180,255), prog);

            char buf[128]; double pct = (tot>0)? (100.0 * (double)now / (double)tot) : 0.0;
            snprintf(buf,sizeof(buf),"%.1f%%  (%lld / %lld KB)", pct, now/1024, tot/1024);
            text(buf, bar.x, bar.y + 34, C(200,210,220), true);

            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        }
        SDL_RenderPresent(ren);
    }

    void gotoMenuIndex(int idx){
        idx = std::max(0, std::min(idx, (int)menu.size()-1));
        menu_cursor = idx;
        if(menu_cursor == (int)menu.size()-1){ running=false; return; } // Exit
        switch(menu_cursor){
            case 0: screen=Screen::Mods; break;
            case 1: screen=Screen::Target; break;
            case 2: screen=Screen::Apply; break;
            case 3: {
                screen = Screen::Browse;
                if (gb_items.empty()) {
                    gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1); // page N => skip N*10
                    browse_cursor = 0;
                }
                break;
            }
            case 4: screen=Screen::Log; break;
            case 5: screen=Screen::About; break;
            default: screen=Screen::Mods; break;
        }
    }
    void moveTab(int delta){
        int idx = menu_cursor;
        if(idx > 5) idx = 5;
        idx += delta;
        if(idx < 0) idx = 5;
        if(idx > 5) idx = 0;
        gotoMenuIndex(idx);
    }

    void handlePad(u64 k){
        if(k&HidNpadButton_Plus){ running=false; return; }
        if(k&HidNpadButton_L) moveTab(-1);
        if(k&HidNpadButton_R) moveTab(+1);
        if(k&HidNpadButton_Left)  moveTab(-1);
        if(k&HidNpadButton_Right) moveTab(+1);

        if(screen==Screen::Mods){
            int item_h=32, top=100, max_vis=(WIN_H-top-20)/item_h;
            int page = std::max(1, max_vis - 1);
            if(k&HidNpadButton_ZL) mod_cursor=std::max(0,mod_cursor-page);
            if(k&HidNpadButton_ZR) mod_cursor=std::min((int)modlist.size()-1,mod_cursor+page);
            if(k&HidNpadButton_Up)   mod_cursor=std::max(0,mod_cursor-1);
            if(k&HidNpadButton_Down) mod_cursor=std::min((int)modlist.size()-1,mod_cursor+1);
            if((k&HidNpadButton_A) && !modlist.empty()) modlist[mod_cursor].selected=!modlist[mod_cursor].selected;
            if(k&HidNpadButton_X){ bool any=false; for(auto& m:modlist) any|=m.selected; for(auto& m:modlist) m.selected=!any; }
            if(k&HidNpadButton_Y) rescanMods();
        } else if(screen==Screen::Target){
            if(k&HidNpadButton_A){ if(target_romfs.size()<6 || strcasecmp(target_romfs.c_str()+target_romfs.size()-6,"/romfs")!=0) target_romfs+="/romfs"; }
            if(k&HidNpadButton_Y){ if(fsx::makedirs(target_romfs)) log+="[mkdir] ok "+target_romfs+"\n"; else log+="[mkdir] fail "+target_romfs+"\n"; }
        } else if(screen==Screen::Apply){
            if(k&HidNpadButton_X) clear_target_before_apply=!clear_target_before_apply;
            if(k & HidNpadButton_A){
                log += "[apply] target="+target_romfs+"\n";
                if(!fsx::makedirs(target_romfs)){ log+="[apply] cannot create target\n"; gotoMenuIndex(4); return; }
                if(clear_target_before_apply) mods::clear_target_known(target_romfs, log);

                applying = true; app_done = false; app_failed = false;
                app_prog.done = 0; app_prog.total = 0; app_prog.bytes = 0; app_prog.cancel = false;

                app_stack = memalign(0x1000, APP_STACK_SZ);
                if(!app_stack){ applying=false; log += "[thread] apply memalign failed\n"; gotoMenuIndex(4); return; }
                Result rc = threadCreate(&app_thread, s_app_entry, this, app_stack, APP_STACK_SZ, 0x2C, -2);
                if(R_FAILED(rc)){
                    applying=false;
                    char buf[96]; snprintf(buf,sizeof(buf),"[thread] apply create failed rc=0x%08X\n", rc); log+=buf;
                    free(app_stack); app_stack=nullptr; gotoMenuIndex(4); return;
                }
                app_thread_alive=true;
                threadStart(&app_thread);
                gotoMenuIndex(4); // send to Log during apply
            }
            if (k & HidNpadButton_Y) {
                log += "[launch] requesting start...\n";
                // optional: ensure renderer flushes to show the log line
                SDL_RenderPresent(ren);
                launch_title_id(TRINITY_TID, log);
                // Usually system will switch away; if it returns, show Log
                gotoMenuIndex(4);
            }
            if((k & HidNpadButton_B) && applying.load()){ app_prog.cancel = true; }
        } else if(screen==Screen::Browse){
            if(k&HidNpadButton_Y){
                gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1);
                browse_cursor = 0;
            }
            if(k&HidNpadButton_ZL){
                if(gb_page>0){ gb_page--; gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1); browse_cursor=0; }
            }
            if(k&HidNpadButton_ZR){
                gb_page++; gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1); browse_cursor=0;
            }
            if(k&HidNpadButton_Up){
                if(browse_cursor>0) browse_cursor--;
            }
            if(k&HidNpadButton_Down){
                if(browse_cursor+1 < (int)gb_items.size()) browse_cursor++;
            }
            if ((k & HidNpadButton_A) && !gb_items.empty() && !downloading.load()){
                dl_item = gb_items[browse_cursor];
                downloading = true;
                dl_done = false; dl_failed = false;
                dl_saved.clear(); dl_thread_log.clear();
                dl_prog.now = 0; dl_prog.total = 0; dl_prog.cancel = false;

                // allocate aligned stack (libnx API: threadCreate(..., void* stack, size, prio, affinity))
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
            if ((k & HidNpadButton_B) && downloading.load()){
                dl_prog.cancel = 1; // worker sees this and aborts
            }
        } else if(screen==Screen::Log){
            // count lines approximately (for scrolling math here)
            int total=0; for(char c: log) if(c=='\n') total++; if(!log.empty() && log.back()!='\n') total++;
            const int vis =  (WIN_H - 64 - 48) / 20; // match drawLog()
            int max_scroll = std::max(0, total - vis);

            if(k & HidNpadButton_Up)    log_scroll = std::min(max_scroll, log_scroll + 1);
            if(k & HidNpadButton_Down)  log_scroll = std::max(0, log_scroll - 1);
            if(k & HidNpadButton_ZL)    log_scroll = std::min(max_scroll, log_scroll + vis - 1);
            if(k & HidNpadButton_ZR)    log_scroll = std::max(0, log_scroll - (vis - 1));
            if(k & HidNpadButton_X)     log_scroll = 0;               // bottom
            if(k & HidNpadButton_Minus) log_scroll = max_scroll;       // top
            if(k & HidNpadButton_Y)     { log.clear(); log_scroll = 0; }
        }
    }

    void handleTouch(float nx, float ny){
        int x = (int)(nx * WIN_W);
        int y = (int)(ny * WIN_H);

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
                    gb_page--;
                    gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1);
                    browse_cursor = 0;
                }
                return;
            }
            // tap near bottom => next page
            if (y > WIN_H - 48){
                gb_page++;
                gb_items = gb::fetch_mods_index_page(log, GB_PER_PAGE, gb_page + 1);
                browse_cursor = 0;
                return;
            }
            // row selection
            const int startY = 100;
            const int rowH   = 88;
            if (y >= startY){
                int idx = (y - startY) / rowH;
                if (idx >= 0 && idx < (int)gb_items.size()){
                    browse_cursor = idx;
                }
            }
            return;
        }
    }

    // REPLACE your loop() with this (unchanged logic, just here for completeness)
    void loop(){
        PadState pad{}; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
        while (running && appletMainLoop()){
            SDL_Event ev;
            while (SDL_PollEvent(&ev)){
                if (ev.type == SDL_QUIT){ running = false; }
                else if (ev.type == SDL_FINGERUP){ handleTouch(ev.tfinger.x, ev.tfinger.y); }
            }
            padUpdate(&pad); u64 k = padGetButtonsDown(&pad);
            handlePad(k);
            if (dl_done.exchange(false)) {
                if (!dl_thread_log.empty()) { log += dl_thread_log; dl_thread_log.clear(); }
                if (!dl_saved.empty()) {
                    log += "[gb] download complete: " + dl_saved + "\n";
                    rescanMods();          // safe: UI thread
                }
            }
            if (dl_failed.exchange(false)) {
                if (!dl_thread_log.empty()) { log += dl_thread_log; dl_thread_log.clear(); }
                log += "[gb] download failed or canceled\n";
            }
            // apply completion
            if (app_done.exchange(false))  { log += "[apply] complete\n"; }
            if (app_failed.exchange(false)){ log += "[apply] failed or canceled\n"; }
            if (!app_thread_alive && app_thread.handle){
                threadClose(&app_thread);
                memset(&app_thread, 0, sizeof(app_thread));
                if(app_stack){ free(app_stack); app_stack=nullptr; }
            }
            // after handling dl_done/dl_failed:
            if (!dl_thread_alive && dl_thread.handle){
                threadClose(&dl_thread);
                memset(&dl_thread, 0, sizeof(dl_thread));
            }
            render();
        }
    }
};
} // namespace ui

int main(int, char**){
    ui::App app; if(!app.init()) return 0; app.loop(); app.shutdown(); return 0;
}
