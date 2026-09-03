# Test Remediation Plan

**Status:** Open test gaps only. Completed audit work is in `CHANGELOG.md` and
git history.

Prioritise defect classes over line-count coverage.

## Validate untrusted inputs

Add the existing exact-sized, ASan-backed payload sweep to ESP32
`PacketRouter::route_uart_message` / `route_packet` and shared
`ParseUartPacket` / `ProtocolHandler::ValidatePacket`. Exercise every message
type, payload lengths from zero through the largest message plus four bytes,
and NUL/non-NUL fill patterns. Assert that undersized payloads reach no handler.

Replace `PacketRouterTest.ErrorMessageWithUnterminatedTextIsBounded` with a
test translation unit that does not override `PacketRouter::handle_error`.
Add focused tests for I/O-cost, semantic-path, excluded-TU, and concurrency
defects that a size sweep cannot cover.

## Add DSP properties

1. `Render()` never reads outside `[start_frame, end_frame)` for randomized
   trigger parameters. Use a guard page as well as ASan.
2. Output stays within the active source region's min/max and phase remains in
   that region after every render.
3. Zeroed storage plus `Init()` is equivalent to constructed storage plus
   `Init()`. Reset `voices_` in `Init()` before enabling this test.

Keep regression cases for trimmed release tails, fractional phase preservation,
and source-rate fade duration.

## Test concurrency at the right boundary

- Replace the FreeRTOS recursive-mutex mock with a real `std::recursive_mutex`
  and stress `ListenerSlot` under ASan/TSan.
- Extract the HAL-free WAV ring buffer from `audio_engine.cpp` and stress its
  producer/consumer behavior under TSan. TSan validates the C++ model, not
  Cortex-M7 interrupt preemption.
- Use on-target assertions or an architectural lint for LVGL task provenance;
  it is not a value-level unit-test property.
- Consider a countdown ratchet for first-party `volatile` declarations only
  after auditing the remaining uses. It prevents regressions but is not a race
  detector.

## Close structural coverage gaps

- Continue extracting HAL-free units from `audio_engine.cpp`; its excluded
  placeholder tests are a documented gap, not coverage.
- Build an LVGL fake sufficient to test navigation and input dispatch after the
  `components/ui` ↔ `main` dependency cycle is addressed. Keep pixel tests for
  leaf widgets on the real host LVGL path.
- Add direct `pattern.hpp` tests before callback integration. Keep profiling
  code hardware-tested unless a HAL shim has a clear payoff.

## Test retention rule

Delete a test only when its production code is dead. Keep tests for compiled-out
SPI code, forward-built WXCF code, and documented HAL-bound placeholders. Every
behavioral fix needs a regression test that fails on the pre-fix code, or a
brief commit note explaining why the property cannot be host-tested.

## Related

- [Testing guide](testing_guide.md)
- [Backlog](backlog.md)
- [Daisy real-time guide](daisy_rt_audio_coding_guide.md)
