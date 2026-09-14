#!/usr/bin/env python3
"""dexdump.py -- list a class's methods and JNI signatures from an APK's dex.

The engine's .so carries the names of its JNI entry points but not their
signatures: the JVM matches those by name at registration time, so they exist
only in the Java side. That makes the APK's own classes.dex the authoritative
source for what main.c and nx_input.c must match -- guessing from upstream
Godot source risks matching a different point release.

Parses just enough of the DEX format (string_ids / type_ids / proto_ids /
method_ids) to resolve method_id -> class, name, signature.

usage: tools/dexdump.py <apk> [class-substring]     default substring: GodotLib
"""

import struct, sys, zipfile

def uleb(b, o):
    r=0; s=0
    while True:
        x=b[o]; o+=1; r |= (x&0x7f)<<s; s+=7
        if not x&0x80: return r,o

class Dex:
    def __init__(self, b):
        self.b=b
        (self.string_ids_size,self.string_ids_off)=struct.unpack_from('<II',b,56)
        (self.type_ids_size,self.type_ids_off)=struct.unpack_from('<II',b,64)
        (self.proto_ids_size,self.proto_ids_off)=struct.unpack_from('<II',b,72)
        (self.field_ids_size,self.field_ids_off)=struct.unpack_from('<II',b,80)
        (self.method_ids_size,self.method_ids_off)=struct.unpack_from('<II',b,88)
    def string(self,i):
        off=struct.unpack_from('<I',self.b,self.string_ids_off+4*i)[0]
        n,off=uleb(self.b,off)
        e=self.b.index(b'\0',off)
        return self.b[off:e].decode('utf-8','replace')
    def type(self,i):
        return self.string(struct.unpack_from('<I',self.b,self.type_ids_off+4*i)[0])
    def proto(self,i):
        base=self.proto_ids_off+12*i
        shorty,ret,params_off=struct.unpack_from('<III',self.b,base)
        args=[]
        if params_off:
            n=struct.unpack_from('<I',self.b,params_off)[0]
            for k in range(n):
                args.append(self.type(struct.unpack_from('<H',self.b,params_off+4+2*k)[0]))
        return '('+''.join(args)+')'+self.type(ret)
    def methods(self):
        for i in range(self.method_ids_size):
            c,p,n=struct.unpack_from('<HHI',self.b,self.method_ids_off+8*i)
            yield self.type(c), self.string(n), self.proto(p)

if len(sys.argv) < 2:
    sys.exit(__doc__)
want=sys.argv[2] if len(sys.argv)>2 else 'GodotLib'
z=zipfile.ZipFile(sys.argv[1])
for name in z.namelist():
    if name.endswith('.dex'):
        d=Dex(z.read(name))
        for cls,n,proto in d.methods():
            if want in cls:
                print(f'{cls[1:-1].split("/")[-1]}.{n} {proto}')
