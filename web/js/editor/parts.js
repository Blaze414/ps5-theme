"use strict";

const ASPECTS = [
  { id: "background", title: "Background", mode: "single",
    sub: "One picture or video for the home screen, made at each area's own size.",
    filter: a => (a.kind === "image" || a.kind === "video" || a.kind === "texture") && BG_RE.test(a.path), accept: "image/*,video/*" },
  { id: "icons", title: "Other icons", mode: "tiles",
    sub: "Icons in the console's own screens, one by one. Every size the console uses is made from your one picture.",
    filter: a => isImage(a) && ICON_RE.test(a.path) && !BG_RE.test(a.path), accept: "image/*,.svg" },
  { id: "music", title: "Home music", mode: "music",
    sub: "The music the home screen plays. Any audio file; you choose the part that plays and loops." },
  { id: "sounds", title: "Sounds", mode: "rows",
    sub: "The interface's sounds and the console's other sound files. Any audio file, matched to each sound's loudness.",
    filter: a => a.kind === "audio" && a.path !== HOME_MUSIC, accept: "audio/*,.wav,.at9" },
  { id: "fonts", title: "Fonts", mode: "rows",
    sub: "The fonts the home screen and Settings draw their text in. Use your own TTF or OTF font.",
    filter: a => a.kind === "font", accept: ".ttf,.otf" },
  { id: "animations", title: "Animations", mode: "rows",
    sub: "The interface's animation files. A replacement keeps the same structure and canvas size.",
    filter: a => a.kind === "json", accept: ".json,application/json" },
  { id: "video", title: "Video", mode: "rows",
    sub: "Video clips other than the background, as MP4 or WebM like the console's.",
    filter: a => a.kind === "video" && !BG_RE.test(a.path), accept: "video/*,.mp4,.webm,.m4v" },
  { id: "appearance", title: "Appearance themes", mode: "rows",
    sub: "The artwork and colours of the PlayStation, PlayStation 2, 3 and 4 looks in Settings > Appearance and Sound.",
    filter: a => APPEARANCE_RE.test(a.path), accept: "image/*" },
  { id: "textures", title: "Textures", mode: "rows",
    sub: "The interface's other pictures. Any image is fitted to each one's size.",
    filter: a => a.kind === "texture" && !BG_RE.test(a.path) && !isTitlePicture(a.path) && !APPEARANCE_RE.test(a.path), accept: "image/*" },
  { id: "tiles", title: "Home screen tiles", mode: "systiles",
    sub: "The console's own tiles, such as PlayStation Store: their icons, and the background while one is selected.",
    filter: a => a.kind === "image" && !!tileTitle(a.path), accept: "image/*" },
  { id: "titles", title: "Games and apps", mode: "titles",
    sub: "Installed games and apps, homebrew too: their icons and other pictures on the home screen.",
    filter: a => titlesOf(a.path).length > 0 && (a.kind === "image" || a.kind === "texture"), accept: "image/*" },
  { id: "interface", title: "Interface icons", mode: "rco",
    sub: "Icons, buttons and symbols in the menus, and the shape of their corners. A test shows which picture is where.",
    filter: a => a.kind === "rco" && !(state.rco && state.rco.get(a.path) === null) },
  { id: "images", title: "Other images", mode: "tiles",
    sub: "Every other picture that can be replaced, with search.",
    filter: a => isImage(a) && !ICON_RE.test(a.path) && !BG_RE.test(a.path) && !isTitlePicture(a.path), accept: "image/*,.svg" },
];
const aspectById = id => ASPECTS.find(a => a.id === id);

function aspectAssets(asp) { return asp.filter ? (state.assets || []).filter(asp.filter) : []; }
function aspectReplaced(asp) {
  if (asp.mode === "music") return (themeById(state.editId) || {}).music || state.files.has(HOME_MUSIC) ? 1 : 0;
  return aspectAssets(asp).filter(a => state.files.has(a.path)).length;
}
function aspectFlags() { return state.choices.sections || {}; }
function aspectOn(asp) { return aspectReplaced(asp) > 0 || !!aspectFlags()[asp.id]; }
function setAspectFlag(asp, on) {
  if (!!aspectFlags()[asp.id] === on) return;
  setChoice(c => {
    const f = c.sections = c.sections || {};
    if (on) f[asp.id] = 1; else delete f[asp.id];
    if (!Object.keys(f).length) delete c.sections;
  });
}
