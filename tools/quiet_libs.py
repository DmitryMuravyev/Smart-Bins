"""Turns off compiler warnings for third-party code we build but don't own.

Runs as a PlatformIO pre-build script (extra_scripts in platformio.ini).

Why: the SQLite library (Sqlite3Esp32) compiles the 250 000-line SQLite
amalgamation plus its own file layer, and GCC prints a hundred or so warnings
for it ("discards 'const' qualifier", unused variables, a deprecated header).
They are harmless, can't be fixed without patching the library, and bury the
warnings that matter in our own code, which keeps -Wall.

How: a build middleware sees every source file before it is compiled, the
library's files included. For files inside one of the listed libraries it
compiles the object with -w (no warnings); every other file is left alone.
"""

Import("env")  # noqa: F821 - provided by PlatformIO (SCons)

# Matched against the source file's path, so a library is recognised whether
# PlatformIO keeps it under .pio/libdeps/<env>/ or elsewhere.
QUIET_LIBS = ("Sqlite3Esp32",)


def quiet_third_party(env, node):
    path = node.get_abspath().replace("\\", "/")
    if any("/" + lib + "/" in path for lib in QUIET_LIBS):
        return env.Object(node, CCFLAGS=env["CCFLAGS"] + ["-w"])
    return node


env.AddBuildMiddleware(quiet_third_party)  # noqa: F821
