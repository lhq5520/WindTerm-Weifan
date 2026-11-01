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

These commands have no default shortcut (assign one in *Settings ▸ Keyboard*): *New Telnet
Session*, *New Serial Session* (File menu), *Send Break*, *Port Forwarding*, *Log Session to File*,
*Broadcast Input to All Panes of the Tab* and *Broadcast Input to All Tabs* (Session menu).

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
In the Flatpak build local sessions run on the host (`flatpak-spawn --host`), so your own shell and
tools are used; set `WT_FLATPAK_SANDBOX_SHELL=1` to keep them inside the sandbox.

**Telnet sessions:** host, port (23), terminal type (default `xterm-256color`), local echo
(*Automatic* follows the server's ECHO option), whether Enter is sent as CR LF or CR NUL,
environment variables sent with NEW-ENVIRON (e.g. `USER`), connect timeout and a SOCKS5 / HTTP
CONNECT proxy. The window size is reported with NAWS.

**Serial sessions:** device (`/dev/ttyUSB0`, `/dev/cu.usbserial-*`, `COM3`), baud rate (any rate the
driver accepts, including non-standard ones), data bits, parity, stop bits, flow control
(none, XON/XOFF, RTS/CTS, DSR/DTR), local echo and what Enter sends (CR, LF or CR LF). The device is
opened exclusively and reopened automatically when a USB adapter is unplugged and plugged in again.
*Session ▸ Send Break* sends a BREAK (also on Telnet).

**SSH sessions:**

* Host, port, user (defaults to your local user name), remote command (empty = shell),
  keep-alive interval, connect timeout.
* Authentication: *Automatic*, password, public key (identity file; encrypted keys are
  supported), SSH agent (`SSH_AUTH_SOCK`, Pageant/OpenSSH agent on Windows) or
  keyboard-interactive.
* **Certificates:** an OpenSSH user certificate next to the key (`id_ed25519-cert.pub`) is used
  automatically; another file can be set as *Certificate*.
* **Security keys** (FIDO/U2F, `sk-ssh-ed25519` / `sk-ecdsa`): add the key to ssh-agent
  (`ssh-add ~/.ssh/id_ed25519_sk`) and use agent authentication; the terminal asks you to touch the
  key while logging in.
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
type a path), upload (button, or drop files and folders from your file manager), download files
and whole folders, create folders, rename and delete (folders recursively). Transfers show
progress and can be cancelled.

## Port forwarding, agent and X11 forwarding

The *Forwarding* tab of an SSH session holds its port forwards; *Session ▸ Port Forwarding* shows
the forwards of the active connection with their state and lets you add or remove forwards while
connected.

| Kind | Listens on | Connects to |
|---|---|---|
| Local (`-L`) | this computer (default `127.0.0.1`) | a host and port as seen from the server |
| Remote (`-R`) | the server (default `localhost` there) | a host and port as seen from this computer |
| Dynamic (`-D`) | this computer, as a SOCKS4/4a/5 proxy | wherever the client asks, through the server |

Use `*` as the bind address to listen on all interfaces (other computers can then use the forward).
*Forward the SSH agent* lets programs on the server use your local agent; *Forward X11* shows
remote X11 programs on your display (`$DISPLAY`, or the display set in the session). Both give the
server's administrators access to what they forward: enable them only for hosts you trust.

## Session logs

*Session ▸ Log Session to File* starts or stops logging the active pane;
*Settings ▸ Logging & Triggers ▸ Log every session from its start* logs all sessions. Logs go to
`Documents/WindTerm-Weifan/Logs` unless another folder is set. The file name pattern understands
`%Y %m %d %H %M %S` (date and time), `%N` (session name), `%h` (host) and `%%`; the default is
`%Y-%m-%d_%H-%M-%S_%N.log`. *Plain text* logs contain the text as it appeared (lines rewritten with
CR or backspaces are logged in their final form), optionally with the time at the start of every
line; *Raw* logs contain everything received, escape sequences included (replay them with `cat`).

## Triggers

Triggers (*Settings ▸ Logging & Triggers*) run a regular expression (PCRE syntax) over every line
of output, including the line being written, so prompts can trigger them. Actions:

* **Highlight** the match with a foreground and/or background colour;
* **Notify**: the trigger's text (or the line) appears in the status bar, the tab is marked and a
  window in the background asks for attention (taskbar);
* **Beep**;
* **Send** text back, e.g. to answer a prompt (`\r`, `\n`, `\t`, `\e` and `\\` escapes).

A trigger fires at most once per line. In `settings.json`:

```json
"triggers": [
  { "name": "errors", "pattern": "\\b(error|failed)\\b", "action": "highlight", "foreground": "#ffffff", "background": "#aa0000" },
  { "name": "continue", "pattern": "Continue\\? \\[y/N\\]", "action": "send", "text": "y\\r" }
]
```

Do not put passwords into *Send* triggers: `settings.json` is not encrypted.

## Command completion

While you type at a shell prompt, commands from your history that start with the typed text are
suggested once at least two characters are typed (*Settings ▸ Logging & Triggers ▸ Completion*).
Up/Down choose a suggestion, Right, Enter or Tab insert it, Esc closes the list. The history is
learned from commands you run (`<config>/history.json`, shared by all sessions, with the commands
of the same session suggested first); only commands the
shell echoed are recorded, so input typed at password prompts never is. Prompts marked with OSC 133
(shell integration of fish, zsh/bash plugins) are recognised exactly; otherwise the typing is
tracked from the keys pressed.

## Broadcasting input

*Session ▸ Broadcast Input to All Panes of the Tab* sends what you type in the active pane to every
pane of the tab as well; *… to All Tabs* to every pane of the window. Typing, pasting and completion
are broadcast; the status bar shows that broadcasting is on.

## Images

Programs can show images in the terminal with **Sixel** (`img2sixel`, `chafa -f sixels`, `lsix`,
gnuplot's sixel terminal) or the **kitty graphics protocol** (`kitten icat`, `timg`, `chafa -f kitty`,
yazi, ranger). Images scroll with the text and scale with the font. Kitty images must be sent
directly in the escape sequence (`t=d`, what tools use over SSH); requests to read local files or
shared memory are refused, as are animations.

## Credentials and the vault

*Settings ▸ SSH & Security ▸ Remember passwords in* chooses between the encrypted vault (one master
password) and the system keychain (macOS Keychain, Windows Credential Manager, or the Secret
Service — GNOME Keyring, KWallet — on Linux). The vault locks itself after 30 minutes without use
(*Lock the vault when idle for*, or *Never*) and asks for the master password again when a secret
is needed.

## Restoring the window layout

On exit the window, its tabs, the split layout and the session of every pane are saved to
`<config>/workspace.json` and reopened on the next start (local panes in their last working
directory; SSH, Telnet and serial panes reconnect). Turn this off with *Settings ▸ SSH & Security ▸
Reopen the last windows, tabs and sessions on start*, or start with a command/`--profile`, which
opens just that.

## Language

*Settings ▸ Appearance ▸ Language* switches between English and Simplified Chinese (or the system
language) after a restart. `WT_LANGUAGE=zh_CN` (or `en`) overrides it for one run.

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
| `logging.autoStart` | false | log every session from its start |
| `logging.directory` | `Documents/WindTerm-Weifan/Logs` | log folder |
| `logging.fileName` | `%Y-%m-%d_%H-%M-%S_%N.log` | log file name pattern |
| `logging.format` / `timestamps` | `text` / true | `text` or `raw`; time at the start of text lines |
| `triggers` | [] | trigger rules (see above) |
| `completion.enabled` / `minimumPrefix` | true / 2 | command suggestions from the history |
| `security.credentialStore` | `vault` | `vault` or `system` |
| `security.vaultLockMinutes` | 30 | lock the vault after this many idle minutes (0 = never) |
| `restoreWorkspace` | true | reopen the last windows, tabs and sessions |
| `language` | "" (system) | `en` or `zh_CN` |
