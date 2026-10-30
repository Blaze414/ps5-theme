# Contributing

Thank you for helping. This page says what a change needs to be accepted, and why: the plugin runs with a console's
privileges, so the bar is set by what it could break.

## The rules no change may break

These are not style preferences. A change that breaks one is not merged, however useful it is.

1. **Never write the system.** Nothing under `/system` or `/system_ex` (or any other system folder) is opened for
   writing. Themes are read-only overlays. Every write goes through the write guard (`src/fsguard.c`).
2. **Only media, checked.** A new kind of themable file needs a check against the system file it replaces, with tests
   for files that must be refused, and fuzzing for its parser.
3. **No code in other processes.** No hooking, injecting or patching the home screen or any other process.
4. **Nothing in a sandbox while the system builds it.** Nothing goes into the home screen's sandbox before its process
   is watched, nothing into any sandbox while it restarts. See
   [Design decisions](docs/decisions.md#nothing-in-a-sandbox-while-the-system-builds-it): breaking this kept the
   console from waking, twice.
5. **No one else's files.** Sony's system files, icons, artwork, fonts, sounds and libraries, and games' pictures, are
   never added to the repository, a release or the manager's page. The docs' screenshots use stand-ins
   (`tools/docshots.mjs`).
6. **Read-only on someone else's console.** Investigating on a console means reading: its logs, its mount table, its
   files over FTP. Nothing is changed on a console except by its owner, on purpose.

## Setting up

You need Docker, Python 3 and git. Nothing else is installed on your machine.

```bash
./build.sh                     # the PS5 build, in build/
tests/host/run.sh              # the end-to-end tests against a fake console
tests/host/run.sh serve        # the plugin and manager on http://localhost:18095, for trying the page
```

[Building](docs/building.md) and [Testing](docs/testing.md) have the rest; [Architecture](docs/architecture.md) is
the map of the code.

## Making a change

- **One change per pull request**, with what it fixes or adds and how you know it works.
- **A bug fix comes with a test** that fails without it. Bugs found on a console get one too, on the fake console:
  [Testing](docs/testing.md#problems-found-on-the-console-and-the-tests-that-keep-them-fixed) lists each such bug and
  its test.
- **Everything the CI runs must pass:** the end-to-end tests with real mounts, with mounts emulated and under
  ThreadSanitizer, the codec cross-checks, the fuzzing, the PS5 build, the reproducible build and the docs checks.
  Run at least `tests/host/run.sh` and `python3 tools/check_repo.py` before pushing.
- **Threads:** shared state is either main-thread only, an atomic, or under one of the locks in
  [Architecture](docs/architecture.md#locks-and-the-order-they-are-taken-in), taken in its order. Say which in a
  comment.
- **Docs in the same change.** What a user sees goes in the guides and [the manager's tour](docs/manager.md); a
  problem worth knowing about goes in [Challenges](docs/challenges.md); a choice that rules something out goes in
  [Design decisions](docs/decisions.md). Screenshots come from `tools/docshots.mjs`.
- **The version** goes up when the plugin's files change, in the `Makefile`, `src/theme.h`, the README and the tests
  together (`tools/check_repo.py` checks they agree). The update folder only takes a newer version.

## Writing for people

The manager and the console's notifications speak to people who don't know how the plugin works:

- say what happens, in plain words: "To see the new icons, put the console in rest mode", not "ShellUI caches the
  appmeta";
- say what to do when something fails, not what failed internally;
- no exclamation marks, no marketing words, nothing a person would have to look up.

Code comments say why, in the same plain words.

## Testing on a console

A change that touches what the console does (mounts, the home screen, updates, rest mode) is tried on a console
before release. If you have one, the [hardware testing guide](docs/hardware-testing.md) has the steps and a form for
the results; reports from consoles other than the author's are very welcome, and are listed as independent results.

## Commits

A summary line that says what changed for the user (`1.80: nothing in a sandbox while the home screen restarts`),
then the details. AI tools may be used; say so in the commit (`Co-Authored-By:`), and test the result as you would
your own.

## Reporting

- **A bug:** open an issue with the bug form: what you did, what happened, the firmware, and the Activity log (the
  manager's Activity page; its text can be selected and copied).
- **A security problem:** report it privately, as in
  [Security](docs/security.md#reporting-a-security-problem); don't open a public issue for it.

## Licence

Contributions are under the project's licence, GPL-3.0-or-later.
