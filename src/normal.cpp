#include "normal.h"
#include "Config.h"
#include "SpinBarrier.h"
#include "ll/api/base/StdInt.h"
#include "mc/world/redstone/circuit/components/RepeaterCapacitor.h"
#include "nocheck_vector.h"
#include "rewrite.h"


#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RedstoneLogicExecutionFlags.h"
#include <cstdint>
#include <float.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>


#include <atomic>
#include <chrono>
#include <cstdio>
#include <emmintrin.h>
#include <semaphore>
#include <thread>
#include <vector>


namespace normal {

constexpr uchar producerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsProducer);
constexpr uchar consumerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsConsumer);
constexpr uchar cacheFlag    = static_cast<uchar>(RedstoneLogicExecutionFlags::CacheValues);
constexpr uchar lockFlag     = static_cast<uchar>(RedstoneLogicExecutionFlags::CheckLock);

using Item = ChunkCircuitComponentList::Item;

static int                       count = 0;
static SpinBarrier*              bar   = nullptr;
static std::counting_semaphore<> wake{0};
static std::vector<std::thread>  threads;
static CircuitSystem*            sys;
static const Item* const*        data;
static int                       size;
static bool                      stop = false;

static std::vector<nocheck_vector<const Item*>> tickProducer; // 需要 evaluate 的 producer 类
static std::vector<nocheck_vector<const Item*>> tickRepeater; // 中继器里 powered 变化、只需落地的那部分
static std::vector<nocheck_vector<const Item*>> tickConsumer; // 需要 evaluate 的 consumer 类
static std::vector<nocheck_vector<const Item*>> tickLock; // 需要 checkLock 的中继器
static std::vector<nocheck_vector<const Item*>> tickDust; // 强度变化、待落地的红石粉
static std::atomic<int>                         totalProducer{0};
static std::atomic<int>                         totalRepeater{0};
static std::atomic<int>                         totalConsumer{0};
static std::atomic<int>                         totalLock{0};
static std::atomic<int>                         totalDust{0};

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

void toggleRepeaterPowered(const Item* item) {
    auto* const repeater   = static_cast<RepeaterCapacitor*>(item->mComponent);
    repeater->mPowered     = !repeater->mPowered;
    repeater->mNeedsUpdate = true;
};

void evaluateComponent(const Item* item) {
    BaseCircuitComponent* const comp = item->mComponent;
    if (comp->evaluate(*sys, item->mPos)) comp->mNeedsUpdate = true;
};

void applyDustStrength(const Item* item) {
    auto* const dustComponent   = static_cast<TransporterComponent*>(item->mComponent);
    dustComponent->mStrength    = dustComponent->mNextStrength;
    dustComponent->mNeedsUpdate = true;
};

void checkRepeaterLock(const Item* item) {
    rewrite::checkLock(*static_cast<RepeaterCapacitor*>(item->mComponent), *sys, item->mPos);
};

// 按 id/parts 把各线程收集到的列表当成一段拼接序列，取自己那一份逐项回调
template <bool singleList, bool allLists, typename Func>
static void forEachItem(int id, int parts, const std::vector<nocheck_vector<const Item*>>& lists, int total, Func&& func) {
    if constexpr (singleList) {
        for (const auto& item : lists[0]) {
            func(item);
        }
        return;
    } else if constexpr (allLists) {
        for (const auto& list : lists) {
            for (const auto& item : list) {
                func(item);
            }
        }
        return;
    } else {
        const int sliceBegin = id * total / parts;
        const int sliceEnd   = (id + 1) * total / parts;
        int       base       = 0;
        for (int t = 0; t < parts && base < sliceEnd; ++t) {
            const nocheck_vector<const Item*>& items = lists[t];
            const int                          count = static_cast<int>(items.size());
            const int                          from  = sliceBegin > base ? sliceBegin : base;
            const int                          to    = sliceEnd < base + count ? sliceEnd : base + count;
            for (int k = from - base; k < to - base; ++k) func(items[k]);
            base += count;
        }
    }
}

template <bool Parallel>
static void workImpl(int id) {
    const int                    workers       = Parallel ? count : 1;
    const int                    begin         = Parallel ? id * size / workers : 0;
    const int                    end           = Parallel ? (id + 1) * size / workers : size;
    const int                    slice         = Parallel ? end - begin : size;
    nocheck_vector<const Item*>& producerItems = tickProducer[Parallel ? id : 0];
    nocheck_vector<const Item*>& repeaterItems = tickRepeater[Parallel ? id : 0];
    nocheck_vector<const Item*>& consumerItems = tickConsumer[Parallel ? id : 0];
    nocheck_vector<const Item*>& lockItems     = tickLock[Parallel ? id : 0];
    nocheck_vector<const Item*>& dustItems     = tickDust[Parallel ? id : 0];
    producerItems.clear_reserve(slice);
    repeaterItems.clear_reserve(slice);
    consumerItems.clear_reserve(slice);
    lockItems.clear_reserve(slice);
    dustItems.clear_reserve(slice);

    for (int i = begin; i < end; ++i) {
        const Item*                 item = data[i];
        BaseCircuitComponent* const comp = item->mComponent;
        const uchar                 flag = static_cast<uchar>(item->cachedExecutionFlags);

        if (flag & lockFlag) {
            auto* const repeater = static_cast<RepeaterCapacitor*>(comp);
            rewrite::cacheValues(*repeater, *sys, item->mPos);

            const bool oldPowered = repeater->mPowered;
            const bool newPowered = rewrite::updatePower(*repeater); // 状态机提前算掉，只差 mPowered 落地
            if (newPowered != oldPowered) repeaterItems.push_back(item);
            if (!repeater->mSideComponents->mComponents.empty() || repeater->mLocked) {
                lockItems.push_back(item);
            }
            continue;
        }

        const CircuitComponentType exact = comp->getCircuitComponentType();
        if (flag & cacheFlag) {
            if (exact == CircuitComponentType::TransporterComponent) {
                auto* const dust = static_cast<TransporterComponent*>(comp);
                rewrite::cacheValues(*dust, *sys, item->mPos);
                if (dust->mNextStrength != dust->mStrength) dustItems.push_back(item);
                continue;
            }
            else if (exact == CircuitComponentType::ComparatorCapacitor) {
                rewrite::cacheValues(*static_cast<ComparatorCapacitor*>(comp), *sys, item->mPos);
                producerItems.push_back(item);
                continue;
            } else {
                comp->cacheValues(*sys, item->mPos);
                RedstoneTorchCapacitor* const torch = static_cast<RedstoneTorchCapacitor*>(comp);
                const uchar* const            state = reinterpret_cast<const uchar*>(&torch->mState);
                const bool                    noop  = state[3] == state[0] && state[4] == state[1] && state[5] == state[2] && state[2] == 0;
                if (!noop) producerItems.push_back(item);
                continue;
            }
        }

        const auto type = static_cast<CircuitComponentType>(static_cast<uint64_t>(exact) & 0xFFFF0000ull);
        const bool producer = type == CircuitComponentType::ProducerComponent || type == CircuitComponentType::CapacitorComponent;
        if ( producer & flag & producerFlag) producerItems.push_back(item);
        if (!producer & flag & consumerFlag) consumerItems.push_back(item);
    }

    if constexpr (Parallel) {
        totalProducer.fetch_add(static_cast<int>(producerItems.size()), std::memory_order_relaxed);
        totalRepeater.fetch_add(static_cast<int>(repeaterItems.size()), std::memory_order_relaxed);
        totalConsumer.fetch_add(static_cast<int>(consumerItems.size()), std::memory_order_relaxed);
        totalLock.fetch_add(static_cast<int>(lockItems.size()), std::memory_order_relaxed);
        totalDust.fetch_add(static_cast<int>(dustItems.size()), std::memory_order_relaxed);
        bar->arrive_and_wait();

        const int  producerItemCount = totalProducer.load(std::memory_order_relaxed);
        const int  repeaterItemCount = totalRepeater.load(std::memory_order_relaxed);
        const int  consumerItemCount = totalConsumer.load(std::memory_order_relaxed);
        const int  lockItemCount     = totalLock.load(std::memory_order_relaxed);
        const int  dustItemCount     = totalDust.load(std::memory_order_relaxed);
        const int  threshold         = config::get().normal_phase_threshold;
        const bool producerParallel  = producerItemCount + repeaterItemCount >= threshold;
        const bool consumerParallel  = dustItemCount + consumerItemCount + lockItemCount >= threshold;

        if (producerParallel || consumerParallel) {
            forEachItem<false, false>(id, workers, tickRepeater, repeaterItemCount, toggleRepeaterPowered);
            forEachItem<false, false>(id, workers, tickProducer, producerItemCount, evaluateComponent);
            bar->arrive_and_wait();
        } else if (id == 0) {
            forEachItem<false, true>(0, 1, tickRepeater, repeaterItemCount, toggleRepeaterPowered);
            forEachItem<false, true>(0, 1, tickProducer, producerItemCount, evaluateComponent);
        }

        if (consumerParallel) {
            forEachItem<false, false>(id, workers, tickConsumer, consumerItemCount, evaluateComponent);
            forEachItem<false, false>(id, workers, tickLock, lockItemCount, checkRepeaterLock);
            forEachItem<false, false>(id, workers, tickDust, dustItemCount, applyDustStrength);
            bar->arrive_and_wait();
        } else if (id == 0) {
            forEachItem<false, true>(0, 1, tickConsumer, consumerItemCount, evaluateComponent);
            forEachItem<false, true>(0, 1, tickLock, lockItemCount, checkRepeaterLock);
            forEachItem<false, true>(0, 1, tickDust, dustItemCount, applyDustStrength);
        }
    } else {
        forEachItem<true, true>(0, 1, tickRepeater, repeaterItems.size(), toggleRepeaterPowered);
        forEachItem<true, true>(0, 1, tickProducer, producerItems.size(), evaluateComponent);
        forEachItem<true, true>(0, 1, tickConsumer, consumerItems.size(), evaluateComponent);
        forEachItem<true, true>(0, 1, tickDust, dustItems.size(), applyDustStrength);
        forEachItem<true, true>(0, 1, tickLock, lockItems.size(), checkRepeaterLock);
    }
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
    if (allItems == 0) return;

    items.resize(allItems);
    size_t start = 0;
    for (auto cl : chunkLists) {
        ChunkCircuitComponentList::Item* const chunkItems = cl->mComponents->data();
        const size_t                           chunkSize  = cl->mComponents->size();
        for (size_t i = 0; i < chunkSize; ++i) items[start + i] = chunkItems + i;
        start += chunkSize;
    }

    const auto& worker = [](int id) {
        while (true) {
            wake.acquire();
            if (stop) return;
            workImpl<true>(id);
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
        tickProducer.resize(count);
        tickRepeater.resize(count);
        tickConsumer.resize(count);
        tickLock.resize(count);
        tickDust.resize(count);
        for (int i = 1; i < count; ++i) threads.emplace_back(worker, i);
    }

    sys  = &system;
    data = items.data();
    size = static_cast<int>(items.size());

    const bool parallel = count > 1 && static_cast<int>(items.size()) >= config::get().parallel_threshold;
    if (parallel) {
        totalProducer.store(0, std::memory_order_relaxed);
        totalRepeater.store(0, std::memory_order_relaxed);
        totalConsumer.store(0, std::memory_order_relaxed);
        totalLock.store(0, std::memory_order_relaxed);
        totalDust.store(0, std::memory_order_relaxed);
        for (int i = 1; i < count; ++i) wake.release();
        workImpl<true>(0);
    } else {
        workImpl<false>(0);
    }
}

} // namespace normal




