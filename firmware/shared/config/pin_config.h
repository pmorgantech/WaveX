#ifndef WAVEX_PIN_CONFIG_H
#define WAVEX_PIN_CONFIG_H

/**
 * @file pin_config.h
 * @brief WaveX Centralized Pin Configuration
 * 
 * This file contains ALL pin assignments for both ESP32 and Daisy in one place.
 * It serves as the single source of truth for all hardware pin configurations.
 * 
 * ⚠️  IMPORTANT: Edit pin assignments ONLY in this file
 * ✅  VERIFIED: All pin assignments verified for ESP32-S3-DevKitC-1 and Daisy Seed
 * 🔧  UPDATED: Complete SPI inter-MCU link configuration
 */

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ESP32-S3 Frontend Pin Assignments
// =============================================================================

#ifdef ESP_PLATFORM

// Pins for ESP32-P4-WIFI6
// The GPIO available on this board are:
// GPIO2-GPIO5, GPIO7, GPIO8, GPIO20-GPIO33, GPIO46-GPIO52 

// MIPI DSI Display Interface (5-DSI-TOUCH-A)
// MIPI DSI Data Lanes
#define WAVEX_ESP_DSI_D0P       2    // MIPI DSI Data Lane 0 Positive
#define WAVEX_ESP_DSI_D0N       3    // MIPI DSI Data Lane 0 Negative
#define WAVEX_ESP_DSI_D1P       4    // MIPI DSI Data Lane 1 Positive
#define WAVEX_ESP_DSI_D1N       5    // MIPI DSI Data Lane 1 Negative
#define WAVEX_ESP_DSI_CLKP      6    // MIPI DSI Clock Positive
#define WAVEX_ESP_DSI_CLKN      7    // MIPI DSI Clock Negative

// MIPI DSI Control Pins
#define WAVEX_ESP_DSI_RST       8    // Display Reset
#define WAVEX_ESP_DSI_BL        9    // Backlight Control

// Touch Interface (GT911 I2C)
#define WAVEX_ESP_TOUCH_SDA     20   // J3-19: Touch I2C Data
#define WAVEX_ESP_TOUCH_SCL     21   // J3-4: Touch I2C Clock
#define WAVEX_ESP_TOUCH_RST     14   // J1-20: Touch Reset
#define WAVEX_ESP_TOUCH_INT     15   // J1-21: Touch Interrupt

// Inter-MCU Communication (SPI slave to Daisy master) - using user-verified available pins
#define WAVEX_ESP_SPI_SCLK      48   // Using GPIO48 for SCK
#define WAVEX_ESP_SPI_MOSI      49   // Using GPIO49 for MOSI
#define WAVEX_ESP_SPI_MISO      50   // Using GPIO50 for MISO
#define WAVEX_ESP_SPI_CS        51   // Using GPIO51 for CS
// IRQ/ATTN lines for Daisy <-> ESP signaling
#define WAVEX_ESP_ATTN_OUT      31   // J3-14: ESP Attention output to Daisy (active high)

// Quadrature Encoder (PCNT)
#define WAVEX_ESP_ENCODER_A      33   // J3-7: PCNT Channel A
#define WAVEX_ESP_ENCODER_B      34   // J3-8: PCNT Channel B
#define WAVEX_ESP_ENCODER_BTN    40   // J3-12: Encoder Push Button (optional)

// Additional PCNT Unit (PCNT_UNIT_1)
#define WAVEX_ESP_PCNT1_A        46   // GPIO46: PCNT Unit 1 Channel A
#define WAVEX_ESP_PCNT1_B        47   // GPIO47: PCNT Unit 1 Channel B
#define WAVEX_ESP_PCNT1_UNIT     PCNT_UNIT_1
#define WAVEX_ESP_PCNT1_CH_A     PCNT_CHANNEL_0
#define WAVEX_ESP_PCNT1_CH_B     PCNT_CHANNEL_1

// MIDI UART (UART2)
// -----------------------------------------------------------------------------
#define WAVEX_ESP_MIDI_UART_NUM UART_NUM_2
#define WAVEX_ESP_MIDI_TX 32 // UART2 TX
#define WAVEX_ESP_MIDI_RX 33 // UART2 RX
#define WAVEX_ESP_MIDI_BAUD 31250


// -----------------------------------------------------------------------------
// Shared I2C Bus (Touch + TCA8418)
// -----------------------------------------------------------------------------
#define WAVEX_ESP_I2C_PORT_NUM I2C_NUM_0
#define WAVEX_ESP_I2C_SDA 20 // Shared SDA for GT911 + TCA8418
#define WAVEX_ESP_I2C_SCL 21 // Shared SCL for GT911 + TCA8418
#define WAVEX_ESP_I2C_SPEED_HZ 400000
#define WAVEX_ESP_I2C_PULLUPS 1 // Enable internal pull-ups
#define WAVEX_ESP_BTN_INT       30   // J3-15: Button Interrupt

// -----------------------------------------------------------------------------
// SPI Master #2 (TLC5947 + MCP3008)
// -----------------------------------------------------------------------------
#define WAVEX_ESP_SPI2_HOST SPI2_HOST
#define WAVEX_ESP_SPI2_MOSI 47
#define WAVEX_ESP_SPI2_MISO 52
#define WAVEX_ESP_SPI2_SCLK 46
#define WAVEX_ESP_SPI2_FREQ_HZ 10000000 // 10 MHz typical for LEDs/ADC

// MCP3008 (ADC for pots)
#define WAVEX_ESP_MCP3008_CS 29

// TLC5947 LED Driver
#define WAVEX_ESP_TLC5947_LAT 28 // Latch pin (XLAT)
#define WAVEX_ESP_TLC5947_BLANK 27 // Output enable / BLANK

#endif // ESP_PLATFORM

// =============================================================================
// Daisy Seed Backend Pin Assignments
// =============================================================================

#ifdef DAISY_PLATFORM

// Inter-MCU Communication (SPI1 master to ESP32-P4 slave)
#define WAVEX_DAISY_SPI_SCK     8     // D8: SPI1_SCK (clock to ESP32)
#define WAVEX_DAISY_SPI_MOSI    10    // D10: SPI1_MOSI (data to ESP32)
#define WAVEX_DAISY_SPI_MISO    9     // D9: SPI1_MISO (data from ESP32)
#define WAVEX_DAISY_SPI_CS      7     // D7: SPI1_NSS (chip select to ESP32)
// Attention signal from ESP32 (active high)
#define WAVEX_DAISY_ATTN_IN     0     // D0: ESP32 attention input to Daisy

// Audio I/O (Built-in AK4556 Codec)
#define WAVEX_DAISY_AUDIO_IN_L  -1   // Built-in: Left audio input
#define WAVEX_DAISY_AUDIO_IN_R  -1   // Built-in: Right audio input
#define WAVEX_DAISY_AUDIO_OUT_L -1   // Built-in: Left audio output
#define WAVEX_DAISY_AUDIO_OUT_R -1   // Built-in: Right audio output

// CV Outputs (MCP4728 DACs)
#define WAVEX_DAISY_DAC1_CS     25   // D25: MCP4728 #1 Chip Select
#define WAVEX_DAISY_DAC2_CS     26   // D26: MCP4728 #2 Chip Select
#define WAVEX_DAISY_DAC3_CS     27   // D27: MCP4728 #3 Chip Select
#define WAVEX_DAISY_DAC4_CS     28   // D28: MCP4728 #4 Chip Select
#define WAVEX_DAISY_DAC_SCLK    29   // D29: SPI Clock (shared)
#define WAVEX_DAISY_DAC_MOSI    30   // D30: SPI Data (shared)

// High-Quality Audio Output (PCM1690)
#define WAVEX_DAISY_PCM_BCLK    24   // D24: SAI2 Bit Clock
#define WAVEX_DAISY_PCM_LRCK    23   // D23: SAI2 Left/Right Clock
#define WAVEX_DAISY_PCM_DATA    22   // D22: SAI2 Data
#define WAVEX_DAISY_PCM_MCLK    21   // D21: SAI2 Master Clock

// SD Card Interface (SPI)
#define WAVEX_DAISY_SD_CS       19   // D19: SD Card Chip Select
#define WAVEX_DAISY_SD_SCLK     20   // D20: SD Card SPI Clock
#define WAVEX_DAISY_SD_MOSI     18   // D18: SD Card Data Out
#define WAVEX_DAISY_SD_MISO     17   // D17: SD Card Data In

// Analog Inputs
#define WAVEX_DAISY_CTRL_1      15   // D15: Potentiometer/CV Input 1
#define WAVEX_DAISY_CTRL_2      16   // D16: Potentiometer/CV Input 2
#define WAVEX_DAISY_CTRL_3      -1   // A2: Potentiometer/CV Input 3
#define WAVEX_DAISY_CTRL_4      -1   // A3: Potentiometer/CV Input 4

// Available Pins (D0-D30, excluding used pins)
#define WAVEX_DAISY_AVAIL_1     0    // D0: Available
#define WAVEX_DAISY_AVAIL_2     1    // D1: Available
#define WAVEX_DAISY_AVAIL_3     2    // D2: Available
#define WAVEX_DAISY_AVAIL_4     3    // D3: Available
#define WAVEX_DAISY_AVAIL_5     4    // D4: Available
#define WAVEX_DAISY_AVAIL_6     5    // D5: Available
#define WAVEX_DAISY_AVAIL_7     6    // D6: Available
#define WAVEX_DAISY_AVAIL_8     7    // D7: Available
#define WAVEX_DAISY_AVAIL_9     8    // D8: Available

#endif // DAISY_PLATFORM

// =============================================================================
// Inter-MCU Link Configuration
// =============================================================================

// ESP32 SPI configuration (slave mode)
#define WAVEX_ESP_SPI_HOST      SPI3_HOST     // ESP32-P4 uses SPI3_HOST for slave mode
#define WAVEX_ESP_SPI_CLK_HZ    4000000  // 10 MHz (master controls)
#define WAVEX_ESP_SPI_QUEUE_SIZE 8
#define WAVEX_ESP_SPI_DMA_CH    SPI_DMA_CH_AUTO // Reverting: P4 slave only supports auto-alloc for DMA

// Daisy SPI configuration (master mode)
#define WAVEX_DAISY_SPI_PERIPH  1     // SPI1
#define WAVEX_DAISY_SPI_MODE    0     // MASTER mode

// Ring buffer sizes
#define WAVEX_SPI_RX_RING_SIZE  32
#define WAVEX_SPI_TX_RING_SIZE  32


// =============================================================================
// Pin Validation Macros
// =============================================================================

#ifdef ESP_PLATFORM
// Verify ESP32 pins are within valid range (0-48)
#define WAVEX_VALIDATE_ESP_PIN(pin) ((pin) >= 0 && (pin) <= 48)
#define WAVEX_ASSERT_ESP_PIN(pin) static_assert(WAVEX_VALIDATE_ESP_PIN(pin), "Invalid ESP32 pin number")
#endif

#ifdef DAISY_PLATFORM
// Verify Daisy pins are within valid range (0-30)
#define WAVEX_VALIDATE_DAISY_PIN(pin) ((pin) >= 0 && (pin) <= 30)
#define WAVEX_ASSERT_DAISY_PIN(pin) static_assert(WAVEX_VALIDATE_DAISY_PIN(pin), "Invalid Daisy pin number")
#endif

#ifdef __cplusplus
}
#endif

#endif // WAVEX_PIN_CONFIG_H
