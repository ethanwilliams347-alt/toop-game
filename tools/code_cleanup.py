"""The pre-PR cleanup pass: format, lint, build, test. Run it as `just code_cleanup`.

    python tools/code_cleanup.py                 # fix formatting, lint, build, run ctest
    python tools/code_cleanup.py --check         # change nothing; fail if a format is off
    python tools/code_cleanup.py --no-tests      # format and lint only
    python tools/code_cleanup.py --base origin/main --config Debug

In order:
  1. clang-format on the C++ lines this branch changed (.clang-format)
  2. clang-tidy on the C++ files this branch changed (.clang-tidy)
  3. ruff format + ruff check --fix on the Python files this branch changed (ruff.toml)
  4. cmake --build, then ctest -- every suite, golden_frame_test included

Every step runs even if an earlier one failed, so one pass shows everything, and
the exit code is non-zero if any of them did.

**Why changed lines and changed files, never the whole tree.** The code was
written by hand before any formatter existed, so a whole-tree pass rewrites a
few thousand lines. Done inside a feature branch, that buries the real change in
noise and conflicts with every other open branch. So the formatter only touches
lines this branch already touches (the same thing `git clang-format` does,
without needing that script on PATH, which LLVM's Windows installer does not put
there), and the linters only look at files this branch already touches. A
one-time full reformat is its own commit, made when nothing else is open.

"Changed" means against the merge-base with the base branch, plus uncommitted
and untracked files, so it works the same before and after you commit.

**Why clang-tidy gets its flags here rather than from compile_commands.json.**
The Visual Studio generator cannot write compile_commands.json, and asking for a
second Ninja build tree just to lint is a bigger ask than the flags are: every
target in CMakeLists.txt is C++20 with `src` and `tests` on the include path, and
the SDL headers are wherever FetchContent put them under the build directory.
SDL goes in as -isystem so its own headers never produce findings.
"""

import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CPP_EXTS = ('.cpp', '.h')
# Only our code. SDL lives under build/_deps and is not ours to format or lint.
CPP_DIRS = ('src/', 'tests/')
PY_EXTS = ('.py',)

# One line per step in the summary at the end.
results = []


def say(msg):
    print(msg, flush=True)


def run(cmd, **kwargs):
    """Runs a command from the repo root and returns the CompletedProcess."""
    return subprocess.run(cmd, cwd=REPO_ROOT, text=True, **kwargs)


def git(*args):
    proc = run(['git', *args], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed:\n{proc.stderr.strip()}")
    return proc.stdout


def find_tool(name):
    """A tool on PATH, or LLVM's default Windows install location for the clang
    tools -- the LLVM installer offers to skip the PATH edit, and many do."""
    found = shutil.which(name)
    if found:
        return found
    if os.name == 'nt' and name.startswith('clang'):
        for root in (
            os.environ.get('ProgramFiles', r'C:\Program Files'),
            os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
        ):
            candidate = os.path.join(root, 'LLVM', 'bin', name + '.exe')
            if os.path.isfile(candidate):
                return candidate
    return None


def resolve_base(requested):
    """The commit this branch forked from. Falls back from origin/main to main so
    a fresh clone without the remote ref fetched still works."""
    for ref in [requested] if requested else ['origin/main', 'main']:
        proc = run(['git', 'merge-base', 'HEAD', ref], capture_output=True)
        if proc.returncode == 0:
            return proc.stdout.strip(), ref
    raise RuntimeError(
        f"could not find a merge-base with {requested or 'origin/main or main'}; pass --base <ref>"
    )


def changed_files(base):
    """Paths (repo-relative, forward slashes) added, copied, modified or renamed
    since `base`, committed or not, plus untracked files git is not ignoring."""
    tracked = git('diff', '--name-only', '--diff-filter=ACMR', base).splitlines()
    untracked = git('ls-files', '--others', '--exclude-standard').splitlines()
    paths = sorted(set(tracked) | set(untracked))
    return [p for p in paths if os.path.isfile(os.path.join(REPO_ROOT, p))], set(untracked)


HUNK = re.compile(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@')


def changed_line_ranges(base, path):
    """1-based inclusive (first, last) line ranges of `path` that differ from
    `base`. A pure deletion leaves no line to format and yields nothing."""
    ranges = []
    for line in git('diff', '-U0', '--no-color', base, '--', path).splitlines():
        m = HUNK.match(line)
        if not m:
            continue
        start = int(m.group(1))
        count = int(m.group(2)) if m.group(2) is not None else 1
        if count > 0:
            ranges.append((start, start + count - 1))
    return ranges


def step_clang_format(files, untracked, base, check):
    name = 'clang-format (changed lines)'
    if not files:
        results.append((name, True, 'no C++ changes'))
        return
    exe = find_tool('clang-format')
    if not exe:
        results.append((name, False, 'clang-format not found; install LLVM'))
        return

    say(f'\n== {name} ==')
    touched, failed = [], False
    for path in files:
        # An untracked file is new in its entirety, so all of it is "changed".
        ranges = None if path in untracked else changed_line_ranges(base, path)
        if ranges == []:
            continue
        line_args = [] if ranges is None else [f'--lines={a}:{b}' for a, b in ranges]
        if check:
            proc = run([exe, '--dry-run', '--Werror', *line_args, path], capture_output=True)
            if proc.returncode != 0:
                failed = True
                say(proc.stderr.rstrip())
        else:
            with open(os.path.join(REPO_ROOT, path), 'rb') as f:
                before = f.read()
            proc = run([exe, '-i', *line_args, path], capture_output=True)
            if proc.returncode != 0:
                failed = True
                say(proc.stderr.rstrip())
                continue
            with open(os.path.join(REPO_ROOT, path), 'rb') as f:
                if f.read() != before:
                    touched.append(path)

    for path in touched:
        say(f'  formatted {path}')
    if check:
        results.append((name, not failed, 'needs formatting' if failed else 'clean'))
    else:
        results.append(
            (name, not failed, f'reformatted {len(touched)} file(s)' if touched else 'clean')
        )


def sdl_include_dirs(build_dir):
    """SDL's public headers plus its generated SDL_config.h, which lives in a
    per-config directory under the build tree. Empty if the tree has not been
    configured yet -- then only files that never include SDL lint cleanly."""
    deps = os.path.join(REPO_ROOT, build_dir, '_deps')
    dirs = sorted(glob.glob(os.path.join(deps, 'sdl2-build', 'include-config-*', 'SDL2')))
    # Release first if there is a choice. The configs differ only in debug macros.
    dirs.sort(key=lambda d: 'release' not in d.lower())
    dirs = dirs[:1]
    for extra in (
        os.path.join(deps, 'sdl2-build', 'include', 'SDL2'),
        os.path.join(deps, 'sdl2-src', 'include'),
    ):
        if os.path.isdir(extra):
            dirs.append(extra)
    return dirs


def step_clang_tidy(files, build_dir, jobs):
    name = 'clang-tidy (changed files)'
    if not files:
        results.append((name, True, 'no C++ changes'))
        return
    exe = find_tool('clang-tidy')
    if not exe:
        results.append((name, False, 'clang-tidy not found; install LLVM'))
        return

    say(f'\n== {name} ==')
    flags = ['-std=c++20', '-Isrc', '-Itests']
    if os.name == 'nt':
        # On Windows clang-tidy targets MSVC, so SDL_endian.h takes its _MSC_VER
        # path. Under __clang__, SDL 2.30.0 works around a Clang 11 clash with
        # winnt.h by defining its own _m_prefetch inside prfchwintrin.h's
        # include guard. Newer clang makes _m_prefetch a builtin, so that
        # definition is a hard error in every file that includes SDL, and no
        # amount of -isystem hides an error. Predefining the guard skips SDL's
        # stand-in header, which is all the workaround wanted; nothing of ours
        # uses the prefetch intrinsics. Here rather than in SDL, which is
        # fetched, not ours to patch.
        flags += ['-D__PRFCHWINTRIN_H']
    for d in sdl_include_dirs(build_dir):
        flags += ['-isystem', d]

    # Findings are reported for the changed files only. A .cpp pulls in a dozen
    # project headers, and without this a long-standing finding in, say,
    # random.h would fail every change to every file that includes it. A
    # changed header still gets its findings, both as its own lint target below
    # and when a changed .cpp includes it. Either slash, for Windows paths.
    sep = r'[/\\]'
    headers = [re.escape(h).replace('/', sep) for h in files if h.endswith('.h')]
    header_filter = f'.*{sep}({"|".join(headers)})$' if headers else '^$'

    def tidy(path):
        # A header is linted as C++ in its own right, so a header-only change
        # (most of src/game/ is headers) is checked without a .cpp around it.
        lang = ['-x', 'c++'] if path.endswith('.h') else []
        proc = run(
            [
                exe,
                '--quiet',
                '--warnings-as-errors=*',
                f'--header-filter={header_filter}',
                path,
                '--',
                *lang,
                *flags,
            ],
            capture_output=True,
        )
        return path, proc

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, proc in pool.map(tidy, files):
            # clang-tidy prints "N warnings generated" to stderr for every file,
            # findings or not, so stdout is the part worth showing.
            if proc.returncode != 0:
                failed.append(path)
                say(proc.stdout.rstrip() or proc.stderr.rstrip())
    results.append(
        (
            name,
            not failed,
            f'findings in {len(failed)} of {len(files)} file(s)'
            if failed
            else f'{len(files)} file(s) clean',
        )
    )


def step_ruff(files, check):
    name = 'ruff (changed files)'
    if not files:
        results.append((name, True, 'no Python changes'))
        return
    exe = find_tool('ruff')
    if not exe:
        results.append((name, False, 'ruff not found; pip install ruff'))
        return

    say(f'\n== {name} ==')
    fmt = run([exe, 'format', *(['--check'] if check else []), *files])
    lint = run([exe, 'check', *([] if check else ['--fix']), *files])
    ok = fmt.returncode == 0 and lint.returncode == 0
    results.append((name, ok, f'{len(files)} file(s) ' + ('clean' if ok else 'have findings')))


def step_build_and_test(build_dir, config):
    say(f'\n== build ({config}) ==')
    if not os.path.isfile(os.path.join(REPO_ROOT, build_dir, 'CMakeCache.txt')):
        results.append(('build', False, f'{build_dir}/ is not configured; run cmake -S . -B build'))
        return
    if run(['cmake', '--build', build_dir, '--config', config]).returncode != 0:
        results.append(('build', False, 'build failed'))
        results.append(('ctest', False, 'skipped, build failed'))
        return
    results.append(('build', True, config))

    say('\n== ctest ==')
    ok = (
        run(['ctest', '--test-dir', build_dir, '-C', config, '--output-on-failure']).returncode == 0
    )
    results.append(('ctest', ok, 'all passed' if ok else 'failures above'))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        '--check', action='store_true', help='change no files; fail if formatting is needed'
    )
    parser.add_argument('--base', help='branch to diff against (default: origin/main, then main)')
    parser.add_argument('--build-dir', default='build')
    parser.add_argument('--config', default='Release')
    parser.add_argument('--no-tests', action='store_true', help='skip the build and ctest')
    parser.add_argument(
        '--jobs', type=int, default=os.cpu_count() or 4, help='parallel clang-tidy processes'
    )
    args = parser.parse_args()

    try:
        base, base_ref = resolve_base(args.base)
        files, untracked = changed_files(base)
    except RuntimeError as e:
        say(f'error: {e}')
        return 2

    cpp = [f for f in files if f.endswith(CPP_EXTS) and f.startswith(CPP_DIRS)]
    py = [f for f in files if f.endswith(PY_EXTS)]
    say(f'Changes since {base_ref} ({base[:10]}): {len(cpp)} C++ file(s), {len(py)} Python file(s)')

    # Format before linting, so tidy reads the code as it will be committed.
    step_clang_format(cpp, untracked, base, args.check)
    step_clang_tidy(cpp, args.build_dir, max(1, args.jobs))
    step_ruff(py, args.check)
    if not args.no_tests:
        step_build_and_test(args.build_dir, args.config)

    say('\n== summary ==')
    width = max(len(n) for n, _, _ in results)
    for step, ok, detail in results:
        say(f"  {'ok  ' if ok else 'FAIL'}  {step.ljust(width)}  {detail}")
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == '__main__':
    sys.exit(main())
