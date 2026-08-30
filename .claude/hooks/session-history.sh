#!/usr/bin/env bash
# SessionEnd hook: immediate path. Only fires on a graceful CLI exit
# (/clear, /exit, logout) - archiving a session from the sidebar, killing
# the process, or a crash do NOT trigger this. session-history-sweep.sh
# (run from cron) is the safety net for everything this misses.
set -euo pipefail

input="$(cat)"
reason="$(jq -r '.reason // "other"' <<<"$input")"
transcript_path="$(jq -r '.transcript_path // empty' <<<"$input")"
cwd="$(jq -r '.cwd // empty' <<<"$input")"

# "resume" means the session isn't actually done - it will continue later.
if [[ "$reason" == "resume" || -z "$transcript_path" ]]; then
  exit 0
fi

# Resolve to the MAIN checkout even when this session ran inside a
# worktree (.claude/worktrees/<name>), so history survives worktree
# cleanup instead of being lost with it. `git worktree list`'s first
# entry is always the main worktree, regardless of where it's invoked from.
repo_root="$(git -C "${cwd:-$PWD}" worktree list --porcelain 2>/dev/null | sed -n 's/^worktree //p' | head -1 || true)"
repo_root="${repo_root:-$(git -C "${cwd:-$PWD}" rev-parse --show-toplevel 2>/dev/null || echo "${cwd:-$PWD}")}"

"$(dirname "${BASH_SOURCE[0]}")/summarize-session.sh" "$transcript_path" "$repo_root"
