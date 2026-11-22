#!/usr/bin/env python3
"""Generate a clang VFS overlay that makes #include lookups case-insensitive.

Windows headers (and this project's own includes such as "BDD.hxx" -> bdd.hxx) rely on a
case-insensitive filesystem. clang's -ivfsoverlay with 'case-sensitive: false' fixes that
without touching the sources or making symlink farms.

usage: gen-vfs.py OUT.yaml DIR [DIR ...]
"""
import os
import sys


def q(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def emit(path: str, name: str, out: list, ind: int) -> None:
    pad = " " * ind
    out.append(f"{pad}- name: {q(name)}")
    out.append(f"{pad}  type: directory")
    out.append(f"{pad}  contents:")
    for e in sorted(os.listdir(path)):
        full = os.path.join(path, e)
        if os.path.isdir(full):
            continue  # include dirs are listed explicitly; subdirs fall through to the real FS
        else:
            out.append(f"{pad}    - name: {q(e)}")
            out.append(f"{pad}      type: file")
            out.append(f"{pad}      external-contents: {q(full)}")


def main() -> None:
    out_path, dirs = sys.argv[1], [os.path.realpath(d) for d in sys.argv[2:]]
    lines = ["{ version: 0, case-sensitive: false, roots: [ ] }"]
    lines = ["version: 0", "case-sensitive: false", "roots:"]
    for d in dirs:
        emit(d, d, lines, 2)
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
