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
    ├── integration/   # Inter-MCU integration tests
    ├── utils/         # Shared test utilities
    └── CMakeLists.txt
```

## Running Tests

### Run All Tests
```bash
make test
```

### Run Tests for Specific Platform
```bash
make test-daisy      # Daisy Seed tests
make test-esp32      # ESP32 tests
make test-shared     # Shared protocol tests
```

### Run Tests from Individual Directories
```bash
# Daisy tests
cd firmware/daisy
make test

# ESP32 tests
cd firmware/esp32/tests/build
cmake ..
make
ctest --output-on-failure

# Shared tests
cd firmware/shared/tests/build
cmake ..
make
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

Daisy hardware mocks should be created in `firmware/daisy/tests/mocks/`:

```cpp
// TODO: Implement libDaisy hardware mocks
// For now, hardware-dependent tests are disabled with DISABLED_ prefix
TEST_F(DaisyComponentTest, DISABLED_HardwareTest) {
    // Requires hardware mocking
}
```

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

Current test coverage:
- ✅ UART Protocol Tests: Complete
- ✅ Message Type Tests: Complete
- ✅ Packet Router Tests: Complete
- ⏳ Daisy Component Tests: In Progress (require hardware mocks)
- ⏳ ESP32 Component Tests: In Progress
- ⏳ Integration Tests: Planned

See `docs/testing_strategy.md` for detailed test plan and objectives.
