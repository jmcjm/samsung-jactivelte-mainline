# Sample the Adreno 320 performance counters through /dev/mem: RBBM_1 counts
# core clock cycles, PWR_1 counts GPU busy cycles. Usage: gpuctr.py <seconds>
import mmap, os, struct, time, sys
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x20000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x04300000)
def r(o): return struct.unpack("<I", m[o*4:o*4+4])[0]
def snap(): return (r(0x94) | (r(0x95) << 32), r(0xec) | (r(0xed) << 32))
a = snap(); time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 1.0); b = snap()
print("rbbm1 delta=%d pwr1 delta=%d (%.1f%% of core clock)" % (b[0]-a[0], b[1]-a[1], 100.0*(b[1]-a[1])/max(1, b[0]-a[0])))
