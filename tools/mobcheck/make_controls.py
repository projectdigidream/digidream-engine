# Regenerate maps/mobs/bad/ from maps/mobs/doom_monsters.lua: each file is the good
# table with exactly ONE thing wrong, so a control that fails is failing for
# the reason it names and not for some drift of its own.
#
#   python3 tools/mobcheck/make_controls.py
import os
src = open('maps/mobs/doom_monsters.lua').read()
cases = {
 'typo_action.lua':     ('action = "hitscan", damage = 15',   'action = "hitscn", damage = 15'),
 'typo_arg.lua':        ('action = "hitscan", damage = 15',   'action = "hitscan", dammage = 15'),
 'missing_arg.lua':     ('action = "hitscan", damage = 15, range = du(2048),',
                         'action = "hitscan", range = du(2048),'),
 'bad_state_ref.lua':   ('ticks = o.attack_ticks, next = "chase"',
                         'ticks = o.attack_ticks, next = "chace"'),
 'undeclared_var.lua':  ('action = "set_cooldown", var = "ready", ticks = tics(50)',
                         'action = "set_cooldown", var = "redy", ticks = tics(50)'),
 'bad_enum.lua':        ('anim = 1, move = "chase", face = "target"',
                         'anim = 1, move = "charge!", face = "target"'),
 'bad_missile.lua':     ('action = "missile", kind = "fireball"',
                         'action = "missile", kind = "firebal"'),
 'inexact_number.lua':  ('sight = o.sight or du(2048)', 'sight = o.sight or 42.7'),
 'ticks_no_next.lua':   ('ticks = tics(6), next = "chase",', 'ticks = tics(6),'),
 'bad_initial.lua':     ('initial = "look",', 'initial = "lok",'),
 'corpse_no_death.lua': ('''      death = {
        anim = 4, move = "stop", face = "hold", solid = false,
        ticks = o.death_ticks or tics(30), next = "death",
      },''', ''),
}
os.makedirs('maps/mobs/bad', exist_ok=True)
missing = []
for name, (a, b) in cases.items():
    if a not in src:
        missing.append(name); continue
    open('maps/mobs/bad/' + name, 'w').write(src.replace(a, b, 1))
if missing:
    raise SystemExit('these mutations no longer match doom.lua: ' + ', '.join(missing))
# seq_after_end needs a sequence entry past its state's end.
open('maps/mobs/bad/seq_after_end.lua','w').write(
    src.replace('{ at = tics(10), action = "hitscan", damage = 15',
                '{ at = tics(900), action = "hitscan", damage = 15', 1))
print(len(cases) + 1, 'controls written')
