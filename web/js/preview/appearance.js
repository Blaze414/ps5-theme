"use strict";

/* ---- The Appearance part's preview: the home screen or the Control Center as the console draws them, with each
   look's pieces from the very textures the console will use. A piece the console has none of shows as an outline
   where it would sit. */
const AP_LOOKS = ["ps", "ps2", "ps3", "ps4"];
let apView = { look: "", view: "home" };
const apPath = (look, which) => `/system/vsh_asset/${which === "homeui" ? `homeui_${look}` : `control_center_${look}_${which}`}.dds`;
const apChanged = look => ["homeui", "top", "bottom"].some(w => state.files.has(apPath(look, w)));
const AP_PIECES = { home: [["homeui", "Home screen artwork", "art"]], cc: [["top", "Top-right corner", "top"], ["bottom", "Bottom panel", "bottom"]] };

function apPreviewBox() {
  if (!apView.look) apView.look = AP_LOOKS.find(apChanged) || "ps";
  const chip = (act, v, label, on, dot) => `<button type="button" class="btn sm chip" data-act="${act}" data-v="${v}" aria-pressed="${on}">${esc(label)}${dot ? `<span class="ap-dot" aria-label=", changed"></span>` : ""}</button>`;
  return `<div class="ap-preview">
      <div class="pv-toolbar">
        <div class="pv-tabs">${chip("ap-view", "home", "Home screen", apView.view === "home")}${chip("ap-view", "cc", "Control Center", apView.view === "cc")}</div>
        <div class="pv-tabs">${AP_LOOKS.map(l => chip("ap-look", l, LOOK_NAMES[l], apView.look === l, apChanged(l))).join("")}</div>
      </div>
      <div class="pv-stage" id="ap-stage"><div class="pv-busy">Drawing…</div></div>
      <p class="note" id="ap-whose"></p>
    </div>`;
}

async function apPreviewShow() {
  if (!$("#ap-stage")) return;
  const id = state.editId, files = state.files, { look, view } = apView;
  const where = { ...homeCardView(await homeRows(), files, homeIndex()), look };
  const scene = view === "home" ? await homeScene(id, files, where) : await ccScene(id, files, where);
  const stage = $("#ap-stage");
  if (!stage || state.editId !== id || apView.look !== look || apView.view !== view) return;
  const pieces = AP_PIECES[view], mine = w => files.has(apPath(look, w)), has = w => (state.assets || []).some(a => a.path === apPath(look, w));
  const label = view === "home" ? `The home screen with the ${LOOK_NAMES[look]} artwork` : `The Control Center with the ${LOOK_NAMES[look]} pieces`;
  stage.innerHTML = `<canvas class="hs" width="1920" height="1080" role="img" aria-label="${esc(label)}"></canvas>
    ${pieces.filter(([w]) => !mine(w) && !has(w)).map(([, name, cls]) => `<div class="ap-slot ${cls}">${esc(`${name}: none on this firmware`)}</div>`).join("")}`;
  (view === "home" ? homeScreenDraw : ccScreenDraw)(stage.querySelector("canvas").getContext("2d"), scene);
  const yours = pieces.filter(([w]) => mine(w)).map(p => p[1]), theirs = pieces.filter(([w]) => !mine(w) && has(w)).map(p => p[1]);
  const whose = $("#ap-whose");
  if (whose) whose.textContent = [yours.length ? `Yours: ${yours.join(", ")}.` : "", theirs.length ? `The console's own: ${theirs.join(", ")}.` : ""].filter(Boolean).join(" ");
}

/* The preview follows the piece being worked on. */
function apFollow(path) {
  const m = APPEARANCE_RE.exec(path || "");
  if (!m) return;
  apView = { look: m[2], view: m[1] === "homeui" ? "home" : "cc" };
}
