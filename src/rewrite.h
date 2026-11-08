#pragma once

#include <cstdint>
#include <mc/world/level/BlockPos.h>
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>
#include <mc/world/redstone/circuit/components/ComparatorCapacitor.h>
#include <mc/world/redstone/circuit/components/RedstoneTorchCapacitor.h>
#include <mc/world/redstone/circuit/components/RepeaterCapacitor.h>
#include <mc/world/redstone/circuit/components/TransporterComponent.h>

namespace rewrite {

// 红石粉：mNextStrength = max(0, max(src->getStrength() - mDampening))
inline void cacheValues(TransporterComponent& c, ::CircuitSystem&, ::BlockPos const&) {
    int maxStrength = 0;
    for (const auto& item : c.mSources->mComponents)
        maxStrength = std::max(maxStrength, item.mComponent->getStrength() - item.mDampening);
    c.mNextStrength = maxStrength;
}

// 中继器：先算 mNextPower = 有没有源(强度-衰减) > 0；没被锁死时维护 mPulseCount / mPulse / mNextPulse
inline void cacheValues(RepeaterCapacitor& c, ::CircuitSystem&, ::BlockPos const&) {
    c.mNextPower = false;

    for (const auto& item : c.mSources->mComponents) {
        if (item.mComponent->getStrength() > item.mDampening) {
            c.mNextPower = true;
            break;
        }
    }

    if (c.mLocked) return;

    const bool pulse = c.mPulse;
    if (pulse == c.mNextPower) {
        c.mPulseCount = 0;
    } else {
        ++c.mPulseCount;
        if (c.mPulseCount == 1) c.mNextPulse = !pulse;
        c.mPulse = !pulse;
    }
}

// 中继器 evaluate 的状态机部分：只改 mOnStates / mNextPulse（没人会读它们），返回新的 mPowered。
// mPowered 的写入要留到 producer 相位，因为它通过 getStrength 被别的元件读到。
inline bool updatePower(RepeaterCapacitor& c) {
    if (c.mLocked) return c.mPowered; // 锁死时 evaluate 整段被门控

    const int count = c.mInsertAt;
    for (int i = 0; i < 4; ++i)
        c.mOnStates[i] = i < count ? c.mOnStates[i + 1] : static_cast<RepeaterCapacitor::States>(c.mNextPower ? 1 : 0);

    if (count > 0) {
        const int pulseCount = c.mPulseCount;
        if (pulseCount > count && (!c.mPulse || pulseCount != 2)) {
            if (c.mOnStates[0] <= RepeaterCapacitor::States::On) {
                const auto newState = static_cast<RepeaterCapacitor::States>(c.mNextPulse + 2);
                c.mNextPulse        = !c.mNextPulse;
                for (int i = 0; i <= count; ++i) c.mOnStates[i] = newState;
            }
        } else {
            const RepeaterCapacitor::States s0 = c.mOnStates[0];
            const RepeaterCapacitor::States s1 = c.mOnStates[1];

            if (s1 != RepeaterCapacitor::States::Off || s0 != RepeaterCapacitor::States::OffLocked) {
                if (s1 == RepeaterCapacitor::States::On && s0 == RepeaterCapacitor::States::OnLocked) {
                    c.mOnStates[1] = RepeaterCapacitor::States::OnLocked;
                } else if (s0 == RepeaterCapacitor::States::Off || s0 == RepeaterCapacitor::States::On) {
                    int same = 0;
                    for (int i = 1; i <= count; ++i) same += (c.mOnStates[i] == s0);

                    const RepeaterCapacitor::States newState =
                        (c.mOnStates[0] != RepeaterCapacitor::States::Off) || (same < count)
                            ? RepeaterCapacitor::States::OnLocked
                            : RepeaterCapacitor::States::OffLocked;
                    for (int i = 0; i <= count; ++i) c.mOnStates[i] = newState;
                }
            } else {
                c.mOnStates[1] = RepeaterCapacitor::States::OffLocked;
            }
        }
    }

    return c.mOnStates[0] == RepeaterCapacitor::States::On || c.mOnStates[0] == RepeaterCapacitor::States::OnLocked;
}

// 中继器 checkLock：mLocked 先清 0，扫 mSideComponents，任一侧 (getStrength - mDampening) > 0
// 就置 mLocked = 1 并把 mOnStates[1..3] 压平成 mOnStates[0]
inline void checkLock(RepeaterCapacitor& c, ::CircuitSystem&, ::BlockPos const&) {
    c.mLocked = false;

    for (const auto& item : c.mSideComponents->mComponents) {
        if (item.mComponent->getStrength() > item.mDampening) {
            c.mLocked                          = true;
            const RepeaterCapacitor::States s0 = c.mOnStates[0];
            c.mOnStates[1]                     = s0;
            c.mOnStates[2]                     = s0;
            c.mOnStates[3]                     = s0;
            break;
        }
    }
}

// 比较器：没被容器写过模拟强度就直接返回；否则重算 rear（只算 mDirectlyPowered 的源）和 side 最大值
inline void cacheValues(ComparatorCapacitor& c, ::CircuitSystem&, ::BlockPos const&) {
    if (!c.mHasAnalogBeenSet) return;

    int RearStrength   = 0;
    int SideStrengths = 0;

    for (const auto& item : c.mSources->mComponents) {
        if (item.mDirectlyPowered) RearStrength = std::max(RearStrength, item.mComponent->getStrength() - item.mDampening);
    }

    for (const auto& item : c.mSideComponents->mComponents)
        SideStrengths = std::max(SideStrengths, item.mComponent->getStrength() - item.mDampening);

    c.mRearStrength = RearStrength;
    c.mSideStrengths = SideStrengths;
}

// 位置比较：BlockPos::operator== 有歧义，逐字段比
inline bool samePos(::BlockPos const& a, ::BlockPos const& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// RedstoneTorchCapacitor::_findStrongestStrength 的移植：
// 取所有源里最大的 (getStrength - mDampening)；同时记录最强那条路是否来自火把自身位置（bPowerFromSelf）
inline int findStrongestStrength(RedstoneTorchCapacitor& c, ::BlockPos const& pos, bool& powerFromSelf) {
    using List = ::CircuitComponentList;
    const auto& srcs = c.mSources->mComponents;

    powerFromSelf      = false;
    if (srcs.empty()) return 0;

    int          best      = 0;
    const List::Item* bestItem  = nullptr;
    const List::Item* prevItem  = nullptr;
    const List::Item* selfItem  = nullptr;
    const List::Item* queryItem = nullptr;

    for (const auto& item : srcs) {
        if (!item.mComponent) continue;

        const CircuitComponentType t = item.mComponent->getCircuitComponentType();
        if (t == CircuitComponentType::ProducerComponent
            || (t == CircuitComponentType::CapacitorComponent && item.mComponent->mSources->mComponents.empty())) {
            const int s = item.mComponent->getStrength();
            if (s >= best && s != 0) {
                bestItem = &item;
                best     = s;
            }
            continue;
        }

        int v10 = 0;
        int v11 = 0;
        int v21 = 0;

        for (const auto& sub : item.mComponent->mSources->mComponents) {
            if (!sub.mDirectlyPowered || !sub.mComponent) continue;

            int s = sub.mComponent->getStrength() - sub.mDampening;
            if (s < 0) s = 0;
            v11 = s;

            bool matched = false;
            if (s >= best && s != 0) {
                best     = s;
                bestItem = &sub;
                if (samePos(sub.mPos, pos)) {
                    selfItem = &sub;
                    v10      = s;
                    v21      = s;
                    matched  = true;
                } else
                    prevItem = &sub;
            }
            if (!matched) {
                if (samePos(sub.mPos, pos)) {
                    selfItem = &sub;
                    v10      = v11;
                    v21      = v11;
                } else
                    v10 = v21;
            }
        }

        if (v11 == v10) {
            if (prevItem) {
                queryItem = nullptr;
                bestItem  = prevItem;
                selfItem  = nullptr;
                goto after;
            }
        }
        queryItem = selfItem;
    after:
        if (best || !queryItem) powerFromSelf = (bestItem == queryItem && queryItem != nullptr);
        else {
            bestItem    = queryItem;
            powerFromSelf = true;
        }
    }
    return best;
}

// 红石火把：mState 的 3 个 bool 在生成头里是无名 UntypedStorage，
// mState[0] = state[0..2]{on, onLocked, changed}，mState[1] = state[3..5]
inline void cacheValues(RedstoneTorchCapacitor& c, ::CircuitSystem& sys, ::BlockPos const& pos) {
    uchar* state = reinterpret_cast<uchar*>(&c.mState);

    if (c.mNextOrder) {
        bool found = false;
        for (const auto& item : c.mSources->mComponents) {
            if (!item.mComponent) continue;
            const CircuitComponentType group = item.mComponent->getCircuitComponentGroupType();
            if (group != CircuitComponentType::ConsumerComponent
                && group != CircuitComponentType::PoweredBlockComponent)
                continue;

            for (const auto& sub : item.mComponent->mSources->mComponents) {
                if (!sub.mComponent || !sub.mDirectlyPowered) continue;
                if (sub.mComponent->getCircuitComponentType() == CircuitComponentType::RedstoneTorchCapacitor) continue;

                int strength = sub.mComponent->getStrength() - sub.mDampening;
                if (strength < 0) strength = 0;
                if (strength != 0) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }

        if (found) {
            state[3] = 1;
            state[4] = 0;
            state[5] = (state[0] == 0);
            c.mSelfPowerCount = -1;
        } else {
            state[4] = 0;
            if (c.mSelfPowerCount) {
                state[3] = 0;
                state[5] = state[0];
            } else {
                state[3] = 1;
                state[5] = (state[0] == 0);
                if (c.mNextOrder) c.mNextOrder->mSelfPowerCount = -1;
                if (c.mNextOrder) {
                    RedstoneTorchCapacitor* cur = c.mNextOrder->mNextOrder;
                    for (int i = 0; cur && cur != &c && i < 4; ++i) {
                        cur->mSelfPowerCount = 1;
                        cur                  = cur->mNextOrder;
                    }
                }
            }
        }
        return;
    }

    bool      powerFromSelf = false;
    const int strongest     = findStrongestStrength(c, pos, powerFromSelf);

    bool on       = false;
    bool onLocked = false;
    int  count    = 0;

    if (powerFromSelf) {
        count     = c.mSelfPowerCount;
        int next  = count;
        if (!c.mCanReigniteFromBurnout && count <= 32) {
            next              = count + 1;
            c.mSelfPowerCount = count + 1;
            count             = count + 1;
        }
        if (next >= 16) {
            if (next == 16) onLocked = true;
        } else {
            onLocked = true;
            on       = (state[0] == 0);
        }
    } else {
        count                     = 0;
        c.mCanReigniteFromBurnout = false;
        c.mSelfPowerCount         = 0;
        on                        = strongest <= 0;
    }

    const bool changed = on != (state[0] != 0);
    state[4]           = onLocked;
    state[3]           = on;
    state[5]           = changed;
    if (count > 32) c.mCanReigniteFromBurnout = true;
}

} // namespace rewrite
