#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace fsx {

bool isdir(const std::string& p);
bool isfile(const std::string& p);
long file_size(const std::string& p);
time_t file_mtime(const std::string& p);
bool makedirs(const std::string& path);
bool rmtree(const std::string& root);

struct DirEnt {
    std::string path;
    bool is_dir;
    bool is_file;
};

std::vector<DirEnt> list_dirents(const std::string& root);
void list_files_rec(const std::string& root,const std::string& rel,std::vector<std::string>& out);

} // namespace fsx
