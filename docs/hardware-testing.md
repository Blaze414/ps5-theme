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
  theme** if you'd rather it didn't. Then report it, with the Activity log: this is the most useful report there is.
- **To remove everything:** see [Removing](../README.md#removing).

## The steps

Do them in order; each says what you should see. The Activity page (in the manager) is the plugin's log: the lines to
look for are quoted.

1. **Start.** Install the release as the README says and start it. *Expected:* a notification with the manager's
   address; the **Themes** tile on the home screen; in Activity, "ps5-theme 1.xx starting" and "web: listening on
   port 8095".
2. **A theme.** Create a theme with a background picture (Home: Game Library) and apply it. *Expected:* the banner
   says it was applied and how many files were checked, and what needs rest mode to show; the background on the
   console as it says; in Activity, a line ending in "s (preparing …, switching …, checking …)".
3. **Apply again.** Apply the same theme again. *Expected:* a second or two; "… of … folder(s) reused".
4. **Icon style.** In **Games and apps**, choose a shape and the theme's background, style the icons, save and apply,
   then use rest mode once. *Expected:* after waking, the home row's icons in the shape, the console's own tiles
   too.
5. **Rest mode, three times.** Put the console in rest mode and wake it, three times, the last one a minute after
   the second. *Expected:* it wakes every time, with the theme; in Activity, for each, "ShellUI exited: … taken out
   of its sandbox" then "watching ShellUI (pid …, sandbox _…)" within about a second.
6. **A game installed.** With **Style games and apps installed later** on, install any game or homebrew app.
   *Expected:* the console says it is installing, and about 2 minutes after it finishes, that its icon was styled.
7. **No theme.** Choose **No theme**. *Expected:* the console's own look; in Activity, "reverted … mount(s)".
8. **Restart.** Apply the theme again, then restart the console and start the plugin as you do. *Expected:* the theme
   comes back by itself ("putting '…' back").
9. **Update.** Put the same release's `ps5-theme.elf` in `/data/ps5-theme/update/` (upload as `.part`, then rename).
   *Expected:* nothing happens: "not newer than this copy". With a newer release it hands over and keeps the theme.

## Reporting

Open an issue with the **Hardware test report** form, or copy this into one:

```
Release:            (e.g. 1.80; checksums matched: yes/no)
Console:            (model: launch disc / digital / slim / pro; firmware)
Jailbreak:          (etaHEN version, or Payload Manager version, and how it was loaded)
Other homebrew:     (anything else running: ShadowMountPlus, Itemzflow, ...)
Steps 1 to 9:       (for each: as expected / not, and what you saw)
Activity log:       (the manager's Activity page, copied)
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
