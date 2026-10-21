"""Build flim.exe's icon (launcher/flim.ico, several sizes) from the bean artwork.
Usage: python gen_icon.py [source.png]   (default: <repo>/bean/bean256.png)"""
from PIL import Image
import os, sys

here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "bean", "bean256.png")
out = os.path.join(here, "launcher", "flim.ico")

img = Image.open(src).convert("RGBA")
w, h = img.size
side = max(w, h)                       # pad to a square, centred
sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
sq.paste(img, ((side - w) // 2, (side - h) // 2))
sizes = [16, 24, 32, 48, 64, 128, 256]
sq.save(out, format="ICO", sizes=[(s, s) for s in sizes])
print("wrote", out, "sizes", sizes)
