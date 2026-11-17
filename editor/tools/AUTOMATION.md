# Editor automation (background control without the desktop)

`MA2 Scenario Editor.exe --automation [PIPE] [--automation-show] [--settings FILE] [file.ma2map]`

- Developer flag; normal launches never open the pipe.
- Which exe the tools start: `edctl.py launch` starts `out\MA2 Scenario Editor.exe` (env `MA2SE_EDITOR`
  overrides), whatever build that file is. It can be old: run plain `build.cmd` first and compare the hash. Publish only an exe built under that final name (no `MA2SE_EXE=...` override).
- The window stays hidden (`--automation-show` shows it without taking focus) and keeps rendering when covered
  or minimized. Client area 1584x861 (window 1600x900) unless `--window-size WxH` sets another client area at launch
  (`edctl.py launch --window-size 1280x720`) or the `window_size` command resizes it later (see below).
- Settings: `--settings FILE`, default `%TEMP%\MA2SE-automation\ScenarioEditor.ini`, with its own recovery slot
  beside it. Tests never change the user's preferences, keys or recovery copy. No recovery offer at start.
  The scenario folder (Save / Save as / Open, the Options tab) and its Scenarios.ini also live beside the settings
  file (`<settings dir>\Scenarios`), so test saves never reach the user's real Scenarios folder.
- Message boxes and file/folder pickers never appear: they are answered by the `dialogs` command
  (default answer "no"; file pickers return `next_file` once, else cancel). Exception: Save / Save as with no
  `next_file` saves under a new name in the scenario folder (like Test) and the status line gives the full path.
- Input goes into ImGui's queue (no OS input, no focus change). Edge scrolling treats the window as focused.

## Transport

`\\.\pipe\PIPE` (default `ma2se`), byte mode, current user only (explicit DACL), remote clients rejected.
One client at a time; one JSON object per line in, one JSON reply line out (`"ok": true|false`, `"error"`).

## Commands (`"cmd": ...`)

| cmd | fields | reply |
|---|---|---|
| ping | | ok |
| state | | mods_down[], keys_down[], mouse_down[] (what the automation holds: all empty at rest), test_launch_skipped, test_command, test_dir (see Test below), tab, tool, camera, windows{help,getting_started,new,about,check,retail,preferences,message}, popup_open, message_title/text, confirm_text, status, last_dialog, hotkey_capture, edge_scrolling, right_dragged, test_running, preview{on,valid,reason,heading_deg}, gizmo{mode 0 move/1 rotate/2 tilt, hot, drag handle ids}, undo, redo, id_conflicts, check_items[], doc{name,file,dirty,multiplayer,read_only,objects,triggers,graphics_mb_last_save,graphics_mb_live (the footer's graphics-memory figure; equals the last save after a save and Check)}, selection[], selection_count; adds: windows.keys (shortcut sheet), viewport{x,y,w,h} (screen px of the map view), brush{radius,strength,hardness}, undo_label (top undo step), box_selecting, dragging, context_menu_open (the right-click object menu is open) and paste_armed (the paste ghost hangs on the mouse; tool PasteGhost), keys_fired{action label: count} and last_action (what the key dispatcher ran: one key press must add exactly 1 to exactly one label), keymap_ok (the key-name table equals ImGui's), palette_filter (text of the palette search box), text_input (a text field has the keyboard); adds: orbiting (Alt+left-drag is turning the view), orb_press (an Alt press is waiting: a click on release, a turn past 6 px); Ctrl+wheel / Shift+wheel with a brush tool change brush.radius / brush.strength (`mods` on the wheel command); adds: paste{count,heading_deg,duplicate,over_map}, labels[] (helper labels drawn on the map: text, uid, x0..y1, cx, cy; a click on one picks what it names); the object menu's items are reached as `##objmenu/Duplicate`; adds: undo_cap (200 steps), undo_bytes (snapshot memory, shared buffers counted once; 1 GiB budget), undo_parts{world_meta,heights,colours,types,ground_weights,ground_layers,audio} (bytes), saved_state (true when the document is exactly the saved one: dirty follows undo / redo) |
| dump | | text: the developer state dump (--play dump). About 1,700 tokens: prefer `sel`, `objects`, `ground`, `state` |
| sel | | `sel[]`: the selected objects (cap 50, `truncated`), `count`. Row: uid, name (palette / friendly name), class, x, y, z, heading, pitch, roll (degrees), ground_drawn, ground_saved, snap_family (only when 1 wall / 2 bridge / 3 foundation / 4 road), sx, sy (screen position of the pivot in window pixels; absent when it is off the map view). `sel` only: `crop` = the kept rectangle [x0, x1, z0, z1] in model metres or null; `size` = `{kind: none / road / plate / run / water, w, l, whole_w, whole_l, along_x}` (w across, l along: a road's or plate's z, a wall's or bridge's long axis; metres kept and uncut); `why` (kind none: the line the Size section shows); `indestructible` (the `Indestructible` flag), `has_wreck` (a `DestroyedBuilding` is set); `joints` = the world points the next piece joins at, `[[x, y, z], ...]`, a cut piece's cut ends |
| palette | limit (default 20) | The open object browser (Place, Roads & walls or Scatter page; an error on any other): `mode`, `chip` (the lit chip), `view` (Pictures / List), `cols`, `count` (all entries after search and filter), `entries[]` in list order: name, `uid` (or `road`: the .road path), tier (search ranking,: 0 = a word of the shown name or its kind label starts with the typed word, 1 = inside a word of it (5+ letters), 2 = official name or model file word, 3 = synonym / category / group word, 4 = 5+ letters inside the official name glued; every typed word must match, the tier is the worst; without a search all 0), theme (false = comes from another level), cut, in_scatter, size (modular pieces: roads "60 m"; walls, bridges and bases "140 m" or "38 x 38 m" from the model box, a few frames after the first read because the model has to load), gfx_mb (MB an other-level object adds; the first `limit` rows are measured on the read) |
| objects | filter (substring of name, description or class), cx, cz, r (near; nearest first, adds `dist`), limit (default 20, max 500) | `objects[]` (same rows as `sel`), `count` (all matches), `truncated` |
| triggers | none | The Triggers tab's list: `rows[]` `{uid, name, kind, section ("yours" = made in this map, "level" = one of the Base level's own), depth, enabled, repeat, actions, orders}` (first 200), `yours` / `level` (counts of top-level and nested triggers), `cutscene_hidden` (a cutscene's start / end triggers and cue timers, left out: they live on the timeline), `picked`, `level_open` (the "From the level" section is unfolded) |
| trigger | uid (default: the picked trigger) | `{uid, name, kind, cls, section, enabled, repeat, seconds (timers), actions[] {uid, cls, label, ActionEntity / MarkerUID / cinemaID when it has them}, watches[] (Destroyed / Logic triggers: the uids they watch), orders[] {uid, unit, kind (patrol/defend/follow/attack), points, unit_side (Nation), unit_ai}, warnings[]}`. Reading a text box: `state.text_input` is true while one has the keyboard (New trigger, Duplicate and Rename focus the Name box) |
| cutscene | uid (default: the picked cutscene, else the first) | `uid`, `name`, `total` (s), `played_by[]` `{trigger, at}` (a timer's delay in s, -1 for another kind of trigger), `shots[]` `{n, uid, start, len, move_len (the eye path's own duration, 0 without a path), still, fov, init, exit, eye_path, focus, eye[[x,y,z]...], look}`, `events[]` `{uid, t, shot (1-based), offset, lane (Sound / Text / Units / Other), label, cls, order, holder (the trigger it hangs under: the shot's start trigger or a cue timer), unit (orders)}` in time order, `end_actions[]` (same row, under the last shot's end trigger), `selected_event`, `framing` null or `{on, ctrl, which (0 still, 1 start, 2 end), eye [x,y,z], fov_deg}` (the map view is that shot's camera while it is on), `glyphs[]` `{label ("2a" start, "2b" end, "2" still), shot, x, y}` (the shot glyph labels drawn over the map while a cutscene is picked; x, y = window pixels of the eye point), `preview` `{on, t, paused}` (paused = a ruler scrub), `strip` `{shown, folded, auto_folded, shot (1-based, the shot the panel shows), playhead, rect{x,y,w,h}}`. Same reader as the preview and Check (core/cutscene). The timeline strip sits under the map view on the Cameras & Cutscenes tab while a cutscene is picked (148 px x UI scale; header only when folded: `Hide timeline##tl_fold` / `Show timeline##tl_fold`, saved as `[Layout] Timeline=`; folds itself when the map would be under 300 px; `state.viewport` is the map without the strip). Items: `Preview##tl_play` / `Stop##tl_play`, `Add shot##tl_add`, `##tl_ruler` (click / drag = playhead, preview paused), `##tl_shot<n>` (click selects; drag sideways past half the neighbour reorders), `##tl_edge<n>` (drag = shot length, 0.5 s steps, Shift = free, one undo step `shot length`), `##tl_end`. The left panel shows the picked shot: `##shotlen` (typed Length), `Still##cam` / `Moving##cam`, `Draw a path on the map`, `Aim at view centre`, `##shotzoom`, `More##shotmore` (`##movelen`), and one `Shot n holds still for X s ...` row (with `Move over the whole shot`) per shot whose camera move ends early |
| (framing) | | On a picked cutscene the shot panel has, under Camera: still shot `Frame it` / `Use this view`; moving shot `Frame start` / `Start = this view` / `Frame end` / `End = this view` (items are in the scrolling left panel: scroll it with `mouse wheel` at x 250 until the item is visible). A Frame button puts the map view at that shot camera with the 2.35:1 game band (rest dimmed, banner on top); the camera keys and mouse work as usual (looking up is allowed while framing); Enter (action `Use this view for the shot (while framing)`, context Frame) writes the view and leaves, Escape leaves without writing (the editor camera comes back). `Use this view` without framing writes the editor view as it is (eye = camera eye, look = camera target); undo labels `shot view`, `shot start view`, `shot end view`. `state.camera` has `y` (target height), `eye` [x, y, z] and `fov_deg` (vertical angle) since. Camera glyphs: while a cutscene is picked every shot draws a 4 m frustum at its start and end eye point toward what it looks at; the eye points are ordinary waypoints (click the glyph's point to pick it, PgUp / PgDn or the lift knob change the height; `cutscene.shots[].eye` follows). A shot length typed in Properties (CameraDuration) now carries the eye path's duration along when the two were equal. |
| handles | | The orange size handles of the one selected road piece, base, wall or bridge (Select tool, Move mode): `rows[]` `{id, label, kind (side / corner), sx, sy (mouse-command pixels), x, y, z (world), glued, on_screen}`, `count`; when none show, `hidden_reason` ("rotate mode", "tilt mode", "too small on screen", "not sizable", "more than one selected", "nothing selected"...). `id` is a bit mask of the sides it moves: 1 left (x min), 2 right (x max), 4 back / end (z min), 8 front / start (z max); a corner has one of each. Labels: roads start / end / left / right; bases front / back / left / right and the four corners ("front-right"); walls, fences, bridges end 1 / end 2. A handle on a glued end is `glued` (pressing it only says how to unglue); an end with a lock icon stands 28 px further out so the lock stays clickable. Drag a handle with `mouse drag` from its sx, sy: the side follows the mouse (0.1 m steps, the snap grid when on, Shift = 0.01 m), one undo step "size", the readout ("Length 40.1 m of 60 (end cut 19.9)") in `state.size_gizmo.readout` and the status line. Two single clicks on a handle: that side whole again. `state.size_gizmo` = `{hot (handle under the mouse, else the Size panel's), drag, label, readout, cursor (ResizeEW / ResizeNS / ResizeNESW / ResizeNWSE), hint, joint (" - meets the end of X" / " - flush with X" while a joint holds the dragged edge; the same text ends the readout), panel_hot (the handle id of the Size field under the mouse)}`. A dragged cut edge snaps to the open end of another piece of its family (roads, walls, bridges: lands flush on it) or to the edge lines of other bases, building footprints and straight-road kerbs (bases), within the placing snap's reach; the snap grid (kept length in whole steps) and 0.1 m steps come after it; Shift turns all of it off. The Properties > Size section has the fields `##size_start / ##size_end / ##size_left / ##size_right` (roads), `##size_front / ##size_back / ##size_left / ##size_right` (bases), `##size_end1 / ##size_end2` (walls, bridges), each with a caption item "Cut at ..." (hover the caption or the field to light its handle on the map); `Whole again`; the tick `Cannot be destroyed` (run pieces with a wreck); the why lines are items too (filter "keep their model's size", "Select one piece to size it"). A selected resizable water has the same handles (4 edges `front / back / left / right` change Length / Width, 4 corners both; the opposite edge stays and the centre moves by half the change along the water's own axes; 1 m steps, 8..1000 m, the snap grid when on, Shift = free) plus a 9th row `level` (kind `level`, id 16: the Move gizmo's lift knob, drawn water-blue; its readout starts "Level"). One undo step `size` per drag; the basin is refitted once when the mouse comes up (refit_moved_basins), inside that step. `sel.size` of a water = `{kind:water, w, l}`; a fixed-size water has kind none and the `why` line. Place water by dragging: press a corner, drag past 6 px, release: centred at that size in the Q / E frame (`water_drag`); a plain click places the boxes' 60 x 40 m. The window of the right panel is `##right/##inspector_...`, not "Properties"; `state.model_cache_crops` = cut variants of the selected piece's model in the model cache (1 after a drag ends) |
| (layout) | | The right panel's overview map is hidden by default on a short window (panel under 700 px) and has a button under Properties, `Hide the overview map##mmtoggle` / `Show the overview map##mmtoggle` (settings `[Layout] Minimap` -1 auto, 0 hidden, 1 shown): Properties never sits under it. The Size outline turns with the piece on the map view (quarter turns): the field of the side that is on the left on the map is on the left. View > `Brighten dark maps` (settings `BrightenDark`, default on) lifts a very dark ambient in the editor view only. Tooltips of the tool strip and both side panels open over the map beside the panel. A wide window (over 1920 UI px) gets a wider default left panel. Joints / Grid / Angle exist once (the snap bar); the placing panel no longer repeats them. |
| joints | near_px (default 400), limit (default 40) | `rows[]` the join points of placed roads, walls, bridges and foundations within `near_px` screen pixels of the mouse, nearest first: uid, name, fam (1 wall, 2 bridge, 3 foundation, 4 road), index, x, y, z, ox, oz (outward direction), open (nothing meets it), sx, sy (mouse-command pixels), `count` |
| fail_next_build | | F-V5R-1 test hook: the next file build (Save, Test or autosave) behaves as if the machine ran out of memory (one `std::bad_alloc`, once). The editor must answer with a message ("Not enough memory to build the file ...") and keep the map open and unchanged instead of closing |
| project | x, z, y (world metres) | Sx, sy (mouse-command pixels, viewport offset added) and on_screen: where to aim the mouse for a world point |
| ground | x, z (world metres) | inside, drawn, saved, fine, step, min_h, max_h, type, type_value |
| env | | The Environment tab in one small read: `look` (the exact look, sky included, or ""), `look_light` (the look the light matches, whatever the sky), `look_label` (the Look row's text: "Warm sun", "Warm sun, with the Starry space sky", "This map's own light"), `sky` (name), `sky_words`, `sky_night`, `light_night`, `mismatch_line` (the amber sky-time line shows), `fog{start, end, colour[r,g,b], preset ("Default" / "Light" / "Medium" / "Thick" / ""), default{start, end}, scale}`, `precip` ("Clear" / "Light rain" / "Heavy rain" / "Snow" / "Dust storm"), `lightning{on, min, max (s), colour[r,g,b]}`, `building_lights`, `sun{direction (0-359), height, body ("Sun" / "Moon")}`, `ground_fog{from, up_to}`, `sky_out_of_view` (camera pitch 0.5 or steeper), `helpers_faded` E2: `precip_draw` {sprites, rate, life, size_m, px_at_10m} (the precipitation sprites drawn this frame, the snow emitter's rate / lifespan / sprite size), `flash_now` {model (the flash added to lit models' sun term, 0 = none), sky (the sky gain, 1 = none)}; `script --lightning-hold N` holds the flash while lightning is on (1 models, 2 sky, 3 both, 0 off). E2b developer flags: `script --set-look "Warm sun"` (a look by its label), `--set-sky Env_SkySpace` (a sky by its name, like Advanced > Sky), `--set-weather none|light|heavy|snow|dust`, `--sky-mtb PATH` (the MASky2 CorporalMTB, `-` removes it); save with `--save-to FILE`. |
| window_size | w, h (client area in pixels, 320..8192) | w, h: resizes the (hidden) window, also past the screen size, so 1280x720, 1920x1080 and 3840x1600 layouts can be checked on any monitor; the swap chain follows on the next frame, so `wait_frames` 3 before a screenshot. A first launch with no settings file picks a larger UI scale for a big window (3840x1600 starts at 1.5x); a window under 1800 UI pixels wide has a two-line status bar |
| release_all | | released[]: lifts every key and mouse button the automation put down |
| screenshot | path (.png or .bmp) | path, width, height (after the next frame is drawn) |
| mouse | action move/down/up/click/double/drag/wheel, x, y, x2, y2, button left/right/middle, frames (drag), delta, dx (wheel), mods | ok, mods_seen (when `mods` was given) |
| key | key (ImGui name: A, F5, Escape, Delete, Comma, UpArrow...), mods "ctrl+shift+alt", action press/down/up, hold_frames | ok, mods_seen (when `mods` was given) |
| text | text | ok |
| wait_frames | n | ok |
| items | filter, all (include hidden), limit (default 60, max 2000) | items[]: id, label, full_label, window, visible, rect{x,y,w,h,cx,cy}, `win` (the window's own clip rectangle), `text_w` (the label's drawn width, the `##` part cut), `clipped` (a button / selectable / radio / link whose label is wider than its rectangle, or any item reaching past its window sideways, or past the bottom of a window that cannot scroll; titles and tooltips are never flagged); count (all matches), truncated |
| windows | | `windows[]` every open window (name, child, popup, scroll_max, rect) and `client` {w,h}: `tools\layout_check.py` asserts each lies inside the client area |
| find_item / hover_item / click_item | item ("Build/Enemy", "Preferences/Hotkeys", "#1234ABCD"), button, double, frames, mods (hover / click) | item |
| script | flag, arg | the editor's developer flags (--select-class MAMech, --tab 3, --place ..., --check...) |
| script `--crop-census FILE` | | developer flag: writes every wall / bridge / base template and road piece model through the save's cut test (`crop::cuttable`) as `family<TAB>name<TAB>model<TAB>ok<TAB>reason`, then `# family N: a cuttable, b refused` lines (family 1 walls, 2 bridges, 3 bases, 4 road pieces) |
| open | path | ok |
| dialogs | answer yes/no/cancel, next_file | answer, last_question (see Dialogs below) |
| quit | force | ok (force skips the unsaved-changes question) |

Item paths: the last part is the visible label (or the full `Label##id`); earlier parts must match the item's window
name or the editor tab (Map, Build, Objectives & Triggers, Cameras & Cutscenes, Environment). Exact labels first, else a unique
substring. Items come from ImGui's test-engine hooks (build flag IMGUI_ENABLE_TEST_ENGINE), from the last frame.

## Layout check
`python tools\layout_check.py <pipe> [--layouts L1,L2,L3,L4,L5] [--out FILE] [--shots DIR]` (editor started with `--automation` on that pipe): per layout (1584x861 UI 1.0, L2 1920x1080, L3 3840x1600 UI 1.5, L4 1584x861 UI 1.25, L5 1280x720) opens every tab, Build page (also the list view of the object browsers), the right-click menu, Settings (each page), New map / New mission, Help, Keyboard shortcuts, About, Check scenario, the Look list, the sky list and a 3-shot cutscene timeline, and fails on any `clipped` item that is not in its allow-list (with the reason), any item or window outside the client area, a scrolling Build page or Keyboard shortcuts sheet, or fewer than 16 key rows in Settings > Keys. `--ui-scale` (script) pins the size for a run; without it the size follows the window (Settings > General > "Fit the size to the window", on by default: 1 to 2 in quarter steps).

## Events on the timeline
- Strip: `+ Add at <t> s##tl_addev` (menu at the playhead: `Play a sound`, `Play a voice line`, `Change the music`, `Show a message`, `A unit walks a path`, `A unit patrols`, `A unit attacks the player` (these three arm `state.tool` PickPoint: click the unit on the map; walks / patrols then arm PlaceWaypoint for the route, right-click ends), `Other action...` submenu), `+##tl_endadd` ("When it ends", only when the header has room), event markers `##tl_ev<uid>` (click selects, drag retimes: 0.5 s steps, Shift free, one undo step `move event`; a diamond per event in its lane; own clips draw a bar of the clip's length). The playhead sets the time: click `##tl_shot<n>` (the shot's start) or the ruler.
- Left panel (Cameras & Cutscenes, below the shot): `When it ends` (rows = the end actions, `+ Add when it ends##endadd`), `Event` with `Delete##evdel`, `##evtime` (type + Enter, or - / +: retimes, one undo step) and the same editor an action has on the Triggers tab (`ui_action_body`); orders show `Unit`, `##evorder`, the route buttons (`Draw on map##evpath` / `Add points##evpath` / `Redraw##evpath`) and `Go round and round`. Picking an event scrolls the panel to it. A structural edit from these menus runs one frame later: `wait_frames 3` before reading.
- Where an event lives: at shot start (offset under 0.05 s) under the shot's start trigger; later under a cue timer `<cutscene> - shot k + o s` (disabled, Repeat on, root group `Cutscene timers`; its last action switches it off; the shot's start trigger switches it on). `trigger <timer uid>` shows it. Check rows: `an event at 17.0 s ... is after its end`, and `Shot n of "X" fly from a marker ...` with the fix `Play from its own view` (`check_items[].fix == 4`).

## Mission blocks and orders
- Combos (`##arrives`, `##how`, `##order`) are not listed by `items`: click them by pixel (a screenshot gives the spot); their entries (`After a delay...`, `After a delay`, `When the player reaches a place`, ...) are items while open.
- Arrives: `After a delay...` makes a timer `<unit> arrives after N s` (30 s) and the unit a spawner reinforcement; the typed box is `##arrdelay` (Properties, When it arrives / Arrives); `trigger` of the timer reads `seconds`.
- Orders chooser: `Give this unit orders...` (Select page and Properties) and `Give the selected unit orders...` (a trigger's AI orders) open `Walk a path once`, `Patrol a path (loops)`, `Defend a point`, `Follow the player`, `Attack the player`, `Attack a unit...`; only the two path kinds arm `PlaceWaypoint` (`state.tool`). With no unit selected the trigger button is `Pick a unit on the map` (`PickPoint`; the click opens the chooser). Changing the Order combo drops a waiting path / point / unit click.
- Neutral units: an order request on one opens the `Neutral unit` popup (`Player's side##neutral`, `Enemy##neutral`, `Cancel##neutral`); an existing order shows a red line with `Player's side##nfix` / `Enemy##nfix` (trigger panel, unit Starting orders); Check lists "is Neutral, so it ignores its orders".
- Objectives: `Completed by` on an objective nothing completes has a chooser (`##how`); a delay it makes is named `Objective N: after S s`, its box is `##objdelay`. Under the Win box, `Lose the mission if the time runs out` owns the trigger `Countdown` (120 s, HUD clock); its box is `##countdown`; unticking deletes it.

## Modifiers, held keys and mouse buttons

- `mods` on `mouse` (move, down, up, click, double, drag, wheel), `click_item` and `hover_item`: `"ctrl"`, `"shift"`, `"alt"` or
  joined with `+` (`"ctrl+shift"`). The modifiers go down one frame before the first event and come up after the last, so
  Shift+click, Ctrl+wheel, Ctrl+Alt+drag work in one call and nothing stays down. The reply's `mods_seen` is what the UI toolkit
  reported just before they were lifted (`"ctrl+alt"`): proof they arrived. For `mouse action=down` the modifiers are up again
  when the call returns (the button stays down): to hold a modifier across several commands use the key form below.
- `mods` on `key` works the same way (down for that one key action; `action=down` of a normal key with `mods` releases the
  modifier when the call returns, the key stays down until its `action=up`).
- Holding a modifier across several commands: `key LeftShift action=down` ... any commands ... `key LeftShift action=up`
  (also RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt, ModShift, ModCtrl, ModAlt; the names shift, ctrl, alt mean the left
  key). Down sets the modifier for both ways the app reads it (`io.KeyShift`-style and `IsKeyDown(ImGuiKey_LeftShift)`), up
  clears it (it stays set while the other side's key is still down). Do NOT use `mods=shift` together with `action=down` as a
  way to hold a modifier (old advice): it is released when the call returns.
- `state` reports `mods_down` (`["shift"]`), `keys_down` (`["LeftShift", "Q"]`) and `mouse_down` (`["right"]`). Check they are
  empty at the end of a test. `release_all` lifts everything left down (and names it in `released`).
- A command runs to its end before the next one starts (one queue), so a `state` can never be read in the middle of a `drag`:
  use `mods_seen`, or hold the modifier with `key ... action=down` and read `state` while it is held.

## Test (File > Test, the Test button, F5)

Under `--automation` Test does what it always does (Check scenario, save, build the game's command line) but never starts the
game, not even hidden. The status line says so, and `state` has `test_launch_skipped: true`, `test_command` (the full command
line) and `test_dir` (the working folder it would run in). No "Game started" note opens and `test_running` stays false. To
really play a scenario, save it and start the game with `test_command`. Normal (non-automation) runs are unchanged.

## Dialogs

- `dialogs answer=yes|no|cancel` sets the answer for Windows message boxes and also answers an in-app "Please confirm" popup
  (Delete trigger, Make the whole map playable...) that is open at that moment: send it after the click that opened the popup,
  not before (the popup answer is delivered on the next frame; `state.confirm_text` keeps the last question after it closes,
  `popup_open` tells whether it is still up). The popup's own Yes / No buttons can be clicked with `click_item` as well.
- A "Please confirm" popup is not a message box: `last_dialog` in `state` is only for message boxes.

## Keys

Every key goes through one table (`src/app/input_map.h`) and one dispatcher (`App::dispatch_keys`, `src/app/app_input.cpp`).
`state.keys_fired` counts what the dispatcher ran by action label; read it before and after a `key` command to prove a key ran
exactly one action once (the old `[` / `]` ran two handlers). Key commands use ImGui names (`LeftBracket`, `Equal`,
`KeypadAdd`, `Slash` with `mods=shift` for `?`). Contexts: Global (no map needed), Map, Edit, Selection, Placing, Brush, Glue,
Preview; actions marked pending in the table (weaker / stronger brush) have no handler yet and never fire.

## Transform keys and handles

Arrow keys with a selection nudge it 1 m (Shift 10 m) along the world axis nearest the camera; PgUp / PgDn raise / lower 0.5 m (Shift
5 m) and stop at the floor ("Stopped at the ground"); each burst is one merged undo step (`undo_label` "nudge" / "raise"). While
placing, PgUp / PgDn set `state.gizmo.place_lift` (the next piece lands that far above the click). `state.gizmo` also has `local`
(World / Local Move arrows), `turn_each` ("Turn each in place"), `readout` (the floating label while it shows: "Heading 105 deg (+15)",
"Height 10.97 m (+0.50), ...", also copied to `status`), `knob` {x, y} (screen position of the lift knob, null when not drawn) and
`pivot` {x, y} (the handles' centre). Properties items: `##heading`, and for 2+ selected `##mheading`, `##mpx`, `##mpy`, `##mpz`.

## Flat pieces

Roads and thin foundation plates are "sheets": each has a layer (`objects` / `sel` rows carry `layer`, `"<n>"` or `"<n> fixed"`,
and `y3`, the height to the millimetre). `state.preview` has `x`, `z` (the placing spot under the mouse) and `y` (the origin
height the ghost is drawn at; `null` when no ghost): it equals the placed object's `y` to 0.001. `--road-tool NAME` (script) arms
a road piece like picking it in the palette (`--road-tool longstraight`); with `--place-tool` it lets a check place by real
clicks, so the click path (ghost, layer, stacking) is what runs. Hold Shift (`key LeftShift action=down`) while reading the
ghost of a Shift-placed piece: without it the ghost is the snapped one.

## What stands on what

Every object stands on the top surface under the middle of its model's base: the highest flat piece (road or plate) there, else the
ground. `sel` / `objects` rows carry `gap` (the foot minus that surface, metres: 0 = resting on it) and `on` (the uid of the flat piece
it stands on, 0 = the ground). The `state` `selection[]` rows add `ground`, `surface` and `foot` (the lowest point of its model). A
flat piece's rows carry `fixed`, `float_gap` (how far the lowest ground is below it) and `above_ground` (its underside over the
highest ground under it) instead. `--dump-near` prints ` ground G gap G layer N` after each object.
Roads always lie above the plates they overlap, whatever order they were put down in ("last placed on top" applies within one kind);
Bring forward / Send back still override. Placing: the object under the cursor (a roof, a rock, a tree) holds the thing being placed only
while Alt is held (`key LeftAlt action=down`, then read `state.status_hint`: "on top of <name>").
The lift handle (the green arrow) stops where the foot meets the surface; hold Shift to push it below (the readout and status line go
amber). The `h` box does the same when dragged; a typed number is taken as is. G says what it did in the status line.

## Ground tools carry what stands on them

The sculpt brushes, Fit ground to road and the height range take what stands on the ground (and on flat pieces) before they change it and
move it by the change of the surface under each object afterwards, once, in the same undo step. A unit on a pad moves with the pad and
keeps its own gap, so `gap` of every `objects` row is unchanged by a stroke. Fit ground never changes the ground under another building
or flat piece (the status line names them); hovering the button draws the plan and `Fit ground to road` shows a forecast line. The tick
box is "Objects follow the ground": off = only the ground moves and the Fit message says so.

## Slopes

A road piece or base placed where the ground slopes under it (`float_gap` over the tolerance, no other flat piece under it) is levelled
into the ground, tilted onto it, or left floating. Roads default to Tilt, bases to Level; the choice is remembered per kind and L
(Settings > Keys, "On a slope: level, tilt or float") cycles it while placing; the placing options show an "On a slope:" row with three
radio buttons (`items` filter `Level the ground`, `Tilt`, `Float`). `state.preview.slope` is the plan under the mouse (`null` on flat
ground): `mode` (what will happen), `wanted` (what was chosen: Tilt falls back to Float when a piece cannot tilt, `fell_back`), `gap`
(`float_gap` of the level piece), `left` (what still floats after a tilt), `pitch_deg`, `roll_deg`, `up` / `down` (Level: how far the
ground rises and falls), `kept` (buildings and flat pieces whose ground it leaves alone); `state.status_hint` carries the sentence. The
click gives exactly the plan (`preview.y` is the origin height). The status line names the change. A selected flat piece that floats
shows the gap line and the buttons "Level the ground under it" and "Tilt it to the slope" in Properties (`items` filter `Tilt it`).
`sel` / `objects` `pitch` and `roll` are degrees for every kind of object (road pieces read RoadRotation), and `above_ground` of a tilted
piece is its lowest clearance over the ground (0.08 after a tilt or a Level).

## Water basins and the placement checks

A water placed with "Dig a basin" (or palette water) lies 0.15 m under the ground you clicked (`objects` `y3`). The bowl is a ground edit like Fit
ground: it never changes the ground under another building, flat piece, unit, turret or start point (the status line says "The basin stops short
of ..."); before the click the ghost outlines the cells it will change and the objects it keeps (yellow / white), and a line under the options says
it. The water remembers its basin (saved in `editor/project.json` "basins"): when its size, shape, level or place changes (once the mouse is up
and no field is being edited), or it is deleted, the old bowl is filled and a new one is dug in the same undo step. A water placed without the
tick box has no basin and its ground is never touched. `Dig a basin under it` in Properties does the same on a selected water and previews on hover.
`state.check_items[]` rows now carry `uid`, `uids[]` (all objects of the warning) and `fix` (0 none, 1 lift to the ground, 2 put the newer flat piece
on top, 3 put on the ground). The Check scenario window lists, as warnings: flat pieces that hang in the air, flat pieces that overlap at one height,
objects under the ground, objects deep in it (units, pickups, vehicle pads), objects that hang with nothing under them. `Show` / `Show next` selects
the next object of a warning and puts the camera target on it (dist 30..250, pitch 0.45). Retail levels (read-only) are not checked.

## Sculpting by the stroke

Raise, Lower and Roughen move the ground by at most `brush.amount` metres (`Height per stroke`, default 2.0, saved as `[Brush] RaiseAmount=`) x the brush weight, measured from the ground at the stroke's start: holding still or dragging back and forth adds nothing, a click is a full dab, drag speed and frame rate do not matter. `brush.build_up` = the "Keep raising while held" tick (stroke cap grows by one amount per second held). Shift+wheel / Shift+`[ ]` change `brush.amount` by one ground step while Raise, Lower or Roughen is armed (Smooth / Flatten keep `brush.strength` = Speed). `state.brush` also has `min_radius` (the smallest brush for the armed tool on this map: 1.5 cells, 12 m on 8 m cells; typing a smaller size into the Brush size slider grows it and the status says why) and `readout` (the text drawn at the cursor, two lines joined by a newline: `40 m  +2.0 m per stroke` / `ground 12.50 m  slope 18%`; the first line reads `ground 14.50 m (+2.00)` during a Raise / Lower stroke). Flatten: Alt+click or Ctrl+click only picks the height (no stroke, no undo step). Shift+click on a sculpt or paint brush draws a straight stroke from where that tool's last stroke ended (one undo step). The sculpt and paint tool buttons stay lit when clicked again; Esc or a right-click on the map puts the tool down. Items: `##amount` (Height per stroke), `Keep raising while held`, `##edge` (sculpt pages too), `##speed` only for Smooth / Flatten and the paint tools.

## Ramp and steep ground

`state.tool` can be `Ramp`. Two map clicks make a ramp: click 1 sets the start A, click 2 the end B and applies it (one undo step, "Ramp"; objects follow the ground). Esc or a right-click after click 1 drops A; the tool stays armed. `state.ramp` = `{stage (0 waiting for A, 1 A set), ax, az, ha, hb (the heights it runs between, as the preview shows them), width, slope (rise over run), length, label (the text on the map: "Ramp 55 m long, rises 20.0 m: 36%. 'Mechs and bots can climb it." / "... Bots' 'Mechs won't WALK up this (jump-jet 'Mechs may jet up a short face), over 55%; Battle Armor and pilots walk up to 80%." / "... Very steep (over 80%): bots won't route here; some 'Mechs may still climb it." / "Too short for a ramp (at least 16 m)." while the preview is under the minimum length), typed}`. Slope tiers in `core/brush.h` (`steep_tier`): `kSteepBot` 0.55 (above it the bot planner will not route a 'Mech or a tank: `TG_STEP_HEAVY` 1.1 m per 2 m cell, `ma2_bot_nav.c:142`, tanks `slope_max 55` `ma2_bot_ctrl.c:2337`; Battle Armor and pilots plan on the light grid up to `kSteepLight` 0.80, `ma2_bot_nav.c:137`; hover: not measured), and the orange tier starts at `kSteepLight` (above it no bot body routes). Nothing in the tiers says what the player can climb (the player's Atlas walked up 169% and 825% faces in the reach rerun; `steep_tier` returns 0 / 1 / 2, no wall tier). The map overlay, the ramp strip and the cursor readout (`slope 80%: bots' 'Mechs won't walk up this (lighter bodies: up to 80%)`) follow the tiers: green all, yellow bots' 'Mechs won't walk up it, orange very steep (bots won't route here; some 'Mechs may still climb it). The width is `[Brush] RampWidth` (`[ ]` change it while Ramp is armed; minimum two ground cells). `state.overlay` = `{steep, typemap, plants}`: `steep` is true while the yellow / orange steep-ground overlay is drawn (View > Steep ground, the Sculpt page box "Show steep ground", or automatically while Ramp is armed). The lean `ground` read has `slope` (rise over run of the ground triangle under the point).

## Painting by the stroke

Texture and Colour tint work by the stroke like the sculpt brushes: each texel goes from what it was when the stroke began toward the target by `Strength` x the biggest brush weight it has felt, so holding still adds nothing and a click is a visible dab (100% paints it fully at the centre of the brush). `state.brush.strength` mirrors the current tool's strength; `state.brush.strengths` = `{texture, tint, smooth, flatten, plants}` is what each tool remembers (Texture 100%, the others 50%; saved as `[Brush] StrengthTexture / StrengthTint / StrengthSmooth / StrengthFlatten / StrengthPlants`). `state.brush.reverse` is true while the reverse key (Ctrl by default) is held, or for a stroke that began with it: Texture then paints the other texture (a map with more than two: rubs the chosen texture out, its weight going to the others in proportion), Boundaries paints Playable (type 0), Plants clears (or grows, when "Clear" is latched). `ground` also returns `color` (the colour-map texel under the point as `[r, g, b, a]`) and, on a map with ground layers, `weights` (the eight layer weights there). `state.perf` = `{frame_ms (mean of the last 30 frames), brush_ms (mean over those frames of the time the brush spent applying a stroke and uploading what it touched), full_upload, stroke {frames, frame_ms, brush_ms}: the same two means over the frames of the last stroke}`. Strokes upload only the rectangle they touched; `script --full-upload 1` switches back to the old whole-texture upload to compare (developer, session only).

## The scatter brush

A stroke earns objects by the ground it covers: every 2 x 2 m cell newly inside the ring adds `density` per hectare of its area (default 40 per hectare; a click with a 25 m ring places about 8), so drag speed, frame rate and holding still change nothing, and going over covered ground adds nothing. A spot is skipped (up to 8 random tries per object) when it is closer than `spacing x (r_a + r_b)` to another object (r = half the longer side of the model box, at least 0.5 m; 2.5 m while the model loads), when it lies on a road piece, a base or flat plate, a building or a water rectangle ("Keep off", on by default; the object's rim is tested too), or on ground steeper than `slope_limit` per cent ("Skip slopes over", off by default). Random facing (on), `Lean up to` (0 to 20 deg, objects with a `CorporalPitch` only) and `Follow the slope` (tilt to `terrain.normal_at`, plus the lean) pose each object; its height is the surface under it (the ground with Keep off on) plus the lowest point of the turned model. No size variation (objects have no scale). Ctrl held at the press = an erase stroke: removes every root object whose template is in the brush set and whose origin lies inside the ring (nothing else); Shift+click draws a straight stroke from where the last Scatter stroke ended. One undo step per stroke (`scatter` / `scatter erase`; none when nothing changed); the selection is never touched. Status after a stroke: `Scattered 23 objects (3 kinds). Skipped 9 spots: 7 too close to others, 2 on a road, base, building or water. Ctrl+Z removes them.` (+ the graphics-memory line when a kind comes from another level), erase: `Removed 12 objects (3 kinds). Ctrl+Z puts them back.` Settings: `[Scatter] Density, Spacing, RandomFacing, Lean, FollowSlope, KeepOff, SkipSteep, SlopeLimit`.

`state.scatter` = `{density, spacing, facing, lean, follow, keep_off, skip_steep, slope_limit, radii {template uid: footprint radius, 0 until its model has loaded}, active, so_far, erase, placed, erased, kinds, skipped_close, skipped_keepoff, skipped_slope}` (the last six describe the last finished stroke; `so_far` is the count of the stroke in progress, -1 otherwise). `state.brush.readout` on Scatter reads `40 m  about 20` (`placed N` / `removed N` during a stroke, `remove the set's kinds` with Ctrl held). Items: `##scdensity`, `##scspacing`, `##sclean`, `##scslope`, `Random facing##sc`, `Follow the slope##sc`, `Keep off roads, bases and buildings##sc`, `Skip slopes over##sc`.

## Token cost

`dump` is about 1,700 tokens, `state` about 1,200, and an unfiltered `items` with `all` can be thousands of rows: use
`items` with a `filter` and a `limit`, `sel`, `objects` and `ground` instead. `items` now returns at most `limit` (default 60)
rows and sets `truncated` when more matched. `ground_drawn` / `drawn` is the ground the editor uses and the view draws (the saved 8-bit steps, cells split on the game's diagonal: `Terrain::height_at`);
`ground_saved` / `saved` is the same point worked out from the bytes the file will hold. They are equal (a difference is a bug). `fine` (the `ground` read) is the
sculpt brushes' private sub-step layer, for debugging strokes only; `step` is one stored height step (0.50 m on a new map, 0.47 m on a -20..100 m map).

## Clients

- `tools/edctl.py` (CLI; `python edctl.py --help`): `launch`, `state`, `shot out.png`, `click "Build/Enemy"`,
  `drag x1 y1 x2 y2 --button right`, `key Comma --mods ctrl`, `items Hotkeys`, `script --select-class MAMech`, `quit --force`.
  Mouse commands take `--mods ctrl+shift`; `sel`, `objects [FILTER] --near X Z R --limit N`, `ground X Z`, `release_all`.
  Env `MA2SE_PIPE` picks the pipe.

## Snapping

One solver joins a road piece, wall, bridge piece or foundation to the open end / edge of a placed one of its family. The joint is
chosen by the ghost's OWN end (free ghost: footprint centre under the mouse, the user's heading) coming within 16 px (setting, scaled
by dpi) AND within a cap of 25 % of the piece's long side (2 to 12 m) of the target end; heading may differ by up to 50 deg; nearest wins,
no bonus for the newest piece. Hold Shift (`mods` on the mouse command or `key LeftShift action=down`) for no joints at all.
`state.snap`: `on`, and when on: `kind` (End, Edge ...), `target_uid`, `target`, `socket`, `own` (index of the piece's own end), `d_px`,
`d_m`, `yaw_deg` (the joined heading); always `reach_px`, `cap_m`, `free` (Shift is down), `joints` (the setting), `grid`, `grid_m`,
`candidates`, `label` ("Joins X"). `state.preview.heading_deg` is the ghost's heading (the joined one when `snap.on`); `preview.x/z` the
footprint centre of the ghost. `state.status_hint` names the joint while joined. Q / E while joined step through the other ways to join
at that joint (a straight road flips ends, a wall turns its corner), free they turn 15 deg; Ctrl+Q / E always turn 90 deg. After a click
the user's heading becomes the placed heading. `--place-tool "road:<name>"` (script) arms a road piece by name (`road:Long straight`).
`--place-road name@snap` places a piece joined to the last dev-placed one with the solver (no camera: 8 px per metre).
Walls whose model box is under 0.5 m long (e.g. "Terra barricade light") have no ends and never join (as before).

### Grid, angle steps and the snap bar
`state.snap` also has `angle` / `angle_deg` (steps) and `grid_label` ("Grid 8 m" while the grid rounded the ghost, empty when a joint won or
it is off). Grid: off by default; sizes 1, 2, 5, 8, 10, 20 m; it rounds `preview.x/z` (the footprint centre) to multiples of `grid_m`.
Keys: Ctrl+G `GridToggle`; `[` / `]` `GridSmaller` / `GridBigger` while placing or with a selection (a brush tool keeps them for the brush);
`SnapJoints` / `AngleSnap` unbound. The snap bar is a window `##snapbar` at the viewport's top-left (items `Joints##snapj`, `Grid##snapg`,
combo `##snapgs`, `Angle##snapa`, combo `##snapas`; a combo's entries are in the `##Combo_00` window); the placing panel has the same items
(window `##left`) and a `##placefacing` DragFloat (degrees; double-click types). Angle steps (15 deg, absolute from north, on by default) apply to
a new palette pick's heading, drag-aim, Q / E and the turn ring (the ring lands on multiples of the step, whatever the start heading); a
typed Facing or inspector Heading is taken as typed. Hold Shift: no joints, no grid, no angle steps. The settings are saved per user in
`[Snap]` of the settings file (`Joints`, `Grid`, `GridSize`, `Angle`, `AngleStep`, `ReachPx`); automation runs share one default file unless
`--settings <file>` names another.

### Across families
Besides the same-family joints, a piece snaps to a plate or building when its own point is within reach (same 16 px / cap rule):
a building (palette class MABuilding) on a flat plate: footprint centre, flush inside each edge, flush inside each corner (`kind` Centre /
Inside, `socket` 0 centre, 1-4 edge +x -x +z -z, 5-8 corner ++ +- -+ --, in the plate's own frame; heading = the plate's plus a quarter turn);
a straight road beside a plate edge (kind Beside: kerb on the edge line, slides along it; pulled by the free centre's distance to the edge
line) or with an end on it (EndOnEdge, plus the edge middle as `Edge`; a road end flush with a plate corner is kind Corner); a wall's end
at a building's or plate's corner (kind Corner: along either edge, over the face or on along the edge line). `state.snap` also has `anchor`
(the winner is cross-family) and `tx`, `tz` (the target point). Labels: "On X: centre" / ": north edge" / ": NE corner", "Beside X",
"Ends at X", "At the corner of X". Plates also join one run when their edges touch along 2 m or more (flush with a corner), not only
middle to middle; the joint candidates for plates of different sizes are middle and corner-flush.

### Moving snaps like placing
A body drag of the selection (press on a selected object, past 6 px) and the gizmo's plane square solve the pressed object (the first selected
object for the square) the same way a placed ghost is solved: the free target is the ground under the cursor plus the grab offset (the
anchor's footprint centre minus the ground under the press), the anchor's heading is H. One unglued object gets every joint (it may turn to
the joint's heading; a wall or bridge joined end to end takes its neighbour's height; a road joined to a tilted road takes the joint height and
is marked fixed). Several objects or a glued group slide only (candidates within 1 degree of the anchor's heading) and the grid goes on the
anchor; the rest keep their offset. The Shift (place-freely) key turns joints and grid off for the drag. The axis arrows use the grid along
their own axis only. Nudge arrows with the grid on (no Shift) move one cell and land on the lattice of the first selected object's footprint
centre; Shift stays 10 m free. The single-object paste ghost joins like a placed piece (several objects: the grid on the group centre).
`state.snap` adds `moving` (a move drag is solving), `moving_fam` (snap family of the anchor, 0 = grid only) and `moving_single`; `on`, `kind`,
`label` and `grid_label` describe the joint or grid while it lasts and `status_hint` reads "Moving: Joins X - release puts it where it joins".

### The Build tab, the tool strip and the pages
- The tab row is Map | Build | Objectives & Triggers | Cameras & Cutscenes | Environment (Ctrl+1 to Ctrl+5). "Terrain" and "Units and
  Props" are pages of Build. A strip of eight buttons (`Select##strip`, `Place##strip`, `Scatter##strip`, `Roads & walls##strip`,
  `Sculpt##strip`, `Paint##strip`, `Water##strip`, `Starts##strip`; window `##strip`) is at the far left whenever a map is open, on every
  tab; Shift+1 to Shift+8 press them. Opening Sculpt arms its last tool (Raise first), Paint its last (Texture), Scatter its brush when the
  brush set is not empty; the others arm nothing. Esc stops the tool and keeps the page; picking another page stops the armed tool.
  A read-only level enables Select only.
- `state.tab` is "Build" for all of it; `state.page` is the page ("Place", "Roads & walls"...); `state.panels` = {strip, left, right,
  viewport: {x,y,w,h} in window pixels, left_scroll_max}: the left panel never scrolls on a Build page (`left_scroll_max` 0), only the
  browser or list in it does. `state.scatter_set` = the uids of the Scatter brush set.
- Old paths keep working: `click_item "Units and Props/Enemy"`, `Terrain/...`, `Players/...` (the scope names and the page names all
  mean Build); `script --tab 1` (Terrain) opens Sculpt, `--tab 2` Starts, `--tab 3` Place.
- Palette cells carry ids the `items` filter can find: `##cell_<template uid>` (both views), `##road_<file stem>`, `##tick_<uid>` (Scatter
  only: the box on a picture's corner, or in the first column of the list; a click on the cell does the same), `##set_<uid>` (a brush
  set chip: a click takes it out), `##chip` (the group buttons), `##palfind` (the search box; Ctrl+F lands here), `##newtype` (water tiles).
  `Brush##scatter` is the Scatter brush button, `##scdensity` (objects per hectare) and `##brushsc` (size) its sliders; the other scatter rows are `##scspacing`, `Random facing##sc`, `##sclean`, `Follow the slope##sc`, `Keep off roads, bases and buildings##sc`, `Skip slopes over##sc` + `##scslope`.
- The panel width: drag the splitter on its right edge (300 to 720 px at UI 1.0; saved as `[Layout] LeftPanel=`), double-click for the
  default (370). The map view keeps 45 % of the window: a wide panel on a small window is shrunk, so `panels.left.w` is smaller than set.
- `window` is another name of `window_size`. `script --ui-scale X` sets the UI scale for this run only (never saved).
- `tools\pickers_check.py <pipe> [--out file] [--layouts L1,L2,L3,L4]` checks, for four layouts, every page, sub-tool and view, that the
  required pickers (src\app\page_map.h, tools\pickers.json) are visible inside the left panel and the panel does not scroll.

### Plants from other levels
- Paint > Plants: `+ Add a plant##plants` opens the modal `Add a plant##plantpick` (every MACultureType of the 22 retail worlds that have a culture system, grouped by level). Rows: `Add to this map##add` (greyed when the map already has that plant), `Scatter it as objects##sco` (adds the plant's template to the scatter brush and opens Scatter); the page's own plant rows also have `Scatter it as objects##sco`. Click rows by id (`#XXXXXXXX` from `items`): the label repeats on every row. `Close` closes it.
- `script --dump-culture <file>` lists the map's culture types (shift, bits, template, share of the map) and the `culturenBitfields` count (key 2E9F44D9 in the `info` lines). Adding a plant: one undo step `add plant`; the new layer starts empty.

### The palette browser
One function draws Place, Roads & walls and Scatter: search box `##palfind` (a plain click on its rectangle, `click_item` does not take `##` ids that have no label), `Filter##palfilter` (popup: `Pictures##view` / `List##view`, `Small` / `Medium` / `Large` `##picsize`, `##narrowto`; saved as `[Palette] View=` / `PicSize=`), chips `<Name>##chip` (the cut chip is `Not in the game (N)###cutchip`). Cell ids: `##cell_<uid>`, `##road_<file stem>`, `##water_<uid>`; Scatter mode adds `##tick_<uid>` (both views); the brush set row has one `##set_<uid>` thumbnail per kind (a click takes it out), `Clear##brushset`, `+ Trees of this theme##brushset`. Right-click a cell: `Place it`, `Add to the scatter brush` / `Remove from the scatter brush` (from the Place page it only adds: the brush is not armed and the page stays), `Select all of these on the map (N)`, `Copy the game name`. A cell dragged onto the map (`mouse` `drag` from the cell's centre, `frames` 6 or more) arms its object, shows the ghost under the mouse and places one at the release point; the tool stays armed. `state.left_panel_ms` / `state.frame_ms` are smoothed per-frame costs (the left panel alone, the whole frame).

`script --dump-palette FILE` writes uid, official name, `shown` (the name the palette shows, without the official name in brackets), category, group, cut for every placeable template; no `shown` holds `_`.

The graphics memory line is the item `##gfxmem` (label "Graphics memory: about N of 64 MB") in the footer of Place, Scatter, Roads & walls and on the Map tab. The New map dialog is a fixed 600 px frame (its body scrolls); the six theme pictures are `##themepic_0..5`; theme rows read "<look> - N buildings, N bases, N trees (<level>)". Check scenario ends with an info row "Graphics memory: about N of the game's 64 MB ...".

## Listening and reinforcement orders
- `state.audition` = `{playing, clip, ms}`: `clip` is the own clip last played (-1 after Stop or with none), `playing` is true for `ms` after the click. Under `--automation` nothing is heard; the state is kept the same way. Buttons: `Play##aud<clip>` / `Stop##aud<clip>` (the clip list: id suffix `clip<n>`), the picker rows (`pick<n>`), and beside the picker of a sound action (`cue`); a game cue's `Play##audcue` is disabled (tooltip: the game's own sounds play only in the game for now).
- Cue pickers: filter box on top (matches words, mission and the old trigger line), own clips first, then one tree node per mission (open while a filter is typed); a game cue that shows a subtitle reads `"words" - Mission 05` (tooltip: the old line). `core/triggers.h` `CueInfo {id, label, mission, words, detail}`.
- A reinforcement (a unit inside a spawner a trigger switches on) given orders starts them on its **arrival trigger**; `trigger` of that trigger lists the order in `orders[]`. Changing Arrives moves the orders with it (At the start gives them a new "Orders: <unit>" start timer).

## Cutscene replay and shot picking
- `state.later_hidden`: objects left off the map view because the mission shows them later (View > Objects the mission shows later off), and, while a cutscene preview runs, the reinforcements that have not arrived when that cutscene starts (the earliest timer that plays it); objects only used in cutscenes are shown in the preview.
- A plain click on a still shot's eye point (camera glyph "3") selects both of its eye points (2 cm apart), so PgUp / moves keep the shot still; the shot's camera object ranks behind an eye point within 4 m on the ray (Alt+click reaches it).

## Environment tab
One Look section (Day / Evening / Night, the Look list, the source line, the amber sky-time line, Building lights), then Weather (Clear, Light rain,
Heavy rain, Snow, Dust storm, Lightning with its timing and colour), Fog (Default, Light, Medium, Thick as `Default##fog` ... `Thick##fog`, the
pair, the colour swatch), Sun (Moon on a night light) with Direction 0-359 and Height, Update shadows, and Advanced (Sky picture, Use this sky's
fog colour, Fog distance, Fog colour, Ground fog from / up to). `env` reads all of it. Every control writes `state.status` (8 s); a look or sky
change adds " Graphics memory: about 25.6 of 64 MB (+1.2)." when the exact figure moved by 0.1 MB or more. A look sets the lightning its retail
level has (on with its timings and colour, or off); changing the weather never touches it. The tab never moves the camera: from a steep view
(`env.sky_out_of_view`) a dim line and `Show the sky` tilt it on request. The Look and Sky lists open downward within the window. The colour
swatches have no item of their own: click them by pixel, then type a hex value into the picker's `##Text` item. The lightning range items are
`##min` / `##max`; the sun sliders are `##sundir` / `##sunh` (Ctrl+click to type). H also hides the sun arrow, the water outlines and the
water's blue handle (a selected water keeps them); start slabs fade to 35 % while the camera is within 20 m (`state.helpers_faded`).

## Help

`state.help` = `{topic, filter, shown[], topics}`: the open topic, the "Find in Help" text, the topic titles the filter leaves, and the topic count. The filter box is `Help/##helpfind` (click it, then `text`). `script --dump-help FILE` writes the whole Help text with the current key bindings (one `== Title` block per topic), for checking a rebound key. The text comes from `src/app/help_text.h`; `out/fmt_test.exe` writes `out/Scenario editor.txt` from it and fails when `docs/Scenario editor.txt` differs.
