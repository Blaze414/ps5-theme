"use strict";

/* ---------------------------------------------------------------- interface sounds */

/* What each interface sound is for, by the console's name for it (snd_<name>), in the order shown. */
const UI_SOUND_NAMES = {
  focus_move: "Moving between items", enter: "Selecting", cancel: "Going back", cannot_move_focus: "Reaching the end of a list",
  change_space: "Switching between Games and Media", change_panel: "Changing panel", change_panel2: "Changing panel (2)",
  open_control_center: "Opening the Control Center", close_control_center: "Closing the Control Center",
  open_option_menu: "Opening the options menu", close_option_menu: "Closing the options menu", take_screenshot: "Taking a screenshot",
  open_home: "Going to the home screen",
  open_dialog: "A dialog opening", open_error_dialog: "An error dialog opening", yes_in_dialog: "Confirming in a dialog",
  no_in_dialog: "Declining in a dialog", neutral_in_dialog: "Another choice in a dialog", error: "An error",
  switch_on: "Turning a switch on", switch_off: "Turning a switch off", checkbox_on: "Ticking a box", checkbox_off: "Unticking a box",
  slider_level_meter: "Moving a slider",
  informative_toasts_something_to_read: "A notification", interactive_toasts_something_to_do: "A notification to act on",
  error_toasts_something_is_broken: "An error notification", trophy_toast: "A trophy", platinum_trophy_toast: "A platinum trophy",
  join_party: "Joining a party", leave_party: "Leaving a party", log_out: "Logging out", pass_code: "Entering a passcode",
  face_recognition: "Face recognition", open_osk: "The keyboard opening", text_input: "Typing", focus_move_in_keyboard: "Moving on the keyboard",
  backspace: "Deleting on the keyboard", error_in_text_box: "A typing error", voice_recognition_for_osk_starts: "Voice typing starting",
  voice_recognition_for_osk_ends: "Voice typing ending", voice_recognition_for_osk_error: "Voice typing failing",
  agent_wake_up: "A voice command listening", agent_utterance_received: "A voice command heard", agent_no_results: "A voice command not understood",
  purchase_universal_checkout: "A purchase", open_discover_tour: "Opening the tour", psvr2_setup_section_finish: "A PS VR2 setup step done",
  psvr2_room_scale_finish: "The PS VR2 play area done", set_pad_orientation_1: "Controller orientation (1)",
  set_pad_orientation_2: "Controller orientation (2)", undefined: "A sound without its own",
};
/* The interface's sounds by where you hear them. The first three groups are open from the start (they hold the sounds
   people change most: moving, selecting, going back, L1/R1 — snd_change_space, heard on 13.42 — the Control Center,
   the Options button and screenshots); the others fold away, and one holding a replaced sound opens. A sound in no
   group goes to Other. */
const UI_SOUND_GROUPS = [
  { id: "move", title: "Moving around", open: true, ids: ["focus_move", "enter", "cancel", "cannot_move_focus", "open_home"] },
  { id: "tabs", title: "Tabs and panels", open: true, ids: ["change_space", "change_panel", "change_panel2"] },
  { id: "menus", title: "Control Center, options and screenshots", open: true,
    ids: ["open_control_center", "close_control_center", "open_option_menu", "close_option_menu", "take_screenshot"] },
  { id: "dialogs", title: "Dialogs and errors", ids: ["open_dialog", "open_error_dialog", "yes_in_dialog", "no_in_dialog", "neutral_in_dialog", "error"] },
  { id: "controls", title: "Switches, boxes and sliders", ids: ["switch_on", "switch_off", "checkbox_on", "checkbox_off", "slider_level_meter"] },
  { id: "toasts", title: "Notifications and trophies",
    ids: ["informative_toasts_something_to_read", "interactive_toasts_something_to_do", "error_toasts_something_is_broken", "trophy_toast", "platinum_trophy_toast"] },
  { id: "keyboard", title: "Keyboard and voice",
    ids: ["open_osk", "text_input", "focus_move_in_keyboard", "backspace", "error_in_text_box", "voice_recognition_for_osk_starts",
      "voice_recognition_for_osk_ends", "voice_recognition_for_osk_error", "agent_wake_up", "agent_utterance_received", "agent_no_results"] },
  { id: "account", title: "Parties, sign-in and purchases", ids: ["join_party", "leave_party", "log_out", "pass_code", "face_recognition", "purchase_universal_checkout"] },
  { id: "setup", title: "Set-up, controller and PS VR2", ids: ["open_discover_tour", "psvr2_setup_section_finish", "psvr2_room_scale_finish", "set_pad_orientation_1", "set_pad_orientation_2"] },
];
const uiSoundGroup = s => /^stick_sensitivity_/.test(s.key) ? "setup" : (UI_SOUND_GROUPS.find(g => g.ids.includes(s.key)) || { id: "other" }).id;
const UI_SOUND_SETS = { "": "Default", 1: "PlayStation", 2: "PlayStation 2", 3: "PlayStation 3", 4: "PlayStation 4" };

/* The interface's sounds (default ones, then each Appearance theme's: snd_2_focus_move is the PlayStation 2
   theme's), in the order above. The controller's vibration patterns (ids ending in _vibration) are left out:
   they stay the console's. */
function uiSounds(r) {
  const order = Object.keys(UI_SOUND_NAMES), out = [];
  for (const f of r.files) {
    if (f.elem !== "sounddata" || f.type !== "sound/vag" || /_vibration$/.test(f.id)) continue;
    const m = /^snd_(?:([1-4])_)?(.+)$/.exec(f.id);
    if (!m) continue;
    const set = m[1] || "", key = m[2], stick = /^stick_sensitivity_(\d+)$/.exec(key);
    const name = stick ? `Stick sensitivity (${+stick[1]})` : UI_SOUND_NAMES[key] || key.replace(/_/g, " ");
    const rank = order.includes(key) ? order.indexOf(key) : order.length + (stick ? +stick[1] : 100);
    out.push({ f, id: f.id, set, key, name, rank, label: set ? `${name} (${UI_SOUND_SETS[set]} theme)` : name });
  }
  return out.sort((a, b) => (a.set || "0").localeCompare(b.set || "0") || a.rank - b.rank || a.id.localeCompare(b.id));
}

/* The resource file that holds the sounds (Sce.PlayStation.PUI_UI3.rco on 13.42, read first) and its parse. */
async function uiSoundFile(onWait) {
  for (const path of rcoPaths()) {
    if (rcoFailed.has(path)) throw new Error(rcoFailed.get(path));
    if (onWait && !(state.rco && state.rco.has(path))) onWait(path);
    const r = await rcoStock(path);
    if (r && uiSounds(r).length) return { path, r };
  }
  return null;
}

const UI_SOUND_LATE = 0.05;   /* a theme's sound that gets loud later than this is offered Start at the hit */
let uiSoundsLast = null;   /* the list as last shown, and for which theme: shown again at once when the section is redrawn */
function uiSoundsBox() {
  if (uiSoundsLast && uiSoundsLast.id === state.editId) return uiSoundsLast.html;
  const first = rcoPaths()[0];
  return first ? loadingMarker(`stock:${first}`, "Reading the interface sounds…", colorsWaitNote(first)) : "";
}
async function uiSoundsShow() {
  const box = $("#ui-sounds");
  if (!box) return;
  const id = state.editId, shown = uiSoundsLast && uiSoundsLast.id === id;
  const wait = (key, label, path) => {
    if (!box.isConnected || shown) return;
    box.innerHTML = loadingMarker(key, label, colorsWaitNote(path));
    paintLoads(box);
  };
  let html = "", replaced = 0;
  try {
    const found = await uiSoundFile(path => wait(`stock:${path}`, "Reading the interface sounds…", path));
    if (found) {
      const { path, r } = found;
      if (!rcoThemeReady(path)) wait(`theme:${path}`, "Reading this theme's sounds…", path);
      const changes = await rcoChanges(path), list = uiSounds(r);
      const mineOf = s => changes.has(`${s.f.el}\t${s.f.attr}`);
      const length = s => { try { const d = rcoData(r, s.f); return `${(vagMaxSamples(d) / vagInfo(d).rate).toFixed(2)} s`; } catch (e) { return ""; } };
      /* When the theme's sound gets loud, if later than UI_SOUND_LATE (heard as a delay after pressing) */
      const late = s => {
        try { const d = vagDecode(changes.get(`${s.f.el}\t${s.f.attr}`)), t = uiSoundHit(d.chans, d.rate) / d.rate; return t > UI_SOUND_LATE ? t : 0; }
        catch (e) { return 0; }
      };
      const row = s => {
        const mine = mineOf(s), at = `data-path="${esc(path)}" data-id="${esc(s.id)}"`, wait = mine ? late(s) : 0;
        return `<div class="row-item${mine ? " replaced" : ""}">
          <div class="info"><div class="file">${esc(s.name)}${mine ? ` <span class="tag on">Replaced</span>` : ""}</div>
            <div class="dir">${esc(s.id)} · ${length(s)}${wait ? ` · yours gets loud ${wait.toFixed(2)} s after you press` : ""}</div></div>
          <div class="acts"><button class="btn sm ghost" data-act="uisnd-play" ${at}>Play</button>
            ${mine ? `<button class="btn sm ghost" data-act="uisnd-play" ${at} data-mine="1">Play yours</button>` : ""}
            ${wait ? `<button class="btn sm" data-act="uisnd-hit" ${at} title="Cut the quieter sound before its hit (keeping the last 10 ms, faded in), so it plays the moment you press">Start at the hit</button>` : ""}
            <button class="btn sm" data-act="uisnd-pick" ${at}>${mine ? "Replace" : "Attach"}</button>
            ${mine ? `<button class="btn sm ghost danger" data-act="uisnd-reset" ${at}>Reset</button>` : ""}</div></div>`;
      };
      const defaults = list.filter(s => !s.set);
      const sets = Object.keys(UI_SOUND_SETS).filter(k => k && list.some(s => s.set === k));
      /* A group as a fold: its name, how many sounds and how many replaced; open as it was left, or when it is one
         of the first three or holds a replaced sound. */
      const opened = state.uiSoundOpen || (state.uiSoundOpen = {});
      const fold = (key, title, items, first) => {
        if (!items.length) return "";
        const mine = items.filter(mineOf).length;
        const open = key in opened ? opened[key] : first || mine > 0;
        return `<details class="fold" data-sound-group="${esc(key)}"${open ? " open" : ""}>
          <summary><span class="fold-title">${esc(title)}</span><span class="fold-count">${plural(items.length, "sound")}${mine ? ` · ${mine} replaced` : ""}</span></summary>
          <div class="rows">${items.map(row).join("")}</div></details>`;
      };
      const groups = [...UI_SOUND_GROUPS, { id: "other", title: "Other" }]
        .map(g => fold(g.id, g.title, defaults.filter(x => uiSoundGroup(x) === g.id), !!g.open)).join("");
      replaced = list.filter(mineOf).length;
      html = `<h3 class="sub-head">Interface sounds</h3>
        <p class="note">Moving between items, selecting, going back and the rest. They change after rest mode.${help(`Any audio file, converted on this device to the console's own sound format, as loud as the sound it replaces and no longer: a longer one is cut with a short fade (moving between items is 0.41 s on 13.42). It starts at its hit, as the console's do: silence and quieter sound before it are cut (the last 10 ms kept, faded in), so it plays the moment you press. The PlayStation to PlayStation 4 sounds are the Appearance themes' own (Settings > Appearance and Sound). The controller's vibration patterns stay the console's. The sounds are kept in the interface's resource file (${baseName(path)}), so Interface icons shows that file as changed too.`)}</p>
        <div class="folds">${groups}
        ${sets.map(k => fold(`set${k}`, `${UI_SOUND_SETS[k]} look (Appearance themes)`, list.filter(s => s.set === k), false)).join("")}</div>`;
    }
  } catch (e) {
    html = `<p class="note">The interface sounds could not be read: ${esc(e.message)} <button class="btn sm" data-act="uisnd-retry">Try again</button></p>`;
  }
  if (state.editId !== id) return;
  const before = uiSoundsLast && uiSoundsLast.id === id ? uiSoundsLast.replaced : 0;
  uiSoundsLast = { id, html, replaced };
  if (replaced !== before) return renderAspect(aspectById("sounds"));   /* the section's count too; shown from uiSoundsLast */
  if (box.isConnected) box.innerHTML = html;
}

/* Play the console's sound, or this theme's (`mine`), decoded on this device. */
let uiSoundAudio = null;
async function playUiSound(path, id, mine) {
  const r = await rcoStock(path), s = r && uiSounds(r).find(x => x.id === id);
  if (!s) return;
  try {
    const bytes = mine ? (await rcoChanges(path)).get(`${s.f.el}\t${s.f.attr}`) : rcoData(r, s.f);
    if (!bytes) return;
    const d = vagDecode(bytes);
    if (uiSoundAudio) { uiSoundAudio.pause(); URL.revokeObjectURL(uiSoundAudio.src); }
    uiSoundAudio = new Audio(URL.createObjectURL(pcmWav(d.chans, d.rate)));
    await uiSoundAudio.play();
  } catch (e) { toast(`${s.label}: ${e.message}`, "err"); }
}

function replaceUiSound(path, id, file) { return rcoQueued("Adding your sound", () => replaceUiSoundNow(path, id, file), true); }
async function replaceUiSoundNow(path, id, file) {
  const r = await rcoStock(path), s = r && uiSounds(r).find(x => x.id === id);
  if (!s) return;
  const p = progressToast(`Converting your sound for ${s.label.toLowerCase()}…`);
  try {
    const stock = rcoData(r, s.f), info = vagInfo(stock);
    const chans = await decodeAudio(file, info.rate, info.channels);
    const prep = uiSoundPrepare(chans, info.rate, vagDecode(stock).chans, vagMaxSamples(stock));
    const made = vagEncode(stock, prep.chans);
    const changes = await rcoChanges(path);
    changes.set(`${s.f.el}\t${s.f.attr}`, made.bytes);
    p.set(`Sending ${baseName(path)} to the console…`, 0.6);
    await rcoSave(path, changes);
    const notes = [prep.lead >= 0.02 ? `starting at its hit: ${prep.lead.toFixed(2)} s of quieter sound before it cut` : "",
                   prep.cut ? `cut to ${prep.seconds.toFixed(2)} s, the console's length` : ""].filter(Boolean);
    p.done(`${s.label}: replaced and verified${notes.length ? ` (${notes.join("; ")})` : ""}. Apply the theme, then put the console in rest mode and wake it.`, "ok");
  } catch (e) {
    p.done(`${s.label}: ${e.message}`, "err");
  }
  renderAspect(aspectById("sounds"));
}

/* A theme's sound made earlier, started at its hit: decoded, trimmed as a new one is, written again. */
function startUiSoundAtHit(path, id) { return rcoQueued("Starting the sound at its hit", () => startUiSoundAtHitNow(path, id), true); }
async function startUiSoundAtHitNow(path, id) {
  const r = await rcoStock(path), s = r && uiSounds(r).find(x => x.id === id);
  if (!s) return;
  const p = progressToast(`Starting ${s.label.toLowerCase()} at its hit…`);
  try {
    const stock = rcoData(r, s.f), changes = await rcoChanges(path), key = `${s.f.el}\t${s.f.attr}`, mine = changes.get(key);
    if (!mine) throw new Error("the theme has the console's own sound here");
    const d = vagDecode(mine), prep = uiSoundPrepare(d.chans, d.rate, vagDecode(stock).chans, vagMaxSamples(stock));
    changes.set(key, vagEncode(stock, prep.chans).bytes);
    p.set(`Sending ${baseName(path)} to the console…`, 0.6);
    await rcoSave(path, changes);
    p.done(`${s.label}: now starts at its hit (${prep.lead.toFixed(2)} s of quieter sound before it cut). Apply the theme, then put the console in rest mode and wake it.`, "ok");
  } catch (e) {
    p.done(`${s.label}: ${e.message}`, "err");
  }
  renderAspect(aspectById("sounds"));
}

function resetUiSound(path, id) { return rcoQueued("Putting the console's sound back", () => resetUiSoundNow(path, id)); }
async function resetUiSoundNow(path, id) {
  const r = await rcoStock(path), s = r && uiSounds(r).find(x => x.id === id);
  if (!s) return;
  const changes = await rcoChanges(path);
  changes.delete(`${s.f.el}\t${s.f.attr}`);
  try { await rcoSave(path, changes); toast(`${s.label} is back to the console's own.`, "ok"); }
  catch (e) { toast(e.message, "err"); }
  renderAspect(aspectById("sounds"));
}
