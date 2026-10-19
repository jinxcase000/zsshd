# zsshd

A minimal SSH server payload for the PS5, forked from
[`zftpd`](https://github.com/seregonwar/zftpd). Gives a passwordless `root`
shell over SSH with a small set of built-in file commands.

## Features
- SSH server (libssh 0.11.1 + mbedtls), RSA-2048 host key auto-generated on
  first run at `/data/zsshd/hostkey`.
- Passwordless `root` by default; optional password / public-key auth.
- Built-in shell: `ls cd pwd cat echo whoami uname clear help exit`
  (eza-style `ls`, colored fish-style prompt, Up/Down command history).
- Console toast on startup with the listening `IP:port`.

## Auth configuration
Default: anyone can log in as `root` with no password. To require real auth,
create either file on the console (presence disables passwordless `none`):
- `/data/zsshd/password` - first line is the accepted password.
- `/data/zsshd/authorized_keys` - OpenSSH `authorized_keys` format.

## Building
Requires [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) with
`$PS5_PAYLOAD_SDK` set, and mbedTLS 3.6.x built into `external/prefix`.

Dependencies (mbedtls, then libssh) are built once into `external/prefix`.
libssh is configured against the already-installed mbedtls like so:

    set P $PWD/external/prefix
    $PS5_PAYLOAD_SDK/bin/prospero-cmake -S external/libssh-0.12.2 -B build/libssh-0.12 \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
      -DBUILD_SHARED_LIBS=OFF -DWITH_MBEDTLS=ON \
      -DMBEDTLS_INCLUDE_DIR=$P/include \
      -DMBEDTLS_SSL_LIBRARY=$P/lib/libmbedtls.a \
      -DMBEDTLS_CRYPTO_LIBRARY=$P/lib/libmbedcrypto.a \
      -DMBEDTLS_X509_LIBRARY=$P/lib/libmbedx509.a \
      -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_GSSAPI=OFF -DWITH_ZLIB=OFF \
      -DWITH_NACL=OFF -DWITH_PCAP=OFF -DWITH_EXEC=OFF -DWITH_EXAMPLES=OFF \
      -DUNIT_TESTING=OFF -DWITH_SYMBOL_VERSIONING=OFF \
      -DWITH_DEBUG_CALLTRACE=OFF
    cmake --build build/libssh-0.12 -j(nproc); and cmake --install build/libssh-0.12

Then build the payload:

    make

Output: `zsshd.elf`. Deploy with `make test PS5_HOST=<ip>` or your loader.

## License
MIT (see `LICENSE.zftpd`).
