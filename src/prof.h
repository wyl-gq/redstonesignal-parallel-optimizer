#pragma once

constexpr int profTimes = 20;

namespace prof {

inline int    remaining = 0;
inline double evaluate  = 0;
inline double work      = 0;
inline int    works     = 0;

inline int total    = 0;
inline int cache    = 0;
inline int producer = 0;
inline int consumer = 0;
inline int lock     = 0;

inline int graphNodes  = 0;
inline int graphPending = 0;
inline int graphLevel  = 0;
inline int graphCycle  = 0;
inline int withSources = 0;
inline int level0      = 0;
inline int level1      = 0;
inline int level2      = 0;
inline int level3plus  = 0;
inline int itemLevels    = 0;
inline int itemMaxLevel  = 0;
inline int itemScheduled = 0;

} // namespace prof
