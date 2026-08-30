# WaveX Testing Guide

This document provides guidance on writing, running, and maintaining tests for the WaveX firmware project.

## Overview

WaveX uses GoogleTest for unit testing across all platforms:
- **Daisy Seed**: Host-based tests using GoogleTest (via libDaisy)
- **ESP32-P4**: Host-based tests using GoogleTest with ESP-IDF mocks
- **Shared Protocol**: Cross-platform protocol validation tests

## Test Structure

```
firmware/
├── daisy/tests/
│   ├── unit/          # Unit tests for Daisy components
│   ├── integration/   # Integration tests
│   ├── mocks/         # Hardware mocks
│   ├── utils/         # Test utilities
│   └── CMakeLists.txt
├── esp32/tests/
│   ├── unit/          # Unit tests for ESP32 components
│   ├── integration/   # Integration tests
│   ├── mocks/         # ESP-IDF API mocks
│   ├── utils/         # Test utilities
│   └── CMakeLists.txt
└── shared/tests/
    ├── protocol/      # Protocol validation tests
    ├── midi/          # MIDI parsing/forwarding tests
    ├── wav/           # WAV file parsing tests
    ├── wxcf/          # WXCF chunk-container tests
    ├── integration/   # Inter-MCU integration tests
    ├── utils/         # Shared test utilities
    └── CMakeLists.txt
```

## Running Tests

### Run All Tests
```bash
make test
```

### Run All Tests Under Sanitizers
```bash
make test-asan
```

Same test bodies, built with AddressSanitizer + UndefinedBehaviorSanitizer
into separate `build-asan/` directories. **This finds a class of defect the
ordinary run cannot**: an out-of-bounds read returns a plausible value, so
value assertions still pass and the suite runs over the bug without noticing.
Two of the August 2026 audit findings were exactly that shape. Run it before
sending anything that touches buffer indexing, payload parsing, or DSP read
positions. It also runs in CI.

Individual suites can be built the same way with
`cmake -DWAVEX_TEST_SANITIZE=address ..` (or `=thread`).

### Run Tests for Specific Platform
```bash
make test-daisy      # Daisy Seed tests
make test-esp32      # ESP32 tests
make test-shared     # Shared protocol tests
```

### Run Tests from Individual Directories
```bash
# Daisy tests (firmware/daisy/Makefile has no `test` target - build the
# CMake test tree directly, same pattern as ESP32/shared below)
cd firmware/daisy/tests/build
cmake ..
make -j$(nproc)
ctest --output-on-failure

# ESP32 tests
cd firmware/esp32/tests/build
cmake ..
make -j$(nproc)
ctest --output-on-failure

# Shared tests
cd firmware/shared/tests/build
cmake ..
make -j$(nproc)
ctest --output-on-failure
```

### Clean Test Builds
```bash
make test-clean
```

## Writing Tests

### Test File Naming

Test files should follow the pattern: `<component>_test.cpp`

Examples:
- `uart_protocol_test.cpp`
- `packet_router_test.cpp`
- `audio_engine_test.cpp`

### Basic Test Structure

```cpp
#include <gtest/gtest.h>
#include "component_under_test.h"

class ComponentTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize test fixtures
    }

    void TearDown() override {
        // Cleanup test fixtures
    }

    // Test helper methods and member variables
};

TEST_F(ComponentTest, TestName) {
    // Arrange
    // Act
    // Assert
    EXPECT_EQ(expected, actual);
}
```

### Test Categories

#### Unit Tests
- Test individual components in isolation
- Use mocks for hardware dependencies
- Fast execution (< 1 second per test)
- Located in `tests/unit/`

#### Integration Tests
- Test component interactions
- May use simulated hardware
- Longer execution time acceptable
- Located in `tests/integration/`

### Using Test Helpers

Test helpers are available in `tests/utils/test_helpers.h`:

```cpp
#include "../utils/test_helpers.h"

using namespace WaveX::Test;

// Create test packets
auto packet = PacketGenerator::CreateControlChangePacket(0x01, 0, 0x7FFF);

// Generate audio buffers
auto sine_wave = AudioBufferGenerator::GenerateSineWave(1024, 440.0f, 48000.0f);

// Create filesystem fixtures
FilesystemFixture::CreateTestDirectoryStructure("/tmp/test");
```

### Mocking Hardware

#### ESP32 Mocks

ESP-IDF APIs are mocked in `firmware/esp32/tests/mocks/esp32_mocks.h`:

```cpp
#include "../mocks/esp32_mocks.h"

// FreeRTOS queues, tasks, semaphores are automatically mocked
QueueHandle_t queue = xQueueCreate(10, sizeof(int));
xQueueSend(queue, &data, 0);
```

#### Daisy Mocks

Daisy hardware mocks live in `firmware/daisy/tests/mocks/`: `daisy_mocks.h`,
`daisy_seed.h`, `dispatch_mocks.cpp/.h`, `fatfs_mock.cpp/.h`, `ff.h`,
`log_ring_mock.cpp`. `DISABLED_` is only still used for the handful of tests
that genuinely need real hardware timing (e.g. `daisy_uart_link_test.cpp`,
`audio_engine_test.cpp`), not as the general pattern for hardware-dependent code —
most of it is mocked and runs on the host.

## Writing a regression test for a fix

A fix's test must **fail against the pre-fix code**. That is the only thing
separating a regression test from a test that happens to pass, and it is cheap
to check:

```bash
git show <fix-sha>^:path/to/file.hpp > path/to/file.hpp   # restore pre-fix
# build + run the new test -> confirm it FAILS
git checkout path/to/file.hpp                             # restore
```

If the defect cannot be caught this way (HAL-bound code, an excluded
translation unit, or a property like "no `lv_*` call happens off the LVGL
task"), say so in the commit message rather than writing a test that passes
either way. `docs/testing-remediation.md` records which of the August 2026
defects fall into that category and why.

## Test Coverage Goals

- **Core Components**: >80% code coverage
  - Protocol handlers
  - Message routing
  - Audio processing logic
  - Storage operations

- **Hardware Abstraction**: >60% code coverage
  - UART/SPI link implementations
  - Hardware initialization
  - DMA handling

- **Integration**: Critical paths covered
  - Inter-MCU communication
  - End-to-end message flow
  - Error recovery

## Best Practices

### 1. Test Independence
- Each test should be independent
- Don't rely on test execution order
- Clean up resources in `TearDown()`

### 2. Descriptive Test Names
```cpp
// Good
TEST_F(PacketRouterTest, RouteHeartbeatMessage)
TEST_F(UartProtocolTest, ValidateFrameInvalidCRC)

// Bad
TEST_F(PacketRouterTest, Test1)
TEST_F(UartProtocolTest, Test)
```

### 3. Arrange-Act-Assert Pattern
```cpp
TEST_F(ComponentTest, Example) {
    // Arrange: Set up test data
    auto packet = CreateTestPacket();

    // Act: Execute the code under test
    bool result = ProcessPacket(packet);

    // Assert: Verify the results
    EXPECT_TRUE(result);
}
```

### 4. Test Edge Cases
- Empty/null inputs
- Maximum size inputs
- Boundary conditions
- Error conditions

### 5. Avoid Testing Implementation Details
- Test public interfaces
- Test behavior, not implementation
- Focus on what, not how

## Debugging Tests

### Run Single Test
```bash
cd firmware/shared/tests/build
./bin/uart_protocol_test --gtest_filter=UartProtocolTest.CreatePacketEmptyPayload
```

### Verbose Output
```bash
ctest --output-on-failure --verbose
```

### Debug with GDB
```bash
cd firmware/shared/tests/build
gdb ./bin/uart_protocol_test
(gdb) run --gtest_filter=UartProtocolTest.*
```

## Continuous Integration

Tests should run automatically in CI/CD:

```yaml
# Example GitHub Actions workflow
- name: Run Tests
  run: |
    make test-all
```

## Test Maintenance

### Adding New Tests

1. Create test file: `tests/unit/<component>_test.cpp`
2. Add test cases following existing patterns
3. Update CMakeLists.txt if needed (auto-discovery should handle it)
4. Run tests: `make test`
5. Verify coverage meets goals

### Updating Tests

- Update tests when interfaces change
- Keep tests in sync with implementation
- Remove obsolete tests
- Refactor tests for clarity

## Common Issues

### Tests Fail After Code Changes
- Update test expectations
- Check if interface changed
- Verify mocks are still valid

### Tests Pass But Code Doesn't Work
- Check test coverage
- Verify tests actually test the code
- Add integration tests

### Hardware-Dependent Tests
- Use mocks for unit tests
- Create hardware-in-the-loop tests separately
- Mark hardware tests with `DISABLED_` prefix until mocks available

## Resources

- [GoogleTest Documentation](https://google.github.io/googletest/)
- [libDaisy Testing Guide](firmware/daisy/libs/libDaisy/doc/md/_b1_Development-Unit-Testing.md)
- [ESP-IDF Testing](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-python-scripts.html#unit-testing)

## Test Status

- ✅ UART Protocol, Message Type, Packet Router tests: Complete
- ✅ Daisy Component Tests: substantial coverage under
  `firmware/daisy/tests/unit/{audio,comm,cv,sequencer,storage}/`
- ✅ ESP32 Component Tests: substantial coverage under
  `firmware/esp32/tests/unit/{comm,ui}/`
- ⏳ Integration Tests: only `firmware/esp32/tests/integration/inter_mcu_protocol_test.cpp`
  has content; `firmware/shared/tests/integration/` and
  `firmware/daisy/tests/integration/` are still empty

`docs/archive/testing_strategy.md` predates this test suite and describes
results for tests that were never run — do not use it as a current
reference (see `docs/README.md`'s archive notes).

### Known gaps

Two Daisy test files are **excluded from the build** by
`firmware/daisy/tests/CMakeLists.txt`, and their contents are `DISABLED_`
besides: `audio_engine_test.cpp` (3 tests) and `daisy_uart_link_test.cpp`
(5). Both need STM32 HAL headers. Treat them as placeholders, not coverage —
`audio_engine.cpp` is 3476 lines with no host tests at all, which is why
several of the August 2026 audit defects in it have no regression test.

The remediation plan for this and the other gaps, with the defect classes it
is organised around, is [`testing-remediation.md`](testing-remediation.md).
