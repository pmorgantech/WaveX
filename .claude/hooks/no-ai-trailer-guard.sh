#!/usr/bin/env bash
# PreToolUse/Bash hook: hard-block `git commit` invocations that carry an
# AI co-author trailer. AGENTS.md forbids this outright (see "Versioning
# and changelog").
set -euo pipefail

input="$(cat)"
command="$(jq -r '.tool_input.command // empty' <<<"$input")"

[[ -z "$command" ]] && exit 0
[[ "$command" != *"git commit"* ]] && exit 0

if echo "$command" | grep -qiE 'co-authored-by:[^"'"'"']*(claude|anthropic)'; then
  jq -n '{
    hookSpecificOutput: {
      hookEventName: "PreToolUse",
      permissionDecision: "deny",
      permissionDecisionReason: "AGENTS.md forbids AI co-author trailers in commits (no \"Co-Authored-By: Claude ...\"). Remove the trailer before committing."
    }
  }'
fi
exit 0
