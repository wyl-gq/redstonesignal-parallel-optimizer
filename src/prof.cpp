#include "prof.h"

#include "redstonesignal-parallel-optimizer.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/runtime/ParamKind.h"
#include "ll/api/command/runtime/RuntimeCommand.h"
#include "ll/api/command/runtime/RuntimeOverload.h"
#include "ll/api/memory/Hook.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/redstone/circuit/ChunkCircuitComponentList.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/BaseCircuitComponent.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace prof {

namespace {

constexpr int DIM_COUNT = 5; // 主世界、下界、末地，以及后续新维度

constexpr int TYPE_DUST      = 0;
constexpr int TYPE_REPEATER  = 1;
constexpr int TYPE_COMPARATOR= 2;
constexpr int TYPE_PULSE     = 3;
constexpr int TYPE_TORCH     = 4;
constexpr int TYPE_PRODUCER  = 5;
constexpr int TYPE_CONSUMER  = 6;
constexpr int TYPE_PISTON    = 7;
constexpr int TYPE_RAIL      = 8;
constexpr int TYPE_POWERED   = 9;
constexpr int TYPE_OTHER     = 10;
constexpr int TYPE_COUNT     = 11;

char const* const TYPE_NAMES[TYPE_COUNT] = {
    "红石粉",
    "中继器",
    "比较器",
    "脉冲",
    "火把",
    "生产者",
    "消费者",
    "活塞",
    "铁轨",
    "充能方块",
    "其它",
};

int typeIndexOf(CircuitComponentType type) {
    switch (type) {
    case CircuitComponentType::TransporterComponent:
        return TYPE_DUST;
    case CircuitComponentType::RepeaterCapacitor:
        return TYPE_REPEATER;
    case CircuitComponentType::ComparatorCapacitor:
        return TYPE_COMPARATOR;
    case CircuitComponentType::PulseCapacitor:
        return TYPE_PULSE;
    case CircuitComponentType::RedstoneTorchCapacitor:
        return TYPE_TORCH;
    case CircuitComponentType::ProducerComponent:
        return TYPE_PRODUCER;
    case CircuitComponentType::ConsumerComponent:
        return TYPE_CONSUMER;
    case CircuitComponentType::PistonConsumer:
        return TYPE_PISTON;
    case CircuitComponentType::BaseRailTransporter:
        return TYPE_RAIL;
    case CircuitComponentType::PoweredBlockComponent:
        return TYPE_POWERED;
    default:
        return TYPE_OTHER;
    }
}

// 数量类统计：平均 / 最大 / 最小
struct CountRange {
    bool               touched  = false;
    unsigned long long samples  = 0;
    unsigned long long sum      = 0;
    unsigned long long maxValue = 0;
    unsigned long long minValue = 0;

    void add(unsigned long long value) {
        if (!touched) {
            touched  = true;
            minValue = value;
        }
        if (value > maxValue) maxValue = value;
        if (value < minValue) minValue = value;
        sum += value;
        ++samples;
    }

    [[nodiscard]] double average() const { return samples ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0; }
};

// 毫秒类统计：平均 / 最大 / 最小
struct TimeRange {
    bool               touched  = false;
    unsigned long long samples  = 0;
    double             sum      = 0.0;
    double             maxValue = 0.0;
    double             minValue = 0.0;

    void add(double value) {
        if (!touched) {
            touched  = true;
            minValue = value;
        }
        if (value > maxValue) maxValue = value;
        if (value < minValue) minValue = value;
        sum += value;
        ++samples;
    }

    [[nodiscard]] double average() const { return samples ? sum / static_cast<double>(samples) : 0.0; }
};

struct ChunkCounter {
    unsigned long long itemsSum = 0;
    unsigned long long samples  = 0;
};

using ChunkKey = std::tuple<int, int, int>; // 维度, 区块 x, 区块 z

struct DimensionStats {
    TimeRange                evaluateTime;
    CountRange               items;
    CountRange               chunks;
    CountRange               typeCounts[TYPE_COUNT];
    std::map<ChunkKey, ChunkCounter> chunkItems;

    // 本 tick 的最新快照，用于整体段跨维度求和
    bool pending       = false;
    int  lastItems     = 0;
    int  lastChunks    = 0;
    int  lastTypeCounts[TYPE_COUNT] = {};
};

struct Profiler {
    bool         active       = false;
    int          targetTicks  = 0;
    int          elapsedTicks = 0;
    int          topChunks    = 3;
    DimensionStats overall;
    DimensionStats dimensions[DIM_COUNT];
};

Profiler g;

std::string formatSeconds(double milliseconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", milliseconds);
    return buffer;
}

std::string formatCount(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

std::string formatLine(std::string const& name, CountRange const& range) {
    if (!range.touched) return " - §a" + name + "§r 无数据";
    return " - §a" + name + "§r 平均 " + formatCount(range.average()) + " / 最大 " + std::to_string(range.maxValue)
         + " / 最小 " + std::to_string(range.minValue);
}

std::string formatChunks(std::map<ChunkKey, ChunkCounter> const& chunkItems, int topChunks) {
    if (chunkItems.empty()) return " - §a区块 TOP " + std::to_string(topChunks) + "§r 无数据";

    std::vector<std::pair<ChunkKey, double>> averages;
    averages.reserve(chunkItems.size());
    for (auto const& [key, counter] : chunkItems) {
        if (counter.samples == 0) continue;
        averages.emplace_back(key, static_cast<double>(counter.itemsSum) / static_cast<double>(counter.samples));
    }
    if (averages.empty()) return " - §a区块 TOP " + std::to_string(topChunks) + "§r 无数据";

    const size_t wanted = std::min(averages.size(), static_cast<size_t>(std::max(topChunks, 0)));
    std::partial_sort(
        averages.begin(),
        averages.begin() + wanted,
        averages.end(),
        [](auto const& left, auto const& right) { return left.second > right.second; }
    );

    std::string result = " - §a区块 TOP " + std::to_string(wanted) + "§r";
    for (size_t i = 0; i < wanted; ++i) {
        result += "\n   - §a维度 " + std::to_string(std::get<0>(averages[i].first)) + " (" + std::to_string(std::get<1>(averages[i].first))
                + ", " + std::to_string(std::get<2>(averages[i].first)) + ")§r 平均 " + formatCount(averages[i].second);
    }
    return result;
}

std::string printDimension(std::string const& title, DimensionStats const& stats, int ticks, int topChunks) {
    std::string text = "§b§l-- " + title + " --§r";

    text += "\n" + std::string(" - §aevaluate§r 平摊 ") + formatSeconds(ticks > 0 ? stats.evaluateTime.sum / ticks : 0.0)
          + " ms/tick / 平均 " + formatSeconds(stats.evaluateTime.average()) + " ms/次 / 最大 "
          + formatSeconds(stats.evaluateTime.maxValue) + " ms / 最小 " + formatSeconds(stats.evaluateTime.minValue)
          + " ms / 执行 " + std::to_string(stats.evaluateTime.samples) + " 次";

    text += "\n" + formatLine("items", stats.items);
    text += "\n" + formatLine("区块数", stats.chunks);

    for (int type = 0; type < TYPE_COUNT; ++type) {
        if (!stats.typeCounts[type].touched) continue;
        text += "\n" + formatLine(TYPE_NAMES[type], stats.typeCounts[type]);
    }

    text += "\n" + formatChunks(stats.chunkItems, topChunks);
    return text;
}

void finish() {
    g.active = false;

    std::string text = "§b§l[rpo prof]§r 最近 " + std::to_string(g.targetTicks) + " tick / 平摊 "
                     + formatSeconds(g.targetTicks > 0 ? g.overall.evaluateTime.sum / g.targetTicks : 0.0)
                     + " ms/tick / 平均 " + formatSeconds(g.overall.evaluateTime.average()) + " ms/次\n";

    text += printDimension("整体", g.overall, g.targetTicks, g.topChunks);

    for (int dimension = 0; dimension < DIM_COUNT; ++dimension) {
        DimensionStats const& stats = g.dimensions[dimension];
        if (!stats.evaluateTime.touched) continue;
        text += "\n";
        text += printDimension("维度 " + std::to_string(dimension), stats, g.targetTicks, g.topChunks);
    }

    TextPacket::createRawMessage(text).sendToClients();
    redstone_optimizer::RedstoneOptimizerMod::getInstance().getSelf().getLogger().info(text);

    g = Profiler{};
}

void accumulateDimension(DimensionStats& stats, double milliseconds, CircuitSystem& system, int dimension) {
    stats.evaluateTime.add(milliseconds);

    int items  = 0;
    int chunks = 0;
    int typeCounts[TYPE_COUNT] = {};

    for (auto& [chunkPos, chunkList] : system.mSceneGraph->mActiveComponentsPerChunk) {
        if (chunkList.mComponents->empty()) continue;

        const int chunkItems = static_cast<int>(chunkList.mComponents->size());
        ++chunks;
        items += chunkItems;

        const ChunkKey key{dimension, chunkPos.x, chunkPos.z};
        ChunkCounter&  counter    = stats.chunkItems[key];
        counter.itemsSum += static_cast<unsigned long long>(chunkItems);
        ++counter.samples;

        ChunkCounter& overallCounter = g.overall.chunkItems[key];
        overallCounter.itemsSum += static_cast<unsigned long long>(chunkItems);
        ++overallCounter.samples;

        for (auto& item : *chunkList.mComponents) {
            if (!item.mComponent) continue;
            ++typeCounts[typeIndexOf(item.mComponent->getCircuitComponentType())];
        }
    }

    stats.items.add(static_cast<unsigned long long>(items));
    stats.chunks.add(static_cast<unsigned long long>(chunks));
    for (int type = 0; type < TYPE_COUNT; ++type) {
        stats.typeCounts[type].add(static_cast<unsigned long long>(typeCounts[type]));
    }

    g.overall.evaluateTime.add(milliseconds);

    // 记录本 tick 快照，tick 结束时统一跨维度求和到整体
    stats.pending    = true;
    stats.lastItems  = items;
    stats.lastChunks = chunks;
    for (int type = 0; type < TYPE_COUNT; ++type) stats.lastTypeCounts[type] = typeCounts[type];
}

// 每个 tick 结束时把各维度的最新快照求和，整体段的 items / 区块数 / 各类组件数都是全维度之和
void accumulateOverallPerTick() {
    int  items                 = 0;
    int  chunks                = 0;
    int  typeCounts[TYPE_COUNT] = {};
    bool any                   = false;

    for (int dimension = 0; dimension < DIM_COUNT; ++dimension) {
        DimensionStats& stats = g.dimensions[dimension];
        if (!stats.pending) continue;

        any       = true;
        items    += stats.lastItems;
        chunks   += stats.lastChunks;
        for (int type = 0; type < TYPE_COUNT; ++type) typeCounts[type] += stats.lastTypeCounts[type];

        stats.pending = false;
    }

    if (!any) return;

    g.overall.items.add(static_cast<unsigned long long>(items));
    g.overall.chunks.add(static_cast<unsigned long long>(chunks));
    for (int type = 0; type < TYPE_COUNT; ++type) {
        g.overall.typeCounts[type].add(static_cast<unsigned long long>(typeCounts[type]));
    }
}

LL_TYPE_INSTANCE_HOOK(ProfTickHook, ll::memory::HookPriority::Normal, Level, &Level::$tick, void) {
    origin();

    if (!g.active) return;

    accumulateOverallPerTick();

    if (++g.elapsedTicks < g.targetTicks) return;

    finish();
}

} // namespace

void enable(ll::command::CommandHandle& command) {
    ProfTickHook::hook();

    command.runtimeOverload()
        .text("prof")
        .required("ticks", ll::command::ParamKind::Int)
        .optional("chunks", ll::command::ParamKind::Int)
        .execute([](CommandOrigin const&, CommandOutput& output, ll::command::RuntimeCommand const& self) {
            const int ticks = self["ticks"].get<ll::command::ParamKind::Int>();
            if (ticks <= 0) {
                output.error("ticks must be greater than 0 but got: {}", ticks);
                return;
            }

            int chunks = 3;
            if (self["chunks"].has_value()) chunks = self["chunks"].get<ll::command::ParamKind::Int>();
            if (chunks <= 0) {
                output.error("chunks must be greater than 0 but got: {}", chunks);
                return;
            }

            if (g.active) {
                output.error("profiling is already running, remaining {} ticks", g.targetTicks - g.elapsedTicks);
                return;
            }

            g             = Profiler{};
            g.active      = true;
            g.targetTicks = ticks;
            g.topChunks   = chunks;

            output.success("profiling started, results will be sent after {} ticks", ticks);
        });
}

void onEvaluate(CircuitSystem& system, BlockSource& region, double milliseconds) {
    if (!g.active) return;

    int dimension = static_cast<int>(region.getDimensionId());
    if (dimension < 0 || dimension >= DIM_COUNT) dimension = DIM_COUNT - 1;

    accumulateDimension(g.dimensions[dimension], milliseconds, system, dimension);
}

} // namespace prof
