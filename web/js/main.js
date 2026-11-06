"use strict";

/* ================================================================ events */

document.addEventListener("click", async e => {
  const el = e.target.closest("[data-act], .tile");
  if (!el || el.disabled) return;

  if (el.classList.contains("tile")) {
    const asp = aspectById(el.dataset.aspect);
    const file = await pickFile(asp.accept);
    if (file) attachToGroup(asp, el.dataset.group, file);
    return;
  }
  const id = el.dataset.id;
  const asp = el.dataset.aspect && aspectById(el.dataset.aspect);
  switch (el.dataset.act) {
    case "apply": return applyPressed(id);
    case "preview": return openPreview();
    case "pv-close": return previewClose();
    case "pv-view": pvState.view = el.dataset.view; return previewRender(state.editId);
    case "pv-refresh": homeDb = null; return previewRender(state.editId);
    case "export": return exportTheme(id);
    case "wav-play": return playWav(el);
    case "duplicate": return duplicateTheme(id);
    case "save": return saveTheme(id, el);
    case "discard": return discardTheme(id, el);
    case "delete": return armDelete(el, id);
    case "new": return newTheme();
    case "mix": return newTheme("mix");
    case "new-start": {
      const c = state.creating;
      if (!c || el.getAttribute("aria-disabled") === "true" || c.mode === el.dataset.v) return;
      c.mode = el.dataset.v;
      if (c.mode === "mix" && !state.mix) openMix();
      if (c.mode !== "mix") state.mix = null;
      return redrawCreate();
    }
    case "new-copy": if (state.creating) state.creating.copyOf = el.dataset.v; return redrawCreate();
    case "new-anyfw": if (state.creating) state.creating.anyFw = !state.creating.anyFw; return redrawCreate();
    case "mix-pick": if (state.mix) state.mix.picks[el.dataset.part] = el.dataset.v; return redrawCreate();
    case "mix-elem-pick": {
      const m = state.mix;
      if (!m) return;
      const own = m.elems[el.dataset.part] = m.elems[el.dataset.part] || {};
      if (el.dataset.v === "*") delete own[el.dataset.key]; else own[el.dataset.key] = el.dataset.v;
      return redrawCreate();
    }
    case "mix-fill": {
      const m = state.mix, info = m && m.themes.get(el.dataset.v);
      if (!info) return;
      for (const part of MIX_PARTS) m.picks[part.id] = mixHas(info, part) ? info.id : "";
      m.elems = {};
      return redrawCreate();
    }
    case "cancel-mix": return closeCreate();
    case "mix-elements": {
      const part = mixPartById(el.dataset.part), m = state.mix;
      if (!part || !m) return;
      m.open[part.id] = !m.open[part.id];
      if (!m.open[part.id]) delete m.elems[part.id];   /* one choice for the whole part again */
      redrawCreate();
      if (m.open[part.id] && part.rco && !m.rcoReady) mixLoadRco();
      return;
    }
    case "new-from-editor": return newTheme();
    case "cancel-new": e.preventDefault(); return closeCreate();
    case "import": {
      const file = await pickFile(".zip,application/zip");
      if (file) importFile(file).catch(err => toast(err.message, "err", 9000));
      return;
    }
    case "reset-all": {
      state.confirming = asp.id;
      renderAspect(asp);
      const keep = $(`#aspect-${asp.id} [data-act="cancel-off"]`);
      if (keep) keep.focus();
      return;
    }
    case "confirm-off":
      el.disabled = true; el.textContent = "Removing…";
      state.confirming = "";
      if (asp.mode === "music") await removeMusic();
      else await resetAssets(aspectAssets(asp));
      await refreshStatus();
      setAspectFlag(asp, false);
      toast(`${asp.title}: the console's own again in this theme.`, "ok");
      return renderAspect(asp);
    case "cancel-off": {
      state.confirming = ""; renderAspect(asp);
      const b = $(`#aspect-${asp.id} [data-act="reset-all"]`);
      if (b) b.focus();
      return;
    }
    case "attach-aspect": {
      const file = await pickFile(asp.mode === "music" ? "audio/*" : asp.accept);
      if (file) attachToAspect(asp, file);
      return;
    }
    case "fonts-latin": {
      const file = await pickFile(asp.accept);
      if (!file) return;
      await attachTo(aspectAssets(asp).filter(a => FONT_LATIN.test(baseName(a.path))), file, "Latin interface text");
      return renderAspect(asp);
    }
    case "attach-group": {
      const file = await pickFile(asp.accept);
      if (asp.id === "appearance") { const g = findGroup(asp, el.dataset.group); if (g) apFollow(g.assets[0].path); }
      if (file) attachToGroup(asp, el.dataset.group, file);
      return;
    }
    case "reset-group": {
      el.disabled = true; el.textContent = "Resetting…";
      const g = findGroup(asp, el.dataset.group);
      if (g) await resetAssets(g.assets);
      renderAspect(asp);
      if (g && asp.id === "appearance") restReminder();
      return;
    }
    case "reset-aspect": el.disabled = true; el.textContent = "Resetting…"; await resetAssets(aspectAssets(asp)); renderAspect(asp); if (asp.id === "appearance") restReminder(); return;
    case "remove-originals": {
      if (!armed.get("originals")) {
        armed.set("originals", true);
        el.classList.add("armed");
        el.textContent = "Press again to remove";
        setTimeout(() => { armed.delete("originals"); if (el.isConnected) { el.classList.remove("armed"); el.textContent = "Remove them"; } }, 4000);
        return;
      }
      armed.delete("originals");
      const had = state.sources.size, bytes = state.originalsBytes;
      try {
        await api("DELETE", "/api/theme/sources", { id: state.editId });
        await refreshStatus();
        toast(`Removed ${plural(had, "original")} (${fmtSize(bytes)}). Their options now apply when you attach a picture again.`, "ok", 7000);
      } catch (err) { toast(err.message, "err"); }
      await loadOriginals();
      ASPECTS.forEach(renderAspect);
      return;
    }
    case "remove-music":
      await removeMusic();
      toast("Home music is back to stock in this theme.", "ok");
      return renderAspect(aspectById("music"));
    case "convert-music":
      await remakeHomeMusic(state.editId);
      return renderAspect(aspectById("music"));
    case "choose-part": return openThemePart(state.editId);
    case "song-add": return addSong();
    case "song-part": return openSongPart(+el.dataset.pos);
    case "song-replace": return replaceSong(+el.dataset.pos);
    case "song-remove": el.disabled = true; el.textContent = "Removing…"; return removeSong(+el.dataset.pos);
    case "song-up": return moveSong(+el.dataset.pos, -1);
    case "song-down": return moveSong(+el.dataset.pos, 1);
    case "pl-play": return preview && !preview.timer ? stopPreview() : playPlaylist(false);
    case "pl-seam": return playPlaylist(true);
    case "rco-number": return numberPictures(el.dataset.path, el.dataset.vector === "1");
    case "rco-retry": return rcoRetry();
    case "rco-reset": await resetAssets([{ path: el.dataset.path }]); return renderAspect(aspectById("interface"));
    case "rco-pick": {
      const file = await pickFile("image/*,.svg");
      if (file) replacePicture(el.dataset.path, +el.dataset.el, file);
      return;
    }
    case "rco-unpick": return resetPicture(el.dataset.path, +el.dataset.el);
    case "uisnd-play": return playUiSound(el.dataset.path, el.dataset.id, el.dataset.mine === "1");
    case "uisnd-pick": {
      const file = await pickFile("audio/*,.wav,.mp3,.m4a,.aac,.ogg,.flac");
      if (file) replaceUiSound(el.dataset.path, el.dataset.id, file);
      return;
    }
    case "uisnd-hit": el.disabled = true; el.textContent = "Trimming…"; return startUiSoundAtHit(el.dataset.path, el.dataset.id);
    case "uisnd-reset": el.disabled = true; el.textContent = "Resetting…"; return resetUiSound(el.dataset.path, el.dataset.id);
    case "uisnd-retry": rcoFailed.clear(); uiSoundsLast = null; return renderAspect(aspectById("sounds"));
    case "shapes-save": return saveShapes(el.dataset.path, false);
    case "shapes-reset": return saveShapes(el.dataset.path, true);
    case "colors-test": el.disabled = true; el.textContent = "Saving…"; return saveColors(el.dataset.path, false, el.dataset.palette);
    case "colors-reset": el.disabled = true; el.textContent = "Saving…"; return saveColors(el.dataset.path, true, el.dataset.palette);
    case "colors-panel": el.disabled = true; el.textContent = "Saving…"; return saveColors(el.dataset.path, false, el.dataset.palette, true);
    case "colors-reset-others": el.disabled = true; el.textContent = "Saving…"; return resetOtherPalettes(el.dataset.path);
    case "colors-edit": state.colorEdit = el.dataset.palette; return renderAspect(aspectById("appearance"));
    case "colors-close": state.colorEdit = null; return renderAspect(aspectById("appearance"));
    case "colors-save": el.disabled = true; el.textContent = "Saving…"; return saveColorEdit(el.dataset.path, el.dataset.palette);
    case "tiles-number": return numberTileIcons();
    case "istyle-tile": el.disabled = true; return styleTile(el.dataset.v, el.getAttribute("aria-pressed") !== "true");
    case "istyle-shape": {   /* shown at once; kept on the console as it goes (setChoice says if it can't be) */
      const kept = setIconStyle(x => { x.shape = el.dataset.v; });
      renderAspect(aspectById("titles"));
      const b = document.querySelector(`[data-act="istyle-shape"][data-v="${el.dataset.v}"]`);
      if (b) b.focus();
      return kept;
    }
    case "istyle-behind": {
      if (el.dataset.v === "picture" && !await iconBackdropKept()) {   /* no picture yet: choose one and place it */
        const file = await pickFile("image/*");
        if (file) await openBgEditor(file);
        return;
      }
      const kept = setIconStyle(x => { x.behind = el.dataset.v; });
      renderAspect(aspectById("titles"));
      const b = document.querySelector(`[data-act="istyle-behind"][data-v="${el.dataset.v}"]`);
      if (b) b.focus();
      return kept;
    }
    case "istyle-picture": {
      const file = await pickFile("image/*");
      if (file) await openBgEditor(file);
      return;
    }
    case "istyle-adjust": return openBgEditor(`/api/theme/preview?${q({ id: state.editId, kind: "iconbg" })}&v=${stamp}`);
    case "bge-cancel": return bgeClose();
    case "bge-save": el.disabled = true; el.textContent = "Keeping…"; return bgeSave().catch(e => { el.disabled = false; el.textContent = "Use this picture"; toast(e.message, "err"); });
    case "bge-zoom": return bge && bgeZoom(bge.zoom * (+el.dataset.v > 0 ? 1.15 : 1 / 1.15));
    case "bge-reset": if (bge) { bge.zoom = 1; bge.s = bge.fill; bge.ox = (bge.F - bge.iw * bge.s) / 2; bge.oy = (bge.F - bge.ih * bge.s) / 2; bgeZoom(1); } return;
    case "bge-file": { const file = await pickFile("image/*"); if (file) await openBgEditor(file); return; }
    case "bge-fromtheme": return openBgEditor(`/api/theme/preview?${q({ id: state.editId, kind: "background" })}&v=${stamp}`);
    case "istyle-run": {
      el.disabled = true;
      await catchUp();
      await adoptStyledIcons();
      if (!(await styleIcons(false)).length) renderAspect(aspectById("titles"));
      return;
    }
    case "istyle-remove": el.disabled = true; el.textContent = "Removing…"; await catchUp(); return removeIconStyle();
    case "title-pic": {
      const file = await pickFile("image/*"), title = el.dataset.title, stem = el.dataset.stem;
      if (!file || !(await titlePicture(title, stem, file))) return;
      if (stem === "icon0") { await iconChangedByHand(title); renderAspect(aspectById("titles")); }
      return;
    }
    case "title-reset": {
      const g = titleGroups().find(x => x.title === el.dataset.title);
      if (g) { await resetAssets(g.pictures); await iconChangedByHand(g.title, true); renderAspect(aspectById("titles")); }
      return;
    }
    case "tile-icon": {
      const file = await pickFile("image/*,.svg");
      if (file) tileIcon(el.dataset.title, file);
      return;
    }
    case "tile-bg": {
      const g = tileGroups().find(x => x.title === el.dataset.title), file = g && await pickFile("image/*");
      if (file) { await attachTo([g.bg], file, `${(state.tileNames && state.tileNames.get(g.title)) || g.title} background`); renderAspect(aspectById("tiles")); }
      return;
    }
    case "tile-reset": {
      const g = tileGroups().find(x => x.title === el.dataset.title);
      if (g) {
        await resetAssets(g.icons.concat(g.bg ? [g.bg] : []));
        if (g.title in STYLE_TILES) await iconChangedByHand(g.title, true);
        renderAspect(aspectById("tiles"));
        renderAspect(aspectById("titles"));
      }
      return;
    }
    case "choose-cover": return chooseCover();
    case "remove-cover": return removePreview(state.editId, "picture");
    case "trim-play": return preview && !preview.timer ? stopPreview() : playPreview("");
    case "trim-seam": return playPreview("loop");
    case "trim-joint": return playPreview("joint");
    case "trim-cancel":
      stopPreview();
      state.trim = null;
      return renderAspect(aspectById("music"));
    case "trim-save": return saveTrim();
    case "more": state.pages[asp.id] = (state.pages[asp.id] || 48) + 96; return renderAspect(asp);
    case "toggle-area": {
      const off = bgOff();
      if (off.has(el.dataset.area)) off.delete(el.dataset.area); else off.add(el.dataset.area);
      setBgOff(off);
      renderAspect(aspectById("background"));
      const again = document.querySelector(`[data-act="toggle-area"][data-area="${CSS.escape(el.dataset.area)}"]`);
      if (again) again.focus();
      return;
    }
    case "refresh-log": return loadLog();
    case "copy-log": return copyLog();
    case "pair-start": return pairStart(el);
    case "pref": return setPref(el);
    case "storage-clean": return storageClean(el);
    case "access-mode": return accessMode(el);
    case "access-forget": return accessForget(el);
    case "fit-pick": {
      if (state.fit === el.dataset.v) return;
      state.fit = el.dataset.v;
      setChoice(c => { c.fit = state.fit; });
      for (const b of document.querySelectorAll('[data-act="fit-pick"]')) b.setAttribute("aria-checked", String(b.dataset.v === state.fit));
      return refitPictures();
    }
    case "place-pick": {
      const path = el.dataset.path, mode = el.dataset.v;
      if (placementFor(path).mode === mode) return;
      apFollow(path);
      setChoice(c => { pieceChoice(c, path).place = mode; });
      return placementChanged(path, true);
    }
    case "ap-view": case "ap-look": {
      apView = { ...apView, [el.dataset.act === "ap-view" ? "view" : "look"]: el.dataset.v };
      const box = el.closest(".ap-preview");
      if (box) box.outerHTML = apPreviewBox();
      apPreviewShow();
      const again = document.querySelector(`.ap-preview [data-act="${el.dataset.act}"][data-v="${el.dataset.v}"]`);
      if (again) again.focus();
      return;
    }
    case "advanced": {
      state.showAdvanced = !advancedShown();
      $("#secnav").innerHTML = secNavHtml();
      if ($("#steps")) $("#steps").innerHTML = stepsHtml(state.section || "theme");
      const first = state.showAdvanced && $('#secnav [data-section="textures"]');
      if (first) first.focus(); else { const m = $('#secnav [data-act="advanced"]'); if (m) m.focus(); }
      return;
    }
    case "view-crash":
      try { $("#log").textContent = await api("GET", "/api/crash", { name: el.dataset.name }); }
      catch (err) { toast(err.message, "err"); }
      return;
  }
});

document.addEventListener("submit", async e => {
  if (e.target.id === "pair") { e.preventDefault(); return pairFinish(); }
  if (e.target.id !== "create") return;
  e.preventDefault();
  return submitCreate();
});

let filterTimer = 0;
/* A placement option changed for a piece: its texture is made again from the kept picture, if there is one. */
/* A piece's placement, padding or clock band changed: the piece is made again with it, at once, with its progress
   showing from the first moment. Changes are made one at a time; a piece changed again while it waits is made once,
   with the latest choice (each run reads the options as they are when it starts). */
let placeWork = Promise.resolve();
const placeWaiting = new Set();
function placementChanged(path, redraw) {
  if (redraw) renderAspect(aspectById("appearance"));   /* "Fill with padding" shows its padding box at once */
  if (!state.files.has(path) || placeWaiting.has(path)) return placeWork;
  placeWaiting.add(path);
  const id = state.editId;
  placeWork = placeWork.then(() => {
    placeWaiting.delete(path);
    return state.editId === id ? placementChangedNow(path) : null;
  }).catch(e => toast(e.message, "err"));
  return placeWork;
}
async function placementChangedNow(path) {
  const asp = aspectById("appearance");
  if (!state.files.has(path)) return;
  if (!state.sources.has(path) && !state.keepOriginals) {
    toast("This theme does not keep original pictures, so this applies the next time you attach a picture.", "", 7000);
    return;
  }
  const p = progressToast(`Making ${appearanceName(path)} again…`);
  let recovered = false;
  try {
    if (!state.sources.has(path)) {
      try { recovered = await recoverSource(path, p); }
      catch (e) { /* a texture this page did not make: it needs its picture attached */ }
    }
    if (!state.sources.has(path)) {
      p.done("This piece's picture can't be read back from the console. Attach the picture again to use the new option.", "err");
      return;
    }
    if (!(await remakeFromSource(path, p))) p.done("The picture kept in the theme is gone. Attach the picture again to use the new option.", "err");
    else if (recovered) toast("Made again from the picture in the texture. Attach the original once more if you want back what the earlier placement cropped away.", "", 9000);
  } catch (e) { p.done(e.message, "err"); }
  await refreshStatus();
  renderAspect(asp);
}

document.addEventListener("input", e => {
  if (e.target.id === "new-name" && state.creating) { state.creating.name = e.target.value; return; }
  if (e.target.id === "istyle-scale") {   /* the shapes redraw at the new size as the slider moves; kept when let go */
    $("#istyle-scale-label").textContent = `${e.target.value}%`;
    state.choices.iconStyle = { ...iconStyle(), scale: +e.target.value };
    return iconStyleShow();
  }
  if (e.target.dataset && e.target.dataset.pad) {
    const v = Math.max(0, Math.min(40, Math.round(+e.target.value || 0)));
    pieceChoice(state.choices, e.target.dataset.pad).pad = v;
    return;     /* kept and made again when the box is left (its change event) */
  }
  const aspId = e.target.dataset && e.target.dataset.filterAspect;
  if (aspId) {
    state.filters[aspId] = e.target.value;
    state.pages[aspId] = 48;
    clearTimeout(filterTimer);
    filterTimer = setTimeout(() => {
      renderAspect(aspectById(aspId));
      const input = $(`[data-filter-aspect="${aspId}"]`);
      if (input) { input.focus(); input.setSelectionRange(input.value.length, input.value.length); }
    }, 250);
  }
  if (e.target.id === "music-gain") $("#gain-label").textContent = gainLabel(+e.target.value);
  if (e.target.id === "rco-lookup") {
    state.rcoLookup = e.target.value;
    clearTimeout(rcoLookupTimer);
    rcoLookupTimer = setTimeout(rcoLookup, 250);
  }
  const timeField = { "trim-start-time": "start", "trim-end-time": "end", "trim-lstart-time": "lstart", "trim-lend-time": "lend",
                      "trim-from-time": "from" }[e.target.id];
  if (timeField && state.trim) {
    const v = parseClock(e.target.value);
    if (!isNaN(v)) { state.trim[EDGE_KEY[timeField]] = v; clampTrim(timeField); drawTrim(); }
  }
});

document.addEventListener("change", async e => {
  const mixEl = e.target;
  if (e.target.dataset && e.target.dataset.keep) {
    const on = e.target.checked;
    setChoice(c => { pieceChoice(c, e.target.dataset.keep).keepClear = on; });
    return placementChanged(e.target.dataset.keep, false);
  }
  if (e.target.dataset && e.target.dataset.place) {
    apFollow(e.target.dataset.place);
    const mode = e.target.value;
    setChoice(c => { pieceChoice(c, e.target.dataset.place).place = mode; });
    return placementChanged(e.target.dataset.place, true);   /* "Fill with padding" shows its padding box */
  }
  if (e.target.dataset && e.target.dataset.pad) { setChoice(() => {}); return placementChanged(e.target.dataset.pad, false); }
  const t = e.target;
  if (t.id === "pv-look") { pvState.look = t.value; return previewRender(state.editId); }
  if (t.id === "istyle-scale") { const kept = setIconStyle(x => { x.scale = +t.value; }); iconStyleShow(); return kept; }
  if (t.id === "istyle-colour") { const kept = setIconStyle(x => { x.colour = t.value; }); iconStyleShow(); return kept; }
  if (t.id === "istyle-mine") { await setIconStyle(x => { x.mine = t.checked; }); return renderAspect(aspectById("titles")); }
  if (t.id === "istyle-auto") {
    await setIconStyle(x => { x.auto = t.checked; });
    await api("POST", "/api/theme/icon-style", { id: state.editId, auto: t.checked ? 1 : 0 }).catch(err => toast(err.message, "err"));
    return;
  }
  if (t.id === "pl-join") return setPlaylistJoin(+t.value);
  if (t.id === "pl-end") return setPlaylistEnd(t.value);
  if (t.id === "keep-originals") {
    try {
      await api("POST", "/api/theme/settings", { id: state.editId, keep_originals: t.checked ? 1 : 0 });
      state.keepOriginals = t.checked;
      await refreshStatus();
      toast(t.checked ? "Original pictures are kept from now on." : "Original pictures are not kept from now on. Those already kept stay until you remove them.", "ok");
    } catch (err) { t.checked = state.keepOriginals; toast(err.message, "err"); }
    await loadOriginals();
    ASPECTS.forEach(renderAspect);
    return;
  }
  if (t.id === "fit") { state.fit = t.value; setChoice(c => { c.fit = t.value; }); return refitPictures(); }
  try {
    if (t.id === "theme-name" && t.value.trim()) {
      await api("POST", "/api/theme/settings", { id: state.editId, name: t.value.trim() });
      await refreshStatus();
      const h = $("#edit-name");
      if (h) h.textContent = t.value.trim();
      document.title = `${t.value.trim()} · PS5 Themes`;
      toast("Name saved.", "ok");
    }
    if (t.id === "theme-fw") {
      await api("POST", "/api/theme/settings", { id: state.editId, fw: t.value });
      await refreshStatus();
      toast("Firmware saved.", "ok");
    }
    if (t.id === "trim-loop" && state.trim) {
      const [loop, ms] = LOOP_CHOICES[+t.value];
      Object.assign(state.trim, { loop, blend: ms / 1000 });
      if (preview) stopPreview();
      return;
    }
    if (t.id === "trim-order" && state.trim) {
      const tr = state.trim, was = tr.order;
      stopPreview();
      tr.order = t.value;
      if (was === "music" || (t.value === "music-first") !== (was === "music-first")) defaultLoop(tr);
      clampTrim("lend");
      renderAspect(aspectById("music"));
      $("#trim-order").focus();
      return;
    }
    if (/^trim-(l?(start|end)|from)-time$/.test(t.id) && state.trim) { drawTrim(); return; }
    if (t.id === "music-gain") {
      const id = state.editId;
      await api("POST", "/api/theme/settings", { id, gain: t.value });
      await refreshStatus();
      await remakeHomeMusic(id);
      renderAspect(aspectById("music"));
    }
  } catch (err) { toast(err.message, "err"); }
});

/* Drag and drop: onto a drop zone, tile or row attaches a file; elsewhere in
   the Library it imports a theme. */
const drop = $("#drop");
let dragDepth = 0;
const hasFiles = e => Array.from(e.dataTransfer?.types || []).includes("Files");
const dropTarget = e => e.target.closest && e.target.closest("[data-drop], .tile, [data-drop-group]");
window.addEventListener("dragenter", e => {
  if (!hasFiles(e)) return;
  dragDepth++;
  if (state.route === "library") drop.classList.add("on");
});
window.addEventListener("dragleave", e => {
  if (!hasFiles(e)) return;
  dragDepth = Math.max(0, dragDepth - 1);
  if (!dragDepth) drop.classList.remove("on");
});
window.addEventListener("dragover", e => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  document.querySelectorAll(".drag").forEach(el => el.classList.remove("drag"));
  const target = dropTarget(e);
  if (target) target.classList.add("drag");
  e.dataTransfer.dropEffect = state.route === "edit" && !target ? "none" : "copy";
});
window.addEventListener("drop", e => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragDepth = 0;
  drop.classList.remove("on");
  document.querySelectorAll(".drag").forEach(el => el.classList.remove("drag"));
  const target = dropTarget(e);
  const file = e.dataTransfer.files[0];
  if (target && file) {
    const asp = aspectById(target.dataset.drop || target.dataset.aspect);
    if (target.dataset.drop) attachToAspect(asp, file);
    else {
      const key = target.dataset.group || target.dataset.dropGroup;
      if (asp.id === "appearance") { const g = findGroup(asp, key); if (g) apFollow(g.assets[0].path); }
      attachToGroup(asp, key, file);
    }
  } else if (state.route === "library") {
    importDrop(e.dataTransfer);
  }
});

window.addEventListener("hashchange", e => {
  /* Another theme isn't opened while the page is still writing into this one (an upload, styling the icons, a
     save): each of those writes to the theme that is open as it goes, and would carry on into the other one. The
     library and Settings can be opened meanwhile; this theme stays the one written to. */
  if (writing > 0 || styling || autoRun || progressStack.length) {
    const parts = location.hash.replace(/^#\/?/, "").split("/");
    const next = parts[0] === "edit" ? decodeURIComponent(parts[1] || "") : "";
    if (next && state.editId && next !== state.editId) {
      history.replaceState(null, "", e.oldURL);
      const t = themeById(state.editId), name = t ? t.name : "the theme";
      toast(styling || autoRun ? `The icons of ${name} are being styled: open another theme once that is done, or stop styling.`
        : "A change is still on its way to the console: open the other theme once it is done.", "", 6000);
      return;
    }
  }
  bgeClose();   /* a dialog of the page being left goes with it */
  if ($("#pv-overlay")) previewClose();
  const left = state.route === "edit" ? themeById(state.editId) : null;
  state.confirming = "";
  stopPreview();
  state.trim = null;
  /* Another part of the theme being edited: only the part beside the list changes */
  const was = state.route === "edit" && $("#editor") ? state.editId : null;
  parseRoute();
  if (was && state.route === "edit" && state.editId === was && themeById(was)) return showPane(true);
  render();
  const f = document.activeElement;
  if (!f || f === document.body || !view.contains(f)) view.focus({ preventScroll: true });
  /* (f) a theme's "unsaved changes" message goes once that theme is open again */
  if (state.route === "edit") for (const t of document.querySelectorAll(".toast[data-unsaved]")) if (t.dataset.unsaved === state.editId) leave(t);
  if (left && left.unsaved && !(state.route === "edit" && state.editId === left.id)) {
    toast(`${left.name} has unsaved changes. Open it again to save or discard them.`, "", 7000).dataset.unsaved = left.id;
  }
});

/* Closing or reloading the page while something is being made or sent would cut it off: the browser asks first. */
addEventListener("beforeunload", e => {
  if (!progressStack.some(p => p.el.isConnected)) return;
  e.preventDefault();
  e.returnValue = "";
});

/* Escape closes New theme, as Circle backs out of a menu on the console. */
document.addEventListener("keydown", e => {
  if (e.key !== "Escape" || state.route !== "library" || $("#pv-overlay") || !(state.creating || state.mix)) return;
  closeCreate();
});

/* Handles of the part editor: arrow keys move by 0.1 s (Shift: 1 s), Page keys
   by 10 s, Home/End to the limits. A PS5 controller's D-pad sends arrows. */
document.addEventListener("keydown", e => {
  const which = Object.keys(HANDLES).find(k => HANDLES[k].slice(1) === e.target.id);
  if (!state.trim || !which) return;
  const t = state.trim, key = EDGE_KEY[which];
  const step = { ArrowLeft: -0.1, ArrowDown: -0.1, ArrowRight: 0.1, ArrowUp: 0.1, PageDown: -10, PageUp: 10 }[e.key];
  if (step !== undefined) t[key] += step * (e.shiftKey && Math.abs(step) < 1 ? 10 : 1);
  else if (e.key === "Home") t[key] = 0;
  else if (e.key === "End") t[key] = t.duration;
  else return;
  e.preventDefault();
  clampTrim(which);
  drawTrim();
});
window.addEventListener("resize", () => { if (state.trim) drawTrim(); });

(async () => {
  if (!location.hash) history.replaceState(null, "", "#/library");
  if (!(await accessReady())) return;
  await refreshStatus();
  render();
  let tick = 0, polling = false;
  const poll = async t => { if (polling || gated) return; polling = true; try { await pollConsole(t); } catch (e) { /* the next one */ } finally { polling = false; } };
  setInterval(() => poll(++tick), 1000);
  document.addEventListener("visibilitychange", () => { if (!document.hidden) poll(0); });   /* back to the page: at once */
})();
