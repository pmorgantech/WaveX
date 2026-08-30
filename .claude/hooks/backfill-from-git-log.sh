#!/usr/bin/env bash
# One-off backfill: reconstruct docs/history/YYYYMMDD.md entries from git log
# for dates with no Claude Code transcript available. Lower fidelity than a
# real session summary (no prompts/reasoning) - clearly labeled as such.
#
# Usage: backfill-from-git-log.sh <since-date> <until-date> [repo_root]
set -euo pipefail

since_date="${1:?since date required, e.g. 2026-07-02}"
until_date="${2:?until date required, e.g. 2026-08-28}"
repo_root="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

cd "$repo_root"
history_dir="$repo_root/docs/history"
mkdir -p "$history_dir"

days="$(git log --since="$since_date 00:00:00" --until="$until_date 23:59:59" \
  --format='%ad' --date=format:'%Y-%m-%d' | sort -u)"

for day in $days; do
  datestamp="${day//-/}"
  history_file="$history_dir/$datestamp.md"

  if [[ -f "$history_file" ]]; then
    echo "skip $day: docs/history/$datestamp.md already exists"
    continue
  fi

  gitlog="$(git log --since="$day 00:00:00" --until="$day 23:59:59" \
    --pretty=format:'%h %s%n%b%n---' --stat)"

  if [[ -z "$gitlog" ]]; then
    continue
  fi

  prompt="You are reconstructing a development history log entry for the WaveX embedded firmware project from git commit history, because no Claude Code session transcript exists for this date. Below is the git log (commit hashes, subjects, bodies, and diffstat) for all commits made on $day.

Write a concise Markdown summary with these sections, using ### headers:
### Result
Bullet list of what changed (infer from commit messages and diffstat - files touched, features implemented, fixes).
### Notes
Bullet list of anything the commit messages themselves flag as a problem, deferred work, or follow-up. Omit this section entirely if nothing stands out.

Rules: no preamble, no code fences, no restating these instructions. Do not invent goals, reasoning, or problems that are not evidenced in the commit messages/diffstat themselves - this is reconstructed from commits only, not a transcript.

Git log for $day:
$gitlog"

  echo "backfilling $day..."
  summary="$(echo "$prompt" | claude -p \
    --model claude-haiku-4-5-20251001 \
    --safe-mode \
    --tools "" \
    --no-session-persistence \
    --max-budget-usd 0.20 \
    2>/dev/null || true)"

  [[ -z "$summary" ]] && continue

  {
    echo "# $day"
    echo ""
    echo "## Reconstructed from git log (no session transcript available)"
    echo ""
    echo "$summary"
    echo ""
    echo "---"
    echo ""
  } > "$history_file"
done
