# Writes test.pdf: 200 numbered pages with distinct colours, so frames can be told apart.
# Pages 190-195 are "heavy" (tens of thousands of tiny shapes, ~0.3 s to draw); pages 196-199 are "very heavy"
# (see-through shapes, seconds to draw), so jumping there makes SumatraPDF hold the old Spread for 500 ms and
# then show "Rendering page N...".
import sys, random

out = sys.argv[1] if len(sys.argv) > 1 else "test.pdf"
N = 200
W, H = 595, 842
random.seed(1)

def page_content(i):
    r, g, b = random.random(), random.random(), random.random()
    ops = ["1 1 1 rg 0 0 %d %d re f" % (W, H)]
    ops.append("%.2f %.2f %.2f rg 40 560 515 240 re f" % (r * 0.7, g * 0.7, b * 0.7))
    ops.append("BT /F1 160 Tf 1 1 1 rg 70 600 Td (%d) Tj ET" % (i + 1))
    for k in range(24):
        y = 520 - k * 19
        w = 300 + random.randint(0, 215)
        ops.append("0.2 0.2 0.2 rg 40 %d %d 9 re f" % (y, w))
    if i >= 196:
        ops.append("/G1 gs")
        for k in range(120000):
            x, y = random.uniform(40, 555), random.uniform(40, 540)
            ops.append("%.2f %.2f %.2f rg %.1f %.1f 9 9 re f" % (random.random(), random.random(), random.random(), x, y))
    elif i >= 190:
        for k in range(50000):
            x, y = random.uniform(40, 555), random.uniform(40, 540)
            ops.append("%.2f %.2f %.2f rg %.1f %.1f 1.3 1.3 re f" % (random.random(), random.random(), random.random(), x, y))
    return "\n".join(ops).encode()

objs = []
def add(b):
    objs.append(b)
    return len(objs)

font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>")
pages_id = len(objs) + 1
objs.append(None)
kids = []
for i in range(N):
    c = page_content(i)
    cid = add(b"<< /Length %d >>\nstream\n" % len(c) + c + b"\nendstream")
    pid = add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Resources << /Font << /F1 %d 0 R >> /ExtGState << /G1 << /ca 0.3 >> >> >> /Contents %d 0 R >>" % (pages_id, W, H, font, cid))
    kids.append(pid)
objs[pages_id - 1] = b"<< /Type /Pages /Count %d /Kids [%s] >>" % (N, b" ".join(b"%d 0 R" % k for k in kids))
cat = add(b"<< /Type /Catalog /Pages %d 0 R >>" % pages_id)

data = bytearray(b"%PDF-1.4\n")
offs = []
for n, o in enumerate(objs, 1):
    offs.append(len(data))
    data += b"%d 0 obj\n" % n + o + b"\nendobj\n"
xref = len(data)
data += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs:
    data += b"%010d 00000 n \n" % o
data += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, cat, xref)
open(out, "wb").write(data)
print("wrote", out, len(data) // 1024, "KB")
