# Editor automation (background control without the desktop)

`MA2 Scenario Editor.exe --automation [PIPE] [--automation-show] [--settings FILE] [file.ma2map]`

- Developer flag; normal launches never open the pipe.
- Which exe the tools start: `edctl.py launch` starts `out\MA2 Scenario Editor.exe` (env `MA2SE_EDITOR`
  overrides), whatever build that file is. It can be old: run plain `build.cmd` first and compare the hash. Publish only an exe built under that final name (no `MA2SE_EXE=...` override).
- The window stays hidden (`--automation-show` shows it without taking focus) and keeps rendering when covered
  or minimized. Client area 1584x861 (window 1600x900).
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
| state | | tab, tool, camera, windows{help,getting_started,new,about,check,retail,preferences,message}, popup_open, message_title/text, confirm_text, status, last_dialog, hotkey_capture, edge_scrolling, right_dragged, test_running, preview{on,valid,reason,heading_deg}, gizmo{mode 0 move/1 rotate/2 tilt, hot, drag handle ids}, undo, redo, id_conflicts, check_items[], doc{name,file,dirty,multiplayer,read_only,objects,triggers,graphics_mb_last_save}, selection[], selection_count |
| dump | | text: the developer state dump (--play dump) |
| screenshot | path (.png or .bmp) | path, width, height (after the next frame is drawn) |
| mouse | action move/down/up/click/double/drag/wheel, x, y, x2, y2, button left/right/middle, frames (drag), delta, dx (wheel) | ok |
| key | key (ImGui name: A, F5, Escape, Delete, Comma, UpArrow...), mods "ctrl+shift+alt", action press/down/up, hold_frames | ok |
| text | text | ok |
| wait_frames | n | ok |
| items | filter, all (include hidden) | items[]: id, label, full_label, window, visible, rect{x,y,w,h,cx,cy} |
| find_item / hover_item / click_item | item ("Units and Props/Enemy", "Preferences/Hotkeys", "#1234ABCD"), button, double, frames | item |
| script | flag, arg | the editor's developer flags (--select-class MAMech, --tab 3, --place ..., --check...) |
| open | path | ok |
| dialogs | answer yes/no/cancel, next_file | answer, last_question |
| quit | force | ok (force skips the unsaved-changes question) |

Item paths: the last part is the visible label (or the full `Label##id`); earlier parts must match the item's window
name or the editor tab (Map, Terrain, Units and Props, Objectives & Triggers, Cameras & Cutscenes, Environment). Exact labels first, else a unique
substring. Items come from ImGui's test-engine hooks (build flag IMGUI_ENABLE_TEST_ENGINE), from the last frame.

## Clients

- `tools/edctl.py` (CLI; `python edctl.py --help`): `launch`, `state`, `shot out.png`, `click "Units and Props/Enemy"`,
  `drag x1 y1 x2 y2 --button right`, `key Comma --mods ctrl`, `items Hotkeys`, `script --select-class MAMech`, `quit --force`.
  Env `MA2SE_PIPE` picks the pipe.
