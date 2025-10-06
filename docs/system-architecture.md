# WaveX System Architecture

## Overview

The WaveX dual-MCU sampler/synthesizer implements a distributed architecture that separates user interface and file management from real-time audio processing, optimizing each microcontroller for its specific role.

## High-Level System Architecture

```mermaid
graph TB
    subgraph "ESP32-P4 Frontend"
        UI[LVGL Touchscreen UI<br/>1280x720 MIPI DSI]
        FS[File System Manager]
        MIDI[MIDI Handler]
        PROT_ESP[Protocol Handler]
        SPI_M[SPI Master]
        LED_CTRL[LED Controller<br/>TLC5947]

        UI --> PROT_ESP
        FS --> PROT_ESP
        MIDI --> PROT_ESP
        PROT_ESP --> SPI_M
        PROT_ESP --> LED_CTRL
    end

    subgraph "Communication Layer"
        SPI_BUS[SPI Bus<br/>4MHz, Mode 0]
        IRQ_LINE[IRQ Line<br/>Ready Signal]
    end

    subgraph "Daisy Seed Backend"
        SPI_S[SPI Slave]
        PROT_STM[Protocol Processor]
        AUDIO[Audio Engine]
        VOICES[Voice Manager]
        SAMPLES[Sample Manager]
        CV_OUT[CV Output DACs<br/>MCP4728]
        SDIO[SDIO Interface<br/>4-bit SD Card]

        SPI_S --> PROT_STM
        PROT_STM --> AUDIO
        AUDIO --> VOICES
        AUDIO --> SAMPLES
        AUDIO --> CV_OUT
        PROT_STM --> SDIO
    end
    
    SPI_M -.->|MOSI/MISO/CLK/CS| SPI_BUS
    SPI_BUS -.->|MOSI/MISO/CLK/CS| SPI_S
    SPI_S -.->|Ready Signal| IRQ_LINE
    IRQ_LINE -.->|Interrupt| SPI_M
    
    subgraph "Message Types"
        MSG1[Control Change<br/>0x01]
        MSG2[Note On/Off<br/>0x02/0x03]
        MSG3[Sample Load<br/>0x04]
        MSG4[Sample Data<br/>0x05]
        MSG5[Status/Error<br/>0x06-0xFF]
    end
    
    PROT_ESP -.-> MSG1
    PROT_ESP -.-> MSG2
    PROT_ESP -.-> MSG3
    PROT_ESP -.-> MSG4
    PROT_STM -.-> MSG5
```

## Hardware Architecture

### Physical Layout
```mermaid
graph LR
    subgraph "User Interface"
        TOUCH["5 inch Touchscreen<br/>5-DSI-TOUCH-A 1280x720<br/>MIPI DSI + GT911 Touch"]
        ENCODERS["4x Dual Rotary Encoders<br/>Endless via MCP3008 ADC"]
        BUTTONS["Capacitive Buttons<br/>TCA8418 8x8 Matrix<br/>Enabled"]
        LEDS["LED Indicators<br/>TLC5947 48-channel"]
        POTS["Analog Pots<br/>MCP3008 ADC x4"]
    end

    subgraph "ESP32-P4 Module"
        ESP32["ESP32-P4<br/>Dual Core 400MHz<br/>PSRAM Enabled"]
        FLASH["16MB Flash"]
        WIFI["WiFi/Bluetooth<br/>(Disabled)"]
        SPI2["SPI2 Master<br/>LEDs + ADC"]
    end

    subgraph "Storage & I/O"
        SD["SD Card Slot<br/>SDIO 4-bit on Daisy<br/>SDXC Support"]
        USB["USB-C<br/>MIDI Host/Device"]
        MIDI_IO["MIDI UART<br/>3.5mm TRS"]
    end

    subgraph "Audio Processing"
        DAISY["Daisy Seed<br/>STM32H7 480MHz<br/>64MB SDRAM"]
        CODEC["Audio Codec<br/>AK4556 24-bit"]
        AUDIO_IO["Audio I/O<br/>1/4 inch TRS"]
        TDM_DAC["TDM DAC<br/>PCM1690 8-channel"]
    end

    subgraph "CV/Gate Output"
        DAC1["CV Out 1-8<br/>MCP48CMB28 DACs"]
        DAC2["CV Out 9-16<br/>MCP48CMB28 DACs"]
        DAC1["CV Out 17-24<br/>MCP48CMB28 DACs"]
        DAC2["CV Out 25-32<br/>MCP48CMB28 DACs"]
        GATE["Gate Outputs<br/>x8 3.3V"]
    end
    
    TOUCH --> ESP32
    ENCODER --> ESP32
    BUTTONS --> ESP32
    LEDS --> ESP32
    POTS --> ESP32
    ESP32 --> USB
    ESP32 --> MIDI_IO
    ESP32 <-->|SPI Slave-Master| DAISY
    DAISY --> SD
    DAISY --> CODEC
    CODEC --> AUDIO_IO
    DAISY --> TDM_DAC
    DAISY --> DAC1
    DAISY --> DAC2
    DAISY --> GATE
```

## Software Architecture

### ESP32-P4 Frontend Components
```mermaid
graph TD
    subgraph "Application Layer"
        UI_APP[UI Application<br/>LVGL-based]
        FILE_MGR[File Manager<br/>Sample Browser]
        PRESET_MGR[Preset Manager<br/>Save/Load]
        MIDI_APP[MIDI Application<br/>USB MIDI]
    end

    subgraph "Service Layer"
        AUDIO_SVC[Audio Service<br/>Parameter Management]
        COMM_SVC[Communication Service<br/>SPI Protocol Handler]
        STORAGE_SVC[Storage Service<br/>USB Management]
        CONFIG_SVC[Configuration Service<br/>Settings]
        LED_SVC[LED Service<br/>TLC5947 Control]
    end

    subgraph "Hardware Abstraction"
        TOUCH_HAL[Touch Driver<br/>GT911 I2C]
        DISPLAY_HAL[Display Driver<br/>MIPI DSI hx8394]
        SPI_MASTER[SPI Master<br/>Inter-MCU Link]
        SPI2_MASTER[SPI2 Master<br/>LEDs + ADC]
        PCNT_HAL|[PEC24R encoder<br>Encoder]
        ADC_HAL[ADC Driver<br/>MCP3008 for Encoders]
        I2C_HAL[I2C Driver<br/>Touch + Buttons]
        UART_MIDI[MIDI UART<br/>31250 baud]
    end
    
    subgraph "ESP-IDF Framework"
        FREERTOS[FreeRTOS<br/>Task Scheduler]
        DRIVERS[Hardware Drivers]
        NETWORK[WiFi/Bluetooth<br/>(Disabled)]
    end

    UI_APP --> AUDIO_SVC
    FILE_MGR --> STORAGE_SVC
    PRESET_MGR --> CONFIG_SVC
    MIDI_APP --> COMM_SVC

    AUDIO_SVC --> COMM_SVC
    COMM_SVC --> SPI_MASTER
    STORAGE_SVC --> LED_SVC

    TOUCH_HAL --> I2C_HAL
    DISPLAY_HAL --> DRIVERS
    SPI_MASTER --> DRIVERS
    SPI2_MASTER --> DRIVERS
    ADC_HAL --> SPI2_MASTER
    I2C_HAL --> DRIVERS
    UART_MIDI --> DRIVERS

    LED_SVC --> SPI2_MASTER
    DRIVERS --> FREERTOS
```

### Daisy Seed Backend Components
```mermaid
graph TD
    subgraph "Audio Engine"
        VOICE_ENG[Voice Engine<br/>16 Polyphonic Voices]
        SAMPLE_ENG[Sample Engine<br/>Playback & Streaming]
        EFFECT_ENG[Effects Engine<br/>Reverb, Delay, etc.]
        MIX_ENG[Mixing Engine<br/>Voice Combining]
    end

    subgraph "Parameter System"
        PARAM_MGR[Parameter Manager<br/>Real-time Updates]
        MOD_MATRIX[Modulation Matrix<br/>LFO, Envelope Routing]
        PRESET_ENG[Preset Engine<br/>Voice Management]
    end

    subgraph "Communication"
        PROTOCOL[Protocol Handler<br/>SPI Message Processing]
        CMD_QUEUE[Command Queue<br/>Priority-based]
        STATUS_MGR[Status Manager<br/>System Monitoring]
    end

    subgraph "Hardware Interface"
        AUDIO_HAL[Audio HAL<br/>AK4556 Codec]
        SPI_SLAVE[SPI Slave<br/>Inter-MCU Link]
        DAC_HAL[DAC Control<br/>MCP48CMB28 CV Output]
        SDIO_HAL[SDIO Interface<br/>4-bit SD Card]
        TDM_HAL[TDM DAC<br/>PCM1690 8-channel]
        GPIO_HAL[GPIO Control<br/>Gate Outputs]
    end

    subgraph "libDaisy Framework"
        DAISY_CORE[Daisy Core<br/>Hardware Abstraction]
        DAISYSP[DaisySP<br/>DSP Library]
        FATFS[FAT32 File System<br/>SD Card Support]
        HAL_DRIVERS[STM32 HAL<br/>Low-level Drivers]
    end
    
    VOICE_ENG --> SAMPLE_ENG
    VOICE_ENG --> EFFECT_ENG
    SAMPLE_ENG --> MIX_ENG
    EFFECT_ENG --> MIX_ENG

    PARAM_MGR --> VOICE_ENG
    MOD_MATRIX --> PARAM_MGR
    PRESET_ENG --> PARAM_MGR

    PROTOCOL --> CMD_QUEUE
    CMD_QUEUE --> PARAM_MGR
    STATUS_MGR --> PROTOCOL

    MIX_ENG --> AUDIO_HAL
    PARAM_MGR --> DAC_HAL
    PARAM_MGR --> GPIO_HAL
    PROTOCOL --> SPI_SLAVE
    STATUS_MGR --> SDIO_HAL

    AUDIO_HAL --> DAISY_CORE
    SPI_SLAVE --> HAL_DRIVERS
    DAC_HAL --> HAL_DRIVERS
    SDIO_HAL --> FATFS
    TDM_HAL --> HAL_DRIVERS
    GPIO_HAL --> HAL_DRIVERS

    DAISY_CORE --> DAISYSP
    FATFS --> HAL_DRIVERS
    DAISYSP --> HAL_DRIVERS
```

## Data Flow Architecture

### Audio Processing Pipeline
```mermaid
graph LR
    subgraph "Input Stage"
        AUDIO_IN[Audio Input<br/>Line/Instrument]
        MIDI_IN[MIDI Input<br/>External Controller]
        UI_INPUT[UI Input<br/>Touch/Encoders]
    end
    
    subgraph "Parameter Processing"
        PARAM_PROC[Parameter<br/>Processing]
        MOD_PROC[Modulation<br/>Processing]
        ENV_PROC[Envelope<br/>Processing]
    end
    
    subgraph "Voice Processing"
        VOICE1[Voice 1<br/>Sample + Synthesis]
        VOICE2[Voice 2<br/>Sample + Synthesis]
        VOICEN[Voice N<br/>Sample + Synthesis]
    end
    
    subgraph "Effects Chain"
        FILTER[Filter<br/>Resonant Lowpass]
        DELAY[Delay<br/>Tempo Sync]
        REVERB[Reverb<br/>Plate/Hall]
        CHORUS[Chorus<br/>Stereo Width]
    end
    
    subgraph "Output Stage"
        MIXER[Stereo Mixer<br/>Voice Combining]
        AUDIO_OUT[Audio Output<br/>Line Level]
        CV_OUT[CV Outputs<br/>x8 Channels]
    end
    
    AUDIO_IN --> PARAM_PROC
    MIDI_IN --> PARAM_PROC
    UI_INPUT --> PARAM_PROC
    
    PARAM_PROC --> MOD_PROC
    MOD_PROC --> ENV_PROC
    
    ENV_PROC --> VOICE1
    ENV_PROC --> VOICE2
    ENV_PROC --> VOICEN
    
    VOICE1 --> FILTER
    VOICE2 --> FILTER
    VOICEN --> FILTER
    
    FILTER --> DELAY
    DELAY --> REVERB
    REVERB --> CHORUS
    
    CHORUS --> MIXER
    MIXER --> AUDIO_OUT
    MIXER --> CV_OUT
```

### Communication Data Flow
```mermaid
graph TB
    subgraph "ESP32 Frontend"
        UI_EVENT[UI Event<br/>Touch/Button]
        PARAM_CHANGE[Parameter<br/>Change]
        SPI_TX[SPI Transmit<br/>Queue]
    end
    
    subgraph "SPI Communication"
        SPI_BUS[SPI Bus<br/>10MHz]
        PACKET[Protocol Packet<br/>Header + Payload]
    end
    
    subgraph "STM32 Backend"
        SPI_RX[SPI Receive<br/>Buffer]
        MSG_PROC[Message<br/>Processor]
        AUDIO_UPDATE[Audio Engine<br/>Update]
    end
    
    subgraph "Response Path"
        STATUS_GEN[Status<br/>Generation]
        RESPONSE[Response<br/>Message]
        SPI_RESPONSE[SPI Response<br/>Transmission]
    end
    
    UI_EVENT --> PARAM_CHANGE
    PARAM_CHANGE --> SPI_TX
    SPI_TX --> SPI_BUS
    SPI_BUS --> PACKET
    PACKET --> SPI_RX
    SPI_RX --> MSG_PROC
    MSG_PROC --> AUDIO_UPDATE
    
    AUDIO_UPDATE --> STATUS_GEN
    STATUS_GEN --> RESPONSE
    RESPONSE --> SPI_RESPONSE
    SPI_RESPONSE --> SPI_BUS
```

## Memory Architecture

### ESP32-P4 Memory Layout
```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-P4 Memory Map                      │
├─────────────────────────────────────────────────────────────┤
│ Internal SRAM (1.25MB)                                      │
│ ├─ Stack/Heap (512KB)                                      │
│ ├─ LVGL Double Buffers (320KB x2)                          │
│ ├─ Protocol Ring Buffers (128KB)                           │
│ ├─ MIPI DSI Frame Buffer (256KB)                           │
│ └─ System Reserved (128KB)                                 │
├─────────────────────────────────────────────────────────────┤
│ External PSRAM (Enabled)                                    │
│ ├─ UI Graphics Cache (Variable)                            │
│ ├─ LVGL PSRAM Buffers (if >20KB threshold)                 │
│ └─ Application Heap (Variable)                             │
├─────────────────────────────────────────────────────────────┤
│ Flash Memory (16MB)                                         │
│ ├─ Application Code (8MB)                                  │
│ ├─ File System (4MB)                                       │
│ ├─ Configuration (2MB)                                     │
│ └─ OTA Updates (2MB)                                       │
└─────────────────────────────────────────────────────────────┘
```

### Daisy Seed Memory Layout
```
┌─────────────────────────────────────────────────────────────┐
│                   Daisy Seed Memory Map                     │
├─────────────────────────────────────────────────────────────┤
│ Internal SRAM (512KB)                                       │
│ ├─ Audio Buffers (256KB)                                   │
│ ├─ Voice Data (128KB)                                      │
│ ├─ Protocol Ring Buffers (64KB)                            │
│ └─ System Stack/Heap (64KB)                                │
├─────────────────────────────────────────────────────────────┤
│ External SDRAM (64MB)                                       │
│ ├─ Sample Storage (48MB)                                   │
│ ├─ Audio Processing Buffers (8MB)                          │
│ ├─ Effects Buffers (4MB)                                   │
│ └─ Parameter Storage (4MB)                                 │
├─────────────────────────────────────────────────────────────┤
│ QSPI Flash (8MB)                                           │
│ ├─ Application Code (4MB)                                  │
│ ├─ Sample Library (2MB)                                    │
│ └─ Configuration (2MB)                                     │
├─────────────────────────────────────────────────────────────┤
│ SD Card (External, SDIO 4-bit)                              │
│ ├─ User Samples (Variable)                                 │
│ └─ Preset Storage (Variable)                               │
└─────────────────────────────────────────────────────────────┘
```

## Performance Characteristics

### Real-Time Constraints
- **Audio Latency**: <3ms (input to output)
- **Parameter Update**: <1ms (UI to audio)
- **Sample Loading**: <100ms (per MB)
- **Voice Allocation**: <100μs
- **Effect Processing**: <500μs per voice

### Throughput Specifications
- **SPI Communication**: 4 MHz (ESP32 slave, Daisy master)
- **Audio Processing**: 48kHz/24-bit stereo + 8-channel TDM
- **Voice Polyphony**: 16 simultaneous voices
- **Sample Rate**: Up to 96kHz (configurable)
- **CV Update Rate**: 1kHz per channel
- **Display**: 1280x720 MIPI DSI @1500Mbps lane bitrate
- **SDIO**: 4-bit SD card interface on Daisy

## System Interfaces

### External Connectivity
```mermaid
graph LR
    subgraph "Audio I/O"
        AUDIO_L["Audio Left<br/>1/4 inch TRS"]
        AUDIO_R["Audio Right<br/>1/4 inch TRS"]
        HP_OUT["Headphone<br/>1/4 inch TRS"]
    end
    
    subgraph "MIDI I/O"
        MIDI_IN["MIDI In<br/>DIN-5"]
        MIDI_OUT["MIDI Out<br/>DIN-5"]
        MIDI_THRU["MIDI Thru<br/>DIN-5"]
    end
    
    subgraph "CV/Gate"
        CV1["CV 1-4<br/>3.5mm TS"]
        CV2["CV 5-8<br/>3.5mm TS"]
        GATE1["Gate 1-4<br/>3.5mm TS"]
        GATE2["Gate 5-8<br/>3.5mm TS"]
    end
    
    subgraph "Data I/O"
        USB_C["USB-C<br/>Host/Device"]
        SD_CARD["SD Card<br/>SDXC"]
        WIFI_BT["WiFi/Bluetooth<br/>Internal"]
    end
    
    subgraph "User Interface"
        DISPLAY["5 inch Touchscreen<br/>1280x720 MIPI DSI<br/>GT911 Touch"]
        ENCODERS["4x Dual Rotary Encoders<br/>Endless via MCP3008 ADC"]
        BUTTONS["Capacitive Buttons<br/>TCA8418 8x8 Matrix<br/>Enabled"]
        LEDS["48-channel LED Driver<br/>TLC5947 PWM"]
        POTS["4x Analog Pots<br/>MCP3008 ADC"]
    end
    
    WAVEX["WaveX<br/>Main Unit"] --> AUDIO_L
    WAVEX --> AUDIO_R
    WAVEX --> HP_OUT
    WAVEX --> MIDI_IN
    WAVEX --> MIDI_OUT
    WAVEX --> MIDI_THRU
    WAVEX --> CV1
    WAVEX --> CV2
    WAVEX --> GATE1
    WAVEX --> GATE2
    WAVEX --> USB_C
    WAVEX --> SD_CARD
    WAVEX --> WIFI_BT
    WAVEX --> DISPLAY
    WAVEX --> ENCODERS
    WAVEX --> BUTTONS
    WAVEX --> LEDS
    WAVEX --> POTS
```

## Detailed Hardware Configuration

### ESP32-P4 Pin Assignments

#### MIPI DSI Display Interface
- **Data Lanes**: GPIO2 (D0P), GPIO3 (D0N), GPIO4 (D1P), GPIO5 (D1N)
- **Clock**: GPIO6 (CLKP), GPIO7 (CLKN)
- **Control**: GPIO8 (RST), GPIO9 (BL)

#### Touch Interface (GT911 I2C)
- **I2C Bus**: GPIO20 (SDA), GPIO21 (SCL)
- **Control**: GPIO14 (RST), GPIO15 (INT)

#### Inter-MCU Communication (SPI Slave)
- **SPI**: GPIO48 (SCK), GPIO49 (MOSI), GPIO50 (MISO), GPIO51 (CS)
- **IRQ**: GPIO31 (ATTN_OUT)

#### Rotary Encoders (MCP3008 ADC)
- **4x Dual Rotary Encoders**: Connected via MCP3008 ADC channels
- **ADC**: GPIO47 (MOSI), GPIO52 (MISO), GPIO46 (SCK), GPIO29 (CS)

#### MIDI UART
- **UART2**: GPIO32 (TX), GPIO33 (RX) @31250 baud

#### SPI2 Master (LEDs + Encoders)
- **SPI2**: GPIO47 (MOSI), GPIO52 (MISO), GPIO46 (SCK)
- **TLC5947**: GPIO28 (XLAT), GPIO27 (BLANK)
- **MCP3008**: GPIO29 (CS) - Rotary encoders + potentiometers

#### I2C Shared Bus (Touch + Buttons)
- **GT911 Touch**: GPIO20 (SDA), GPIO21 (SCL), GPIO14 (RST), GPIO15 (INT)
- **TCA8418 Buttons**: GPIO20 (SDA), GPIO21 (SCL), GPIO30 (INT)

### Daisy Seed Pin Assignments

#### Inter-MCU Communication (SPI Master)
- **SPI1**: D8 (SCK), D10 (MOSI), D9 (MISO), D7 (CS)
- **IRQ**: D0 (ATTN_IN)

#### Audio Interface (AK4556)
- **Built-in codec**: Standard Daisy audio I/O

#### CV Output DACs (MCP48CMB28)
- **DAC1**: D25 (CS), **DAC2**: D26 (CS), **DAC3**: D27 (CS), **DAC4**: D28 (CS)
- **SPI**: D29 (SCK), D30 (MOSI)

#### TDM DAC (PCM1690)
- **SAI2**: D24 (BCLK), D23 (LRCK), D22 (DATA), D21 (MCLK)

#### SD Card (SDIO 4-bit)
- **SDIO**: D19 (CS), D20 (SCK), D18 (MOSI), D17 (MISO)

### Hardware Component Configuration

#### Audio Engine (Daisy)
- **Sample Rate**: 48kHz
- **Block Size**: 48 samples
- **Buffer Size**: 256 samples
- **Meters Update**: 100ms intervals

#### Display (ESP32)
- **Resolution**: 1280x720
- **Interface**: MIPI DSI
- **Color Depth**: 16-bit RGB565
- **Controller**: HX8394
- **Touch**: GT911 I2C

#### LED Driver (TLC5947)
- **Channels**: 48
- **PWM Frequency**: 1000Hz
- **Bit Depth**: 12-bit brightness

#### ADC (MCP3008)
- **Channels**: 8 total (4x rotary encoders + 4x potentiometers)
- **Resolution**: 10-bit
- **Samples**: 64 per reading
- **Rotary Encoders**: 4x dual endless encoders

#### Button Matrix (TCA8418)
- **Matrix**: 8x8 capacitive buttons
- **I2C Address**: Standard
- **I2C Bus**: Shared with GT911 touch controller
- **Debounce**: 50ms
- **Interrupt**: GPIO30

This architecture provides a clear separation of concerns, optimizing each microcontroller for its specific role while maintaining tight integration through the high-speed SPI communication protocol. 