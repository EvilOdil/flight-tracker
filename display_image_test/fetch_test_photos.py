#!/usr/bin/env python3
"""Fetch real aircraft photos from planespotters.net for bench testing.

Usage: python3 fetch_test_photos.py <icao24hex> [<icao24hex> ...]

Example (live hexes change constantly — grab fresh ones from
https://globe.adsb.lol or any ADS-B tracker if these are stale):
    python3 fetch_test_photos.py a1b2c3 aa8cb2

For each hex: looks up photos via the same planespotters pub API endpoint
and descriptive User-Agent contract backend/lib/upstream.js uses (that file
is the source of truth for the UA string and endpoint — planespotters
403s generic UAs, see CLAUDE.md Iron Rule 8), downloads the
`thumbnail_large` JPEG, and saves it into images/<hex>.jpg. Run
convert_images.py afterward (or let this script do it automatically) to
embed the downloads into images_data.h — planespotters thumbnails are
progressive and larger than the panel, so that step's Pillow
auto-normalize path is what makes them usable (see convert_images.py).
"""
import io
import json
import os
import sys
import urllib.error
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMAGES_DIR = os.path.join(SCRIPT_DIR, "images")

# Mirrors backend/lib/upstream.js's USER_AGENT and PS_BASE exactly.
# planespotters rejects generic UAs — keep this in sync with upstream.js if
# that ever changes (Iron Rule 8: descriptive UA, contact mailto).
USER_AGENT = "flight-tracker-gauge/1.0 (+mailto:odiljanandith@gmail.com)"
PS_BASE = "https://api.planespotters.net/pub/photos"


def fetch_json(url):
    req = urllib.request.Request(url, headers={"accept": "application/json", "user-agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.load(resp)


def fetch_bytes(url):
    req = urllib.request.Request(url, headers={"user-agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read()


def fetch_one(hexcode):
    hexcode = hexcode.strip().lower()
    try:
        data = fetch_json(f"{PS_BASE}/hex/{hexcode}")
    except (urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError) as e:
        print(f"FAIL {hexcode}: lookup error ({e})")
        return False

    photos = data.get("photos") or []
    if not photos:
        print(f"SKIP {hexcode}: no photos on planespotters for this hex")
        return False

    p = photos[0]
    thumb = p.get("thumbnail_large") or p.get("thumbnail")
    src = thumb and thumb.get("src")
    if not src:
        print(f"SKIP {hexcode}: photo entry has no usable thumbnail URL")
        return False

    try:
        img_bytes = fetch_bytes(src)
    except (urllib.error.URLError, urllib.error.HTTPError) as e:
        print(f"FAIL {hexcode}: download error ({e})")
        return False

    os.makedirs(IMAGES_DIR, exist_ok=True)
    out_path = os.path.join(IMAGES_DIR, f"{hexcode}.jpg")
    with open(out_path, "wb") as f:
        f.write(img_bytes)

    photographer = p.get("photographer", "?")
    size = thumb.get("size", {})
    print(f"OK {hexcode}: {len(img_bytes)} bytes, {size.get('width', '?')}x{size.get('height', '?')}, "
          f"photo by {photographer} -> images/{hexcode}.jpg")
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    hexes = sys.argv[1:]
    ok = sum(fetch_one(h) for h in hexes)
    print(f"\n{ok}/{len(hexes)} photo(s) saved to {IMAGES_DIR}/")

    if ok:
        print("Running convert_images.py to embed them...\n")
        sys.path.insert(0, SCRIPT_DIR)
        import convert_images
        convert_images.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
