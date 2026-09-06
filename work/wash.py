import time
W, H, STRIDE = 1080, 1920, 4352
colors = [(255,255,255),(0,0,0),(255,0,0),(0,255,0),(0,0,255),(0,0,0),(255,255,255),(128,128,128)]
frames = []
for r, g, b in colors:
    row = bytes([b, g, r, 0]) * W + b"\0" * (STRIDE - W * 4)
    frames.append(row * H)
end = time.time() + 25 * 60
i = 0
with open("/dev/fb0", "r+b", buffering=0) as f:
    while time.time() < end:
        f.seek(0)
        f.write(frames[i % len(frames)])
        i += 1
        time.sleep(1.0)
