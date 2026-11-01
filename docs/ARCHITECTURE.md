# Architecture

WindTerm-Weifan is a WindTerm-inspired terminal emulator and SSH/SFTP client rebuilt on a modern,
portable base (C++20, CMake, Qt 6) with first-class support for macOS on Apple Silicon, Linux and
Windows.

The upstream WindTerm repository is only *partially* open source: it ships some components
(Pty, Telnet, a few widgets, forks of libssh and Onigmo) but no application, GUI or build
system. This project therefore builds a new application around clean layers and treats the
legacy `src/` tree as reference material.

## Layers

```
           ┌────────────────────────────────────────────────────────────────────────┐
  app/     │ Qt 6 Widgets front end                                                  │
           │  main.cpp         CLI, AppContext (settings, sessions, vault)           │
           │  MainWindow       tabs, actions/menus from the keymap, SFTP dock        │
           │  TerminalTab      split panes (nested QSplitters)                       │
           │  TerminalPane     TerminalWidget + SearchBar + TerminalSession          │
           │  TerminalWidget   rendering, keyboard/IME/mouse, selection, links       │
           │  TerminalSession  Terminal model <-> SessionBackend                     │
           │  PtyBackend       local process (QSocketNotifier / QWinEventNotifier)   │
           │  TelnetBackend    Telnet over TCP/proxy (QSocketNotifier, reconnect)    │
           │  SerialBackend    serial port (notifier, reopen when unplugged)         │
           │  ssh/SshBackend   remote shell (worker connect, reconnect with backoff) │
           │  ssh/SftpPanel    file browser (own connection, worker thread)          │
           │  ssh/ForwardingDialog  live port forwards of a connection               │
           │  Triggers         compiled trigger rules, TriggerRunner per session     │
           │  credentials/     Secret Service (QtDBus), system store / vault switch  │
           │  Translations     .qm loading, translated action titles                 │
           │  Settings/Session dialogs, SshPrompter (host keys, passwords)           │
           └───────┬───────────────┬───────────────┬──────────────┬─────────────────┘
                   │               │               │              │
  libs/    ┌───────▼──────┐ ┌──────▼──────┐ ┌──────▼─────┐ ┌──────▼────────┐
           │ wt::terminal │ │ wt::session │ │ wt::ssh    │ │ wt::security  │
           │ VtParser     │ │ Theme       │ │ Connection │ │ Argon2id      │
           │ Terminal     │ │ Keymap      │ │ KnownHosts │ │ ChaCha20-     │
           │ Search       │ │ Settings    │ │ SshShell   │ │  Poly1305     │
           │ Key/Mouse    │ │ Profiles,   │ │ Sftp       │ │ CredentialVault│
           │  encoders    │ │  Store,     │ │ Forwarding │ │ (OpenSSL 3.2+ │
           │ Sixel,       │ │  Backoff    │ │ X11, Agent │ │  or libsodium)│
           │ KittyGraphics│ │ SessionLog  │ │ (libssh2)  │ │               │
           │              │ │ History,    │ │            │ │               │
           │              │ │ Workspace   │ │            │ │               │
           └───────┬──────┘ └──────┬──────┘ └──┬─────┬───┘ └──────┬────────┘
                   │               │           │ ┌───▼──────┐     │
                   │               │           │ │ wt::net  │     │
                   │               │           │ │ TCP,     │     │
                   │               │           │ │ SOCKS5,  │     │
                   │               │           │ │ HTTP,    │     │
                   │               │           │ │ Telnet,  │     │
                   │               │           │ │ SOCKS    │     │
                   │               │           │ │ server   │     │
                   │               │           │ └───┬──────┘     │
           ┌───────▼───────────────▼───────────▼─────▼────────────▼───────┐
           │ wt::platform  Pty (POSIX openpt, Windows ConPTY), SerialPort, │
           │               Keychain (macOS, Windows), Files                │
           │ wt::core      Status, UTF-8, char width, base64/hex, JSON     │
           └────────────────────────────────────────────────────────────────┘
```

Rules:

* `libs/**` must not depend on Qt. This is enforced by the `wt_qt_free_libs` CTest, which
  scans the sources for Qt includes/macros. It keeps the emulator, the SSH client and the
  credential vault testable without a display and reusable from other front ends.
* Dependencies point downwards only. `wt::ssh` and `wt::security` are optional (they need
  libssh2 and OpenSSL ≥ 3.2 / libsodium); the app compiles their integration only when they are
  available (`WT_HAVE_SSH`, `WT_HAVE_VAULT`).
* OS-specific code lives in `wt::platform` (`src/posix`, `src/win`), `wt::net` (sockets) and a few
  clearly marked `#ifdef`s in the app (macOS modifier mapping, Windows PTY notifier).

## Threading model

| Thread | Work |
|---|---|
| GUI thread | Qt widgets, terminal model, PTY I/O (non-blocking, socket/event notifiers), all dialogs |
| SSH connect worker (one per attempt) | TCP/proxy connect, jump-host chain, handshake, host key check, authentication, opening the shell channel. Questions go to the GUI thread through `GuiInteraction` (promise/future, cancellable). Joined when the backend is destroyed; `ssh::Aborter` shuts the socket down to unblock it. |
| SSH shell I/O thread (`wt::ssh::SshShell`) | non-blocking libssh2 channel: reads, writes, window-change, keep-alives; `poll()` on the socket and a wake-up socket pair. Data and close events are posted to the GUI thread. The same thread runs the `ForwardingEngine`: local/dynamic listeners, remote forward listeners, X11 and agent channels opened by the server, each relayed to a local socket (the session stays single-threaded). |
| Telnet connect worker | TCP/proxy connect off the GUI thread; the connected socket is then served by notifiers on the GUI thread. |
| Windows agent pipe bridge | relays agent messages between the OpenSSH agent's named pipe and a socket pair (named pipes cannot be polled). |
| Jump-host bridge thread | relays between a `direct-tcpip` channel on the jump connection and a socket pair used as the inner connection's transport. |
| SFTP worker (one per panel) | runs file operations sequentially on a dedicated connection. |
| ConPTY reader (Windows) | blocking `ReadFile` into a buffer, signals a manual-reset event. |

A libssh2 session is only ever used by one thread at a time: connections are handed from the
connect worker to the shell thread, and every SFTP panel / jump bridge owns its own session.

## Data flow (local session)

1. `TerminalPane::startLocal()` → `TerminalSession::start(Options)` creates a `PtyBackend`.
2. `PosixPty` opens a master with `posix_openpt`, configures termios + window size on the
   slave, prepares argv/envp, then `fork()`s. The child only performs async-signal-safe calls
   (`setsid`, `TIOCSCTTY`, `dup2`, mark fds close-on-exec, `chdir`, `execve`) and reports
   failures through a close-on-exec pipe, so errors such as a bad working directory surface
   synchronously from `start()`. On Windows `ConPty` uses `CreatePseudoConsole` + `CreateProcessW`
   with a UTF-16 environment block.
3. A notifier on the master fd (or the ConPTY event) triggers `PtyBackend::onReadable()`, which reads
   up to 256 KiB per event-loop pass and emits `dataReceived`; `TerminalSession` feeds the bytes to
   `Terminal::feed()`.
4. `VtParser` decodes UTF-8 incrementally and runs the DEC/ANSI state machine (Williams' VT500
   parser incl. DCS), dispatching to `Terminal` (print / execute / CSI / ESC / OSC / DCS). Runs of
   printable ASCII in the ground state bypass the decoder and are written cell by cell in one call
   (`printAscii`); lines scrolled into the history are moved, and the buffer of the line that falls
   out of the history is reused for the new blank line.
5. Replies (DA, DSR/CPR, DECRQSS, XTGETTCAP, colour queries) go back through the response
   handler to the backend.
6. `TerminalWidget` repaints from the model. `Terminal::takeDamage()` reports the changed rows, so
   only those rows (and the old/new cursor rows) are invalidated; scrolling, resizes, palette and
   selection changes repaint everything. Runs of identical attributes are drawn together, non-ASCII
   glyphs one per cell. While `?2026` (synchronized output) is set the widget holds repaints for up
   to 150 ms.
7. Exit detection uses EOF/EIO on the master and a 200 ms `waitpid(WNOHANG)` poll; a child that
   closes the terminal but keeps running is hung up (SIGHUP, then SIGKILL) after 500 ms.
8. Resizes: `TerminalWidget::resizeEvent → TerminalSession::resize(rows, cols, cellW, cellH)`:
   the model reflows, then the backend forwards the size (`TIOCSWINSZ` → SIGWINCH,
   `ResizePseudoConsole`, or an SSH `window-change` request).

## Data flow (SSH session)

1. `MainWindow::openProfile()` builds an `SshPlan` from the profile: one `ConnectOptions` per hop
   (outermost jump host first), credentials fetched from the vault, the known_hosts path and the
   proxy (SOCKS5/HTTP CONNECT applies to the first hop).
2. `SshBackend` starts a connect worker: `connectPlan()` calls `SshConnection::connect()` for each
   hop, tunnelling through the previous one. Each connect verifies the host key
   (`KnownHosts`, libssh2_knownhost) and authenticates (agent → key files → keyboard-interactive →
   password for *Auto*). Secrets that worked are remembered in memory for reconnects and SFTP.
3. `SshShell::start()` requests a PTY (`xterm-256color`, current size, env) and a shell or the
   profile's command, switches the session to non-blocking mode and starts the I/O thread.
4. When the connection drops (read/write/keep-alive failure), `SshBackend` schedules a reconnect
   with `wt::session::Backoff` (exponential, ±20 % jitter) and shows notices in the terminal. A
   normal remote exit ends the session (exit status shown); authentication and host key failures
   are not retried.

## Data flow (Telnet and serial sessions)

* `TelnetBackend` connects through `wt::net` (direct, SOCKS5 or HTTP CONNECT) on a worker, then reads
  and writes with socket notifiers. `wt::net::TelnetProtocol` strips and answers option negotiation
  (RFC 1143 Q method, so negotiation cannot loop), escapes IAC and reports the window size (NAWS),
  terminal type and environment; local echo follows the server's ECHO option unless forced.
* `SerialBackend` opens the device through `wt::platform::SerialPort` (termios / termios2 for custom
  baud rates on Linux, `IOSSIOSPEED` on macOS, `DCB` on Windows) with exclusive access, applies the
  newline and echo options, and reopens the device with a backoff when it disappears.

## Images

`Terminal` keeps a list of placed images anchored to absolute rows: sixel images (`DCS q`, decoded by
`wt::terminal::decodeSixel`) and kitty graphics placements (`APC G`, parsed and decoded by
`KittyGraphics.cpp`; the kitty image store holds the transmitted bitmaps, shared by placements).
The model has no image libraries: PNG decoding is a hook (`Terminal::setPngDecoder`) that
`TerminalSession` fills with `QImage`, and zlib is optional. `TerminalWidget` converts bitmaps to
premultiplied `QImage`s once (cached per bitmap) and draws negative z-indices before the text and
the rest after it, scaled from the cell size at placement time to the current one.

## Screen model details

* **Absolute rows.** Every line has an absolute row number: `historyStart()` is the oldest line
  still in the scrollback, `screenStart()` is screen row 0. Selections and search matches are stored
  in absolute coordinates, so they follow the text as it scrolls into the history.
* **Grapheme clusters.** Zero-width code points are appended to the previous cell. Such cells
  store `kClusterBase + id` in `Cell::ch`; the cluster text is interned once per terminal. With
  mode 2027 whole extended grapheme clusters (ZWJ emoji sequences, flags, VS16) form one character
  of the width of the cluster.
* **Attributes.** `CellAttributes` holds foreground, background, underline colour (SGR 58), flags
  (bold, dim, italic, underline + style bits for SGR 4:x, blink, inverse, hidden, strike-through,
  overline) and a 16-bit OSC 8 hyperlink id (URIs interned per terminal).
* **Character widths** come from tables generated by `tools/gen_char_width.py`.
* **Reflow.** When the column count changes, the primary screen and its history are rejoined into
  logical lines and re-wrapped. Wide characters never straddle a break; the blank left when a wide
  character wrapped early is marked `WidePadding` and dropped by reflow, copy and search. The cursor,
  the DECSC saved cursor and the selection end points are mapped through the rewrap.
* **Selection** (character, word, line, block) lives in the model; block selections and selections
  on the alternate screen are cleared on resize.
* **Search** (`wt::terminal::searchTerminal`) flattens logical lines to UTF-8 with a byte → cell
  map and runs a pluggable matcher: literal (with simple case folding) in the library, PCRE2 via
  `QRegularExpression` in the GUI, `std::regex` for headless use.

## Terminal emulation coverage

* C0: BEL, BS, HT, LF/VT/FF, CR, SO/SI, CAN/SUB.
* ESC: DECSC/DECRC, IND, NEL, HTS, RI, RIS, DECKPAM/DECKPNM, SCS G0/G1 (ASCII, DEC Special
  Graphics), DECALN, DECDHL/DECSWL/DECDWL (`ESC # 3/4/5/6`).
* CSI: ICH, CUU/CUD/CUF/CUB, CNL/CPL, CHA/HPA/HPR, CUP/HVP, CHT/CBT, ED (0-3), EL, IL, DL, DCH, SU,
  SD, ECH, REP, DA1/DA2, XTVERSION, VPA/VPR, TBC, SM/RM (IRM, LNM), DECRQM (ANSI and DEC private),
  SGR, DSR/CPR, DECSTBM, SCOSC/SCORC, DECSCUSR, DECSTR, XTWINOPS 14/16/18/19/22/23, XTMODKEYS
  (`CSI > 4 ; n m`), kitty keyboard protocol (`CSI > u`, `CSI < u`, `CSI = u`, `CSI ? u`).
* SGR: bold, dim, italic, underline styles (`4:0`–`4:5`, `21`), underline colour (`58`/`59`),
  blink, inverse, hidden, strike-through, overline (`53`/`55`), 16/256/truecolor with `;` and `:`.
* DEC private modes: 1, 5, 6, 7, 9, 12, 25, 47, 66, 80 (DECSDM), 1000, 1002, 1003, 1004, 1005,
  1006, 1007, 1015, 1047, 1048, 1049, 2004, 2026, 2027 (grapheme clusters).
* OSC: 0/2 title, 4/104 palette, 10/11/12 and 110/111/112, 7 working directory, 8 hyperlinks,
  52 clipboard (write only, opt-in), 133 shell integration marks (prompt / command / output).
* DCS: DECRQSS (`m`, `r`, ` q`, `"p`, `"q`), XTGETTCAP (TN, Co/colors, RGB, Tc, Su, bce, Ms, Smulx,
  Setulc), sixel graphics (`q`); XTSMGRAPHICS (`CSI ? Pi ; Pa S`) reports colour registers and
  geometry, DA1 advertises sixel (`4`).
* APC: kitty graphics protocol (`G`): transmit (direct, chunked, zlib), put, delete, query.
* Input: xterm key encoding incl. keypad application mode; kitty keyboard protocol with all flags
  (disambiguate, event types, alternate keys, report all keys, associated text; keypad keys as
  KP_* codes; modifier keys); modifyOtherKeys 1/2; mouse reports X10/1000/1002/1003 in default,
  UTF-8, SGR and urxvt encodings; wheel → history, cursor keys on the alternate screen (`?1007`).

## Legacy sources (`src/`)

Kept untouched and **not** part of the build.

| Legacy component | Status |
|---|---|
| `src/Pty` (ptyqt-based) | Replaced by `libs/platform`: `PosixPty` (no Qt, async-signal-safe child setup) and `ConPty` (Windows), which follows the reader-thread approach of the legacy `ConPty.cpp`. |
| `src/Protocols/TelnetProtocol` | Reimplemented without Qt as `wt::net::TelnetProtocol` (RFC 1143 negotiation, NAWS, TTYPE, TSPEED, NEW-ENVIRON); serial ports are new (`wt::platform::SerialPort`). |
| `src/libssh` (0.9.5 fork) | Not used. SSH is built on upstream **libssh2** (1.10+) from the system, Homebrew or vcpkg. |
| `src/Onigmo` | Not used; search uses PCRE2 (through Qt) in the GUI. |
| `src/Widgets` | Not used (Qt 5-only APIs). |
| `src/Utility` | Not used; the credential vault uses Argon2id + ChaCha20-Poly1305 from OpenSSL 3.2+ or libsodium (see [SECURITY.md](SECURITY.md)). |
