"""Print the Markdown result tables for docs/research/tables.md from run_research.py's JSON.

usage: python3 scripts/research_tables.py docs/research/results.json
"""

from __future__ import annotations

import json
import sys

MODELS = ["Majority class", "Imbalance rule", "Logistic regression", "LightGBM"]


def main() -> None:
    r = json.loads(open(sys.argv[1]).read())
    s = r["setup"]
    print(f"Dev rows: {s['dev_rows']:,}; holdout rows: {s['holdout_rows']:,}\n")

    print("### Class balance (down / flat / up)\n")
    print("| Horizon | Development days | Holdout day |\n|---|---|---|")
    for h, res in r["horizons"].items():
        dev = " / ".join(f"{x:.0%}" for x in res["class_share_dev"])
        ho = " / ".join(f"{x:.0%}" for x in res["class_share_holdout"])
        print(f"| {h} | {dev} | {ho} |")

    for section, title in (
        ("walk_forward", "Walk-forward (mean over folds)"),
        ("holdout", "Holdout day (scored once)"),
    ):
        print(f"\n### {title}\n")
        print("| Horizon | Model | Accuracy | Balanced accuracy | Log-loss |")
        print("|---|---|---:|---:|---:|")
        for h, res in r["horizons"].items():
            for m in MODELS:
                x = res[section][m]
                print(
                    f"| {h} | {m} | {x['accuracy']:.1%} | {x['balanced_accuracy']:.1%} "
                    f"| {x['log_loss']:.4f} |"
                )

    print("\n### Walk-forward folds, LightGBM log-loss vs majority class\n")
    print(
        "| Horizon | Test block | Rows | Majority | LightGBM | Better? |\n"
        "|---|---|---:|---:|---:|---|"
    )
    for h, res in r["horizons"].items():
        base = res["walk_forward_folds"]["Majority class"]
        lgbm = res["walk_forward_folds"]["LightGBM"]
        for b, g in zip(base, lgbm, strict=True):
            better = "yes" if g["log_loss"] < b["log_loss"] else "no"
            print(
                f"| {h} | {b['block']} | {b['rows']:,} | {b['log_loss']:.4f} "
                f"| {g['log_loss']:.4f} | {better} |"
            )

    print("\n### Feature importance (share of LightGBM gain)\n")
    horizons = list(r["horizons"])
    print("| Feature | " + " | ".join(horizons) + " |\n|---|" + "---:|" * len(horizons))
    for f in r["horizons"][horizons[0]]["importance"]:
        cells = " | ".join(f"{r['horizons'][h]['importance'][f]:.1%}" for h in horizons)
        print(f"| `{f}` | {cells} |")

    print("\n### Trading the predictions on the holdout day\n")
    print(
        "| Horizon | Threshold | Signals | Round trips | Signal PnL (before costs) | Spread "
        "| Fees | Net PnL (after costs) |"
    )
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for h, res in r["horizons"].items():
        for threshold, rows in res["backtest"].items():
            tot = {
                k: sum(row[k] for row in rows)
                for k in ("signals", "traded_shares", "signal_pnl", "spread_pnl", "fees", "net_pnl")
            }
            print(
                f"| {h} | {threshold} | {tot['signals']:,} | {tot['traded_shares'] / 200:,.0f} "
                f"| ${tot['signal_pnl']:,.2f} | ${tot['spread_pnl']:,.2f} | ${-tot['fees']:,.2f} "
                f"| ${tot['net_pnl']:,.2f} |"
            )


if __name__ == "__main__":
    main()
