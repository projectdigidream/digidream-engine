//
// Validate a trigger table without standing up a server.
//
//   build/lua/triggercheck . maps/doors_trig.lua
//
// Exit 0 if the table loaded, 1 if it did not, 2 if the tool could not run.
// Every problem is printed by the runtime itself, naming the trigger and the
// argument -- this program only decides what to exit with.
//
// WHY IT EXISTS. A trigger table is fail-closed: one bad argument anywhere and
// NOTHING loads. That is the right call for a server, but it means an author
// with a typo sees a map where nothing works, and the message explaining why is
// on the server's stderr at startup. An editor wants that answer while somebody
// is still typing, and it should not have to compile a map and launch a stack
// to get it.
//
// WHAT IT DOES NOT ANSWER: whether the table fits a particular map. There is no
// geometry here, so expect_neighbour and expect_tag cannot be checked -- the
// runtime prints a line saying exactly that, so a clean run from this tool is
// never mistaken for a clean load on a server.
//
#include <cstdio>
#include <cstdlib>
#include <string>

#include "lua/vm.hpp"

extern "C" {
#include "lua.h"
}

using namespace dnp::server::lua;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: triggercheck <repo-root> <trigger-table.lua>\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::string table = argv[2];

    Vm vm;
    VmConfig cfg;
    // Full, so that a table using the `lua` action's `code` form can be
    // compiled here. Under Strict it could not be, and a linter that refused
    // what the server would accept is worse than no linter.
    cfg.trust = Trust::Full;
    if (!vm.open(cfg)) {
        std::fprintf(stderr, "cannot open Lua: %s\n", vm.last_error());
        return 2;
    }

    const char* const files[] = {
        "/tools/triggercheck/stub.lua",
        "/maps/scripts/trigger_vocab.lua",
        "/maps/scripts/triggers.lua",
    };
    for (const char* f : files) {
        const std::string path = root + f;
        if (!vm.run_file(path.c_str(), f + 1)) {
            std::fprintf(stderr, "triggercheck: %s\n", vm.last_error());
            return 2;
        }
    }

    // The table itself. A Lua error in the FILE (as opposed to a problem in the
    // table) is reported differently, because they are different mistakes: one
    // is a syntax error the author can see, the other is a rule they did not
    // know about.
    if (!vm.run_file(table.c_str(), table.c_str())) {
        std::fprintf(stderr, "triggercheck: %s does not run: %s\n",
                     table.c_str(), vm.last_error());
        return 1;
    }

    // Did anything actually load? A file that never calls triggers.load is not
    // an error worth failing on -- a map may legitimately have none -- but it
    // is worth saying, because "no problems found" over an empty table is the
    // most misleading clean bill there is.
    lua_getglobal(vm.state(), "triggers");
    lua_getfield(vm.state(), -1, "list");
    const int n = static_cast<int>(lua_objlen(vm.state(), -1));
    lua_pop(vm.state(), 2);

    if (n == 0) {
        std::fprintf(stderr, "triggercheck: %s loaded no triggers\n", table.c_str());
        return 1;
    }
    std::fprintf(stderr, "triggercheck: %s ok (%d triggers)\n", table.c_str(), n);
    return 0;
}
