#!/usr/bin/env python3
"""Synthetic bytecode ABI and malformed-input checks; no game assets."""
import struct
import unittest
from import_censhine_pack import Reader,adapt,collection_shaders,retail_shaders

def binary(*w):return struct.pack('<%dI'%len(w),*w)
class CenshineTests(unittest.TestCase):
    def test_complex_fog_abi(self):
        # mov r0, c1; mov r1, c2; mov r2, c3; mov r3, c0
        source=binary(0xffff0200,0x02000001,0x800f0000,0xa0e40001,0x02000001,0x800f0001,0xa0e40002,0x02000001,0x800f0002,0xa0e40003,0x02000001,0x800f0003,0xa0e40000,0xffff)
        result=adapt(source,True,True);w=struct.unpack('<%dI'%(len(result)//4),result)
        # Two constant definitions precede the original instructions.
        self.assertEqual([w[i]&2047 for i in (15,18,21,24)],[0,1,2,16])
        self.assertEqual(struct.unpack('<4f',result[36:52]),(1,1,1,1))
    def test_renderer_retains_simple_fog(self):
        source=binary(0xffff0200,0x02000001,0x800f0000,0x90ff0000,0xffff)
        result=adapt(source,True,False);w=struct.unpack('<%dI'%(len(result)//4),result)
        self.assertEqual(w[15],0xa0ff0011) # v0.wwww becomes defined-zero c17.wwww
    def test_lightmap_keeps_lighting_registers(self):
        source=binary(0xffff0200,0x02000001,0x800f0000,0xa0e40005,0x02000001,0x800f0001,0xa0ff0006,0xffff)
        result=adapt(source);w=struct.unpack('<%dI'%(len(result)//4),result)
        self.assertEqual(w[9],0xa0e40005);self.assertEqual(w[12],0xa0ff0011)
    def test_reject_bad_input(self):
        for data in (b'',b'bad',binary(0xffff0300,0xffff),binary(0xffff0200,0x05000051,0xffff),binary(0xffff0200,0xffff,0)):
            with self.assertRaises(ValueError):adapt(data)
        with self.assertRaises(ValueError):collection_shaders(b'x'*100)
        with self.assertRaises(ValueError):retail_shaders(binary(100)+b'x')
        with self.assertRaises(ValueError):Reader(b'').u32()
if __name__=='__main__':unittest.main()
