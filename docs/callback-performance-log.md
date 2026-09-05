# Daisy callback performance log

This is the durable report for the recurring callback-headroom gate defined in
[`performance_monitoring.md`](performance_monitoring.md#callback-headroom-gate).
One row represents one scenario, captured in its own serial log, on the
persistent QSPI `-O2` image with DWT profiling enabled. Host timings and the
`-O0` SRAM debug image do not belong here, and the helper refuses them.

Record a run with `make perf-record`. The helper reads the firmware's
`profile_config:` line (core clock, sample rate, block size, storage layout,
optimization level) and every `audio_callback` profiling window in the
capture, computes utilization from the raw `DWT->CYCCNT` counts against the
budget `core_hz * block_size / sample_rate`, and classifies the worst observed
callback. **Hz/block**, **Core**, **Image**, **Budget cycles** and
**Stream underruns** are read from the capture, not typed in. A trailing `+`
in **Commit** means the measurement came from a dirty tree.

**Stream underruns** counts the SD-stream ring running dry
(`AudioEngine::CheckAndLogUnderruns()` episodes), which is a refill problem and
can happen under a comfortable callback. It is not a callback overrun. Zero is
required for the gate to pass, but the cycle columns are what the **Decision**
band is made from.

The log deliberately records whether callback-resident features remain. At
80% or above that answer changes the architectural decision: remaining work
activates the backend chip-upgrade path; a feature-complete build is still
blocked from release or further callback scope until its margin is resolved.

| Date | Commit | Scenario | Voices | Hz/block | Core | Image | Duration | Budget cycles | Average cycles | Maximum cycles | Worst headroom | Stream underruns | Callback features left | Decision | Note |
|---|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---|---|
