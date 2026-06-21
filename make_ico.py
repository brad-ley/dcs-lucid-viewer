"""Convert assets/tv_png.png → assets/tv_png.ico with standard Windows icon sizes."""
from pathlib import Path
from PIL import Image

src = Path(__file__).parent / 'assets' / 'tv_png.png'
dst = src.with_suffix('.ico')

img = Image.open(src).convert('RGBA')
sizes = [16, 24, 32, 48, 64, 128, 256]
img.save(dst, format='ICO', sizes=[(s, s) for s in sizes])
print(f'Saved {dst}')
