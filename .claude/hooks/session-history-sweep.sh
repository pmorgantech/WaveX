#!/usr/bin/env bash
# Cron safety net: catches sessions that never fired a graceful SessionEnd
# (archived from the sidebar, terminal closed, process killed, crash).
# Scans this repo's Claude Code transcript directory - AND every worktree's
# transcript directory, including ones whose worktree has since been
# removed - for sessions that have gone idle and haven't been summarized
# yet, and processes them. Everything lands in the main checkout's
# docs/history, so it survives worktree cleanup.
#
# Intended to run from crontab, e.g. every 30 minutes:
#   */30 * * * * /home/petem/src/Synth/WaveX/.claude/hooks/session-history-sweep.sh >> /home/petem/src/Synth/WaveX/logs/session-sweep.log 2>&1
#
# Safe to run as often as you like: summarize-session.sh dedupes against
# .claude/hooks/.state/processed-sessions.txt, so nothing is logged twice,
# and a session still being actively written to is skipped until it's idle.
# Every run prints a timestamped start/found/done line even when there is
# nothing to do, so the log file itself is evidence the cron is firing.
set -euo pipefail

idle_minutes="${SESSION_SWEEP_IDLE_MINUTES:-120}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Claude Code sanitizes a project path into its transcript-dir name by
# replacing both '/' and '.' with '-' (so ".claude" -> "-claude").
sanitized="$(echo "$repo_root" | sed -e 's/\//-/g' -e 's/\./-/g')"

ts() { date '+%Y-%m-%d %H:%M:%S'; }

# Main checkout's transcript dir, plus any worktree transcript dirs
# (pattern: <main>--claude-worktrees-<name>) - present even after the
# worktree itself has been removed, since transcripts outlive the checkout.
project_dirs=("$HOME/.claude/projects/$sanitized")
for d in "$HOME/.claude/projects/${sanitized}--claude-worktrees-"*; do
  [[ -d "$d" ]] && project_dirs+=("$d")
done

echo "[$(ts)] sweep start (idle_minutes=$idle_minutes, scanning ${#project_dirs[@]} project dir(s))"

total_found=0
for project_dir in "${project_dirs[@]}"; do
  [[ -d "$project_dir" ]] || continue

  mapfile -d '' -t candidates < <(find "$project_dir" -maxdepth 1 -name '*.jsonl' -mmin "+$idle_minutes" -print0)
  echo "[$(ts)] $project_dir: ${#candidates[@]} idle transcript(s)"
  total_found=$((total_found + ${#candidates[@]}))

  for transcript_path in "${candidates[@]}"; do
    "$(dirname "${BASH_SOURCE[0]}")/summarize-session.sh" "$transcript_path" "$repo_root" || \
      echo "[$(ts)] $(basename "$transcript_path" .jsonl) ERROR (see above)"
  done
done

echo "[$(ts)] sweep done ($total_found transcript(s) checked)"
