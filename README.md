# agterm-qt

A terminal for Linux/KDE organised as named workspaces of sessions, with a control API so coding agents and scripts
can drive it and report whether they are working, blocked, or done.

agterm-qt is a Qt6/KDE Frameworks 6 implementation of [agterm](https://github.com/umputun/agterm) by
[@umputun](https://github.com/umputun), a native macOS terminal. It follows agterm's model, keys (translated to Linux
conventions), config files, agent-status hooks and `agtermctl` wire protocol, including the features of the
[NaikSoftware/agterm](https://github.com/NaikSoftware/agterm) fork. It is an independent project: report bugs here,
not upstream. [docs/PARITY.md](docs/PARITY.md) tracks what is ported and what is not yet (web-page overlays,
HTML companions, `browser.*` and remote sessions are missing).

Each terminal is a Konsole KPart, so Konsole's profiles, colour schemes, scrollback, search and copy/paste work as
in Konsole.

## Build and install

Needs Qt 6 (Widgets, Network, DBus, Svg), KDE Frameworks 6 (CoreAddons, XmlGui, Config, Notifications, KParts) and
`konsole-kpart`. Install `wl-clipboard` (or `xclip` on X11) too: agents like Claude Code read a pasted image
(Ctrl+V) through `wl-paste`/`xclip`, and without them the paste does nothing, in agterm-qt and plain Konsole alike.

```sh
sudo apt install libkf6parts-dev   # or, without root: scripts/fetch-kparts-headers.sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=~/.local   # the menu entry gets an absolute Exec
cmake --build build -j
ctest --test-dir build              # core, file panel, offscreen UI tests
scripts/smoke.sh                    # end-to-end over the control socket
cmake --install build && kbuildsycoca6 --noincremental
```

## What it does

- Workspaces of sessions in a sidebar. A workspace remembers the folder its sessions last worked in, and new
  sessions open there. Its "+" (on hover) starts Terminal, Claude Code, Codex, OpenCode or Antigravity (whichever
  your login shell finds), and the button beside it repeats the last choice.
- A session row shows the logo of the agent running in it, its status dot (active, blocked, completed), an unseen
  notification count, and a filled icon when flagged. Companions sit indented under their parent; Duplicate as
  Companion copies a session's split layout and the program in each pane.
- Tint a session or workspace row (row menu ▸ Tint: seven colours, on the icon, the name, or both).
- Rename inline (double-click), multi-select, drag to reorder or move between workspaces, and drop folders from the
  file manager to open sessions there. The footer has New Session, New Workspace, the workspace filter and the
  flagged view. Mark workspaces with Focus / Add to Focus in their row menu; the grid button applies or suspends
  the filter, and selecting a session outside it suspends it.
- The flagged view is a flat list (with or without workspace names) or a workspace tree (Settings ▸ General); View ▸
  Edit Flagged Sessions… and Edit Focused Workspaces… tick rows on and off.
- Auto-follow (Settings ▸ Agent Status) moves to the oldest blocked session once you have been idle for a chosen time.
- Splits, terminal zoom, full screen.
- Session (Ctrl+P) and action (Ctrl+Shift+P) palettes, a Ctrl-Tab recent-session switcher and its title-bar clock menu, which also reopens recently closed sessions (a Claude Code one resumes its conversation)
  button, undo of a close for 3 seconds, and reopen-closed.
- A file panel (the session folder as a tree with file-type icons, a built-in viewer, clipboard, drag and drop).
- Settings (Ctrl+Shift+,) with agterm's six pages; terminal font, colour scheme and cursor apply to every open
  terminal on top of the Konsole profile. View ▸ Select Theme… previews schemes live (Enter keeps, Esc reverts).
- `keymap.conf` and `hooks.conf` in `~/.config/agterm-qt/` (or `$AGTERM_STATE_DIR/config`): rebind built-ins, add custom
  shell commands with `{AGT_*}` tokens (Ctrl+Alt+O lists them), and run scripts on control events. Both files start
  as commented references; File ▸ Edit Keymap… / Reload Keymap and the Hooks pair apply them.
- Programs in a pane can raise notifications with OSC 9 or OSC 777 (`printf '\e]9;done\a'`).
- Several windows, each with its own workspaces (File ▸ New Window, Open Window); the ones open at quit come back.
- A dashboard (Ctrl+Alt+Shift+G, the title-bar grid button, or `agtermctl dashboard <ids>`) shows up to nine live panes side by side, view-only,
  with each session's name in its status colour; Enter or a click opens one.
- Restore sessions (Settings ▸ General): fresh shells; re-run commands, which records what each pane runs at quit
  and types it into its new shell on the next launch (`restore-denylist.conf` lists programs to leave out); or live
  sessions, where each pane's shell keeps running in an `agterm-session-host` daemon and the next launch reattaches it.
- Overlays: `agtermctl session overlay open <command>` runs a program in a temporary terminal over a session (full,
  floating with `--size-percent`, or one split pane with `--pane`); it closes when the program exits and
  `overlay result` (or `--block`) returns the status. Ctrl+Alt+Shift+J hides and shows them. `session hud` puts a
  passive message panel over a session that stays typable; keymap `--error-hud` commands report failures in one.

## Keys

agterm's ⌘ is Ctrl+Shift here (Konsole's convention); ⌘⇧ and ⌘⌃ are Ctrl+Alt+Shift; ⌃ stays Ctrl; ⌥ is Alt.
`keymap.conf` reads agterm's `cmd`/`opt` words by the same table (`cmd` alone = ctrl+shift, `cmd` with another modifier =
ctrl+alt+shift), next to plain `ctrl`, `shift`, `alt` and `super`.

| Key | Action |
| --- | --- |
| Ctrl+Shift+N / Ctrl+Alt+Shift+N | New session / new workspace |
| Ctrl+Alt+N | New window (agterm's ⌥⌘N; its literal mapping is taken by New Workspace) |
| Ctrl+Shift+O | Open directory as a session |
| Ctrl+Shift+W | Close session (leaves terminal zoom first) |
| Ctrl+Shift+Z | Undo a close within its 3 s grace (Settings can turn the grace off) |
| Ctrl+Alt+Shift+T | Reopen last closed item |
| Ctrl+Shift+= / - / 0 | Terminal font size larger / smaller / reset |
| Ctrl+Shift+D / Ctrl+Alt+Shift+D | Split side by side / top and bottom (again: hide; the other: transpose) |
| Ctrl+Alt+Shift+← / → | Focus left / right pane |
| Ctrl+Alt+Shift+Return | Terminal zoom |
| F11 | Full screen |
| Ctrl+Alt+Shift+S / Ctrl+Alt+Shift+E | Toggle sidebar / file panel |
| Ctrl+Alt+Shift+↑ / ↓ | Previous / next session |
| Ctrl+Alt+↑ / ↓ | Previous / next session needing attention |
| Ctrl+P / Ctrl+Shift+P / Ctrl+Tab | Session palette / action palette / recent sessions |
| Ctrl+Alt+O | Custom-commands palette (agterm's ⌃⇧O; Ctrl+Shift+O is Open Directory here) |
| Ctrl+Alt+Shift+F | Flag session |
| Ctrl+Alt+Shift+J | Hide / show overlays |
| Ctrl+Alt+Shift+G | Dashboard: recent sessions' live panes in a grid (arrows, Enter, Esc) |

## Agent status, skill and CLI

Help ▸ Install Agent Status Hooks… copies the status package to `~/.config/agterm-qt/agent-status` and wires it into
Claude Code (`~/.claude/settings.json`), Codex (`~/.codex/config.toml`), Pi, OpenCode and your shell rc files; each file
it edits is backed up as `.bak`, and running it again changes nothing. Help ▸ Install Agent Skill… copies the agent
skill (which teaches Claude Code and Codex to drive agterm-qt) into `~/.claude/skills` and `~/.codex/skills`, and
Install Command Line Tool… links `agtermctl` into `~/.local/bin`. The first launch offers the skill and the hooks.
From a shell: `agterm-qt --install agent-hooks|skill|cli [--home DIR]` (`scripts/install-claude-hooks.sh` runs the first).

The hooks run agterm's own status adapter (a no-op outside agterm-qt). Typing into a blocked or completed session
clears its dot (Settings sets on the first key, on Enter, or never).

## Control API

Every pane exports `AGTERM_WINDOW_ID`, `AGTERM_SESSION_ID`, `AGTERM_WORKSPACE_ID`, `AGTERM_PANE`, `AGTERM_PANE_ID`, `AGTERM_SOCKET`
and `AGTERMCTL`, so
`agtermctl` inside a pane addresses its own session. Targets are an id, a unique id prefix, an exact name, or
`active`; `--window` picks a window other than the frontmost. `agtermctl --help` lists the commands; the wire format is agterm's (one JSON request line, one response
line) on `<state dir>/agterm.sock`. The state dir is `~/.local/share/agterm-qt`, or `AGTERM_STATE_DIR`.
Beyond sessions and workspaces it streams events (`agtermctl events`), shows pickers and questions (`pick`, `ask`),
passes messages between panes (`msg`, addressed by `AGTERM_PANE_ID`), and reads a pane's screen or scrollback
(`session text --all`). Commands for features agterm-qt lacks answer "not supported on this platform".

## Credits and license

The design, the control model and the agent-status adapters come from [agterm](https://github.com/umputun/agterm)
(MIT, © Umputun). Terminal emulation is [Konsole](https://konsole.kde.org)'s KPart. MIT licensed, see [LICENSE](LICENSE).
