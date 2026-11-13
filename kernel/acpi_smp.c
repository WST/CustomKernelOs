/* ACPI RSDP/RSDT/XSDT/MADT, independent of the kernel's identity RAM limit.
 * Layout: ACPI specification, sections 5.2.5, 5.2.7, 5.2.8 and 5.2.12. */
#include "acpi_smp.h"
#include "string.h"

static u32 get32(const u8 *p) {
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static bool sum_ok(const u8 *p,u32 n) {
    u8 sum=0; while(n--) sum+=*p++; return sum==0;
}
static const u8 *map_range(acpi_smp_map_fn map,void *ctx,u32 address,u32 length,unsigned slot) {
    if(address<0x1000 || !length || length>ACPI_SMP_TABLE_MAX || length-1>0xffffffffu-address) return NULL;
    return map(address,length,slot,ctx);
}
static const u8 *table(acpi_smp_map_fn map,void *ctx,u32 address,unsigned slot,const char *signature) {
    const u8 *p=map_range(map,ctx,address,36,slot);
    if(!p || memcmp(p,signature,4)) return NULL;
    u32 length=get32(p+4);
    if(length<36) return NULL;
    p=map_range(map,ctx,address,length,slot);
    return p && sum_ok(p,length)?p:NULL;
}
static void add_cpu(struct acpi_smp_info *info,u8 seen[256],u32 id,u32 flags) {
    if(!(flags&1)) return; /* Online Capable alone requires CPU hotplug. */
    if(id>=255) { info->unaddressable++; return; } /* xAPIC physical broadcast ID */
    if(seen[id]) return;
    seen[id]=1; info->reported++;
    if(info->count<SMP_MAX_CPUS) info->ids[info->count++]=(u8)id;
}
static enum acpi_smp_error madt(const u8 *p,struct acpi_smp_info *info) {
    u32 length=get32(p+4);
    if(length<44) return ACPI_SMP_BAD_MADT;
    u8 seen[256]={0}; seen[info->ids[0]]=1;
    info->lapic=get32(p+36);
    for(u32 off=44;off<length;) {
        if(length-off<2) return ACPI_SMP_BAD_MADT;
        const u8 *entry=p+off; unsigned size=entry[1];
        if(size<2 || size>length-off) return ACPI_SMP_BAD_MADT;
        switch(entry[0]) {
        case 0:
            if(size<8) return ACPI_SMP_BAD_MADT;
            add_cpu(info,seen,entry[3],get32(entry+4)); break;
        case 5:
            if(size<12) return ACPI_SMP_BAD_MADT;
            if(get32(entry+8)) return ACPI_SMP_HIGH_ADDRESS;
            info->lapic=get32(entry+4); break;
        case 9:
            if(size<16) return ACPI_SMP_BAD_MADT;
            add_cpu(info,seen,get32(entry+4),get32(entry+8)); break;
        default: break;
        }
        off+=size;
    }
    if(!info->lapic || (info->lapic&4095)) return ACPI_SMP_BAD_MADT;
    return ACPI_SMP_OK;
}
static enum acpi_smp_error root_table(u32 address,bool xsdt,acpi_smp_map_fn map,void *ctx,
                                     const struct acpi_smp_info *initial,struct acpi_smp_info *out) {
    const u8 *root=table(map,ctx,address,ACPI_SMP_ROOT_SLOT,xsdt?"XSDT":"RSDT");
    if(!root) return ACPI_SMP_BAD_ROOT;
    u32 length=get32(root+4),stride=xsdt?8:4;
    if((length-36)%stride) return ACPI_SMP_BAD_ROOT;
    enum acpi_smp_error error=ACPI_SMP_NO_MADT;
    for(u32 off=36;off<length;off+=stride) {
        if(xsdt && get32(root+off+4)) { error=ACPI_SMP_HIGH_ADDRESS; continue; }
        u32 physical=get32(root+off);
        const u8 *header=map_range(map,ctx,physical,36,ACPI_SMP_CHILD_SLOT);
        if(!header || memcmp(header,"APIC",4)) continue;
        const u8 *p=table(map,ctx,physical,ACPI_SMP_CHILD_SLOT,"APIC");
        if(!p) { error=ACPI_SMP_BAD_MADT; continue; }
        struct acpi_smp_info candidate=*initial;
        candidate.root=address; candidate.madt=physical; candidate.xsdt=xsdt;
        error=madt(p,&candidate);
        if(error==ACPI_SMP_OK) { *out=candidate; return error; }
    }
    return error;
}
enum acpi_smp_error acpi_smp_discover(u32 rsdp,u8 bsp,acpi_smp_map_fn map,void *ctx,struct acpi_smp_info *out) {
    memset(out,0,sizeof(*out)); out->rsdp=rsdp;
    out->count=out->reported=1; out->ids[0]=bsp;
    const u8 *p=map_range(map,ctx,rsdp,20,ACPI_SMP_ROOT_SLOT);
    if(!p || memcmp(p,"RSD PTR ",8) || !sum_ok(p,20)) return ACPI_SMP_BAD_RSDP;
    u32 rsdt=get32(p+16),xsdt=0; bool high_xsdt=false;
    if(p[15]>=2) {
        p=map_range(map,ctx,rsdp,36,ACPI_SMP_ROOT_SLOT);
        if(!p) return ACPI_SMP_BAD_RSDP;
        u32 length=get32(p+20);
        if(length<36 || length>4096) return ACPI_SMP_BAD_RSDP;
        p=map_range(map,ctx,rsdp,length,ACPI_SMP_ROOT_SLOT);
        if(!p || !sum_ok(p,length)) return ACPI_SMP_BAD_RSDP;
        xsdt=get32(p+24); high_xsdt=get32(p+28)!=0;
    }
    struct acpi_smp_info initial=*out;
    enum acpi_smp_error error=high_xsdt?ACPI_SMP_HIGH_ADDRESS:ACPI_SMP_BAD_ROOT;
    if(xsdt && !high_xsdt) {
        error=root_table(xsdt,true,map,ctx,&initial,out);
        if(error==ACPI_SMP_OK) return error;
    }
    /* A valid 32-bit RSDT remains usable when an XSDT is corrupt/unaddressable. */
    if(rsdt) error=root_table(rsdt,false,map,ctx,&initial,out);
    return error;
}
const char *acpi_smp_error_string(enum acpi_smp_error error) {
    switch(error) {
    case ACPI_SMP_OK: return "OK";
    case ACPI_SMP_BAD_RSDP: return "invalid RSDP/checksum";
    case ACPI_SMP_BAD_ROOT: return "invalid/unmapped RSDT/XSDT";
    case ACPI_SMP_NO_MADT: return "MADT not found";
    case ACPI_SMP_BAD_MADT: return "invalid MADT/checksum/entries";
    case ACPI_SMP_HIGH_ADDRESS: return "ACPI/APIC address above supported 32-bit range";
    default: return "unknown ACPI error";
    }
}
