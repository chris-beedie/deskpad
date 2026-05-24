"""Generate 60x60 JPEGs for the AKP03E LCD keys.

Three pages of six keys, each with a default and a pressed (shrunk-on-black)
variant — 36 files total. Each image is pre-rotated 90 degrees CCW to
compensate for the device's own +90 deg display rotation (mirajazz protocol
v3, ImageRotation::Rot90).

User-supplied images dropped into ../source_images/pageX_keyY.{png,jpg,webp,
gif,bmp} override the procedural icons for that slot. Anything Pillow can open
is fine; the script handles cropping to square, resize, alpha compositing,
and rotation.

This script is invoked at CMake configure time by main/CMakeLists.txt, so any
edit here, or any add/remove/edit in source_images/, re-triggers regeneration
on the next idf.py build.
"""
from pathlib import Path
from PIL import Image, ImageDraw

SIZE = 60
PRESSED_INNER = 48
ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "main" / "keyimages"
SOURCE_DIR = ROOT / "source_images"
SOURCE_EXTS = (".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tiff", ".tif")


def find_source(page: int, key: int) -> Path | None:
    """Return the first ../source_images/page{page}_key{key}.* match, or None."""
    if not SOURCE_DIR.is_dir():
        return None
    stem = f"page{page}_key{key}"
    for ext in SOURCE_EXTS:
        p = SOURCE_DIR / f"{stem}{ext}"
        if p.is_file():
            return p
    return None


def load_source(path: Path) -> Image.Image:
    """Open a user image, composite alpha over black, centre-crop to square,
    resize to SIZE x SIZE. Returns a 60x60 RGB image ready for save()."""
    img = Image.open(path)
    if img.mode in ("RGBA", "LA"):
        bg = Image.new("RGB", img.size, (0, 0, 0))
        bg.paste(img, mask=img.split()[-1])
        img = bg
    else:
        img = img.convert("RGB")
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2,
                    (w - s) // 2 + s, (h - s) // 2 + s))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def save(img: Image.Image, name: str) -> None:
    rotated = img.rotate(-90, expand=False)
    p = OUT_DIR / name
    rotated.save(p, "JPEG", quality=92, optimize=True)
    print(f"  {p.name}: {p.stat().st_size} B")


def make_pressed(img: Image.Image) -> Image.Image:
    """Shrink the whole image, paste centred on a black border."""
    out = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    inner = img.resize((PRESSED_INNER, PRESSED_INNER), Image.LANCZOS)
    off = (SIZE - PRESSED_INNER) // 2
    out.paste(inner, (off, off))
    return out


# ---- Page 0: arcade icons (current KVM page) ------------------------------

def pixel_sprite(grid, palette, bg):
    rows = len(grid)
    cols = max(len(r) for r in grid)
    cell = min((SIZE - 4) // cols, (SIZE - 4) // rows)
    w = cols * cell
    h = rows * cell
    ox = (SIZE - w) // 2
    oy = (SIZE - h) // 2
    img = Image.new("RGB", (SIZE, SIZE), bg)
    draw = ImageDraw.Draw(img)
    for ry, row in enumerate(grid):
        for cx, ch in enumerate(row):
            if ch == " " or ch not in palette:
                continue
            x0 = ox + cx * cell
            y0 = oy + ry * cell
            draw.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], fill=palette[ch])
    return img


def pacman() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.pieslice([4, 4, 56, 56], start=30, end=330, fill=(255, 220, 0))
    d.ellipse([28, 12, 36, 20], fill=(0, 0, 0))
    return img


def ghost() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (0, 0, 30))
    d = ImageDraw.Draw(img)
    red = (230, 40, 40)
    d.pieslice([6, 4, 54, 52], start=180, end=360, fill=red)
    d.rectangle([6, 28, 54, 50], fill=red)
    for x in (10, 26, 42):
        d.polygon([(x, 50), (x + 8, 50), (x + 4, 56)], fill=(0, 0, 30))
    d.ellipse([14, 20, 26, 34], fill=(255, 255, 255))
    d.ellipse([34, 20, 46, 34], fill=(255, 255, 255))
    d.ellipse([19, 25, 25, 33], fill=(40, 60, 230))
    d.ellipse([39, 25, 45, 33], fill=(40, 60, 230))
    return img


def space_invader() -> Image.Image:
    grid = [
        "  X     X  ",
        "   X   X   ",
        "  XXXXXXX  ",
        " XX XXX XX ",
        "XXXXXXXXXXX",
        "X XXXXXXX X",
        "X X     X X",
        "   XX XX   ",
    ]
    return pixel_sprite(grid, {"X": (40, 230, 80)}, (8, 8, 24))


def mushroom() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (90, 170, 240))
    d = ImageDraw.Draw(img)
    red, white, peach, eye = (210, 40, 40), (255, 245, 230), (250, 215, 175), (40, 30, 30)
    d.pieslice([6, 4, 54, 52], start=180, end=360, fill=red)
    d.rectangle([6, 26, 54, 32], fill=red)
    d.ellipse([14, 10, 24, 20], fill=white)
    d.ellipse([36, 10, 46, 20], fill=white)
    d.ellipse([26, 16, 34, 24], fill=white)
    d.rectangle([16, 32, 44, 54], fill=peach)
    d.ellipse([16, 38, 44, 54], fill=peach)
    d.rectangle([21, 38, 25, 46], fill=eye)
    d.rectangle([35, 38, 39, 46], fill=eye)
    return img


def skull() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (8, 8, 8))
    d = ImageDraw.Draw(img)
    bone, dark = (240, 240, 230), (8, 8, 8)
    d.ellipse([6, 4, 54, 44], fill=bone)
    d.rectangle([14, 36, 46, 52], fill=bone)
    d.ellipse([14, 44, 46, 56], fill=bone)
    d.ellipse([14, 16, 26, 30], fill=dark)
    d.ellipse([34, 16, 46, 30], fill=dark)
    d.polygon([(30, 26), (26, 34), (34, 34)], fill=dark)
    for x in (22, 28, 34, 40):
        d.line([(x, 44), (x, 52)], fill=dark, width=2)
    return img


def cherries() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (255, 252, 220))
    d = ImageDraw.Draw(img)
    red, hl, green, brown = (210, 30, 50), (255, 180, 180), (40, 160, 60), (110, 70, 30)
    d.ellipse([8, 32, 32, 56], fill=red)
    d.ellipse([30, 36, 54, 58], fill=red)
    d.ellipse([14, 36, 20, 42], fill=hl)
    d.ellipse([36, 40, 42, 46], fill=hl)
    d.line([(20, 34), (28, 12)], fill=brown, width=3)
    d.line([(42, 38), (32, 12)], fill=brown, width=3)
    d.polygon([(28, 12), (44, 4), (40, 16)], fill=green)
    return img


# ---- Page 1: media glyphs --------------------------------------------------

MEDIA_BG = (15, 15, 25)
MEDIA_FG = (235, 235, 235)


def _speaker(d: ImageDraw.ImageDraw) -> None:
    # Speaker trapezoid: cone + cabinet
    d.rectangle([10, 24, 22, 36], fill=MEDIA_FG)
    d.polygon([(22, 18), (34, 8), (34, 52), (22, 42)], fill=MEDIA_FG)


def play_pause() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    d.polygon([(18, 12), (18, 48), (46, 30)], fill=MEDIA_FG)
    return img


def prev_track() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    d.rectangle([12, 14, 18, 46], fill=MEDIA_FG)
    d.polygon([(46, 14), (46, 46), (22, 30)], fill=MEDIA_FG)
    return img


def next_track() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    d.polygon([(14, 14), (14, 46), (38, 30)], fill=MEDIA_FG)
    d.rectangle([42, 14, 48, 46], fill=MEDIA_FG)
    return img


def vol_up() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    _speaker(d)
    d.arc([34, 16, 50, 44], start=300, end=60, fill=MEDIA_FG, width=2)
    d.arc([38, 8, 56, 52], start=300, end=60, fill=MEDIA_FG, width=2)
    return img


def vol_down() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    _speaker(d)
    d.arc([34, 16, 50, 44], start=300, end=60, fill=MEDIA_FG, width=2)
    return img


def mute() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), MEDIA_BG)
    d = ImageDraw.Draw(img)
    _speaker(d)
    d.line([(38, 18), (54, 42)], fill=(230, 80, 80), width=3)
    d.line([(54, 18), (38, 42)], fill=(230, 80, 80), width=3)
    return img


# ---- Page 2: system glyphs -------------------------------------------------

SYS_BG = (20, 24, 32)
SYS_FG = (220, 220, 225)
SYS_ACCENT = (90, 170, 240)


def lock_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), SYS_BG)
    d = ImageDraw.Draw(img)
    # Shackle
    d.arc([16, 8, 44, 36], start=180, end=360, fill=SYS_FG, width=4)
    # Body
    d.rounded_rectangle([12, 26, 48, 52], radius=4, fill=SYS_ACCENT)
    # Keyhole
    d.ellipse([27, 33, 33, 39], fill=SYS_BG)
    d.rectangle([29, 37, 31, 46], fill=SYS_BG)
    return img


def sleep_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), SYS_BG)
    d = ImageDraw.Draw(img)
    # Three Z's staggered
    for (x, y, s) in [(8, 38, 14), (22, 24, 18), (38, 8, 20)]:
        d.line([(x, y), (x + s, y)],         fill=SYS_FG, width=3)
        d.line([(x + s, y), (x, y + s)],     fill=SYS_FG, width=3)
        d.line([(x, y + s), (x + s, y + s)], fill=SYS_FG, width=3)
    return img


def screenshot_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), SYS_BG)
    d = ImageDraw.Draw(img)
    # Camera body
    d.rounded_rectangle([8, 18, 52, 50], radius=4, fill=SYS_FG)
    # Viewfinder bump
    d.rectangle([22, 12, 38, 20], fill=SYS_FG)
    # Lens
    d.ellipse([22, 24, 38, 44], fill=SYS_BG)
    d.ellipse([26, 28, 34, 40], fill=SYS_ACCENT)
    return img


def rec_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([14, 14, 46, 46], fill=(220, 30, 30))
    return img


def dnd_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), SYS_BG)
    d = ImageDraw.Draw(img)
    # Bell
    d.pieslice([14, 8, 46, 40], start=180, end=360, fill=SYS_FG)
    d.rectangle([14, 24, 46, 42], fill=SYS_FG)
    d.rectangle([10, 42, 50, 46], fill=SYS_FG)
    d.ellipse([26, 46, 34, 54], fill=SYS_FG)
    # Slash
    d.line([(10, 50), (50, 10)], fill=(230, 80, 80), width=4)
    return img


def calc_icon() -> Image.Image:
    img = Image.new("RGB", (SIZE, SIZE), SYS_BG)
    d = ImageDraw.Draw(img)
    # Body
    d.rounded_rectangle([10, 6, 50, 54], radius=4, fill=SYS_FG)
    # Display
    d.rectangle([14, 10, 46, 20], fill=(60, 90, 60))
    # 4x3 button grid
    for r in range(4):
        for c in range(3):
            x = 14 + c * 12
            y = 24 + r * 8
            d.rectangle([x, y, x + 8, y + 6], fill=SYS_BG)
    return img


# ---- Page definitions + main -----------------------------------------------

PAGES = [
    ("page0", [
        ("key0", pacman),
        ("key1", ghost),
        ("key2", space_invader),
        ("key3", mushroom),
        ("key4", skull),
        ("key5", cherries),
    ]),
    ("page1", [
        ("key0", play_pause),
        ("key1", prev_track),
        ("key2", next_track),
        ("key3", vol_up),
        ("key4", vol_down),
        ("key5", mute),
    ]),
    ("page2", [
        ("key0", lock_icon),
        ("key1", sleep_icon),
        ("key2", screenshot_icon),
        ("key3", rec_icon),
        ("key4", dnd_icon),
        ("key5", calc_icon),
    ]),
]


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"writing 60x60 JPEGs to {OUT_DIR}")
    for page_idx, (page_name, keys) in enumerate(PAGES):
        for key_idx, (key_name, fn) in enumerate(keys):
            src = find_source(page_idx, key_idx)
            if src is not None:
                print(f"  using source: {src.name}")
                img = load_source(src)
            else:
                img = fn()
            save(img,               f"{page_name}_{key_name}.jpg")
            save(make_pressed(img), f"{page_name}_{key_name}_pressed.jpg")


if __name__ == "__main__":
    main()
