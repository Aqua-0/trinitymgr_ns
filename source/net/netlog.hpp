#pragma once

#include <string>

namespace netlog {

int connectWithCache(std::string& log);
void shutdown(int& sock);

} // namespace netlog
