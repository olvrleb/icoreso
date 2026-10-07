"""Builds test.ico with 16/32/48/256 px images (256 px one is PNG)."""
import sys
from PIL import Image, ImageDraw

def draw(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / 256
    d.rounded_rectangle([16 * s, 16 * s, 240 * s, 240 * s], radius=40 * s,
                        fill=(88, 101, 242, 255))
    d.ellipse([64 * s, 64 * s, 192 * s, 192 * s], fill=(255, 255, 255, 255))
    for i in range(5):
        x = (80 + i * 24) * s
        d.line([x, 90 * s, x, 166 * s], fill=(30, 30, 30, 255),
               width=max(1, int(8 * s)))
    return img

out = sys.argv[1]
big = draw(1024).resize((256, 256), Image.LANCZOS)
big.save(out, sizes=[(16, 16), (32, 32), (48, 48), (256, 256)],
         append_images=[])
big.save(out.replace(".ico", "_ref256.png"))
