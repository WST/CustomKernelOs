"""Shared dotenv parser and actual GNU Make precedence/export behavior."""
import json,os,pathlib,shutil,subprocess,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import env_config

class EnvConfigTest(unittest.TestCase):
    def test_dotenv(self):
        with tempfile.TemporaryDirectory() as temp:
            path=pathlib.Path(temp)/'.env'
            path.write_text('\ufeff# comment\r\nexport USB_ROOT_MB = "512" # root\r\nNETBOOT_URL=\'http://host:9000/a#b\' # comment with "quote"\nNETBOOT_DIRECTORY="C:\\root\\files"\nSSH_PORT=\n',encoding='utf-8')
            values=env_config.settings(path,{'USB_ROOT_MB':'128'})
            self.assertEqual(values['USB_ROOT_MB'],'128')
            self.assertEqual(values['NETBOOT_URL'],'http://host:9000/a#b')
            self.assertEqual(values['NETBOOT_DIRECTORY'],r'C:\root\files')
            self.assertEqual(values['SSH_PORT'],'')
            self.assertEqual(env_config.settings(path,{})['USB_ROOT_MB'],'512')
            for text in ('USB_ROOT_MB','UNKNOWN_KEY=1','USB_ROOT_MB="broken','USB_ROOT_MB="512" junk'):
                path.write_text(text)
                with self.assertRaises(ValueError):env_config.settings(path,{})
    def test_missing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            values=env_config.settings(pathlib.Path(temp)/'missing',{})
            self.assertEqual(values['USB_ROOT_MB'],'256')
            self.assertEqual(values['NETBOOT_PORT'],'8080')
    @unittest.skipUnless(shutil.which('make'),'GNU Make integration runs in Linux/WSL')
    def test_make_precedence_and_literals(self):
        with tempfile.TemporaryDirectory(prefix='myos-env-') as temp:
            root=pathlib.Path(temp);dotenv=root/'.env';makefile=root/'test.mk'
            marker=root/'must-not-exist'
            dotenv.write_text('NETBOOT_PORT=9001\nDATA_DISK="  data.img  "\nUSB_ROOT_MB=128\nNETBOOT_URL=$(shell touch '+str(marker)+')\nNETBOOT_DIRECTORY="C:\\folder\\#name"\n')
            makefile.write_text(env_config.make_text(dotenv,{})+"\nprobe:\n\t@python3 -c 'import os,json; print(json.dumps({k:os.environ[k] for k in (\"NETBOOT_PORT\",\"USB_ROOT_MB\",\"NETBOOT_URL\",\"NETBOOT_DIRECTORY\",\"CC\",\"DATA_DISK\")}))'\n")
            base=dict(os.environ)
            for key in (*env_config.DEFAULTS,*env_config.COMPILERS,'MAKEFLAGS','MFLAGS','MAKEOVERRIDES'):base.pop(key,None)
            def probe(extra=(),environ=None):
                result=subprocess.run(['make','--no-print-directory','-s','-f',str(makefile),'probe',*extra],env=base if environ is None else environ,capture_output=True,text=True,check=True)
                return json.loads(result.stdout)
            values=probe();self.assertEqual(values['NETBOOT_PORT'],'9001');self.assertEqual(values['DATA_DISK'],'  data.img  ')
            self.assertEqual(values['NETBOOT_DIRECTORY'],r'C:\folder\#name')
            self.assertEqual(values['NETBOOT_URL'],'$(shell touch '+str(marker)+')');self.assertFalse(marker.exists())
            values=probe(environ=dict(base,NETBOOT_PORT='9002'));self.assertEqual(values['NETBOOT_PORT'],'9002')
            values=probe(['NETBOOT_PORT=9003','CROSS=myos-'],dict(base,NETBOOT_PORT='9002'))
            self.assertEqual(values['NETBOOT_PORT'],'9003');self.assertEqual(values['CC'],'myos-gcc')
            self.assertEqual(values['USB_ROOT_MB'],'128')
            self.assertFalse(marker.exists())

if __name__=='__main__':unittest.main()
