"""Public key validation, non-destructive merging and MyFS ownership/modes."""
import base64,pathlib,struct,subprocess,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import configure_ssh_keys as keys

def key(byte,comment='test'):
    blob=struct.pack('>I',11)+b'ssh-ed25519'+struct.pack('>I',32)+bytes([byte])*32
    return 'ssh-ed25519 '+base64.b64encode(blob).decode()+' '+comment
class KeyConfigTest(unittest.TestCase):
    def test_validate(self):
        self.assertEqual(keys.public_key(key(1))[0],key(1))
        for value in ('-----BEGIN OPENSSH PRIVATE KEY-----','ssh-rsa AAAA','ssh-ed25519 not-base64',
                      'ssh-ed25519 '+base64.b64encode(b'wrong wire format').decode(),key(1)+'\x00',key(1)+'\nextra'):
            with self.assertRaises(ValueError):keys.public_key(value)
        self.assertIsNone(keys.existing_identity('# '+key(1)))
        restricted='command="echo hello",no-pty '+key(1,'unbalanced " comment')
        self.assertEqual(keys.existing_identity(restricted),keys.public_key(key(1))[1])
        self.assertEqual(keys.public_key(key(1).replace(' ', '\t', 1))[1],keys.public_key(key(1))[1])
    def test_windows_path(self):
        if sys.platform.startswith('linux'):
            self.assertEqual(str(keys.source_path('C:/Users/test/id_ed25519.pub')),'/mnt/c/Users/test/id_ed25519.pub')
            self.assertEqual(str(keys.source_path(r'C:\Users\test\id_ed25519.pub')),'/mnt/c/Users/test/id_ed25519.pub')
    def test_file_selection(self):
        with tempfile.TemporaryDirectory() as folder:
            pub=pathlib.Path(folder)/'id.pub';pub.write_text('# comment\n'+key(1)+'\n'+key(2)+'\n')
            selected=keys.selected({'SSH_AUTHORIZED_KEY':key(1,'another comment'),'SSH_AUTHORIZED_KEY_FILE':str(pub)})
            self.assertEqual(len(selected),2)
            pub.write_text('-----BEGIN OPENSSH PRIVATE KEY-----\n')
            with self.assertRaises(ValueError):keys.selected({'SSH_AUTHORIZED_KEY':'','SSH_AUTHORIZED_KEY_FILE':str(pub)})
    def test_image_merge(self):
        with tempfile.TemporaryDirectory(prefix='myos-ssh-key-test-') as folder:
            disk=pathlib.Path(folder)/'disk.img'
            with disk.open('wb') as f:f.truncate(8*1048576)
            def fs(*args):return subprocess.check_output(['build/myfs',str(disk),*args],text=True)
            self.assertEqual(keys.install(disk,[]),0)
            self.assertEqual(keys.install(disk,[keys.public_key(key(1))]),1)
            self.assertEqual(fs('cat',keys.TARGET),key(1)+'\n')
            # Preserve restricted entries, unknown key types and a missing trailing newline.
            original='# existing\ncommand="echo hello",no-pty '+key(1)+'\nssh-rsa AAAA old-comment'
            source=pathlib.Path(folder)/'old';source.write_text(original);fs('put',str(source),keys.TARGET)
            self.assertEqual(keys.install(disk,[keys.public_key(key(1,'changed')),keys.public_key(key(2))]),1)
            expected=original+'\n'+key(2)+'\n';self.assertEqual(fs('cat',keys.TARGET),expected)
            self.assertEqual(keys.install(disk,[keys.public_key(key(2,'changed'))]),0)
            self.assertEqual(fs('cat',keys.TARGET),expected)
            for path,mode in (('/root','0700'),('/root/.ssh','0700'),(keys.TARGET,'0600')):
                self.assertIn('mode='+mode+' uid=0 gid=0',fs('stat',path))
    def test_invalid_target(self):
        with tempfile.TemporaryDirectory(prefix='myos-ssh-key-type-') as folder:
            disk=pathlib.Path(folder)/'disk.img'
            with disk.open('wb') as f:f.truncate(8*1048576)
            subprocess.run(['build/myfs',str(disk),'mkdir','/root/.ssh/authorized_keys'],check=True,stdout=subprocess.DEVNULL)
            with self.assertRaises(ValueError):keys.install(disk,[keys.public_key(key(1))])
if __name__=='__main__':unittest.main()
