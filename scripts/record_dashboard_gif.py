"""Record docs/images/dashboard.gif by driving the running dashboard in a headless browser.

usage:
    streamlit run dashboard/app.py          # in another terminal, with LOB_DATA_DIR set
    python3 scripts/record_dashboard_gif.py [url] [output.gif]

Needs `pip install playwright pillow` and `playwright install chromium`. The script clicks
through the dashboard's time buttons, screenshots each state, and stitches them into a GIF.
"""

from __future__ import annotations

import io
import sys
import time
from pathlib import Path

from PIL import Image
from playwright.sync_api import Page, sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8501"
# Open SPY on the holdout day just before a moment where the model is confident (66% "down")
# while the spread is 15 cents wide: the dashboard flags exactly the research finding.
URL = f"{BASE}/?day=20200130&symbol=SPY&t=10:00:08"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("docs/images/dashboard.gif")
VIEWPORT = {"width": 1440, "height": 1300}
WIDTH = 1000  # GIF width in pixels

# (button to click, or None for the first frame; how long to show the frame, in ms).
STEPS = [
    (None, 2000),  # 10:00:08
    ("+1 s", 1200),  # 10:00:09
    ("+1 s", 1200),  # 10:00:10
    ("+1 s", 3500),  # 10:00:11: confident, but a 15-cent spread
    ("+1 s", 1500),  # 10:00:12
    ("+1 min", 1800),  # 10:01:12
    ("+1 min", 2500),  # 10:02:12
]


def wait_until_idle(page: Page) -> None:
    """Wait for Streamlit to finish rerunning the script and the charts to draw."""
    time.sleep(0.5)
    page.wait_for_selector('[data-testid="stStatusWidget"]', state="detached", timeout=120_000)
    page.wait_for_load_state("networkidle")
    time.sleep(1.0)


def main() -> None:
    frames: list[Image.Image] = []
    durations: list[int] = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport=VIEWPORT, device_scale_factor=1)
        page.goto(URL)
        page.wait_for_selector('[data-testid="stMetric"]', timeout=180_000)
        wait_until_idle(page)
        for button, ms in STEPS:
            if button is not None:
                page.get_by_role("button", name=button, exact=True).click()
                wait_until_idle(page)
            shot = Image.open(io.BytesIO(page.screenshot()))
            height = round(shot.height * WIDTH / shot.width)
            frames.append(shot.convert("RGB").resize((WIDTH, height), Image.LANCZOS))
            durations.append(ms)
            print(f"frame {len(frames)}: {button or 'start'}")
        browser.close()

    palette = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT) for f in frames]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    palette[0].save(OUT, save_all=True, append_images=palette[1:], duration=durations, loop=0,
                    optimize=True)  # fmt: skip
    print(f"wrote {OUT} ({OUT.stat().st_size / 1e6:.1f} MB, {sum(durations) / 1000:.0f} s)")


if __name__ == "__main__":
    main()
