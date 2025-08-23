# Inter-MCU Communication Refactoring

## Overview

The `inter_mcu` system has been refactored from a monolithic 928-line file into a modular, maintainable architecture. This refactoring improves code organization, testability, and future extensibility.

## New Architecture

### Directory Structure
```
firmware/esp32/main/
├── inter_mcu.h                 # Main interface (simplified)
├── inter_mcu.cpp               # High-level coordination only
├── comm/
│   ├── link_manager.h/cpp      # Link selection and management
│   ├── packet_processor.h/cpp  # Packet parsing and handling
│   ├── statistics.h/cpp        # Statistics tracking
│   └── listeners.h/cpp         # Callback management
├── links/
│   ├── uart_link.h/cpp         # UART-specific implementation
│   └── spi_link.h/cpp          # SPI-specific implementation (existing)
└── tasks/
    ├── rx_task.h/cpp           # RX task implementation
    └── tx_task.h/cpp           # TX task implementation
```

### Component Responsibilities

#### 1. LinkManager (`comm/link_manager.h/cpp`)
- **Purpose**: Manages link selection (SPI vs UART) and coordinates communication
- **Key Features**:
  - Singleton pattern for global access
  - Automatic link initialization based on configuration
  - Delegates all communication to the active link
- **Usage**: Automatically managed by the system

#### 2. PacketProcessor (`comm/packet_processor.h/cpp`)
- **Purpose**: Handles incoming packet parsing and routing
- **Key Features**:
  - Frame synchronization and validation
  - Message type routing
  - Integration with listeners and statistics
- **Usage**: Automatically processes received data

#### 3. StatisticsManager (`comm/statistics.h/cpp`)
- **Purpose**: Tracks all communication statistics
- **Key Features**:
  - Packet counting by type
  - TX message tracking
  - Backend heartbeat monitoring
  - Thread-safe operations
- **Usage**: Provides statistics through public API functions

#### 4. ListenersManager (`comm/listeners.h/cpp`)
- **Purpose**: Manages callback registration and invocation
- **Key Features**:
  - Meter data callbacks
  - Wave chunk callbacks
  - Null-safe callback invocation
- **Usage**: Register callbacks through `inter_mcu_set_*_listener()`

#### 5. UartLink (`links/uart_link.h/cpp`)
- **Purpose**: Implements UART communication
- **Key Features**:
  - Implements `ILink` interface
  - UART configuration and management
  - Task-based RX/TX handling
  - Protocol packet creation
- **Usage**: Automatically selected when UART is configured

#### 6. RxTask (`tasks/rx_task.h/cpp`)
- **Purpose**: Manages incoming data reception
- **Key Features**:
  - Continuous UART reading
  - Data forwarding to packet processor
  - Status logging and recovery
- **Usage**: Automatically started with the system

#### 7. TxTask (`tasks/tx_task.h/cpp`)
- **Purpose**: Manages outgoing message transmission
- **Key Features**:
  - Message queue management
  - Periodic ping/test messages
  - UART writing coordination
- **Usage**: Automatically started with the system

## Migration Guide

### From Old to New System

#### 1. Replace Header Files
```cpp
// Old
#include "inter_mcu.h"

// New (same interface, different implementation)
#include "inter_mcu_refactored.h"
```

#### 2. Update CMakeLists.txt
```cmake
# Old
idf_component_register(
    SRCS "inter_mcu.cpp"
    # ...
)

# New
idf_component_register(
    SRCS 
        "inter_mcu_refactored.cpp"
        "comm/link_manager.cpp"
        "comm/packet_processor.cpp"
        "comm/statistics.cpp"
        "comm/listeners.cpp"
        "links/uart_link.cpp"
        "tasks/rx_task.cpp"
        "tasks/tx_task.cpp"
        # ...
    INCLUDE_DIRS 
        "."
        "comm"
        "links"
        "tasks"
        # ...
)
```

#### 3. Build and Test
```bash
# Clean build to ensure all new files are included
idf.py clean
idf.py build
```

## Benefits of Refactoring

### ✅ **Improved Maintainability**
- Single responsibility principle: each class has one job
- Clear separation of concerns
- Easier to locate and fix bugs

### ✅ **Better Testability**
- Individual components can be unit tested
- Mock interfaces for testing
- Isolated functionality testing

### ✅ **Enhanced Extensibility**
- Easy to add new link types (e.g., I2C, CAN)
- Simple to add new packet types
- Modular feature additions

### ✅ **Cleaner Code**
- Smaller, focused functions
- Reduced cognitive load
- Better readability

### ✅ **Future-Proof Design**
- Interface-based design allows easy swapping
- Configuration-driven link selection
- Scalable architecture

## Configuration

### Link Selection
The system automatically selects the appropriate link based on `link_config.h`:

```cpp
// Set to 1 to use SPI, 0 to use UART
#define WAVEX_USE_SPI_LINK 1
```

### UART Configuration
UART settings are defined in `uart_link.h`:

```cpp
static const int UART_NUM = 1;        // UART_NUM_1
static const int TX_PIN = 17;         // GPIO17
static const int RX_PIN = 18;         // GPIO18
static const int BAUD_RATE = 460800;  // Baud rate
static const int BUFFER_SIZE = 512;   // Buffer size
```

## Troubleshooting

### Common Issues

#### 1. Compilation Errors
- Ensure all new source files are included in CMakeLists.txt
- Check include paths are correct
- Verify ESP-IDF version compatibility

#### 2. Runtime Issues
- Check link configuration in `link_config.h`
- Verify UART pin assignments
- Monitor serial output for initialization messages

#### 3. Performance Issues
- Adjust task priorities if needed
- Monitor memory usage
- Check UART buffer sizes

## Future Enhancements

### Planned Improvements
1. **SPI Link Refactoring**: Create `SpiLink` class implementing `ILink`
2. **Error Handling**: Enhanced error reporting and recovery
3. **Configuration**: Runtime link configuration
4. **Monitoring**: Real-time performance metrics
5. **Testing**: Comprehensive unit test suite

### Extension Points
1. **New Link Types**: Add I2C, CAN, or other protocols
2. **Packet Types**: Extend protocol with new message types
3. **Statistics**: Add custom metrics and monitoring
4. **Callbacks**: Support for additional event types

## Support

For issues or questions about the refactored system:
1. Check this documentation
2. Review the component interfaces
3. Examine the example usage in `inter_mcu_refactored.cpp`
4. Check build logs for compilation issues
