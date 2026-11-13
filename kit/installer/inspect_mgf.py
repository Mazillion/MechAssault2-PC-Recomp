"""Read owned MA2 archives using MGF Explorer's published directory format.

Reference: https://github.com/LarvalExtract/MGF-Explorer/blob/master/src/MGF/Structures/mgf.h
Compressed archives contain independently checksummed zlib blocks. Only blocks
needed for the selected file are expanded; extracted game files stay unchanged.
"""
from pathlib import Path
import argparse,struct,zlib

class Mgf:
    def __init__(self,path):
        self.data=Path(path).read_bytes();self.blocks={};self.chunks=[]
        if self.data[:4]==b'mgf ':self.size=len(self.data)
        elif self.data[4:8]==bytes.fromhex('0f5a73c5'):
            count,self.size=struct.unpack_from('<II',self.data,8)
            self.block_size=struct.unpack_from('<I',self.data,20)[0]
            self.chunks=[struct.unpack_from('<III',self.data,32+n*12) for n in range(count)]
        else:raise ValueError('Unknown archive header')
        header=struct.unpack('<11I',self.read(0,44))
        if header[0]!=0x2066676d or self.read(4,1)!=b'\x04':raise ValueError('Not an MA2 archive')
        file_count,file_length,file_offset,dir_count,dir_length,dir_offset,strings_length,strings_offset=header[3:]
        if file_length!=file_count*32 or dir_length!=dir_count*24:raise ValueError('Invalid archive directory sizes')
        records=self.read(file_offset,file_length);strings=self.read(strings_offset,strings_length)
        self.records=[struct.unpack_from('<8I',records,n*32) for n in range(file_count)]
        rows=self.read(dir_offset,dir_length)
        self.rows=[struct.unpack_from('<6i',rows,n*24) for n in range(dir_count)]
        self.names=[strings[r[4]:].split(b'\0')[0].decode('ascii',errors='replace') for r in self.rows]
        self.files={self.name(n).replace('\\','/').strip('/'):r[5] for n,r in enumerate(self.rows) if r[5]>=0}
    def read(self,address,size):
        if address<0 or size<0 or address+size>self.size:raise ValueError('Archive range outside file')
        if not self.chunks:return self.data[address:address+size]
        result=[]
        while size:
            number,offset=divmod(address,self.block_size)
            if number not in self.blocks:
                start,length,_=self.chunks[number]
                self.blocks[number]=zlib.decompress(self.data[start:start+length])
            part=self.blocks[number][offset:offset+size]
            if not part:raise ValueError('Compressed archive block is short')
            result.append(part);address+=len(part);size-=len(part)
        return b''.join(result)
    def name(self,index):
        parts=[];seen=set()
        while index>=0:
            if index in seen:raise ValueError('Archive directory cycle')
            seen.add(index);parts.append(self.names[index]);index=self.rows[index][1]
        return '/'.join(reversed(parts))
    def file(self,name):
        key=next(n for n in self.files if n.lower()==name.lower())
        row=self.records[self.files[key]]
        return self.read(row[6],row[3])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('archive');p.add_argument('--filter',default='');p.add_argument('--file');p.add_argument('--output',type=Path)
    a=p.parse_args();mgf=Mgf(a.archive)
    if a.file:
        data=mgf.file(a.file)
        if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data)
        else:print(data.decode('utf-8',errors='replace'))
    else:print('\n'.join(n for n in mgf.files if a.filter.lower() in n.lower()))
