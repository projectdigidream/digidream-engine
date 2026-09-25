//
// The server, hosted: ios/dnp_bridge.h's four calls over dnp_server_run.
//
// One server per process at a time, on a thread with an 8 MB stack -- iOS
// gives a secondary thread 512 KB and the server keeps its snapshot buffers
// and a level on the stack of main(). Stopping sets the flag the loop polls
// and joins; the loop turns every 50 ms at most, so a play-test restart is
// not something a person can see.
//
// No platform code: the shells pump stderr to their own log if they want it
// (Android's ProcessBuilder path does that by reading the pipe; a JNI host
// would dup2 stderr onto a pipe here).
//
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "server_main.hpp"

extern "C" {

namespace {
pthread_t g_thread;
volatile int g_stop = 0;
volatile int g_running = 0;
int g_exit = 0;
std::vector<std::string> g_args;      // the thread's own copy of argv
std::vector<char*> g_argv;

void* run(void*) {
    g_exit = dnp_server_run(static_cast<int>(g_argv.size()) - 1, g_argv.data(), &g_stop);
    g_running = 0;
    return nullptr;
}
}  // namespace

int dnp_embed_start(int argc, const char* const* argv) {
    if (g_running) return 1;
    g_stop = 0;
    g_exit = 0;
    g_args.assign(argv, argv + argc);
    g_argv.clear();
    for (std::string& a : g_args) g_argv.push_back(&a[0]);
    g_argv.push_back(nullptr);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    g_running = 1;
    const int rc = pthread_create(&g_thread, &attr, run, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) { g_running = 0; return 2; }
    return 0;
}

void dnp_embed_stop(void) {
    if (!g_running && g_argv.empty()) return;
    g_stop = 1;
    pthread_join(g_thread, nullptr);
    g_argv.clear();
    g_running = 0;
}

int dnp_embed_running(void) { return g_running; }

int dnp_embed_exit_code(void) { return g_exit; }

}  // extern "C"
