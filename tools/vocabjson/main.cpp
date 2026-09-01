//
// Emit maps/scripts/trigger_vocab.lua as JSON, on stdout.
//
//   build/lua/trigger_vocab_json . > maps/scripts/trigger_vocab.json
//   build/lua/trigger_vocab_json . maps/scripts/mob_vocab.lua mob_vocab \
//       > maps/scripts/mob_vocab.json
//
// The trigger vocabulary is normative Lua: the runtime validates against it and
// docs/TRIGGERS.md is written from it. The editor is JavaScript in a browser and
// cannot read a Lua table -- and a Lua reader written in JS would be a second
// implementation of a grammar, which is the trap `tools/mapc/main.cpp` and the
// editor's .map parser already have to pay a hash control for.
//
// So the table is read HERE, by the real interpreter the server uses, and
// translated once. server/tests/test_lua_triggers.cpp asserts the checked-in
// JSON still matches what this produces, so the two cannot drift apart quietly.
//
// It runs the same tools/vocabjson/emit.lua the test does, for the same reason:
// a generator and its checker that each knew how to serialise would be two
// implementations of the thing this program exists to avoid having two of.
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
    std::string root = ".";
    if (argc > 1) root = argv[1];
    else if (const char* e = std::getenv("DNP_ROOT")) root = e;

    Vm vm;
    VmConfig cfg;
    // Full, because this is a build tool run by a developer over a file in this
    // repository -- not a server loading somebody else's map.
    cfg.trust = Trust::Full;
    if (!vm.open(cfg)) {
        std::fprintf(stderr, "cannot open Lua: %s\n", vm.last_error());
        return 2;
    }

    // Which vocabulary. Defaults to the trigger one, which is what every
    // existing caller wants and is why this program is still named for it.
    //
    // It is general because there is now a SECOND normative vocabulary --
    // maps/scripts/mob_vocab.lua, whose own header says "the mob editor's UI is
    // generated from it" -- and the alternative was a second program that did
    // the same thing to a different global. Two generators is two places for
    // the JSON to stop matching the Lua, which is the one failure this whole
    // arrangement exists to prevent.
    std::string vocab_rel = "maps/scripts/trigger_vocab.lua";
    std::string global    = "trigger_vocab";
    if (argc > 2) vocab_rel = argv[2];
    if (argc > 3) global = argv[3];

    const std::string vocab = root + "/" + vocab_rel;
    const std::string emit  = root + "/tools/vocabjson/emit.lua";

    // emit.lua reads this to decide what to serialise. A global rather than an
    // argument because it is a script being run, not a function being called.
    lua_pushstring(vm.state(), global.c_str());
    lua_setglobal(vm.state(), "VOCAB_NAME");

    if (!vm.run_file(vocab.c_str(), vocab_rel.c_str())) {
        std::fprintf(stderr, "%s: %s\n", vocab.c_str(), vm.last_error());
        return 1;
    }
    if (!vm.run_file(emit.c_str(), "emit.lua")) {
        std::fprintf(stderr, "%s: %s\n", emit.c_str(), vm.last_error());
        return 1;
    }

    lua_getglobal(vm.state(), "VOCAB_JSON");
    const char* json = lua_tostring(vm.state(), -1);
    if (json == nullptr) {
        std::fprintf(stderr, "emit.lua produced nothing\n");
        return 1;
    }
    std::fputs(json, stdout);
    return 0;
}
