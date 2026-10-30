#include "redstonesignal-parallel-optimizer.h"
#include "Config.h"

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

#include "mc/world/level/BlockSource.h"
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include "mc/world/redstone/circuit/components/CircuitComponentList.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RedstoneLogicExecutionFlags.h"
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>

#include "nocheck_vector.h"
#include <barrier>
#include <semaphore>
#include <thread>
#include <vector>

constexpr uchar producerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsProducer);
constexpr uchar consumerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsConsumer);

nocheck_vector<ChunkCircuitComponentList::Item*>& CollectCircuitSystemItems(CircuitSystem& system, BlockSource* region, bool skipRedstoneDust) {
    static nocheck_vector<ChunkCircuitComponentList*> collectedChunkLists;
    static nocheck_vector<ChunkCircuitComponentList::Item*> collectedItems;

    collectedChunkLists.clear_reserve(system.mSceneGraph->mActiveComponentsPerChunk.size());
    size_t all_items = 0;
    for (auto& [chunkPos, chunkList] : system.mSceneGraph->mActiveComponentsPerChunk) {
        if (chunkList.mComponents->empty()) continue;

        const bool isShouldEvaluate = region->areChunksFullyLoaded(chunkPos, 32);
        chunkList.bShouldEvaluate   = isShouldEvaluate;
        if (!isShouldEvaluate) continue;

        collectedChunkLists.push_back(&chunkList);
        all_items += chunkList.mComponents->size();
    }

    if (skipRedstoneDust) {
        collectedItems.clear_reserve(all_items);
        for (auto cl : collectedChunkLists) {
            ChunkCircuitComponentList::Item* const data = cl->mComponents->data();
            const size_t                           size = cl->mComponents->size();
            for (size_t i = 0; i < size; ++i) {
                if (data[i].mComponent->getCircuitComponentGroupType() == CircuitComponentType::TransporterComponent) continue;
                collectedItems.push_back(data + i);
            }
        }
    } else {
        collectedItems.resize(all_items);
        size_t start = 0;
        for (auto cl : collectedChunkLists) {
            ChunkCircuitComponentList::Item* const data = cl->mComponents->data();
            const size_t                           size = cl->mComponents->size();
            for (size_t i = 0; i < size; ++i) collectedItems[start + i] = data + i;
            start += size;
        }
    }

    return collectedItems;
}

LL_TYPE_INSTANCE_HOOK(
    CircuitSystemevaluateHook,
    ll::memory::HookPriority::Normal,
    CircuitSystem,
    &CircuitSystem::evaluate,
    void,
    BlockSource* region
) {
    if (!region) return origin(region);
    const auto& collectedItems = CollectCircuitSystemItems(*this, region, config::get().skip_redstone_dust);
    if (collectedItems.empty()) {
        mHasBeenEvaluated = true;
        return;
    }

    using Item = ChunkCircuitComponentList::Item;

    static int                       count = 0;
    static std::barrier<>*           bar   = nullptr;
    static std::counting_semaphore<> wake{0};
    static std::vector<std::thread>  threads;
    static CircuitSystem*            system;
    static const Item* const*        items;
    static int                       size;
    static bool                      stop = false;

    auto work = [](int id) {
        const int begin = id * size / count;
        const int end   = (id + 1) * size / count;

        for (int i = begin; i < end; ++i) {
            const Item* item = items[i];
            if (static_cast<uchar>(item->cachedExecutionFlags) & static_cast<uchar>(RedstoneLogicExecutionFlags::CacheValues))
                item->mComponent->cacheValues(*system, item->mPos);
        }
        bar->arrive_and_wait();

        for (int i = begin; i < end; ++i) {
            const Item*                 item = items[i];
            BaseCircuitComponent* const comp = item->mComponent;
            const CircuitComponentType& type = comp->getCircuitComponentGroupType();
            if ((type == CircuitComponentType::ProducerComponent || type == CircuitComponentType::CapacitorComponent)
                && (static_cast<uchar>(item->cachedExecutionFlags) & producerFlag) && comp->evaluate(*system, item->mPos))
                comp->mNeedsUpdate = true;
        }
        bar->arrive_and_wait();

        for (int i = begin; i < end; ++i) {
            const Item*                 item = items[i];
            BaseCircuitComponent* const comp = item->mComponent;
            const CircuitComponentType& type = comp->getCircuitComponentGroupType();
            if (type != CircuitComponentType::ProducerComponent && type != CircuitComponentType::CapacitorComponent
                && (static_cast<uchar>(item->cachedExecutionFlags) & consumerFlag) && comp->evaluate(*system, item->mPos))
                comp->mNeedsUpdate = true;
        }
        bar->arrive_and_wait();

        for (int i = begin; i < end; ++i) {
            const Item* item = items[i];
            if (static_cast<uchar>(item->cachedExecutionFlags) & static_cast<uchar>(RedstoneLogicExecutionFlags::CheckLock))
                item->mComponent->checkLock(*system, item->mPos);
        }
        bar->arrive_and_wait();
    };

    auto worker = [work](int id) {
        while (true) {
            wake.acquire();
            if (stop) return;
            work(id);
        }
    };

    const int want = std::max(config::get_thread_num(), 1);
    if (want != count) {
        stop = true;
        for (int i = 1; i < count; ++i) wake.release();
        for (auto& thread : threads) thread.join();
        threads.clear();
        count = want;
        stop  = false;
        delete bar;
        bar = new std::barrier<>(count);
        for (int i = 1; i < count; ++i) threads.emplace_back(worker, i);
    }

    system = this;
    items  = collectedItems.data();
    size   = static_cast<int>(collectedItems.size());
    for (int i = 1; i < count; ++i) wake.release();
    work(0);

    mHasBeenEvaluated = true;
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

    // redstonesignal_parallel_optimizer set_thread_num <thread_num>
    ll::command::CommandHandle& cmd_set_thread_num =
        cmdRegistrar.getOrCreateCommand("redstonesignal_parallel_optimizer", "set thread num, be careful to use this command", CommandPermissionLevel::Admin);

    cmd_set_thread_num.runtimeOverload()
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

    cmd_set_thread_num.runtimeOverload()
        .text("set_skip_redstone_dust")
        .required("skip", ll::command::ParamKind::Bool)
        .execute([this](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            config::get().skip_redstone_dust = self["skip"].get<ll::command::ParamKind::Bool>();

            auto configPath = getSelf().getConfigDir() / "config.json";
            ll::config::saveConfig(config::get(), configPath);

            output.success("successful set skip redstone dust to: {}", config::get().skip_redstone_dust);
        });

    return true;
}

[[nodiscard]] bool RedstoneOptimizerMod::disable() const {
    getSelf().getLogger().debug("Disabling...");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::RedstoneOptimizerMod, redstone_optimizer::RedstoneOptimizerMod::getInstance());
