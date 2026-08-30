#!/usr/bin/env bash
# One-off backfill: pull WaveX sessions out of ~/.codex/sessions (Codex CLI's
# own transcript format) and append them to docs/history/YYYYMMDD.md, same
# as a real Claude Code session would be. Filters to sessions whose cwd is
# this repo (Codex is used across multiple unrelated projects).
#
# Usage: backfill-from-codex.sh <YYYY-MM> [repo_root]
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"

month="${1:?month required, e.g. 2026-07}"
repo_root="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
year="${month%-*}"
mon="${month#*-}"

sessions_dir="$HOME/.codex/sessions/$year/$mon"
[[ -d "$sessions_dir" ]] || { echo "no codex sessions for $month"; exit 0; }

state_dir="$repo_root/.claude/hooks/.state"
state_file="$state_dir/processed-codex-sessions.txt"
mkdir -p "$state_dir"
touch "$state_file"

history_dir="$repo_root/docs/history"
mkdir -p "$history_dir"

for transcript_path in $(find "$sessions_dir" -type f -name '*.jsonl' | sort); do
  cwd="$(head -1 "$transcript_path" | jq -r '.payload.cwd // empty' 2>/dev/null)"
  [[ "$cwd" == "$repo_root"* ]] || continue

  session_id="$(basename "$transcript_path" .jsonl)"
  if grep -qxF "$session_id" "$state_file"; then
    echo "skip $session_id: already processed"
    continue
  fi

  condensed="$(jq -r '
    def clip: if (. | length) > 4000 then .[0:4000] + " [...clipped]" else . end;
    def boilerplate: test(
      "<environment_context>|<permissions instructions>|<collaboration_mode|<apps_instructions>|<skills_instructions>|<plugins_instructions>|^# AGENTS\\.md instructions|^<INSTRUCTIONS>"
    );
    select(.type == "response_item") | .payload as $p
    | if $p.type == "message" and ($p.role == "user" or $p.role == "assistant") then
        ([$p.content[]? | select(.type=="input_text" or .type=="output_text") | .text
          | select(boilerplate | not)] | join(" ")) as $t
        | if ($t | length) > 0 then
            (if $p.role == "user" then "USER: " else "ASSISTANT: " end) + ($t | clip)
          else empty end
      elif $p.type == "function_call" then
        "ASSISTANT_TOOL: " + $p.name
      else empty end
  ' "$transcript_path" 2>/dev/null | sed '/^$/d')"

  if [[ -z "$condensed" ]]; then
    echo "$session_id" >> "$state_file"
    continue
  fi

  max_total=100000
  if [[ ${#condensed} -gt $max_total ]]; then
    head_part="${condensed:0:60000}"
    tail_part="${condensed: -35000}"
    condensed="$head_part

[...middle of session clipped...]

$tail_part"
  fi

  prompt="You are compiling a running development history log for the WaveX embedded firmware project. Below is a condensed transcript of one Codex CLI session (user prompts, assistant replies, and tool names used).

Write a concise Markdown summary with these sections, using ### headers:
### Goal
One line: what the user was trying to accomplish.
### Result
Bullet list of what actually changed or was decided (files touched, features implemented, commits - infer from tool use where visible).
### Findings / recurring problems
Bullet list of anything surprising, a mistake that had to be corrected, a recurring pattern worth remembering, or an open question. Omit this section entirely if nothing stands out.
### Follow-ups
Bullet list of explicitly deferred work, if any. Omit this section entirely if none.

Rules: no preamble, no code fences, no restating these instructions. If the session was trivial (a single question with no lasting outcome), respond with exactly: SKIP

Transcript:
$condensed"

  echo "summarizing $session_id..."
  claude_err="$(mktemp)"
  if ! summary="$(echo "$prompt" | claude -p \
    --model claude-haiku-4-5-20251001 \
    --safe-mode \
    --tools "" \
    --no-session-persistence \
    --max-budget-usd 0.20 \
    2>"$claude_err")"; then
    echo "ERROR summarizing $session_id: $(tail -c 300 "$claude_err" | tr '\n' ' ')"
    rm -f "$claude_err"
    continue
  fi
  rm -f "$claude_err"

  if [[ -z "$summary" || "$summary" == "SKIP" ]]; then
    echo "$session_id" >> "$state_file"
    continue
  fi

  datestamp="$(date -r "$transcript_path" +%Y%m%d)"
  history_file="$history_dir/$datestamp.md"
  if [[ ! -f "$history_file" ]]; then
    { echo "# $(date -r "$transcript_path" +%Y-%m-%d)"; echo ""; } > "$history_file"
  fi

  session_short="$(echo "$session_id" | sed -E 's/^rollout-[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}-[0-9]{2}-[0-9]{2}-//' | cut -c1-8)"
  {
    echo "## $(date -r "$transcript_path" +%H:%M) - session $session_short (Codex CLI)"
    echo ""
    echo "$summary"
    echo ""
    echo "---"
    echo ""
  } >> "$history_file"

  echo "$session_id" >> "$state_file"
  echo "logged $session_id -> docs/history/$datestamp.md"
done
