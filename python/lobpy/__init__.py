"""lobpy: Python research layer for the lob C++ order book engine.

    >>> import lobpy
    >>> r = lobpy.Replayer("data/20190730.BX_ITCH_50", ["AAPL", "MSFT"])
    >>> snaps = r.snapshots(interval_ns=1_000_000_000, depth=5)   # dict of NumPy arrays
    >>> df = lobpy.snapshots_frame(snaps, r.symbols)               # tidy pandas DataFrame

The replay runs in C++ (see lobpy._lob); results come back as NumPy arrays without copying.
"""

from ._lob import PRICE_SCALE, Replayer
from .frames import snapshots_frame, trades_frame

__version__ = "0.4.0"

__all__ = ["PRICE_SCALE", "Replayer", "snapshots_frame", "trades_frame", "__version__"]
