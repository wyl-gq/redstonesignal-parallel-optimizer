#pragma once
#include "ll/api/mod/NativeMod.h"

namespace redstone_optimizer {

class RedstoneOptimizerMod {

public:
    static RedstoneOptimizerMod& getInstance();

    RedstoneOptimizerMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    [[nodiscard]] bool load() const;

    [[nodiscard]] bool enable() const;

    [[nodiscard]] bool disable() const;

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace redstone_optimizer
