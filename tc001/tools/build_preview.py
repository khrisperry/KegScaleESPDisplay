"""Make a standalone browser preview and contact sheet from real C framebuffer output."""
from pathlib import Path
import json
import gzip
import base64
from PIL import Image, ImageDraw, ImageFont
root=Path(__file__).resolve().parents[1]
js=(root/'preview/frames.js').read_text()
frames=json.loads(js.removeprefix('const frames = ').rstrip(';\n'))
template=(root/'tools/preview-template.html').read_text()
packed=base64.b64encode(gzip.compress(json.dumps(frames,separators=(',',':')).encode(),mtime=0)).decode()
(root/'preview/index.html').write_text(template.replace('__FRAME_DATA__',packed))
font_path=next((str(p) for p in [Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'),Path('C:/Windows/Fonts/arial.ttf')] if p.exists()),None)
font=ImageFont.truetype(font_path,19) if font_path else ImageFont.load_default(size=19)
large=ImageFont.truetype(font_path,30) if font_path else ImageFont.load_default(size=30)
image=Image.new('RGB',(1040,780),'#10151d')
d=ImageDraw.Draw(image)
d.text((35,22),'Keg TC001 · 32 × 8 pixel firmware preview',font=large,fill='#edf3fb')
d.text((35,65),'Sample data · 42 servings · 68% full · 3.40 gallons',font=font,fill='#aebace')
labels=['Servings remaining','Keg percentage','Gallons remaining','Beer name (scrolls)','Pairing code','Connection lost']
for panel,(page,tick) in enumerate([(0,20),(1,20),(2,20),(3,22),(4,0),(6,0)]):
 x=35+(panel%2)*500;y=115+(panel//2)*215
 d.text((x,y),labels[panel],font=font,fill='#aebace')
 d.rounded_rectangle((x,y+36,x+470,y+192),radius=20,fill='#222a35',outline='#424c5a',width=2)
 d.rounded_rectangle((x+12,y+48,x+458,y+180),radius=12,fill='#030509')
 for py in range(8):
  for px in range(32):
   c=frames[page][tick][py*32+px]
   color=('#%06x'%c) if c else '#111925'
   dx=x+18+px*13.6;dy=y+59+py*13.6
   d.ellipse((dx,dy,dx+9,dy+9),fill=color)
d.text((35,753),'Renderer-accurate pixels; physical device testing pending.',font=font,fill='#aebace')
image.save(root/'preview/screens.png')
(root/'preview/frames.js').unlink()
print('Created standalone preview and screen contact sheet')
