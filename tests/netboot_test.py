"""HTTP sector reads and atomic publications for the network boot server."""
import argparse,hashlib,http.client,json,pathlib,sys,tempfile,threading,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import netboot
from build_ipxe import script_url

class NetbootTest(unittest.TestCase):
    def test_publication_and_http(self):
        with tempfile.TemporaryDirectory(prefix='myos-netboot-http-') as directory:
            root=pathlib.Path(directory);public=root/'public';image=root/'live.img';manifest=root/'manifest.json'
            data=bytes(range(256))*4096
            def publish(payload):
                image.write_bytes(payload)
                manifest.write_text(json.dumps({'sha256':hashlib.sha256(payload).hexdigest(),'bytes':len(payload),'root_mb':64}))
                return netboot.publish(image,manifest,public)
            first=publish(data)
            with netboot.Server(('127.0.0.1',0),public) as server:
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                def request(path,method='GET',headers=None):
                    connection=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=5)
                    try:
                        connection.request(method,path,headers=headers or {})
                        result=connection.getresponse();return result.status,dict(result.getheaders()),result.read()
                    finally:connection.close()
                try:
                    url='/'+first['image']
                    status,headers,body=request('/boot.ipxe');self.assertEqual(status,200)
                    self.assertIn(first['image'].encode(),body);self.assertEqual(headers['Cache-Control'],'no-store')
                    for span,expected in [('0-0',data[:1]),('511-1024',data[511:1025]),('1048574-',data[-2:]),('-16',data[-16:])]:
                        status,headers,body=request(url,headers={'Range':'bytes='+span})
                        self.assertEqual(status,206);self.assertEqual(body,expected);self.assertIn('Content-Range',headers)
                    status,headers,body=request(url,'HEAD',{'Range':'bytes=0-511'})
                    self.assertEqual(status,206);self.assertEqual(headers['Content-Length'],'512');self.assertEqual(body,b'')
                    status,headers,body=request(url,'HEAD');self.assertEqual(status,200);self.assertEqual(headers['Content-Length'],str(len(data)))
                    for span in ('bytes=1048576-','bytes=10-2','bytes=-0','bytes=0-1,4-5','bytes=-','junk'):
                        self.assertEqual(request(url,headers={'Range':span})[0],416)
                    for path in ('/','/../README.md','/images/../../README.md','/images/%2e%2e/secret','/latest.json/secret','/bootstrap.ipxe'):
                        self.assertEqual(request(path)[0],404)
                    secret=root/'secret';secret.write_bytes(b'private')
                    link=public/'images'/('a'*64+'.img')
                    try:link.symlink_to(secret)
                    except OSError:pass  # Windows may require a developer-mode symlink privilege.
                    else:self.assertEqual(request('/images/'+link.name)[0],404)
                    self.assertEqual(request(url,headers={'Range':'bytes=0-1','If-Range':'"old"'})[0],200)
                    pointer=(public/'latest.json').read_bytes()
                    manifest.write_text(json.dumps({'sha256':'0'*64,'bytes':len(data),'root_mb':64}))
                    with self.assertRaises(ValueError):netboot.publish(image,manifest,public)
                    self.assertEqual((public/'latest.json').read_bytes(),pointer)
                    second=publish(b'new-version')
                    self.assertNotEqual(first['image'],second['image'])
                    self.assertIn(second['image'].encode(),request('/boot.ipxe')[2])
                    self.assertEqual(request(url)[2],data)  # an in-flight old boot remains pinned
                finally:server.shutdown();thread.join()
    def test_bootstrap_urls(self):
        self.assertEqual(script_url('http://192.168.1.10:8080'),'http://192.168.1.10:8080/boot.ipxe')
        self.assertEqual(script_url('https://host/boot.ipxe'),'https://host/boot.ipxe')
        for value in ('','file:///tmp/file','http://host/\nshell','http://user:password@host','http://host/${name}','http://host:0','http://host/?x=y','http://host/#x'):
            with self.assertRaises(argparse.ArgumentTypeError):script_url(value)

if __name__=='__main__':unittest.main()
