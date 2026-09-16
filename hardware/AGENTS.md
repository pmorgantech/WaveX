# WaveX hardware agent instructions

These instructions apply to `hardware/` and supplement the root
[AGENTS.md](../AGENTS.md). Before designing, editing, generating, or reviewing
KiCad files, read the project [KiCad skill](../.agents/skills/kicad/SKILL.md), including
its validation reference when running checks or preparing outputs. The skill lives in
`.agents/skills/` alongside the other WaveX project skills and is also
available for Codex discovery as `$kicad`.

For circuit design, component selection, or electrical review, also read the
[electrical guidance](../.agents/skills/kicad/references/electrical-review.md).
Apply only the sections relevant to the requested change.

## Sources of truth

- Read [project principles](../docs/project-principles.md),
  [roadmap](../docs/roadmap.md), and [architecture](../docs/architecture.md)
  before electrical design decisions. Load the relevant feature document.
  Phase 2 is active; the analog voice board is deferred to Phase 3. An explicit
  hardware request can authorize design work, but does not close a phase gate.
- MCU assignments and feature flags belong in
  [pin_config.h](../firmware/shared/config/pin_config.h) and
  [hardware_config.h](../firmware/shared/config/hardware_config.h).
  Reconcile schematic connectivity against them; do not create prose pin tables.
  Flag conflicts before changing either side to fit an assumption.
- The schematic owns electrical connectivity and component identity. The PCB
  owns physical placement, routing, and stackup. Project settings and local
  libraries belong with the design; reports and manufacturing files are derived.

## Prerequisites and starting state

Run hardware CAD commands, checks, exports, and Python IPC automation on the
host with KiCad 10 installed. This is an explicit exception to the root
container workflow; firmware builds/tests, pre-commit, and commits still run
inside the devcontainer. Check `kicad-cli version` and the actual file headers
before choosing tools; preserve the format version unless a migration is requested.

At this guide's creation (2026-09-16), `wavex.kicad_sch` and
`wavex.kicad_pcb` are **empty KiCad 10 starters**. The saved ERC report has no
violations, while DRC reports a missing `Edge.Cuts` outline. Neither report
demonstrates a working circuit. Reinspect the files on each task.

[requirements.txt](requirements.txt) pins the Python IPC client, not the KiCad
application. Use the host's `kicad-cli` and symbol/footprint libraries; no KiCad
installation or image rebuild is needed in the devcontainer. Report unavailable
checks if host tools are missing; do not downgrade the design.

## Check and export commands

Run these on the host from the repository root:

```bash
make -C hardware -j$(nproc) help
make -C hardware -j$(nproc) check
make -C hardware -j$(nproc) gerbers
make -C hardware -j$(nproc) docs
```

[Makefile](Makefile) also exposes individual `erc`, `drc`, and `pdf` targets.
`check` collects both reports and fails if either check fails. `all` runs only
checks; exports are opt-in and do not certify a design for fabrication.
Reports go to `build/reports/`, Gerbers plus Excellon drills to `build/gerbers/`,
and the multi-page schematic PDF to `build/docs/`, relative to `hardware/`.

Override `KICAD_CLI` for an executable path and `BUILD_DIR` for a separate output
set (for example, `BUILD_DIR=build/before`). `PROJECT` defaults to `wavex`;
`SCHEMATIC` and `PCB` can override its input filenames. `GERBER_LAYERS` defaults
to two copper layers, both masks and silkscreens, and the outline; include the
actual inner copper layers when the board becomes multilayer. Exports use the
absolute origin for both Gerbers and drills. Use a fresh output directory when
changing layer selection so old layers cannot enter a fabrication package.

## Working rules

- Establish the requested circuit, electrical constraints, package choices, and
  mechanical constraints before creating geometry. Resolve only uncertainties
  that block the current step; continue independent work.
- Preserve reference designators, UUIDs, sheet paths, library identifiers, and
  schematic-to-PCB associations. Avoid whole-file regeneration for local edits.
- Verify symbol pin numbers against footprint pad numbers and the exact part's
  datasheet. A plausible rendering is not evidence of correct connectivity.
- Save and validate coherent schematic/PCB changes with KiCad, inspect visual
  exports, and report new versus pre-existing ERC/DRC findings. Do not silence
  checks or add power flags merely to obtain a clean report.
- Put new reports, previews, and fabrication outputs in ignored `hardware/build/`
  unless a reviewed artifact is requested. Avoid incidental changes to the
  already-tracked `wavex.kicad_prl` and root-level reports.
- Report what changed, checks actually run, unresolved findings, and remaining
  electrical/mechanical/bench verification. Fabrication readiness needs more
  than an empty violation count.

## Related

The [KiCad skill](../.agents/skills/kicad/SKILL.md) covers editing and automation;
its [validation reference](../.agents/skills/kicad/references/validation.md) provides
commands and handoff checks.
