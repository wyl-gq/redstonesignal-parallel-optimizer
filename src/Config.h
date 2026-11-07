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
    int parallel_threshold = 4096;        // normal 与 mchprs 共用：元件数达到才并行
    int normal_phase_threshold = 2048;    // normal：单个相位元件数不足则串行
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
