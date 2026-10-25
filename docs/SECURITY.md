# Security model

This document describes how WindTerm-Weifan handles host keys, credentials and data a remote
program can send to the terminal.

## SSH host keys

* Host keys are checked against an OpenSSH `known_hosts` file: `~/.ssh/known_hosts` by default,
  or the file chosen in *Settings ▸ SSH*. Plain and hashed (`|1|...`) entries and the
  `[host]:port` form are understood (libssh2's known-hosts parser). Lines libssh2 cannot parse are
  kept untouched.
* **Unknown key**: the user sees the key type and SHA-256 fingerprint (same format as
  `ssh-keygen -l`) and chooses *Accept and Save*, *Connect Once* or *Cancel*. Saved entries are
  appended in OpenSSH format, so `ssh` agrees with them.
* **Changed key**: a prominent warning is shown and *Cancel* is the default. *Replace and Connect*
  removes the entries of that host:port with a different key of the same type and appends the new
  key; other entries and comments are preserved.
* Jump hosts are verified like any other host; the target behind a jump host is checked under its
  own name.

## Authentication

Methods (per session): *Automatic* (SSH agent → identity file or `~/.ssh/id_ed25519`, `id_ecdsa`,
`id_rsa` → keyboard-interactive → password), or one of password, public key, agent,
keyboard-interactive. Encrypted private keys (OpenSSH and PEM formats) are detected and the
passphrase is asked for (three attempts). Passwords and passphrases that worked are kept in memory
for automatic reconnects and SFTP connections of that session only, and wiped when the connection
object goes away. Private key files are read only to detect encryption and the copy is wiped;
libssh2 loads the key itself.

## Connection handling

* Keep-alives are sent every *Keep-alive interval* seconds; the TCP user timeout is set to about
  three intervals, so a silently dead peer (no RST) is detected in that time and triggers the
  automatic reconnect.
* A connection must stay up for 10 seconds before the reconnect backoff resets, so a server that
  drops sessions right after login is not retried forever.
* known_hosts entries are appended with `O_APPEND` (symlinks followed, other lines untouched);
  a file that exists but cannot be read is never rewritten.

## Credential vault

Passwords, key passphrases and proxy passwords can be stored in an encrypted vault
(`<config>/vault.json`, permissions 0600). Session files (`sessions.json`) only contain an opaque
credential id, never a secret.

* **Key derivation:** Argon2id (RFC 9106), default 64 MiB memory, 3 passes, 1 lane, 16-byte random
  salt, 32-byte key. Only the derived key is kept in memory while the vault is unlocked; the
  master password itself is not stored.
* **Encryption:** ChaCha20-Poly1305 (RFC 8439, IETF variant) over a JSON document holding all
  entries. A fresh random 96-bit nonce is used for every save; changing the master password also
  generates a new salt.
* **Integrity:** the file header (format, version, cipher and KDF parameters including the salt) is
  authenticated as associated data, so lowering the KDF cost or swapping the salt makes decryption
  fail. KDF parameters above 1 GiB / 32 passes / 16 lanes are rejected before any work is done.
* **Backends:** OpenSSL ≥ 3.2 or libsodium; both produce identical files (a vault written by one
  opens with the other; covered by `tests/data/vault-v1.json`).
* A wrong master password and a damaged file are indistinguishable by design ("wrong master
  password or damaged vault").

File format (version 1):

```json
{
  "format": "windterm-weifan-vault",
  "version": 1,
  "cipher": "chacha20-poly1305",
  "kdf": { "algorithm": "argon2id", "memoryKiB": 65536, "iterations": 3, "parallelism": 1, "salt": "<base64>" },
  "nonce": "<base64, 12 bytes>",
  "ciphertext": "<base64, encrypted entries || 16-byte tag>"
}
```

The associated data is the compact JSON of the header without `nonce` and `ciphertext` (keys
sorted). The plaintext is `{"entries": {"<id>": {"secret": ..., "label": ..., "updated": <unix time>}}}`.

Limitations: the vault does not lock itself after a timeout yet, secrets passed to Qt widgets
(password fields) and to libssh2 cannot be wiped by us, and swap/hibernation files are outside our
control.

## Terminal escape sequences

* **OSC 52 (clipboard):** writes are ignored unless *Allow programs to set the clipboard* is
  enabled; clipboard *reads* (`OSC 52 ; c ; ?`) are never answered, so a remote host cannot steal
  the clipboard.
* **OSC 8 / URLs:** links are only opened on Ctrl+click (or the context menu), and only for
  `http`, `https`, `ftp`, `file` and `mailto` URLs. The URI is shown in a tooltip first.
* **Paste:** in bracketed-paste mode, embedded end markers (`ESC [ 201 ~`) and every other ESC
  byte are removed, so pasted text cannot terminate the bracket early (not even with fragments that
  only form a marker once another one is removed).
* Replies to queries (DECRQSS, XTGETTCAP, colour queries) never contain user data.
* Input from the PTY/SSH channel is bounded: OSC and DCS strings are capped at 64 KiB, grapheme
  clusters are interned with fixed limits, OSC 8 ids are truncated to 250 bytes and all interned
  links together are capped at 8 MiB (further links are shown as plain text).

## Files written

| File | Content | Permissions |
|---|---|---|
| `<config>/settings.json` | appearance, key bindings, options | default |
| `<config>/sessions.json` | session profiles (no secrets) | 0600 |
| `<config>/vault.json` | encrypted credentials | 0600 |
| `known_hosts` | host keys you accepted | 0600 (rewritten atomically when a key is added) |

All of them are written atomically (temporary file, `fsync`, rename). `<config>` is
`~/.config/WindTerm-Weifan` on Linux, `~/Library/Preferences/WindTerm-Weifan` on macOS and
`%LOCALAPPDATA%\WindTerm-Weifan` on Windows.

## Reporting

Please report security issues privately to the maintainers rather than in a public issue.
