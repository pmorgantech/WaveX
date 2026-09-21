#pragma once
#include "spi_protocol/protocol.h"

#include "sequencer/pattern_exchange.hpp"
namespace WaveX {
namespace PatternStore {
// True only for a newly accepted mutation; read/duplicate/rejected requests have no side effects.
bool Request(const Protocol::SeqFileOpMessage& request,
             Sequencer::PatternExchange& exchange,
             bool external_busy = false);
void Pump(Sequencer::PatternExchange& exchange);
// Keep edits/play from crossing a confirmed load/new. Stop/readback remain available.
bool BlocksEdits();
bool Busy();
const char* CurrentName();
void SetProjectPatternName(const char* name);  // foreground, only while !Busy()
}  // namespace PatternStore
}  // namespace WaveX
