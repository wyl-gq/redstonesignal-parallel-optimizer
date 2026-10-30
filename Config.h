#pragma once
#include <algorithm>
#include <thread>

namespace config {

struct Config {
    int version = 1;
    int thread_num = 4;
    bool skip_redstone_dust = false;
};

inline Config& get() {
    static Config instance;
    return instance;
}

inline int get_thread_num() {
    return std::min(get().thread_num, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
}

} // namespace config
