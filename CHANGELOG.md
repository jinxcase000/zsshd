# Changelog

All notable changes to **zsshd** are recorded here.
Format loosely follows [Keep a Changelog]; versioning is SemVer-ish.

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
