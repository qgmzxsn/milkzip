"""Regenerate the MilkZip application icon from the checked-in mascot image.

Builds use the checked-in ICO. The mascot is a user-provided image; see
THIRD-PARTY-NOTICES.md for its licensing note.
"""
from pathlib import Path

from PIL import Image, ImageOps

root = Path(__file__).resolve().parents[1]
source = root / "assets/milkzip-mascot.jpg"
target = root / "assets/milkzip.ico"

image = Image.open(source).convert("RGBA")
image = ImageOps.fit(image, (256, 256), method=Image.LANCZOS, centering=(0.5, 0.5))
image.save(
    target,
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
