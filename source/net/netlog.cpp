#include "netlog.hpp"

#include "fs/fs_utils.hpp"

#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace netlog {

static constexpr const char* kHostDir  = "sdmc:/switch/trinitymgr_ns";
static constexpr const char* kHostFile = "sdmc:/switch/trinitymgr_ns/nxlink_host.txt";

static bool readCachedHost(in_addr& out){
    FILE* f = fopen(kHostFile, "r");
    if(!f) return false;
    char buf[64]{};
    if(!fgets(buf, sizeof(buf), f)){ fclose(f); return false; }
    fclose(f);
    std::string s(buf);
    while(!s.empty() && (s.back()=='\r' || s.back()=='\n' || isspace((unsigned char)s.back()))) s.pop_back();
    size_t start=0;
    while(start < s.size() && isspace((unsigned char)s[start])) start++;
    if(start>0) s.erase(0,start);
    if(s.empty()) return false;
    in_addr addr{};
    if(inet_pton(AF_INET, s.c_str(), &addr)!=1) return false;
    out = addr;
    return true;
}

static void cacheHost(const in_addr& addr){
    char ipbuf[INET_ADDRSTRLEN]{};
    if(!inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf))) return;
    fsx::makedirs(kHostDir);
    if(FILE* f=fopen(kHostFile,"w")){
        fputs(ipbuf,f);
        fputc('\n',f);
        fclose(f);
    }
}

int connectWithCache(std::string& log){
    int sock = nxlinkStdio();
    if(sock>=0){
        cacheHost(__nxlink_host);
        log += "[net] nxlink logging active\n";
        return sock;
    }
    in_addr cached{};
    if(readCachedHost(cached)){
        __nxlink_host = cached;
        sock = nxlinkStdio();
        if(sock>=0){
            log += "[net] nxlink logging via cached host\n";
            return sock;
        }
        log += "[net] cached nxlink host unreachable\n";
    }else{
        log += "[net] no cached nxlink host; run once via netloader to save\n";
    }
    return -1;
}

void shutdown(int& sock){
    if(sock>=0){
        close(sock);
        sock = -1;
    }
}

} // namespace netlog
