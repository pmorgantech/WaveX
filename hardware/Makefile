# Run on the host with KiCad 10 installed: make -C hardware <target>
# KiCad 10 CLI reference: https://docs.kicad.org/10.0/en/cli/cli.html
KICAD_CLI ?= kicad-cli
PROJECT ?= wavex
BUILD_DIR ?= build
SCHEMATIC ?= $(PROJECT).kicad_sch
PCB ?= $(PROJECT).kicad_pcb

# Two-layer board default; include inner copper here for a multilayer design.
GERBER_LAYERS ?= F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,Edge.Cuts

.DEFAULT_GOAL := help
# KiCad CLI processes share an instance lock; serialize even when given -j.
.NOTPARALLEL:
.PHONY: help all check erc drc gerbers docs pdf require-kicad

help: ## Show targets and configuration
	@awk 'BEGIN {FS = ":.*## "} /^[a-z-]+:.*## / {printf "  %-16s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf '\nRun on the host with KiCad 10 installed.\n'
	@printf 'Overrides: KICAD_CLI, PROJECT, SCHEMATIC, PCB, BUILD_DIR, GERBER_LAYERS\n'
	@printf 'Outputs: $(BUILD_DIR)/reports/, $(BUILD_DIR)/gerbers/, $(BUILD_DIR)/docs/\n'

all: check ## Run checks only; exports are opt-in

# Keep going to collect both reports even if one check fails.
check: ## Run ERC and DRC; fail if either check fails
	+$(MAKE) --no-print-directory -f "$(firstword $(MAKEFILE_LIST))" -k erc drc

require-kicad:
	@command -v "$(KICAD_CLI)" >/dev/null 2>&1 || { \
		printf '%s\n' 'KiCad CLI not found: $(KICAD_CLI). Install KiCad 10 on the host or set KICAD_CLI to its executable path.' >&2; \
		exit 127; \
	}

erc: $(SCHEMATIC) | require-kicad ## Check schematic; write reports/PROJECT-erc.rpt
	mkdir -p "$(BUILD_DIR)/reports"
	"$(KICAD_CLI)" sch erc --severity-all --exit-code-violations \
		--output "$(BUILD_DIR)/reports/$(PROJECT)-erc.rpt" "$(SCHEMATIC)"

# Refill for checking without saving or mutating the source PCB.
drc: $(PCB) $(SCHEMATIC) | require-kicad ## Check PCB and schematic parity; write reports/PROJECT-drc.rpt
	mkdir -p "$(BUILD_DIR)/reports"
	"$(KICAD_CLI)" pcb drc --severity-all --schematic-parity \
		--refill-zones --exit-code-violations \
		--output "$(BUILD_DIR)/reports/$(PROJECT)-drc.rpt" "$(PCB)"

# Both exports use the absolute origin. These are outputs, not a release gate.
gerbers: $(PCB) | require-kicad ## Export Gerbers and Excellon drill files
	mkdir -p "$(BUILD_DIR)/gerbers"
	"$(KICAD_CLI)" pcb export gerbers --layers "$(GERBER_LAYERS)" --check-zones \
		--output "$(BUILD_DIR)/gerbers/" "$(PCB)"
	"$(KICAD_CLI)" pcb export drill --format excellon --drill-origin absolute \
		--excellon-units mm --output "$(BUILD_DIR)/gerbers/" "$(PCB)"

docs: pdf ## Export schematic documentation (PDF)

pdf: $(SCHEMATIC) | require-kicad ## Export all schematic sheets to docs/PROJECT.pdf
	mkdir -p "$(BUILD_DIR)/docs"
	"$(KICAD_CLI)" sch export pdf \
		--output "$(BUILD_DIR)/docs/$(PROJECT).pdf" "$(SCHEMATIC)"
