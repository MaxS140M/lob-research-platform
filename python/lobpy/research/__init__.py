"""ML research on order book data: features, labels, leakage-free validation and models.

See docs/research-report.md for the method and results, and scripts/run_research.py to
reproduce them.
"""

from .features import CLASSES, DOWN, FEATURES, FLAT, UP, book_features, build_day, direction_labels
from .strategy import PredictionTaker
from .validation import Fold, time_blocks, walk_forward

__all__ = [
    "CLASSES",
    "DOWN",
    "FEATURES",
    "FLAT",
    "UP",
    "Fold",
    "PredictionTaker",
    "book_features",
    "build_day",
    "direction_labels",
    "time_blocks",
    "walk_forward",
]
