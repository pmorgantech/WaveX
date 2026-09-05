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
 */

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ESP32-P4 Frontend Pin Assignments
// =============================================================================

#ifdef ESP_PLATFORM

// Board: Waveshare ESP32-P4-WIFI6. Its two 2x20 headers expose exactly 27
// GPIOs (Waveshare pin-definition diagram, re-checked 2026-09-05):
//   GPIO2-5, GPIO7, GPIO8, GPIO20-33, GPIO46-52
// Nothing else on the chip is reachable: MIPI-DSI/CSI, the USB 2.0 HS pair,
// the TF-card SDMMC pads (GPIO39-45) and the ESP32-C6 SDIO are on-board nets.
// A pin that is not in that list cannot be wired, whatever a define says -
// the previous revision of this section carried GPIO14/15/34/40, none of
// which exist on the header (it was written for an ESP32-S3 DevKit).
//
// Not GPIO, and not in this file:
//   - GPIO7/GPIO8 are the BSP's I2C bus (SDA/SCL, waveshare__esp32_p4_nano):
//     GT911 touch via the display FPC and the TCA8418 keypad via the header.
//     The code takes bsp_i2c_get_handle(); there is no WaveX-owned I2C bus.
//   - GPIO24/GPIO25 are USB D-/D+ of the USB-Serial/JTAG controller: the
//     flash and debug port. Never claim them as GPIO (see the DIN MIDI note
//     in hardware_config.h for what happened when UART2 RX sat on GPIO24).
//   - USB MIDI runs on the USB 2.0 High-Speed OTG controller, whose PHY has
//     dedicated pins - the board's 4-pin "V D- D+ G" USB connector. It shares
//     nothing with GPIO24/25.
//   - GPIO26/GPIO27 are the second full-speed USB transceiver (USB 1.1 OTG
//     default pair). Left unassigned on purpose: a USB *host* port (USB stick
//     sample import, a USB MIDI controller) would need them. Do not spend
//     them on GPIO.
// The panel allocation below is the plan in docs/features/panel-controls.md;
// what is wired on the bench today is only the inter-MCU UART, the I2C
// keypad, and the PCNT encoders.

// Inter-MCU UART (UART1) - control link to Daisy, the live transport
#define WAVEX_ESP_UART_INTER_NUM UART_NUM_1
#define WAVEX_ESP_UART_INTER_TX 22
#define WAVEX_ESP_UART_INTER_RX 23
#define WAVEX_ESP_UART_INTER_BAUD 2000000
#define WAVEX_ESP_UART_INTER_BUF_SIZE 2048

// Inter-MCU SPI (SPI3 slave to Daisy master) + ATTN. Compiled out
// (WAVEX_SPI_LINK_ENABLED=0, link_config.h); the pins stay reserved until
// the SPI revival decision in backlog.md is made one way or the other.
#define WAVEX_ESP_SPI_SCLK 48
#define WAVEX_ESP_SPI_MOSI 49
#define WAVEX_ESP_SPI_MISO 50
#define WAVEX_ESP_SPI_CS 51
#define WAVEX_ESP_ATTN_OUT 31  // ESP attention output to Daisy (active high)

// Quadrature encoders (PCNT, 4x decode, PEC11R with detents). Two units;
// the push switches go into the TCA8418 matrix, not GPIO.
//
// Unit 1 is the encoder on the bench (confirmed 2026-09-05): GPIO46/47.
// Its A phase is on GPIO47 - with A on 46 the count ran negative on a
// clockwise turn, so clockwise arrived at pages as EncoderDown and every
// page honouring the "clockwise is positive" contract (InputEvent::steps())
// ran backwards. Swapping the two here is the wiring fact; pages must not
// compensate.
//
// Unit 0 has nothing wired. Its channel B was GPIO34, which is not on the
// header (and is a boot strapping pin) - moved to GPIO32 so the pair sits on
// adjacent header pins (32/33), like unit 1.
#define WAVEX_ESP_ENCODER_A 33  // PCNT unit 0 channel A
#define WAVEX_ESP_ENCODER_B 32  // PCNT unit 0 channel B
#define WAVEX_ESP_PCNT1_A 47    // PCNT unit 1 channel A
#define WAVEX_ESP_PCNT1_B 46    // PCNT unit 1 channel B

// TCA8418 keypad interrupt (active low, open-drain on the part). Configured
// as an input today but not yet used to wake the keypad task.
#define WAVEX_ESP_BTN_INT 30

// DIN MIDI (UART2, 31250 baud). RX was GPIO24 (USB-Serial/JTAG D-) and TX
// GPIO32; both moved 2026-09-05 to an adjacent header pair. RX needs the
// optocoupler receiver on the new pin before WAVEX_ESP_DIN_MIDI_ENABLED is
// turned back on.
#define WAVEX_ESP_MIDI_UART_NUM UART_NUM_2
#define WAVEX_ESP_MIDI_RX 20
#define WAVEX_ESP_MIDI_TX 21
#define WAVEX_ESP_MIDI_BAUD 31250

// SPI2 master: TLC5947 LED chain + MCP3008 ADC (endless pots). Was 46/47/52,
// colliding with PCNT unit 1 on 46/47; moved to the adjacent header run
// GPIO2-5. The MCP3008 has a chip select; the TLC5947 does not - it is a
// shift register that latches whatever was clocked in when XLAT pulses, so
// every MCP3008 transaction also shifts garbage through it and the LED frame
// must always be re-sent in full before XLAT. Per-device clocks: the
// TLC5947 takes up to 30 MHz, the MCP3008 about 2 MHz at 3.3 V.
#define WAVEX_ESP_SPI2_HOST SPI2_HOST
#define WAVEX_ESP_SPI2_SCLK 2
#define WAVEX_ESP_SPI2_MOSI 3  // TLC5947 SIN + MCP3008 DIN
#define WAVEX_ESP_SPI2_MISO 4  // MCP3008 DOUT
#define WAVEX_ESP_SPI2_FREQ_HZ 10000000

#define WAVEX_ESP_MCP3008_CS 5  // ADC #0 (8 channels = 4 endless pots)

// TLC5947: XLAT latches the frame; BLANK high forces all outputs off. Wire a
// pull-up on BLANK so the LEDs stay dark from power-on until the first frame
// is latched, instead of showing the shift register's random contents.
#define WAVEX_ESP_TLC5947_LAT 28
#define WAVEX_ESP_TLC5947_BLANK 29

// Unassigned header GPIO: 52 (earmarked: chip select for a second MCP3008
// if more than four endless pots or any plain pots are added). GPIO26/27 are
// reserved for USB, see above.

#endif  // ESP_PLATFORM

// =============================================================================
// Daisy Seed Backend Pin Assignments
// =============================================================================

#ifdef DAISY_PLATFORM

// Inter-MCU Communication (SPI1 master to ESP32-P4 slave)
#define WAVEX_DAISY_SPI_SCK 8    // D8: SPI1_SCK (clock to ESP32)
#define WAVEX_DAISY_SPI_MOSI 10  // D10: SPI1_MOSI (data to ESP32)
#define WAVEX_DAISY_SPI_MISO 9   // D9: SPI1_MISO (data from ESP32)
#define WAVEX_DAISY_SPI_CS 7     // D7: SPI1_NSS (chip select to ESP32)
// Attention signal from ESP32 (active high)
#define WAVEX_DAISY_ATTN_IN 0  // D0: ESP32 attention input to Daisy

// Inter-MCU UART (UART4) - control link from Daisy
#define WAVEX_DAISY_UART_INTER_PERIPH 3  // matches daisy::UartHandler::Config::Peripheral::UART_4
#define WAVEX_DAISY_UART_INTER_TX 12     // D12: UART4_TX (PORTB9)
#define WAVEX_DAISY_UART_INTER_RX 11     // D11: UART4_RX (PORTB8)
#define WAVEX_DAISY_UART_INTER_BAUD 2000000
#define WAVEX_DAISY_UART_INTER_BUF_SIZE 2048

// Audio I/O (Built-in AK4556 Codec)
#define WAVEX_DAISY_AUDIO_IN_L -1   // Built-in: Left audio input
#define WAVEX_DAISY_AUDIO_IN_R -1   // Built-in: Right audio input
#define WAVEX_DAISY_AUDIO_OUT_L -1  // Built-in: Left audio output
#define WAVEX_DAISY_AUDIO_OUT_R -1  // Built-in: Right audio output

// CV Outputs (MCP48CMB28 DACs)
#define WAVEX_DAISY_DAC1_CS 25   // D25: MCP48CMB28 #1 Chip Select
#define WAVEX_DAISY_DAC2_CS 26   // D26: MCP48CMB28 #2 Chip Select
#define WAVEX_DAISY_DAC3_CS 27   // D27: MCP48CMB28 #3 Chip Select
#define WAVEX_DAISY_DAC4_CS 28   // D28: MCP48CMB28 #4 Chip Select
#define WAVEX_DAISY_DAC_SCLK 29  // D29: SPI Clock (shared)
#define WAVEX_DAISY_DAC_MOSI 30  // D30: SPI Data (shared)

// High-Quality Audio Output (PCM1690)
#define WAVEX_DAISY_PCM_BCLK 24  // D24: SAI2 Bit Clock
#define WAVEX_DAISY_PCM_LRCK 23  // D23: SAI2 Left/Right Clock
#define WAVEX_DAISY_PCM_DATA 22  // D22: SAI2 Data
#define WAVEX_DAISY_PCM_MCLK 21  // D21: SAI2 Master Clock

// SD Card Interface (SPI)
#define WAVEX_DAISY_SD_CS 19    // D19: SD Card Chip Select
#define WAVEX_DAISY_SD_SCLK 20  // D20: SD Card SPI Clock
#define WAVEX_DAISY_SD_MOSI 18  // D18: SD Card Data Out
#define WAVEX_DAISY_SD_MISO 17  // D17: SD Card Data In

// Analog Inputs
#define WAVEX_DAISY_CTRL_1 15  // D15: Potentiometer/CV Input 1
#define WAVEX_DAISY_CTRL_2 16  // D16: Potentiometer/CV Input 2
#define WAVEX_DAISY_CTRL_3 -1  // A2: Potentiometer/CV Input 3
#define WAVEX_DAISY_CTRL_4 -1  // A3: Potentiometer/CV Input 4

// Bookkeeping list of unclaimed Daisy pins. Stale and consumed by nothing:
// it overlaps assignments above (D0 = ATTN_IN, D7/D8 = SPI CS/SCK). Trust
// the sections above, not this list.
#define WAVEX_DAISY_AVAIL_1 0  // D0: Available
#define WAVEX_DAISY_AVAIL_2 1  // D1: Available
#define WAVEX_DAISY_AVAIL_3 2  // D2: Available
#define WAVEX_DAISY_AVAIL_4 3  // D3: Available
#define WAVEX_DAISY_AVAIL_5 4  // D4: Available
#define WAVEX_DAISY_AVAIL_6 5  // D5: Available
#define WAVEX_DAISY_AVAIL_7 6  // D6: Available
#define WAVEX_DAISY_AVAIL_8 7  // D7: Available
#define WAVEX_DAISY_AVAIL_9 8  // D8: Available

#endif  // DAISY_PLATFORM

// =============================================================================
// Inter-MCU Link Configuration
// =============================================================================

// ESP32 SPI configuration (slave mode)
#define WAVEX_ESP_SPI_HOST SPI3_HOST  // ESP32-P4 uses SPI3_HOST for slave mode
#define WAVEX_ESP_SPI_CLK_HZ 4000000  // 4 MHz (master controls)
#define WAVEX_ESP_SPI_QUEUE_SIZE 8
#define WAVEX_ESP_SPI_DMA_CH \
    SPI_DMA_CH_AUTO  // P4 slave mode supports only auto-allocated DMA channels

// Daisy SPI configuration (master mode)
#define WAVEX_DAISY_SPI_PERIPH 1  // SPI1
#define WAVEX_DAISY_SPI_MODE 0    // MASTER mode

// Ring buffer sizes
#define WAVEX_SPI_RX_RING_SIZE 32
#define WAVEX_SPI_TX_RING_SIZE 32

// =============================================================================
// Pin Validation Macros
// =============================================================================

#ifdef ESP_PLATFORM
// Verify ESP32 pins are within valid range. The ESP32-P4 has GPIO0-GPIO54;
// the old bound of 48 was an ESP32-S3 number and would have rejected this
// file's own SPI pins (49-51) had anything used the macro.
#define WAVEX_VALIDATE_ESP_PIN(pin) ((pin) >= 0 && (pin) <= 54)
#define WAVEX_ASSERT_ESP_PIN(pin) \
    static_assert(WAVEX_VALIDATE_ESP_PIN(pin), "Invalid ESP32 pin number")
#endif

#ifdef DAISY_PLATFORM
// Verify Daisy pins are within valid range (0-30)
#define WAVEX_VALIDATE_DAISY_PIN(pin) ((pin) >= 0 && (pin) <= 30)
#define WAVEX_ASSERT_DAISY_PIN(pin) \
    static_assert(WAVEX_VALIDATE_DAISY_PIN(pin), "Invalid Daisy pin number")
#endif

#ifdef __cplusplus
}
#endif

#endif  // WAVEX_PIN_CONFIG_H
