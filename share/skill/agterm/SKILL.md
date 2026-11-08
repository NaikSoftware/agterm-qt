---
name: agterm
description: >
  Drive agterm-qt, a Qt/KDE terminal for Linux, via agtermctl. Use in an agterm-qt session:
  report agent status (active, blocked, completed) on the session's sidebar row; manage sessions, companions,
  workspaces and windows; split panes; run, hide or show a program overlay; post a passive HUD message;
  open a dashboard of live panes; show a picker, a question or an input dialog and wait for the answer;
  send messages between panes; read a pane's screen or scrollback; type into a session; notify the user;
  set a session's context line or row tint; reload the keymap or event hooks; run a custom command;
  subscribe to status, notify, lifecycle, selection, split, tree, message and launch events.
  Covers window/workspace/session addressing, AGTERM_* variables, and what agterm-qt does not support.
when_to_use: >
  Trigger on: agterm, agterm-qt, agtermctl, AGTERM_SESSION_ID, and, from inside a session, plain requests such as
  tell agterm I'm blocked or done, mark the session as needing input, split the pane, open a session for a worker,
  keep a related program next to this session, make a companion, duplicate this session as a companion,
  run this in an overlay and give me the exit code, close or hide the overlay, show a message over the session,
  show a spinner while you work, ask me a question, let me pick from a list, ask me to type a value,
  send a message to the other pane or another agent, read my messages, wait for a message,
  read what the other pane printed, search the scrollback, show the dashboard of my agents,
  flag every session in a workspace, tint a session or workspace row, show or hide the file panel,
  tell me when the selected session changes, wait until that worker finishes, bring back the session I just closed,
  run my custom command, reload the keymap or hooks, which commands agterm-qt lacks.
allowed-tools: Bash(agtermctl *)
---
<!-- agterm-skill -->

# Driving agterm-qt

agterm-qt is a terminal for Linux (Qt 6, KDE Frameworks, a Konsole terminal per pane) organised as named
workspaces of sessions. It exposes a control channel over a local unix socket, driven by the CLI `agtermctl`.
Use it to report what an agent is doing, build layouts, run programs in overlays, ask the user things,
pass messages between panes, read terminal text, and subscribe to control events.
Events do not stream terminal output; read a buffer with `session text`.

## Am I inside agterm-qt?

Every pane shell gets these variables; `AGTERM_SESSION_ID` being set is the test:

- `AGTERM_SESSION_ID`: the session this shell belongs to.
- `AGTERM_WORKSPACE_ID`, `AGTERM_WINDOW_ID`: the workspace and window it started in.
  The workspace id is not rewritten when the session moves; read `tree` for the current one.
- `AGTERM_PANE`: `main` or `right` (the split pane); an overlay's shell gets `overlay`.
- `AGTERM_PANE_ID`: a per-terminal token, the pane's identity for `msg`.
  A respawned shell gets a new one; overlays get none.
- `AGTERM_SOCKET`: the control socket this app bound.
- `AGTERMCTL`: the absolute path of the app's own `agtermctl`.

The app also prepends its binary directory to the pane's `PATH`.
Call plain `agtermctl`. If it reports an unknown command or flag that this skill documents, a login profile put an older
`agtermctl` first on PATH; `"$AGTERMCTL"` is the app's own binary.
Examples below write `agtermctl` for brevity.

The variables are inherited by everything the shell starts, including daemons that outlive it.
A tmux server or session manager started inside a session passes that session's `AGTERM_*` to every child,
so status hooks in those children report to the wrong session.
Scrub them before starting such a daemon:
`env -u AGTERM_SESSION_ID -u AGTERM_WORKSPACE_ID -u AGTERM_WINDOW_ID -u AGTERM_PANE -u AGTERM_PANE_ID -u AGTERM_SOCKET -u AGTERMCTL <cmd>`.

## Running agtermctl

- Socket: `--socket PATH`, else `$AGTERM_STATE_DIR/agterm.sock`, else `~/.local/share/agterm-qt/agterm.sock`.
  `agtermctl` never reads `AGTERM_SOCKET`, so pass `--socket "$AGTERM_SOCKET"` when you need the exact app
  that owns this pane (an instance started with its own `AGTERM_STATE_DIR`).
- Options may go anywhere; write them after the subcommand: `agtermctl tree --json`.
- `--json` prints the raw response line `{"ok":true,"result":{...}}` or `{"ok":false,"error":"..."}`.
  Without it, a failure prints `agtermctl: <error>` on stderr and exits 1; most mutations print nothing;
  create commands (`session new`, `session duplicate`, `workspace new`, `window new`, `recent open`) print the new id.
- One request per invocation, except `events`, `msg read --wait`, blocking `pick`/`ask` and
  `session overlay open --block`, which poll.
- A command for a feature agterm-qt lacks fails with `<cmd> is not supported on this platform (<reason>)`.
- `agtermctl --help` lists every command; reference.md has each one's arguments, errors and read-back.

## The model

A **window** holds a tree of **workspaces**, each holding **sessions**.
A session has a main pane and can have a **split** pane (side by side or top/bottom).
A **companion** is a session pinned under a parent: indented below it, moved and closed with it.
An **overlay** runs one program over the whole session, over one split pane, or in a floating panel, and closes
when the program exits.
A **HUD** is a passive message panel in the same session-wide slot; the session stays typable under it.
The **dashboard** shows up to nine live panes in a view-only grid.
Config lives in `~/.config/agterm-qt/` (or `$AGTERM_STATE_DIR/config`): `keymap.conf`, `hooks.conf`,
`restore-denylist.conf`. Terminal colours, font and scrollback come from the Konsole profile.

Inspect the live state with `agtermctl tree --json`. Each session node carries
`id`, `name`, `customName`, `cwd`, `title`, `active`, `status`, `blink`, `flagged`, `agent`, `unseen`, `parent`,
`hasSplit`, `split`, `splitAxis`, `splitRatio`, `splitFocused`, `fontSize`, `splitFontSize`, `overlay`,
and when set `context`, `collapsed`, `tint`/`tintOn`, `restoreCommand`/`splitRestoreCommand`,
`overlaySizePercent`, `overlayWidth`/`overlayHeight`, `paneOverlays`, `hiddenOverlays`, `hud`, `ask`.
Workspace nodes carry `id`, `name`, `rememberedFolder`, `active`, `sessions`, and when set `focused`,
`collapsed`, `tint`/`tintOn`.
The top level carries `workspaceFilter`, `sidebarMode`, `sidebarFlaggedLayout`, `sidebarVisible`, `sidebarWidth`,
`filePanel` (`{visible, width}`), `idleMs`, and when set `autoFollowMs`, `zoomedSurface`, `pickPending`,
`askPending` and the `dashboard*` fields.
`agtermctl session info --json` adds the live `pid`, `foreground` (process name), `foregroundPid`, `liveCwd`
and `profile` for one session.
`window list --json` reports each window's `id`, `name`, `open`, `active`, `geometry`, `fullscreen`, `zoomed`,
`minimized`, `sidebarVisible`.

## Addressing

- Session and workspace targets: `--target` takes a full id, a unique id prefix, an exact name, or `active`.
  No match is an error; an ambiguous prefix or name lists the candidates.
- Without `--target`, `session …`, `font …`, `notify` and `keymap run` address `$AGTERM_SESSION_ID`, so a
  command run in a pane acts on its own session. Outside a pane they fall back to `active`, the session the
  user has selected. A hook or custom command is outside a pane: pass `--target "$AGT_SESSION_ID"`.
- `ask` does not default to your session: pass `--target "$AGTERM_SESSION_ID"`.
- `window …` commands take the window id, prefix or `active` as a positional argument; other commands take
  `--window W` to address a window other than the frontmost. A session id finds its own window without it.
- Pane selectors accept `left` (aliases `primary`, `top`, `main`) and `right` (`split`, `bottom`).
- Scripts rarely type ids: capture what `*.new` prints, or act on `$AGTERM_SESSION_ID`.

## Reporting agent status

`agtermctl session status <idle|active|completed|blocked> [--blink] [--auto-reset]` sets the status dot on the
session's sidebar row; `blocked` and `completed` count as needing attention for `session go --to next-attention`
and the auto-follow setting. `--blink` pulses the dot; `--auto-reset` turns a `completed` back to idle when the user
selects the session. Typing into the session also clears `blocked`/`completed` (Settings ▸ Agent Status picks
first key, Enter, or never). Read back as the node's `status` and `blink`; each change emits a `status` event.

Prefer status over `notify` for "I need you": it stays on the row until acted on.
Keep `notify <body> [--title T]` for a one-off desktop notification; it also raises the row's unseen badge.

Claude Code: `scripts/install-claude-hooks.sh` (from the agterm-qt checkout) points Claude Code's hooks at the
installed adapter `~/.local/share/agterm-qt/agent-status/agterm-claude-status.sh`
(prompt submit and tool use → `active --blink`, stop → `completed --auto-reset`, permission prompt → `blocked`).
Any other agent can call `agterm-agent-status.sh <state> [--blink|--auto-reset]` from the same directory;
outside agterm-qt it is a silent no-op.

## Launching a program in a session

Bind the program at creation; the session runs it through `$SHELL -c` with the app's `PATH` and closes when it exits:

```bash
id=$(agtermctl session new --cwd ~/proj --name worker --no-select \
  --command 'claude "$(cat ~/brief.md)"')
```

`session type` drives an already-running program; it is not a launcher. Its text lands in a line buffer the
user and other agents share, and every newline submits. Use `--stdin` for text from a file or pipe.
A worker that reports status can be waited on through `events`. The stream replays nothing, so check the current
`status` first (`agtermctl session info --target "$id" --json | jq -r .result.session.status`) and wait only if it
is not there yet:

```bash
agtermctl events --kind status --json |
  jq -c --unbuffered --arg id "$id" 'select(.session == $id and .payload.status == "completed")' | head -n1
```

## Companions

`session new --parent SID [--name N] [--cwd DIR] [--command CMD] [--no-select]` makes a companion of SID:
listed right after it, indented, moved with it and closed with it (one undo restores the group).
Use one to keep a related program beside a session:
`agtermctl session new --parent "$AGTERM_SESSION_ID" --command 'tail -f build.log' --name logs --no-select`.
`--cwd` defaults to the parent's directory; workspace options are refused.
`session duplicate --companion` copies the session's split layout and each pane's program into a new companion.
`session collapse|expand` folds a parent's companions. Read back as the node's `parent` and `collapsed`.
A parent with companions whose shell exits gets a fresh shell; one that exits again within 2 s closes the group.

## Overlays and the HUD

`session overlay open <command> [--cwd DIR] [--wait | --block] [--size-percent N [--width PT] [--height PT]]
[--background-color #rrggbb] [--follow] [--pane left|right]` runs a program over a session:

- full session by default; `--size-percent` floats it in a centred panel; `--pane` covers one split pane only
  (a pane overlay is always full-pane, so it refuses `--size-percent`).
- it opens without switching the user to the session; `--follow` selects the session.
- `--wait` keeps the panel after exit until a key is pressed. `--block` makes the CLI wait and exit with the
  program's status (printing none of its output); the two are exclusive.
- `session overlay result` prints the exit status after it closes (`overlay still running` before);
  `session overlay text [--lines N]` reads its screen; `close`, `hide|show|toggle` (the program keeps running),
  and `resize (--size-percent N | --width/--height PT | --full)`.
- one program per slot: a second open answers `overlay already open` (or `overlay hidden`).

```bash
agtermctl session overlay open 'git log --oneline | fzf > /tmp/pick' --size-percent 80 --block && cat /tmp/pick
```

`session hud [open] <message> | --file F` posts a passive panel saying what you are doing; it takes no input.
Options: `--detail T` (dim second line), `--spinner` / `--spinner-style bar|braille|circle|blocks|dot|none`,
`--position` (nine anchors `top-left` … `bottom-right`, default `center`), `--markdown`, `--font-size PT`,
`--background-color`, `--text-color`, `--size-percent N` (width), `--sticky`, `--no-frame`, `--hide-after S`,
`--pane left|right`.
`session hud update <message> …` replaces the whole spec (repeat every option you want kept; font size and
background stay from open); `session hud close` removes it.
A program overlay replaces a HUD; a HUD is refused while a program holds the slot.
Give a HUD from a process that may die a `--hide-after` and refresh it, or it outlives the work it describes.
Read back as the node's `hud` object.

## Pick and ask

Both open a dialog, block until the user answers, print one JSON line, and exit by outcome.

`pick [--prompt T] [--query T] [--select ID] [--allow-custom] [--follow] [--no-block]` reads items from stdin:
lines (each line is id and label) or a JSON array of `{id,label,subtitle}`.
Stdin is always read; an itemless `--allow-custom` call needs `< /dev/null`.
Exit 0 for `{"result":"picked","id":…,"label":…,"index":N}` or `{"result":"custom","query":…}`, 2 for
`{"result":"cancelled"}`, 1 on error.

`ask <title> [--message T] --button ID=LABEL …` asks a question; `--input line|multiline` collects text and
`--choice ID=LABEL …` (`--multiple`, `--checked`, `--choice-subtitle`, `--other PH`) a selection; either
defaults the buttons to OK/Cancel. Exit 0 answered (inspect `.id`: a "No" button is an answer too, and
`.text`/`.selected` carry input), 2 cancelled, 3 escaped (Esc or closing the dialog).
Pass `--target "$AGTERM_SESSION_ID"` so the ask belongs to your session.

`--no-block` prints `{"id":…}` instead; `pick|ask result <id>` reads the outcome (exit 1 while pending) and
`pick|ask cancel <id>` cancels it. One picker per window; one terminal-style ask per session, one GUI ask per window.

```bash
env=$(printf 'staging\nproduction\n' | agtermctl pick --prompt 'Deploy to' | jq -er .id) || exit
agtermctl ask "Deploy to $env?" --button yes=Deploy --button no=Cancel --default no --target "$AGTERM_SESSION_ID" |
  jq -e '.id == "yes"' >/dev/null && ./deploy.sh "$env"
```

## Messages between panes

`msg` gives each pane a mailbox, so one program can leave text for another without typing into its terminal.

- `msg send <text>|--stdin --to SESSION:PANE [--to …]` or `--all [--workspace W]`. PANE is a pane id or a
  role (`left`/`right`, resolved at send time). `--all` reaches every pane in the window but the sender.
  Prints the message id; a rejected recipient (`unknown pane`, `mailbox full`) goes to stderr and exits 1.
- `msg read [--ack] [--wait [--timeout S]]` prints `ID from SENDER` then the body, oldest first (32 per read),
  and removes nothing unless `--ack`; `--timeout` exits 124.
- `msg ack <id>…` removes messages.
- Identity is `$AGTERM_SESSION_ID:$AGTERM_PANE_ID` (or `--as SESSION:PANE-ID`), never a role; a send without
  either is anonymous. Pane ids are not in `tree`: a receiver tells its own `$AGTERM_PANE_ID` to its peers, or
  senders address a role.
- Mail lives in memory and is lost when the app quits; a `message` event fires for each delivery.

## Reading and driving a pane

- `session text [--all | --lines N] [--pane left|right]`: the visible screen; `--all` adds the scrollback;
  `--lines N` the last N non-blank rows. Read the other pane with `--pane right`.
- `session type <text> | --stdin`: keystrokes into the main pane.
- `session copy` (the selection), `session paste`, `session select-all`.
- `session search [needle] [--next | --prev | --close]`: Konsole's search bar; selects the session and prints the
  match count.
- `session context <text> | --clear`: one line in the title bar saying what the session is about (a PR, a task).

## Events

`agtermctl events [--kind K]… [--run ID --after SEQ] [--limit N] [--json]` follows events from now (no history
without a cursor). Kinds: `status`, `notify`, `session.created` (with `parent` for a companion), `session.closed`,
`session.selected` (with `previous`), `tree.changed`, `pane.split`, `app.launched`, `message`
(`paneID`, `from`, `messageID`; never the body). `pane.focused`, `pane.scratch` and `remote.*` are accepted and
never fire. With `--json` each line is `{seq, ts, kind, window?, workspace?, session?, payload}`.
The stream keeps its own cursor; `--run ID --after SEQ` resumes from one (the run id is `result.events.run` of the
raw `events.read` reply, see reference.md). The app keeps 4096 events per run; `event run changed` and
`event cursor expired` are fatal. `hooks.conf` runs a shell line on any kind (`on <kind> <shell>`).

## Layout, windows and navigation

- Splits: `session split [on|off|toggle] [--axis vertical|horizontal]` (off hides, the shell lives),
  `session split close`, `session swap`, `session focus [left|right|other]`, `session resize --split-ratio R`.
- Navigation: `session go --to next|prev|first|last|next-attention|prev-attention`, `workspace go --to next|prev`,
  `session select`, `workspace select`, `recent list|open [id]|clear`.
- Sidebar: `sidebar [show|hide|toggle]`, `sidebar mode tree|flagged`, `sidebar flagged-layout flat|plain|tree`,
  `sidebar expand|collapse`, `sidebar width`; `filepanel [show|hide|toggle]`, `filepanel width`.
- Working sets: `session flag`, `workspace flag`, `workspace focus on|off|toggle|add`, `workspace filter`,
  `session tint|workspace tint <red|orange|yellow|green|blue|purple|gray|clear> [--on icon,name,background]`.
- `surface zoom [show|hide|toggle] [--target surface:<session>:left|right]`,
  `dashboard <id[:left|:right]>… | --mru [--font-size N | --auto-size] | --close`.
- Windows: `window new|list|select|go|close|rename|delete|resize|move|zoom|fullscreen|minimize`.
  On Wayland `window move` is refused and `window list` omits `x`/`y`.
- `font inc|dec|reset [--pane left|right]`, `theme list|set [name]` (Konsole colour schemes).

## Restore

Settings ▸ General ▸ Restore sessions, also `restore mode [none|rerun|live]`, applies from the next launch.
`none` restores windows, workspaces, sessions, directories and splits with fresh shells; `rerun` also types each
pane's captured foreground command into its new shell once. `restore capture` records commands now (rerun mode),
`restore clear` forgets them, and `session restore <command> | --none | --clear [--pane left|right]` pins what a pane
re-runs (read back as `restoreCommand`/`splitRestoreCommand`; the pin is stored shell code, so no secrets).
`restore-denylist.conf` lists programs never re-run. `live` keeps every pane's shell running in an
`agterm-session-host` daemon across quits and reattaches it at launch with its recent output; a daemon that is gone
(after a reboot) falls back to re-running the captured command. Deleting a session or window ends its daemons; a
`none` or `rerun` launch ends them all.

## Not in agterm-qt

These master agterm features are absent; their commands answer `<cmd> is not supported on this platform (<reason>)`:

- quick terminal (`quick …`) and scratch terminal (`session scratch`);
- every `zmx …` command (live sessions use their own daemon), remote sessions and pane leads (`session lead`), `session restart`;
- `session background` (Konsole takes backgrounds from the profile), `surface cursor`;
- `config reload` (no Ghostty config; Konsole profiles instead);
- the built-in browser (`browser …`), page overlays (`session overlay open --html|--url`,
  `overlay reload|navigate|submit|copy`) and page companions: they need Qt WebEngine.
  There is no inline image, rendered markdown or file-at-line view either; do not pass master's `--image`,
  `--markdown` or `--text` to `overlay open` (`--markdown FILE` would run FILE as the command).
  To show a file, run a viewer as the overlay program (`less +N file`, `glow file`).

Also absent from the CLI: `session wait` (use `events`), `clipboard set` (use `wl-copy`/`xclip`), `terminfo`,
`session info` as an activity line (here it prints the session's live process details), per-call status colour,
shape, sound and pane, and `session new --wait|--after|--before`.

## Keys

agterm's ⌘ is Ctrl+Shift here (Konsole's convention); ⌘⇧ and ⌘⌃ are Ctrl+Alt+Shift; ⌃ stays Ctrl; ⌥ is Alt.
`keymap.conf` reads agterm's `cmd`/`opt` words by the same table.

| Key | Action |
| --- | --- |
| Ctrl+Shift+N / Ctrl+Alt+Shift+N | New session / new workspace |
| Ctrl+Alt+N | New window |
| Ctrl+Shift+O | Open directory as a session |
| Ctrl+Shift+W / Ctrl+Shift+Z | Close session / undo the close within 3 s |
| Ctrl+Alt+Shift+T | Reopen last closed session |
| Ctrl+Shift+D / Ctrl+Alt+Shift+D | Split side by side / top and bottom |
| Ctrl+Alt+Shift+← / → | Focus left / right pane |
| Ctrl+Alt+Shift+Return / F11 | Terminal zoom / full screen |
| Ctrl+Alt+Shift+S / Ctrl+Alt+Shift+E | Toggle sidebar / file panel |
| Ctrl+Alt+Shift+↑ / ↓ | Previous / next session |
| Ctrl+Alt+↑ / ↓ | Previous / next session needing attention |
| Ctrl+P / Ctrl+Shift+P / Ctrl+Tab | Session palette / action palette / recent sessions |
| Ctrl+Alt+O | Custom-commands palette |
| Ctrl+Alt+Shift+F | Flag session |
| Ctrl+Alt+Shift+J | Hide / show overlays |
| Ctrl+Alt+Shift+G | Dashboard of recent sessions |
| Ctrl+Shift+F | Search the scrollback |

`agtermctl keymap list` prints what each built-in and custom command is bound to right now.

## Troubleshooting

- `cannot connect to …/agterm.sock`: the app is not running on that state directory; check `--socket`/`AGTERM_STATE_DIR`.
- A keymap or hooks line that does nothing: `keymap list` / `hooks list` print the parse diagnostics and each
  hook's running pid and last failure; `keymap reload` / `hooks reload` re-read the file.
- Status landing on the wrong session: a daemon inherited another session's `AGTERM_*` (see above).

## Reference files

- **reference.md**: every command's syntax, arguments, errors and read-back, plus the `keymap.conf` and
  `hooks.conf` formats.
