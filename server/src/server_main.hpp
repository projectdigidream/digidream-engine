#pragma once
//
// The server as a function.
//
// main.cpp's main() is a one-line wrapper over this, so a shell that cannot
// spawn a process (iOS) -- or would rather not (Android, one code path for
// both phones) -- runs the same server on a thread. Same arguments as the
// command line; `stop` is polled once per loop turn and may be null.
//
// Returns what main() returns: 0 when asked to stop or when the link failed,
// 1 when the content or the arguments were refused (said on stderr), 2 for a
// malformed command line.
//
extern "C" int dnp_server_run(int argc, char** argv, const volatile int* stop);
