/* First SMP implementation: parallel ring 3, serialized kernel, pinned tasks.
 * Legacy IRQs stay on the BSP. PIT broadcasts scheduler IPIs to the APs.
 * No kernel code outside the entry/exit/idle paths executes without the BKL.
 */
#include "smp.h"
#include "task.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "vga.h"

struct cpu_state cpu_states[SMP_MAX_CPUS];
static u8 cpu_map[256];
static u8 cpu_apic[SMP_MAX_CPUS];
static volatile u32 online = 1;
static volatile u32 kernel_lock;
static volatile u32 owner;
static bool enabled;
static volatile u32 *lapic;
static volatile u32 arrived;
static u32 ebda_base;
static u32 mapping_generation = 1;
extern u8 ap_trampoline[], ap_trampoline_end[];

void smp_early_init(void) {
    u16 segment;
    __asm__ volatile("movw 0x40e,%0":"=r"(segment));
    ebda_base=(u32)segment<<4;
}

static u32 apic_id(void) {
    u32 a=1,b,c,d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b >> 24;
}
u32 smp_cpu(void) {
    u16 selector;
    __asm__ volatile("mov %%gs,%0":"=r"(selector));
    if (selector == 0x30) {
        u32 index;
        __asm__ volatile("movl %%gs:0,%0":"=r"(index));
        return index;
    }
    /* Only early bootstrap/AP startup runs without the CPU-local segment. */
    return enabled ? cpu_map[apic_id()] : 0;
}
void smp_kernel_mapping_changed(void) { mapping_generation++; }
u32 smp_count(void) { return online; }
bool smp_task_active(struct task *t) {
    for (u32 i=0;i<online;i++) if (cpu_states[i].running==t) return true;
    return false;
}
static volatile u32 user_tlb_generation;
static volatile u32 user_tlb_seen[SMP_MAX_CPUS];
static void poll_user_tlb(void) {
    u32 c=smp_cpu(), g=user_tlb_generation;
    if (user_tlb_seen[c]!=g) {
        u32 cr3;
        __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3) :: "memory");
        __sync_synchronize(); user_tlb_seen[c]=g;
    }
}
void smp_enter(void) {
    if (!enabled) return;
    poll_user_tlb();
    u32 me=smp_cpu()+1;
    if (owner==me) return;
    while (__sync_lock_test_and_set(&kernel_lock,1))
        while (kernel_lock) { poll_user_tlb(); __asm__ volatile("pause" ::: "memory"); }
    owner=me;
    poll_user_tlb();
    /* Kernel mappings are shared, but most entries change no mapping.
     * Only flush when another serialized kernel operation changed them. */
    struct cpu_state *local = &cpu_states[me-1];
    if (local->tlb_generation != mapping_generation) {
        u32 cr3;
        __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3) :: "memory");
        local->tlb_generation = mapping_generation;
    }
}
static void unlock(void) {
    if (!enabled) return;
    owner=0;
    __sync_lock_release(&kernel_lock);
}
void smp_leave(struct regs *r) { if (r->cs & 3) unlock(); }
void smp_idle(void) {
    __asm__ volatile("cli" ::: "memory");
    unlock();
    __asm__ volatile("sti; hlt; cli" ::: "memory");
    smp_enter();
    __asm__ volatile("sti" ::: "memory");
}
static void write_apic(u32 reg,u32 value) { lapic[reg/4]=value; (void)lapic[0x20/4]; }
void smp_eoi(void) { if(lapic) write_apic(0xb0,0); }
static bool ipi(u32 id,u32 command) {
    for(u32 n=0;n<1000000;n++) {
        if(!(lapic[0x300/4]&0x1000)) {
            write_apic(0x310,id<<24); write_apic(0x300,command); return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}
void smp_user_tlb_flush(u32 dir) {
    if (!enabled || online<2) return;
    bool remote=false;
    for (u32 c=0;c<online;c++) {
        struct task *t=cpu_states[c].running;
        if (c!=smp_cpu() && t && t->page_dir==dir) remote=true;
    }
    if (!remote) return;
    u32 flags=irq_save();
    u32 generation=++user_tlb_generation;
    __sync_synchronize();
    poll_user_tlb();
    for (u32 c=0;c<online;c++) if(c!=smp_cpu()) {
        if (!ipi(cpu_apic[c],0x83)) kpanic("TLB IPI delivery failed");
    }
    for (u32 c=0;c<online;c++)
        while (user_tlb_seen[c]!=generation) __asm__ volatile("pause" ::: "memory");
    irq_restore(flags);
}
void smp_tick(void) { if(online>1) ipi(0,0xc0000|0x82); }
void smp_wake(u32 cpu) {
    cpu_states[cpu].resched=true;
    if(enabled && cpu<online && cpu!=smp_cpu() &&
       cpu_states[cpu].running==cpu_states[cpu].idle) {
        /* Publish READY and resched before notifying the idle target CPU. */
        __sync_synchronize();
        ipi(cpu_apic[cpu],0x83);
    }
}
static void local_init(bool bsp) {
    u32 lo,hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
    lo=((u32)lapic&0xfffff000)|(lo&0x100)|0x800;
    __asm__ volatile("wrmsr"::"a"(lo),"d"(0),"c"(0x1b));
    write_apic(0xf0,0x100|0xff);
    write_apic(0x80,0);
    write_apic(0x320,0x10000); /* no local timer: BSP PIT supplies IPIs */
    write_apic(0x350,bsp ? 0x700 : 0x10000);
    write_apic(0x360,0x10000);
    write_apic(0x370,0x10000);
    smp_eoi();
}
static bool checksum(const void *p,u32 n) {
    const u8 *s=p; u8 sum=0;
    while(n--) sum+=*s++;
    return sum==0;
}
static u32 scan_rsdp(u32 start,u32 end) {
    for(u32 a=start;a+20<=end;a+=16)
        if(!memcmp((void *)a,"RSD PTR ",8) && checksum((void *)a,20)) return a;
    return 0;
}
static bool table_ok(u32 a) {
    if(a<0x1000 || a>paging_identity_limit()-36) return false;
    u32 len=*(u32 *)(a+4);
    return len>=36 && len<=0x100000 && len<=paging_identity_limit()-a && checksum((void *)a,len);
}
static u32 discover(u8 ids[SMP_MAX_CPUS],u32 *address) {
    u32 rsdp=scan_rsdp(0xe0000,0x100000);
    if(!rsdp) {
        u32 base=ebda_base;
        if(base>=0x80000 && base<0xa0000) rsdp=scan_rsdp(base,base+1024);
    }
    if(!rsdp) return 1;
    u32 root=*(u32 *)(rsdp+16);
    if(!table_ok(root) || memcmp((void *)root,"RSDT",4)) return 1;
    u32 count=1; ids[0]=apic_id();
    for(u32 off=36;off+4<=*(u32 *)(root+4);off+=4) {
        u32 a=*(u32 *)(root+off);
        if(!table_ok(a) || memcmp((void *)a,"APIC",4)) continue;
        u32 len=*(u32 *)(a+4);
        if(len<44) continue;
        *address=*(u32 *)(a+36);
        for(u32 p=a+44;p+2<=a+len;) {
            u8 *e=(u8 *)p;
            if(e[1]<2 || e[1]>a+len-p) return 1;
            if(e[0]==0 && e[1]>=8 && (*(u32 *)(e+4)&1) && e[3]!=ids[0]) {
                bool duplicate=false;
                for(u32 j=1;j<count;j++) if(ids[j]==e[3]) duplicate=true;
                if(!duplicate && count<SMP_MAX_CPUS) ids[count++]=e[3];
            }
            /* LAPIC address override: this 32-bit kernel needs an address below 4G. */
            if(e[0]==5 && e[1]>=12) {
                if(*(u32 *)(e+8)) return 1;
                *address=*(u32 *)(e+4);
            }
            p+=e[1];
        }
        return count;
    }
    return 1;
}
static void delay(u32 ticks) {
    u32 start=timer_ticks();
    while(timer_ticks()-start<ticks) __asm__ volatile("pause" ::: "memory");
}
static void ap_main(void) {
    gdt_init(); idt_reload();
    /* fpu_init also writes a shared clean template; use the same BKL as BSP. */
    local_init(false);
    arrived=1;
    smp_enter();
    if(smp_cpu()>=online) {
        /* A timed-out AP may arrive late. Keep its stack allocated and park
         * it; do not allow an uncounted CPU into the task table. */
        unlock();
        for(;;) __asm__ volatile("cli; hlt");
    }
    fpu_init();
    tasking_init_ap();
    for(;;) { task_yield(); smp_idle(); }
}
void smp_init(void) {
    u32 original,changed;
    __asm__ volatile("pushfl; popl %0":"=r"(original));
    changed=original^(1u<<21);
    __asm__ volatile("pushl %0; popfl"::"r"(changed):"cc");
    __asm__ volatile("pushfl; popl %0":"=r"(changed));
    __asm__ volatile("pushl %0; popfl"::"r"(original):"cc");
    if(!((original^changed)&(1u<<21))) { kprintf("SMP: 1 CPU (no CPUID)\n"); return; }
    u32 a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    if((d&((1u<<9)|(1u<<5)))!=((1u<<9)|(1u<<5))) { kprintf("SMP: 1 CPU (no APIC/MSR)\n"); return; }
    u8 ids[SMP_MAX_CPUS]={0}; u32 address=0;
    u32 count=discover(ids,&address);
    if(count<2 || !address || (address&4095)) { kprintf("SMP: 1 CPU (no additional MADT CPUs)\n"); return; }
    if((u32)(ap_trampoline_end-ap_trampoline)>0xc00) { kprintf("SMP: trampoline too large\n"); return; }
    if(vmm_map(address,address,PG_WRITE|0x18)) { kprintf("SMP: 1 CPU (cannot map LAPIC)\n"); return; } /* uncacheable APIC MMIO */
    lapic=(volatile u32 *)address;
    u32 lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
    if(lo&(1u<<10)) { lapic=NULL; kprintf("SMP: x2APIC mode unsupported\n"); return; }
    lo=(address&0xfffff000)|(lo&0x100)|0x800;
    __asm__ volatile("wrmsr"::"a"(lo),"d"(0),"c"(0x1b));
    for(u32 i=0;i<count;i++) { cpu_map[ids[i]]=i; cpu_apic[i]=ids[i]; }
    memcpy((void *)0x7000,ap_trampoline,ap_trampoline_end-ap_trampoline);
    kernel_lock=1; owner=1; enabled=true;
    local_init(true);
    for(u32 i=1;i<count;i++) {
        void *stack=kmalloc(TASK_STACK);
        if(!stack) break;
        volatile u32 *mail=(volatile u32 *)0x7f00;
        mail[0]=(u32)stack+TASK_STACK; mail[1]=vmm_kernel_dir(); mail[2]=(u32)ap_main;
        arrived=0; __sync_synchronize();
        ipi(ids[i],0xc500); delay(2); /* INIT assert, >=10 ms */
        ipi(ids[i],0x8500); delay(1); /* INIT deassert */
        ipi(ids[i],0x607); delay(1); ipi(ids[i],0x607);
        u32 start=timer_ticks();
        while(!arrived && timer_ticks()-start<TIMER_HZ) __asm__ volatile("pause" ::: "memory");
        if(!arrived) { kprintf("SMP: CPU APIC %u startup timeout\n",ids[i]); break; }
        online=i+1;
    }
    kprintf("SMP: %u CPUs online (parallel userspace, serialized kernel)\n",online);
}
