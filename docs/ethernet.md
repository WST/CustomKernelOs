# Intel Ethernet on hardware and QEMU

The polling driver shares the existing Ethernet/ARP/IPv4/DHCP/TCP stack across
these explicitly selected Intel PCI devices:

| Controller | PCI vendor:device | Initialization |
| --- | --- | --- |
| 82540EM / QEMU `e1000` | `8086:100e` | Legacy MAC |
| 82574L / QEMU `e1000e` | `8086:10d3` | PCIe MAC, coherent DMA |
| I219-V, 100/200-series PCH | `8086:1570`, `8086:15b8`, `8086:15d8`, `8086:15d6` | PCH firmware/PHY path |

MSI specifies I219-V for the B250M GAMING PRO. The driver checks all functions
on each populated PCI device and all legacy PCI buses, including the integrated
PCH LAN function at `00:1f.6`. An unsupported Intel Ethernet ID is logged instead
of being treated as an interchangeable e1000 adapter.

## I219-V startup

The PCI power-management capability is walked with a fixed limit; a controller
left in D3 is brought back to D0 before MMIO access. BAR0 must be a page-aligned
memory BAR below 4 GiB. MAC registers are mapped uncached. Initialization masks
interrupts, stops receive/transmit and drains PCIe master requests before reset.
The PCH path requests ME's exit from ultra-low-power state before acquiring
the PHY software semaphore. Firmware ownership, MDIO completion, register
echoes and every wait have bounded checks.

The MAC-only reset preserves the firmware's PHY calibration. Standard MII
registers use PHY address 2; vendor pages use address 1 and the page-select
register. The software-managed path clears ULP/forced-SMBus state and starts
10/100/1000 auto-negotiation without advertising pause. PHY power-cycling is
attempted only if the firmware grants reset permission. If firmware holds PHY
configuration, its existing negotiation is retained. The driver does not write
GbE flash or NVM.

Both descriptor rings and every buffer are allocated before enabling bus
mastering or RX/TX. DMA publication/completion uses memory barriers. Failed
allocation frees pages before DMA is enabled. RX drops bad, oversized and
multi-descriptor frames without copying their lengths beyond a buffer. The
PCH multicast table has 32 registers; the legacy MAC has 128. Only the factory
unicast MAC and broadcasts are accepted. Processing remains polling at 10 ms;
MSI/interrupt routing is not needed for this initial backend.

82540EM uses legacy RX descriptors; 82574L/I219 use 16-byte extended receive
writebacks, with status/error at byte 8 and length at byte 12. Recycling restores
the buffer address overwritten by hardware, clears the writeback and publishes
it with a barrier. RX starts with an empty tail; the ready descriptors are
published after RCTL enables receive. Ring wrap tests cover both formats.

The PCH path allocates a 26 KiB receive packet buffer before MAC reset, clears
the PHY receive gate when firmware permits configuration, and applies the
SPT/Kaby Lake polling/legacy clock workarounds (FEXTNVM7/FEXTNVM9). TARC limits
outstanding transmit requests to two for the same silicon family. Boot logs
show PHY receive control, descriptor mode, RCTL, RFCTL and PBA. Timeout snapshots
also show RXDCTL, RAL/RAH, management routing and sideband-clock controls.

Link-up and speed changes now program the PHY EMI receive configuration,
PLL gate interval (250 at 1 Gb/s, 1000 at 10/100), the gigabit K1 clock request,
and the SPT FIFO pointer gap (at least 24 at 1 Gb/s). The MAC IPG, K1 beacon
duration and board-selected SPT K1-off setting are updated at the same time.
Inherited EEE advertisement and LPI are disabled before autonegotiation because
this backend does not manage EEE power transitions. These operations hold the
PHY semaphore and preserve firmware-owned configuration and unrelated fields.
Timeout diagnostics include live PHY gate/PM/PLL/FIFO/LPI/SMBus/EMI state,
MAC control, descriptor base/length, delay timers and receive error counters.
The additional hardware counters are also clear-on-read.

The software-owned PHY path also exits retained host wake and active
power-down mode through page 769 register 17. It enables MACPD access before
clearing Host_WU_Active, then clears Active_PD/MACPD and verifies the result.
Other register bits and firmware-owned configuration are preserved. The LPI
auto-enable bit is cleared together with the speed-specific LPI enables.
Logs report host wake control before/after and live host/BMCR/BMSR/RXERR
values, so an ordinary RX gate clear is not mistaken for exiting wake mode.

## Address assignment and diagnostics

The background network task waits for physical link, requests DHCP and retries
after timeout. It reacquires a DHCP address on reconnect and does not invent
QEMU's `10.0.2.15` address when a real router has not answered. `ifconfig` shows
"no address yet" while waiting. Loopback processing continues while link is down.

Kernel/network messages are retained from early boot in a 16 KiB memory ring.
Network retries and register dumps do not draw on the console; serial debug
output still includes them. `dmesg` reads the ring directly through a checked
system call, without requiring a mounted filesystem. Reading does not consume
the shared history: each viewer has an independent 64-bit byte cursor.
Use `dmesg -n 50` for the last 50 lines, `dmesg -w` to follow until Ctrl+C,
or `dmesg -n 0 -w` for only new messages. Slow readers resume at the oldest
retained data and report overwritten bytes. History is lost on reboot.
No kernel log file or filesystem writer task is created automatically. To save
a snapshot explicitly, use `dmesg > /tmp/boot.log` (live RAM root), or redirect
to a file on a mounted persistent volume. User terminal output and password
input are not captured.

Log messages include:

* `NET: Intel I219-V PCI ... id=... MMIO=...`
* `NET: I219 PHY id=... FWSM=...`
* `NET: RX/TX ready...`
* `NET: link up, ... Mbps ... duplex` or `NET: link down...`
* `NET: DHCP configured ...` or a specific ownership/MDIO/ULP/power failure.

DHCP requests are padded to at least 300 BOOTP bytes, including a stable
Ethernet client ID and an elapsed-seconds field. The client waits 4/8/16 seconds
instead of abandoning each exchange after one second. OFFER starts a separate
ACK window; an ACK timeout retransmits REQUEST with the same transaction ID,
and NAK restarts discovery. OFFER/ACK/NAK and rejection counts are logged.

On timeout the driver prints queued TX frames, busy descriptors, TDH/TDT,
RDH/RDT, accepted/dropped RX frames and raw GPTC/GPRC/CRC counters. This is a
passive snapshot, not a controller reset. The three hardware counters are
clear-on-read, so subsequent snapshots show activity since the previous read;
software queued/accepted counts remain cumulative. These distinguish no server
reply from a TX/RX DMA problem without guessing from "DHCP pending" alone.

For a hardware check, plug an Ethernet cable into a DHCP-enabled router before
booting `build/myos-usb.img`, then run `ifconfig` and `ping` against the gateway
shown there. The integrated I219-V PHY has not been tested on physical hardware
in this environment. QEMU does not emulate it; successful emulator tests do not
establish compatibility with a particular PHY/firmware revision. A boot photo
including the `NET:` messages is needed to diagnose a hardware failure.

## Verification

`make ethernettest` runs the real driver against a PCI/MMIO/MDIO model under
ASan/UBSan: function 6 and bridged-bus discovery, D3/D0, PHY/ME ownership and ULP,
timeouts, invalid MAC/BAR, every allocation failure, RX/TX wrap, padding,
oversized/error/fragmented receives. This checks software behavior, not the
electrical behavior or timing of a physical I219.

`make dhcptest` runs the real DHCP packet serializer and receive/state-machine
path under ASan/UBSan with a server that enforces the 300-byte minimum, delays
OFFER/ACK, returns NAK and drops responses. It checks REQUEST retransmission,
invalid replies and propagation of a full TX queue as `EAGAIN`.

`make ethernet-smoke` builds a USB image and boots disposable snapshots with
QEMU `e1000` and `e1000e`. It checks address absence with the cable down, DHCP,
ARP/ICMP, reconnect and 40 exact HTTP file transfers per model (ring reuse).
The normal data disk and the USB image are checked for unchanged hashes.

Register/protocol references:

* [MSI B250M GAMING PRO specifications](https://www.msi.com/Motherboard/B250M-GAMING-PRO/Specification)
* [Intel I219 datasheet](https://www.intel.com/content/www/us/en/content-details/612523/intel-ethernet-connection-i219-datasheet.html)
* [Intel 200-series PCH datasheet](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/200-series-chipset-pch-datasheet-vol-1.pdf)
* [RFC 1542 BOOTP consistency checks](https://www.rfc-editor.org/rfc/rfc1542.html#section-2.1)
* [Linux e1000e register and initialization reference](https://github.com/torvalds/linux/tree/v6.6/drivers/net/ethernet/intel/e1000e)

`make klogtest` checks independent readers, overwrite recovery, cursor ABI
validation and sequence wrap beyond 4 GiB. `make dmesgtest` checks line
selection, partial output, independent viewers and follow after ring overwrite.
Both run under ASan/UBSan. The Ethernet smoke runs the installed viewer,
explicit file redirection, live follow through reconnect and Ctrl+C.
