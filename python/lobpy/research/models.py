"""Models, simplest first: two baselines, logistic regression, and LightGBM.

Every model exposes ``fit(X, y)`` and ``predict_proba(X)`` over the classes DOWN / FLAT / UP,
so they are all scored the same way. Hyperparameters are fixed in advance, not tuned on the
test folds (tuning on test data is another form of leakage).
"""

from __future__ import annotations

from typing import Self

import numpy as np
import pandas as pd

from .features import DOWN, FLAT, UP

N_CLASSES = 3


def _frequencies(y: np.ndarray) -> np.ndarray:
    counts = np.bincount(y.astype(int), minlength=N_CLASSES).astype(float) + 1.0  # smoothed
    return counts / counts.sum()


class MajorityBaseline:
    """Always predicts the training class frequencies (so its top class is the most common)."""

    name = "Majority class"

    def fit(self, X: pd.DataFrame, y: np.ndarray) -> Self:
        self.p = _frequencies(y)
        return self

    def predict_proba(self, X: pd.DataFrame) -> np.ndarray:
        return np.tile(self.p, (len(X), 1))


class ImbalanceRule:
    """ "Imbalance > 0 means up": predicts UP for positive top-of-book imbalance, DOWN for
    negative and FLAT for zero. Probabilities are the training frequencies of each outcome
    within each of those three groups, so it can be scored by log-loss too."""

    name = "Imbalance rule"

    def _group(self, X: pd.DataFrame) -> np.ndarray:
        imb = X["imbalance_l1"].fillna(0).to_numpy()
        return np.where(imb > 0, UP, np.where(imb < 0, DOWN, FLAT))

    def fit(self, X: pd.DataFrame, y: np.ndarray) -> Self:
        g = self._group(X)
        self.p = np.vstack([_frequencies(y[g == k]) for k in range(N_CLASSES)])
        return self

    def predict_proba(self, X: pd.DataFrame) -> np.ndarray:
        return self.p[self._group(X)]

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        return self._group(X)  # the rule itself, not the most likely class in each group


class Logistic:
    """Multinomial logistic regression on clipped, standardised features.

    A thin book produces extreme values (a side briefly empties and the spread or mid jumps).
    A linear model is sensitive to those, so each feature is clipped to its 1st-99th
    percentile *of the training data* before scaling. Test data is clipped with the training
    bounds, so nothing about the test set leaks into the fit. NaNs become the training median.
    """

    name = "Logistic regression"

    def _prepare(self, X: pd.DataFrame) -> np.ndarray:
        return X.clip(self.lo, self.hi, axis=1).fillna(self.median).to_numpy()

    def fit(self, X: pd.DataFrame, y: np.ndarray) -> Self:
        from sklearn.linear_model import LogisticRegression
        from sklearn.pipeline import make_pipeline
        from sklearn.preprocessing import StandardScaler

        self.lo, self.hi = X.quantile(0.01), X.quantile(0.99)
        self.median = X.median()
        self.model = make_pipeline(StandardScaler(), LogisticRegression(C=1.0, max_iter=500))
        self.model.fit(self._prepare(X), y.astype(int))
        return self

    def predict_proba(self, X: pd.DataFrame) -> np.ndarray:
        return self.model.predict_proba(self._prepare(X))


class GradientBoosting:
    """LightGBM multiclass. Handles NaNs natively. Conservative fixed settings: shallow trees
    and large leaves, because market data is noisy and overlapping labels make rows far less
    independent than their count suggests."""

    name = "LightGBM"

    def __init__(self, seed: int = 0, min_child_samples: int = 2_000) -> None:
        self.seed = seed
        self.min_child_samples = min_child_samples

    def fit(self, X: pd.DataFrame, y: np.ndarray) -> Self:
        import lightgbm as lgb

        self.model = lgb.LGBMClassifier(
            objective="multiclass",
            n_estimators=150,
            learning_rate=0.05,
            num_leaves=15,
            min_child_samples=self.min_child_samples,
            subsample=0.5,
            subsample_freq=1,
            colsample_bytree=0.8,
            reg_lambda=1.0,
            random_state=self.seed,
            verbose=-1,
            n_jobs=-1,
        )
        self.model.fit(X, y.astype(int))
        return self

    def predict_proba(self, X: pd.DataFrame) -> np.ndarray:
        return self.model.predict_proba(X)

    def importance(self, features: list[str]) -> pd.Series:
        gain = self.model.booster_.feature_importance(importance_type="gain")
        return pd.Series(gain / gain.sum(), index=features)


def score(
    y: np.ndarray, proba: np.ndarray, predicted: np.ndarray | None = None
) -> dict[str, float]:
    """Accuracy, balanced accuracy (mean recall over the three classes) and log-loss."""
    from sklearn.metrics import balanced_accuracy_score, log_loss

    y = y.astype(int)
    pred = proba.argmax(axis=1) if predicted is None else predicted
    return {
        "accuracy": float((pred == y).mean()),
        "balanced_accuracy": float(balanced_accuracy_score(y, pred)),
        "log_loss": float(log_loss(y, np.clip(proba, 1e-9, 1), labels=[DOWN, FLAT, UP])),
    }
