# ESP32 IRQ Signaling Analysis and Fixes

## **Issues Identified and Fixed**

### 1. **IRQ Edge Configuration Mismatch** ✅ FIXED
**Problem**: ESP32 was configured for falling-edge interrupts (`GPIO_INTR_NEGEDGE`) but Daisy drives the IRQ line **low** when it has data, which means the ESP32 would miss the IRQ signal.

**Solution**: Changed ESP32 IRQ configuration to rising-edge (`GPIO_INTR_POSEDGE`) so it triggers when Daisy releases the IRQ line (goes from low to high).

**File**: `firmware/esp32/main/links/spi_link.cpp`
```cpp
// Before: .intr_type = GPIO_INTR_NEGEDGE
// After:  .intr_type = GPIO_INTR_POSEDGE
```

### 2. **Missing ESP→Daisy IRQ Signaling** ✅ FIXED
**Problem**: ESP32 was not signaling Daisy when it had data to send, relying only on Daisy to initiate communication.

**Solution**: Added proper ESP→Daisy IRQ signaling:
- Drive IRQ line low when ESP32 has data to send
- Release IRQ line high after sending data
- This allows Daisy to know when ESP32 has data ready

**File**: `firmware/esp32/main/links/spi_link.cpp`
```cpp
// Signal Daisy that we have data
gpio_set_level((gpio_num_t)PIN_IRQ_ESP2DAISY, 0);
push_packet_to_daisy();
// Release IRQ line after sending
gpio_set_level((gpio_num_t)PIN_IRQ_ESP2DAISY, 1);
```

### 3. **Inefficient Polling vs IRQ-Driven Operation** ✅ FIXED
**Problem**: The link task was aggressively polling every 2ms even when no IRQ was received, wasting CPU cycles.

**Solution**: Reduced polling frequency and made the system more IRQ-driven:
- Increased IRQ timeout from 2ms to 10ms
- Only poll for TX data every 10th iteration
- Only poll for RX data every 100th iteration
- This reduces CPU overhead while maintaining responsiveness

**File**: `firmware/esp32/main/links/spi_link.cpp`
```cpp
// Before: const TickType_t tout = pdMS_TO_TICKS(2);
// After:  const TickType_t tout = pdMS_TO_TICKS(10);

// Only poll every 10th iteration for TX
if (poll_counter % 10 == 0) { ... }

// Very occasional RX poll (only every 100th iteration)
if (poll_counter % 100 == 0) { ... }
```

### 4. **Manual Packet Processing in Main Loop** ✅ FIXED
**Problem**: Main loop was manually calling `spi_link_recv()` and processing packets, bypassing the integrated packet processor system.

**Solution**: Removed manual packet processing from main loop since the link manager and packet processor handle this automatically in the background.

**File**: `firmware/esp32/main/main.cpp`
```cpp
// Before: Manual SPI packet processing with spi_link_recv()
// After:  Automatic processing by link manager and packet processor
```

## **Current IRQ Signaling Flow**

### **Daisy → ESP32 Communication**
1. Daisy has data to send
2. Daisy drives IRQ line low (asserts interrupt)
3. ESP32 detects rising edge (Daisy releasing line)
4. ESP32 processes IRQ and reads packet from Daisy
5. ESP32 sends any pending data to Daisy

### **ESP32 → Daisy Communication**
1. ESP32 has data to send
2. ESP32 drives IRQ line low (signals Daisy)
3. Daisy detects interrupt and prepares to receive
4. ESP32 sends data packet to Daisy
5. ESP32 releases IRQ line high

## **Pin Configuration Verified**

### **ESP32 Pins**
- **GPIO16 (J1-14)**: SPI2 SCLK to Daisy
- **GPIO17 (J1-10)**: SPI2 MOSI to Daisy  
- **GPIO18 (J1-11)**: SPI2 MISO from Daisy
- **GPIO19 (J1-15)**: SPI2 CS to Daisy
- **GPIO8 (J3-26)**: Daisy IRQ input (rising-edge)
- **GPIO41 (J3-14)**: ESP attention output (idle high, drive low when sending)

### **Daisy Pins**
- **D8**: SPI1_SCK (clock from ESP32)
- **D10**: SPI1_MOSI (data from ESP32)
- **D9**: SPI1_MISO (data to ESP32)
- **D7**: SPI1_NSS (chip select from ESP32)
- **D13**: IRQ output to ESP32 (push-pull, drive low when has data)
- **D14**: Attention input from ESP32

## **Performance Improvements**

### **Before Fixes**
- Aggressive 2ms polling (500Hz)
- No ESP→Daisy signaling
- Manual packet processing in main loop
- IRQ edge mismatch causing missed interrupts

### **After Fixes**
- Reduced to 10ms IRQ timeout (100Hz)
- Smart polling: TX every 100ms, RX every 1000ms
- Proper bidirectional IRQ signaling
- Automatic packet processing by link manager
- Proper IRQ edge detection

## **Testing Recommendations**

1. **Verify IRQ Detection**: Monitor ESP32 logs for "IRQ received from Daisy" messages
2. **Check Bidirectional Communication**: Ensure both Daisy→ESP32 and ESP32→Daisy work
3. **Monitor Performance**: Check CPU usage and packet latency
4. **Test Edge Cases**: Verify communication works under load and during rapid data exchange

## **Next Steps**

1. **Test the current implementation** with actual Daisy hardware
2. **Monitor packet statistics** using the built-in statistics system
3. **Verify IRQ timing** meets performance requirements
4. **Consider further optimizations** if needed (e.g., DMA improvements, interrupt priorities)

## **Files Modified**

- `firmware/esp32/main/links/spi_link.cpp` - IRQ configuration and task improvements
- `firmware/esp32/main/main.cpp` - Removed manual packet processing
- `firmware/esp32/main/comm/link_manager.h` - Added packet processing method
- `firmware/esp32/main/comm/link_manager.cpp` - Implemented packet processing
- `firmware/esp32/main/links/spi_link_wrapper.h` - Added packet processing method

The ESP32 inter-MCU link should now properly use IRQ-driven communication with efficient bidirectional signaling between ESP32 and Daisy.
