//
// The three C calls the iOS shell makes into the engine.
//
// iOS spawns nothing, so the authoritative server runs on a thread inside the
// app: the same server/src/main.cpp, entered through dnp_server_run() instead
// of main(). These wrappers live in server/src/embed.cpp (pending: see
// docs/STUDIO.md section 7 and docs/IOS.md) and are all the Swift side needs.
//
#ifndef DNP_BRIDGE_H
#define DNP_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Start the server on a thread with an 8 MB stack, with exactly the arguments
// dnp-server takes (--port, --module, --map, --spawn, --props). Returns 0
// when the thread is running, or a non-zero code when one already is.
int dnp_embed_start(int argc, const char* const* argv);

// Ask the running server to stop and wait for it. Safe when none is running.
void dnp_embed_stop(void);

// 1 while the server thread is alive.
int dnp_embed_running(void);

// The server's exit code once it has stopped (0 = asked to stop; 1 = it
// refused its arguments or its content, and said why on stderr, which the
// shell pipes to the system log).
int dnp_embed_exit_code(void);

#ifdef __cplusplus
}
#endif

#endif
