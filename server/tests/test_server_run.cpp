//
// The server, started twice in one process.
//
// WHY. A shell that cannot spawn (iOS) hosts the server on a thread and, for
// every play-test, stops it and starts it again on the level just written.
// main() was written to run once and exit: two statics inside it kept the
// resolved module list and a second run would have indexed into the first
// run's. This proves the entry point stops when asked, releases its port, and
// serves a different module the second time.
//
// The control: a run whose stop flag is never raised is still alive after the
// stop would have taken effect -- so "it stopped" means the flag did it.
//
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include "server_main.hpp"
#include "../src/embed.cpp"

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static bool alive_after(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return dnp_embed_running() != 0;
}

int main(int argc, char** argv) {
    // The two modules the tree always has; the port is one nothing else in
    // the suite uses.
    const char* root = argc > 1 ? argv[1] : ".";
    std::string a = std::string(root) + "/modules/shootrange";
    std::string b = std::string(root) + "/modules/starter";
    std::string doom = std::string(root) + "/modules/doom2";   // shootrange borrows its catalogue

    const char* run1[] = {"dnp-server", "--port", "34598", "--module", doom.c_str(), "--module", a.c_str(), "--spawn", "start"};
    CHECK(dnp_embed_start(9, run1) == 0);
    CHECK(alive_after(1500));
    CHECK(dnp_embed_start(9, run1) != 0);         // one at a time
    dnp_embed_stop();
    CHECK(dnp_embed_running() == 0);
    CHECK(dnp_embed_exit_code() == 0);

    // The same port, a different module: the first run's list must be gone
    // and its listen socket closed.
    const char* run2[] = {"dnp-server", "--port", "34598", "--module", b.c_str(), "--spawn", "start"};
    CHECK(dnp_embed_start(7, run2) == 0);
    CHECK(alive_after(1500));
    dnp_embed_stop();
    CHECK(dnp_embed_exit_code() == 0);

    // CONTROL: without the flag it keeps running.
    CHECK(dnp_embed_start(7, run2) == 0);
    CHECK(alive_after(600));
    CHECK(dnp_embed_running() == 1);
    dnp_embed_stop();

    std::printf("\ntest_server_run: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
