# Synthetic-cat synchronization stress tests

Build separately with `cmake -S . -B build_synctest -A x64 -DMGMP_SYNCTEST=ON`,
then `cmake --build build_synctest --config Release`. The option defaults OFF;
normal builds compile no-op harness functions. Existing Release binaries are not
replaced. Both peers must use the same test build.

Generate `synctest_pool.txt` beside each test DLL using
`python tools/synctest_pool.py <resources.gpak> <output>`. This reads only real
class ability/passive pools, including advanced classes. Generated pools and
binary START captures contain game-derived data: keep them local, out of releases.

Enable `[auto] Enabled=1` and `[synctest] Enabled=1`, `Seed=20261102` in pvp.ini.
The host transforms copies of selected donor cats before EVERY START, including
rematches: fresh class, 4 cross-class active skills, 2 passives, level 8, adult age,
clean injury/mutation data and improved stats. The donors supply valid appearance
and movement data. All transformations happen on scratch clone 15, outside the
arena's 8 fighter slots. The resulting serialized cats go through the unchanged
START path on both peers. No cat is registered in the save.

Item preset is always Random (3–5 items per cat). Loner is excluded from the
generated passive pool so that repeated early friendly wipes do not dominate
coverage. Scenario rotation assigns charm, summons, revival, teleportation,
reactions, random/elemental effects, then mixed skills. Assignment does not force
the AI to cast: coverage reports separately record actual Ability::trigger entries.
Passive activation is not separately instrumented.

The synthetic harness also sends one explicit FaceTowards input before each
owned cat's turn. It goes through the same hook and ACTION queue as a manual
click, followed by the AI's ordinary ability/end-turn choice. PvP state hashes
include facing, so a rotation disagreement fails before a later backstab.
`FightMinutes=0` disables the per-fight timeout (the default). A supervising
script can still stop the whole run after the requested overall duration.

`synctest.log` records seeds, full generated skill/passive/stat/class manifests,
and trigger names. Arena logs record item kits. `synctest_NNNN.start` captures the
whole start packet: fighters, kits, rules, map bytes, simulation seed and battle ID.
To replay an incident from the house, set `[synctest] ReplayStart=<absolute path>`
on the HOST and start an online lobby. The first START uses that exact capture,
with only the rematch byte cleared to allow entry from the house. Use the same
game/mod build and data. Reproducing the input does not guarantee the AI makes
the same choices: a frame-sensitive decision can still differ.

Run `python tools/synctest_report.py <run-folder>` after stopping both games.
It writes coverage.json with paired hashes, assigned versus triggered skills,
and per-ability trigger counts. A trigger entry does not prove a successful hit.

Back up the whole profile before any launch. Stop both known test PIDs on the
first HALTED/crash, preserve both peers' logs and START captures, then restore the
original profile after both processes exit. Never auto-apply a fix during a run.
