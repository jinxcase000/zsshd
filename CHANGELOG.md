# Changelog

All notable changes to **zsshd** are recorded here.
Format loosely follows [Keep a Changelog]; versioning is SemVer-ish.

## [0.1.0] - 2026-10-18
### Changed
- Upgraded the libssh dependency from 0.11.1 to 0.12.2 (built against the same
  mbedTLS 3.6.x prefix).
### Added
- Post-quantum key exchange: the server now offers `sntrup761x25519-sha512` and
  `mlkem768x25519-sha256` (provided by libssh 0.12 on the mbedTLS backend),
  clearing the OpenSSH "not using a post-quantum key exchange" warning.
### Fixed
- Host-key generation migrated to the new `ssh_pki_generate_key` + `ssh_pki_ctx`
  API; `ssh_pki_generate` was deprecated in libssh 0.12.

## [0.0.5] - 2026-10-18
### Added
- Full in-line editing in the shell: Left/Right move the cursor, Home/End jump
  to the ends, Delete removes the character under the cursor, and typing /
  backspace now insert and delete mid-line (input was previously append-only).

## [0.0.4] - 2026-10-18
### Added
- Text commands: `head` and `tail` (`-n N`, default 10 lines), `grep`
  (`-i`, POSIX extended regex), and `less` aliased to `cat` (no pager).

## [0.0.3] - 2026-10-18
### Added
- Filesystem commands in the shell: `mkdir` (with `-p`), `rmdir`, `touch`,
  `rm` (with `-r` / `-f`), `cp` (with `-r`), and `mv` (rename, with a
  copy-then-delete fallback across filesystem boundaries).
### Fixed
- Worker threads now get a 1 MiB stack and `cp`'s copy buffer was reduced from
  64 KiB to 8 KiB; the oversized on-stack buffer overflowed the small default
  thread stack and segfaulted the whole payload on the first `cp`.

## [0.0.2] - 2026-10-18
### Fixed
- Main-thread naming now uses `syscall(SYS_thr_set_name, -1, "zsshd.elf")`
  (the same mechanism as zftpd). The 0.0.1 `pthread_set_name_np` path was an
  unresolved weak symbol on the SDK, so the task manager still showed
  `payload.elf`.

## [0.0.1] - 2026-10-18
### Added
- Initial working PS5 SSH payload, forked from zftpd.
- Build: Makefile targeting ps5-payload-sdk (prospero clang), linking prebuilt
  libssh 0.11.1 + mbedtls static libs from `external/prefix`. `PLATFORM_PS5`
  defined so the real code paths (not the stubs) compile.
- SSH bring-up: RSA-2048 host key auto-generated at `/data/zsshd/hostkey`,
  listener on port 22, one detached thread per connection.
- Auth: passwordless `root` by default. Dropping in `/data/zsshd/password` or
  `/data/zsshd/authorized_keys` enables that method and disables `none`.
- Interactive shell with built-ins: `ls cd pwd cat echo whoami uname clear
  help exit`.
- `ls` in eza style: permission string, human-readable size, owner, mtime,
  Nerd Font folder/file glyphs, color.
- fish-style colored prompt: `root@host path ❯`.
- Command history with Up/Down arrow navigation.
- Startup console toast: `zSSHd [ver.0.0.1] by JinxCase Started` + `IP:port`.
- Main thread renamed to `zsshd.elf` so instance-control's thread scan can
  detect a previous instance.

[Keep a Changelog]: https://keepachangelog.com/
