# Project instructions

## Control guides: keep them up to date

The repo has two user guides that must always match the code:

- `playercontrols.md`: the basic controls for players, ordered from most to least important.
- `gmcontrols.md`: the complete guide to every control and command for the GM.

Whenever a change adds, removes or changes anything a user does or sees, update the guides in the same change. That covers:
- console commands, their arguments, short forms and messages;
- key or mouse bindings and tools;
- what players are allowed to do;
- limits such as roll or claim limits;
- hosting, joining and saving behaviour;
- command-line options and environment variables.

This is a standing task: check the guides after every change.
- The guides describe only what is in the game. Never add "coming soon" notes for features that are approved or
  planned but not built.
- A feature goes into the guides in the commit that implements it.

Check `src/input.c`, `src/console.c`, `src/session.c`, `src/ops.c` and `src/tools.c` against both files.
- `gmcontrols.md` covers everything.
- `playercontrols.md` covers only what a player can use, and stays short and ordered by importance.
- Also keep the Controls section of `README.md` consistent with them.

## Test mode: keep it up to date

`:tm` / `testmode` (GM only, the whole table) lets the GM try features without their requirements. While it is on,
move actions ignore initiative, the turn, used actions, Sprint's earlier move and stamina, but their effects still
apply (the action is used, stamina is spent but never below 0). See `src/moves.h` and `gh->testMode`.

This is a standing task, like the guides:
- When a new feature adds a requirement (a turn, a cost, a cooldown, a prerequisite, a limit), decide whether test
  mode lifts it. **Ask the user when unsure.**
- Update the test-mode code, its unit test (`test_test_mode` in `tests/core_test.c`) and the Test mode section of
  `gmcontrols.md` (and the note in `playercontrols.md`) in the same change.
