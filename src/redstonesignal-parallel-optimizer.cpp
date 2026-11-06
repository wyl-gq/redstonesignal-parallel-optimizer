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
#include <mc/world/level/block/Block.h>
#if __has_include(<mc/world/level/block/states/vanilla_states/VanillaStates.h>)
#include <mc/world/level/block/states/vanilla_states/VanillaStates.h>
#else
#include <mc/world/level/block/VanillaStates.h>
#endif

#include <algorithm>
#include <chrono>

BlockSource*  gRegion = nullptr;
CircuitSystem* gSystem = nullptr;

LL_TYPE_INSTANCE_HOOK(
    CircuitSystemevaluateHook,
    ll::memory::HookPriority::Normal,
    CircuitSystem,
    &CircuitSystem::evaluate,
    void,
    BlockSource* region
) {
    if (!region) return origin(region);

    gRegion = region;
    gSystem = this;

    const bool active     = prof::remaining > 0;
    const bool mchprsMode = config::get().mode == "mchprs";
    const auto profStart  = std::chrono::steady_clock::now();

    if (mchprsMode) mchprs::evaluate(*this, region);
    else normal::evaluate(*this, region);

    mHasBeenEvaluated = true;

    if (active) {
        prof::evaluate += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - profStart).count();
        if (--prof::remaining == 0) {
            auto& logger = redstone_optimizer::RedstoneOptimizerMod::getInstance().getSelf().getLogger();
            logger.info(
                "- CircuitSystem::evaluate: {:.3f} ms\n - work: {:.3f} ms",
                prof::evaluate / profTimes,
                prof::work / std::max(prof::works, 1)
            );
            if (mchprsMode)
                logger.info(
                    "- components: {} (cache {} / producer {} / consumer {} / lock {})",
                    prof::total,
                    prof::cache,
                    prof::producer,
                    prof::consumer,
                    prof::lock
                );
            else logger.info("- components: {}", prof::total);
            logger.info(
                "- graph: nodes {} / pending {} / withSources {} / max level {} / cycles {}",
                prof::graphNodes,
                prof::graphPending,
                prof::withSources,
                prof::graphLevel,
                prof::graphCycle
            );
            logger.info(
                "- level distribution: L0 {} / L1 {} / L2 {} / L3+ {}",
                prof::level0,
                prof::level1,
                prof::level2,
                prof::level3plus
            );
            logger.info(
                "- item levels: distinct {} / max {} / scheduled items {}",
                prof::itemLevels,
                prof::itemMaxLevel,
                prof::itemScheduled
            );

            prof::evaluate = 0;
            prof::work     = 0;
            prof::works    = 0;
            prof::total    = 0;
            prof::cache    = 0;
            prof::producer = 0;
            prof::consumer = 0;
            prof::lock     = 0;
        }
    }
}

namespace redstone_optimizer {

RedstoneOptimizerMod& RedstoneOptimizerMod::getInstance() {
    static RedstoneOptimizerMod instance;
    return instance;
}

[[nodiscard]] bool RedstoneOptimizerMod::load() const {
    getSelf().getLogger().debug("Loading...");

    try {
        auto configPath = getSelf().getConfigDir() / "config.json";
        bool upToDate = ll::config::loadConfig(config::get(), configPath);
        if (!upToDate) 
            ll::config::saveConfig(config::get(), configPath);

    } catch (const std::exception& e) {
        getSelf().getLogger().error("Failed to load config: {}", e.what());
        return false;
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

    cmd_redstone_optimizer.runtimeOverload()
        .text("set_mode")
        .required("mode", ll::command::ParamKind::String)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const auto& mode = self["mode"].get<ll::command::ParamKind::String>();
            if (mode != "normal" && mode != "mchprs") {
                output.error("mode must be normal or mchprs but got: {}", mode);
                return;
            }

            config::get().mode = mode;
            mchprs::invalidate();

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set mode to: {}", mode);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("set_thread_num")
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
        .text("set_speeds")
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
        .text("set_layered")
        .required("on", ll::command::ParamKind::Bool)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            config::layered = self["on"].get<ll::command::ParamKind::Bool>();
            mchprs::invalidate();
            output.success("layered: {}", config::layered);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("release")
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const&) {
            mchprs::setCompiled(false);
            output.success("released: component classification is rebuilt every evaluate");
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("prof")
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const&) {
            prof::remaining = profTimes;
            prof::evaluate  = 0;
            prof::work      = 0;
            prof::works     = 0;
            output.success("profiling next {} CircuitSystem::evaluate", profTimes);
        });

    cmd_redstone_optimizer.runtimeOverload()
        .text("lever")
        .required("x", ll::command::ParamKind::Int)
        .required("y", ll::command::ParamKind::Int)
        .required("z", ll::command::ParamKind::Int)
        .required("on", ll::command::ParamKind::Bool)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            if (!gRegion) {
                output.error("no block source yet");
                return;
            }

            BlockPos pos{
                self["x"].get<ll::command::ParamKind::Int>(),
                self["y"].get<ll::command::ParamKind::Int>(),
                self["z"].get<ll::command::ParamKind::Int>()
            };
            const bool on = self["on"].get<ll::command::ParamKind::Bool>();

            auto next = gRegion->getBlock(pos).setState(VanillaStates::OpenBit(), on);
            if (!next) {
                output.error("block at {} {} {} has no open_bit", pos.x, pos.y, pos.z);
                return;
            }

#if __has_include(<mc/world/level/block/BlockChangeContext.h>)
            gRegion->setBlockAndRetainCompatibleBlockActor(pos, *next, 3);
#else
            gRegion->setBlock(pos, *next, 3, nullptr, nullptr, nullptr);
#endif

            if (gSystem) {
                for (auto& [chunkPos, chunkList] : gSystem->mSceneGraph->mActiveComponentsPerChunk) {
                    for (auto& item : *chunkList.mComponents) {
                        if (item.mPos->x == pos.x && item.mPos->y == pos.y && item.mPos->z == pos.z) {
                            item.mComponent->setStrength(on ? 15 : 0);
                            item.mComponent->mNeedsUpdate = true;
                        }
                    }
                }
            }

            output.success("lever {} {} {} -> {}", pos.x, pos.y, pos.z, on ? 1 : 0);
        });

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
