# TimeTaker — GUI spec

The reference for every GUI session. It records what the user wants, the
decisions taken, and the open questions. Brainstormed 2026-11-23. The
backbone under it (device, race engine, health, sync) is described in
[README.md](README.md) and `../SYSTEM_ARCHITECTURE.md` §11; race rules in
`../RACE_RULES.md`.

Status markers: **[v1]** build now · **[later]** planned, design for it ·
**[idea]** not decided · **[PP]** needs the Power Pack (power board,
SYSTEM_ARCHITECTURE §12).

---

## 1. Who uses it and how

- **The host** (the user's uncle, and colleagues who get a system) runs a
  race evening on a **laptop**, sometimes with an **external monitor** or an
  **iPad**. They are not developers. Everything they touch on race day must be
  obvious.
- **One car per lane, for the whole evening.** The car stays on its lane;
  **the drivers rotate** between lanes. A "lane" and a "car" are the same thing
  in the UI.
- Tracks have **2, 4 or 6 lanes** (up to 6). Every screen must work for any
  lane count, and for fewer drivers than lanes.
- Typical numbers: fastest lap about 19 s, average 26–28 s; about 12 drivers.
- The **Track Setup** tab is the exception: it is used once per setup and may
  be technical.

### The reference evening (12 drivers, 6 lanes)

1. **Groups A and B.** The host pins the best 2–4 drivers so the strongest are
   split evenly; the rest are drawn at random.
2. Each group runs **race preset A**: 5 laps per race, 3 races per lane, every
   driver on every lane → 3 × 6 = 18 races per group.
3. **Scoring per race:** 1st 6 points, 2nd 5 … 6th 1. First to crash out is
   last; the first to finish 5 laps wins.
4. **Top 2 of each group → Grand Final.**
5. The other 8 go into **two losers brackets of 4**, crossed over by points
   (A3, B4, A5, B6 / B3, A4, B5, A6). They run **race preset B** (3 laps, 2
   races per lane) on the inner 4 lanes. **The winner of each → Grand Final.**
6. **Grand Final:** 6 drivers, race preset A.

The GUI must make this evening easy to build, to run, and to scale to other
driver counts (§5.4).

---

## 2. Navigation and areas

### 2.1 Sidebar

- **No sidebar panel:** the rail is a column of **thick, tall boxes that
  touch** each other (neighbours share one border).
  Groups stay separated by a gap; System sits at the bottom.
- **Borders and fill:** none while collapsed (just the icons); when open,
  2.5 px outlines and the panel fill fade in with the opening motion. The
  selected item: 3 px orange, no tint (no fill colour).
- **Collapsed:** only the icons, each centred in an invisible **upright
  square as wide as the rail** (screen edge to the rail's edge), icon 40 %
  of the square. No grey edge line: the orange lines are the rail's edge. Square = rail width = **92 px**, smaller on short
  screens so all 8 squares + logo always fit (60 px minimum). The selected square is
  an **open tab**: orange on top, left and bottom, no right side and no fill
  (it opens into the page), and orange lines
  run from its right corners up and down to the screen edges. Open: a full
  orange parallelogram, no lines. Closing: the right side fades out
  linearly with the morph and is gone the moment the box is square, then the
  lines fade in. Opening: the lines vanish at once (so none sweeps across the
  parallelograms) and the right side fades in with the morph.
- **Opening (one 0.2 s motion):** the squares **widen and lean into
  parallelograms** at the same time (rail 360 px), the orange lines vanish,
  the names appear (large, bold), and the **page slides right and goes dark** (scrim), so choosing
  the area is the only thing in focus. Closing runs it backwards.
  (Decided 2026-11-23: no overlay panel.)
- **Desktop:** hovering the rail expands it. A click navigates and collapses it;
  it stays collapsed while the pointer rests on the clicked icon and expands
  again as soon as the pointer moves onto any other icon (or leaves and
  comes back).
- **Touch (iPad):** a tap anywhere on the rail expands it; a tap on an item
  navigates and collapses it; a tap on the dark page collapses it.
- **No connection badge in the rail** (decided 2026-11-23): the bottom group
  is just Status, Track Setup, Settings. Connection (USB / BLE / simulator /
  offline) and Power Pack state live on the **Status** page. **State icons:**
  - **Status:** dark grey = nothing connected; green = a module connected
    (USB, Bluetooth or simulator); red = connected but a check fails
    (`HealthReport.overall` = fail). Warnings stay green.
  - **Track Setup:** green with a Power Pack, dark grey without.
  - The colour shows the state also when the item is selected.
- Keyboard: every item is reachable by Tab, Enter navigates; the rail is a
  `nav` landmark.

### 2.2 Areas (tabs)

Grouped top to bottom. Placeholder icons until the user approves the look.

| Group | Area | Purpose |
|---|---|---|
| **Drive** | **Grand Prix** | Run an event: enter the participants, pick a tournament or a single race preset, race. The live race screen lives here. |
| | **Time Attack** | Continuous / session lap timing for showing off the track. Big numbers. |
| | **Training** | Analyse laps of a session: lap lists, best laps, later throttle traces and sectors **[PP]**. |
| **Plan** | **Tournaments** | Build tournament templates from race presets (the bracket builder). |
| | **Race Presets** | Create, rename, edit race presets. |
| **System** | **Track Setup** **[PP]** | Speed level, power limits, lane compensation, calibration. Expert mode. |
| | **Status** | Connection and health of control module, BLE bridge, every lane (`src/health`). Debug view. |
| | **Settings** | Language, theme, units, data import/export, about. |

Why a Grand Prix area: the user's list had places to *design* races and
tournaments but none to *run* them. Separating "plan" from "run" keeps the
running screen clean and stops the host from editing a template mid-event.

---

## 3. Look

### 3.1 Style

- **Sharp, no rounded corners.** Panels, buttons, tabs, chips and the active
  marker are **parallelograms** slanted at **one fixed angle (≈ 12°)**,
  leaning forward like the logo's speed lines. Implementation: `clip-path`
  polygons or `skewX(-12deg)` on the box with the content counter-skewed, so
  text stays upright.
- Large rectangular areas (tables, the bracket canvas) stay rectangular; their
  *headers* and *edges* carry the slant. Slant is an accent, not a wall of
  rhombuses.
- **Checkered flag motif** from the logo, used sparingly: finish states,
  winner rows, the podium.
- **Dark theme by default** (dim room, TV, laptop at night), light theme in
  Settings. Colours as CSS tokens on `:root`.
- **Lane colours:** every lane has one fixed colour, used everywhere a lane
  appears (lap tiles, tables, bracket seat lists, start lights preview). Six
  distinct colours that work on dark and light.
- **Accent colour: Märklin Sprint orange** (the user's wish, decided
  2026-11-23) for the active area, primary buttons, live states. Exact shade
  and the lane colours: decided on the shell.

### 3.2 Fonts

The logo's brush script is not used in the UI. Instead, sharp italic faces
that fit the parallelogram look. All **OFL, bundled as local woff2** (the app
is an offline PWA, no Google Fonts at runtime).

| Role | Font (candidate) | Why |
|---|---|---|
| Headings, nav labels, big numbers | **Chakra Petch** SemiBold/Bold *Italic* | Chamfered, angular letters: matches sharp parallelograms |
| Body, tables, forms | **Titillium Web** Regular/SemiBold (italic for emphasis) | Motorsport feel, very readable at small sizes |
| Lap times | the heading font with `font-variant-numeric: tabular-nums`, else a monospaced fallback | Digits must not jump while times update |

To confirm in the shell: tabular digits in the chosen font, German umlauts,
readability at 13 px. Alternatives if one fails: Saira (italic), Exo 2
(italic).

### 3.3 Layout

- Designed for a **1366 × 768 laptop** first, must work at 1920 × 1080 and on
  an iPad (1024 × 768 landscape, touch targets ≥ 44 px).
- **External monitor: v1 mirrors the laptop.** So the Grand Prix and Time
  Attack screens must read well from a few metres: big lane tiles, big times.
  **[later]** "Open display window": a second browser window that is a pure
  view (BroadcastChannel sync already exists in `src/sync/raceSync.ts`).
- **Master–detail pattern** for Race Presets and Tournaments: a fixed
  list on the left (items + a "+" at the end), the editor on the right.

---

## 4. Race Presets

### 4.1 Layout

Left: list of presets (name + a one-line summary such as "5 laps · 3×/lane").
Below the last one: **+ New preset**. Right: the editor of the selected one.
Actions: rename (inline), duplicate, delete (with confirm; refused while a
tournament template uses it, naming the template). Shipped **default presets**
are read-only, "duplicate to edit".

### 4.2 What a race preset holds

| Setting | Values | Notes |
|---|---|---|
| Name | text | |
| Race length | **laps** (default) or **time** | engine supports both |
| Laps / minutes | number | |
| **Races per lane** (k) | 1…n | each driver drives every used lane k times |
| **Rotation order** | **block** (k races on a lane, then move; the reference evening) or **rotate every race** | |
| **Result basis** **[PP for distance]** | **Distance covered** or **Order of crashing out** | see 4.3 |
| Crash policy | crash = out (DNF) / replacement allowed | `crashPolicy` |
| **Points table** | points per place, 1st…6th | default 6-5-4-3-2-1; editable list; presets "6…1", "10-7-5-3-2-1", "F1-style" |
| Bonus points **[idea]** | fastest lap +1 | |
| False start | flag / time penalty, seconds | existing main settings |
| Deeper (collapsed) | min lap time, start light timing, penalty apply | existing deeper settings |

The editor shows a live **summary card**: races per group for N drivers
(k × N), and an **estimated duration** (races × (laps × average lap +
changeover)); average lap and changeover are Settings values, defaults 27 s
and 60 s. Reference evening: 18 races × (5 × 27 s + 60 s) ≈ 59 min per group.

### 4.3 Result basis (who is last when cars crash)

- **Distance covered [PP]:** a crashed car is ranked by how far it got (laps +
  position in the lap from the Power Pack). The most fair; the user's
  preference.
- **Order of crashing out [v1]:** the first car out is last, the second out is
  second-to-last, and so on. Without the Power Pack the crash time is when the
  host marks the car out (`director.crashOut`); with it, the detected time.
- Without a Power Pack the option "distance" is shown **locked** with the
  Power Pack badge (§9), and presets that use it fall back to crash order with
  a visible note.

### 4.4 Fewer drivers than lanes

A group of 4 on 6 lanes uses **4 lanes**; which ones comes from Track Setup
("preferred lanes for partial fields", default **centre out**, can exclude a
broken lane). The rotation then runs over those lanes only (k × 4 races).
Points: places beyond the field size are simply unused (4 drivers → 6-5-4-3
with the default table). **[idea]** option "scale table to field size".

---

## 5. Tournaments

### 5.1 Concepts

- **Tournament template:** a reusable structure built from race presets.
  Stored like presets (left list + editor).
- **Event:** a template applied to a list of drivers on a track with L lanes,
  run on Grand Prix. The template is copied into the event, so editing a
  template later never changes a running or finished event.

### 5.2 Building blocks (nodes)

| Node | In → out | Meaning |
|---|---|---|
| **Entry** | drivers → drivers | The field. Seeding: pinned seeds (manual) + random draw for the rest **[v1]**; ratings from history **[later]**. |
| **Split** | drivers → K groups | Snake by seed (strong drivers spread evenly), or random. |
| **Stage** | group → ranked group | Runs a race preset over the group; ranks by points, then tie-breaks. |
| **Merge / Crossover** | ranks from several stages → group(s) | Collects "ranks 3–6 of A and B", optionally crossed over into M groups. |
| **Final** | group → podium | A stage whose result is the event result. |

**Edges carry rank ranges:** "A 1–2 → Final", "A 3–6 → Losers merge",
"Losers 1 → Final".

**Stage ranking tie-breaks** (decided 2026-11-23). First always more points.
For a points tie, **one setting per template** (changeable per stage):

| Option | Meaning |
|---|---|
| **More wins, then best lap** (default) | |
| **Best lap only** | |
| **More wins only** | still tied → shared place |
| **Tiebreaker race** | only when the tie matters for routing (e.g. the last qualifying place). Before the next stage starts, Grand Prix shows "Tiebreaker: just these drivers", puts them on the **inner lanes** and runs a **tiebreaker race preset** (template setting; shipped default "Tiebreaker": 1 race per lane, 3 laps). Its result orders the tied drivers. |

Ties that do not change routing (e.g. 4th vs 5th when both go to the same
repechage) are resolved by "more wins, then best lap" without a race.

### 5.3 The builder (visual)

A full freeform node editor (drag nodes, draw wires) is heavy to use on a
laptop touchpad and an iPad. Recommendation: a **column-based bracket
builder**:

- Columns are rounds (left → right). Each column holds node cards
  (parallelograms).
- Add a node with "+" in a column; set its preset, its input and its routing
  with dropdowns ("send ranks 1–2 to: Grand Final").
- The **wires are drawn automatically** from the routing. Layout is automatic,
  so it always looks tidy.
- Each card shows its preset, size ("6 drivers"), races and estimated time.
- A footer shows totals: races, estimated duration, final size vs lanes.
- **Validation inline:** a stage larger than the lane count, a final with
  empty seats, a driver routed twice, a rank nobody receives.

**[later]** free placement / dragging wires if the column builder proves too
limited.

### 5.4 Templates that scale with the field

A template should not hard-code "12 drivers". It is written as **rules**, and
the app computes the tree for N drivers on L lanes, shows it as a **preview**,
and the host can still adjust it before starting.

Example rule set, **"Groups → Repechage → Final"**:

1. Groups G = ceil(N / L), sizes as even as possible.
2. Final has L seats; R of them are for repechage winners (template setting,
   default R = L / 3).
3. Direct qualifiers per group = (L − R) / G.
4. The rest go to repechage: M = ceil(rest / L) groups, crossover by rank;
   R / M winners each to the final.

| N, L | Groups | Direct | Repechage | Final |
|---|---|---|---|---|
| 12, 6 | 2 × 6 | 2 + 2 | 8 → 2 × 4, winner each | 6 |
| 10, 6 | 2 × 5 | 2 + 2 | 6 → 1 × 6, top 2 | 6 |
| 6, 6 | 1 × 6 | — | — | the group *is* the final |
| 8, 4 | 2 × 4 | 1 + 1 | 6 → 2 × 3, winner each | 4 |

Shipped templates **[v1]**: *Single race* (everyone in one stage, N ≤ L),
*Groups → Final*, *Groups → Repechage → Final* (the reference evening).
Custom templates built in the builder are fixed-size until rule editing exists
**[later]**.

---

## 6. Grand Prix

The screen the host uses all evening. Must work from across the room.

### 6.1 Flow

1. **Start an event:** **enter the names of all participants** (names used
   before are suggested); pick a tournament template *or* a single race
   preset; pin seeds; draw groups.
   Preview the tree and the estimated duration. Start.
2. **Event overview:** the bracket with progress (done / running / waiting),
   current standings per stage.
3. **Next race card:** big, per lane: **lane colour + lane number → driver
   name**, so everyone knows where to sit. Also "race 7 of 18 · Group A".
4. **Live race:** one tile per used lane: driver, lap x / n, last lap, best
   lap, gap, position; flags (false start, penalty, out). Controls: start,
   abort, mark crashed/out, penalty, ±lap, disqualify, undo (the existing
   host actions).
   **[later]** a live **track minimap** with car positions (Power Pack
   position tracking), next to the tiles.
5. **Race result:** places, points awarded, any flags. The host **confirms**
   (or corrects) → standings update → next race card.
6. Stage done → routing applied automatically, shown as "Advancing: …".
7. Final done → **podium screen** (checkered).

### 6.2 Rules

- The event survives a page reload or a laptop crash (resumed from the event
  store).
- Every correction is a host action (undo-able), never an edit of stored
  results.
- A race can be **re-run** (voided) by the host, e.g. after a track problem.

---

## 7. Time Attack and Training

Both reuse the same blocks (lane tile, lap list, best-lap board).

### Time Attack

- **Continuous mode** (an endless session) or **timed session** (e.g. 10 min,
  qualifying style).
- Per lane: current lap running, last lap, session best; overall session best
  highlighted. Optional driver name per lane.
- **Leaderboard** of the session's best laps (by driver if names are set,
  else by lane).
- Made for the big screen.

### Training

- Opens a session (live, or a stored Time Attack session or race).
- Per lane: **all laps** of the session, best/average/consistency.
- **[PP / later]** throttle (current/voltage) trace of a lap; **overlay** of
  laps (best vs last, or all); **sector times** if position tracking allows.

**Open:** two tabs, or one "Practice" area with Time Attack / Training modes.
Spec keeps two tabs; they share all components, so merging later is cheap.

---

## 8. Track Setup [PP]

The whole area needs the Power Pack. Without it: the area shows what it does
and "Connect a Power Pack to use Track Setup" (§9).

### 8.1 Basic (always visible)

- **Speed level 1–5** as big segmented parallelogram buttons. Each level maps
  to a percentage of the absolute limits (§8.2); level 1 is **not** near 0 %:
  cars must still overcome static friction. Default mapping 60 / 70 / 80 / 90 /
  100 %, editable in expert mode.
- Current state of each lane at a glance (power on/off, fault).

### 8.2 Expert settings (toggle at the top)

- A switch **"Show expert settings"**, **off every time the tab is opened**.
- **Absolute limits:** system voltage limit, system current limit.
- **Per lane:** start (static) voltage, maximum voltage, current limit.
- **Lane compensation:** equalise lanes with different track resistance;
  per-lane trim, plus a guided measurement **[later]**.
- **Speed level mapping** (the 5 percentages).
- **Preferred lanes for partial fields** and excluded lanes (§4.4).
- Calibration functions of the power board; live telemetry per lane (V, I).
- Values are validated against the absolute limits; "Apply" writes to the
  device, with a confirmation for anything that raises a limit.

Saved as a **track profile** (so a colleague's track has its own).

---

## 9. Power Pack gating

- Capability comes from the node model (a power board node present).
- **Gated features stay visible but locked**, with a small "Power Pack" badge
  and one line of why. They are never silently hidden: hosts should know the
  feature exists.
- Gated: the whole Track Setup area; result basis "distance"; automatic crash
  detection; stop-and-go penalty; throttle traces and sectors in Training.
- Presets/templates that use a gated option still load; Grand Prix shows the
  fallback (e.g. crash order instead of distance).

---

## 10. Status

The `HealthReport` from `src/health/` made visual: one card per part (control
module, BLE bridge, lanes 1–6), fixed rows per check in catalogue order,
level colour, message, value. Lane sensor readout and calibration
(`tt sensors`, `tt calibrate` equivalents). Raw event trace collapsible (the
current `debug/` console's content). Technical tone is fine here.

---

## 11. Settings

- **Language:** English **[v1]**, German **[v1 after English]**.
- Theme: dark / light / follow system.
- Number/time format follows the language (German: `27,384 s`).
- Average lap and changeover time for duration estimates.
- Data: export / import presets, templates, drivers (JSON file) to share with
  colleagues; export events/races (`timetaker-race` exists).
- About: app version, protocol version, device firmware versions.

---

## 12. Engineering rules

- **i18n from the first commit:** no UI string in components; keys in
  `en.json` / `de.json`.
- **Components are reused** across areas: lane tile, lap table, standings
  table, parallelogram button/panel, master–detail list, Power Pack lock.
- **UI state only in the view layer.** Presets, templates, drivers, events are
  plain data with pure functions (validation, rotation schedule, tree
  generation, scoring), in a DOM-free folder with `node:test` tests, like
  `race/`.
- Persisted in IndexedDB; every stored object has a `schemaVersion`.
- The simulator stays the default development target; the GUI must be fully
  usable against `--sim`, including a simulated Power Pack **[later]**.
- Constraints from README.md hold: Chrome/Edge, offline PWA, erasable TS
  syntax only, no build step for development.
- **Framework (decided 2026-11-23): Preact + htm + @preact/signals**,
  vendored in `vendor/preact/` and mapped with an import map (no JSX, no
  build step, about 25 kB). Graphics such as the bracket wires and a **live
  track minimap [later]** are SVG components. Fonts in `vendor/fonts/`.
- The app lives in `app/` (`/app/` on the dev server); `src/` stays DOM-free.

---

## 13. Open questions

1. Exact orange and the six lane colours (decide on the shell).
2. Time Attack and Training: two tabs or one (§7).
3. How the Power Pack reports position for "distance covered" (hardware not
   designed yet).

## 14. Build order (proposal)

1. **Shell:** sidebar rail (hover/tap expand), areas with placeholder pages
   and icons, theme tokens, fonts, i18n plumbing, connection badge. → user
   review of the look.
2. Status area (backend exists).
3. Race Presets (data model + editor).
4. Grand Prix for a single preset (live race screen on the existing engine).
5. Time Attack.
6. Tournament data model + scaling templates + Grand Prix for events.
7. Bracket builder.
8. Training; Track Setup when the Power Pack exists; German translation.
