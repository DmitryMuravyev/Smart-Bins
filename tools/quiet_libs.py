"""Turns off compiler warnings for third-party code we build but don't own.

Runs as a PlatformIO pre-build script (extra_scripts in platformio.ini).

Why: the SQLite library (Sqlite3Esp32) compiles the 250 000-line SQLite
amalgamation plus its own file layer, and GCC prints a hundred or so warnings
for it ("discards 'const' qualifier", unused variables, a deprecated header).
They are harmless, can't be fixed without patching the library, and bury the
warnings that matter in our own code, which keeps -Wall.

How: a build middleware sees every source file before it is compiled, the
library's files included, together with the build environment of the library
it belongs to (each library is built in its own copy of the environment). For
a file of one of the listed libraries it adds -w to that library's
environment, so only that library's files are affected.

The middleware deliberately doesn't compile the file itself (env.Object):
on Windows the espressif32 platform registers a middleware of its own
(include path shortening, against command-line length limits) that runs after
this one and expects a source file, not an object file. Changing the
environment and passing the source on lets every middleware run, and the last
one compiles the file with -w in its flags.
"""

Import("env")  # noqa: F821 - provided by PlatformIO (SCons)

# Matched against the source file's path, so a library is recognised whether
# PlatformIO keeps it under .pio/libdeps/<env>/ or elsewhere.
QUIET_LIBS = ("Sqlite3Esp32",)


def quiet_third_party(env, node):
    path = node.get_abspath().replace("\\", "/")
    if any("/" + lib + "/" in path for lib in QUIET_LIBS) and "-w" not in env["CCFLAGS"]:
        env.Append(CCFLAGS=["-w"])
    return node


env.AddBuildMiddleware(quiet_third_party)  # noqa: F821
