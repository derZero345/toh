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
