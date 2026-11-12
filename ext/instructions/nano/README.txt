GNU nano 9.3 for MyOS
====================
Upstream: https://www.nano-editor.org/
Source: https://ftp.gnu.org/gnu/nano/nano-9.3.tar.gz
SHA256: 3208f3c459c64e4a4e0ef242d2653572a56ca9a6cfbdc4de5af4714594ae8836
License: GNU GPL version 3 or later; upstream COPYING is installed beside the port.

setup.inst downloads upstream, extracts src and COPYING, applies myos.patch,
and builds the real editor with MyOS cc/make. No prebuilt Linux binaries.
The native package recipe currently relies on HTTPS transport integrity;
SHA256 is verified by the host build/test helper.

The private ANSI curses subset draws changed screen runs, reads real terminal
input, restores terminal modes and detects PTY geometry changes. POSIX shims
adapt existing MyOS syscalls. UTF-8 editing, help, multiple buffers, line
numbers, wrapping and undo/redo are enabled. Unicode width/case tables are
limited (including Cyrillic), not a complete Unicode implementation.

Disabled: syntax colors/nanorc, NLS, file browser, mouse, external tools.
External command execution and suspend explicitly report unsupported.
Signals are cooperative, resize is polled; crash/hangup recovery is not
provided. No fsync guarantee or backup timestamp preservation is simulated.

Usage: ext install nano; nano /path/file
Save: Ctrl+O, Enter. Exit: Ctrl+X. Help: Ctrl+G.
Uninstall removes the executable/license only, preserving edited documents.
Build/runtime/SSH checks: make nano-build nanotest nano-networktest nano-sshtest.
See docs/nano.md in the source repository for details.
