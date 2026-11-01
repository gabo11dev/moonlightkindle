#!/usr/bin/env python3
"""GameStream pairing fixture with real client-certificate TLS.
Requires cryptography and pyOpenSSL. Usage: pair_host.py [emulator ...] pair-test
The fixed test PIN 1234 replaces Sunshine's user-entered PIN approval.
"""
import hashlib,http.server,os,socket,subprocess,sys,tempfile,threading
from pathlib import Path
from urllib.parse import parse_qs,urlsplit
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
from OpenSSL import SSL

def root(body='',status=200,message=None):
    attr=f' status_message="{message}"' if message else ''
    return f'<root status_code="{status}"{attr}>{body}</root>'.encode()
def crypt(key,data,encrypt=True):
    context=Cipher(algorithms.AES(key),modes.ECB()).encryptor() if encrypt else Cipher(algorithms.AES(key),modes.ECB()).decryptor()
    return context.update(data)+context.finalize()
class State:
    def __init__(self,cert,key,mode):
        self.cert=x509.load_pem_x509_certificate(cert.read_bytes())
        self.key=serialization.load_pem_private_key(key.read_bytes(),None)
        self.pem=cert.read_bytes();self.sessions={};self.authorized=set();self.mode=mode
    def reply(self,path,tls=False,authorized=False):
        u=urlsplit(path);q={k:v[0] for k,v in parse_qs(u.query).items()}
        if tls and not authorized:
            return root(status=401,message='The client is not authorized. Certificate verification failed.')
        if u.path=='/serverinfo':
            return root(f'<currentgame>0</currentgame><PairStatus>{int(authorized)}</PairStatus><appversion>7.1.431.0</appversion><state>SUNSHINE_SERVER_FREE</state><ServerCodecModeSupport>1</ServerCodecModeSupport><gputype>Fixture</gputype><GsVersion>1</GsVersion><GfeVersion>3.26.0</GfeVersion><HttpsPort>{self.tls_port}</HttpsPort><DisplayMode><Height>360</Height><Width>640</Width><RefreshRate>10</RefreshRate></DisplayMode>')
        if u.path=='/unpair':return root(status=401,message='cleanup error must not hide original failure')
        if u.path=='/applist':return root('<App><ID>123</ID><AppTitle>Desktop</AppTitle></App>')
        assert u.path=='/pair'
        if q.get('phrase')=='pairchallenge':return root('<paired>1</paired>')
        uid=q['uniqueid']
        if q.get('phrase')=='getservercert':
            if self.mode:return root(status=503 if self.mode==1 else 409,message=None if self.mode==1 else 'A pairing session already exists')
            client=x509.load_pem_x509_certificate(bytes.fromhex(q['clientcert']))
            key=hashlib.sha256(bytes.fromhex(q['salt'])+b'1234').digest()[:16]
            self.sessions[uid]={'cert':client,'key':key,'secret':os.urandom(16),'challenge':os.urandom(16)}
            return root('<paired>1</paired><plaincert>'+self.pem.hex()+'</plaincert>')
        s=self.sessions[uid]
        if 'clientchallenge' in q:
            challenge=crypt(s['key'],bytes.fromhex(q['clientchallenge']),False)
            response=hashlib.sha256(challenge+self.cert.signature+s['secret']).digest()+s['challenge']
            return root('<paired>1</paired><challengeresponse>'+crypt(s['key'],response).hex()+'</challengeresponse>')
        if 'serverchallengeresp' in q:
            s['hash']=crypt(s['key'],bytes.fromhex(q['serverchallengeresp']),False)
            signature=self.key.sign(s['secret'],padding.PKCS1v15(),hashes.SHA256())
            return root('<paired>1</paired><pairingsecret>'+(s['secret']+signature).hex()+'</pairingsecret>')
        assert 'clientpairingsecret' in q
        data=bytes.fromhex(q['clientpairingsecret']);secret=data[:16]
        s['cert'].public_key().verify(data[16:],secret,padding.PKCS1v15(),hashes.SHA256())
        assert s['hash']==hashlib.sha256(s['challenge']+s['cert'].signature+secret).digest()
        self.authorized.add(s['cert'].fingerprint(hashes.SHA256()))
        return root('<paired>1</paired>')
class HTTP(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_GET(self):
        body=self.server.state.reply(self.path)
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
class TLS:
    def __init__(self,state,cert,key):
        self.state=state;self.context=SSL.Context(SSL.TLS_SERVER_METHOD)
        self.context.use_certificate_file(str(cert));self.context.use_privatekey_file(str(key))
        # Establish TLS first, then authorize the exact previously registered cert.
        self.context.set_verify(SSL.VERIFY_PEER|SSL.VERIFY_FAIL_IF_NO_PEER_CERT,lambda *args:True)
        self.socket=socket.socket();self.socket.bind(('127.0.0.1',0));self.socket.listen();self.socket.settimeout(.2)
        state.tls_port=self.socket.getsockname()[1];self.stop=False
        threading.Thread(target=self.accept,daemon=True).start()
    def accept(self):
        while not self.stop:
            try: conn,_=self.socket.accept()
            except socket.timeout:continue
            except OSError:return
            threading.Thread(target=self.handle,args=(conn,),daemon=True).start()
    def handle(self,sock):
        conn=SSL.Connection(self.context,sock)
        try:
            conn.set_accept_state();conn.do_handshake()
            peer=conn.get_peer_certificate().to_cryptography()
            authorized=peer.fingerprint(hashes.SHA256()) in self.state.authorized
            data=b''
            while True:
                while b'\r\n\r\n' not in data:
                    chunk=conn.recv(65536)
                    if not chunk:return
                    data+=chunk
                request,data=data.split(b'\r\n\r\n',1)
                path=request.split(b' ',2)[1].decode()
                body=self.state.reply(path,True,authorized)
                # Latch authorization at handshake time to catch stale TLS reuse.
                conn.sendall(f'HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\nConnection: keep-alive\r\n\r\n'.encode()+body)
        except (SSL.ZeroReturnError,SSL.SysCallError,ConnectionResetError):pass
        finally:conn.close()
    def close(self):self.stop=True;self.socket.close()
with tempfile.TemporaryDirectory(prefix='moonlight-pair-') as folder:
    tmp=Path(folder);cert=tmp/'server.pem';key=tmp/'server.key'
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-days','1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for mode in [0,1,2]:
        state=State(cert,key,mode);tls=TLS(state,cert,key)
        http_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),HTTP);http_server.state=state
        threading.Thread(target=http_server.serve_forever,daemon=True).start()
        keys=tmp/f'keys-{mode}';keys.mkdir(mode=0o700)
        try:
            result=subprocess.run(sys.argv[1:]+[str(http_server.server_port),str(keys),str(mode)],timeout=45)
            if result.returncode:sys.exit(result.returncode)
            if mode==0:assert len(state.authorized)==2
        finally:http_server.shutdown();http_server.server_close();tls.close()
