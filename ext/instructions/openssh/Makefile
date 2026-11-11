CC = cc -DHAVE_CONFIG_H -I. -Imyos-include -Iopenbsd-compat

all: installed

installed: /ext/bin/openssh-setup /ext/bin/ssh /ext/bin/sshd /ext/bin/sshd-session /ext/bin/sshd-auth /ext/bin/ssh-keygen /ext/bin/ssh-keyscan /ext/bin/sftp-server /ext/bin/sftp /ext/bin/scp
	/ext/bin/openssh-setup

s0.a: ssh_api.o ssherr.o sshbuf.o sshkey.o sshbuf-getput-basic.o sshbuf-misc.o sshbuf-getput-crypto.o krl.o bitmap.o authfd.o authfile.o canohost.o channels.o cipher.o cipher-aes.o cipher-aesctr.o cleanup.o compat.o fatal.o hostfile.o
	ar rc s0.a ssh_api.o ssherr.o sshbuf.o sshkey.o sshbuf-getput-basic.o sshbuf-misc.o
	ar rc s0.a sshbuf-getput-crypto.o krl.o bitmap.o authfd.o authfile.o canohost.o
	ar rc s0.a channels.o cipher.o cipher-aes.o cipher-aesctr.o cleanup.o compat.o
	ar rc s0.a fatal.o hostfile.o

s1.a: log.o match.o moduli.o nchan.o packet.o readpass.o ttymodes.o xmalloc.o addr.o addrmatch.o atomicio.o dispatch.o mac.o misc.o utf8.o monitor_fdpass.o rijndael.o ssh-ecdsa.o ssh-ecdsa-sk.o ssh-ed25519-sk.o
	ar rc s1.a log.o match.o moduli.o nchan.o packet.o readpass.o
	ar rc s1.a ttymodes.o xmalloc.o addr.o addrmatch.o atomicio.o dispatch.o
	ar rc s1.a mac.o misc.o utf8.o monitor_fdpass.o rijndael.o ssh-ecdsa.o
	ar rc s1.a ssh-ecdsa-sk.o ssh-ed25519-sk.o

s2.a: ssh-rsa.o dh.o msg.o dns.o entropy.o gss-genr.o umac.o umac128.o smult_curve25519_ref.o poly1305.o chacha.o cipher-chachapoly.o cipher-chachapoly-libcrypto.o ssh-ed25519.o digest-openssl.o digest-libc.o libcrux-mlkem-mldsa.o ssh-mldsa-eddsa.o hmac.o ed25519.o
	ar rc s2.a ssh-rsa.o dh.o msg.o dns.o entropy.o gss-genr.o
	ar rc s2.a umac.o umac128.o smult_curve25519_ref.o poly1305.o chacha.o cipher-chachapoly.o
	ar rc s2.a cipher-chachapoly-libcrypto.o ssh-ed25519.o digest-openssl.o digest-libc.o libcrux-mlkem-mldsa.o ssh-mldsa-eddsa.o
	ar rc s2.a hmac.o ed25519.o

s3.a: ed25519-openssl.o kex.o kex-names.o kexdh.o kexgex.o kexecdh.o kexc25519.o kexgexc.o kexgexs.o kexmlkem768ecdh.o kexsntrup761x25519.o kexmlkem768x25519.o sntrup761.o kexgen.o sftp-realpath.o platform-pledge.o platform-tracing.o platform-misc.o sshbuf-io.o misc-agent.o
	ar rc s3.a ed25519-openssl.o kex.o kex-names.o kexdh.o kexgex.o kexecdh.o
	ar rc s3.a kexc25519.o kexgexc.o kexgexs.o kexmlkem768ecdh.o kexsntrup761x25519.o kexmlkem768x25519.o
	ar rc s3.a sntrup761.o kexgen.o sftp-realpath.o platform-pledge.o platform-tracing.o platform-misc.o
	ar rc s3.a sshbuf-io.o misc-agent.o

s4.a: ssherr-libcrypto.o
	ar rc s4.a ssherr-libcrypto.o

c0.a: openbsd-compat/arc4random.o openbsd-compat/arc4random_uniform.o openbsd-compat/base64.o openbsd-compat/basename.o openbsd-compat/bcrypt_pbkdf.o openbsd-compat/bindresvport.o openbsd-compat/blowfish.o openbsd-compat/daemon.o openbsd-compat/dirname.o openbsd-compat/explicit_bzero.o openbsd-compat/fmt_scaled.o openbsd-compat/freezero.o openbsd-compat/fnmatch.o openbsd-compat/getcwd.o openbsd-compat/getgrouplist.o openbsd-compat/getopt_long.o openbsd-compat/getrrsetbyname.o openbsd-compat/glob.o openbsd-compat/inet_aton.o openbsd-compat/inet_ntoa.o
	ar rc c0.a openbsd-compat/arc4random.o openbsd-compat/arc4random_uniform.o openbsd-compat/base64.o openbsd-compat/basename.o openbsd-compat/bcrypt_pbkdf.o openbsd-compat/bindresvport.o
	ar rc c0.a openbsd-compat/blowfish.o openbsd-compat/daemon.o openbsd-compat/dirname.o openbsd-compat/explicit_bzero.o openbsd-compat/fmt_scaled.o openbsd-compat/freezero.o
	ar rc c0.a openbsd-compat/fnmatch.o openbsd-compat/getcwd.o openbsd-compat/getgrouplist.o openbsd-compat/getopt_long.o openbsd-compat/getrrsetbyname.o openbsd-compat/glob.o
	ar rc c0.a openbsd-compat/inet_aton.o openbsd-compat/inet_ntoa.o

c1.a: openbsd-compat/inet_ntop.o openbsd-compat/md5.o openbsd-compat/memmem.o openbsd-compat/mktemp.o openbsd-compat/pwcache.o openbsd-compat/readpassphrase.o openbsd-compat/reallocarray.o openbsd-compat/recallocarray.o openbsd-compat/rresvport.o openbsd-compat/setenv.o openbsd-compat/setproctitle.o openbsd-compat/sha1.o openbsd-compat/sha2.o openbsd-compat/sigact.o openbsd-compat/strcasestr.o openbsd-compat/strlcat.o openbsd-compat/strlcpy.o openbsd-compat/strmode.o openbsd-compat/strndup.o openbsd-compat/strnlen.o
	ar rc c1.a openbsd-compat/inet_ntop.o openbsd-compat/md5.o openbsd-compat/memmem.o openbsd-compat/mktemp.o openbsd-compat/pwcache.o openbsd-compat/readpassphrase.o
	ar rc c1.a openbsd-compat/reallocarray.o openbsd-compat/recallocarray.o openbsd-compat/rresvport.o openbsd-compat/setenv.o openbsd-compat/setproctitle.o openbsd-compat/sha1.o
	ar rc c1.a openbsd-compat/sha2.o openbsd-compat/sigact.o openbsd-compat/strcasestr.o openbsd-compat/strlcat.o openbsd-compat/strlcpy.o openbsd-compat/strmode.o
	ar rc c1.a openbsd-compat/strndup.o openbsd-compat/strnlen.o

c2.a: openbsd-compat/strptime.o openbsd-compat/strsep.o openbsd-compat/strtoll.o openbsd-compat/strtonum.o openbsd-compat/strtoull.o openbsd-compat/strtoul.o openbsd-compat/timingsafe_bcmp.o openbsd-compat/vis.o openbsd-compat/bsd-asprintf.o openbsd-compat/bsd-closefrom.o openbsd-compat/bsd-cygwin_util.o openbsd-compat/bsd-err.o openbsd-compat/bsd-flock.o openbsd-compat/bsd-getentropy.o openbsd-compat/bsd-getline.o openbsd-compat/bsd-getpagesize.o openbsd-compat/bsd-getpeereid.o openbsd-compat/bsd-malloc.o openbsd-compat/bsd-misc.o openbsd-compat/bsd-nextstep.o
	ar rc c2.a openbsd-compat/strptime.o openbsd-compat/strsep.o openbsd-compat/strtoll.o openbsd-compat/strtonum.o openbsd-compat/strtoull.o openbsd-compat/strtoul.o
	ar rc c2.a openbsd-compat/timingsafe_bcmp.o openbsd-compat/vis.o openbsd-compat/bsd-asprintf.o openbsd-compat/bsd-closefrom.o openbsd-compat/bsd-cygwin_util.o openbsd-compat/bsd-err.o
	ar rc c2.a openbsd-compat/bsd-flock.o openbsd-compat/bsd-getentropy.o openbsd-compat/bsd-getline.o openbsd-compat/bsd-getpagesize.o openbsd-compat/bsd-getpeereid.o openbsd-compat/bsd-malloc.o
	ar rc c2.a openbsd-compat/bsd-misc.o openbsd-compat/bsd-nextstep.o

c3.a: openbsd-compat/bsd-openpty.o openbsd-compat/bsd-poll.o openbsd-compat/bsd-pselect.o openbsd-compat/bsd-setres_id.o openbsd-compat/bsd-signal.o openbsd-compat/bsd-snprintf.o openbsd-compat/bsd-statvfs.o openbsd-compat/bsd-timegm.o openbsd-compat/bsd-waitpid.o openbsd-compat/fake-rfc2553.o openbsd-compat/getrrsetbyname-ldns.o openbsd-compat/kludge-fd_set.o openbsd-compat/openssl-compat.o openbsd-compat/libressl-api-compat.o openbsd-compat/xcrypt.o openbsd-compat/port-aix.o openbsd-compat/port-irix.o openbsd-compat/port-linux.o openbsd-compat/port-linux-selinux.o openbsd-compat/port-prngd.o
	ar rc c3.a openbsd-compat/bsd-openpty.o openbsd-compat/bsd-poll.o openbsd-compat/bsd-pselect.o openbsd-compat/bsd-setres_id.o openbsd-compat/bsd-signal.o openbsd-compat/bsd-snprintf.o
	ar rc c3.a openbsd-compat/bsd-statvfs.o openbsd-compat/bsd-timegm.o openbsd-compat/bsd-waitpid.o openbsd-compat/fake-rfc2553.o openbsd-compat/getrrsetbyname-ldns.o openbsd-compat/kludge-fd_set.o
	ar rc c3.a openbsd-compat/openssl-compat.o openbsd-compat/libressl-api-compat.o openbsd-compat/xcrypt.o openbsd-compat/port-aix.o openbsd-compat/port-irix.o openbsd-compat/port-linux.o
	ar rc c3.a openbsd-compat/port-linux-selinux.o openbsd-compat/port-prngd.o

c4.a: openbsd-compat/port-solaris.o openbsd-compat/port-net.o openbsd-compat/port-uw.o
	ar rc c4.a openbsd-compat/port-solaris.o openbsd-compat/port-net.o openbsd-compat/port-uw.o

p-ssh.a: readconf.o clientloop.o sshtty.o sshconnect.o sshconnect2.o mux.o ssh-pkcs11.o ssh-sk-client.o
	ar rc p-ssh.a readconf.o clientloop.o sshtty.o sshconnect.o sshconnect2.o mux.o
	ar rc p-ssh.a ssh-pkcs11.o ssh-sk-client.o

/ext/bin/ssh: ssh.o p-ssh.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/ssh ssh.o p-ssh.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-sshd.a: platform-listen.o servconf.o srclimit.o groupaccess.o auth2-methods.o dns.o fatal.o compat.o utf8.o authfd.o canohost.o ssh-pkcs11-client.o ssh-sk-client.o
	ar rc p-sshd.a platform-listen.o servconf.o srclimit.o groupaccess.o auth2-methods.o dns.o
	ar rc p-sshd.a fatal.o compat.o utf8.o authfd.o canohost.o ssh-pkcs11-client.o
	ar rc p-sshd.a ssh-sk-client.o

/ext/bin/sshd: sshd.o p-sshd.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/sshd sshd.o p-sshd.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-sshd-session.a: auth-rhosts.o auth-passwd.o audit.o audit-bsm.o audit-linux.o platform.o sshpty.o sshlogin.o servconf.o serverloop.o auth.o auth2.o auth2-methods.o auth-options.o session.o auth2-chall.o groupaccess.o auth-bsdauth.o auth2-hostbased.o auth2-kbdint.o auth2-none.o auth2-passwd.o auth2-pubkey.o auth2-pubkeyfile.o monitor.o monitor_wrap.o auth-krb5.o auth2-gss.o gss-serv.o gss-serv-krb5.o loginrec.o auth-pam.o auth-shadow.o auth-sia.o sftp-server.o sftp-common.o uidswap.o platform-listen.o ssh-pkcs11-client.o ssh-sk-client.o
	ar rc p-sshd-session.a auth-rhosts.o auth-passwd.o audit.o audit-bsm.o audit-linux.o platform.o
	ar rc p-sshd-session.a sshpty.o sshlogin.o servconf.o serverloop.o auth.o auth2.o
	ar rc p-sshd-session.a auth2-methods.o auth-options.o session.o auth2-chall.o groupaccess.o auth-bsdauth.o
	ar rc p-sshd-session.a auth2-hostbased.o auth2-kbdint.o auth2-none.o auth2-passwd.o auth2-pubkey.o auth2-pubkeyfile.o
	ar rc p-sshd-session.a monitor.o monitor_wrap.o auth-krb5.o auth2-gss.o gss-serv.o gss-serv-krb5.o
	ar rc p-sshd-session.a loginrec.o auth-pam.o auth-shadow.o auth-sia.o sftp-server.o sftp-common.o
	ar rc p-sshd-session.a uidswap.o platform-listen.o ssh-pkcs11-client.o ssh-sk-client.o

/ext/bin/sshd-session: sshd-session.o p-sshd-session.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/sshd-session sshd-session.o p-sshd-session.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-sshd-auth.a: auth2-methods.o auth-rhosts.o auth-passwd.o sshpty.o sshlogin.o servconf.o serverloop.o auth.o auth2.o auth-options.o session.o auth2-chall.o groupaccess.o auth-bsdauth.o auth2-hostbased.o auth2-kbdint.o auth2-none.o auth2-passwd.o auth2-pubkey.o auth2-pubkeyfile.o auth2-gss.o gss-serv.o gss-serv-krb5.o monitor_wrap.o auth-krb5.o audit.o audit-bsm.o audit-linux.o platform.o loginrec.o auth-pam.o auth-shadow.o auth-sia.o sandbox-null.o sandbox-rlimit.o sandbox-darwin.o sandbox-seccomp-filter.o sandbox-capsicum.o sandbox-solaris.o sftp-server.o sftp-common.o uidswap.o ssh-pkcs11-client.o ssh-sk-client.o
	ar rc p-sshd-auth.a auth2-methods.o auth-rhosts.o auth-passwd.o sshpty.o sshlogin.o servconf.o
	ar rc p-sshd-auth.a serverloop.o auth.o auth2.o auth-options.o session.o auth2-chall.o
	ar rc p-sshd-auth.a groupaccess.o auth-bsdauth.o auth2-hostbased.o auth2-kbdint.o auth2-none.o auth2-passwd.o
	ar rc p-sshd-auth.a auth2-pubkey.o auth2-pubkeyfile.o auth2-gss.o gss-serv.o gss-serv-krb5.o monitor_wrap.o
	ar rc p-sshd-auth.a auth-krb5.o audit.o audit-bsm.o audit-linux.o platform.o loginrec.o
	ar rc p-sshd-auth.a auth-pam.o auth-shadow.o auth-sia.o sandbox-null.o sandbox-rlimit.o sandbox-darwin.o
	ar rc p-sshd-auth.a sandbox-seccomp-filter.o sandbox-capsicum.o sandbox-solaris.o sftp-server.o sftp-common.o uidswap.o
	ar rc p-sshd-auth.a ssh-pkcs11-client.o ssh-sk-client.o

/ext/bin/sshd-auth: sshd-auth.o p-sshd-auth.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/sshd-auth sshd-auth.o p-sshd-auth.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-ssh-keygen.a: sshsig.o ssh-pkcs11.o ssh-sk-client.o
	ar rc p-ssh-keygen.a sshsig.o ssh-pkcs11.o ssh-sk-client.o

/ext/bin/ssh-keygen: ssh-keygen.o p-ssh-keygen.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/ssh-keygen ssh-keygen.o p-ssh-keygen.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-ssh-keyscan.a: ssh-pkcs11-client.o ssh-sk-client.o
	ar rc p-ssh-keyscan.a ssh-pkcs11-client.o ssh-sk-client.o

/ext/bin/ssh-keyscan: ssh-keyscan.o p-ssh-keyscan.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/ssh-keyscan ssh-keyscan.o p-ssh-keyscan.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-sftp-server.a: sftp-server.o sftp-server-main.o ssherr-nolibcrypto.o
	ar rc p-sftp-server.a sftp-server.o sftp-server-main.o ssherr-nolibcrypto.o

/ext/bin/sftp-server: sftp-common.o p-sftp-server.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/sftp-server sftp-common.o p-sftp-server.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-sftp.a: sftp-usergroup.o progressmeter.o sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o
	ar rc p-sftp.a sftp-usergroup.o progressmeter.o sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o

/ext/bin/sftp: sftp.o p-sftp.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/sftp sftp.o p-sftp.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

p-scp.a: progressmeter.o sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o
	ar rc p-scp.a progressmeter.o sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o

/ext/bin/scp: scp.o p-scp.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a
	$(CC) -o /ext/bin/scp scp.o p-scp.a myos-posix.o myos-auth.o s0.a s1.a s2.a s3.a s4.a c0.a c1.a c2.a c3.a c4.a

/ext/bin/openssh-setup: install.c force
	cc install.c -o /ext/bin/openssh-setup

%.o: %.c config.h myos-posix.h force
	$(CC) -c $< -o $@

.PHONY: all installed force
force:
	@echo
