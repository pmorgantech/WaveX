# Claude Code Instructions

**Primary instructions live in [`AGENTS.md`](AGENTS.md).** Read it first — it covers doc orientation (`docs/roadmap.md`, `docs/architecture.md`), the non-negotiable real-time/audio constraints, C++ conventions for this embedded target, and versioning/changelog rules. Everything below is Claude-specific routing on top of that.

## Model selection

Pick the model to match the kind of work, so architecture/design gets the most capable reasoning and routine work doesn't burn it unnecessarily:

| Work | Model |
|---|---|
| Architecture/design (system design, protocol changes, DSP/real-time tradeoffs, anything touching `docs/architecture.md` or `docs/roadmap.md`) | **Fable 5**, fallback **Opus 4.8** |
| Standard implementation, coding chores, refactors | **Sonnet 5** |
| Git operations, documentation edits, builds, test runs and their analysis | **Haiku** |

If a task starts as a design question and turns into implementation mid-conversation, it's fine to switch models rather than force one model through the whole task.

## Reminders specific to this repo

- Check `docs/roadmap.md` for the current phase before proposing new work — don't design ahead of the active phase's gate.
- Never implement from `docs/archive/`.
- Pin/flag truth lives only in `firmware/shared/config/pin_config.h` and `hardware_config.h` — never in docs or comments.
