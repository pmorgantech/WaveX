#!/usr/bin/env bash
# Shared core: condense one transcript, summarize it, append to
# docs/history/YYYYMMDD.md, and mark it processed so nothing gets logged
# twice. Called by session-history.sh (SessionEnd hook, immediate) and
# session-history-sweep.sh (cron safety net for sessions that never fired
# a graceful SessionEnd - archived, killed, crashed, etc).
#
# Usage: summarize-session.sh <transcript_path> <repo_root>
set -euo pipefail

# cron's PATH doesn't include ~/.local/bin, where `claude` lives - without
# this, the claude -p call below fails with "command not found" whenever
# this runs from cron instead of interactively.
export PATH="$HOME/.local/bin:$PATH"

transcript_path="${1:?transcript_path required}"
repo_root="${2:?repo_root required}"

[[ -f "$transcript_path" ]] || exit 0

session_id="$(basename "$transcript_path" .jsonl)"
state_dir="$repo_root/.claude/hooks/.state"
state_file="$state_dir/processed-sessions.txt"
mkdir -p "$state_dir"
touch "$state_file"

if grep -qxF "$session_id" "$state_file"; then
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $session_id already-processed"
  exit 0
fi

mark_processed() {
  echo "$session_id" >> "$state_file"
}

# -R + fromjson? reads line-by-line and skips any single malformed line
# instead of aborting the whole file - large transcripts (tens of MB) can
# have one corrupted line (interrupted write, crash) that would otherwise
# make jq exit non-zero and, under pipefail, kill this script with zero
# diagnostic output.
condensed="$(jq -R -r '
  def clip: if (. | length) > 4000 then .[0:4000] + " [...clipped]" else . end;
  fromjson? | .message.role as $role
  | if $role == "user" then
      if (.message.content | type) == "string" then
        "USER: " + (.message.content | clip)
      else
        ([.message.content[]? | select(.type=="text") | .text] | join(" ")) as $t
        | if ($t | length) > 0 then "USER: " + ($t | clip) else empty end
      end
    elif $role == "assistant" then
      ([.message.content[]? |
        if .type == "text" then "ASSISTANT: " + (.text | clip)
        elif .type == "tool_use" then "ASSISTANT_TOOL: " + .name
        else empty end
      ] | join("\n"))
    else empty end
' "$transcript_path" 2>/dev/null | sed '/^$/d' || true)"

if [[ -z "$condensed" ]]; then
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $session_id skipped (no extractable dialogue)"
  mark_processed
  exit 0
fi

# Cap total size: keep the opening (initial ask) and the tail (recent work),
# since a session's skill/tool output can otherwise dwarf the actual dialogue.
max_total=100000
if [[ ${#condensed} -gt $max_total ]]; then
  head_part="${condensed:0:60000}"
  tail_part="${condensed: -35000}"
  condensed="$head_part

[...middle of session clipped...]

$tail_part"
fi

prompt="You are compiling a running development history log for the WaveX embedded firmware project. Below is a condensed transcript of one Claude Code session (user prompts, assistant replies, and tool names used).

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

claude_err="$(mktemp)"
if ! summary="$(echo "$prompt" | claude -p \
  --model claude-haiku-4-5-20251001 \
  --safe-mode \
  --tools "" \
  --no-session-persistence \
  --max-budget-usd 0.20 \
  2>"$claude_err")"; then
  # Genuine failure (missing binary, network, budget, etc) - do NOT mark
  # processed, so the next sweep retries instead of silently losing this
  # session for good.
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $session_id ERROR: claude -p failed: $(tail -c 300 "$claude_err" | tr '\n' ' ')"
  rm -f "$claude_err"
  exit 1
fi
rm -f "$claude_err"

if [[ -z "$summary" || "$summary" == "SKIP" ]]; then
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $session_id skipped (trivial)"
  mark_processed
  exit 0
fi

history_dir="$repo_root/docs/history"
mkdir -p "$history_dir"

# Date/time from the transcript's own last-modified stamp, not "now" - a
# swept session may be processed well after the fact, but it belongs in
# the day it actually happened.
datestamp="$(date -r "$transcript_path" +%Y%m%d)"
history_file="$history_dir/$datestamp.md"

if [[ ! -f "$history_file" ]]; then
  {
    echo "# $(date -r "$transcript_path" +%Y-%m-%d)"
    echo ""
  } > "$history_file"
fi

session_short="${session_id:0:8}"
# Prefer the branch recorded in the transcript itself over the live repo
# state - correct even for a worktree session whose checkout is long gone.
branch="$(jq -r 'select(.gitBranch != null) | .gitBranch' "$transcript_path" 2>/dev/null | head -1 || true)"
branch="${branch:-$(git -C "$repo_root" branch --show-current 2>/dev/null || echo "unknown")}"

{
  echo "## $(date -r "$transcript_path" +%H:%M) - session $session_short ($branch)"
  echo ""
  echo "$summary"
  echo ""
  echo "---"
  echo ""
} >> "$history_file"

mark_processed
echo "[$(date '+%Y-%m-%d %H:%M:%S')] $session_id logged to docs/history/$datestamp.md"
