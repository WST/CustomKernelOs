"""Publish immutable MyOS live images and serve only netboot artifacts over HTTP."""
from env_config import settings,project_path
import argparse,hashlib,http.server,json,os,pathlib,re,socket,tempfile,time,urllib.parse

ROOT=pathlib.Path(__file__).resolve().parents[1]
PUBLIC=ROOT/'build/netboot'
IMAGE_NAME=re.compile(r'[0-9a-f]{64}\.img')

def publish(image,manifest,directory=PUBLIC):
    metadata=json.loads(manifest.read_text())
    directory.mkdir(parents=True,exist_ok=True)
    images=directory/'images';images.mkdir(exist_ok=True)
    h=hashlib.sha256();size=0
    fd,name=tempfile.mkstemp(prefix='.publish-',dir=images)
    temporary=pathlib.Path(name)
    try:
        with os.fdopen(fd,'wb') as out,image.open('rb') as source:
            for chunk in iter(lambda:source.read(1048576),b''):
                out.write(chunk);h.update(chunk);size+=len(chunk)
            out.flush();os.fsync(out.fileno())
        digest=h.hexdigest()
        if digest!=metadata['sha256'] or size!=metadata['bytes']:
            raise ValueError('Live image does not match its manifest; rebuild it first')
        target=images/(digest+'.img')
        if target.exists():temporary.unlink()
        else:os.replace(temporary,target)
        metadata=dict(metadata,image='images/'+target.name)
        fd,name=tempfile.mkstemp(prefix='.latest-',dir=directory)
        pointer=pathlib.Path(name)
        try:
            with os.fdopen(fd,'w',encoding='utf-8') as out:
                json.dump(metadata,out,indent=2);out.write('\n');out.flush();os.fsync(out.fileno())
            # Windows readers can briefly hold the old pointer without delete sharing.
            for attempt in range(20):
                try:os.replace(pointer,directory/'latest.json');break
                except PermissionError:
                    if attempt==19:raise
                    time.sleep(.05)
        finally:pointer.unlink(missing_ok=True)
        print(f'Published {size//1048576} MiB image: {target}')
        return metadata
    finally:temporary.unlink(missing_ok=True)

def boot_script(metadata):
    name=pathlib.PurePosixPath(metadata['image']).name
    if not IMAGE_NAME.fullmatch(name):raise ValueError('Invalid published image name')
    return (f'#!ipxe\necho MyOS BIOS live: root {metadata["root_mb"]} MiB, image {name[:12]}\n'
            f'sanboot --no-describe images/{name} || goto failed\n'
            ':failed\necho MyOS network boot failed; check server, Ethernet and available RAM.\n'
            'prompt Press any key for the iPXE shell\nshell\n').encode()

class Server(http.server.ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,address,directory=PUBLIC):
        self.directory=pathlib.Path(directory).resolve()
        self.range_reads=0;self.last_range=None
        super().__init__(address,Handler)

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def setup(self):
        super().setup();self.connection.settimeout(30)
        self.connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
    def log_message(self,fmt,*args):
        if fmt.startswith('Request timed out:'):return
        # BIOS sector reads can generate thousands of Range requests.
        if self.path=='/boot.ipxe' or (args and str(args[1] if len(args)>1 else '').startswith(('4','5'))):
            super().log_message(fmt,*args)
    def do_HEAD(self):self.handle_file(head=True)
    def do_GET(self):self.handle_file(head=False)
    def handle_file(self,head=False):
        try:path=urllib.parse.urlsplit(self.path).path
        except ValueError:self.send_error(400);return
        directory=self.server.directory
        try:
            if path in ('/boot.ipxe','/latest.json'):
                metadata=json.loads((directory/'latest.json').read_text())
                data=boot_script(metadata) if path=='/boot.ipxe' else (json.dumps(metadata,indent=2)+'\n').encode()
                self.send_response(200);self.send_header('Content-Type','text/plain' if path=='/boot.ipxe' else 'application/json')
                self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers()
                if not head:self.wfile.write(data)
                return
            if not path.startswith('/images/') or not IMAGE_NAME.fullmatch(path[8:]):
                self.send_error(404);return
            target=directory/'images'/path[8:]
            # No directory listing, arbitrary workspace paths, or symlink targets.
            if target.is_symlink():self.send_error(404);return
            with target.open('rb') as source:
                size=os.fstat(source.fileno()).st_size;start=0;end=size-1
                requested=self.headers.get('Range')
                etag='"'+target.stem+'"'
                if self.headers.get('If-Range') not in (None,etag):requested=None
                if requested:
                    match=re.fullmatch(r'bytes=(\d*)-(\d*)',requested)
                    if not match or not any(match.groups()):self.range_error(size);return
                    first,last=match.groups()
                    if first:
                        start=int(first);end=min(int(last),end) if last else end
                    else:
                        length=int(last)
                        if not length:self.range_error(size);return
                        start=max(0,size-length)
                    if start>=size or end<start:self.range_error(size);return
                if requested:
                    self.server.range_reads+=1;self.server.last_range=(start,end)
                length=end-start+1
                self.send_response(206 if requested else 200)
                self.send_header('Content-Type','application/octet-stream');self.send_header('Accept-Ranges','bytes')
                self.send_header('Content-Length',str(length));self.send_header('Cache-Control','public, max-age=31536000, immutable')
                self.send_header('ETag',etag)
                if requested:self.send_header('Content-Range',f'bytes {start}-{end}/{size}')
                self.end_headers()
                if head:return
                source.seek(start)
                while length:
                    chunk=source.read(min(length,1048576))
                    if not chunk:break
                    self.wfile.write(chunk);length-=len(chunk)
        except (FileNotFoundError,NotADirectoryError):self.send_error(404)
        except (BrokenPipeError,ConnectionResetError):pass
        except (ValueError,KeyError,json.JSONDecodeError):self.send_error(503,'Invalid netboot publication')
    def range_error(self,size):
        self.send_response(416);self.send_header('Content-Range',f'bytes */{size}');self.send_header('Content-Length','0');self.end_headers()

def main():
    config=settings()
    ap=argparse.ArgumentParser(description=__doc__);commands=ap.add_subparsers(dest='command',required=True)
    pub=commands.add_parser('publish');pub.add_argument('--image',type=pathlib.Path,default=project_path(config['NETBOOT_IMAGE']))
    pub.add_argument('--manifest',type=pathlib.Path,default=project_path(config['NETBOOT_MANIFEST']));pub.add_argument('--directory',type=pathlib.Path,default=project_path(config['NETBOOT_DIRECTORY']))
    serve=commands.add_parser('serve',formatter_class=argparse.ArgumentDefaultsHelpFormatter);serve.add_argument('--bind',default=config['NETBOOT_BIND'],help='Listen address (NETBOOT_BIND)');serve.add_argument('--port',type=int,default=config['NETBOOT_PORT'],help='Listen port (NETBOOT_PORT)')
    serve.add_argument('--directory',type=pathlib.Path,default=project_path(config['NETBOOT_DIRECTORY']))
    args=ap.parse_args()
    if args.command=='publish':publish(args.image,args.manifest,args.directory);return
    if not (args.directory/'latest.json').is_file():ap.error('No image published; run make netboot-image first')
    with Server((args.bind,args.port),args.directory) as server:
        print(f'MyOS netboot HTTP server on {args.bind}:{server.server_port}',flush=True)
        print(f'iPXE: dhcp && chain http://<IP-of-this-PC>:{server.server_port}/boot.ipxe',flush=True)
        print('Only published live images are served. Ctrl+C stops the server.',flush=True)
        try:server.serve_forever()
        except KeyboardInterrupt:pass

if __name__=='__main__':main()
