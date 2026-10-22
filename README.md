# zsshd

A minimal **SSH server payload for the PlayStation 5**, forked from
[`zftpd`](https://github.com/seregonwar/zftpd). It gives you a `root` shell over
SSH with a small set of built-in file commands, and a non-interactive mode so
other payloads/mods can automate against it.

> Homebrew for jailbroken consoles. Use on hardware you own. Passwordless root
> over the network is the default — see **Authentication** before exposing it.

## Features

- SSH server (libssh 0.12.2 + Mbed-TLS 3.6.7), RSA-2048 host key auto-generated
  on first run at `/data/zsshd/hostkey`.
- **Post-quantum key exchange** (`mlkem768x25519-sha256`, `sntrup761x25519-sha512`)
  — no "store now, decrypt later" warning from modern OpenSSH clients.
- **Passwordless `root` by default**; optional password and/or public-key auth.
- Interactive shell: eza-style `ls`, fish-style colored prompt, command history
  (Up/Down), and in-line editing (Left/Right, Home/End, Delete).
- Built-in commands: `ls cd pwd cat echo whoami uname clear help exit`,
  `mkdir rmdir touch rm cp mv`, `head tail grep`, `less` (alias of `cat`).
- **Scripting / automation:**
  - `ssh root@ps5 'cmd1; cmd2'` runs the command(s) and returns their exit status.
  - Piping commands (no PTY) runs them line-by-line with clean, uncolored output.
  - `;` and newlines separate multiple commands.
- Single-instance control: a new copy asks a running one to stand down.
- On-screen toast on startup showing the listening `IP:port`.

## Requirements

- [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) with the
  `$PS5_PAYLOAD_SDK` environment variable set.
- CMake, and a host `cmake`/toolchain the SDK uses (`prospero-cmake`).
- `git` (the dependencies are fetched as submodules / vendored source).

Dependencies, and how they are provided:

| Dependency | Version | How it's included |
|------------|---------|-------------------|
| Mbed-TLS   | 3.6.7   | git submodule (`external/mbedtls`) |
| libssh     | 0.12.2  | vendored source (`external/libssh-0.12.2`) |

## Building

```fish
# 1. Clone with submodules (pulls Mbed-TLS and its 'framework' submodule)
git clone --recursive https://github.com/jinxcase000/zsshd.git
cd zsshd

# 2. zsshd's server is multi-threaded, so Mbed-TLS needs threading enabled.
#    Enable the two threading options in the Mbed-TLS config:
sed -i 's|^//#define MBEDTLS_THREADING_C$|#define MBEDTLS_THREADING_C|; \
        s|^//#define MBEDTLS_THREADING_PTHREAD$|#define MBEDTLS_THREADING_PTHREAD|' \
    external/mbedtls/include/mbedtls/mbedtls_config.h

# 3. Build + install Mbed-TLS (static) into external/prefix
set P $PWD/external/prefix
$PS5_PAYLOAD_SDK/bin/prospero-cmake -S external/mbedtls -B build/mbedtls \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF
cmake --build build/mbedtls -j(nproc); and cmake --install build/mbedtls

# 4. Build + install libssh against that Mbed-TLS prefix
$PS5_PAYLOAD_SDK/bin/prospero-cmake -S external/libssh-0.12.2 -B build/libssh-0.12 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
  -DBUILD_SHARED_LIBS=OFF -DWITH_MBEDTLS=ON \
  -DMBEDTLS_INCLUDE_DIR=$P/include \
  -DMBEDTLS_SSL_LIBRARY=$P/lib/libmbedtls.a \
  -DMBEDTLS_CRYPTO_LIBRARY=$P/lib/libmbedcrypto.a \
  -DMBEDTLS_X509_LIBRARY=$P/lib/libmbedx509.a \
  -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF -DWITH_ZLIB=OFF \
  -DWITH_NACL=OFF -DWITH_PCAP=OFF -DWITH_EXEC=OFF -DWITH_EXAMPLES=OFF \
  -DUNIT_TESTING=OFF -DWITH_SYMBOL_VERSIONING=OFF -DWITH_DEBUG_CALLTRACE=OFF
cmake --build build/libssh-0.12 -j(nproc); and cmake --install build/libssh-0.12

# 5. Build the payload
make
```

Output: `zsshd.elf`.

> **bash/zsh note:** the commands above use fish syntax (`set P ...`, `(nproc)`).
> In bash/zsh use `P=$PWD/external/prefix` and `$(nproc)`.
>
> **GEN_FILES:** `-DGEN_FILES=OFF` uses the generated files already present in
> the 3.6.7 checkout. If your checkout is missing them, use `-DGEN_FILES=ON`
> (requires Python 3 + `jinja2`).

## Deploying

Send `zsshd.elf` to the console's ELF loader (default port 9021):

```fish
socat -t 99999999 - TCP:<ps5-ip>:9021 < zsshd.elf
```

Then connect:

```fish
ssh root@<ps5-ip>
```

## Authentication

Default: anyone can log in as `root` with **no password**. To require real auth,
create either file on the console (the presence of either disables passwordless
`none`):

- `/data/zsshd/password` — first line is the accepted password.
- `/data/zsshd/authorized_keys` — OpenSSH `authorized_keys` format (one key per
  line; supports multiple keys).

## Usage

Interactive:

```fish
ssh root@<ps5-ip>
```

Run a command (or several) and get the exit status back:

```fish
ssh root@<ps5-ip> 'mkdir /data/mymod; ls /data'
```

Pipe a script of commands:

```fish
printf 'cd /data\nmkdir backup\ncp foo backup/\nls backup\n' | ssh root@<ps5-ip>
```

### File transfer (SFTP / scp)

```fish
# browse / download interactively
sftp root@<ps5-ip>

# copy a file down, or up
scp root@<ps5-ip>:/data/file ./file
scp ./file root@<ps5-ip>:/data/dir/
```

## Limitations

- **Not a max-throughput transfer path.** SFTP runs inside the encrypted SSH
  channel; for bulk transfers the plaintext [`zftpd`](https://github.com/seregonwar/zftpd)
  payload is faster. zsshd's SFTP is the secure/convenient option. The server
  prefers hardware-accelerated AES-GCM to keep it reasonable.
- **Built-in SFTP server gaps (libssh 0.12 default server).** Upload, download,
  directory listing, `mkdir`/`rmdir`/`remove`, `stat`/`lstat`,
  `symlink`/`readlink` and `realpath` all work. `rename`, `fsetstat` and `fstat`
  are not implemented by libssh's default server, so:
  - `scp` prints a harmless `remote fsetstat: Operation unsupported` after an
    upload. The file transfers correctly; only mode/timestamp preservation is
    skipped.
  - Renaming over SFTP fails — rename via the shell instead:
    `ssh root@<ps5-ip> 'mv old new'`.

## Versioning

See [`CHANGELOG.md`](CHANGELOG.md). The version is defined once in
`include/zsshd_version.h` and shown in both the startup toast and the shell
banner.

## License

zsshd is MIT (see [`LICENSE.zftpd`](LICENSE.zftpd), inherited from the upstream
`zftpd` fork). Bundled dependencies keep their own licenses: libssh is
LGPL-2.1, Mbed-TLS is Apache-2.0 / GPL-2.0.

## Credits

- [`zftpd`](https://github.com/seregonwar/zftpd) by SeregonWar — the fork base.
- [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) by John Törnblom.
