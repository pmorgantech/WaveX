# WaveX System Architecture

## Overview

The WaveX dual-MCU sampler/synthesizer implements a distributed architecture that separates user interface and file management from real-time audio processing, optimizing each microcontroller for its specific role.

## High-Level System Architecture

```mermaid
graph TB
    subgraph "ESP32-S3 Frontend"
        UI[LVGL Touchscreen UI]
        FS[File System Manager]
        MIDI[MIDI Handler]
        PROT_ESP[Protocol Handler]
        SPI_M[SPI Master]
        
        UI --> PROT_ESP
        FS --> PROT_ESP
        MIDI --> PROT_ESP
        PROT_ESP --> SPI_M
    end
    
    subgraph "Communication Layer"
        SPI_BUS[SPI Bus<br/>10MHz, Mode 0]
        IRQ_LINE[IRQ Line<br/>Ready Signal]
    end
    
    subgraph "STM32H750 Backend"
        SPI_S[SPI Slave]
        PROT_STM[Protocol Processor]
        AUDIO[Audio Engine]
        VOICES[Voice Manager]
        SAMPLES[Sample Manager]
        CV_OUT[CV Output DACs]
        
        SPI_S --> PROT_STM
        PROT_STM --> AUDIO
        AUDIO --> VOICES
        AUDIO --> SAMPLES
        AUDIO --> CV_OUT
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
        TOUCH["7 inch Touchscreen<br/>ST7796S 480x320"]
        ENCODER["Rotary Encoders<br/>x4"]
        BUTTONS["Push Buttons<br/>x8"]
    end
    
    subgraph "ESP32-S3 Module"
        ESP32["ESP32-S3<br/>Dual Core 240MHz<br/>8MB PSRAM"]
        FLASH["16MB Flash"]
        WIFI["WiFi/Bluetooth"]
    end
    
    subgraph "Storage & I/O"
        SD["SD Card Slot<br/>SDXC Support"]
        USB["USB-C<br/>Host/Device"]
        MIDI_IO["MIDI In/Out<br/>DIN-5 Connectors"]
    end
    
    subgraph "Audio Processing"
        DAISY["Daisy Seed<br/>STM32H750 480MHz<br/>64MB SDRAM"]
        CODEC["Audio Codec<br/>AK4556 24-bit"]
        AUDIO_IO["Audio I/O<br/>1/4 inch TRS"]
    end
    
    subgraph "CV/Gate Output"
        DAC1["CV Out 1-4<br/>12-bit DACs"]
        DAC2["CV Out 5-8<br/>12-bit DACs"]
        GATE["Gate Outputs<br/>x8 3.3V"]
    end
    
    TOUCH --> ESP32
    ENCODER --> ESP32
    BUTTONS --> ESP32
    ESP32 --> SD
    ESP32 --> USB
    ESP32 --> MIDI_IO
    ESP32 <-->|SPI Protocol| DAISY
    DAISY --> CODEC
    CODEC --> AUDIO_IO
    DAISY --> DAC1
    DAISY --> DAC2
    DAISY --> GATE
```

## Software Architecture

### ESP32-S3 Frontend Components
```mermaid
graph TD
    subgraph "Application Layer"
        UI_APP[UI Application<br/>LVGL-based]
        FILE_MGR[File Manager<br/>Sample Browser]
        PRESET_MGR[Preset Manager<br/>Save/Load]
        MIDI_APP[MIDI Application<br/>External Control]
    end
    
    subgraph "Service Layer"
        AUDIO_SVC[Audio Service<br/>Parameter Management]
        COMM_SVC[Communication Service<br/>Protocol Handler]
        STORAGE_SVC[Storage Service<br/>SD/USB Management]
        CONFIG_SVC[Configuration Service<br/>Settings]
    end
    
    subgraph "Hardware Abstraction"
        TOUCH_HAL[Touch Driver<br/>XPT2046]
        DISPLAY_HAL[Display Driver<br/>ST7796S]
        SPI_HAL[SPI Master<br/>ESP-IDF]
        FS_HAL[File System<br/>FatFS]
    end
    
    subgraph "ESP-IDF Framework"
        FREERTOS[FreeRTOS<br/>Task Scheduler]
        DRIVERS[Hardware Drivers]
        NETWORK[WiFi/Bluetooth<br/>Stack]
    end
    
    UI_APP --> AUDIO_SVC
    FILE_MGR --> STORAGE_SVC
    PRESET_MGR --> CONFIG_SVC
    MIDI_APP --> COMM_SVC
    
    AUDIO_SVC --> COMM_SVC
    COMM_SVC --> SPI_HAL
    STORAGE_SVC --> FS_HAL
    
    TOUCH_HAL --> DRIVERS
    DISPLAY_HAL --> DRIVERS
    SPI_HAL --> DRIVERS
    FS_HAL --> DRIVERS
    
    DRIVERS --> FREERTOS
```

### STM32H750 Backend Components
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
        AUDIO_HAL[Audio HAL<br/>I2S/SAI Interface]
        SPI_HAL[SPI Slave<br/>STM32 HAL]
        DAC_HAL[DAC Control<br/>CV Output]
        GPIO_HAL[GPIO Control<br/>Gate Outputs]
    end
    
    subgraph "libDaisy Framework"
        DAISY_CORE[Daisy Core<br/>Hardware Abstraction]
        DAISYSP[DaisySP<br/>DSP Library]
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
    PROTOCOL --> SPI_HAL
    
    AUDIO_HAL --> DAISY_CORE
    SPI_HAL --> HAL_DRIVERS
    DAC_HAL --> HAL_DRIVERS
    GPIO_HAL --> HAL_DRIVERS
    
    DAISY_CORE --> DAISYSP
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

### ESP32-S3 Memory Layout
```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-S3 Memory Map                      │
├─────────────────────────────────────────────────────────────┤
│ Internal SRAM (512KB)                                       │
│ ├─ Stack/Heap (256KB)                                      │
│ ├─ LVGL Buffers (128KB)                                    │
│ ├─ Protocol Buffers (64KB)                                 │
│ └─ System Reserved (64KB)                                  │
├─────────────────────────────────────────────────────────────┤
│ External PSRAM (8MB)                                        │
│ ├─ UI Graphics Cache (4MB)                                 │
│ ├─ File System Cache (2MB)                                 │
│ ├─ Sample Preview Buffer (1MB)                             │
│ └─ Application Heap (1MB)                                  │
├─────────────────────────────────────────────────────────────┤
│ Flash Memory (16MB)                                         │
│ ├─ Application Code (8MB)                                  │
│ ├─ File System (4MB)                                       │
│ ├─ Configuration (2MB)                                     │
│ └─ OTA Updates (2MB)                                       │
└─────────────────────────────────────────────────────────────┘
```

### STM32H750 Memory Layout
```
┌─────────────────────────────────────────────────────────────┐
│                   STM32H750 Memory Map                      │
├─────────────────────────────────────────────────────────────┤
│ Internal SRAM (1MB)                                         │
│ ├─ Audio Buffers (512KB)                                   │
│ ├─ Voice Data (256KB)                                      │
│ ├─ Protocol Stack (128KB)                                  │
│ └─ System Stack/Heap (128KB)                               │
├─────────────────────────────────────────────────────────────┤
│ External SDRAM (64MB)                                       │
│ ├─ Sample Storage (48MB)                                   │
│ ├─ Audio Processing Buffers (8MB)                          │
│ ├─ Effects Buffers (4MB)                                   │
│ └─ Parameter Storage (4MB)                                 │
├─────────────────────────────────────────────────────────────┤
│ Flash Memory (128KB)                                        │
│ ├─ Bootloader (64KB)                                       │
│ ├─ Application Code (32KB)                                 │
│ └─ Configuration (32KB)                                    │
│                                                             │
│ Note: Main application runs from external QSPI Flash       │
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
- **SPI Communication**: 8.5 Mbps effective
- **Audio Processing**: 48kHz/24-bit stereo
- **Voice Polyphony**: 16 simultaneous voices
- **Sample Rate**: Up to 96kHz (configurable)
- **CV Update Rate**: 1kHz per channel

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
        DISPLAY["7 inch Touchscreen<br/>480x320 RGB"]
        ENCODERS["4x Rotary<br/>Encoders"]
        BUTTONS["8x Push<br/>Buttons"]
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
```

This architecture provides a clear separation of concerns, optimizing each microcontroller for its specific role while maintaining tight integration through the high-speed SPI communication protocol. 