# GPU display and compositor

`make run` selects the display before QEMU opens the guest disks:

* Linux/WSL: when QEMU has `virtio-vga-gl` and SDL, and an EGL/OpenGL probe
  identifies a hardware renderer, use VirGL with SDL/OpenGL.
* WSLg: select the installed Mesa D3D12 driver, unless the caller explicitly
  selected another driver. This avoids silently selecting llvmpipe. The launch
  line prints the renderer, for example `D3D12 (AMD Radeon(TM) Graphics)`.
* macOS, missing GL, or a software GL renderer: use the existing Bochs linear
  framebuffer. macOS does not need VirGL, EGL, or changes to its build tools.

Overrides: `make run GPU=on` requires the hardware path and fails clearly if it
is unavailable; `make run GPU=off` uses software. Explicit `VGA=std` and
`VGA=vmware` retain the previous adapters. `QEMU=...` can select another binary.
`python3 tools/run_qemu.py --print-command -- -m 256M` inspects selection without
opening a guest. There is no automatic restart after guest boot.

## Pointer input

GPU launches also attach `virtio-tablet-pci`. `kernel/virtinput.c` negotiates
modern VirtIO PCI, reads the absolute-axis ranges and consumes a bounded event
queue on PCI interrupts. If the IRQ is unavailable or occupied, it retains a
timer fallback instead of replacing another device's handler. Coordinates map directly to the current guest framebuffer;
motion does not depend on SDL/WSLg relative capture, pointer warping or sign
adjustments. The QEMU window uses absolute input once the guest device is ready.
Button state, wheel steps and coordinates are applied together at SYN_REPORT.
The queue's buffers stay in private kernel memory and are reused after completion.

Existing PS/2 and USB relative input remain available without the tablet. While
the tablet is active, relative reports cannot add a second displacement. Mode
changes rescale the saved absolute coordinates to the new framebuffer bounds.
Mouse and keyboard input wake the screen owner's existing IPC mailbox without
allocating in the interrupt handler or inventing application messages. This
lets the desktop sleep when idle and react when input arrives. App message
batches are bounded so a stream of window updates cannot indefinitely postpone
input processing and presentation.
Restart QEMU to apply this change; an already running VM has no new tablet.

`make mousetest` uses disposable boot/data images and checks both axes, left/right
buttons, wheel, screen corners, resize and repeated queue reuse through QMP.
It tests the GPU/tablet path, tablet with Bochs, and plain PS/2 independently.
The source disk can be overridden with `MYOS_TEST_DISK`; it is only copied.

## Independent cursor

The VirtIO GPU cursor uses a separate split queue and a private 64x64 ARGB
resource. Moving it never submits render commands or waits on a control-queue
fence. In-flight packets are immutable; when the queue fills, the latest pending
position replaces earlier pending movement and is retried from the timer, even
during an unfinished frame. Shape changes upload pixels synchronously, while
position and visibility changes use the independent cursor queue.

The GPU desktop enables it by default when no display preference is saved.
An explicit `cursor=sw`/`cursor=hw` preference remains respected. `mode cursor hw`
or the Display checkbox enables it in an existing configuration. Software cursor
composition remains available. SDL uses the host cursor for absolute input, so
the visible pointer does not wait for guest window rasterization/presentation.

`make mousetest` also checks input wakeups while a GPU frame remains intentionally
unfinished, verifies that the visible frame stays unchanged, and checks QEMU's
cursor-queue trace. Latency is host injection to a guest owner reporting through
serial, not a measurement of physical mouse-to-monitor latency. A comparison on
WSLg measured about 17 ms median before and 11 ms after; the larger practical
cursor improvement is removing its dependence on rendering whole window frames.

The boot stack occupies 0x9b000..0x9f000. A linker assertion prevents static GPU
command buffers from overlapping it or the BIOS video-memory area.

## Guest rendering

`kernel/virtgpu.c` implements modern VirtIO PCI feature negotiation, a polled
split control queue, scatter/gather backing, a VirGL context and fenced GPU
commands. Opaque copies use GPU blits; ARGB layers use immutable textured-quad
shaders and an explicit source-over blend state, because BLIT alpha flags are
not implemented consistently by VirGL. Integer code builds float constants
without using the kernel FPU. It has a VirtIO 2D path for `-vga virtio` as well. Only driver-built
commands are accepted; applications cannot submit arbitrary VirGL streams.

The graphics API provides bounded, owner-only texture upload/blit operations.
Uploads pin their source only until a fenced transfer finishes; resources do
not keep pointers into user address spaces. Texture count, dimensions and total
storage are bounded. Partial updates must match the retained texture dimensions
and fit entirely inside its bounds. Textures are released on owner exit and mode change.

The desktop retains wallpaper, application-content and ARGB decoration textures.
Window clients draw into private pixels. `win_update_rect` copies completed
damage into a shared publication buffer, which stays immutable until the
desktop captures it into its own retained CPU snapshot. A shared sequence
acknowledgement releases the publication even for minimized windows; it cannot
be lost when the input mailbox fills. The application may rasterize its next
frame meanwhile, and a subsequent update waits before reusing the publication.
Resize seeds and transfers a new publication before releasing the old mapping.
This prevents an application's clear/paint pass from appearing as flicker.
The handshake is negotiated when creating a window; legacy clients/desktops
retain their previous layout. Rebuild window applications and restart the
desktop to use the buffered path on both sides.

The dock, panel, menu, tooltip and cursor keep separate small cached textures;
hover, focus, title, clock and layout changes invalidate their appearance.
Trusted draw/blit streams are batched until the next fenced transfer or scanout
change, avoiding a host round trip for each individual layer.
Unchanged window contents remain on the GPU. Client dirty rectangles update
only the corresponding texture rows and columns, preserving their full-buffer
stride. New/resized buffers receive a full upload. The
the GPU composites wallpaper, windows, translucent shadows and shell overlays
in stacking order. Existing CPU vector/font routines rasterize decoration
textures when their appearance changes; applications still rasterize their own
content. This is GPU **composition**, not a new OpenGL API for applications.
Overview and an outstanding resize use software composition, uploaded through
the same display backend. A failed surface allocation switches composition to
software; an actual lost/unresponsive host GPU is not transparently recovered.

Frames are assembled in a hidden GPU texture. Nested `gfx_frame_begin/end`
brackets include window moves and all dirty-region updates. The final end
switches scanout once. The previous render target is synchronized with a GPU
blit, preserving unchanged pixels without a full CPU upload. Overlapping moves
use a distinct scratch texture. Timer updates cannot expose unfinished frames.
This prevents partially assembled guest frames; it does not promise physical
monitor vsync on every host/display configuration.

`gfx_readback` reads the visible GPU scanout for an owning program, including
while another frame is unfinished. QEMU's `screendump` cannot read SDL GL scanout,
so the GPU integration test uses this fenced readback to compare actual pixels.

## Verification

`python3 tests/graphics_launcher_test.py` tests selection, macOS fallback,
overrides and strict GPU mode. `make graphicstest` boots disposable data disks
and tests VirGL, Bochs software and VirtIO 2D. Builds, boot images and data disks
are isolated in `build/graphics-test`, including when the main VM is running. The GPU test checks opaque and
alpha composition, bounds/pointer validation, nested frame boundaries, hidden
versus committed scanout, overlapping copies, resource deletion, multi-packet draw ordering and mode change.
`gpuscene` compares the real desktop compositor against its software reference
for overlapping windows, off-screen clipping, partial uploads, minimization,
shell-state and cursor changes, menu show/hide, and damage outside the current
window size that becomes visible again after resizing. It measures both guest ticks and host
wall time for 100 full frames. The test then loads and scrolls a real browser
page through the desktop and checks that GPU composition remains active. It never modifies the normal `disk.img`.

The GPU test requires a hardware-capable Linux/WSL graphical session. macOS
selection is covered by launcher tests; a native macOS runtime still needs to
be checked on a Mac.

`windowframecheck` uses the real window library and desktop message handler
in separate processes, deliberately delaying capture until the client starts
an unfinished repaint. GPU readback must contain only completed pixels. The
graphics test runs it at 1024x768 and 2560x1600 and checks partial damage,
back-to-back publications, resize, minimized updates and window destruction.

### Recorded hardware run

On WSLg with Mesa D3D12 / AMD Radeon Graphics, `gpuscene` at 1024x768
measured 100 full frames at 6.51 s for software composition/upload and 1.53 s
for retained GPU composition (host wall clock, approximately 4.3x).
All scene comparisons had no channel difference above 2/255 relative to
the software reference. This measures a retained desktop scene; browser layout,
font/vector rasterization and changing page contents still have CPU costs.
