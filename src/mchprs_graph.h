#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "mc/world/level/BlockPos.h"

class BaseCircuitComponent;
class CircuitSystem;

namespace mchprs {

struct Entry {
    BaseCircuitComponent* comp;
    BlockPos              pos;
    int                   slot = -1;
};

struct SlotIdx {
    int cache    = -1;
    int producer = -1;
    int consumer = -1;
    int lock     = -1;
};

// remaining 用普通 uint8_t 而非 atomic: 单字节读写不会撕裂, 同一 slot 只有本线程递减,
// 与上游写入的先后无论怎么交错, 都只是让元件早/晚一轮生效, 不影响正确性。
// 值为 1<<n 时元件连续 n+1 轮活跃, 每轮右移一位衰减到 0 即跳过。
inline uint8_t*                        remaining = nullptr;
inline std::vector<uint8_t>            delayOf;
inline std::vector<std::vector<int>>   slotDownstream;
inline std::vector<int>                downstream;
inline std::vector<int>                downstreamOffset;
inline std::vector<int>                lastStrength;
inline std::vector<SlotIdx>            slotIdx;

inline std::vector<Entry> cacheEntries;
inline std::vector<Entry> producerEntries;
inline std::vector<Entry> consumerEntries;
inline std::vector<Entry> lockEntries;

void buildGraph(CircuitSystem& system);

} // namespace mchprs
