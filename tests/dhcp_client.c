/* Exercise the real DHCP serializer/state machine with a delayed strict server. */
#define HOST_TEST
#include "../kernel/net.c"
extern int printf(const char *,...);
extern void exit(int);
extern void *malloc(size_t);
extern void free(void *);
#define CHECK(x) do { if(!(x)) { printf("DHCP test failed at %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static u32 now,offer_due,ack_due;
static unsigned discovers,requests,diagnostics;
static bool offer_sent,ack_sent,no_server,nak_once,nak_sent,fail_tx;
static unsigned ack_delay;
static int types[16];static unsigned sent_count;
void klogf(const char *fmt,...) { (void)fmt; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
void task_wakeup(void *p) { (void)p; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
u32 timer_ticks(void) { return now/10; }
void sleep_ms(u32 ms) { now+=ms; }
bool e1000_link(void) { return true; }
void e1000_diagnose(void) { diagnostics++; }
void udp_deliver(u32 src,u16 sport,u16 dport,const u8 *data,u32 len) { (void)src;(void)sport;(void)dport;(void)data;(void)len; }
void tcp_input(u32 src,u32 dst,const u8 *data,u32 len) { (void)src;(void)dst;(void)data;(void)len; }

static int option_type(const struct dhcp_msg *m,u32 len) {
    const u8 *o=m->options,*end=(const u8 *)m+len;
    while(o<end && *o!=255) {
        if(!*o) { o++;continue; }
        CHECK(o+2<=end && o+2+o[1]<=end);
        if(*o==53) { CHECK(o[1]==1);return o[2]; }
        o+=2+o[1];
    }
    return 0;
}
int e1000_send(const void *data,u32 len) {
    if(fail_tx)return -1;
    const struct eth_hdr *eth=data;
    if(ntohs(eth->type)!=ETH_IP)return 0;
    const struct ip_hdr *ip=(const void *)(eth+1);
    const struct udp_hdr *udp=(const void *)(ip+1);
    CHECK(len>=sizeof(*eth)+ntohs(ip->len) && net_checksum(ip,20,0)==0);
    CHECK(ntohs(udp->sport)==68 && ntohs(udp->dport)==67);
    u32 bytes=ntohs(udp->len)-8;
    CHECK(bytes>=300 && bytes<=sizeof(struct dhcp_msg));
    const struct dhcp_msg *m=(const void *)(udp+1);
    CHECK(m->op==1 && m->hlen==6 && m->htype==1 && !memcmp(m->chaddr,netcfg.mac,6));
    CHECK(ntohl(m->xid)==dh.xid && ntohs(m->flags)==0x8000);
    CHECK(ntohs(m->secs)==(now-dh.started*10)/1000);
    int type=option_type(m,bytes);CHECK(sent_count<16);types[sent_count++]=type;
    if(type==1) { discovers++;if(!offer_due)offer_due=now+3000; }
    else { CHECK(type==3);requests++;if(!ack_due)ack_due=now+ack_delay; }
    const u8 *o=m->options,*end=(const u8 *)m+bytes;bool id=false;
    while(o<end && *o!=255) {
        if(!*o) { o++;continue; }
        CHECK(o+2+o[1]<=end);
        if(*o==61) { CHECK(o[1]==7 && o[2]==1 && !memcmp(o+3,netcfg.mac,6));id=true; }
        o+=2+o[1];
    }
    CHECK(id && o<end);
    for(o++;o<end;o++)CHECK(!*o);
    return 0;
}
static void make_reply(struct dhcp_msg *m,int type) {
    memset(m,0,sizeof(*m));m->op=2;m->htype=1;m->hlen=6;
    m->xid=htonl(dh.xid);m->yiaddr=htonl(IP(192,168,7,20));m->magic=htonl(0x63825363);
    memcpy(m->chaddr,netcfg.mac,6);
    u8 options[]={53,1,type,54,4,192,168,7,1,1,4,255,255,255,0,3,4,192,168,7,1,255};
    memcpy(m->options,options,sizeof(options));
}
int e1000_receive(void *buffer,u32 max) {
    if(no_server)return 0;
    int type=0;
    if(offer_due && now>=offer_due && !offer_sent) { type=2;offer_sent=true; }
    else if(ack_due && now>=ack_due && !ack_sent) {
        type=nak_once && !nak_sent?6:5;ack_sent=true;
        if(type==6) { nak_sent=true;offer_sent=false;offer_due=ack_due=0;ack_sent=false; }
    }
    if(!type)return 0;
    CHECK(max>=342);memset(buffer,0,342);
    struct eth_hdr *eth=buffer;memcpy(eth->dst,netcfg.mac,6);eth->type=htons(ETH_IP);
    struct ip_hdr *ip=(void *)(eth+1);ip->ver_ihl=0x45;ip->len=htons(328);ip->proto=PROTO_UDP;
    ip->src=htonl(IP(192,168,7,1));ip->dst=0xffffffff;ip->csum=net_checksum(ip,20,0);
    struct udp_hdr *udp=(void *)(ip+1);udp->sport=htons(67);udp->dport=htons(68);udp->len=htons(308);
    struct dhcp_msg m;make_reply(&m,type);memcpy(udp+1,&m,300);
    return 342;
}
static void setup(unsigned delay) {
    memset(&dh,0,sizeof(dh));memset(&netcfg,0,sizeof(netcfg));
    now=offer_due=ack_due=discovers=requests=diagnostics=sent_count=0;
    offer_sent=ack_sent=no_server=nak_once=nak_sent=fail_tx=false;ack_delay=delay;
    netcfg.up=true;netcfg.mac[0]=2;netcfg.mac[5]=17;
}
int main(void) {
    setup(3000);CHECK(dhcp_run());CHECK(netcfg.ip==IP(192,168,7,20) && now>=6000);
    CHECK(discovers==1 && requests==1 && netcfg.dhcp);
    setup(6000);CHECK(dhcp_run());CHECK(discovers==1 && requests==2 && now>=9000);
    CHECK(types[0]==1 && types[1]==3 && types[2]==3);
    setup(100);nak_once=true;CHECK(dhcp_run());CHECK(nak_sent && discovers==2 && requests==2);
    setup(0);no_server=true;CHECK(!dhcp_run());CHECK(!netcfg.ip && discovers==3 && diagnostics==3 && now>=28000);
    setup(0);fail_tx=true;dh.state=1;dh.started=0;
    CHECK(udp_send(0xffffffff,68,67,"x",1)==-EAGAIN);
    setup(0);struct dhcp_msg m;dh.state=1;dh.xid=7;make_reply(&m,2);
    m.chaddr[0]^=1;dhcp_input((const void *)&m,300);CHECK(dh.state==1 && dh.rejected==1);
    make_reply(&m,2);m.options[1]=0;m.options[2]=255;
    dhcp_input((const void *)&m,243);CHECK(dh.state==1 && dh.rejected==2);
    dhcp_input((const void *)&m,230);CHECK(dh.rejected==3);
    printf("DHCP: strict 300-byte server, delayed OFFER/ACK, REQUEST retry, NAK, no-server timeout, malformed replies and TX backpressure: PASS\n");
    return 0;
}
