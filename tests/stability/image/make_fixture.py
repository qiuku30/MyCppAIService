import struct,zlib,sys
from pathlib import Path
w=h=5000
chunk=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
c=zlib.compressobj(); parts=[]
for _ in range(h): parts.append(c.compress(b'\0'*(1+w*3)))
parts.append(c.flush())
Path(sys.argv[1],'oversize.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',b''.join(parts))+chunk(b'IEND',b''))
out=Path(sys.argv[1])
# Independent PNG encodings: indexed 1-bit, 1-bit grayscale, and Adam7 RGBA8.
for name,color,extra,payload in [('palette.png',3,chunk(b'PLTE',b'\xff\0\0\0\xff\0'),b'\0\x80'),('gray1.png',0,b'',b'\0\x80')]:
 out.joinpath(name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,1,color,0,0,0))+extra+chunk(b'IDAT',zlib.compress(payload))+chunk(b'IEND',b''))
out.joinpath('rgba_interlaced.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,1))+chunk(b'IDAT',zlib.compress(b'\0\xff\x00\x00\xff'))+chunk(b'IEND',b''))
for name,w,h in [('zero.png',0,100),('overflow.png',0xffffffff,0xffffffff)]:
 out.joinpath(name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'))+chunk(b'IEND',b''))
