# Parity with agterm master

The target is agterm `master` (NaikSoftware fork), not a redesign.
Sources: `site/docs.html`, the site screenshots, the Swift code, and the fork-only commits.
macOS keys map to Linux as ⌘ → Ctrl+Shift (Konsole's convention); ⌘⇧ and ⌘⌃ → Ctrl+Alt+Shift; ⌃ → Ctrl; ⌥ → Alt.

Legend: [x] done · [ ] todo · (fork) = fork-only feature, not in upstream docs.

## Phase 1: sidebar, chrome, sessions

- [x] Title bar strip: sidebar toggle, session name
- [x] Title bar: file panel toggle (fork, left of the sidebar toggle); split button
- [x] Sidebar rows: workspace = chevron + grid icon + name; session = terminal icon + name + status dot at the right
- [x] Session icon = logo of the agent in its foreground process group (claude, codex, opencode, agy), sampled every 2 s (fork)
- [x] Workspace "+" on hover: menu of Terminal + agents found on the login shell PATH, with logos; repeat button for the last choice (fork)
- [x] Remembered folder per workspace (last cwd its panes reported); new sessions open there (fork)
- [x] Settings: new session directory (home / workspace folder / custom), placement (end / after current)
- [x] Session name: pinned rename, else the title a running program (agent) set (agy's conversation title, which it keeps in its own store, not the terminal), else the plain command in the foreground (sampled every 2 s, cleared when it ends), else the folder (`~` for home); shell prompt titles ignored; inline rename on double-click
- [x] Sidebar footer: new session, new workspace, flagged view (flat list)
- [x] Sidebar footer: workspace filter
- [x] Session context menu: Clear Status, Rename, Duplicate Session, Move to ▸, Flag, Close Session (batch over a multi-selection)
- [x] Session context menu: New Companion
- [x] Workspace context menu: New Session, Rename, Flag All Sessions, Delete Workspace
- [x] Workspace context menu: Focus/Unfocus, Add to/Remove from Focus, Tint
- [x] Session context menu: Tint (None, seven colours, Show On icon/name; batch), palette Tint Session items, `session|workspace tint`, `--tint` on new
- [x] Drag to reorder sessions and workspaces; drag a session to another workspace; multi-select
- [x] Drop folders from the file manager onto the tree to open sessions there
- [x] Strip agent spinner glyphs (✳ ◑ braille) from terminal titles (fork)
- [x] Close-session reselection by MRU within the workspace
- [x] Shortcuts: new session ⌘N, new workspace ⌘⇧N, open directory ⌘O, close ⌘W, prev/next session ⌥⌘↑↓, attention ⌃⌥↑↓, toggle sidebar ⌘⌃S, flag ⌘⇧F
- [x] Shortcuts: reopen ⌘⇧T, font ⌘+ ⌘− ⌘0 (`font inc|dec|reset`, sizes persisted; driven through Konsole's Ctrl+wheel zoom)

## Phase 2: terminals

- [x] Split ⌘D / ⌘⇧D (one session, two shells; transposes; hides; divider memory, double-click resets; swap panes; focus left/right; close split)
- Scratch: dropped at the owner's request (2026-11-04)
- Quick terminal: dropped at the owner's request (2026-11-04)
- [x] Search: Konsole's own search bar (Ctrl+Shift+F)
- [x] Terminal zoom ⌘⇧↩ (⌘W leaves zoom)
- [x] Fullscreen (F11: ⌘⌃F maps onto the flag chord)
- [x] Companions (pinned under parent, folding with attention marker, moves with parent, closes with parent, respawn on exit, `session new --parent`)
- [x] Duplicate as Companion (copies the split layout and each pane's program; `session duplicate --companion`)

## Phase 3: navigation and windows

- [x] Session palette ⌃P, action palette ⌃⇧P (fuzzy, Ctrl-J/K, disabled actions greyed)
- [x] Custom-commands palette ⌃⇧O → Ctrl+Alt+O (Ctrl+Shift+O is ⌘O, Open Directory, so its Shift becomes Alt)
- [x] Ctrl-Tab MRU switcher (hold Ctrl, Tab walks, release switches; one tap flips back)
- [x] Recent-sessions title-bar button (a menu under the clock, not a popover)
- [x] Flagged view layout (Settings ▸ General: Flat list `session : workspace`, Flat list names only, Workspace tree sharing the
  tree's collapse state), View ▸ Edit Flagged Sessions… / Edit Focused Workspaces… (live checklists; the empty flagged view links
  to the first). The flat list follows tree order, not a per-window flagged order of its own
- [x] Workspace filter / focus (`workspace focus on|off|toggle|add`, `workspace filter`; suspends on an outside select; navigation stays inside)
- [x] Reopen closed session ⌘⇧T, `recent open`
- [x] Undo close ⌘Z within the 3 s grace (sessions, batches with companions, workspaces; Settings switch; `session close --undoable`)
- [x] Multiple windows: a library of named windows, each its own tree (`windows.json` + `windows/<id>.json`; an older
  `workspaces.json` becomes window 1), File ▸ New Window (Ctrl+Alt+N: ⌥⌘N maps to Ctrl+Alt+Shift+N, which is ⌘⇧N New Workspace here),
  Open Window ▸, Rename Window…, Delete Window, Navigate ▸ Previous/Next Window; closing one of several windows asks and keeps it
  reopenable, closing the last quits; windows open at quit reopen with their frames and the frontmost in front. Settings are app-wide.
  `window new|list|select|go|close|rename|delete|resize|move|zoom|fullscreen|minimize`, `--window W` on session/workspace/tree
  commands (a session id finds its window without it), panes export `AGTERM_WINDOW_ID`. Wayland: `window move` is refused and
  `window list` omits x/y (a client can neither place nor locate its window there); resize, zoom, fullscreen and minimize work.
  Not ported: palette "Open Window: <name>" entries, the auto-hide-sidebar-in-background-windows setting, the Dock menu
- [x] Notifications: `notify`, unseen-count badge, banner click focuses the session, `session seen`
- [x] Notifications from OSC 9 / 777 inside the terminal (two OSC 9s in one output burst arrive as the last one: Konsole coalesces an attribute); settings: banner, badge, sound, taskbar highlight for the Dock bounce
- [x] Agent status: dot colours/shapes, blink, status reset (first key / Enter / off), blocked sound, attention includes active, Reset to defaults
- [x] Agent status: auto-follow blocked sessions after an idle timeout (Disabled, 5 s … 5 min; oldest block first, each once per
  episode; parked on a blocked session suppresses; Auto-follow away from a running session); read-back `idleMs`, `autoFollowMs`

## Phase 4: file panel (fork)

- [x] Column right of the sidebar with the active session's folder tree, live refresh
- [x] Atom file-icons
- [x] Open: Markdown, text and images in the built-in viewer; others with the default app
- [x] Context menu: Open, Open with Default App, New Companion Here, Cut, Copy, Paste, Rename…, Move to Trash
- [x] Drag inside to move (Ctrl to copy), drop from the file manager to copy in, drop on a terminal to paste the path
- [x] `filepanel [show|hide|toggle]`, `filepanel width`

## Phase 5: settings, keymap, extension

- [x] Settings dialog: General, Appearance, Interface, Notifications, Agent Status, Key Mapping (KPageDialog, every control live).
  Not here, with their features: flagged view layout, link opening, scroll speed, right-click paste, Ghostty config,
  toolbar mode, opacity/blur, sidebar tint, inactive-pane mute, follow system appearance, window/remote/dashboard/scratch/overlay
  title-bar elements, session info line, multiple-window sidebar, attention indicator, crossing windows, keyboard-layout lock
- [x] Theme picker with live preview (Konsole colour schemes applied per session at runtime, no profile file written; `theme list|set`)
- [x] keymap.conf: built-in rebinding, alternatives, leader sequences, `command "Name" chord shell…`, AGT_* tokens and env, diagnostics,
  File ▸ Edit/Reload Keymap (the editor runs in a 95% overlay and the file reloads when it quits), `keymap reload|list|run`.
  `--repeat` is accepted with a diagnostic and not honoured yet; `--error-hud` opens a 10 s failure HUD with the last stderr line;
  `global-hotkey` is refused (it summons the removed quick terminal)
- [x] Hooks (`hooks.conf`, File ▸ Edit/Reload Hooks, `hooks reload|list`): status, notify, session.created/closed/selected,
  tree.changed, pane.split, app.launched, message fire; pane.scratch, pane.focused and remote.* parse but never fire
- [ ] Page bridge
- [x] Program overlays: `session overlay open|close|result|text|hide|show|toggle|resize` (full, `--size-percent`,
  `--width/--height`, `--pane left|right`, `--wait`, `--follow`, CLI `--block`), View ▸ Hide/Show Overlays Ctrl+Alt+Shift+J;
  read-back `overlay`, `overlaySizePercent`, `overlayWidth/Height`, `paneOverlays`, `hiddenOverlays`.
  `--background-color` tints the floating frame only (Konsole takes colours from the profile); `overlay copy` is not ported
- [x] Dashboard Ctrl+Alt+Shift+G (Navigate ▸ Dashboard, palette: the MRU grid, auto-sized; again closes), `dashboard <id[:left|:right]>…
  | --mru [--font-size N | --auto-size] | --close`; ≤9 cells, ceil(√n) columns, master's auto font factors; the cells are the live
  Konsole panes themselves, re-hosted view-only (arrows without wrap, Enter or click opens that pane, Esc closes) with a name chip in
  the status colour, pulsing while it blinks; members that go away are dropped. Read-back `dashboardMembers`, `dashboardHighlighted`,
  `dashboardFontMode`, `dashboardFontSize`. The title-bar grid button is the `dashboard` Interface element (filled while open).
  Not ported: the label shown for a
  pane under an overlay (the overlay stays on the session page)
- [ ] Page overlays (`--html`, `--url`, `--image`, `--markdown`, `--text`): blocked, they need Qt WebEngine (`qt6-webengine-dev`),
  which is not installed; the command refuses them with that reason
- [x] HUD: `session hud open|update|close` (markdown, detail, spinner styles, nine positions, colours, font size, width percent,
  sticky, no-frame, `--hide-after`, `--pane`), passive (no focus, clicks pass through), read-back `hud`; control-only as in master.
  Not ported: ⌘-click on a HUD link
- [x] Restore modes: Settings ▸ General ▸ Restore sessions (`restoreMode` none | rerun | live, applies at the next launch);
  re-run captures each pane's foreground group leader argv from /proc at a clean quit (idle shells skipped) and types it
  into the new shell once; `restore-denylist.conf` (starter: tmux, screen, zellij); `restore mode|capture|clear`,
  `session restore <cmd>|--none|--clear [--pane]` pins, read-back `restoreCommand`/`splitRestoreCommand`
- [x] Live sessions: each main/right pane runs under `agterm-session-host` (in place of master's zmx), a daemon in
  `$XDG_RUNTIME_DIR/agterm-qt-<state hash>/` that keeps the pty across quits; the next launch reattaches, replaying the
  last 4 MB of output plus the DEC modes in force and making the foreground program redraw. A daemon that is gone
  falls back to the captured command, as master's create-only payload. Deleting a session, closing a split or deleting
  a window ends its daemons; closing a window keeps them; a `none`/`rerun` launch reaps them all. Pane ids survive a
  reattach. Not ported: `zmx.*`, `session restart`, a reset dialog; the replay is raw output, not a rendered screen
- [x] Control API breadth: master's catalog with its argument names, errors and read-backs — `events` (an app-wide ring
  with run/after cursors; the CLI polls it), `workspace go|collapse|expand|flag`, `session context|reveal|collapse|expand|focus|resize|
  copy|paste|select-all|search`, `session text` (visible screen; `--all` and `--lines` page the scrollback through the view's
  scrollbar), `session new --parent` as `session.companion.new`, `surface zoom`, `sidebar [show|hide|toggle]|mode|flagged-layout|
  expand|collapse|width`, `pick` (the palette; exits 0 picked/custom, 2 cancelled), `ask` (a dialog; buttons, hotkeys, line or
  multiline input, choices with other; exits 0/2/3), `msg send|read|ack` (panes export `AGTERM_PANE_ID`), `recent list|open <id>|clear`,
  `notify <body>`, repeated `--target` for `session close|move`. Read-back `context`, `collapsed`, `splitFocused`, `ask`,
  `sidebarFlaggedLayout`, `sidebarWidth`, `zoomedSurface`, `pickPending`, `askPending`. Absent features answer master's
  "<cmd> is not supported on this platform" with the reason: `zmx.*`, `session restart|lead|background`, `surface cursor`
  (Konsole exposes no cursor), `config reload`, `browser.*`, page overlays/companions, quick and scratch terminals.
  Simplified: a terminal-style ask is a dialog over the window rather than drawn in the pane; pick rows show no status glyph;
  `msg send --all` reaches the addressed window only; search counts case-insensitive matches in the pane's history;
  `pane.focused` events are not emitted
- [x] Agent skill (`share/skill/agterm`, adapted to agterm-qt) and Help ▸ Developer Documentation…, Install Command Line Tool…
  (links `~/.local/bin/agtermctl`; an agtermctl already there is left alone), Install Agent Status Hooks… (master's package and
  merge rules: Claude Code hooks + the swarm-companion mod, shell rc integration, Codex `config.toml` block, Pi extension,
  OpenCode v1/v2 plugin with the login-shell version probe; symlinks and modes kept, `.bak` backups, idempotent; an older
  agterm-qt hook install is migrated), Install Agent Skill… (Claude Code and Codex, ownership marker), and master's
  first-launch welcome (skipped when the state dir holds anything). `agterm-qt --install agent-hooks|skill|cli [--home DIR]`
  runs them headless. The Codex adapter reads its hook JSON with python3 (master used macOS plutil). Not detected: an
  unparseable `config.toml` (no TOML parser; a top-level `hooks` key is found line by line)
