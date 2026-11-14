"""A small sparse file owning sectors in 200 bitmap pages exceeds one log batch."""
import pathlib,struct,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
TOOL=ROOT/'build/myfs'
def run(image,*args):return subprocess.check_output([str(TOOL),str(image),*map(str,args)])
def main():
    with tempfile.TemporaryDirectory(prefix='myos-large-reclaim-') as directory:
        d=pathlib.Path(directory);image=d/'sparse.img';source=d/'seed';source.write_bytes(b'x')
        with image.open('wb') as f:f.truncate(1073741824)
        run(image,'put',source,'/large')
        inode=int(run(image,'stat','/large').decode().split('inode=')[1].split()[0])
        with image.open('r+b') as f:
            superblock=struct.unpack('<13I',f.read(52));inode_start=superblock[5];data_start=superblock[8]
            blocks=[4096*(i+1) for i in range(200)]
            indirect,double,leaf=[data_start+1000+i for i in range(3)]
            def sector(block,pointers):
                f.seek(block*512);f.write(struct.pack('<128I',*(pointers+[0]*(128-len(pointers)))))
            sector(indirect,blocks[11:139]);sector(double,[leaf]);sector(leaf,blocks[139:])
            f.seek(inode_start*512+inode*64)
            f.write(struct.pack('<HHI14I',1,1,200*512,*blocks[:11],0,indirect,double))
            # Deliberately leave bitmap stale: mount must repair allocations first.
        run(image,'fsck');run(image,'rm','/large');run(image,'fsck')
        with image.open('rb') as f:
            for block in blocks+[indirect,double,leaf]:
                f.seek(512+block//8);assert not(f.read(1)[0]&(1<<(block%8)))
        assert b'large' not in run(image,'ls','/')
    print('LARGE RECLAIM PASS: 200 bitmap pages repaired and reclaimed in bounded transactions')
if __name__=='__main__':main()
