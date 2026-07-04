#include "redstonesignal-parallel-optimizer.h"
#include "ll/api/mod/RegisterHelper.h"

#include "ll/api/memory/Hook.h"
#include "mc/world/level/BlockSource.h"
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include "mc/world/redstone/circuit/components/CircuitComponentList.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RedstoneLogicExecutionFlags.h"
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>

#include "nocheck_vector.h"
#include <omp.h>

constexpr uchar producerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsProducer);
constexpr uchar consumerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsConsumer);

nocheck_vector<ChunkCircuitComponentList::Item*>& CollectCircuitSystemItems(CircuitSystem& system, BlockSource* region) {
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

    collectedItems.resize(all_items);
    size_t start = 0;
    for (auto cl : collectedChunkLists) {
        ChunkCircuitComponentList::Item* const data = cl->mComponents->data();
        const size_t                           size = cl->mComponents->size();
        for (size_t i = 0; i < size; ++i) collectedItems[start + i] = data + i;
        start += size;
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
    const int thread_num = std::min(8, omp_get_max_threads());
    const auto&  collectedItems = CollectCircuitSystemItems(*this, region);

#pragma omp parallel num_threads(thread_num)
    {
#pragma omp for
        for (int i = 0; i < collectedItems.size(); ++i) {
            const ChunkCircuitComponentList::Item* item = collectedItems[i];
            if (static_cast<uchar>(item->cachedExecutionFlags) & static_cast<uchar>(RedstoneLogicExecutionFlags::CacheValues)) {
                BaseCircuitComponent* const comp = item->mComponent;
                comp->cacheValues(*this, item->mPos);
            }
        }
#pragma omp for
        for (int i = 0; i < collectedItems.size(); ++i) {
            const ChunkCircuitComponentList::Item* item = collectedItems[i];
            BaseCircuitComponent* const            comp = item->mComponent;
            const CircuitComponentType&            type = comp->getCircuitComponentGroupType();
            if ((type == CircuitComponentType::ProducerComponent || type == CircuitComponentType::CapacitorComponent)
                && (static_cast<uchar>(item->cachedExecutionFlags) & producerFlag) && comp->evaluate(*this, item->mPos))
                comp->mNeedsUpdate = true;
        }
#pragma omp for
        for (int i = 0; i < collectedItems.size(); ++i) {
            const ChunkCircuitComponentList::Item* item = collectedItems[i];
            BaseCircuitComponent* const            comp = item->mComponent;
            const CircuitComponentType&            type = comp->getCircuitComponentGroupType();
            if (type != CircuitComponentType::ProducerComponent && type != CircuitComponentType::CapacitorComponent
                && (static_cast<uchar>(item->cachedExecutionFlags) & consumerFlag) && comp->evaluate(*this, item->mPos))
                comp->mNeedsUpdate = true;
        }
#pragma omp for
        for (int i = 0; i < collectedItems.size(); ++i) {
            const ChunkCircuitComponentList::Item* item = collectedItems[i];
            if (static_cast<uchar>(item->cachedExecutionFlags) & static_cast<uchar>(RedstoneLogicExecutionFlags::CheckLock)) {
                BaseCircuitComponent* const comp = item->mComponent;
                comp->checkLock(*this, item->mPos);
            }
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
    CircuitSystemevaluateHook::hook();
    return true;
}

[[nodiscard]] bool RedstoneOptimizerMod::enable() const {
    getSelf().getLogger().debug("Enabling...");
    return true;
}

[[nodiscard]] bool RedstoneOptimizerMod::disable() const {
    getSelf().getLogger().debug("Disabling...");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::RedstoneOptimizerMod, redstone_optimizer::RedstoneOptimizerMod::getInstance());
