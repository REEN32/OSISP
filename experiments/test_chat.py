import argparse, csv, socket, struct, subprocess, time, pathlib
P=argparse.ArgumentParser();P.add_argument('--exe', default='/tmp/lab5');P.add_argument('--host',default='127.0.0.1');P.add_argument('--port',type=int,default=20027);P.add_argument('--timeout',type=float,default=10);P.add_argument('--repeats',type=int,default=3);P.add_argument('--counts',type=int,nargs='+',default=[5,20,21]);args=P.parse_args()
class Client:
 def __init__(self,name):
  self.s=socket.create_connection((args.host,args.port),args.timeout);self.s.settimeout(args.timeout);self.n=0;self.bytes=0;self.name=name
 def send(self,x):
  b=x.encode();self.bytes+=4+len(b);self.s.sendall(struct.pack('!I',len(b))+b)
 def read(self):
  def exact(n):
   b=b''
   while len(b)<n:
    p=self.s.recv(n-len(b))
    if not p: raise EOFError()
    b+=p
   return b
  n=struct.unpack('!I',exact(4))[0];b=exact(n);self.bytes+=4+n;return b.decode().split('\t')
 def cmd(self,cmd,*parts):
  self.n+=1;r=str(self.n);self.send('\t'.join(('1',cmd,r,*parts)));return self.read()
 def drain(self,r):
  out=[]
  while True:
   x=self.read();assert x[2]==r
   if x[1]=='END':return out
   assert x[1]=='MESSAGE';out.append((x[3],x[4],x[5]));self.send('\t'.join(('1','ACK',r,x[3])))
 def login(self):
  x=self.cmd('LOGIN',self.name);assert x[1]=='OK';return self.drain(str(self.n))
 def pending(self):
  self.n+=1;r=str(self.n);self.send('\t'.join(('1','GET_PENDING',r)));return self.drain(r)
 def close(self):
  assert self.cmd('LOGOUT')[1]=='OK';self.s.close()
rows=[]
for count in args.counts:
 for trial in range(1,args.repeats+1):
  server=subprocess.Popen([args.exe,'server','--host',args.host,'--port',str(args.port),'--timeout',str(round(args.timeout*1000))],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
  try:
   deadline=time.monotonic()+args.timeout
   while True:
    try:
     a=Client('alice')
     break
    except ConnectionRefusedError:
     if server.poll() is not None: raise RuntimeError(server.stderr.read())
     if time.monotonic()>=deadline: raise
     time.sleep(.05)
   b,c=[Client(n) for n in ('bob','carol')]
   assert not a.login() and not b.login() and not c.login();b.close()
   start_bytes=a.bytes+c.bytes
   t=time.perf_counter();accepted=0;rejected=0;expected=[]
   for i in range(count):
    x=(a if i%2==0 else c).cmd('SEND','bob',f'message-{i+1:02d}')
    if x[1]=='OK':
     accepted+=1
     expected.append((x[4], 'alice' if i%2==0 else 'carol', f'message-{i+1:02d}'))
    elif x[3]=='QUEUE_FULL':rejected+=1
    else:raise AssertionError(x)
   elapsed=(time.perf_counter()-t)*1000
   total=a.bytes+c.bytes-start_bytes
   b=Client('bob');msgs=b.login();assert msgs==expected
   assert not b.pending();b.close();b=Client('bob');assert not b.login();b.close()
   assert accepted==min(count,20) and rejected==max(count-20,0)
   a.close();c.close();rows.append([count,trial,round(elapsed,3),accepted,rejected,len(msgs),total,round(elapsed/count,3),count,0,round(total*1000/elapsed,1)])
   print('PASS',rows[-1],flush=True)
  finally:
   if server.poll() is None:
    server.stdin.write('stop\n');server.stdin.flush()
    try:server.wait(timeout=12)
    except subprocess.TimeoutExpired:
     server.kill();server.wait();raise
   assert server.returncode==0,server.stderr.read()
out=pathlib.Path(__file__).with_name('results.csv')
with out.open('w',newline='',encoding='utf-8') as f:
 w=csv.writer(f);w.writerow(['attempted','trial','send_total_ms','accepted','rejected','delivered','send_frame_bytes','mean_send_rtt_ms','send_responses','timeouts','send_bytes_per_s']);w.writerows(rows)
