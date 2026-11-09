import csv, statistics
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
p=Path(__file__).parent
rows=list(csv.DictReader((p/'results.csv').open(encoding='utf-8')))
font='/System/Library/Fonts/Supplemental/Arial.ttf'
try: F=ImageFont.truetype(font,25); T=ImageFont.truetype(font,29)
except OSError: F=T=ImageFont.load_default()
def graph(title,ylabel,key,out):
 im=Image.new('RGB',(1100,650),'white');d=ImageDraw.Draw(im);d.text((70,20),title,font=T,fill='black'); d.text((75,75),ylabel,font=F,fill='black');d.line((120,510,1030,510),fill='black',width=3);d.line((120,125,120,510),fill='black',width=3)
 counts=sorted({int(r['attempted']) for r in rows}); vals=[statistics.mean(float(r[key]) for r in rows if int(r['attempted'])==n) for n in counts]; ymax=max(vals)*1.2 or 1
 for j in range(6):
  y=510-j*370/5;v=ymax*j/5;d.line((120,y,1030,y),fill='#dddddd');d.text((15,y-12),f'{v:.2f}',font=F,fill='black')
 pts=[]
 for i,(n,v) in enumerate(zip(counts,vals)):
  x=240+i*330; y=510-v/ymax*370;pts.append((x,y));d.ellipse((x-7,y-7,x+7,y+7),fill='#244b80');d.text((x-12,525),str(n),font=F,fill='black');d.text((x-25,y-35),f'{v:.2f}',font=F,fill='black')
 d.line(pts,fill='#244b80',width=3);d.text((340,580),'Количество отправленных сообщений, шт.',font=F,fill='black');im.save(p/out)
graph('Время отправки сообщений','Время, мс','send_total_ms','time.png')
graph('Отклонённые сообщения','Количество, шт.','rejected','rejected.png')
