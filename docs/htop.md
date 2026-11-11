# htop for MyOS

Run `htop` in the console or Terminal. The build automatically installs it in
`/bin`, like other programs in `user/`.

The monitor reads live process and memory statistics, refreshes every second,
and adapts to terminal size (minimum 60 columns; height depends on CPU count).

- Arrows or j/k select a process; Page Up/Down, Home/End scroll.
- P sorts by CPU, M by memory, N by PID, T by CPU time; s cycles columns.
- I reverses the sort; t toggles the process tree; H hides kernel threads.
- / searches; n selects the next match; backslash edits the name filter.
- Delete or uppercase K opens a kill confirmation. Enter confirms; Esc cancels.
- p pauses statistics; h shows help; q or Ctrl+C exits and restores TTY mode.

This is a native htop-style monitor, not a port of upstream htop. MyOS exposes
per-logical-CPU utilization (CPU0, CPU1, ...), process CPU time, PID/PPID, ring, state, name, and
allocated memory. The USER column distinguishes user/kernel execution, not
account ownership; RES uses the kernel's `mem_kb` accounting. Swap is shown as
unavailable. UID, load averages, priority/nice controls,
and Linux signals are not available through the current ABI. Function keys
are not currently delivered by the MyOS keyboard stack; use the listed keys.
