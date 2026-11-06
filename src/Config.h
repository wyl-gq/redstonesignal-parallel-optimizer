#pragma once
#include <algorithm>
#include <string>
#include <thread>

namespace config {

struct Config {
    int version = 1;
    int thread_num = 4;
    int speeds = 1;
    std::string mode = "normal";
};

inline Config& get() {
    static Config instance;
    return instance;
}

inline bool compiled = false;
inline bool layered  = false;

inline int get_thread_num() {
    return std::min(get().thread_num, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
}

} // namespace config
