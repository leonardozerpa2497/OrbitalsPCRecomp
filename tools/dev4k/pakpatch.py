"""pakpatch.py <in.pak> <out.pak> <path in pak>=<new file> ...: replaces files of a UE v11 pak (unencrypted, unsigned). The new
contents are appended uncompressed; each file's encoded entry is rewritten in place (an uncompressed entry is never longer), so the
path hash and directory indexes stay byte for byte; only the primary index hash in the footer changes.
The output must not exist: this tool never overwrites the original or an earlier patch. Use only owned, unencrypted,
unsigned game data. No game data or keys belong in git."""
import struct, sys, hashlib, io
def rd_str(b):
    n=struct.unpack('<i',b.read(4))[0]
    if n==0: return ''
    if n<0: return b.read(-n*2).decode('utf-16le').rstrip('\0')
    return b.read(n).decode('utf-8','replace').rstrip('\0')
def main():
    src,dst=sys.argv[1],sys.argv[2]; repl=dict(a.split('=',1) for a in sys.argv[3:])
    data=open(src,'rb').read(); n=len(data); assert n>=221
    ft=bytearray(data[n-221:]); assert ft[16]==0, 'encrypted index is unsupported'
    magic,ver,ioff,isize=struct.unpack('<IiQQ',ft[17:41]); assert magic==0x5A6F12E1 and ver==11
    assert ioff+isize<=n-221, 'index outside the pak'
    prim=bytearray(data[ioff:ioff+isize]); assert hashlib.sha1(prim).digest()==ft[41:61], 'index hash mismatch'
    idx=io.BytesIO(bytes(prim))
    rd_str(idx); idx.read(4+8)
    if struct.unpack('<i',idx.read(4))[0]: idx.read(36)
    hasfd=struct.unpack('<i',idx.read(4))[0]; fdoff,fdsize=struct.unpack('<QQ',idx.read(16)); idx.read(20)
    encsz=struct.unpack('<i',idx.read(4))[0]; encpos=idx.tell()
    fd=io.BytesIO(data[fdoff:fdoff+fdsize]); files={}
    for _ in range(struct.unpack('<i',fd.read(4))[0]):
        d=rd_str(fd)
        for _ in range(struct.unpack('<i',fd.read(4))[0]):
            nm=rd_str(fd); files[d+nm]=struct.unpack('<i',fd.read(4))[0]
    # the appended data goes after the end of the last region (index, path hash index, directory index): before the footer
    end=n-221
    out=bytearray(data[:end])
    for path,newfile in repl.items():
        eo=files[path]; p=encpos+eo
        v=struct.unpack_from('<I',prim,p)[0]
        oldlen=4+(4 if (v&0x3f)==0x3f else 0)+(4 if v>>31&1 else 8)+(4 if v>>30&1 else 8)+((4 if v>>29&1 else 8) if (v>>23)&0x3f else 0)
        nb=(v>>6)&0xffff
        if (v>>23)&0x3f and not (nb==1 and not (v>>22)&1): oldlen+=4*nb
        body=open(newfile,'rb').read(); off=len(out)
        out+=struct.pack('<QQQi',0,len(body),len(body),0)+hashlib.sha1(body).digest()+bytes([0])+struct.pack('<I',0)+body
        e=struct.pack('<III',(1<<31)|(1<<30)|(1<<29),off,len(body)); assert len(e)<=oldlen
        prim[p:p+len(e)]=e
        print('replaced',path,len(body),'at',off,'entry',eo,'old entry bytes',oldlen)
    assert ioff<end
    out[ioff:ioff+isize]=prim
    ft[41:61]=hashlib.sha1(prim).digest()
    out+=ft
    with open(dst,'xb') as f: f.write(out)
    print('wrote',dst,len(out))
if __name__=='__main__': main()
