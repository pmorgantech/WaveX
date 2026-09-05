# Daisy callback performance log

This is the durable report for the recurring callback-headroom gate defined in
[`performance_monitoring.md`](performance_monitoring.md#callback-headroom-gate).
One row represents one target-hardware run of the persistent QSPI `-O2` image
with DWT profiling enabled. Host timings and the `-O0` SRAM debug image do not
belong here.

Record a run with `make perf-record`; the helper reads every
`audio_callback` profiling window in the capture, computes utilization from
the raw `DWT->CYCCNT` counts, and classifies the worst observed callback. A
trailing `+` in **Commit** means the measurement came from a dirty tree.

The log deliberately records whether callback-resident features remain. At
80% or above that answer changes the architectural decision: remaining work
activates the backend chip-upgrade path; a feature-complete build is still
blocked from release or further callback scope until its margin is resolved.

| Date | Commit | Scenario | Voices | Hz/block | Core | Image | Duration | Average cycles | Maximum cycles | Worst headroom | Underruns | Callback features left | Decision | Note |
|---|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---|---|---|
