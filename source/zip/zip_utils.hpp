#pragma once

#include <string>
#include <ctime>

namespace zipx {

bool unzip_to(const std::string& zip_path, const std::string& out_root, std::string& log);
bool write_stamp(const std::string& dir, long size, time_t mtime);
bool read_stamp(const std::string& dir, long& size, time_t& mtime);

} // namespace zipx
