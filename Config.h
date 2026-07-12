#pragma once
#include <cstdint>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <omp.h>
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/LevelTickEvent.h"
#include "mc/world/actor/player/Player.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"

namespace config {

struct Config {
    int version = 1;
    int thread_num = 4;
};

inline Config& get() {
    static Config instance;
    return instance;
}

inline int get_thread_num() {
    return std::min(get().thread_num, omp_get_max_threads());
}

} // namespace config
