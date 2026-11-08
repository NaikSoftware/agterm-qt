# agtermctl reference (agterm-qt)

Every command `agtermctl --help` lists, with its arguments, the errors worth handling and where to read the
result back. Errors are quoted as the app returns them; the CLI prints them as `agtermctl: <error>` and exits 1.

## Connection and output

- Socket: `--socket PATH`, else `$AGTERM_STATE_DIR/agterm.sock`, else `~/.local/share/agterm-qt/agterm.sock`.
  `AGTERM_SOCKET` is exported to panes but never read by the CLI; pass it with `--socket` to be exact.
  Inside a pane, run `"$AGTERMCTL"`: the app's own binary, immune to a stale `agtermctl` earlier on `PATH`.
- Wire format: one JSON request line `{"cmd":"session.new","target":"…","args":{…}}`, one response line
  `{"ok":true,"result":{…}}` or `{"ok":false,"error":"…"}`. Request size is capped at 4 MiB.
- `--json` prints the response line verbatim and exits 0 even on `"ok":false`; check `.ok`. On `pick`, `ask`
  and `session overlay open --block` it returns the open reply without waiting; `events --json`
  prints one event per line instead.
- Without `--json`: errors go to stderr with exit 1; `cannot connect to <path>: … (is agterm-qt running?)` when no
  app owns the socket. Create commands print the new id; batch close/move print `N session(s)`; reads print text.
- Options take a value unless they are flags; flags are `--json --stdin --all --no-select --create-workspace
  --auto-reset --blink --undoable --companion --wait --block --follow --full --markdown --sticky --no-frame
  --spinner --none --clear --mru --auto-size --close --minimized --next --prev --allow-custom --no-block
  --multiple --ack`. An option missing its value fails with `--<name> needs a value`.
- `--help` prints usage and touches no socket.

## Addressing

- Session and workspace targets (`--target`): full id (case-insensitive), unique id prefix, exact name, or `active`.
  Errors: `no active session`, `no workspaces`, not found, or ambiguous with the candidates listed.
- Default target: `session.*`, `font.*`, `notify` and `keymap run` use `$AGTERM_SESSION_ID` when set, else `active`.
  Workspace commands default to the active session's workspace. `ask` defaults to `active`.
- `--window W` (id, prefix, `active`) routes session, workspace and tree commands to another window's tree;
  without it the frontmost window answers, except that a session id is looked up across windows.
- Pane selectors: `left` = `primary` = `top` = `main`; `right` = `split` = `bottom`.

## tree

`agtermctl tree [--window W] [--json]`. Human output: one line per workspace, then `* id name status cwd` per
session (`*` marks the selected one). `--json` returns `result.tree`:

Top level:
- `workspaces`: array of workspace nodes.
- `workspaceFilter` (bool), `sidebarMode` (`tree`|`flagged`), `sidebarFlaggedLayout` (`flat`|`plain`|`tree`).
- `sidebarVisible`, `sidebarWidth` (points), `filePanel` (`{visible, width}`), `idleMs` (ms since the last
  user input in the window), `autoFollowMs` (omitted when auto-follow is disabled).
- `zoomedSurface` (`surface:<id>:left|right`), `pickPending` (picker id), `askPending` (GUI ask id): omitted when none.
- `dashboardMembers` (`<id>:left|right` in grid order), `dashboardHighlighted`, `dashboardFontMode`
  (`auto`|`fixed`|`untouched`), `dashboardFontSize`: present only while a dashboard is open.

Workspace node: `id`, `name`, `rememberedFolder` (the last directory its panes reported; new sessions start
there), `active`, `sessions`, plus `focused` (marked for the filter), `collapsed`, `tint`/`tintOn` when set.

Session node:
- `id`, `name` (shown name), `customName` (true when renamed), `cwd`, `title` (program-set title), `active`,
  `agent` (the agent detected in the foreground: `claude`, `codex`, `opencode`, `agy`, or empty).
- `status` (`idle`|`active`|`completed`|`blocked`), `blink`, `flagged`, `unseen` (badge count).
- `parent` (companion's parent id, empty for a top-level session), `collapsed` (parent with folded companions),
  `context`, `tint`/`tintOn`.
- `hasSplit` (a split pane exists, shown or hidden), `split` (shown), `splitAxis` (`vertical`|`horizontal`),
  `splitRatio` (0.05–0.95), `splitFocused` (true = right pane; only with a split).
- `fontSize`, `splitFontSize`: points, 0 when untouched.
- `restoreCommand`/`splitRestoreCommand`: a pinned restore command (`""` = pinned to a plain shell); omitted = auto.
- `overlay` (a session-wide program overlay is open), `overlaySizePercent`, `overlayWidth`/`overlayHeight`
  (floating only), `paneOverlays` (`["left"]`, `["right"]`, both), `hiddenOverlays` (`session`, `left`, `right`).
- `hud`: `{message, detail?, spinner, position, sizePercent?, pane?, hideAfter, markdown, fontSize?, sticky, frame,
  backgroundColor?, textColor?}`; `spinner` is the style name, `none` for a static panel.
- `ask`: `{id, pane?}` for a pending terminal-style ask.

Not in the tree: pane ids, live process details (use `session info`), per-pane status.

## version

`agtermctl version` prints `agterm-qt <version>` (`result.text`). App-wide, needs no window.

## events

`agtermctl events [--kind K]… [--run ID --after SEQ] [--limit N] [--json]`

- Follows events until interrupted. Without a cursor it starts at the current tail; nothing older is replayed.
- `--kind` repeats or takes a comma list. Kinds: `status`, `notify`, `session.created`, `session.closed`,
  `session.selected`, `tree.changed`, `pane.split`, `app.launched`, `message`; `pane.scratch`, `pane.focused`,
  `remote.opened`, `remote.closed` are accepted and never fire.
- Event: `{seq, ts (epoch seconds), kind, window?, workspace?, session?, payload}`. Payloads:
  - `status`: `name`, `status`, `blink`, `previous`.
  - `notify`: `name`, `title`, `body`.
  - `session.created`: `name`, `parent` for a companion. `session.closed`: `name`.
  - `session.selected`: `name`, `previous` (the session that lost the selection).
  - `pane.split`: `name`, `status` (`shown`|`hidden`).
  - `message`: `name`, `paneID` (recipient), `messageID`, `from` (sender identity, absent when anonymous).
  - `tree.changed`: coalesced over 100 ms after any structural change.
- Human output: `HH:MM:SS kind name …`; `--json` prints one event object per line.
- Cursor: the raw `events.read` reply is `result.events = {run, next, items}`; `--run` (that run id) and `--after`
  (a seq) go together and resume after that seq. The CLI advances its own cursor and, once holding one, retries a
  refused connection for about 30 s before exiting.
- `--limit` 1–1000 (default 100) is the page size per poll.
- Errors: `events.read requires --run and --after together`, `invalid event run id`, `invalid event cursor`,
  `event limit must be between 1 and 1000`, `invalid event kind: K`, `event run changed` (the app restarted),
  `event cursor is ahead of the current sequence`, `event cursor expired` (older than the 4096-event ring).

## workspace

All take `--target` (workspace id, prefix, name, `active`) and `--window`.

- `workspace new [--name N] [--tint COLOR [--tint-on icon,name,background]]`: prints the new id. The name comes only from
  `--name`. `--tint-on` without `--tint` fails with `--tint-on requires --tint`.
- `workspace rename <name>`: `name is required` when empty.
- `workspace delete`: removes the workspace and its sessions.
- `workspace select`: selects its first session; `workspace has no sessions` otherwise.
- `workspace go --to next|prev`: steps through the visible workspaces (wrapping) and selects the first session of
  the one it lands on; no `--target`. Errors: `workspace.go requires --to next|prev`,
  `no other workspace to navigate to` (fewer than two visible, or in the flagged view).
- `workspace move --index N`: reorders it to position N (0-based).
- `workspace collapse | expand`: folds one workspace row; read back as the node's `collapsed`.
- `workspace flag [on|off|toggle]`: flags or unflags every session in it; `toggle` unflags only when all are flagged.
- `workspace focus [on|off|toggle|add]`: `on` marks it alone and applies the filter; `add` marks it beside the
  others without applying; `off` unmarks; `toggle` (default) replaces the set with it or clears it.
  Read back as the node's `focused`.
- `workspace filter [on|off|toggle]`: applies or suspends the marked set for the window, keeping the marks.
  Prints nothing; `on` with nothing marked silently stays off, so check `result.workspaceFilter` or the tree.
- `workspace tint <red|orange|yellow|green|blue|purple|gray|clear> [--on icon,name,background]`: colours the workspace row;
  `background` (the default for a fresh tint) also washes its open sessions' rows and, very lightly, their
  terminal. A session's own `background` wash wins over its workspace's. Errors: `invalid tint: X (…)`, `invalid tint placement: X (icon,name,background)`,
  `workspace.tint clear takes no color or placement`. Read back as `tint`/`tintOn`.

## session: lifecycle

- `session new [--name N] [--cwd DIR] [--workspace W | --workspace-name N [--create-workspace]] [--command CMD]
  [--no-select] [--tint COLOR [--tint-on icon,name,background]]`
  - Prints the new id and selects it unless `--no-select`.
  - `--cwd` (`~` expanded) must exist (`not a directory: DIR`). Without `--workspace`, a `--cwd` joins the
    workspace whose remembered folder is that path. Without `--cwd`, the directory follows Settings ▸ General
    (home, workspace folder, or custom).
  - `--workspace-name` matches an exact name; `no workspace named 'N' (pass --create-workspace)`.
  - With no workspaces at all, a workspace `main` is created.
  - `--command` runs through `$SHELL -c` instead of an interactive shell, with the app's `PATH`; the session closes
    when it exits (a parent with companions respawns a shell instead).
- `session new --parent SID [--name N] [--cwd DIR] [--command CMD] [--no-select] [--tint …]`: a companion
  (`session.companion.new`). Lands right after SID, indented; `--cwd` defaults to the parent's directory.
  Refuses workspace options: `a companion takes no workspace or placement: it lands under its parent`.
  Result carries `id` and `parent`.
- `session duplicate [--no-select]`: a fresh shell in the same workspace right after the source, in its directory;
  nothing else carries over. Prints the new id.
- `session duplicate --companion [--no-select]`: a companion copying the split layout, axis, ratio and each pane's
  directory and program (Duplicate as Companion). Fails when a pane's program cannot be read.
- `session close [--undoable]`: closes now. `--undoable` gives it the GUI's 3 s undo grace when Settings allow it
  and prints `ok (undoable)` or `ok (not undoable)` (`result.undoable`). Closing a parent closes its companions.
  Repeat `--target` to close a batch; prints `N session(s)` (`result.affected`, `result.closed`).
- `session select`: selects it and shows it.
- `session rename <name>`: pins a name; an empty name unpins it (the running program's title, else "New session").
- `session info`: prints the session node plus `workspace`, `pid` (shell), `foreground` (foreground process
  name), `foregroundPid`, `liveCwd` (from `/proc`), `profile` (Konsole profile). `--json` nests it in `result.session`.
- `session move <workspace> [--index N]`: moves to that workspace, at index N or the end. Repeat `--target` to move
  several as one block; prints `N session(s)`.
- `session reveal`: opens the session's current folder in the file manager;
  `session cwd is not an existing directory` otherwise.
- `session collapse | expand`: folds or unfolds a parent's companions; `session has no companions` otherwise.
- `session flag [on|off|toggle]`: the flagged-view mark; read back as `flagged`.
- `session tint <color|clear> [--on icon,name,background]`: as `workspace tint`, for the session row.
- `session context <text> | --clear`: a short line in the title bar. Exactly one of text or `--clear`
  (`provide exactly one of a TEXT or --clear`). Trimmed, at most 256 UTF-8 bytes, no control characters or
  line breaks; blank text is refused (`context must not be empty (use --clear to remove it)`).
  Persists across relaunch; read back as `context`.
- `session seen`: clears the unseen badge without selecting the session.
- `session go --to next|prev|first|last|next-attention|prev-attention`: moves the window's selection; attention
  steps visit `blocked`/`completed` (and `active` when Settings include it). Stays inside the flagged view or the
  applied filter. Error: `no session to navigate`. No `--target`.

## session: status and notifications

- `session status <idle|active|completed|blocked> [--blink] [--auto-reset]`
  - `--blink` pulses the dot (ignored for `idle`). `--auto-reset` returns `completed` to idle when the user
    selects the session.
  - Typing into the session clears `blocked`/`completed` per Settings ▸ Agent Status (first key, Enter, or never);
    `session type` text with a newline counts as Enter.
  - A same-value set emits nothing. Each change emits `status` with `previous`.
  - Error: `status must be one of idle, active, completed, blocked`.
  - Read back: `status`, `blink`.
- `notify <body> [--title T]`: a desktop notification attributed to the session (title defaults to its name) and
  +1 on its unseen badge; clicking the notification selects the session. With banners off in Settings it still
  raises the badge and answers `badge updated, but "Show notification banners" is off, so no banner was posted`.
  Errors: `notify requires a body`. Programs in a pane can do the same with OSC 9 or OSC 777
  (`printf '\e]9;done\a'`).

## session: terminal text and input

- `session text [--all | --lines N] [--pane left|right]`: the visible screen of the main pane by default;
  `--all` adds the scrollback; `--lines N` keeps the last N non-blank rows of screen plus scrollback.
  `result.pane` names the pane read. Errors: `use either --all or --lines, not both`,
  `--lines must be greater than 0`, `invalid pane: X`, `session has no split pane`, `session not realized`.
  Text under an overlay is the covered shell's, not the overlay's (use `session overlay text`).
- `session type <text> | --stdin`: sends the text to the main pane as keyboard input; a newline submits.
  `session has no terminal` when the pane is gone.
- `session copy`: prints the pane's selected text; `no selection` when none. Does not touch the clipboard.
- `session paste [--pane left|right]`: pastes the clipboard into the pane, as Konsole's paste action.
- `session select-all`: selects the whole buffer (read it with `session copy`).
- `session search [needle] [--next | --prev | --close]`: opens Konsole's search bar on the session (selecting it),
  sets the needle, steps to the next/previous match, or closes the bar. Prints `N matches` / `no matches`
  (case-insensitive count over the history). Errors: `--next, --prev, and --close are mutually exclusive`,
  `--close cannot be combined with a needle`.

## session: splits and layout

- `session split [on|off|toggle] [--axis vertical|horizontal]`: `vertical` = side by side, `horizontal` = top and
  bottom; omitted keeps the current axis. `off` hides the split and keeps its shell. Error:
  `mode must be on, off or toggle; axis vertical or horizontal`. Emits `pane.split`.
- `session split close`: ends the split pane and what runs in it; `session has no split`.
- `session swap`: exchanges the two terminals' positions without restarting them; `session has no split`.
- `session focus [left|right|other]`: moves keyboard focus between the panes (default `other`);
  `session has no split`, `invalid pane: X`. Read back as `splitFocused`.
- `session resize --split-ratio R | --grow-left D | --grow-right D` (also `--grow-top`, `--grow-bottom`,
  `--grow-primary`, `--grow-split`): sets the main pane's fraction (clamped 0.05–0.95) or moves it by D.
  Prints the applied ratio (`result.ratio`). Errors: `provide exactly one split ratio or grow option`,
  `session has no split`.
- `font inc|dec|reset [--pane left|right]`: the pane's font size, persisted per session.
  Errors: `pane must be left or right`, `session has no split`. Read back: `result.fontSize`, the node's
  `fontSize`/`splitFontSize`.
- `surface zoom [show|hide|toggle] [--target active|surface:<session>:left|right]`: fills the window with one pane
  (sidebar hidden). Zooming selects that session. Errors: `invalid surface zoom mode: X`, `no active surface`,
  `invalid surface: X`, `surface not available: X`. Read back: `zoomedSurface`.

## session overlay

All take `--target` (default your session) and act on the session-wide slot unless `--pane left|right` names a pane slot.

- `session overlay open <command> [--cwd DIR] [--wait | --block] [--size-percent N [--width PT] [--height PT]]
  [--background-color #rrggbb] [--follow] [--pane left|right]`
  - Runs `command` through `/bin/sh -c` in a temporary Konsole terminal over the session, in `--cwd` or the
    session's live directory. The overlay closes when the program exits; its exit status is kept for `result`.
  - Full session by default; `--size-percent 1..100` floats it in a centred panel; `--width`/`--height` (points)
    size that panel per axis and need `--size-percent`. `--pane` covers one split pane and is always full-pane.
  - Opens on the target without switching the user; `--follow` selects the session.
  - `--wait`: after exit shows `[exited N, press a key to close]`. `--block`: the CLI polls and exits with the
    program's status, printing nothing. Exclusive: `--block is mutually exclusive with --wait`.
  - `--background-color` tints the floating frame only; terminal colours come from the Konsole profile.
  - A program replaces a HUD in the session slot.
  - Errors: `session.overlay.open requires a command`, `not a directory: DIR`, `overlay already open`,
    `pane overlay already open`, `overlay hidden` (a hidden one still holds the slot), `pane not visible`
    (the right pane of a hidden split), `session has no split`,
    `session.overlay.open: --pane is mutually exclusive with --size-percent`,
    `session.overlay.open: --width/--height need --size-percent`, `…: --size-percent must be 1...100`,
    `invalid color: X (#rrggbb)`, `page overlays need Qt WebEngine, which this build lacks …` for `--html`/`--url`.
- `session overlay result [--pane left|right]`: prints the exit status of the last overlay in that slot.
  Errors: `overlay still running`, `no overlay result`, `no overlay result: the slot holds a hud`.
- `session overlay text [--lines N] [--pane left|right]`: the overlay terminal's screen; `--lines` keeps the last N.
  Errors: `no overlay`, `no overlay to read: the slot holds a hud`, `overlay not realized`.
- `session overlay close [--pane left|right]`: ends the program; on the session slot it also closes a HUD.
  `no overlay` otherwise.
- `session overlay hide | show | toggle [--pane left|right]`: hides or shows the overlays (all three slots without
  `--pane`); the program keeps running. Prints `hidden`/`shown`. Read back as `hiddenOverlays`.
  The user's key is Ctrl+Alt+Shift+J.
- `session overlay resize (--size-percent N [--width PT] [--height PT] | --width PT | --height PT | --full)`:
  resizes the session-wide overlay without restarting it; `--full` returns it to full size. On a HUD only
  `--size-percent` applies (`a hud is always floating: pass --size-percent, not --full`).
  Errors: `session.overlay.resize: --pane is not supported (pane overlays are always full)`,
  `session.overlay.resize: --full is mutually exclusive with --size-percent`,
  `session.overlay.resize requires --size-percent, --width/--height or --full`, `no overlay`.
- Read back: `overlay`, `overlaySizePercent`, `overlayWidth`/`overlayHeight`, `paneOverlays`, `hiddenOverlays`.

## session hud

A passive panel over the session: no focus, clicks pass through, the session stays typable.

- `session hud [open] <message> | --file F [--markdown] [--font-size PT] [--detail T] [--spinner]
  [--spinner-style S] [--position P] [--background-color #rrggbb] [--text-color #rrggbb] [--size-percent N]
  [--sticky] [--no-frame] [--hide-after SECONDS] [--pane left|right]`
  - `open` is implied; use it explicitly for a message that is literally `update` or `close`.
  - `--file F` reads the message from a file (trimmed). `--markdown` renders it and allows newlines and up to
    4096 characters; plain text is one line of at most 256. `--detail` is a dim second line (256 max).
  - `--spinner` shows the `bar` spinner; `--spinner-style bar|braille|circle|blocks|dot|none` picks one.
  - `--position`: `top-left|top-center|top-right|center-left|center|center-right|bottom-left|bottom-center|bottom-right`
    (default `center`; `top`/`bottom` are accepted for the centres).
  - `--size-percent N` sets the width share; height follows the text. `--sticky` drops the edge margin,
    `--no-frame` the border. `--font-size` 6–72 points. `--hide-after` 0–86400 s (0 = stays).
  - `--pane` uses that pane as the placement area; `pane not visible` when the right pane is hidden.
  - A second open replaces the HUD. Refused while a program overlay holds the slot: `overlay already open`.
  - Errors: `session.hud.open requires a message`, `hud text must not contain control characters`,
    `hud message too long (max N characters)`, `hud detail too long (max 256 characters)`,
    `invalid position: X (…)`, `invalid spinner: X (…)`, `invalid text color: X (#rrggbb)`,
    `session.hud.open: --font-size must be 6...72 points`, `…: --hide-after must be 0...86400 seconds`.
- `session hud update <message> …`: replaces the whole spec in place; omitted options drop. Font size and
  background stay from open (`--font-size` on update is refused, `--background-color` ignored). `no hud` when none.
- `session hud close`: `no hud` when none.
- Read back: the node's `hud`. Nothing emits an event for a HUD; poll `tree`.

## dashboard

`agtermctl dashboard <id[:left|:right]>… | --mru [--font-size N | --auto-size] | --close [--window W]`

- Opens a view-only grid of live panes in the window: at most 9 cells, `ceil(sqrt(n))` columns. A split session
  gives two cells unless the id carries `:left`/`:right`. `--mru` takes the window's most recent sessions.
- The keyboard drives it: arrows move the highlight, Enter or a click opens that pane, Esc closes. Selecting
  another session closes it; zoom and the dashboard exclude each other.
- `--font-size N` sets an absolute cell font; `--auto-size` scales the default font by grid size.
- Dropped targets and the 9-cell overflow come back as a note in `result.text`; `result.members` lists the cells.
- Errors: `dashboard requires at least one session id`, `dashboard --mru cannot be combined with explicit session ids`,
  `dashboard: --font-size is mutually exclusive with --auto-size`, `dashboard --font-size must be a positive number`,
  `dashboard: invalid session id 'X' — use <id>, <id>:left, or <id>:right`, `no recent sessions`,
  `dashboard --close takes no ids, --mru, or font options`, `no dashboard`.
- Read back: `dashboardMembers`, `dashboardHighlighted`, `dashboardFontMode`, `dashboardFontSize`.
- The user's key is Ctrl+Alt+Shift+G (the MRU grid, auto-sized).

## pick

`agtermctl pick [open] [--prompt T] [--query T] [--select ID] [--allow-custom] [--follow] [--no-block] [--window W]`

- Items come from stdin: a JSON array (first non-space byte `[`) of `{id, label, subtitle?}`, or lines, each
  non-blank line being id and label. Stdin must be UTF-8 and is always read.
- Typing filters labels; with an empty query the supplied order holds, so the first item is the default.
  `--prompt` is the placeholder, `--query` prefills, `--select ID` highlights an item.
  `--allow-custom` returns a non-matching query as a custom result and accepts an empty list.
- Blocking (default): prints one JSON line and exits 0 for `{"result":"picked","id":…,"label":…,"index":N}` or
  `{"result":"custom","query":…}`, 2 for `{"result":"cancelled"}`, 1 on error. Killing the CLI leaves the picker
  open; cancel it with `pick cancel`.
- `--no-block` prints `{"id":"<pick-id>"}`. `pick result <id>` prints the outcome (exit 1 while
  `{"result":"pending"}`); `pick cancel <id>` cancels. The last 8 results stay readable.
- Errors: `pick.open requires at least one item`, `too many items (max 1000)`, `pick item label must not be empty`,
  `pick item ids must be unique`, `item text must not contain control characters`,
  `pick select must name an item id`, `pick already pending`, `ask already pending` (a GUI ask holds the slot),
  `unknown pick: <id>`, `pick.result requires a pick id`.
- Read back: `pickPending`.

## ask

`agtermctl ask [open] <title> [options]`; use `ask open` when the title is `open`, `result` or `cancel`.

- `--message T`: body text. `--button ID=LABEL` (1–6; a bare token is id and label), `--hotkey ID=LETTER`,
  `--default ID`, `--destructive ID` (never the default), `--cancel ID` (that button answers `cancelled`).
- `--input line|multiline [--value T] [--placeholder T]`: a text field; the answer adds `text`.
- `--choice ID=LABEL …` (1–50) radio list, checkboxes with `--multiple`; `--choice-subtitle ID=TEXT`,
  `--checked ID`, `--other PLACEHOLDER` (a free-form line). The answer adds `selected` (ids) and `text` for
  the other line. Input and choices are exclusive; either defaults the buttons to `ok`=OK, `cancel`=Cancel.
- `--style terminal|gui`: terminal (default) is one pending ask per session, shown at once as a dialog over the
  window; gui is the window's modal slot shared with `pick`. `--align left|center|right`, `--width 10..100`
  (percent of the window), `--pane left|right`, `--follow` (selects the session).
- Target: `--target` (default `active`); pass `--target "$AGTERM_SESSION_ID"` for your own session.
- Blocking output and exit: `{"result":"answered","id":…,"label":…,"index":N,"text"?:…,"selected"?:[…]}` exit 0;
  `{"result":"cancelled"}` exit 2; `{"result":"escaped"}` exit 3 (Esc or the dialog closed); 1 on error.
  A "No" button is an answer: check `.id`.
- `--no-block` prints `{"id":…}`; `ask result <id>` (exit 1 while pending), `ask cancel <id>`. The last 32
  results stay readable.
- CLI-side errors: `ask requires at least one --button`, `--hotkey requires --button`, `--hotkey requires ID=LETTER`,
  `unknown hotkey button: ID`, `--choice-subtitle requires --choice`, `--other requires --choice`.
  App errors: `ask.open requires a title`, `input and choices are exclusive`, `value requires input`,
  `checked requires choices`, `ask choices must be 1 to 50`, `too many buttons (max 6)`,
  `ask button ids must be unique`, `unknown default button: X`, `default button must not be destructive`,
  `ask input does not take hotkeys`, `ask button hotkey must be one ASCII letter`, `unknown style`,
  `unknown align`, `width must be 10 to 100`, `--pane must be left or right`, `ask already pending`,
  `pick already pending`, `session has no split pane`, `unknown ask: <id>`.
- Read back: the node's `ask` (terminal style), top-level `askPending` (gui style). Ask emits no events.

## msg

Per-pane mailboxes in the app's memory; nothing is drawn.

- `msg send <text> | --stdin (--to SESSION:PANE … | --all [--workspace W]) [--as SESSION:PANE-ID]`
  - PANE is a pane id (`AGTERM_PANE_ID` of that pane) or a role, resolved at send time. Duplicates deliver once.
  - `--all` reaches every pane of the window's sessions except the sender; `--workspace` narrows it.
  - Prints the message id. Each rejected recipient prints `rejected ADDRESS: REASON` (`unknown pane`,
    `mailbox full`) on stderr and the exit is 1; delivered copies stay queued.
  - Result: `messageID`, `delivered` (`SESSION:PANE-ID`), `rejected` (`{address, reason}`).
  - Errors: `provide the body as TEXT or --stdin, exactly one`, `msg.send requires --to or --all`,
    `msg.send takes --to or --all, not both`, `msg.send: --workspace narrows --all only`,
    `message body too large (N bytes, max 65536)`, `invalid address "X": expected SESSION:PANE`.
- `msg read [--ack] [--wait [--timeout S]] [--as SESSION:PANE-ID]`
  - Prints `ID from SENDER` (or `from outside agterm`) then the body, oldest first, at most 32; `N more pending`
    on stderr. Removes nothing unless `--ack`, which removes the returned batch.
  - `--wait` polls every 250 ms until mail arrives; `--timeout` exits 124 with `timed out`.
  - Result: `messages` (`{id, from?, to, ts, body}`), `remaining`, `acked` with `--ack`.
- `msg ack <id>… [--as SESSION:PANE-ID]`: removes those ids; prints `acked N`. Unknown ids are skipped;
  a malformed one fails with `invalid message id: X`.
- Identity: `--as`, else `$AGTERM_SESSION_ID:$AGTERM_PANE_ID`. It must name a live pane id, never a role:
  `identity "X" must name a pane id, not a role`, `pane X is not a live pane of session S`. Only one of the two
  variables set is an error (`incomplete pane identity …`). `read`/`ack` need an identity; `send` without one is anonymous.
- Limits: body 64 KiB; a mailbox holds 256 messages and 1 MiB. A respawned shell has a new pane id and an empty box.
  Mail is lost when the app quits. Each delivery emits `message`.

## recent

- `recent list`: closed sessions, newest first, as `id  kind  title (cwd)`; `no recently closed items` when empty.
  JSON: `result.recent[]` = `{id, kind, title, subtitle, closedAt, workspaceID, sessionIDs}`.
- `recent open [id]`: reopens by id or unique prefix, the newest without one; prints the session id.
  Errors: `no recently closed items`, `no such recent item: X`, `ambiguous recent item prefix …`.
- `recent clear`: empties the list. `list` and `clear` refuse a target or `--window`.

## sidebar and file panel

- `sidebar [show|hide|toggle]`: read back `sidebarVisible`.
- `sidebar mode [tree|flagged|toggle]`: the tree or the flagged sessions; `invalid sidebar mode: X`.
  Read back `sidebarMode`. In flagged mode `session go` stays inside the flagged set.
- `sidebar flagged-layout [flat|plain|tree|toggle]`: app-wide; `flat` = `session : workspace`, `plain` = names only,
  `tree` = under workspace rows. Prints the layout; read back `sidebarFlaggedLayout`.
- `sidebar expand | collapse`: every workspace row; `collapse` keeps the current workspace open.
- `sidebar width <points>`: clamped to 160–560, prints the stored width; `points must be a number`.
  Read back `sidebarWidth`.
- `filepanel [show|hide|toggle]`: the active session's folder tree beside the sidebar.
  `mode must be show, hide or toggle`.
- `filepanel width <px>`: clamped to 160–560; below 120 fails with `width must be at least 120`.
- Read back: `filePanel.visible`, `filePanel.width`.

## window

- `window new [name] [--minimized]`: prints the new id.
- `window list`: `* id name open|closed` per window (`*` = active). JSON `result.windows[]`: `id`, `name`, `open`,
  `active`, and for an open window `geometry` (`{width, height, display, x?, y?}`; `x`/`y` omitted on Wayland),
  `sidebarVisible`, `fullscreen`, `zoomed`, `minimized`.
- `window select [id]`: opens a closed window or raises an open one.
- `window close [id]`: closes it without confirmation, keeping it reopenable; closing the last open window quits
  the app. `window delete [id]` removes it and its saved tree; `cannot delete last window`.
- `window rename <id> <name>`: `window.rename requires a name`.
- `window go --to next|prev`: the next or previous open window; `window.go requires --to next|prev`.
- `window resize [id] --width W --height H`: clamped to the screen; prints `WxH`.
  `window.resize requires positive width and height`.
- `window move [id] --x X --y Y [--display N]`: relative to that display; a strip stays on screen.
  Errors: `window.move requires x and y`, `display N out of range (have M)`,
  `window.move: Wayland does not let an application position its windows`.
- `window zoom [id]`: maximise toggle. `window fullscreen [id]`: full-screen toggle.
- `window minimize [id] [on|off|toggle]`: `cannot minimize a full-screen window — window.fullscreen it first`,
  `invalid window minimize mode: X`.
- Errors shared by resize/move/zoom/fullscreen/minimize: `window not open — window.select it first`.
- The id is a full id, unique prefix, or `active`.

## keymap

- `keymap reload`: re-reads `keymap.conf`; prints `N diagnostic(s)`.
- `keymap list`: the file path, each built-in action with its chords (`(mapped)` when the file rebinds it), each
  custom command, the diagnostics as `line N: message`, and the menu actions with their live chords.
- `keymap run <name> [--target]`: starts that custom command for the session (default your session). The reply
  means it started; `no custom command named 'X'` otherwise.

### keymap.conf format

`~/.config/agterm-qt/keymap.conf` (or `$AGTERM_STATE_DIR/config/keymap.conf`); written as a commented starter
when missing; File ▸ Edit Keymap… opens it.

- `map <chord> <action>`: rebinds a built-in action; the action loses its default chord.
  Modifiers `ctrl`, `shift`, `alt` (`opt`), `super`, joined with `+`; `cmd` alone means ctrl+shift, `cmd` with
  another modifier means ctrl+alt+shift. Keys: a character, `tab`, `space`, `return`, `delete`, `escape`, arrows,
  `home`, `end`, `pageup`, `pagedown`, `f1`–`f20`; a shifted symbol is `shift+<base key>`.
  `|` joins alternatives (no spaces), `>` makes a leader sequence (`ctrl+a>s`). The first key needs ctrl, alt or
  super, or is a function key.
- `command "<name>" [chord] [--error-hud [--error-position P] [--error-pane left|right]] <shell…>`: a custom
  command in the custom-commands palette (Ctrl+Alt+O) and the action palette. The rest of the line runs through
  `/bin/sh -c`, detached with no terminal, in the session's directory, with the app's binary directory first on
  `PATH`. A non-zero exit posts a notification; `--error-hud` also shows a 10 s HUD with the last stderr line.
- Tokens `{AGT_SESSION_ID}`, `{AGT_SESSION_NAME}`, `{AGT_SESSION_PWD}`, `{AGT_SESSION_HOST}`, `{AGT_WORKSPACE_ID}`,
  `{AGT_WORKSPACE_NAME}`, `{AGT_WINDOW_ID}`, `{AGT_WINDOW_NAME}`, `{AGT_PANE}`, `{AGT_PANE_ID}`, `{AGT_SELECTION}`,
  `{AGT_SOCKET}` expand raw in the line and are exported as `$AGT_*`; prefer the quoted variable for text you do
  not control. Pass `--socket "$AGT_SOCKET"` and `--target "$AGT_SESSION_ID"` to agtermctl from a command.
- `--repeat` is accepted with a diagnostic and not honoured; `global-hotkey` is skipped with a diagnostic.

```
map ctrl+alt+l toggle_split
command "Lazygit" ctrl+a>g agtermctl session new --parent "$AGT_SESSION_ID" --command lazygit --socket "$AGT_SOCKET"
```

## hooks

- `hooks reload`: re-reads `hooks.conf`; prints `N diagnostic(s)`.
- `hooks list`: each `line N  on KIND COMMAND` with `[running pid P]`, `[pending N, dropped M]`,
  `[last failure: …]`, `(retired)` for a removed line whose process still runs, then the diagnostics.
- Both are app-wide.

### hooks.conf format

`~/.config/agterm-qt/hooks.conf`; File ▸ Edit Hooks… opens it.

- `on <kind> <shell…>`: runs the line through `/bin/sh -c`, detached, for every event of that kind (kinds as in
  `events`). Several lines per kind run independently; a duplicate kind+command line is skipped.
- One process per line at a time; further events queue (256, oldest dropped).
- The script gets the event JSON on stdin and `AGT_EVENT_KIND`, `AGT_EVENT_STATUS`, `AGT_EVENT_HOST`,
  `AGT_SESSION_ID`, `AGT_WORKSPACE_ID`, `AGT_WINDOW_ID` (empty), `AGT_SOCKET`. Pass `--socket "$AGT_SOCKET"`.
- A hook whose command emits an event of its own kind triggers itself again.
- `on app.launched` is where a script recreates what a relaunch does not restore.

```
on status [ "$AGT_EVENT_STATUS" = blocked ] || exit 0; notify-send "$AGT_SESSION_ID needs input"
```

## theme

- `theme list`: Konsole colour schemes, `*` on the current one.
- `theme set [name]`: applies a scheme to every terminal at runtime (no profile file is written) and persists it;
  no name returns to the Konsole profile's own scheme. `unknown theme: X`.

## restore

- `restore mode [none|rerun|live]`: bare, prints `configured`, `at launch`, `active`, `(restart required)` when
  they differ. A value applies from the next launch. `invalid restore mode: X`.
- `restore capture`: records each pane's foreground command now (rerun mode only; idle shells skipped); prints
  `captured N command(s)`. `restore.capture requires rerun mode; configured restore mode is X`.
- `restore clear`: forgets every captured command.
- `session restore <command> | --none | --clear [--pane left|right]`: pins what the pane re-runs at the next
  launch: a command, `--none` (a plain shell), or `--clear` (back to the captured command). Sticky until cleared;
  applies only in rerun mode (otherwise `saved; it applies only in rerun restore mode`). Errors:
  `--pane must be left or right`, `session has no split`, `invalid restore mode: X`. Read back as
  `restoreCommand`/`splitRestoreCommand`. The pin is stored shell code: keep secrets out.
- `restore-denylist.conf` in the config directory lists programs never re-run (starter: tmux, screen, zellij).

## Not supported on this platform

The app answers `<cmd> is not supported on this platform (<reason>)` for:

| Command | Reason |
| --- | --- |
| `quick …` | no quick terminal |
| `session scratch` | no scratch terminal |
| `zmx …` | live sessions run on agterm-session-host, not zmx |
| `session restart` | not ported to live sessions |
| `session lead` | pane leads belong to remote sessions |
| `session background` | Konsole takes a pane's background from its profile |
| `surface cursor` | Konsole's terminal part does not expose the cursor |
| `config reload` | no Ghostty config; `keymap reload`/`hooks reload` re-read agterm-qt's files |
| `browser …`, `session overlay reload|navigate|submit|copy`, page companions | need Qt WebEngine |

`session overlay open --html|--url` fails with the WebEngine reason above. Never pass master's `--image`, `--markdown`
or `--text` to `overlay open`: the CLI does not know them as page options (`--markdown FILE` would run FILE as the command).
Not in the CLI at all: `session wait`, `clipboard`, `terminfo`, `session flag move`, `workspace move --to`,
`session move --to|--after|--before`, `session new --wait|--after|--before`, `session status --color|--shape|--sound|--pane`.
An unknown command answers `unknown command '<cmd>'`.
