OpenSSH 10.6p2 for MyOS

Install as root: ext install openssh
The package builds upstream OpenSSH with /bin/cc and /bin/make, installs ssh,
sshd, sshd-session, sshd-auth, ssh-keygen, ssh-keyscan, scp, sftp and
sftp-server in /ext/bin, then runs /ext/bin/openssh-setup.

Settings: /etc/ssh/ssh_config and /etc/ssh/sshd_config.
Host key: /etc/ssh/ssh_host_ed25519_key.
Existing configuration and host keys survive reinstall and uninstall.
Installation validates sshd -t, creates /etc/services/sshd.service with
User=root, enables boot autostart and starts the service immediately.
On subsequent boots it starts before anyone logs into the local console.
Use systemctl status|start|stop|restart sshd and systemctl disable --now sshd.
Logs: /var/log/sshd.service. ext uninstall openssh stops/disables the service
and removes its unit and programs, keeping configuration and host keys.
Repeated /ext/bin/openssh-setup preserves existing settings and does not
start a duplicate daemon. Already-installed ext packages are skipped; run
openssh-setup as root after updating the installer to register the service.
An updated kernel with User=root service support is required.
Foreground debugging: systemctl stop sshd, then /ext/bin/sshd -D -e -ddd
sshd needs an absolute executable path because it re-executes helper programs.

Clients: ssh user@host; scp file user@host:/path; sftp user@host.
Generate an identity: ssh-keygen -t ed25519
Root login defaults to public keys only. Put the public key in
/root/.ssh/authorized_keys. Other users authenticate against MyOS accounts;
password checks run in the kernel and do not change the monitor's identity.

Port profile: IPv4, built-in Ed25519/Curve25519/ChaCha20-Poly1305/AES-CTR
cryptography, no OpenSSL or zlib. RSA/ECDSA, PAM, FIDO, agent sockets, Unix
socket forwarding, IPv6 and compression are not included.
RDRAND is required for cryptographic entropy; QEMU must use -cpu max or a
CPU model exposing RDRAND. Host launch: make run QEMU_CPU=max SSH_PORT=2222
Connect from the host: ssh -p 2222 user@127.0.0.1. There is no predictable entropy fallback.
The kernel enforces UID/GID drops, close-on-exec and the pre-auth chroot.
Local socketpairs support the descriptor transfer used by privilege separation.

MyFS enforces ownership and rwx permissions. Keep private keys at mode 0600
and home directories at 0700. Rebuild installed ports after a libc update.
Older files without attributes default to root ownership; assign their owners
and protect existing private keys explicitly. Keep this experimental server on an
isolated test network. LoginGraceTime is zero because MyOS does not deliver
POSIX timer signals; unauthenticated sessions can occupy the two startup slots
until the connection closes or the server is restarted. SIGHUP reload and
signal-based graceful termination are unavailable. Metadata-changing SFTP
operations (timestamps, arbitrary truncation, statvfs, hard links)
return unsupported errors. A bind to a specific IPv4 address is rejected by
this adapter because native MyOS bind currently listens on all interfaces.

Source: https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.6p2.tar.gz
SHA256: 9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b
Release notes/checksum: https://www.openssh.org/txt/release-10.6p2
Build validation: make openssh-build
QEMU integration: make opensshtest
