# System information

`sysinfo` in /bin prints kernel identity, CPU, PMM-managed RAM, uptime and graphics.
The fallback kernel shell supports `sysinfo` and `about` too.

VERSION is the single kernel release source. Each kernel build generates
build/build_info.h with the full Git commit, -dirty when local changes exist,
and UTC build time. Archives without Git report unknown; MYOS_BUILD_COMMIT can
supply their source identity. SOURCE_DATE_EPOCH fixes the timestamp for reproducible
builds. Build metadata is embedded in the kernel, not read from a runtime file.

Programs call sysinfo(&info, sizeof(info)); see user/lib/sysinfo.h (ABI v1).
Too-small buffers return -EINVAL, invalid/unwritable pointers return -EFAULT.
A larger buffer is accepted; only the v1 structure is written.

CPUID supplies vendor, brand, family/model/stepping, feature masks and hypervisor.
Intel CPUID 16h supplies nominal base/max CPU MHz; CPUID 15h supplies TSC MHz
when the crystal clock and ratio are available. TSC is not live CPU frequency.
Missing clock data stays zero and displays unknown (common under QEMU).
The online count comes from the SMP subsystem (up to 8 CPUs); reported logical
package capacity is separate. See [SMP](smp.md) for startup and fallback rules.
RAM describes usable memory managed by PMM, not installed DIMM size.

Reference: Intel SDM, CPUID instruction, leaves 15h and 16h:
https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
