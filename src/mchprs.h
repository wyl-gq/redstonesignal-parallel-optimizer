#pragma once

class BlockSource;
class CircuitSystem;

namespace mchprs {

void evaluate(CircuitSystem& system, BlockSource* region);

void setCompiled(bool compiled);

void invalidate();

void shutdown();

} // namespace mchprs
