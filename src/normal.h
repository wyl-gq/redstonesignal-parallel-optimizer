#pragma once

class BlockSource;
class CircuitSystem;

namespace normal {

void evaluate(CircuitSystem& system, BlockSource* region);
void shutdown();

} // namespace normal
