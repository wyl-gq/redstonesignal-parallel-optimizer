#include "redstonesignal-parallel-optimizer.h"
#include "Config.h"
#include "mchprs.h"
#include "normal.h"
#include "prof.h"

#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/Config.h"
#include "ll/api/memory/Hook.h"
#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include "ll/api/command/runtime/ParamKind.h"
#include "ll/api/command/runtime/RuntimeCommand.h"
#include "ll/api/command/runtime/RuntimeOverload.h"

#include <mc/server/commands/CommandOrigin.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/block/Block.h>
#if __has_include(<mc/world/level/block/states/vanilla_states/VanillaStates.h>)
#include <mc/world/level/block/states/vanilla_states/VanillaStates.h>
#else
#include <mc/world/level/block/VanillaStates.h>
#endif

#include <algorithm>
#include <chrono>

LL_TYPE_INSTANCE_HOOK(
    CircuitSystemevaluateHook,
    ll::memory::HookPriority::Normal,
    CircuitSystem,
    &CircuitSystem::evaluate,
    void,
    BlockSource* region
) {
    if (!region) return origin(region);

    const auto startTime = std::chrono::steady_clock::now();

    if (config::get().mode == "mchprs") {
        // 除主世界外其它维度不做 evaluate
        if (static_cast<int>(region->getDimensionId()) != 0) {
            mHasBeenEvaluated = true;
            return;
        }
        mchprs::evaluate(*this, region);
    } else if (config::get().mode == "original") {
        origin(region);
    } else {
        normal::evaluate(*this, region);
    }

    prof::onEvaluate(
        *this,
        *region,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count()
    );

    mHasBeenEvaluated = true;
}

namespace redstone_optimizer {

RedstoneOptimizerMod& RedstoneOptimizerMod::getInstance() {
    static RedstoneOptimizerMod instance;
    return instance;
}

[[nodiscard]] bool RedstoneOptimizerMod::load() const {
    getSelf().getLogger().debug("Loading...");

    auto configPath = getSelf().getConfigDir() / "config.json";
    try {
        bool upToDate = ll::config::loadConfig(config::get(), configPath);
        if (!upToDate)
            ll::config::saveConfig(config::get(), configPath);

    } catch (const std::exception& e) {
        // 旧配置缺少新字段时按默认值重写，避免整个模组加载失败
        getSelf().getLogger().warn("Failed to load config: {}, rewriting defaults", e.what());
        ll::config::saveConfig(config::get(), configPath);
    }

    CircuitSystemevaluateHook::hook();
    return true;
}

[[nodiscard]] bool RedstoneOptimizerMod::enable() const {
    getSelf().getLogger().debug("Enabling...");

    auto& cmdRegistrar = ll::Overloaded{
        [](auto& (*func)()) -> auto& { return func(); },
        [](auto& (*func)(bool)) -> auto& { return func(false); }
    }(&ll::command::CommandRegistrar::getInstance);

    // redstonesignal_parallel_optimizer <subcommand>
    ll::command::CommandHandle& cmd_redstone_optimizer = cmdRegistrar.getOrCreateCommand(
        "redstonesignal_parallel_optimizer",
        "redstone signal parallel optimizer",
        CommandPermissionLevel::Admin
    );
    cmd_redstone_optimizer.alias("rpo");

    cmdRegistrar.tryRegisterRuntimeEnum(
        "RedstoneOptimizerMode",
        {
            {"normal",   0},
            {"mchprs",   1},
            {"original", 2}
        }
    );

    cmd_redstone_optimizer.runtimeOverload()
        .text("set_parallel_threshold")
        .required("threshold", ll::command::ParamKind::Int)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const int threshold = self["threshold"].get<ll::command::ParamKind::Int>();
            if (threshold < 0) {
                output.error("threshold must not be negative but got: {}", threshold);
                return;
            }

            config::get().parallel_threshold = threshold;

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set parallel threshold to: {}", threshold);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("set_normal_phase_threshold")
        .required("threshold", ll::command::ParamKind::Int)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const int threshold = self["threshold"].get<ll::command::ParamKind::Int>();
            if (threshold < 0) {
                output.error("threshold must not be negative but got: {}", threshold);
                return;
            }

            config::get().normal_phase_threshold = threshold;

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set normal phase threshold to: {}", threshold);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("mode")
        .required("mode", ll::command::ParamKind::Enum, "RedstoneOptimizerMode")
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const auto& mode = self["mode"].get<ll::command::ParamKind::Enum>();

            config::get().mode = mode.name;
            mchprs::invalidate();

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set mode to: {}", mode.name);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("thread_num")
        .required("thread_num", ll::command::ParamKind::Int)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const int thread_num = self["thread_num"].get<ll::command::ParamKind::Int>();
            if (thread_num <= 0) {
                output.error("thread num must be greater than 0 but got: {}", thread_num);
                return;
            }

            config::get().thread_num = thread_num;

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set thread num to: {}", thread_num);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("speeds")
        .required("speeds", ll::command::ParamKind::Int)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const int speeds = self["speeds"].get<ll::command::ParamKind::Int>();
            if (speeds <= 0) {
                output.error("speeds must be greater than 0 but got: {}", speeds);
                return;
            }

            config::get().speeds = speeds;

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set speeds to: {}", speeds);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("compile")
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const&) {
            mchprs::setCompiled(true);
            output.success("compiled: component classification is locked until release");
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("release")
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const&) {
            mchprs::setCompiled(false);
            output.success("released: component classification is rebuilt every evaluate");
        });

    prof::enable(cmd_redstone_optimizer);

    return true;
}

[[nodiscard]] bool RedstoneOptimizerMod::disable() const {
    getSelf().getLogger().debug("Disabling...");
    normal::shutdown();
    mchprs::shutdown();
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::RedstoneOptimizerMod, redstone_optimizer::RedstoneOptimizerMod::getInstance());
