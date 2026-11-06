# Testing on your own console

Every release is tried on the author's PS5 before it is published, but one console is one firmware, one model and
one set of homebrew. This page is for anyone who wants to try a release on theirs and say how it went: what to have
ready, how to get back if something goes wrong, the steps, and how to report. Results from consoles other than the
author's are listed as independent results below. Part of the [PS5 Themes](../README.md#documentation)
documentation.

**On this page:**

- [Before you start](#before-you-start)
- [If something goes wrong](#if-something-goes-wrong)
- [The steps](#the-steps)
- [Reporting](#reporting)
- [Consoles tested](#consoles-tested)
- [Wanted](#wanted)

## Before you start

- A jailbroken PS5 on firmware 13.60 or lower, with etaHEN or PS5 Payload Manager.
- A phone or computer on the same network, to open the manager.
- 15 minutes, and the console's power button within reach.
- The release's files, checked against the checksums in its notes (or rebuilt yourself: see
  [Building](building.md#reproducible-builds)).

The plugin never writes the system: everything it shows goes away with **No theme**, or with a restart. Still, this
is a test of new software on your console; don't start it right before you need the console.

## If something goes wrong

- **The theme looks wrong:** in the manager's library, choose **No theme**.
- **The manager doesn't answer:** start ps5-theme again; if that doesn't help, start `ps5-theme-stop.elf`, which stops
  it and leaves the theme.
- **The console doesn't wake from rest mode, or the home screen doesn't come back:** hold the console's power button
  until it beeps twice and switches off, then start it again. The theme comes back with the plugin; choose **No
  theme** if you'd rather it didn't. Then report it, with the log: this is the most useful report there is.
- **To remove everything:** see [Removing](../README.md#removing).

## The steps

Do them in order; each says what you should see. The log is in the manager: the gear at the top right, then **Log**
(**Activity** before 1.88). The lines to look for are quoted.

1. **Start.** Install the release as the README says and start it. *Expected:* a notification "PS5 Themes is ready"
   with the plugin's icon and the manager's address (1.88 on); the **Themes** tile on the home screen; in the log,
   "ps5-theme 1.xx starting" and "web: listening on port 8095".
2. **Allow your phone or computer.** Open `http://<console-ip>:8095` on it. *Expected:* **Allow this device**;
   **Show a code on the console** puts six digits in a notification on the console; typed in, the manager opens. In
   the log, "access: a code was shown on the console for the device at …" and "access: allowed …". A wrong code
   says so. (Since 1.88; on an earlier release, skip it.)
3. **A theme.** Create a theme with a background picture (Home: Game Library) and apply it. *Expected:* the banner
   says it was applied and how many files were checked, and what needs rest mode to show; the background on the
   console as it says; in the log, a line ending in "s (preparing …, switching …, checking …)".
4. **Apply again.** Apply the same theme again. *Expected:* a second or two; "… of … folder(s) reused".
5. **Icon style.** In **Games and apps**, choose a shape and the theme's background, style the icons, save and apply,
   then use rest mode once. *Expected:* after waking, the home row's icons in the shape, the console's own tiles
   too.
6. **Rest mode, three times.** Put the console in rest mode and wake it, three times, the last one a minute after
   the second. *Expected:* it wakes every time, with the theme; in the log, for each, "ShellUI exited: … taken out
   of its sandbox … and of NPXS40101_…, NPXS40102_… (closing with it) …, as its exit began; up … s", "ShellUI gone …
   ms after its exit began, … ms after ours were out", then "watching ShellUI (pid …, sandbox _…)" within about a
   second, and "5 s after the home screen exited, the system had taken down the sandboxes that closed with it". If it
   doesn't wake, hold the power button until it beeps twice, start it, and report it with the log: the log says
   what held the home screen up (see
   [Challenges](challenges.md#empty-folders-the-system-removed-at-rest-mode)).
7. **A game installed.** With **Style ones installed later** (under **Games and apps**) on, install any game or homebrew app.
   *Expected:* the console says it is installing, and about 2 minutes after it finishes, that its icon was styled.
8. **No theme.** Choose **No theme**. *Expected:* the console's own look; in the log, "reverted … mount(s)".
9. **Restart.** Apply the theme again, then restart the console and start the plugin as you do. *Expected:* the theme
   comes back by itself ("putting '…' back").
10. **Update.** Put the same release's `ps5-theme.elf` in `/data/ps5-theme/update/` (upload as `.part`, then rename).
   *Expected:* nothing happens: "not newer than this copy". With a newer release it hands over and keeps the theme.

## Reporting

Open an issue with the **Hardware test report** form, or copy this into one:

```
Release:            (e.g. 1.88; checksums matched: yes/no)
Console:            (model: launch disc / digital / slim / pro; firmware)
Jailbreak:          (etaHEN version, or Payload Manager version, and how it was loaded)
Other homebrew:     (anything else running: ShadowMountPlus, Itemzflow, ...)
Steps 1 to 10:      (for each: as expected / not, and what you saw)
Log:                (the manager's Settings › Log, copied)
```

A failure with its log is worth more than a pass without one. Nothing in the report should be personal: the log
holds paths, title IDs and your console's network address on your own network, nothing else.

## Consoles tested

**By the author** (every release; details in [Tests on the console](testing.md#tests-on-the-console)):

| Release | Console | Firmware | Jailbreak | Result |
|---|---|---|---|---|
| 1.80 | PS5 | 13.42 | etaHEN 2.5B (unified), PS5 Payload Manager | the update from 1.79, Apply again (2.0 s), three rests in a row, each woke; steps 4, 6 and 8 on 1.73 to 1.79 |

**Independent results** (from anyone else): none yet. Yours would be the first; it is the most useful thing a
non-programmer can give this project.

## Wanted

Every release so far was tried on one console: a PS5 on 13.42 with etaHEN 2.5B and PS5 Payload Manager. Any of these
would be the first result of its kind:

| What | Why it matters |
|---|---|
| Firmware 13.00, 13.20 or 13.60 | The plugin is built for up to 13.60; the interface's folders and its sandboxes may differ |
| Firmware 12.xx or 11.xx | The overlays, the home screen's sandbox and the tile's registration are found on the console, but not tried there |
| A PS5 Pro, a Slim, or a Digital Edition | Same software, different hardware: rest mode and storage may behave differently |
| Another loader (nexHEN, a different etaHEN build, an ELF loader) | How the plugin is started and updated depends on it |
| Other homebrew running alongside (ShadowMountPlus, Itemzflow, a game mounter) | Mounts of theirs share the table with the plugin's |
