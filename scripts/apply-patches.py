#!/usr/bin/env python3
"""Applies patches/<submodule>/*.patch to the matching ext/<submodule> checkout.

The submodules are other people's repositories, so our changes to them live in
patches/ and are applied to the working tree (never committed there). Safe to
run any number of times: a patch that is already applied is skipped.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)


def main():
    status = 0
    for patch in sorted((ROOT / "patches").glob("*/*.patch")):
        repo = ROOT / "ext" / patch.parent.name
        if not repo.is_dir():
            print(f"apply-patches: {repo} is missing (git submodule update --init)", file=sys.stderr)
            status = 1
            continue
        flags = ["--ignore-whitespace", str(patch)]
        if git(repo, "apply", "--check", "-R", *flags).returncode == 0:
            continue  # already applied
        result = git(repo, "apply", *flags)
        if result.returncode != 0:
            print(f"apply-patches: {patch.name} does not apply to {repo.name}:\n{result.stderr}",
                  file=sys.stderr)
            status = 1
            continue
        print(f"apply-patches: applied {patch.name} to {repo.name}")
    return status


if __name__ == "__main__":
    sys.exit(main())
