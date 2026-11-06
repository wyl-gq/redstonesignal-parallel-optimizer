#include "normal.h"
#include "Config.h"
#include "SpinBarrier.h"
#include "prof.h"
#include "nocheck_vector.h"

#include "mc/world/level/BlockSource.h"
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RedstoneLogicExecutionFlags.h"
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>

#include <chrono>
#include <semaphore>
#include <thread>
#include <vector>

namespace normal {

constexpr uchar producerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsProducer);
constexpr uchar consumerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsConsumer);

using Item = ChunkCircuitComponentList::Item;

static int                       count = 0;
static SpinBarrier*              bar   = nullptr;
static std::counting_semaphore<> wake{0};
static std::vector<std::thread>  threads;
static CircuitSystem*            sys;
static const Item* const*        data;
static int                       size;
static bool                      stop = false;

void shutdown() {
    if (threads.empty()) return;
    stop = true;
    for (size_t i = 0; i < threads.size(); ++i) wake.release();
    for (auto& thread : threads) thread.join();
    threads.clear();
    delete bar;
    bar   = nullptr;
    count = 0;
    stop  = false;
}

void evaluate(CircuitSystem& system, BlockSource* region) {
    static nocheck_vector<ChunkCircuitComponentList*>       chunkLists;
    static nocheck_vector<ChunkCircuitComponentList::Item*> items;

    chunkLists.clear_reserve(system.mSceneGraph->mActiveComponentsPerChunk.size());
    size_t allItems = 0;
    for (auto& [chunkPos, chunkList] : system.mSceneGraph->mActiveComponentsPerChunk) {
        if (chunkList.mComponents->empty()) continue;

        const bool shouldEvaluate = region->areChunksFullyLoaded(chunkPos, 32);
        chunkList.bShouldEvaluate = shouldEvaluate;
        if (!shouldEvaluate) continue;

        chunkLists.push_back(&chunkList);
        allItems += chunkList.mComponents->size();
    }

    items.resize(allItems);
    size_t start = 0;
    for (auto cl : chunkLists) {
        ChunkCircuitComponentList::Item* const chunkItems = cl->mComponents->data();
        const size_t                           chunkSize  = cl->mComponents->size();
        for (size_t i = 0; i < chunkSize; ++i) items[start + i] = chunkItems + i;
        start += chunkSize;
    }
    prof::total = static_cast<int>(items.size());

    if (items.empty()) return;

    auto work = [](int id) {
        const int begin = id * size / count;
        const int end   = (id + 1) * size / count;

        for (int i = begin; i < end; ++i) {
            const Item* item = data[i];
            if (static_cast<uchar>(item->cachedExecutionFlags) & static_cast<uchar>(RedstoneLogicExecutionFlags::CacheValues))
                item->mComponent->cacheValues(*sys, item->mPos);
        }
        bar->arrive_and_wait();

        for (int i = begin; i < end; ++i) {
            const Item*                 item = data[i];
            BaseCircuitComponent* const comp = item->mComponent;
            const CircuitComponentType& type = comp->getCircuitComponentGroupType();
            if ((type == CircuitComponentType::ProducerComponent || type == CircuitComponentType::CapacitorComponent)
                && (static_cast<uchar>(item->cachedExecutionFlags) & producerFlag) && comp->evaluate(*sys, item->mPos))
                comp->mNeedsUpdate = true;
        }
        bar->arrive_and_wait();

        for (int i = begin; i < end; ++i) {
            const Item*                 item = data[i];
            BaseCircuitComponent* const comp = item->mComponent;
            const CircuitComponentType& type = comp->getCircuitComponentGroupType();
            if (type != CircuitComponentType::ProducerComponent && type != CircuitComponentType::CapacitorComponent
                && (static_cast<uchar>(item->cachedExecutionFlags) & consumerFlag) && comp->evaluate(*sys, item->mPos))
                comp->mNeedsUpdate = true;
            else if (
                static_cast<uchar>(item->cachedExecutionFlags)
                & static_cast<uchar>(RedstoneLogicExecutionFlags::CheckLock)
            )
                item->mComponent->checkLock(*sys, item->mPos);
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
        bar = new SpinBarrier(count);
        for (int i = 1; i < count; ++i) threads.emplace_back(worker, i);
    }

    sys  = &system;
    data = items.data();
    size = static_cast<int>(items.size());

    const auto startTime = std::chrono::steady_clock::now();
    for (int i = 1; i < count; ++i) wake.release();
    work(0);
    if (prof::remaining > 0) {
        prof::work += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
        ++prof::works;
    }
}

} // namespace normal
