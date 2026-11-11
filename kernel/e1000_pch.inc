/* Intel 100/200-series PCH + I219-V. Register/MDIO definitions follow Intel
 * I219/200-series PCH documentation. Only volatile MAC/PHY state is changed;
 * the GbE NVM/flash region is never written. PHY calibration is retained. */
#define PCH_EXTCNF 0x0f00
#define PCH_FWSM 0x5b54
#define PCH_CTRL_EXT 0x0018
#define PCH_MDIC 0x0020
#define PCH_H2ME 0x5b50
#define PCH_SWFLAG 0x20u

static bool pch_own(void) {
    if (!wait_bits(PCH_EXTCNF,PCH_SWFLAG,0,100)) {
        klogf("NET: PHY ownership busy, FWSM=%x EXTCNF=%x\n",rd(PCH_FWSM),rd(PCH_EXTCNF));
        return false;
    }
    wr(PCH_EXTCNF,rd(PCH_EXTCNF)|PCH_SWFLAG);
    if (wait_bits(PCH_EXTCNF,PCH_SWFLAG,PCH_SWFLAG,1000)) return true;
    wr(PCH_EXTCNF,rd(PCH_EXTCNF)&~PCH_SWFLAG);
    klogf("NET: PHY ownership request timed out\n");
    return false;
}
static void pch_release(void) { wr(PCH_EXTCNF,rd(PCH_EXTCNF)&~PCH_SWFLAG); }

/* Called only while SWFLAG is owned. Page selection is through PHY address 1;
 * standard MII registers use address 2, vendor pages >=768 use address 1. */
static bool mdio(unsigned address,unsigned reg,bool write,u16 *data) {
    u32 request=(address<<21)|(reg<<16)|(write?0x04000000u:0x08000000u);
    if (write) request|=*data;
    wr(PCH_MDIC,request);
    for (unsigned i=0;i<2048;i++) {
        u32 result=rd(PCH_MDIC);
        if (result&0x10000000u) {
            if ((result&0x40000000u) || ((result>>16)&31)!=reg) {
                klogf("NET: MDIO failed addr=%u reg=%u result=%x\n",address,reg,result);
                return false;
            }
            if (!write) *data=result&0xffff;
            return true;
        }
        if ((i&63)==63) sleep_ms(1);
    }
    klogf("NET: MDIO timeout addr=%u reg=%u\n",address,reg);
    return false;
}
static bool phy_read(unsigned page,unsigned reg,u16 *value) {
    if (reg>15) {
        u16 select=page<<5;
        if (!mdio(1,31,true,&select)) return false;
    }
    return mdio(page>=768?1:2,reg,false,value);
}
static bool phy_write(unsigned page,unsigned reg,u16 value) {
    if (reg>15) {
        u16 select=page<<5;
        if (!mdio(1,31,true,&select)) return false;
    }
    return mdio(page>=768?1:2,reg,true,&value);
}
static bool phy_change(unsigned page,unsigned reg,u16 clear,u16 set) {
    u16 value;
    return phy_read(page,reg,&value) && phy_write(page,reg,(value&~clear)|set);
}
static bool phy_identity(u32 *identity) {
    u16 a,b;
    if (!phy_read(0,2,&a) || !phy_read(0,3,&b)) return false;
    *identity=((u32)a<<16)|b;
    return *identity && *identity!=0xffffffffu && a!=0xffff && a!=0;
}
static bool pch_wake_me(void) {
    if (rd(PCH_FWSM)&0x8000) {
        /* A running ME owns ULP configuration. Request exit, never override it. */
        wr(PCH_H2ME,(rd(PCH_H2ME)&~0x800u)|0x1000u);
        bool done=wait_bits(PCH_FWSM,0x400,0,2500);
        wr(PCH_H2ME,rd(PCH_H2ME)&~0x1000u);
        if (!done) { klogf("NET: ME ULP exit timed out, FWSM=%x\n",rd(PCH_FWSM));return false; }
    }
    return true;
}
/* EMI address/data live on PHY address 2, page zero. Page selection must
 * not inherit the last address-1 vendor page. Caller owns SWFLAG. */
static bool emi_read(u16 address,u16 *value) {
    return phy_write(0,16,address) && phy_read(0,17,value);
}
static bool emi_write(u16 address,u16 value) {
    return phy_write(0,16,address) && phy_write(0,17,value);
}
static void pch_phy_diagnose(void) {
    if (!pch_own()) return;
    u16 gate,pm,pll,fifo,lpi,smb,rx_config,host,bmcr,bmsr,rx_errors;
    bool ok=phy_read(769,20,&gate) && phy_read(770,17,&pm) &&
        phy_read(772,28,&pll) && phy_read(776,20,&fifo) &&
        phy_read(772,20,&lpi) && phy_read(769,23,&smb) &&
        emi_read(0xb20c,&rx_config) && phy_read(769,17,&host) &&
        phy_read(0,0,&bmcr) && phy_read(0,1,&bmsr) &&
        phy_read(0,1,&bmsr) && phy_read(0,20,&rx_errors);
    pch_release();
    if (ok)klogf("NET: PHY RX gate=%x PM=%x PLL=%x FIFO=%x LPI=%x SMB=%x EMI_RX=%x\n",
        gate,pm,pll,fifo,lpi,smb,rx_config);
    if(ok)klogf("NET: PHY host=%x BMCR=%x BMSR=%x RXERR=%u\n",host,bmcr,bmsr,rx_errors);
}
/* Run on each link-up/speed change, before the first DHCP frame. The PHY
 * power/FIFO settings are speed-dependent even though MAC STATUS already
 * reports link up. Preserve unrelated calibration and firmware-owned state. */
static bool pch_link_start(u32 status) {
    if (!(rd(PCH_FWSM)&0x40))return true;
    if (!pch_own())return false;
    bool gigabit=(status&0x80)!=0;
    u16 fifo;
    bool ok=emi_write(0xb20c,(!(status&1) && !(status&0xc0))?0:1) &&
        phy_change(772,28,0x07ff,gigabit?0x00fa:0x03e8) &&
        (!gigabit || phy_change(770,17,0,0x0200)) &&
        phy_read(776,20,&fifo);
    if (ok) {
        if (gigabit) {
            unsigned gap=(fifo>>2)&0x3ff;
            if(gap<0x18)ok=phy_write(776,20,(fifo&~0x0ffc)|(0x18<<2));
        } else ok=phy_write(776,20,0xc023);
    }
    pch_release();
    wr(0x24,(rd(0x24)&~7u)|7u); /* 8 us K1 beacon for I219 */
    /* SPT K1-off configuration follows the board's analog configuration. */
    wr(0x10,(rd(0x10)&~0x80000000u)|(rd(0xf18)&0x80000000u));
    u32 ipg=(!(status&1) && !(status&0xc0))?0xff:gigabit?8:12;
    wr(REG_TIPG,(rd(REG_TIPG)&~0x3ffu)|ipg);
    if (!ok)klogf("NET: I219 link PHY setup failed\n");
    else klogf("NET: I219 link PHY configured, %u Mbps\n",gigabit?1000u:(status&0x40)?100u:10u);
    return ok;
}
/* Exit host wake filtering left by firmware/another OS. These PHY bits
 * survive ordinary MAC resets. MACPD must first unlock Host_WU_Active writes.
 * SWFLAG is held; firmware-owned PHY configuration is checked by the caller. */
static bool pch_host_active(void) {
    u16 original,current;
    if(!phy_read(769,17,&original))return false;
    if(!phy_write(769,17,original|4u) ||
       !phy_write(769,17,(original|4u)&~0x50u) ||
       !phy_write(769,17,original&~0x54u) ||
       !phy_read(769,17,&current))return false;
    klogf("NET: PHY host wake control=%x -> %x\n",original,current);
    if(current&0x54u) {
        klogf("NET: PHY host wake/power-down exit failed\n");return false;
    }
    return true;
}
static bool pch_phy_start(void) {
    u32 fw=rd(PCH_FWSM),identity;
    klogf("NET: starting I219 PHY, FWSM=%x\n",fw);
    if (!pch_own()) return false;
    bool ok=false;
    if (!phy_identity(&identity)) {
        wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)|0x800);
        sleep_ms(50);
        if (!phy_identity(&identity)) {
            /* LANPHYPC cycling is allowed only when firmware permits reset. */
            if (!(fw&0x40)) { klogf("NET: PHY inaccessible; firmware blocks power-cycle\n");goto out; }
            wr(0x3c,(rd(0x3c)&~0x0c000000u)|0x08000000u);
            u32 control=rd(REG_CTRL);
            wr(REG_CTRL,(control|0x10000u)&~0x20000u);
            (void)rd(REG_STATUS);sleep_ms(1);
            wr(REG_CTRL,control&~0x10000u);
            sleep_ms(100);
            wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)&~0x800u);
            if (!phy_identity(&identity)) { klogf("NET: PHY ID unavailable after power-cycle\n");goto out; }
        }
    }
    klogf("NET: I219 PHY id=%x FWSM=%x\n",identity,fw);
    u16 receive_control;
    if (!phy_read(769,20,&receive_control))goto out;
    klogf("NET: I219 PHY receive control=%x\n",receive_control);
    if (!(fw&0x40)) {
        klogf("NET: firmware holds PHY configuration; preserving negotiation\n");
        ok=true;goto out;
    }
    if(!pch_host_active())goto out;
    if (!(fw&0x8000)) {
        /* Clear PHY/MAC SMBus forcing and software ULP configuration. */
        if (!phy_change(769,23,1,0)) goto out;
        wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)&~0x800u);
        if (!phy_change(770,17,0,0x4000)) goto out;
        if (!phy_change(779,16,0x1d75,0)) goto out;
        if (!phy_change(779,16,0,1)) goto out;
        wr(0xe4,rd(0xe4)&~0x20u);
    }
    wr(0xf10,rd(0xf10)&~0x4eu); /* D0 low power / GbE-disable controls */
    /* Firmware/previous jumbo setup can leave the PHY receive path gated. */
    if (!phy_change(769,20,1u<<14,0))goto out;
    /* This polling backend does not implement EEE/LPI transitions. Disable
     * inherited LPI and EEE advertisement before restarting negotiation. */
    u16 eee;
    if (!phy_change(772,20,0x6800,0) || !emi_read(0x8001,&eee) ||
        !emi_write(0x8001,eee&~6u))goto out;
    /* Advertise 10/100 and 1000 full duplex, with pause disabled in MAC/PHY. */
    if (!phy_change(0,4,0x0de0,0x01e1) || !phy_change(0,9,0x1900,0x0200) ||
        !phy_change(0,0,0xcc00,0x1200)) goto out;
    ok=true;
out:
    pch_release();
    return ok;
}
static bool pch_reset(void) {
    /* Request firmware's ULP exit before trying to take its PHY semaphore. */
    wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)|0x10000000u);
    if (!pch_wake_me())return false;
    wr(REG_IMC,0xffffffff);wr(REG_RCTL,0);wr(REG_TCTL,1u<<3);
    /* PCH receive FIFO allocation must be established before MAC reset. */
    wr(0x1000,26);
    wr(REG_CTRL,rd(REG_CTRL)|4); /* stop new PCIe master requests */
    if (!wait_bits(REG_STATUS,0x80000,0,100)) {
        klogf("NET: PCIe master did not drain\n");return false;
    }
    sleep_ms(10);
    if (!pch_own()) return false;
    /* MAC-only reset retains firmware PHY calibration/errata configuration. */
    klogf("NET: resetting PCH MAC, retaining PHY calibration\n");
    wr(REG_CTRL,(rd(REG_CTRL)&~0x80000000u)|0x04000000u);
    sleep_ms(20); /* Do not flush/read CTRL immediately after PCH reset. */
    if (!wait_bits(REG_CTRL,0x04000000,0,100)) {
        klogf("NET: MAC reset timed out, CTRL=%x\n",rd(REG_CTRL));
        pch_release();return false;
    }
    /* Global MAC reset releases SWFLAG itself. Do not clear a flag that
     * firmware could have acquired after reset; PHY startup reacquires it. */
    wr(REG_IMC,0xffffffff);(void)rd(0xc0);
    wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)|0x10420000u);
    wr(0x3004,rd(0x3004)|0x50000u);
    wr(0x5008,rd(0x5008)|0x80c0u); /* extended receive status, no NFS filtering */
    wr(0x100c,rd(0x100c)|0x10000u); /* packet-buffer ECC */
    wr(REG_CTRL,(rd(REG_CTRL)&~0xd8001804u)|0x80040u);
    wr(0x3840,(rd(0x3840)&~0x30000000u)|0x2d800000u); /* SPT/KBL: two outstanding TX requests */
    /* Polling/legacy-interrupt path: keep the I219 sideband clock available. */
    wr(0xe4,rd(0xe4)|4u);
    wr(0x5bb4,rd(0x5bb4)|0x1800u);
    return pch_phy_start();
}
