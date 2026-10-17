---
name: handoff
description: Rewrite the local, gitignored HANDOFF.md in the toop repo root so a fresh agent with no context can pick up the current work. Every claim is re-verified against the tree, git, and this conversation, and finished items are pruned. Use only when the user explicitly asks for it, e.g. "write a handoff", "update HANDOFF.md", "rewrite the handoff", "hand this off", or /handoff. Do not use it on your own initiative at the end of a task, and do not read HANDOFF.md as background context unless the user asks or has HANDOFF.md open or selected in the IDE.
---

# Rewrite HANDOFF.md

`HANDOFF.md` is a note the user keeps on their own machine, in the repo root. It is
gitignored and was deliberately untracked. Its job is to let a **fresh agent with zero
conversation context** pick up the current work without redoing what's already done
and without being misled.

The user is asking for this file right now, so reading the existing `HANDOFF.md` is
in scope for this task. Outside this skill, still treat it as off-limits.

## 1. Gather

Collect these before writing anything:

- **This conversation.** It is the main source: what you did, what you measured,
  what you tried that failed, what the user decided, and what is still open.
- **Git state:**
  ```bash
  git rev-parse --show-toplevel          # the real repo path; never type it from memory
  git branch --show-current
  git rev-parse --short HEAD
  git status --short
  git log --oneline main..HEAD           # what this branch has landed
  git diff --stat main...HEAD
  ```
- **`CLAUDE.md`.** Read it to see what *not* to repeat (see step 3).

## 2. Verify every factual claim, or drop it

Go through each statement you are about to write, including ones carried over from
the old file, and check it against the source:

| Claim | How to verify |
|---|---|
| Repo path, branch, commit | the git commands above |
| "Tests are green" / test count | `ctest --test-dir build -C Release -N` for the count. Only call the suite green if you ran it in this session at this commit. Otherwise write "last run green at `<sha>`" or "not run". |
| `file.cpp:123` locations | Open the file and read that line. Cite it as of the current short SHA so the reader knows when line numbers may have moved. |
| Constant values | grep the source. Don't copy them from `TUNING.md` or the old handoff. |
| Bench numbers | Include only numbers you measured in this session, given as paired before/after runs, with date and commit. Otherwise tell the reader to re-baseline with `grid_bench`. |
| "X is still to do" | Check `git log` and the code to see whether it already landed. If it did, move it to **Done** or delete it. |
| Temp/scratchpad paths | Session scratchpads get cleaned up. Mark the path as possibly gone and describe enough that the work can be rebuilt without it. |

If you can't verify a claim cheaply, either say plainly that it's unverified and why,
or leave it out. Never pass on a number or location you haven't seen yourself.

## 3. Don't duplicate CLAUDE.md

`CLAUDE.md` is loaded automatically for the next agent. It already covers the
invariants, the build and test commands, how to verify changes, the perf-claim
rules, and code conventions. Restating them creates a second copy that drifts. The
"rules of engagement" section in the old handoff is an example of this. Link to
CLAUDE.md instead. Only add to the handoff what is specific to *this* work and isn't
derivable from the repo.

## 4. Write it

Write it for that fresh agent: be direct and concrete, and point to `file:line`
locations. Keep it as short as the open work allows. A tidy handoff for two open
items is 40 lines, not 250. Use this structure and drop any section that would be
empty:

```markdown
# Handoff: <the goal, in one line>

_Written <YYYY-MM-DD> · branch `<branch>` @ `<short sha>` · tree <clean | N uncommitted files>_

<2–4 sentences: repo path (from git), what the reader is here to do, and that
CLAUDE.md is auto-loaded and holds the rules and commands.>

## Last response
your last response in this session.

## State right now
- Build / tests: <verified N/N at <sha> this session | last known … | not run>
- Uncommitted changes and what they are, if any
- Baseline numbers that matter for the open work, with date + commit, or "re-baseline"

## Done
- <one line each, with commit sha, so the reader doesn't redo it>

## Open work
### 1. <title>  <- <risk tag: safe | changes golden checksum | invalidates .rec | needs sign-off>
What and why, where (`file:line` @ sha), the intended approach, how to verify
(which tests and which bench rows), and anything that needs the user's go-ahead.

## Tried and rejected
- <approach>: <measured result>, so <conclusion>. Negative results with numbers are
  some of the most valuable content here, because they stop the next agent from
  repeating the experiment.

## Open questions for the user
- <decisions not yet made>

## Start here
<One concrete first action.>
```

Order open work by risk, safest first. Flag anything that changes replay behaviour,
the `golden_frame_test` checksum, or `.rec` compatibility, since CLAUDE.md requires
confirmation before those changes land.

## 5. After writing

- Run `git check-ignore -q HANDOFF.md`. If that exits non-zero, the file is no longer
  ignored. Tell the user, and don't fix `.gitignore` without asking.
- Never `git add` or commit `HANDOFF.md`, and don't mention it in commit messages or
  other tracked files.
- In your reply, report what changed compared with the old version: what was pruned
  as done, which stale claims were corrected, and what's new. Also list anything you
  left marked as unverified.
- After that, go back to treating `HANDOFF.md` as off-limits. Don't use it as context
  for later tasks unless the user asks.
