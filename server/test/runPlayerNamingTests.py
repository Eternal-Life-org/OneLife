#!/usr/bin/env python3
"""Exercise the production name cache lifecycle without adding a wire protocol."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


server = Path(__file__).resolve().parents[1]
compiler = shutil.which("clang++") or shutil.which("g++")
if compiler is None:
    raise SystemExit("C++ compiler required")
source = (server / "curses.cpp").read_text()
handlers = "\n".join(function(source, signature) for signature in (
    "static void forgetPlayerNames(", "void cursesLogBirth(", "void cursesLogDeath(",
    "void logPlayerNameForCurses(", "char isNameDuplicateForCurses(",
))
with tempfile.TemporaryDirectory(prefix="player-name-cache-") as scratch:
    scratch = Path(scratch)
    (scratch / "playerNameCache.inc").write_text(handlers)
    executable = scratch / "testPlayerNaming"
    subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-writable-strings", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(server.parent.parent), "-I", str(scratch),
                    str(server / "test/testPlayerNaming.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=scratch, check=True, timeout=15,
                   env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
server_source = (server / "server.cpp").read_text()
assert '"NAMING"' not in server_source and '"NAME1 ' not in server_source
assert 'eveName,' in server_source
print("PASS: legacy server protocol and fixed Eve given-name path retained")
