#pragma once

class BlockSource;
class CircuitSystem;

namespace redstone_optimizer {
class RedstoneOptimizerMod;
}

namespace ll::command {
class CommandHandle;
}

namespace prof {

void enable(ll::command::CommandHandle& command);

void onEvaluate(CircuitSystem& system, BlockSource& region, double milliseconds);

} // namespace prof
