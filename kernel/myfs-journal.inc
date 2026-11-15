/* Physical redo journal. The VFS lock serializes the single staging buffer.
 * No home sector is written until payload and commit record are durable.
 * Recovery validates the entire record before replay and is idempotent.
 * A device must honour FLUSH and atomic 512-byte sector writes. */
#define JOURNAL_MAGIC 0x314c4e4au
#define JOURNAL_MAX 120u
#define JOURNAL_BLOCKS (JOURNAL_MAX+1)
struct journal_header { u32 magic,count,checksum,reserved; u32 target[JOURNAL_MAX]; u8 pad[16]; };
_Static_assert(sizeof(struct journal_header)==BS,"journal header sector");
struct journal_entry { u32 target; u8 bytes[BS]; };
static struct journal_entry *transaction;
static unsigned transaction_depth,transaction_count;
static int transaction_error;
static bool journal_enabled[5],journal_failed[5];
static u32 crc_bytes(u32 crc,const void *data,u32 size) {
    const u8 *p=data;
    while(size--) { crc^=*p++; for(int i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
    return crc;
}
static int disk_read(u32 b,void *p) { return block_read(states[selected].device,b,p)?FS_EIO:0; }
/* Секторы журнала остаются грязными до disk_barrier: сброс пишет их пачкой.
 * Барьер по-прежнему обязателен перед commit и перед повторным использованием журнала. */
static int disk_write(u32 b,const void *p) { return block_write(states[selected].device,b,p)?FS_EIO:0; }
static int disk_barrier(void) { return block_flush_device(states[selected].device)?FS_EIO:0; }
static int journal_clear(void) {
    struct journal_header h; memset(&h,0,sizeof(h));
    return disk_write(sb.journal_start,&h) || disk_barrier() ? FS_EIO : 0;
}
static int journal_recover(void) {
    struct journal_header h; u8 data[BS];
    if(disk_read(sb.journal_start,&h))return FS_EIO;
    if(!h.magic) {
        const u8 *p=(const u8 *)&h;
        for(unsigned i=0;i<BS;i++)if(p[i])return FS_EIO;
        return 0;
    }
    if(h.magic!=JOURNAL_MAGIC || !h.count || h.count>JOURNAL_MAX)return FS_EIO;
    u32 expected=h.checksum;h.checksum=0;
    u32 crc=crc_bytes(~0u,&h,BS);
    for(unsigned i=0;i<h.count;i++) {
        if(h.target[i]>=sb.journal_start)return FS_EIO;
        for(unsigned j=0;j<i;j++)if(h.target[i]==h.target[j])return FS_EIO;
        if(disk_read(sb.journal_start+1+i,data))return FS_EIO;
        crc=crc_bytes(crc,data,BS);
    }
    if(~crc!=expected)return FS_EIO;
    for(unsigned i=0;i<h.count;i++) {
        if(disk_read(sb.journal_start+1+i,data) || disk_write(h.target[i],data))return FS_EIO;
    }
    if(disk_barrier())return FS_EIO;
    return journal_clear();
}
static int journal_read(u32 b,void *p) {
    if(journal_failed[selected])return FS_EIO;
    /* Между вызовами depth уже 0, а группа ещё не сброшена: чтение обязано видеть её. */
    if(transaction_count) for(unsigned i=0;i<transaction_count;i++)if(transaction[i].target==b) {
        memcpy(p,transaction[i].bytes,BS);return 0;
    }
    int r=disk_read(b,p);if(r && transaction_depth)transaction_error=FS_EIO;return r;
}
/* Повторное использование сектора не должно оставлять в группе старую копию:
 * чтение смотрит сначала туда и иначе не увидит новые байты в кэше. */
static void journal_absorb_data(u32 b,const void *p) {
    if(!transaction)return;
    for(unsigned i=0;i<transaction_count;i++) if(transaction[i].target==b) {
        memcpy(transaction[i].bytes,p,BS);return;
    }
}
static int journal_write(u32 b,const void *p,bool data) {
    if(journal_failed[selected])return FS_EIO;
    if(transaction_depth) {
        unsigned i;
        for(i=0;i<transaction_count;i++)if(transaction[i].target==b)break;
        if(i==JOURNAL_MAX) { transaction_error=FS_EFBIG;return FS_EFBIG; }
        if(i==transaction_count)transaction_count++;
        transaction[i].target=b;memcpy(transaction[i].bytes,p,BS);return 0;
    }
    return data ? (block_write(states[selected].device,b,p)?FS_EIO:0) : disk_write(b,p);
}
/* Группа вызовов копится в журнале, пока не заполнится метаданными или 2 МиБ
 * новых данных. Откат одного вызова не должен стирать уже принятые предыдущие. */
#define COMMIT_DATA (2u<<20)
#define COMMIT_ROOM 48u
static struct journal_entry *undo_log;
static unsigned undo_count;
static bool undo_armed, undo_lost;
static u32 pending_data, saved_blocks, saved_inodes, saved_hint, saved_ino_hint;
static u32 bitmap_undo_sec[32];
static unsigned bitmap_undo_n;
static u8 *bitmap_undo_data;
static int commit_pending(void);
static void bitmap_undo_touch(u32 block) {
    if(!undo_armed || undo_lost || !bitmap) return;
    u32 sec=block/BITS_PER_BLK;
    if(sec>=sb.bitmap_blocks) return;
    for(unsigned i=0;i<bitmap_undo_n;i++) if(bitmap_undo_sec[i]==sec) return;
    if(bitmap_undo_n>=32){undo_lost=true;return;}
    memcpy(bitmap_undo_data+bitmap_undo_n*BS,bitmap+sec*BS,BS);
    bitmap_undo_sec[bitmap_undo_n++]=sec;
}
static int transaction_begin(void) {
    if(journal_failed[selected])return FS_EIO;
    if(!journal_enabled[selected])return 0;
    if(!transaction)transaction=bitmap_alloc(JOURNAL_MAX*sizeof(*transaction));
    if(!transaction)return FS_ENOSPC;
    if(!undo_log)undo_log=bitmap_alloc(JOURNAL_MAX*sizeof(*transaction));
    if(!undo_log)return FS_ENOSPC;
    if(!bitmap_undo_data)bitmap_undo_data=bitmap_alloc(32*BS);
    if(!bitmap_undo_data)return FS_ENOSPC;
    if(!transaction_depth) {
        /* Место в журнале оставляем текущему вызову: перезапись 16 КиБ и её inode. */
        if(transaction_count+COMMIT_ROOM>=JOURNAL_MAX || pending_data>=COMMIT_DATA) {
            int c=commit_pending();
            if(c)return c;
        }
        undo_count=transaction_count;
        if(undo_count)memcpy(undo_log,transaction,undo_count*sizeof(*transaction));
        bitmap_undo_n=0;undo_lost=false;undo_armed=true;transaction_error=0;
        saved_blocks=available_blocks;saved_inodes=available_inodes;
        saved_hint=states[selected].hint;saved_ino_hint=states[selected].ino_hint;
        block_epoch_begin();
    }
    transaction_depth++;return 0;
}
static int transaction_commit(void) {
    struct journal_header h;memset(&h,0,sizeof(h));
    if(!transaction_count)return 0;
    h.magic=JOURNAL_MAGIC;h.count=transaction_count;
    for(unsigned i=0;i<h.count;i++)h.target[i]=transaction[i].target;
    u32 crc=crc_bytes(~0u,&h,BS);
    for(unsigned i=0;i<h.count;i++) {
        crc=crc_bytes(crc,transaction[i].bytes,BS);
        if(disk_write(sb.journal_start+1+i,transaction[i].bytes))return FS_EIO;
    }
    if(disk_barrier())return FS_EIO;
    h.checksum=~crc;
    if(disk_write(sb.journal_start,&h) || disk_barrier())return FS_EIO;
    for(unsigned i=0;i<h.count;i++)if(disk_write(h.target[i],transaction[i].bytes))return FS_EIO;
    if(disk_barrier())return FS_EIO;
    return journal_clear();
}
static int transaction_end(int result);
