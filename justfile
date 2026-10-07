# Task runner for the repo: https://github.com/casey/just
#   winget install Casey.Just      (or: cargo install just / scoop install just)
#
# `just` with no arguments lists the recipes.

# PowerShell on Windows, so recipes read like the commands in CLAUDE.md.
set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-Command"]

python := if os_family() == "windows" { "python" } else { "python3" }

[private]
default:
    @just --list

# Pre-PR pass: clang-format changed lines, clang-tidy changed files, ruff changed tools/ scripts, build, ctest.
code_cleanup *ARGS:
    {{python}} tools/code_cleanup.py {{ARGS}}

# Same checks, but change nothing. Fails if anything needs formatting.
code_check *ARGS:
    {{python}} tools/code_cleanup.py --check {{ARGS}}

# AddressSanitizer build in build-asan/, then every ctest suite under it.
asan:
    cmake -S . -B build-asan -DSLOP_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build-asan --config RelWithDebInfo
    ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
