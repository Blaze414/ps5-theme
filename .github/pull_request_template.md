## What this changes, and why

## How you know it works

- [ ] `tests/host/run.sh` passes (and the CI's other jobs)
- [ ] A bug fix comes with a test that fails without it
- [ ] `python3 tools/check_repo.py` passes; the docs say what changed
- [ ] Tried on a console, if it touches mounts, the home screen, updates or rest mode (which firmware?)

## The rules (CONTRIBUTING.md)

- [ ] Nothing writes the system; every write goes through the write guard
- [ ] Nothing goes into a sandbox while the system builds it, or while the home screen restarts
- [ ] No Sony files, icons or artwork, and no games' pictures, added anywhere
- [ ] Shared state is main-thread only, an atomic, or under a lock taken in its order
