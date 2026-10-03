#!/usr/bin/env python3
"""Build the transport tests against actual minorGems sockets, then run them."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def function_body(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    cursor = body + 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


server = Path(__file__).resolve().parents[1]
workspace = server.parent.parent
engine = workspace / "minorGems"
compiler = shutil.which("clang++") or shutil.which("g++")
if not compiler:
    sys.exit("clang++ or g++ required")
arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument("--sanitize", action="store_true", help="Enable address and undefined behavior sanitizers")
options = arguments.parse_args()
sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if options.sanitize else []
test_environment = os.environ.copy()
if options.sanitize:
    test_environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
with tempfile.TemporaryDirectory(prefix="server-send-tests-") as scratch:
    target = Path(scratch) / "tests"
    command = [compiler, "-std=c++11", "-I", str(workspace), "-pthread",
               "-ffunction-sections", "-fdata-sections", "-Wno-deprecated-declarations",
               str(server / "test/testServerSendBuffer.cpp"),
               str(engine / "network/linux/SocketLinux.cpp"),
               str(engine / "network/NetworkFunctionLocks.cpp"),
               str(engine / "system/linux/MutexLockLinux.cpp")]
    command += ["-DBSD", "-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(command + sanitizers + ["-o", str(target)], check=True)
    subprocess.run([str(target)], check=True, timeout=20, env=test_environment)

    # Compile the actual old client's parser functions, not a second parser
    # implementing the same protocol. Rendering/game dependencies are omitted.
    client = (server.parent / "gameSource/LivingLifePage.cpp").read_text()
    enum_start = client.index("typedef enum messageType {")
    enum_end = client.index("} messageType;", enum_start) + len("} messageType;")
    state_start = client.index("char *pendingMapChunkMessage = NULL;")
    state_end = client.index("// NULL if there's no full message available", state_start)
    parser = client[enum_start:enum_end] + "\n" + function_body(client, "messageType getMessageType(")
    parser += "\n" + client[state_start:state_end]
    parser += "\n" + function_body(client, "char *getNextServerMessageRaw()")
    parser += "\nchar serverFrameReady = false;\nstatic SimpleVector<char*> serverFrameMessages;\n"
    parser += function_body(client, "char *getNextServerMessage()")
    (Path(scratch) / "legacyClientMessageParser.inc").write_text(parser)
    legacy = Path(scratch) / "legacy-tests"
    legacy_command = [compiler, "-std=c++11", "-I", str(workspace), "-I", scratch,
                      "-ffunction-sections", "-fdata-sections", "-Wno-deprecated-declarations",
                      "-Wno-writable-strings", str(server / "test/testLegacyClientOutput.cpp"),
                      str(engine / "formats/encodingUtils.cpp")]
    legacy_command += ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(legacy_command + sanitizers + ["-o", str(legacy)], check=True)
    result = subprocess.run([str(legacy)], timeout=10, capture_output=True, text=True, env=test_environment)
    if result.stderr:
        sys.stderr.write(result.stderr)
    if result.returncode:
        sys.stderr.write(result.stdout)
        result.check_returncode()
    print(result.stdout.splitlines()[-1])
