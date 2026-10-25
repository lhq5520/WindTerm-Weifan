# User guide

## Starting

```
WindTerm-Weifan [options] [-- command [args...]]

  --cwd <dir>           start the first shell in <dir>
  --login               run the shell as a login shell (default on macOS)
  --hold                keep panes open after their program exits
  --geometry COLSxROWS  initial window size (default 80x24)
  --profile <id>        open a saved session instead of a local shell
  --config-dir <dir>    read/write settings, sessions and the vault in <dir>
  --no-config           use defaults and save nothing
```

Configuration lives in `~/.config/WindTerm-Weifan` (Linux), `~/Library/Preferences/WindTerm-Weifan`
(macOS) or `%LOCALAPPDATA%\WindTerm-Weifan` (Windows): `settings.json`, `sessions.json` and, once
used, `vault.json`. Problems in these files are reported on startup and replaced by defaults.

## Tabs and split panes

Every tab holds one or more terminals arranged in splits. A local pane closes when its program
exits (unless `--hold`), a tab closes with its last pane and the window with its last tab. New
local panes start in the working directory the current shell reported (OSC 7). Splitting an SSH
pane opens a second connection to the same host.

Default shortcuts (change them in *Settings ▸ Keyboard*; on macOS "⌘" is Command):

| Command | Linux / Windows | macOS |
|---|---|---|
| New tab | Ctrl+Shift+T | ⌘T |
| Close pane | Ctrl+Shift+W | ⌘W |
| Close tab | Ctrl+Shift+Q | ⌘⇧W |
| Next / previous tab | Ctrl+Tab, Ctrl+PgDown / Ctrl+Shift+Tab, Ctrl+PgUp | ⌘⇧] / ⌘⇧[ |
| Split right / down | Ctrl+Shift+E / Ctrl+Shift+O | ⌘D / ⌘⇧D |
| Next / previous pane | Ctrl+Shift+Right / Ctrl+Shift+Left | ⌘] / ⌘[ |
| Find, next, previous | Ctrl+Shift+F, F3, Shift+F3 | ⌘F, ⌘G, ⌘⇧G |
| Copy / paste / select all | Ctrl+Shift+C / Ctrl+Shift+V / Ctrl+Shift+A | ⌘C / ⌘V / ⌘A |
| Clear scrollback | Ctrl+Shift+K | ⌘K |
| Zoom in / out / reset | Ctrl+= / Ctrl+- / Ctrl+0 (or Ctrl+wheel) | ⌘= / ⌘- / ⌘0 |
| New SSH session | Ctrl+Shift+N | ⌘⇧N |
| Session manager | Ctrl+Shift+M | ⌘⇧O |
| Settings | Ctrl+, | ⌘, |
| SFTP file browser | Ctrl+Shift+B | ⌘⇧B |
| Reconnect | Ctrl+Shift+R | ⌘⇧R |

Also available: Shift+PgUp/PgDown/Home/End scroll the history, Shift+Insert / Ctrl+Insert paste
and copy (Linux/Windows), middle click pastes the primary selection (X11).

## Selection, links and the clipboard

* Drag to select, double-click for a word, triple-click for a line, Alt+drag for a block,
  Shift+click to extend. Selections follow the text when the window is resized.
* *Copy on select* (Settings ▸ Terminal) copies every selection to the clipboard.
* URLs and OSC 8 hyperlinks are underlined on hover; Ctrl+click opens them (the context menu also
  offers *Open Link* and *Copy Link Address*).
* Programs may set the clipboard with OSC 52 only if *Allow programs to set the clipboard* is on.

## Search

Ctrl+Shift+F (⌘F) opens the find bar above the terminal. It searches the scrollback and the
screen, joining wrapped lines, and highlights all matches. Options: **Aa** match case, **W** whole
words, **.\*** regular expression (PCRE syntax). Enter goes to the previous (older) match,
Shift+Enter to the next one, Esc closes the bar. Results update while new output arrives.

## Appearance

*View ▸ Theme* switches between the built-in themes (WindTerm Dark, Campbell, Solarized Dark/Light,
Dracula, Monokai, Gruvbox Dark, One Dark, Nord, Tango Light, xterm). Settings ▸ Appearance sets the
font, size, cursor style and blinking. A session can override the theme. Custom themes go into
`settings.json`:

```json
"customThemes": [
  { "name": "My Theme", "dark": true, "foreground": "#c0c0c0", "background": "#101010",
    "cursor": "#ffcc00", "selection": "#334455",
    "ansi": ["#000000", "#cc0000", "#00cc00", "#cccc00", "#0000cc", "#cc00cc", "#00cccc", "#cccccc",
             "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff"] }
]
```

Key bindings are stored as overrides of the defaults, e.g.
`"keybindings": { "new-tab": ["Alt+T"], "reconnect": [] }` (action ids are listed in
`libs/session/src/Keymap.cpp`).

## Sessions

*File ▸ Session Manager* lists saved sessions by folder; *Open Saved Session* in the Session menu
opens them directly, and `--profile <id>` from the command line.

**Local sessions:** program, arguments, working directory, login shell, environment variables.

**SSH sessions:**

* Host, port, user (defaults to your local user name), remote command (empty = shell),
  keep-alive interval, connect timeout.
* Authentication: *Automatic*, password, public key (identity file; encrypted keys are
  supported), SSH agent (`SSH_AUTH_SOCK`, Pageant/OpenSSH agent on Windows) or
  keyboard-interactive.
* Password / passphrase: either asked for when connecting, or stored in the **credential vault**
  ("Store in the encrypted credential vault"). The vault is created with a master password the
  first time it is needed and asked for once per run. See [SECURITY.md](SECURITY.md).
* Host keys: unknown keys are confirmed once and saved to `known_hosts`; changed keys produce a
  warning (details in [SECURITY.md](SECURITY.md)).
* *Proxy & Reconnect* tab: connect directly, through a SOCKS5 or HTTP CONNECT proxy (optional user
  name; the proxy password can be a vault entry), or through a **jump host** — another saved SSH
  session (chains of jump hosts work; cycles are rejected). Automatic reconnect retries a dropped
  connection with exponential backoff (1 s, 2 s, 4 s … up to 30 s, 5 attempts by default); the
  terminal shows each attempt. *Session ▸ Reconnect* reconnects manually after a session ended.

## SFTP file browser

*Session ▸ File Browser (SFTP)* opens a panel for the active SSH pane. It uses its own connection
with the credentials that already worked for the terminal. You can browse (double-click folders,
type a path), upload (button, or drop files from your file manager), download, create folders,
rename and delete (folders recursively). Transfers show progress and can be cancelled.

## Settings reference (`settings.json`)

| Key | Default | Meaning |
|---|---|---|
| `terminal.fontFamily` / `fontSize` | system monospace / platform default | font |
| `terminal.theme` | `WindTerm Dark` | colour theme |
| `terminal.scrollbackLines` | 10000 | history length (0–1 000 000) |
| `terminal.cursorStyle` / `cursorBlink` | `block` / false | `block`, `underline`, `bar` |
| `terminal.copyOnSelect` | false | copy selections automatically |
| `terminal.allowClipboardWrite` | false | allow OSC 52 clipboard writes |
| `terminal.optionAsMeta` | false | macOS: Option sends ESC-prefixed keys |
| `terminal.bell` | `visual` | `visual`, `audible`, `none` |
| `confirmCloseRunning` | true | ask before closing windows with running sessions |
| `knownHostsFile` | `~/.ssh/known_hosts` | host key store |
| `keybindings` | {} | overrides of the default shortcuts |
| `customThemes` | [] | additional themes |
