# WaveX: Dual-MCU Sampler/Synth

WaveX is an advanced, open-source sampler and synthesizer platform built on a dual-MCU architecture:

- **Backend (Daisy Seed / STM32H750):** Real-time audio engine for sample playback, envelopes, LFOs, modulation, and CV output.
- **Frontend (ESP32-S3):** Touchscreen UI (LVGL), SD/USB MIDI, user controls, and communication with the backend via SPI.

## Project Structure

```
/firmware/esp32/         # ESP32-S3 firmware (frontend/UI)
/firmware/daisy/         # Daisy Seed firmware (backend/audio engine)
/firmware/shared/        # Protocols, utilities, shared code
/docs/          # Architecture, setup, and documentation
/.devcontainer/ # Dockerfile and devcontainer.json
/README.md      # Project overview and quickstart
/setup.md       # Detailed setup and workflow guide
```

## Quickstart (Devcontainer)

1. **Clone the repo**
2. Open in VS Code with the [Dev Containers extension](https://code.visualstudio.com/docs/devcontainers/containers)
3. Hit F1 → "Reopen in Container"
4. Follow the [setup guide](./setup.md) for build, flash, and workflow details

## Learn More
- See [setup.md](./setup.md) for full setup, build, and flashing instructions
- See [docs/architecture.md](./docs/architecture.md) for detailed architecture
