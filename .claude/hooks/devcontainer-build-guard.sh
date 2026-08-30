#!/usr/bin/env bash
# PreToolUse/Bash hook: nudge toward ./devcontainer.sh for build/toolchain
# commands run on the host, where the toolchain is known to be incomplete
# (see AGENTS.md "Building and the devcontainer").
set -euo pipefail

input="$(cat)"
command="$(jq -r '.tool_input.command // empty' <<<"$input")"

[[ -z "$command" ]] && exit 0
[[ -f /.dockerenv ]] && exit 0

if [[ "$command" == *devcontainer.sh* || "$command" == *"docker exec"* || "$command" == *"docker run"* ]]; then
  exit 0
fi

if echo "$command" | grep -qE '(^|[[:space:];&|])(idf\.py|arm-none-eabi-gcc|arm-none-eabi-g\+\+|xtensa-esp32p4-elf-[a-z+]+)([[:space:]]|$)' \
  || echo "$command" | grep -qE '(^|[[:space:];&|])make[[:space:]]+(all|esp32|daisy|test)([[:space:]]|$)'; then
  jq -n '{
    hookSpecificOutput: {
      hookEventName: "PreToolUse",
      permissionDecision: "ask",
      permissionDecisionReason: "This looks like a firmware build/toolchain command running on the host. AGENTS.md requires building through the devcontainer (./devcontainer.sh) - the host toolchain is incomplete and produces broken/incompatible builds. Confirm this is intentional, or rerun via the devcontainer."
    }
  }'
fi
exit 0
