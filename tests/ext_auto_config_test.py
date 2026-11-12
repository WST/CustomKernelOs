"""Validate literal build selection and preservation of persistent configuration."""
import os,pathlib,subprocess,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import configure_ext_auto as auto
class AutoConfigTest(unittest.TestCase):
    def test_names(self):
        self.assertEqual(auto.packages(' openssh lua openssh '),['openssh','lua'])
        self.assertEqual(auto.packages(''),[])
        for value in ('../openssh','$(touch dangerous)','openssh;echo','.', 'not-a-real-package', 'a'*32):
            with self.assertRaises(ValueError):auto.packages(value)
    def test_existing_disk(self):
        with tempfile.TemporaryDirectory(prefix='myos-ext-seed-') as folder:
            disk=pathlib.Path(folder)/'disk.img'
            with disk.open('wb') as f:f.truncate(8*1048576)
            def fs(*args):return subprocess.check_output(['build/myfs',str(disk),*args],text=True)
            fs('mkdir','/etc/services/enabled')
            auto.seed(disk,['openssh','lua'])
            self.assertIn('openssh\nlua\n',fs('cat','/etc/ext/autoinstall.conf'))
            fs('stat','/etc/services/enabled/ext-autoinstall.service')
            original=fs('cat','/etc/ext/autoinstall.conf')
            auto.seed(disk,[])
            self.assertEqual(fs('cat','/etc/ext/autoinstall.conf'),original)
    def test_empty_disabled(self):
        with tempfile.TemporaryDirectory(prefix='myos-ext-empty-') as folder:
            disk=pathlib.Path(folder)/'disk.img'
            with disk.open('wb') as f:f.truncate(8*1048576)
            subprocess.run(['build/myfs',str(disk),'mkdir','/etc/services/enabled'],check=True,stdout=subprocess.DEVNULL)
            auto.seed(disk,[])
            result=subprocess.run(['build/myfs',str(disk),'stat','/etc/services/enabled/ext-autoinstall.service'],capture_output=True)
            self.assertNotEqual(result.returncode,0)
if __name__=='__main__':unittest.main()
