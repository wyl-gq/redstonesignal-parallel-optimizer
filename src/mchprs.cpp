#include "mchprs.h"
#include "Config.h"
#include "SpinBarrier.h"
#include "mchprs_graph.h"
#include "nocheck_vector.h"
#include "prof.h"


#include "mc/world/level/BlockSource.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RedstoneLogicExecutionFlags.h"
#include "mc/world/redstone/circuit/components/RepeaterCapacitor.h"
#include "mc/world/redstone/circuit/components/SidePoweredComponent.h"
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>


#include <atomic>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <queue>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>


namespace mchprs {

constexpr uchar producerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsProducer);
constexpr uchar consumerFlag = static_cast<uchar>(RedstoneLogicExecutionFlags::EvaluateAsConsumer);

constexpr int parallelThreshold = 256;

namespace {

int                       count = 0;
SpinBarrier*              bar   = nullptr;
std::counting_semaphore<> wake{0};
std::vector<std::thread>  threads;
CircuitSystem*            sys;
int                       speeds;
int                       cacheSize;
int                       producerSize;
int                       consumerSize;
int                       lockSize;
int                       remainingSize;
bool                      stop = false;
std::atomic<int>          remainingcacheSize    = 0;
std::atomic<int>          remainingproducerSize = 0;
std::atomic<int>          remainingconsumerSize = 0;
std::atomic<int>          remaininglockSize     = 0;
std::atomic<int>          totalRemainingSize    = 0;

bool classified      = false;
int  classifiedTotal = 0;

std::vector<RepeaterCapacitor*> sideLessRepeaters;

struct ThreadLists {
    nocheck_vector<int> cache;
    nocheck_vector<int> producer;
    nocheck_vector<int> consumer;
    nocheck_vector<int> lock;
};

std::vector<ThreadLists> lists;

template <bool decrement>
void scanSlots(int slotBegin, int slotEnd, ThreadLists& list) {
    auto& [cache, producer, consumer, lock] = list;

    const int lastCacheSize    = static_cast<int>(cache.size()); cache.clear();
    const int lastProducerSize = static_cast<int>(producer.size()); producer.clear();
    const int lastConsumerSize = static_cast<int>(consumer.size()); consumer.clear();
    const int lastLockSize     = static_cast<int>(lock.size()); lock.clear();

    for (int slot = slotBegin; slot < slotEnd; slot += 8) {
        uint64_t w;
        std::memcpy(&w, remaining + slot, sizeof(w));
        if (w == 0) continue;
        uint64_t nz = (((w & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | w) & 0x8080808080808080ULL;
        while (nz) {
            unsigned long bit;
            _BitScanForward64(&bit, nz);
            nz &= nz - 1;
            const int s = slot + static_cast<int>(bit >> 3);
            uint8_t   v = remaining[s];
            if constexpr (decrement) {
                v >>= 1;
                remaining[s] = v;
            }
            if (v == 0) continue;
            const SlotIdx& si = slotIdx[s];
            if (si.cache >= 0) cache.push_back(si.cache);
            if (si.producer >= 0) producer.push_back(si.producer);
            if (si.consumer >= 0) consumer.push_back(si.consumer);
            if (si.lock >= 0) lock.push_back(si.lock);
        }
    }

    const int cacheDelta    = static_cast<int>(cache.size()) - lastCacheSize;
    const int producerDelta = static_cast<int>(producer.size()) - lastProducerSize;
    const int consumerDelta = static_cast<int>(consumer.size()) - lastConsumerSize;
    const int lockDelta     = static_cast<int>(lock.size()) - lastLockSize;
    const int totalDelta    = cacheDelta + producerDelta + consumerDelta + lockDelta;

    remainingcacheSize.fetch_add(cacheDelta, std::memory_order_relaxed);
    remainingproducerSize.fetch_add(producerDelta, std::memory_order_relaxed);
    remainingconsumerSize.fetch_add(consumerDelta, std::memory_order_relaxed);
    remaininglockSize.fetch_add(lockDelta, std::memory_order_relaxed);
    totalRemainingSize.fetch_add(totalDelta, std::memory_order_relaxed);
}

template <typename Fn>
void forEachEntry(int id, int parts, const nocheck_vector<int> ThreadLists::* member, Fn&& fn) {
    int total = 0;
    for (int t = 0; t < parts; ++t) total += static_cast<int>((lists[t].*member).size());

    const int g0   = id * total / parts;
    const int g1   = (id + 1) * total / parts;
    int       base = 0;
    for (int t = 0; t < parts; ++t) {
        const nocheck_vector<int>& vec  = lists[t].*member;
        const int                  sz   = static_cast<int>(vec.size());
        const int                  from = g0 > base ? g0 : base;
        const int                  to   = g1 < base + sz ? g1 : base + sz;
        for (int k = from - base; k < to - base; ++k) fn(vec[k]);
        base += sz;
    }
}

template <bool parallel>
void work(int id) {
    const int    parts     = parallel ? count : 1;
    const int    groups    = remainingSize / 8;
    const int    slotBegin = id * groups / parts * 8;
    const int    slotEnd   = (id + 1) * groups / parts * 8;
    ThreadLists& list      = lists[id];

    scanSlots<false>(slotBegin, slotEnd, list);
    if constexpr (parallel) bar->arrive_and_wait();

    for (int s = 0; s < speeds; ++s) {
        if (totalRemainingSize == 0) break;

        if (remainingcacheSize > 0) {
            forEachEntry(id, parts, &ThreadLists::cache, [&](int idx) {
                cacheEntries[idx].comp->cacheValues(*sys, cacheEntries[idx].pos);
            });
            if constexpr (parallel) bar->arrive_and_wait();
        }

        if (remainingproducerSize > 0) {
            const int* const ds  = downstream.data();
            const int* const dso = downstreamOffset.data();
            forEachEntry(id, parts, &ThreadLists::producer, [&](int idx) {
                const Entry& e = producerEntries[idx];
                if (e.comp->evaluate(*sys, e.pos)) e.comp->mNeedsUpdate = true;
                const int strength = e.comp->getStrength();
                if (strength != lastStrength[idx]) {
                    lastStrength[idx] = strength;
                    for (int k = dso[idx]; k < dso[idx + 1]; ++k) remaining[ds[k]] = delayOf[ds[k]];
                }
            });
            if constexpr (parallel) bar->arrive_and_wait();
        }

        if (remainingconsumerSize > 0) {
            forEachEntry(id, parts, &ThreadLists::consumer, [&](int idx) {
                const Entry& e = consumerEntries[idx];
                if (e.comp->evaluate(*sys, e.pos)) e.comp->mNeedsUpdate = true;
            });
        }

        if (remaininglockSize > 0) {
            forEachEntry(id, parts, &ThreadLists::lock, [&](int idx) {
                const Entry& e = lockEntries[idx];
                e.comp->checkLock(*sys, e.pos);
            });
        }
        scanSlots<true>(slotBegin, slotEnd, list);
        if constexpr (parallel) bar->arrive_and_wait();
    }
}

} // namespace

void evaluate(CircuitSystem& system, BlockSource* region) {
    if (!config::compiled || !classified) {
        cacheEntries.clear();
        producerEntries.clear();
        consumerEntries.clear();
        lockEntries.clear();
        sideLessRepeaters.clear();

        int itemCount = 0;
        for (auto& [chunkPos, chunkList] : system.mSceneGraph->mActiveComponentsPerChunk) {
            if (chunkList.mComponents->empty()) continue;

            const bool shouldEvaluate = region->areChunksFullyLoaded(chunkPos, 32);
            chunkList.bShouldEvaluate = shouldEvaluate;
            if (!shouldEvaluate) continue;

            ChunkCircuitComponentList::Item* const chunkItems = chunkList.mComponents->data();
            const size_t                           chunkSize  = chunkList.mComponents->size();
            for (size_t i = 0; i < chunkSize; ++i) {
                ChunkCircuitComponentList::Item* item = chunkItems + i;

                const CircuitComponentType& type = item->mComponent->getCircuitComponentGroupType();
                if (type == CircuitComponentType::TransporterComponent) continue;
                ++itemCount;

                const uchar flags = static_cast<uchar>(item->cachedExecutionFlags);
                if (flags & static_cast<uchar>(RedstoneLogicExecutionFlags::CacheValues))
                    cacheEntries.push_back({item->mComponent, item->mPos});

                if ((type == CircuitComponentType::ProducerComponent
                     || type == CircuitComponentType::CapacitorComponent)
                    && (flags & producerFlag))
                    producerEntries.push_back({item->mComponent, item->mPos});

                if (type != CircuitComponentType::ProducerComponent && type != CircuitComponentType::CapacitorComponent
                    && (flags & consumerFlag))
                    consumerEntries.push_back({item->mComponent, item->mPos});

                if (flags & static_cast<uchar>(RedstoneLogicExecutionFlags::CheckLock)) {
                    bool skipped = false;
                    if (item->mComponent->getCircuitComponentType() == CircuitComponentType::RepeaterCapacitor) {
                        auto* repeater = static_cast<RepeaterCapacitor*>(item->mComponent);
                        if (repeater->mSideComponents->mComponents.empty()) {
                            repeater->mLocked = false;
                            sideLessRepeaters.push_back(repeater);
                            skipped = true;
                        }
                    }
                    if (!skipped) lockEntries.push_back({item->mComponent, item->mPos});
                }
            }
        }

        classifiedTotal = itemCount;
        classified      = true;

        buildGraph(system);
    }

    std::memcpy(remaining, delayOf.data(), delayOf.size());

    remainingSize = (static_cast<int>(delayOf.size()) + 7) & ~7;

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
        for (int i = 1; i < count; ++i) {
            threads.emplace_back(
                [](int id) {
                    while (true) {
                        wake.acquire();
                        if (stop) return;
                        work<true>(id);
                    }
                },
                i
            );
        }
    }

    if (static_cast<int>(lists.size()) != count) lists.resize(count);
    for (int i = 0; i < count; ++i) {
        const int cap = (i + 1) * remainingSize / count - i * remainingSize / count + 16;
        lists[i].cache.clear_reserve(cap);
        lists[i].producer.clear_reserve(cap);
        lists[i].consumer.clear_reserve(cap);
        lists[i].lock.clear_reserve(cap);
    }

    sys          = &system;
    speeds       = std::max(config::get().speeds, 1);
    cacheSize    = static_cast<int>(cacheEntries.size());
    producerSize = static_cast<int>(producerEntries.size());
    consumerSize = static_cast<int>(consumerEntries.size());
    lockSize     = static_cast<int>(lockEntries.size());

    prof::total    = classifiedTotal;
    prof::cache    = cacheSize;
    prof::producer = producerSize;
    prof::consumer = consumerSize;
    prof::lock     = lockSize;

    remainingcacheSize    = 0;
    remainingproducerSize = 0;
    remainingconsumerSize = 0;
    remaininglockSize     = 0;
    totalRemainingSize    = 0;

    const bool parallel = count > 1 && cacheSize + producerSize + consumerSize + lockSize >= parallelThreshold;
    const auto startTime = std::chrono::steady_clock::now();
    if (parallel) {
        for (int i = 1; i < count; ++i) wake.release();
        work<true>(0);
    } else {
        work<false>(0);
    }
    if (prof::remaining > 0) {
        prof::work  += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
        prof::works += speeds;
    }
}

void setCompiled(bool compiled) {
    config::compiled = compiled;
    invalidate();
}

void invalidate() {
    classified      = false;
    classifiedTotal = 0;
    cacheEntries.clear();
    producerEntries.clear();
    consumerEntries.clear();
    lockEntries.clear();
    sideLessRepeaters.clear();
    delete[] remaining;
    remaining = nullptr;
    delayOf.clear();
    slotDownstream.clear();
    downstream.clear();
    downstreamOffset.clear();
}

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

} // namespace mchprs
