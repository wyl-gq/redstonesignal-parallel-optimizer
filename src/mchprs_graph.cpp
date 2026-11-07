#include "mchprs_graph.h"

#include <algorithm>
#include <queue>

#include "ll/api/base/Containers.h"

#include "Config.h"

#include "mc/world/redstone/circuit/ChunkCircuitComponentList.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/BaseCircuitComponent.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/RepeaterCapacitor.h"
#include "mc/world/redstone/circuit/components/SidePoweredComponent.h"


namespace mchprs {

void buildGraph(CircuitSystem& system) {
    static ll::SmallDenseMap<BaseCircuitComponent*, int> index;
    static ll::SmallDenseSet<BaseCircuitComponent*>      seen;
    static std::vector<BaseCircuitComponent*>            nodes;
    static std::vector<BaseCircuitComponent*>            pending;

    index.clear();
    seen.clear();
    nodes.clear();
    pending.clear();

    pending.reserve(cacheEntries.size() + producerEntries.size() + consumerEntries.size());
    for (auto& e : cacheEntries)
        if (seen.insert(e.comp).second) pending.push_back(e.comp);
    for (auto& e : producerEntries)
        if (seen.insert(e.comp).second) pending.push_back(e.comp);
    for (auto& e : consumerEntries)
        if (seen.insert(e.comp).second) pending.push_back(e.comp);

    for (size_t i = 0; i < pending.size(); ++i) {
        BaseCircuitComponent* c = pending[i];
        if (index.find(c) != index.end()) continue;
        index[c] = static_cast<int>(nodes.size());
        nodes.push_back(c);
        for (auto& s : c->mSources->mComponents) {
            if (!s.mComponent) continue;
            if (seen.insert(s.mComponent).second) pending.push_back(s.mComponent);
            for (auto& ss : s.mComponent->mSources->mComponents)
                if (ss.mComponent && seen.insert(ss.mComponent).second) pending.push_back(ss.mComponent);
        }

        const CircuitComponentType t = c->getCircuitComponentType();
        if (t == CircuitComponentType::RepeaterCapacitor || t == CircuitComponentType::ComparatorCapacitor) {
            for (auto& s : static_cast<SidePoweredComponent*>(c)->mSideComponents->mComponents)
                if (s.mComponent && seen.insert(s.mComponent).second) pending.push_back(s.mComponent);
        }
    }

    static std::vector<std::vector<int>> succ;
    static std::vector<int>              indeg;
    succ.resize(nodes.size());
    for (auto& edges : succ) edges.clear();
    indeg.assign(nodes.size(), 0);
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        for (auto& item : nodes[i]->mSources->mComponents) {
            auto it = index.find(item.mComponent);
            if (it != index.end()) {
                succ[it->second].push_back(i);
                ++indeg[i];
            }
            if (!item.mComponent) continue;
            for (auto& ss : item.mComponent->mSources->mComponents) {
                auto it2 = index.find(ss.mComponent);
                if (it2 != index.end()) {
                    succ[it2->second].push_back(i);
                    ++indeg[i];
                }
            }
        }

        const CircuitComponentType t = nodes[i]->getCircuitComponentType();
        if (t == CircuitComponentType::RepeaterCapacitor || t == CircuitComponentType::ComparatorCapacitor) {
            for (auto& item : static_cast<SidePoweredComponent*>(nodes[i])->mSideComponents->mComponents) {
                auto it = index.find(item.mComponent);
                if (it != index.end()) {
                    succ[it->second].push_back(i);
                    ++indeg[i];
                }
            }
        }
    }

    static std::vector<int> level;
    static std::vector<int> indegCopy;
    level.assign(nodes.size(), 0);
    indegCopy = indeg;
    std::queue<int> ready;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        if (indegCopy[i] == 0) ready.push(i);
    int processed = 0;
    int maxLevel  = 0;
    while (!ready.empty()) {
        const int u = ready.front();
        ready.pop();
        ++processed;
        for (int v : succ[u]) {
            if (level[v] < level[u] + 1) level[v] = level[u] + 1;
            if (--indegCopy[v] == 0) ready.push(v);
        }
    }
    int hist[8]     = {0};
    int withSources = 0;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (indeg[i] > 0) ++withSources;
        if (level[i] > maxLevel) maxLevel = level[i];
        if (level[i] < 8) ++hist[level[i]];
    }

    delete[] remaining;
    remaining = nullptr;
    delayOf.clear();
    slotDownstream.clear();
    delayOf.assign(nodes.size(), 2);
    slotDownstream.assign(nodes.size(), {});
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        for (int d : succ[i]) slotDownstream[i].push_back(d);

        const CircuitComponentType t = nodes[i]->getCircuitComponentType();
        if (t == CircuitComponentType::RepeaterCapacitor) {
            auto* r    = static_cast<RepeaterCapacitor*>(nodes[i]);
            delayOf[i] = static_cast<uint8_t>(1 << (r->mInsertAt > 0 ? r->mInsertAt : 4));
        } else {
            delayOf[i] = 2;
        }
    }

    remaining = new uint8_t[(nodes.size() + 7) & ~static_cast<size_t>(7)]();
    lastStrength.assign(producerEntries.size(), 0);
    for (size_t k = 0; k < producerEntries.size(); ++k)
        lastStrength[k] = producerEntries[k].comp->getStrength();
    for (size_t i = 0; i < nodes.size(); ++i) remaining[i] = delayOf[i];

    for (auto& e : cacheEntries) {
        auto it = index.find(e.comp);
        e.slot  = it == index.end() ? -1 : it->second;
    }
    for (auto& e : producerEntries) {
        auto it = index.find(e.comp);
        e.slot  = it == index.end() ? -1 : it->second;
    }

    size_t downstreamTotal = 0;
    for (size_t k = 0; k < producerEntries.size(); ++k) {
        const int s = producerEntries[k].slot;
        if (s >= 0) downstreamTotal += slotDownstream[s].size();
    }
    downstream.resize(downstreamTotal);
    downstreamOffset.assign(producerEntries.size() + 1, 0);
    size_t offset = 0;
    for (size_t k = 0; k < producerEntries.size(); ++k) {
        downstreamOffset[k] = static_cast<int>(offset);
        const int s = producerEntries[k].slot;
        if (s < 0) continue;
        const size_t n = slotDownstream[s].size();
        if (n == 0) continue;
        std::copy(slotDownstream[s].begin(), slotDownstream[s].end(), downstream.begin() + offset);
        offset += n;
    }
    downstreamOffset[producerEntries.size()] = static_cast<int>(offset);
    for (auto& e : consumerEntries) {
        auto it = index.find(e.comp);
        e.slot  = it == index.end() ? -1 : it->second;
    }
    for (auto& e : lockEntries) {
        auto it = index.find(e.comp);
        e.slot  = it == index.end() ? -1 : it->second;
    }

    slotIdx.assign(nodes.size(), {});
    for (int k = 0; k < static_cast<int>(cacheEntries.size()); ++k)
        if (cacheEntries[k].slot >= 0) slotIdx[cacheEntries[k].slot].cache = k;
    for (int k = 0; k < static_cast<int>(producerEntries.size()); ++k)
        if (producerEntries[k].slot >= 0) slotIdx[producerEntries[k].slot].producer = k;
    for (int k = 0; k < static_cast<int>(consumerEntries.size()); ++k)
        if (consumerEntries[k].slot >= 0) slotIdx[consumerEntries[k].slot].consumer = k;
    for (int k = 0; k < static_cast<int>(lockEntries.size()); ++k)
        if (lockEntries[k].slot >= 0) slotIdx[lockEntries[k].slot].lock = k;
}

} // namespace mchprs

