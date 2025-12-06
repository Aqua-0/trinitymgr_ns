#include "zip_utils.hpp"

#include "fs/fs_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <zlib.h>

namespace zipx {

namespace {
struct CentralEntry {
    uint32_t sig=0;
    uint16_t version_made=0;
    uint16_t version_needed=0;
    uint16_t flags=0;
    uint16_t method=0;
    uint16_t mod_time=0;
    uint16_t mod_date=0;
    uint32_t crc32=0;
    uint32_t comp_size=0;
    uint32_t uncomp_size=0;
    uint16_t name_len=0;
    uint16_t extra_len=0;
    uint16_t comment_len=0;
    uint16_t disk_start=0;
    uint16_t int_attr=0;
    uint32_t ext_attr=0;
    uint32_t local_ofs=0;
    std::string name;
};

static bool read_u16(FILE* f, uint16_t& out){
    uint8_t b[2]; if(fread(b,1,2,f)!=2) return false;
    out = b[0] | (uint16_t(b[1])<<8);
    return true;
}
static bool read_u32(FILE* f, uint32_t& out){
    uint8_t b[4]; if(fread(b,1,4,f)!=4) return false;
    out = b[0] | (uint32_t(b[1])<<8) | (uint32_t(b[2])<<16) | (uint32_t(b[3])<<24);
    return true;
}

static bool read_eocd(FILE* f, long& cd_ofs, uint16_t& entries){
    if(fseek(f,0,SEEK_END)!=0) return false;
    long file_size = ftell(f);
    long search_start = std::max<long>(0, file_size - 65536 - 22);
    for(long pos = file_size - 22; pos >= search_start; --pos){
        if(fseek(f,pos,SEEK_SET)!=0) return false;
        uint32_t sig=0;
        if(!read_u32(f,sig)) continue;
        if(sig==0x06054b50){
            uint16_t disk=0, disk_cd=0, entries_disk=0;
            if(!read_u16(f,disk)) return false;
            if(!read_u16(f,disk_cd)) return false;
            if(!read_u16(f,entries_disk)) return false;
            if(!read_u16(f,entries)) return false;
            uint32_t cd_size=0;
            uint32_t cd_ofs32=0;
            if(!read_u32(f,cd_size)) return false;
            if(!read_u32(f,cd_ofs32)) return false;
            cd_ofs = static_cast<long>(cd_ofs32);
            return true;
        }
    }
    return false;
}

static bool read_central(FILE* f, long cd_ofs, uint16_t entries, std::vector<CentralEntry>& out){
    if(fseek(f, cd_ofs, SEEK_SET)!=0) return false;
    out.clear();
    for(uint16_t i=0;i<entries;i++){
        CentralEntry ce{};
        if(!read_u32(f, ce.sig)) return false;
        if(ce.sig!=0x02014b50) return false;
        if(!read_u16(f, ce.version_made)) return false;
        if(!read_u16(f, ce.version_needed)) return false;
        if(!read_u16(f, ce.flags)) return false;
        if(!read_u16(f, ce.method)) return false;
        if(!read_u16(f, ce.mod_time)) return false;
        if(!read_u16(f, ce.mod_date)) return false;
        if(!read_u32(f, ce.crc32)) return false;
        if(!read_u32(f, ce.comp_size)) return false;
        if(!read_u32(f, ce.uncomp_size)) return false;
        if(!read_u16(f, ce.name_len)) return false;
        if(!read_u16(f, ce.extra_len)) return false;
        if(!read_u16(f, ce.comment_len)) return false;
        if(!read_u16(f, ce.disk_start)) return false;
        if(!read_u16(f, ce.int_attr)) return false;
        if(!read_u32(f, ce.ext_attr)) return false;
        if(!read_u32(f, ce.local_ofs)) return false;

        ce.name.resize(ce.name_len);
        if(fread(ce.name.data(),1,ce.name_len,f)!=ce.name_len) return false;
        if(fseek(f, ce.extra_len + ce.comment_len, SEEK_CUR)!=0) return false;
        out.push_back(std::move(ce));
    }
    return true;
}

static bool find_eocd(FILE* f, long& cd_ofs, long& entries){
    uint16_t ent=0;
    if(!read_eocd(f, cd_ofs, ent)) return false;
    entries = ent;
    return true;
}

static bool sanitize(const std::string& src, std::string& out){
    out.clear();
    out.reserve(src.size());
    for(char c : src){
        if(c=='\\') c='/';
        if(c=='\0') return false;
        out.push_back(c);
    }
    if(out.rfind("..",0)==0) return false;
    if(out.find("..")==0) return false;
    return true;
}

struct LocalHeader {
    uint32_t sig=0;
    uint16_t version=0;
    uint16_t flags=0;
    uint16_t method=0;
    uint16_t mod_time=0;
    uint16_t mod_date=0;
    uint32_t crc32=0;
    uint32_t comp_size=0;
    uint32_t uncomp_size=0;
    uint16_t name_len=0;
    uint16_t extra_len=0;
};

static bool extract_one(FILE* f, const CentralEntry& ce, const std::string& out_root, std::string& log){
    if(fseek(f, ce.local_ofs, SEEK_SET)!=0) return false;
    LocalHeader lh{};
    if(!read_u32(f, lh.sig)) return false;
    if(lh.sig!=0x04034b50) return false;
    if(!read_u16(f, lh.version)) return false;
    if(!read_u16(f, lh.flags)) return false;
    if(!read_u16(f, lh.method)) return false;
    if(!read_u16(f, lh.mod_time)) return false;
    if(!read_u16(f, lh.mod_date)) return false;
    if(!read_u32(f, lh.crc32)) return false;
    if(!read_u32(f, lh.comp_size)) return false;
    if(!read_u32(f, lh.uncomp_size)) return false;
    if(!read_u16(f, lh.name_len)) return false;
    if(!read_u16(f, lh.extra_len)) return false;
    if(fseek(f, lh.name_len + lh.extra_len, SEEK_CUR)!=0) return false;
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
        size_t pos=data_pos;
        while(remain>0){
            size_t want=std::min(remain, CHUNK);
            if(fseek(f, pos, SEEK_SET)!=0){ fclose(out); return false; }
            size_t got=fread(buf.data(),1,want,f);
            if(got!=want){ fclose(out); return false; }
            pos += got; remain -= got;
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
} // namespace

bool unzip_to(const std::string& zip_path, const std::string& out_root, std::string& log){
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

bool write_stamp(const std::string& dir, long size, time_t mtime){
    std::string p=dir+"/.zipstamp";
    auto slash=p.find_last_of('/'); if(slash!=std::string::npos){ std::string d=p.substr(0,slash); if(!fsx::isdir(d) && !fsx::makedirs(d)) return false; }
    FILE* f=fopen(p.c_str(),"wb"); if(!f) return false;
    fprintf(f, "%ld %lld\n", size, (long long)mtime);
    fclose(f); return true;
}

bool read_stamp(const std::string& dir, long& size, time_t& mtime){
    std::string p=dir+"/.zipstamp"; FILE* f=fopen(p.c_str(),"rb"); if(!f) return false;
    long long mt=0; long sz=0; if(fscanf(f, "%ld %lld", &sz, &mt)!=2){ fclose(f); return false; } fclose(f);
    size=sz; mtime=(time_t)mt; return true;
}

} // namespace zipx
