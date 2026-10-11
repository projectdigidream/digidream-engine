# Deliberately broken mob tables

Each of these is `maps/mobs/doom.lua` with exactly one thing wrong, and
`engine/tools/mobcheck` must **reject every one of them**. They are the control for
the loader: a validator nobody has watched fail is a validator that might be
returning "clean" for the wrong reason, and this one already accepted
`move = "charge!"` on its first run because state fields were checked for their
names and never for their values.

Run them with `engine/scripts/check_mobs.sh`, which is wired into `engine/scripts/test.sh`.

| file | what is wrong |
|---|---|
| `typo_action.lua` | an action that does not exist (`hitscn`) |
| `typo_arg.lua` | an argument that does not exist (`dammage`) |
| `missing_arg.lua` | a required argument omitted (`damage`) |
| `bad_enum.lua` | an enum value outside its list (`move = "charge!"`) |
| `bad_state_ref.lua` | `next` names a state the mob does not have |
| `bad_initial.lua` | `initial` names a state the mob does not have |
| `bad_missile.lua` | `missile` names an undefined missile |
| `undeclared_var.lua` | a var used without being declared in `vars` |
| `inexact_number.lua` | a non-integral Lua number where fx is wanted |
| `ticks_no_next.lua` | a state that runs out with nowhere to go |
| `seq_after_end.lua` | a sequence entry past the end of its state |
| `missile_damage.lua` | a `damage` field on a missile (damage is an `on_hit` action); written by hand, not generated |

Generated from the good table by `engine/tools/mobcheck/make_controls.py`, so they
stay one edit apart from it: a control that has drifted into being broken for a
second reason is no longer testing the thing it names.
