# `assets/wip/` — drafts, candidates and superseded art

Nothing in here is loaded by the game, and that is enforced rather than
promised:

- `tools/load_sprite.py` refuses any argument containing a path separator, so a
  file in here cannot be bound to a sprite key by accident.
- Staging copies files and skips directories, so this folder is never placed
  next to the built exe.

**`assets/` proper holds only what the game can load.** Anything you are still
deciding about lives here until it wins.

## What is in here now

| File | What it is |
|---|---|
| `prev_player_sheet_a.bmp` | Superseded player sheet |
| `prev_player_sheet_a_duplicate.bmp` | Byte-identical copy of the above — it was `player_sheet_2.bmp` |
| `prev_player_sheet_fly.bmp` | The first fly-pose sheet, superseded by the current one |
| `copy_candidate_a.bmp` | Single-frame candidate |
| `v2_owl_pixel_art.bmp` | Single-frame candidate |

**All of it is committed to git, so deleting any of it is safe and reversible.**
That is the point of moving it here rather than straight out: the decision to
throw art away is yours and does not need to be made in the same sitting as the
tidying.

## The one naming rule

Filenames say **what a thing is**, never **which revision it is**. `COPY_`,
`_2`, `_final`, `_new` are all filename-as-version-control in a repository that
already has version control — that is how five player sheets ended up here with
no way to tell which one the game was loading.

`prev_` is the one deliberate exception, and only inside this folder, because
"superseded" *is* what those files are now.

