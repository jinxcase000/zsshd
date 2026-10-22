# zsshd roadmap

Tracking the path to a "Linux-grade" interactive SSH shell. Done items live in
[`CHANGELOG.md`](CHANGELOG.md); this file is what's planned.

## Next: full `less`-style pager (upgrade of the 0.4.0 `more` pager)

The current pager is forward-only. A full pager adds:

- **Backward scrolling.** Keep the file's lines in memory (or re-read a
  seekable file) and track a top-line offset, so we can move up as well as down.
- **Navigation keys:** Up/Down (one line), PageUp/PageDown or Space/`b`
  (one screen), `g`/`G` (top/bottom), `q` (quit). Reuse the existing CSI
  escape-sequence parser from the line editor for the arrow keys.
- **Search:** `/pattern` forward, `n`/`N` for next/prev, reusing the POSIX
  `regcomp`/`regexec` already linked for `grep`.
- **Status line:** reverse-video bar showing filename and position (`line x/y`
  or `(END)`), redrawn each screen.
- **Full-screen redraw:** clear + repaint the visible window on each move,
  using the captured PTY rows/cols.
- **Line wrapping:** account for lines longer than the terminal width when
  counting rows (the `more` pager intentionally ignores this).

Scope note: interactive-only. Scripted/piped sessions keep the `cat` fallback
so automation output stays clean.

## Toward a Linux-grade shell

Roughly in priority order:

1. **Window-resize handling.** Honour the SSH `window-change` request so the
   pager and prompt track terminal size after a resize (today we only capture
   the size at PTY-request time).
2. **Quoting in the tokenizer.** Handle `"..."`, `'...'` and `\` escapes so
   paths with spaces work; today arguments split on raw whitespace.
3. **Globbing:** expand `*` and `?` against the filesystem before dispatch.
4. **Redirection & pipes:** `>`, `>>`, `<`, and `|` between built-ins.
5. **Environment variables:** `export`, `$VAR` expansion, a `env`/`set` builtin.
6. **More built-ins:** `wc`, `find`, `which`, `chmod`, `chown`, `ln`, `df`,
   `du`, `date`, `sleep`, `ps`, `kill`.

## Transfer

- Optional **full custom SFTP server** (replacing libssh's built-in default) to
  implement `rename`, `fsetstat` and `fstat`, removing the `scp` metadata
  warning and enabling SFTP rename. Only if the built-in server's gaps become a
  real pain point — see `CHANGELOG.md` 0.3.0 note.
