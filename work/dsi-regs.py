import mmap, os, struct, sys
BASE = 0x04700000
CTRL, VID_CFG0, LANE_CTRL = 0x00, 0x0c, 0xa8
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=BASE)
def rd(o): return struct.unpack("<I", m[o:o+4])[0]
def wr(o, v): m[o:o+4] = struct.pack("<I", v)
def show(tag):
    v, l = rd(VID_CFG0), rd(LANE_CTRL)
    print("%s CTRL=%08x VID_CFG0=%08x (bllp=%d eof_bllp=%d hsa=%d hbp=%d hfp=%d pulse=%d) LANE_CTRL=%08x (clk_cont=%d)" % (
        tag, rd(CTRL), v, v>>12&1, v>>15&1, v>>16&1, v>>20&1, v>>24&1, v>>28&1, l, l>>28&1))
show("before")
if len(sys.argv) > 1 and sys.argv[1] == "android":
    wr(LANE_CTRL, rd(LANE_CTRL) | (1 << 28))           # continuous HS clock, as force_clk_lane_hs=1
    v = rd(VID_CFG0) & ~((1 << 16) | (1 << 20))          # keep HS during HSA and HBP, as downstream
    wr(VID_CFG0, v)
    show("after ")
elif len(sys.argv) > 1 and sys.argv[1] == "clkonly":
    wr(LANE_CTRL, rd(LANE_CTRL) | (1 << 28)); show("after ")
