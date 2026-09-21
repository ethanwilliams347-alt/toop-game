"""Points a sprite key at a BMP you dropped into assets/, from the command line.

The workflow this exists for: drag a .bmp into assets/ in File Explorer, run one
command, launch the game and see it. Before this, both halves of that were code
changes - the path was a string literal in main.cpp, and assets/ is copied next
to the exe at build time, so even editing the existing file in place did nothing
until a rebuild. This writes the binding and stages the file, so neither is true
any more.

    python tools/load_sprite.py --list                        # what is bound to what
    python tools/load_sprite.py player_sheet my_sheet.bmp     # rebind and stage
    python tools/load_sprite.py --stage                       # re-copy assets to the build dirs
    python tools/load_sprite.py --stage --prune               # ...and drop staged files assets/ no longer has

The file argument is a name inside assets/, because assets/ is the only place
the game looks - `my_sheet.bmp` and `assets/my_sheet.bmp` both work, a path
anywhere else is refused with the reason.

**What this does not do is change what a sheet's frames mean.** Frame size, row
order and frame counts are the ANIMATIONS table in tools/player_sheet.py and the
header it generates; they are facts about the game, not about the image. So a
sheet whose grid disagrees with the table is rejected here, with the two numbers
printed, rather than bound and left to draw sliced. Change the table and rerun
--header if the new sheet really is a different shape.
"""
import argparse
import os
import shutil
import sys

from pixel_art import COLOR_KEY, asset_path, read_bmp

MANIFEST = asset_path('sprites.txt')
ASSET_DIR = 'assets'

# Where a staged asset has to land to be seen without a rebuild. CMake copies
# assets/ next to the exe post-build; these are the same directories, written
# directly. Missing ones are skipped rather than created - a build directory
# that does not exist is a configuration you do not have, not an error.
BUILD_ASSET_DIRS = [
    os.path.join('build', 'Release', 'assets'),
    os.path.join('build', 'Debug', 'assets'),
]


def read_manifest():
    """Every line of the file, and the parsed records, kept together.

    The raw lines are carried because rewriting is done in place: a manifest is
    a hand-editable file with comments explaining each group, and regenerating
    it from the records would throw all of that away the first time this tool
    ran.
    """
    if not os.path.exists(MANIFEST):
        return [], {}
    with open(MANIFEST, encoding='utf-8') as f:
        lines = f.read().split('\n')

    records = {}
    for i, line in enumerate(lines):
        text = line.split('#', 1)[0].strip()
        if not text:
            continue
        parts = text.split()
        key, filename = parts[0], parts[1] if len(parts) > 1 else None
        frame = None
        if len(parts) >= 4:
            try:
                frame = (int(parts[2]), int(parts[3]))
            except ValueError:
                frame = None
        records[key] = {'line': i, 'file': filename, 'frame': frame}
    return lines, records


def resolve_in_assets(arg):
    """The bare filename inside assets/, or None with a reason printed."""
    path = arg.replace('\\', '/')
    if path.startswith(ASSET_DIR + '/'):
        path = path[len(ASSET_DIR) + 1:]
    if '/' in path or '..' in path:
        print(f'FAIL  {arg}: sprites are loaded from {ASSET_DIR}/ and nowhere else.')
        print(f'      Copy the file into {ASSET_DIR}/ and name it alone.')
        return None
    if not path.lower().endswith('.bmp'):
        print(f'FAIL  {arg}: must be a .bmp - the loader is SDL_LoadBMP, there is no '
              'PNG path. tools/png_to_bmp.py converts.')
        return None
    if not os.path.exists(os.path.join(ASSET_DIR, path)):
        print(f'FAIL  {ASSET_DIR}/{path} does not exist. Drop the file in {ASSET_DIR}/ first.')
        return None
    return path


def check_image(filename, expect_frame):
    """Reads the BMP and reports what it is. Returns (w, h) or None on failure.

    The checks are the ones whose absence is silent in the game: a file SDL can
    open but this cannot is a format the rest of the pipeline's tools will choke
    on later, and a sheet that does not divide into its declared frame size
    draws sliced with nothing printed anywhere.
    """
    full = os.path.join(ASSET_DIR, filename)
    try:
        w, h, px = read_bmp(full)
    except Exception as exc:
        print(f'FAIL  {full}: not a BMP this project can read ({exc}).')
        print('      24-bit uncompressed is what the tools and SDL_LoadBMP agree on; '
              're-export, or run it through tools/png_to_bmp.py.')
        return None

    print(f'      {full}: {w}x{h}')

    keyed = sum(1 for p in px if p == COLOR_KEY)
    if keyed:
        print(f'      transparency: {keyed} magenta pixels will be keyed out')
    else:
        print('WARN  no magenta (#FF00FF) pixels - this sprite will draw as a solid '
              'rectangle. That is correct for a backdrop and wrong for anything with '
              'a silhouette.')

    if expect_frame:
        fw, fh = expect_frame
        if w % fw or h % fh:
            print(f'FAIL  {w}x{h} does not divide into {fw}x{fh} frames.')
            print('      Either the art is the wrong size, or the frame size in '
                  f'{MANIFEST} and tools/player_sheet.py has changed and this sheet '
                  'was drawn against the old one.')
            return None
        print(f'      grid: {w // fw} cols x {h // fh} rows of {fw}x{fh}')

    return w, h


def check_against_animation_table(w, h):
    """The player sheet only: does the image fit the table the game is compiled
    against? Reported as a hard failure rather than a warning, because binding a
    mismatched sheet is a change that looks like it worked."""
    try:
        from player_sheet import SHEET_W, SHEET_H, SHEET_COLS, SHEET_ROWS, FRAME_W, FRAME_H
    except Exception as exc:
        print(f'WARN  could not read the animation table ({exc}); binding without '
              'checking the grid.')
        return True

    if (w, h) == (SHEET_W, SHEET_H):
        return True

    print(f'FAIL  the animation table expects {SHEET_W}x{SHEET_H} '
          f'({SHEET_COLS} cols x {SHEET_ROWS} rows of {FRAME_W}x{FRAME_H}), '
          f'and this sheet is {w}x{h}.')
    print('      A sheet of the wrong shape still loads and still draws - it just '
          'draws the wrong rectangles, which is why this is refused here.')
    print('      If the new sheet is genuinely a different shape, edit ANIMATIONS in '
          'tools/player_sheet.py, then: python tools/player_sheet.py --header')
    return False


def write_binding(lines, records, key, filename):
    """Rewrites one record in place, or appends a new one, preserving comments
    and the column alignment the file is written in."""
    frame = records.get(key, {}).get('frame')
    fields = [key.ljust(23), filename.ljust(27) if frame else filename]
    if frame:
        fields.append(f'{frame[0]} {frame[1]}')
    record = ' '.join(fields).rstrip()

    if key in records:
        lines[records[key]['line']] = record
    else:
        if lines and lines[-1] == '':
            lines.insert(len(lines) - 1, record)
        else:
            lines.append(record)

    with open(MANIFEST, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print(f'      {MANIFEST}: {key} -> {filename}')


def stage(files=None, prune=False):
    """Copies the manifest and the named files (or all of assets/) into every
    build directory that exists.

    This is the half of the problem that is not the path. `assets/` is copied
    next to the exe as a post-build step, so without this you edit art, launch,
    and see the old drawing - which reads as the tool not having worked.

    Only files are copied, never directories, which is what keeps `assets/wip/`
    out of the shipped set for free rather than by a rule anyone has to
    remember.

    **Staging is copy-only, so a build directory accumulates.** A renamed or
    deleted asset stays next to the exe forever, and this project has already
    lost a session to validating one file while the game loaded another. Names
    that no longer exist in assets/ are therefore *reported* on every run, and
    `--prune` is what removes them. Reporting is unconditional and deleting is
    opt-in on purpose: the report costs nothing and the deletion is the half
    that could throw away something a build put there deliberately.
    """
    staged = 0
    for dest in BUILD_ASSET_DIRS:
        if not os.path.isdir(dest):
            continue
        names = files if files is not None else os.listdir(ASSET_DIR)
        for name in names:
            src = os.path.join(ASSET_DIR, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dest, name))
        print(f'      staged into {dest}')
        staged += 1

        stale = [n for n in os.listdir(dest)
                 if os.path.isfile(os.path.join(dest, n))
                 and not os.path.exists(os.path.join(ASSET_DIR, n))]
        for name in sorted(stale):
            if prune:
                os.remove(os.path.join(dest, name))
                print(f'      pruned {name}')
            else:
                print(f'STALE {dest}{os.sep}{name}: not in {ASSET_DIR}/ any more '
                      '(--prune removes it)')

    if not staged:
        print('WARN  no build directory to stage into - build once, and the assets '
              'are copied by the build itself.')
    return staged


def do_list():
    lines, records = read_manifest()
    if not records:
        print(f'{MANIFEST}: nothing bound (every key falls back to its shipped file).')
        return True
    print(f'{MANIFEST}:')
    width = max(len(k) for k in records)
    ok = True
    for key, rec in records.items():
        full = os.path.join(ASSET_DIR, rec['file'])
        mark = ' ' if os.path.exists(full) else '  <- MISSING'
        frame = f"  frames {rec['frame'][0]}x{rec['frame'][1]}" if rec['frame'] else ''
        print(f'  {key.ljust(width)}  {rec["file"]}{frame}{mark}')
        if not os.path.exists(full):
            ok = False
    if not ok:
        print('\nA missing file is not fatal at runtime - the key falls back to the '
              'file the code shipped with - but nothing you bound is being shown.')
    return ok


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('key', nargs='?', help='the sprite key to bind, e.g. player_sheet')
    p.add_argument('file', nargs='?', help='a .bmp inside assets/')
    p.add_argument('--list', action='store_true', help='show the current bindings')
    p.add_argument('--stage', action='store_true',
                   help='re-copy assets/ into the build directories and exit')
    p.add_argument('--prune', action='store_true',
                   help='with --stage: also delete staged files no longer in assets/')
    args = p.parse_args()

    if not os.path.isdir(ASSET_DIR):
        print(f'FAIL  no {ASSET_DIR}/ here - run this from the repo root.')
        return 1

    if args.list:
        return 0 if do_list() else 1

    if args.stage and not args.key:
        stage(prune=args.prune)
        return 0

    if not args.key or not args.file:
        p.print_help()
        return 2

    filename = resolve_in_assets(args.file)
    if filename is None:
        return 1

    lines, records = read_manifest()
    expect_frame = records.get(args.key, {}).get('frame')

    print(f'binding {args.key}:')
    size = check_image(filename, expect_frame)
    if size is None:
        return 1
    if args.key == 'player_sheet' and not check_against_animation_table(*size):
        return 1

    write_binding(lines, records, args.key, filename)
    stage([filename, os.path.basename(MANIFEST)])
    print(f'OK    {args.key} now loads {ASSET_DIR}/{filename}. Launch the game; no rebuild needed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
