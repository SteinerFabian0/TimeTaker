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
  Groups stay separated by a gap; System sits at the bottom and Plan is
  centred between Drive and System (equal gaps above and below it). Collapsed,
  it sits on the screen's bottom edge with Settings' bottom border just
  below it, so selected Settings is a tab with no bottom side and no
  downward line; open, the group lifts a few pixels so the parallelograms
  are whole.
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
  lines **draw out** from the tab's corners, up and down to the screen
  edges (0.45 s). Opening: the lines vanish at once (so none sweeps across the
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
| **Plan** | **Race Presets** | Create, rename, edit race presets. |
| | **Tournaments** | Build tournament templates from race presets (the bracket builder). |
| | **Car Presets** | Sets of cars: per lane the car type, its colour and its number (§5a). |
| **System** | **Track Setup** | Race timing (start lights, minimum lap time); **[PP]** speed level, power limits, lane compensation, calibration. Expert mode. |
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
- **Lane colours:** every lane has one colour, used everywhere a lane
  appears (lap tiles, tables, bracket seat lists, start lights preview). Six
  distinct colours that work on dark and light are the **defaults**
  (Settings). A Grand Prix overrides them with **its cars' colours** (step
  Cars, §6.1a), so the screen matches the real cars on the track.
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

### 3.4 Number inputs (− / + steppers) (decided 2026-11-23)

**Sizes (decided 2026-11-24, replacing "as wide as the widest allowed
value"):** the − and + boxes are 44 × 44 px, and the number field is **at
least as wide as one of them**, so a one-digit stepper is three equal thirds.
The number **grows with what it shows** (digits, decimal mark) once that no
longer fits, and shrinks back; it is not sized for the largest value allowed.
A **unit** ("min", "s") comes **on top** of that: "5 min" is a 44 px number
plus the unit, never squeezed into 44 px. The underline runs under the number and unit, with the same gap to − and +
(it shifts with the slant). Typed values are clamped and rounded to the step.

| Input | Range | Step |
|---|---|---|
| Laps | 1–99 | 1 |
| Race time | 1–120 min | 1 min |
| Warm-up laps | 0–9 | 1 |
| Races per lane | 1–9 | 1 |
| Points per place | 0–99 | 1 |
| False-start penalty | 0–60 s | 0.5 s |
| Group size (summary) | 2–6 drivers | 1 |
| Attention pulses | 0–99 | 1 |
| Pulse rise | 0.1–1.0 s | 0.1 s |
| Countdown step | 0.1–2.0 s | 0.1 s |
| Hold, at least / at most | 0–10 s, at least ≤ at most | 0.1 s |
| Minimum lap time | 2–99 s | 1 s |
| Average lap (estimate) | 5–120 s | 1 s |
| Changeover (estimate) | 15–600 s | 5 s |

---

## 4. Race Presets

### 4.1 Layout

Left: the presets in **three separate lists, one per kind** (decided
2026-11-24, replacing a race/qualifying switch in the editor): **Qualifying**
on top, **Races** below, **Tiebreaker** last. A preset's kind is fixed when it
is made and a duplicate keeps it; kinds are never interchangeable, so Grand
Prix features and looks can be built for each kind. Each item: name + a
one-line summary such as "5 laps · 3×/lane". Below the qualifying list **+ New
qualifying**, below the races **+ New race** (dashed boxes in **grey**, not
orange, so the selected preset is the only orange in the list). The host orders each list by **drag and
drop** (mouse: drag the item; touch: drag its grip; keyboard: Alt+↑/↓), only
within its kind; the order is stored (setting `presetOrder`). A new preset
goes to the end of its kind, a duplicate right below its source (decided
2026-11-23). Stage dropdowns (Tournaments, Grand Prix) list qualifyings and
races under those two headings. Right: the editor of the selected one.
Actions: rename (inline), duplicate, delete (with confirm; refused while a
tournament template uses it, naming the template). Shipped **default presets**
are editable in place and are never deleted; instead they have **Reset to
defaults** (decided 2026-11-23).

**Changed shipped presets** (decided 2026-11-24, built): every setting of a
shipped preset that differs from its shipped value is **blue** (label, value,
selected option; the name too when renamed), with a **↺** right beside the
control that puts just that value back (tooltip: "Back to the default: …";
points: per place, and the points table as a whole). **Reset to defaults**
then gets a thin blue border, and a new button **Save as new preset** appears
(not for the tiebreaker): a confirm dialog says the changes are saved into a
new preset and this one is reset to its defaults; on confirm the copy (same
kind, "Default race 2", right below) is selected and the shipped one is back
at its defaults.

The sidebar items lean with the slant, so each item's **name row starts
further right** than its summary row: both keep the same distance to the
slanted left edge.

**Tiebreaker** (decided 2026-11-24, built): there is **one**, shipped, in its
own list; it has **no Duplicate** and no "+ New". It runs only when a tournament breaks ties with a tiebreaker race
(§5.2), so it is **never offered as a stage's preset** (Tournaments' preset
dropdowns, the Grand Prix dropdowns). It stays editable and resettable. It
has **no points table** (its editor hides it): a tiebreaker hands out no
points, it only orders the tied drivers. Its summary shows the length for
**2 and 3 drivers only** (a tie of 4 or more is not expected).
Tournaments → Rules no longer picks a tiebreaker preset: with "Tiebreaker
race" it names the Tiebreaker preset and links to it.

### 4.2 What a race preset holds

| Setting | Values | Notes |
|---|---|---|
| Name | text | |
| **Kind** | **qualifying**, **race** or **tiebreaker** (fixed, §4.1) | stored as `kind`; `mode` follows (qualifying: time, else laps) |
| Laps (race, tiebreaker) / minutes (qualifying) | number | |
| **Warm-up laps** | 0…10 | untimed laps before the start, only when the drivers sit down or change lanes (§4.2 estimate). Counted in the duration estimate |
| **Races per lane** (k) | 1…n | each driver drives every used lane k times |
| **Rotation order** | **block** (k races on a lane, then move; the reference evening) or **rotate every race** | **hidden at 1 race per lane** (both orders are the same; keep the editor clean) |
| **Points table** (not for the tiebreaker) | points per place, 1st…6th | default 6-5-4-3-2-1, a qualifying **10-7-5-3-1-0** (rewards pushing); editable list; presets "6…1", "10-7-5-3-1-0" (was 10-7-5-3-2-1 until 2026-11-24), "F1-style" |
| Bonus points **[idea]** | fastest lap +1 | |

**Race or qualifying** (decided 2026-11-24, replacing a free laps/time
choice; since the same day separate kinds, §4.1): a **race** ends after a fixed number of laps, the first to finish
wins, places give points. A **qualifying** is a **timed session** (minutes)
per run instead of a lapped race, and it **rotates over the lanes like
races** (races per lane and rotation order as usual: 6 drivers on 6 lanes,
1×/lane = 6 sessions, each driver on every lane once; each with its warm-up,
then the session). Every session is **ranked by fastest lap** and **gives
points after the session** by the points table, like a race (the host sets
custom points for qualifying places as for any preset; changed 2026-11-24
from "one session for the whole group"). Distance-in-time
races and endurance stay out until the Power Pack. Not built yet: the race
page runs lapped races only; ranking a qualifying by fastest lap comes with
the timed session.

Start light timing and the minimum lap time are **not** preset settings: they
belong to the track (§8.0, decided 2026-11-23). **Crashes, the result basis
and false starts are not preset settings either** (decided 2026-11-23): they
are the tournament's rules (§5.2a), so they stay the same for every race of an
evening; players would be confused if they changed between rounds.

The editor shows a live **summary card**: races per group for N drivers
(k × N), and an **estimated duration** (races × (laps × average lap + changeover) +
warm-ups × warm-up laps × average lap); average lap and changeover are Track Setup values (§8.0), defaults 27 s
and 60 s. **A warm-up only when the drivers sit down cold or change lanes**
(decided 2026-11-24): before a group's first race, the first race of each
section (another group raced in between), and after every lane change
(block: every k races; rotate every race: every race). Racing one lane 3×
in a row: one warm-up. `warmupBefore` / `racesEstimateMs` in
`src/presets/preset.ts`; the race page skips the warm-up laps for a race
that needs none. Reference evening: 18 races × (5 × 27 s + 60 s) + 6 × 2 × 27 s ≈ 64 min per group.

**Decided 2026-11-23 (built):** shipped presets, in default list order:
*Default race* (3×/lane, 5 laps, 2 warm-up laps), *Short race* (2×/lane, 3
laps, 1 warm-up lap), *Qualifying* (5 min, 1×/lane, **2 warm-up laps**, 10-7-5-3-1-0; added 2026-11-24; a new qualifying also starts with 2)
and, apart, *Tiebreaker* (1×/lane, 4 laps, 1 warm-up lap), all crash = out,
crash order, 6…1. Points are edited one row per place, 1st at the top. The result basis is shown only when the
crash policy is "car is out". The summary card shows a **table of the group length per
number of drivers** (2 … the track's lanes: races k × N and the estimated
time), since a smaller group uses fewer lanes and is shorter. Average lap and changeover are in Track Setup → Lap timing → Duration
estimates. Data and rules: `src/presets/preset.ts`; stored in the IndexedDB
database `timetaker-library` (memory fallback when storage is blocked).

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

A group only races on **as many lanes as it has drivers** (decided
2026-11-23): 5 drivers on 6 lanes leave one lane empty for the whole group.
The lanes swap until every driver has driven every *used* lane k times, so a
group always has k × drivers races, however many lanes the track has. Which
lane stays empty is chosen per group **in the Grand Prix**, on the group's
race page (§6.1b); the default is the **last lanes** (5 drivers on 6 lanes:
lane 6 stays empty; decided 2026-11-23, replacing "centre out"). The host
moves a car onto the empty lane: that driver starts there, his old lane is
empty for the whole group. Rules: `src/presets/schedule.ts`
(`defaultSeating`, `moveToLane`, `raceScheduleSeated`: block or every-race
rotation over the used lanes only).
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

### 5.2a Race rules (decided 2026-11-23, built)

Every tournament template has **rules** for all its races: crash policy
(crash = out / replacement allowed), result basis **[PP for distance]** (§4.3)
and false start (time penalty in seconds, added to the first lap or the total,
or a flag only). Tournaments → the **Rules** tab below the tree. 

**Small final** (decided 2026-11-23): a template option, **on** by default
(the small final's tab below the tree: "Race a small final: yes / no"). When
on, the tree shows it under the final whenever the field leaves at least 2
for it; it races the final's preset. Only the split of seats between the
final and the small final (▲ / ▼) is left to the Grand Prix. Data:
`src/presets/rules.ts`; `toRaceConfig(preset, rules, track, …)`.

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
| **Tiebreaker race** | only when the tie matters for routing (e.g. the last qualifying place). Before the next stage starts, Grand Prix shows "Tiebreaker: just these drivers", puts them on the **inner lanes** and runs a **tiebreaker race preset** (template setting; shipped default "Tiebreaker": 1 race per lane, 4 laps). Its result orders the tied drivers. |

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

**Built 2026-11-23, reworked the same day at the user's request (Tournaments
tab).** The **tree is the page**: no left list. At the top, the title and the
tournaments as tabs (+ New tournament). Below them the **stage**, which runs to
the right edge of the screen: a bar with the editable name, the shape (fixed
for shipped tournaments, with a Built-in tag; a segmented choice for your own)
and Duplicate / Reset or Delete; then the tree **left to right** (kept over top
to bottom because laptop screens are wide and short), with column titles
("Drivers · Round 1 · Groups · Round 2 · Second chance · Final"). The tree is
drawn **like a node graph** with plain rectangles (no slant, to save room):
every stage lists its **places on its right edge** ("1." … its size, so a group
of 5 shows 1.–5.; a vertical "Platzierung" / "Place" label runs up in front of
them), and every place that goes on has **its own line**. A stage fed by
places lists those **seats on its left edge** ("A3" = 3rd of Group A, "S1·1" =
winner of Second chance 1; German "H1·1"), where the line ends. Places that go
nowhere are greyed out. Places that **skip a column** (groups straight into the
final) do not snake across: a short line ends in a small **tag** naming the
target ("Final"), and the receiving card has the matching tag in front of
those seats ("Group A"). The tagged seats sit on top, the wired ones below
them, and a column's only card (the final) is moved up or down so its wired
seats face the places feeding them (decided 2026-11-23); lines between
neighbouring columns run straight past the tag zones before they bend, so no
line passes behind a tag. Groups show where their drivers come from
("Seeds 1 · 4 · 5 · 8"). Cards size themselves to the width there is. Last comes a footer with the driver stepper ("on 6 lanes ·
Change in Track Setup") and the totals (races, whole evening). If a field does
not work, the tree is replaced by a plain message. **Clicking a card selects
its kind** (all groups together): its wires turn orange, and below the stage a
tabbed panel (Drivers / Groups / Second chance / Final) explains in sentences
what happens there, next to its settings: group split; race preset per kind of
stage (with the preset's summary); final seats for second-chance winners;
tie-break (shared). "Repechage" is **"Second chance"** in English
(Hoffnungslauf in German). The lane count comes from Track Setup (§8.0);
the planning rules are unchanged (`src/tournaments/`). Stored in
`timetaker-library` v3, store `templates`. A race preset used by a template
cannot be deleted. Not built yet: free per-node editing of custom templates
(step 7) and running events on Grand Prix.

---

## 5a. Car Presets (decided and built 2026-11-24)

A **Car Presets** area in the Plan group, under Tournaments (car icon). A
car preset is a named set of cars, one per lane: the car's type
(open-wheeler / closed-wheeler), its colour and its number or make (at most
4 letters or digits). Master–detail like Race Presets: left the presets in
the host's order (drag and drop, Alt+↑/↓), each with a row of small chips in
its cars' colours, and **+ New car preset**; right the name (edited in
place), Duplicate and Delete (with a confirm), and the **car board**: the
same rows as the Grand Prix's Cars step (type dropdown, colour pop-up,
number field, the lane live with its car). No shipped presets; an empty
list shows a short explanation and a "New car preset" button. Stored in the
library database as the setting `carPresets`. Code: `app/pages/carPresets.ts`,
`app/lib/carPresets.ts`, `app/ui/carBoard.ts`, rules in `src/grandPrix/cars.ts`.

The sidebar now holds nine areas; the rail's item height follows the screen
height for nine.

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

### 6.1a Setup steps (built 2026-11-23, waiting for the user's feedback)

A small **timeline** at the top: Tournament → Drivers → Cars → Overview → Races →
Podium (slanted segments; done steps show a tick and a summary, e.g. the
tournament's name or "12 drivers", and can be opened again; later steps are
dashed until they can be reached). Every setup page ends in a footer with
**Back** and **Next** that stays in view.

1. **Tournament:** "Choose a tournament template to start a Grand Prix."
   Every template as a big parallelogram card, **two columns** (four
   templates make a 2 × 2 matrix): name, Built-in/Own tag, its flow as chips
   (Groups → Second chance → Final), a description (by format and draw), and
   the race preset of each stage. Click selects (double-click goes on);
   link to the Tournaments area.
2. **Drivers:** the list and the side panel are centred as one block; the
   cards are sized for a 12-character name (the limit, since 2026-11-24; was 20). One parallelogram
   card per driver: seed number, the name
   (click and type), × to remove, a **dot grip on the right** to drag (also
   Alt+↑/↓). Below the list a dashed **+ Add driver** area (as in Race
   Presets). Enter adds the next card, so names are typed in one go; an empty
   card disappears when left. Names are **letters and spaces only**; a name
   that is taken gets " 2" (" 3" …) automatically, and the host can click in
   and change it (e.g. to the initial of the last name). If the template
   splits the groups by seed, an orange **double arrow "Fastest ↕ Slowest"**
   stands left of the list with the note to order the drivers by skill;
   with a random draw the note says the order does not matter. On the
   right: the template, the number of drivers, what the plan makes of it
   (groups, second-chance races, final), races and the whole evening, or why
   the field does not fit. At most 48 drivers. Next needs at least 2 and a
   plan that works.
3. **Cars** (decided and built 2026-11-24): the host sets up the evening's
   real cars, one per lane. "Which car is on which lane?" Left the **car
   presets** (§5a): a click puts that preset's cars on the lanes (a copy),
   the preset that matches the lanes is marked, and a line above the board
   says "Cars from the preset …" or "Your own cars for this Grand Prix".
   Below the list **+ Save as car preset** (only when no preset matches)
   and a link to the Car Presets area. Every car can still be changed on the
   fly; that change is for this Grand Prix only. One row per lane
   (lane 1 at the top, as many as the track has): left a **dropdown**
   Open-wheeler / Closed-wheeler, then a **colour swatch** that opens a
   pop-up (16 usual car colours, "Other colour …" with the system picker, and
   "Lane default" = the colour from Settings), then a **text field** for the
   car's number or make (letters and digits, at most 4: "07", "BMW"), and
   right of it the **lane itself, live**: road, the car as it will look in
   every race (it drives up to the line when the page opens or its type
   changes), the checkered line and the lane number in the car's colour. The
   car's colour **is the lane colour for this Grand Prix** everywhere in it
   (name frames, number boxes, lap times, position table); the lane number
   is written dark or white, whichever reads on it, and a very dark or very
   light car is mixed with the text colour where it is text or a thin frame.
   The text sits on a **white plate with black letters** on the car: on the
   open-wheeler's engine cover (the helmet moves forward), on the
   closed-wheeler's roof. The cars are kept with the setup and stay when
   another tournament is picked (they are the physical cars).
   Code: `src/grandPrix/cars.ts`.
4. **Overview:** the same node tree as in Tournaments, but without the
   drivers column: it starts with the first round (Group A, Group B …), whose
   cards list their drivers by name (snake by the list order, or drawn at
   random with "Draw again"); places, lines and "Final" tags as in §5.4.
   Nothing can be selected here; in the Grand Prix the final is called the
   **Grand Final** ("Großes Finale"). Races and the whole evening above the
   tree. Every card shows drivers · races and its time, **no preset name**;
   instead a **race preset dropdown** sits at the bottom of the card
   (decided 2026-11-24, built). It lists the stage presets (not the
   tiebreaker) and defaults to the tournament template's. A different pick
   turns the dropdown **blue** and a circular arrow **↺** appears right of it,
   which goes back to the template's preset. The pick is per **phase**: all
   cards of a column race the same (group A's dropdown also sets group B; the
   Grand Final and the small final go together), and it is the same choice as
   on the phase settings page (§6.1b). Picking another preset drops the
   phase's laps / races-per-lane changes. The overview's other control is
   **▲ / ▼** right of the two finals, moving the final's last seat
   down into the small final or back (▼ only while the small final has room);
   the plan adapts (8 drivers: final 6 + small 2 → final 5 + small 3; for 12 a
   final of 5 becomes 1 direct seat per group and 3 second-chance races).
   The small final is the next best after the final: the places right behind
   the final's from the round that feeds it, by rank, at most the lanes
   (reference evening: places 2–4 of both second-chance races).
   **Start Grand Prix** opens the first phase (§6.1b).
   Logic: `planTournament(t, n, lanes, { smallFinal, finalSeats })`
   (`src/tournaments/plan.ts`) and `runOrder` (`src/tournaments/runOrder.ts`).

   **Decided for later (2026-11-23):** race settings change per phase, not
   per section (§6.1b); rules stay fixed for the evening. A
   driver can be withdrawn: his remaining races fall away, he ranks last in
   the running stage, the next one moves up; a seat he already holds goes to
   the next best from the same source. Whatever has not been raced yet can be
   changed; what has been raced only by correcting or re-running. Every
   change is undo-able and listed as "changed today".

(The Cars step was inserted 2026-11-24; the overview was step 3 before.)

The setup is kept in this browser (localStorage `tt.gp.setup`). Code:
`app/pages/grandPrix.ts`, `app/lib/grandPrix.ts`, `src/grandPrix/names.ts`.

### 6.1b Phases and the race page (built 2026-11-23, warm-up 2026-11-24, waiting for feedback)

The evening is one forward movement through the timeline: Tournament →
Drivers → Cars → Overview → **one tab per phase** (Groups, Second chance, Finals;
from the tree's rounds) → Podium. A phase tab opens once the Grand Prix is
started (for now only the first; the others follow with racing).

1. **Phase settings page** ("Group phase" …), the last chance to change the
   phase. Three columns (2026-11-24): on the left a **window onto the tree**
   (`Tree` with `focus`): only this phase's column, cards as in the overview
   but **without dropdowns** (information only), and every place leaving it
   ends in a tag naming where it goes ("Grand Final", "Second chance 1"; seats
   entering it the same way, from where). Not scrollable: it shrinks to fit
   the screen height. A click opens the **whole tree in a pop-up** (the page
   behind goes dark; Esc, × or a click outside closes; it scrolls inside only
   if the screen is too small), also without dropdowns. Planned: this tree
   fills in as the evening goes on (after the group phase its cards list the
   drivers by their finishing place instead of by seed) and is shown again
   on later pages. In the middle the settings, on the right the phase card.
   Every stage of the phase races
   the same settings (group A and B alike). Shown: the **race preset**
   (dropdown, same choice as the overview's; replaces the laps/time toggle,
   2026-11-24), laps (a race) or minutes (a qualifying), **races per lane**,
   and **take turns** (no / 2 / 3 sections, default 2; only with several
   stages; it sits in the phase card, above the order it changes). The
   tournament template's preset is the default; every value that differs
   turns **blue** with a circular arrow **↺** right of the control that goes
   back to the template (as the overview's dropdowns). Crash and false-start
   rules are not here (fixed, §5.2a). The phase card: races and time of the
   phase, take turns, and its order in two columns (sections: A ½, B ½,
   A ½, B ½). Changes count in the overview's totals. Data:
   `src/grandPrix/phase.ts` (`applyPhase`, `setPhaseField`, `phasesOf`).
2. **Race pages**, one **timeline tab per section** ("Group A 1/2", "Group B
   1/2", "Group A 2/2" …), each showing that section's first race: the
   start/finish line from above, turned so the lanes stack **1 (top) … L**,
   centred: per lane the driver's **name** in a parallelogram with the lane
   colour as border (all the same width), the **car** from above (the lane's
   car from step Cars: open- or closed-wheeler in its colour with its number
   plate, nose at the line, black axles; only a
   short piece of road behind it), one continuous checkered **line** down all
   lanes with the lane's **light** on its middle, and the **lane number** centred
   in its colour box. The whole board sits at the **right edge** (changed
   2026-11-24): line and numbers never move again. The room at the left holds
   a **huge three-line call**, "AUF DIE / PLÄTZE / WARMUP" (EN "TAKE / SEATS /
   WARMUP", the last line orange), as high as the lanes. Names are centred in
   their parallelograms; the lights are drawn **red**, like the track's. All
   lanes are shown; a
   lane without a car reads "Lane free". **Taking a lane out** (2026-11-24,
   replaced the "disable lane" link): car with its piece of road, line and
   number are one box; hovering it draws a **red cross** corner to corner, a
   click moves that lane's car
   to the (first) free lane, the name goes with it, and it holds for all of
   the stage's races; it is offered only until the warm-up of a stage's first
   race (Group A 1/2 and Group B 1/2), later sections show the lanes fixed. Names cannot
   be swapped otherwise (no drag and drop, decided 2026-11-23). This page is
   what the TV shows so everyone finds their seat.
   **Warm-up** (added 2026-11-24, reworked the same day): a big orange
   parallelogram **"Start warm-up"** sits in the footer between Back and Next
   (about three times Next's width). The host first puts every car just
   before the line. On press: the big call fades out, the lanes can no longer be taken out, and the
   stage's lanes are **locked for the whole group phase** (`gpLocked`,
   stored); the used lanes start detecting (`LANE_SAMPLING` ON); all lights
   go **off**. The name boxes, the track's edge and the cars slide to the
   **left edge** (line and numbers stay), each car's **warm-up lap
   "lap/laps"** sits right of its name, and the track
   stretches over the rest of the screen. Every counted crossing starts the
   next warm-up lap (0/2 → 1/2 → 2/2). The moment a car **starts its last
   warm-up lap** its light goes **solid red** (track and screen): stop before
   the line. A crossing after that shows e.g. **3/2 with the 3 in red**: the
   host sees it and decides (e.g. starts that lane further back); the app
   only shows it. With **one** warm-up lap no counter shows (that lap never
   ends; the light goes red at the first crossing), only an extra crossing
   (2/1, red). No lap timer. A crossing counts when it comes at least the
   track's minimum lap time after the car's previous counted one.
   **Cars on screen** (no Power Pack): an estimate. The cars run quickly back
   to the track's left edge (0.6 s), then drive to just before the line in
   **average lap time × warm-up laps × 0.7**; each car ±12 % on that and an
   uneven pace on the way, so they spread out. When the last car arrives, the
   "Warm-up running" button becomes **"Start race"**. With a Power Pack this
   becomes a real progress display (later).
   **Race** (added 2026-11-24; lapped races, time races later): "Start race"
   plays the **start light sequence** on the time taker (`START_SEQUENCE`
   with the track's start lights, all track lanes, lead 300 ms) and the same
   sequence on the screen's lights; the button reads "Start lights", then
   "Race running", then "Race finished". Pressing it again **during the start
   lights** stops them (the time taker aborts its sequence; `abortStart` in
   `app/lib/race.ts`); the button then reads **"Restart start sequence"**,
   which plays the light sequence again from the beginning (also after the
   time taker aborted the sequence itself). The cars **stand still** at the
   line. The track behind them **retracts** towards the cars and frees the
   room for the times; the counter shows the **lap the car is on, F1 style**
   (like the warm-up; `lapOn` in `src/grandPrix/raceClock.ts`, decided
   2026-11-24): **0/5** on the grid, **1/5** from its first crossing after
   lights out (the start: the cars stand just before the line, so it comes
   at once; it is not a lap and gives no time), 2/5 after lap 1, and so on;
   after the flag 5/5. Per lane,
   always at full width **0:00.00** (minutes:seconds.hundredths, one minute
   digit, at most 9:59.99; every digit in a box of its own width, so the
   columns never shift): the **lap in progress** in the lane colour (lap 1
   from lights out, as the engine times it: the start crossing does not stop
   or restart the clock); after each lap **its time for 5 s**
   with "Lap n", **framed** in its colour so the driver spots it, then live
   again; a little apart, the lane's **best lap** in white. The **fastest
   lap of the race** is **purple text** wherever it shows. As a car **starts
   its last lap** its light goes **solid red** (track and screen) and stays
   red after the flag ("Finish" with its last lap). Every device event since
   "Start race" goes into the race's log and the race engine folds it
   (`src/race/raceEngine.ts`: false starts, minimum lap time, finish);
   `app/lib/race.ts`, `src/grandPrix/raceClock.ts`. Without a time taker the
   sequence runs on screen only (stand-in events), the clocks run, no laps
   count. Not stored yet: a reload ends the race (the event store comes with
   the results). The page fits a 1366×768 screen without scrolling (the
   rows size to the window height; the note runs the full width; the footer
   sits on the window's bottom edge). Warm-up and race draw a faint line between
   the rows, from just left of the lap counter into the track; the lap
   counter and the times scale with the row height.
   **Dev tools** (Settings → Developer tools, on by default until the first
   release: `DEV_TOOLS_DEFAULT` in `app/lib/prefs.ts`): during the warm-up
   and the race a small "Dev" bar beside the note has a button per lane
   (and the keys 1–6) that acts like that car crossing the line, to try the
   pages without a time taker (`simulateCrossing` in `app/lib/race.ts`).
   With a time taker connected only real crossings count. In the stand-in
   race (no time taker, dev tools on) the cars also cross the start line by
   themselves just after lights out (0.2–0.5 s), as real cars would: that
   first crossing starts the lap timing and is not a lap, so every press
   after it is a lap (a press under the minimum lap time is ignored and the
   clock runs on). A **"Pre-race"** button in the bar puts the race back to
   before its warm-up, from the warm-up or the race (`resetToPreRace`: a
   running start sequence is aborted, race and warm-up are gone, the race no
   longer counts as finished, a stage's first race frees its lanes again).
   The lap times sit on the row's middle, level with the names. The name
   boxes are sized for **12 letters** (the name limit) at the names' size
   and grow with the rows; a name of wide letters gets a smaller font
   instead of being cut.
   **Race layout** (2026-11-24): with the start the race length text ("5
   laps · 2 warm-up laps") fades out. Names, lap counters and times move
   right, up to a small gap before the cars; **column headers** "Current lap"
   and "Best lap" sit above the times (a just-finished lap shows its number,
   "L3", beside the time; after the flag a small chequered tag). The freed
   left side holds an **F1-style position table**: "Lap n/m" of the leader
   (then "Finish") **huge**, "Lap" small in its top right corner, in an orange strip that fills the room above
   the rows so the table ends level with the last lane, and per position the place, the lane colour, a
   three-letter code (`shortCodes` in `src/grandPrix/names.ts`: ANN, unique)
   and the gap to the leader ("Leader", "+0.40", "+1 lap"); it updates live
   and rows slide when places change (`rank` in `src/race/rules/standings.ts`).
   **Later, with a Power Pack:** a lapped car shows a real time interval to
   the leader (and to the car ahead) instead of "+1 lap", from where each
   car is on the track. Open until the Power Pack exists: how precise its
   position tracking is decides whether that interval is shown to the
   hundredth, rounded, or not at all.
   The warm-up keeps counting and driving while the host looks at another
   tab; a reload ends it (the lock stays). Data: `src/grandPrix/warmup.ts`
   (`addCrossing`, `laneWarmup`, `carDrive`), `app/lib/warmup.ts`; crossings
   come through `trackLink.subscribe` (EVT_CROSSING).
   **Timeline:** with this many tabs the bar scrolls by itself: about half a
   second after a tab opens it slides smoothly so that at least the two tabs
   on either side of it are in view (at once when the page loads); tabs that
   already show that do not move it. **Nothing after a race tab opens until
   its race has finished** (`gpDone`, stored; added 2026-11-24): while Group
   A 1/2's race is not finished, Group B 1/2 and everything after it stay
   closed; going back is always possible. (For now a section counts as
   finished with its first race; with all races of a section, all of them.)
   **Lane lights:** while the page is open the time taker lights the lanes
   with a car and turns the free ones off, following every move (during the
   warm-up: only the lanes in their last warm-up lap); leaving the
   page turns them all off (`LANE_LED`; `app/lib/laneLights.ts`,
   `src/track/laneLights.ts`). Not sent while a start sequence or a race
   session drives the lights. (The app's connection to a real time taker is
   wired with the Status area; until then the page says no time taker is
   connected.)

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

## 8. Track Setup

Two parts: **Race timing** (§8.0) works without the Power Pack; **Track
power** (§8.1, §8.2) needs it. Without it, the power part shows what it does
and "Connect a Power Pack to use Track Setup" (§9).

### 8.0a Track: lanes [v1] (decided 2026-11-23)

At the top of Track Setup: **Lanes on this track: 2 / 4 / 6** (big segmented
buttons, default 6). Stored in the library database (`settings` →
`trackLanes`); rules in `src/track/lanes.ts`. Everything that plans races uses
it: the tournament tree, the group size in the race preset summary, and the
start light preview. A connected time taker caps it: options above the lanes
it reports are locked, and if the setting is higher it **switches down** to
the largest that fits (and stays there after the time taker is unplugged).

### 8.0 Race timing [v1] (decided 2026-11-23)

Per track, used by every race whatever preset it runs. Stored in the library
database (`settings` → `trackTiming`); data and rules in
`src/track/trackTiming.ts`. One **Reset to defaults** for the section.

- **Start lights** panel, grouped: *Attention* (pulses, pulse rise),
  *Countdown* (next light after), *Random hold before lights out* (at least,
  at most). Below the fields a **preview**: one red light per lane of the
  connected time taker (6 without one), lane 1 on the left. A **Preview**
  button next to *at least* and one next to *at most* play the sequence with
  that hold there (`src/track/startLightsPlan.ts`, same timeline as the
  device) and, when a time taker is connected and idle (no race session, no
  sequence running), on the track too (START_SEQUENCE, armSampling off, the
  chosen hold sent as a fixed range so both go out together).
  **Stop** aborts it on the track as well.
- **Lap timing** panel: minimum lap time (any faster lap is discarded), and
  the **duration estimates** (average lap, changeover between races) used for
  presets and tournaments (moved from Settings 2026-11-23; still stored as
  app prefs, not part of the track timing reset).


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
- Gated: Track Setup's track power part; result basis "distance"; automatic crash
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

Built 2026-11-24 (`app/pages/status.ts`, wiring in `app/lib/device.ts`):

- **Connection** lives here: Connect USB / Connect Bluetooth (the browser's
  picker) / Simulator, Disconnect. The choice is remembered per browser: after
  a reload the app reconnects to the simulator or to a time taker this
  browser was granted, without asking. Shows protocol, boot id, uptime, round
  trip, firmware versions; **Self check** and **Lane power on/off**.
- `trackLink.lanes` (the lane count the rest of the app sees) changes only
  after the present lanes held for 3 s, so a chain that is still joining does
  not shrink the Track Setup lane setting.
- **Lane power** switches on by itself once per boot of the control module
  (the rail is off after every boot); switched off here, it stays off until
  the next boot.
- **Cards:** control module and BLE bridge side by side; then the **Lanes**
  panel, two lanes per row (1 2 / 3 4 / 5 6, one per row on narrow screens),
  up to the track's lane count (more if more are present). Each lane card
  holds its live values, graph and Calibrate button on top and its checks
  below. An offline lane shows only its "Present" row. Check titles are translated; the messages come from `src/health` in
  English. Hovering a row shows the check's description.
- **Sensor readout:** a 60 s graph per lane (signal with its min…max band,
  baseline, thresholds, reference, markers); Calibrate per lane / all, Forget
  all, each behind a confirm ("lanes must be free"). Refused while a race runs.
- **Event trace:** collapsible, the last 300 decoded events and link notes.
- **Simulator panel** (developer tools on): cars, drop events, BLE chip
  silent, sensor fault, power cycle.

---

## 11. Settings

- **Language:** English **[v1]**, German **[v1 after English]**.
- Theme: dark / light / follow system.
- Lane colours: the six defaults (a Grand Prix replaces them with its cars'
  colours, §6.1a step Cars). Editable (2026-11-24): a click on a lane's
  swatch opens the system colour picker; a changed lane gets the "changed"
  underline, "Back to the default colours" resets all. Per browser
  (`app/lib/laneColors.ts`); a chosen colour applies in both themes.
- Number/time format follows the language (German: `27,384 s`).
- Data: export / import presets, templates, drivers (JSON file) to share with
  colleagues; export events/races (`timetaker-race` exists).
  **Built 2026-11-25** (`src/store/libraryFile.ts`, `app/lib/library.ts`):
  one file `timetaker-library-<date>.json` with race presets, tournament
  templates and car presets (own items, shipped ones only when edited).
  Track timing and lane colours stay with the track / browser. **Import only
  adds**, never overwrites or deletes: an item already here is left out; a
  clashing id or an edited shipped item comes in as a copy (new id); a taken
  name gets " 2"; templates follow their presets to new ids; the tiebreaker
  is never imported (there is only one). A confirm dialog shows the counts
  first. Drivers and events: later.
- About: app version (`app/lib/version.ts`), protocol version (and the
  time taker's, when it differs), control module and BLE firmware when
  connected, and whether this browser has USB (Web Serial) and Bluetooth
  (Web Bluetooth). Built 2026-11-25.

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
