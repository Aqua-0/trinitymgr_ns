#include "gamebanana.hpp"

#include "fs/fs_utils.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cctype>

namespace gb {

bool http_get(const std::string& url, std::string& body, std::string& log);
bool extract_first_string_after_key(const std::string& body, const char* key, std::string& out);

namespace {
constexpr long kCurlBufferSize = 128 * 1024;
constexpr long long kParallelThreshold = 4ll * 1024 * 1024;
constexpr bool kPreferApiv11ForFiles = true;
size_t match_bracket(const std::string& s, size_t open_idx){
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

bool extract_kv_in_slice(const std::string& s, size_t beg, size_t end, const char* key, std::string& out){
    std::string pat="\""; pat+=key; pat+="\"";
    size_t p=s.find(pat, beg); if(p==std::string::npos || p>=end) return false;
    p=s.find(':', p+pat.size()); if(p==std::string::npos || p>=end) return false;
    while(p<end && (s[p]==':'||s[p]==' '||s[p]=='\t'||s[p]=='\r'||s[p]=='\n')) ++p;
    if(p<end && s[p]=='"'){ size_t a=++p; bool esc=false; for(; p<end; ++p){ char c=s[p]; if(esc){esc=false; continue;} if(c=='\\'){esc=true; continue;} if(c=='"'){ out=s.substr(a,p-a); return true; } } return false; }
    size_t a=p; while(a<end && (s[a]=='-'||(s[a]>='0'&&s[a]<='9'))) ++a; out=s.substr(p,a-p); return !out.empty();
}

size_t curl_write_str(void* ptr, size_t sz, size_t nm, void* userdata){
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), sz*nm);
    return sz*nm;
}
size_t curl_write_vec(void* ptr, size_t sz, size_t nm, void* userdata){
    auto* out = static_cast<std::vector<unsigned char>*>(userdata);
    size_t n = sz*nm;
    const unsigned char* p = static_cast<const unsigned char*>(ptr);
    out->insert(out->end(), p, p+n);
    return n;
}
size_t curl_write_file(void* ptr, size_t sz, size_t nm, void* userdata){
    FILE* f = static_cast<FILE*>(userdata);
    if(!f) return 0;
    return fwrite(ptr, sz, nm, f);
}

size_t match_brace(const std::string& s, size_t open_idx){
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

#ifdef CURLOPT_XFERINFOFUNCTION
size_t curl_xfer_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t){
    auto* p = static_cast<DlProg*>(clientp);
    if(!p) return 0;
    p->total.store((long long)dltotal, std::memory_order_relaxed);
    p->now.store((long long)dlnow,     std::memory_order_relaxed);
    return p->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}
#endif
int curl_prog_cb(void* clientp, double dltotal, double dlnow, double, double){
    auto* p = static_cast<DlProg*>(clientp);
    if(!p) return 0;
    p->total.store((long long)dltotal, std::memory_order_relaxed);
    p->now.store((long long)dlnow,     std::memory_order_relaxed);
    return p->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}
void set_progress_opts(CURL* curl, DlProg* prog){
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef CURLOPT_XFERINFOFUNCTION
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curl_xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,    prog);
#endif
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &curl_prog_cb);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA,     prog);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.1 (+Switch)");
}

} // namespace

std::string json_unescape(const std::string& s){
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

static void replace_all(std::string& s, const std::string& from, const std::string& to){
    if(from.empty()) return;
    size_t pos = 0;
    while((pos = s.find(from, pos)) != std::string::npos){
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string strip_html(std::string s){
    replace_all(s, "<br />", "\n");
    replace_all(s, "<br/>", "\n");
    replace_all(s, "<br>", "\n");
    replace_all(s, "</p>", "\n");
    replace_all(s, "<p>", "\n");
    std::string out;
    out.reserve(s.size());
    bool in_tag=false;
    for(size_t i=0;i<s.size();++i){
        char c=s[i];
        if(c=='<'){ in_tag=true; continue; }
        if(c=='>'){ in_tag=false; continue; }
        if(in_tag) continue;
        if(c=='\r') continue;
        if(c=='&'){
            if(s.compare(i,5,"&amp;")==0){ out.push_back('&'); i+=4; continue; }
            if(s.compare(i,4,"&lt;")==0){ out.push_back('<'); i+=3; continue; }
            if(s.compare(i,4,"&gt;")==0){ out.push_back('>'); i+=3; continue; }
            if(s.compare(i,6,"&quot;")==0){ out.push_back('"'); i+=5; continue; }
            if(s.compare(i,6,"&nbsp;")==0){ out.push_back(' '); i+=5; continue; }
        }
        out.push_back(c=='\n' ? '\n' : c);
    }
    return out;
}

std::string normalize_url(std::string u){
    u = json_unescape(u);
    if(!u.empty() && u.rfind("//",0)==0) u = "https:" + u;
    for(char& c: u) if(c=='\\') c = '/';
    if(u.rfind("http://",0)!=0 && u.rfind("https://",0)!=0) return {};
    return u;
}

std::vector<ModItem> fetch_mods_index_page(std::string& log, int limit, int page, int game_id){
    std::vector<ModItem> out;
    std::unordered_set<int> seen;

    char url[512];
    snprintf(url,sizeof(url),
        "https://gamebanana.com/apiv11/Mod/Index"
        "?_aFilters%%5BGeneric_Game%%5D=%d&_nPerpage=%d&_nPage=%d&_sSort=Generic_Newest",
        game_id, limit, page);
    log += std::string("[gb] index url=") + url + "\n";

    std::string body;
    if(!http_get(url, body, log)){ log += "[gb] index fetch failed\n"; return out; }
    if(body.size()<2){ log += "[gb] empty/short body\n"; return out; }

    size_t recs = body.find("\"_aRecords\"");
    if(recs == std::string::npos){ log += "[gb] _aRecords not found\n"; return out; }
    size_t arr = body.find('[', recs);
    if(arr == std::string::npos){ log += "[gb] _aRecords '[' missing\n"; return out; }
    size_t arr_end = match_bracket(body, arr);
    if(arr_end == std::string::npos){ log += "[gb] _aRecords not closed\n"; return out; }

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
        if (!thumb.empty()) thumb = json_unescape(thumb);
        if (!profile.empty()) profile = json_unescape(profile);
        if (!name.empty()) name = json_unescape(name);
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

std::string ext_from_url(const std::string& u){
    auto q=u.find('?'); std::string s=(q==std::string::npos)?u:u.substr(0,q);
    auto d=s.find_last_of('.'); if(d==std::string::npos) return ".zip";
    std::string e=s.substr(d); for(char& c:e) c=tolower(c);
    if(e==".jpeg") e=".jpg";
    if(e==".7z"||e==".rar"||e==".zip"||e==".tar"||e==".gz"||e==".xz"||e==".bz2") return e;
    if(e==".png"||e==".jpg"||e==".webp") return ".zip";
    return ".zip";
}

std::string safe_name(std::string s){
    for(char& c: s){ if(!( (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c==' ')) c='_'; }
    while(!s.empty() && (s.back()=='.'||s.back()==' ')) s.pop_back();
    if(s.empty()) s = "mod";
    return s;
}

bool find_first_download_url(const std::string& body, std::string& url){
    const char* keys[] = { "\"_sDownloadUrl\"", "\"_sFile\"", "\"_sUrl\"" };
    for(const char* k : keys){
        size_t p = body.find(k);
        if(p==std::string::npos) continue;
        p = body.find('"', p + strlen(k)); if(p==std::string::npos) continue;
        p = body.find('"', p+1); if(p==std::string::npos) continue;
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

bool http_get_bytes_ref(const std::string& in_url,
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
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, kCurlBufferSize);

    if(!referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
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

bool fetch_mod_thumb_url(int mod_id, std::string& out_url, std::string& log){
    char url[256];
    snprintf(url, sizeof(url),
        "https://gamebanana.com/apiv11/Mod/%d?_csvProperties=_aPreviewMedia", mod_id);

    std::string body;
    if(!http_get(url, body, log)){
        log += "[thumb] fetch_mod_thumb_url: GET failed\n";
        return false;
    }

    std::string u;
    if(extract_first_string_after_key(body, "\"_sFileUrl\"", u) && !u.empty()){
        out_url = json_unescape(u);
        return true;
    }

    std::string base, file;
    if(extract_first_string_after_key(body, "\"_sBaseUrl\"", base)){
        size_t p = body.find("\"_sBaseUrl\"");
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

bool extract_first_string_after_key(const std::string& body, const char* key, std::string& out){
    if(!key || !*key) return false;
    size_t p = body.find(key);
    if(p==std::string::npos) return false;
    p = body.find(':', p + strlen(key));
    if(p==std::string::npos) return false;
    ++p;
    while(p<body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
    if(p>=body.size() || body[p]!='"') return false;
    ++p;
    std::string val;
    bool esc=false;
    for(; p<body.size(); ++p){
        char c=body[p];
        if(esc){ val.push_back(c); esc=false; continue; }
        if(c=='\\'){ esc=true; continue; }
        if(c=='"'){ out=val; return true; }
        val.push_back(c);
    }
    return false;
}

bool http_get(const std::string& url, std::string& body, std::string& log){
    CURL* curl = curl_easy_init();
    if(!curl){ log += "[net] curl_easy_init failed\n"; return false; }

    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrinityMgr/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, kCurlBufferSize);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_str);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);

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
    return http_code==200 && !body.empty();
}

bool resolve_primary_zip_via_api(int mod_id, std::string& out_url, std::string& out_name, std::string& log, const FileEntry* preferred) {
    auto parse_file_slice = [&](const std::string& slice, std::string& f_url, std::string& f_name, long long& f_sz)->bool{
        auto find_val = [&](const char* key)->std::string{
            size_t k = slice.find(key);
            if(k==std::string::npos) return {};
            k = slice.find('"', k + strlen(key)); if(k==std::string::npos) return {};
            size_t e = slice.find('"', k+1); if(e==std::string::npos) return {};
            return slice.substr(k+1, e-(k+1));
        };
        f_url  = find_val("_sDownloadUrl");
        f_name = find_val("_sFile");
        size_t ksz = slice.find("_nFilesize");
        f_sz = -1;
        if(ksz!=std::string::npos){
            size_t n = slice.find_first_of("0123456789", ksz);
            if(n!=std::string::npos) f_sz = atoll(slice.c_str()+n);
        }
        return !f_url.empty();
    };

    if(preferred && preferred->id>0){
        char url[512];
        snprintf(url, sizeof(url),
                 "https://api.gamebanana.com/Core/Item/Data?itemtype=File&itemid=%d&fields=_sDownloadUrl,_sFile,_nFilesize",
                 preferred->id);
        std::string body;
        if(!http_get(url, body, log)){ log += "[gb] Core/Item/Data(File) single failed\n"; return false; }
        size_t brace = body.find('{');
        size_t brace_end = (brace==std::string::npos)?std::string::npos:match_brace(body, brace);
        if(brace==std::string::npos || brace_end==std::string::npos) return false;
        std::string slice = body.substr(brace, brace_end-brace+1);
        std::string f_url, f_name; long long f_sz=-1;
        if(parse_file_slice(slice, f_url, f_name, f_sz)){
            out_url = json_unescape(f_url);
            out_name = json_unescape(f_name);
            if(out_name.empty()) out_name = preferred->name.empty() ? ("file_"+std::to_string(preferred->id)+".zip") : preferred->name;
            return true;
        }
        return false;
    }

    char url1[512];
    snprintf(url1, sizeof(url1),
             "https://api.gamebanana.com/Core/Item/Data?"
             "itemtype=Mod&itemid=%d&fields=Files().aFiles()._idRow", mod_id);

    std::string body1;
    if(!http_get(url1, body1, log)){ log += "[gb] Core/Item/Data(Mod) failed\n"; return false; }

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

    std::string url2 = "https://api.gamebanana.com/Core/Item/Data?";
    int cap = (int)std::min<size_t>(file_ids.size(), 8);
    for(int i=0;i<cap;i++){
        if(i) url2 += '&';
        url2 += "itemtype[]=File&itemid[]=" + std::to_string(file_ids[i]) +
                "&fields[]=_sDownloadUrl,_sFile,_nFilesize,_idRow";
    }

    std::string body2;
    if(!http_get(url2, body2, log)){ log += "[gb] Core/Item/Data(File) failed\n"; return false; }

    std::string best_url, best_name; long long best_size=-1;
    size_t p = 0;
    while((p = body2.find("{", p)) != std::string::npos){
        size_t q = body2.find("}", p); if(q==std::string::npos) break;
        std::string slice = body2.substr(p, q-p+1);
        std::string f_url, f_name; long long f_sz=-1;
        if(!parse_file_slice(slice, f_url, f_name, f_sz)){ p = q + 1; continue; }
        f_url = json_unescape(f_url);
        f_name = json_unescape(f_name);

        if(preferred && preferred->id>0){
            size_t kid = slice.find("_idRow");
            if(kid!=std::string::npos){
                size_t n = slice.find_first_of("0123456789", kid);
                int rid = (n==std::string::npos)?0:atoi(slice.c_str()+n);
                if(rid == preferred->id){
                    out_url = f_url;
                    out_name = f_name.empty()?preferred->name:f_name;
                    return true;
                }
            }
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

bool http_download_to_file(const std::string& url,
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
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, kCurlBufferSize);
    if(!referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    if (prog) { set_progress_opts(curl, prog); }
    else      { curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L); }

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 8L);
    CURLcode rc = curl_easy_perform(curl);
    long http=0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    double avg_speed=0.0; curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &avg_speed);
    curl_easy_cleanup(curl);
    fflush(f); fclose(f);

    if(rc!=CURLE_OK || http!=200){
        remove(out_tmp_path.c_str());
        char buf[128]; snprintf(buf,sizeof(buf),"[gb] http_download_to_file rc=%d http=%ld\n",(int)rc,http);
        log += buf;
        return false;
    }
    if(avg_speed > 0.0){
        char buf[128];
        snprintf(buf,sizeof(buf),"[gb] avg speed %.1f KB/s\n", avg_speed / 1024.0);
        log += buf;
    }
    return true;
}

static bool select_primary_zip_via_apiv11(int mod_id, std::string& out_url, std::string& out_name, std::string& log, const FileEntry* preferred){
    std::string j;
    std::string url = "https://gamebanana.com/apiv11/Mod/" + std::to_string(mod_id) + "?_csvProperties=_aFiles";
    if(!http_get(url, j, log)){ log += "[gb] files list fetch failed\n"; return false; }

    auto ext_rank = [](const std::string& name)->int{
        std::string n=name; for(char& c:n) c=tolower(c);
        if(n.find(".zip")!=std::string::npos) return 4;
        if(n.find(".7z") !=std::string::npos) return 4;
        if(n.find(".rar")!=std::string::npos) return 4;
        if(n.find(".tar.gz")!=std::string::npos || n.find(".tgz")!=std::string::npos) return 3;
        return 1;
    };

    size_t af = j.find("\"_aFiles\"");
    if(af==std::string::npos){ log += "[gb] files list empty\n"; return false; }
    size_t arr = j.find('[', af);
    size_t arr_end = (arr==std::string::npos)?std::string::npos:match_bracket(j, arr);
    if(arr==std::string::npos || arr_end==std::string::npos) return false;

    int best_rank=-1;
    long best_size=-1;
    std::string best_name, best_url;
    bool want_specific = preferred && preferred->id>0;
    int prefer_id = want_specific ? preferred->id : 0;

    for(size_t p=arr; ; ){
        p = j.find('{', p); if(p==std::string::npos || p>=arr_end) break;
        size_t q = match_brace(j, p); if(q==std::string::npos || q>arr_end) break;

        std::string nameS, sizeS, hasS, urlS, idS;
        extract_kv_in_slice(j, p, q+1, "_sFile",       nameS);
        extract_kv_in_slice(j, p, q+1, "_nFilesize",   sizeS);
        extract_kv_in_slice(j, p, q+1, "_bHasContents",hasS);
        extract_kv_in_slice(j, p, q+1, "_sDownloadUrl",urlS);
        extract_kv_in_slice(j, p, q+1, "_idRow", idS);

        nameS = json_unescape(nameS);
        urlS  = json_unescape(urlS);
        long sz = sizeS.empty()? -1 : strtol(sizeS.c_str(), nullptr, 10);
        bool has = hasS.empty() || hasS=="true" || hasS=="1";

        if(has && !nameS.empty() && !urlS.empty()){
            if(want_specific){
                int rid = idS.empty()?0:atoi(idS.c_str());
                if(rid == prefer_id){
                    out_url = urlS;
                    out_name = nameS.empty()?preferred->name:nameS;
                    return true;
                }
            }
            int rk = ext_rank(nameS);
            if(rk>best_rank || (rk==best_rank && sz>best_size)){
                best_name=nameS; best_size=sz; best_rank=rk;
                best_url=urlS;
            }
        }
        p = q + 1;
    }

    if(best_url.empty()) return false;
    out_url = best_url;
    out_name = best_name;
    return true;
}

static std::string pick_extension_from_name(const std::string& name, const std::string& fallback_url){
    auto lower = [](std::string e)->std::string{
        for(char& c:e) c=tolower(c);
        if(e==".jpeg") return ".jpg";
        return e.empty()? ".zip" : e;
    };
    size_t dot = name.find_last_of('.');
    if(dot!=std::string::npos && dot+1<name.size()) return lower(name.substr(dot));
    auto q=fallback_url.find('?'); std::string s=(q==std::string::npos)?fallback_url:fallback_url.substr(0,q);
    auto d=s.find_last_of('.'); if(d!=std::string::npos) return lower(s.substr(d));
    return ".zip";
}

bool download_primary_zip(const ModItem& item, const std::string& mods_root, std::string& saved_path, std::string& log, DlProg* prog, const FileEntry* preferred_file){
    std::string dl_url, dl_name;
    auto try_apiv11 = [&](void)->bool{
        if(select_primary_zip_via_apiv11(item.id, dl_url, dl_name, log, preferred_file)) return true;
        log += "[gb] apiv11 file lookup failed\n";
        return false;
    };
    auto try_core_api = [&](void)->bool{
        if(resolve_primary_zip_via_api(item.id, dl_url, dl_name, log, preferred_file)) return true;
        log += "[gb] core API file lookup failed\n";
        return false;
    };

    bool resolved=false;
    if(kPreferApiv11ForFiles){
        resolved = try_apiv11();
        if(!resolved){
            log += "[gb] falling back to core API\n";
            resolved = try_core_api();
        }
    }else{
        resolved = try_core_api();
        if(!resolved){
            log += "[gb] falling back to apiv11\n";
            resolved = try_apiv11();
        }
    }
    if(!resolved && preferred_file && !preferred_file->url.empty()){
        dl_url = preferred_file->url;
        dl_name = preferred_file->name.empty()?preferred_file->url:preferred_file->name;
        resolved = true;
    }
    if(!resolved){
        log += "[gb] unable to resolve primary zip\n";
        return false;
    }

    fsx::makedirs(mods_root);
    std::string base = safe_name(item.name);
    std::string ext  = pick_extension_from_name(dl_name, dl_url);
    if(ext.empty()) ext = ".zip";
    std::string final_path = mods_root + "/" + base + ext;
    for(int n=1; fsx::isfile(final_path); ++n) final_path = mods_root + "/" + base + "-" + std::to_string(n) + ext;
    std::string tmp_path = final_path + ".part";
    log += "[gb] download url: " + dl_url + "\n";
    if(!http_download_to_file(dl_url, item.profile, tmp_path, log, prog)){
        remove(tmp_path.c_str());
        return false;
    }

    if(rename(tmp_path.c_str(), final_path.c_str()) != 0){
        remove(tmp_path.c_str());
        log += "[gb] rename failed\n";
        return false;
    }

    saved_path = final_path;
    log += "[gb] saved -> " + final_path + "\n";
    return true;
}

bool fetch_mod_description(int mod_id,
                           std::string& out_desc,
                           std::vector<CreditGroup>* out_credits,
                           std::vector<GalleryImage>* out_images,
                           std::vector<FileEntry>* out_files,
                           std::string& log){
    std::string body;
    std::string url = "https://gamebanana.com/apiv11/Mod/" + std::to_string(mod_id) + "/ProfilePage";
    if(!http_get(url, body, log)){ log += "[gb] description fetch failed\n"; return false; }
    size_t brace = body.find('{');
    size_t brace_end = (brace==std::string::npos)?std::string::npos:match_brace(body, brace);
    if(brace==std::string::npos || brace_end==std::string::npos) return false;

    auto extract_field = [&](const char* key)->std::string{
        std::string val;
        if(extract_kv_in_slice(body, brace, brace_end+1, key, val)){
            val = json_unescape(val);
            val = strip_html(val);
        }
        return val;
    };

    std::string desc = extract_field("_sText");
    if(desc.empty()) desc = extract_field("_sDescription");
    if(desc.empty()){
        log += "[gb] description missing\n";
        return false;
    }
    out_desc = desc;
    if(out_credits){
        out_credits->clear();
        size_t creditsPos = body.find("\"_aCredits\"");
        if(creditsPos != std::string::npos){
            size_t arr = body.find('[', creditsPos);
            size_t arr_end = (arr==std::string::npos)?std::string::npos:match_bracket(body, arr);
            if(arr != std::string::npos && arr_end != std::string::npos){
                size_t p = arr;
                while(true){
                    p = body.find('{', p);
                    if(p == std::string::npos || p >= arr_end) break;
                    size_t q = match_brace(body, p);
                    if(q == std::string::npos || q > arr_end) break;
                    CreditGroup group;
                    std::string title;
                    extract_kv_in_slice(body, p, q+1, "_sGroupName", title);
                    if(!title.empty()) group.title = json_unescape(title);

                    size_t authorsPos = body.find("\"_aAuthors\"", p);
                    if(authorsPos != std::string::npos && authorsPos < q){
                        size_t authArr = body.find('[', authorsPos);
                        size_t authEnd = (authArr==std::string::npos)?std::string::npos:match_bracket(body, authArr);
                        if(authArr != std::string::npos && authEnd != std::string::npos && authEnd <= q){
                            size_t a = authArr;
                            while(true){
                                a = body.find('{', a);
                                if(a == std::string::npos || a >= authEnd) break;
                                size_t b = match_brace(body, a);
                                if(b == std::string::npos || b > authEnd) break;
                                CreditAuthor author;
                                std::string role, name;
                                extract_kv_in_slice(body, a, b+1, "_sRole", role);
                                extract_kv_in_slice(body, a, b+1, "_sName", name);
                                if(!role.empty()) author.role = json_unescape(role);
                                if(!name.empty()) author.name = json_unescape(name);
                                if(!author.role.empty() || !author.name.empty()){
                                    group.authors.push_back(std::move(author));
                                }
                                a = b + 1;
                            }
                        }
                    }
                    if(!group.title.empty() || !group.authors.empty()){
                        out_credits->push_back(std::move(group));
                    }
                    p = q + 1;
                }
            }
        }
    }
    if(out_images){
        out_images->clear();
        size_t pm = body.find("\"_aPreviewMedia\"");
        if(pm != std::string::npos){
            size_t imagesPos = body.find("\"_aImages\"", pm);
            if(imagesPos != std::string::npos){
                size_t arr = body.find('[', imagesPos);
                size_t arr_end = (arr==std::string::npos)?std::string::npos:match_bracket(body, arr);
                if(arr != std::string::npos && arr_end != std::string::npos){
                    int idx = 0;
                    size_t p = arr;
                    while(true){
                        p = body.find('{', p);
                        if(p == std::string::npos || p >= arr_end) break;
                        size_t q = match_brace(body, p);
                        if(q == std::string::npos || q > arr_end) break;
                        std::string base, file;
                        extract_kv_in_slice(body, p, q+1, "_sBaseUrl", base);
                        extract_kv_in_slice(body, p, q+1, "_sFile", file);
                        if(!base.empty() && !file.empty()){
                            base = json_unescape(base);
                            file = json_unescape(file);
                            if(!base.empty() && !file.empty()){
                                if(base.back()=='/') base.pop_back();
                                std::string full = base + "/" + file;
                                GalleryImage gi;
                                gi.url = full;
                                gi.key = mod_id * 1000 + (++idx);
                                out_images->push_back(std::move(gi));
                            }
                        }
                        p = q + 1;
                    }
                }
            }
        }
    }
    if(out_files){
        out_files->clear();
        size_t filesPos = body.find("\"_aFiles\"");
        if(filesPos != std::string::npos){
            size_t arr = body.find('[', filesPos);
            size_t arr_end = (arr==std::string::npos)?std::string::npos:match_bracket(body, arr);
            if(arr != std::string::npos && arr_end != std::string::npos){
                size_t p = arr;
                while(true){
                    p = body.find('{', p);
                    if(p == std::string::npos || p >= arr_end) break;
                    size_t q = match_brace(body, p);
                    if(q == std::string::npos || q > arr_end) break;
                    FileEntry fe{};
                    std::string idS, sizeS, dateS, descS, urlS, nameS, hasS;
                    extract_kv_in_slice(body, p, q+1, "_idRow", idS);
                    extract_kv_in_slice(body, p, q+1, "_sFile", nameS);
                    extract_kv_in_slice(body, p, q+1, "_sDescription", descS);
                    extract_kv_in_slice(body, p, q+1, "_sDownloadUrl", urlS);
                    extract_kv_in_slice(body, p, q+1, "_nFilesize", sizeS);
                    extract_kv_in_slice(body, p, q+1, "_tsDateAdded", dateS);
                    extract_kv_in_slice(body, p, q+1, "_bHasContents", hasS);
                    fe.id = idS.empty() ? 0 : atoi(idS.c_str());
                    fe.name = json_unescape(nameS);
                    fe.description = json_unescape(descS);
                    fe.url = json_unescape(urlS);
                    fe.size_bytes = sizeS.empty() ? 0 : strtoll(sizeS.c_str(), nullptr, 10);
                    fe.timestamp = dateS.empty() ? 0 : strtoll(dateS.c_str(), nullptr, 10);
                    if(!hasS.empty()){
                        std::string lower = hasS;
                        for(char& c : lower) c = (char)tolower(c);
                        fe.has_contents = (lower=="true" || lower=="1");
                    }else{
                        fe.has_contents = true;
                    }
                    if(!fe.url.empty()){
                        out_files->push_back(std::move(fe));
                    }
                    p = q + 1;
                }
            }
        }
    }
    return true;
}
} // namespace gb
