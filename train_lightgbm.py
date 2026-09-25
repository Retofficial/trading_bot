"""
LightGBM training pipeline for HFT entry model.

Design:
- Regression on log(1 + MFE): target = log(1 + mfe_pct)
- Walk-forward validation grouped by burst_id (prevents data leakage
  across attempts within the same signal burst)
- Sample weights with exponential decay (half-life)
- Grid search over pass-rate: evaluate avg PnL per trade at multiple
  top-k% thresholds
- Bootstrap CI on fold-level metrics
- SHAP feature importance

Feature indices are discovered from column prefixes at runtime.
"""
import pandas as pd
import numpy as np
import lightgbm as lgb
import glob
import os
import sys
import warnings
warnings.filterwarnings('ignore')


# ============================================================================
# CONFIG
# ============================================================================

# Target threshold for defining "winner" (used for classification metrics only;
# model itself is trained as regression on log(1 + mfe))
WINNER_MFE_THRESHOLD = 1.7

# Burst grouping: signals on the same (symbol, direction) within this window
# are considered part of the same burst
BURST_WINDOW_SEC = 10.0

# Pass-rate grid for threshold search
PASS_RATES = [0.05, 0.10, 0.15, 0.20, 0.30, 0.50]

# Sample weight decay
HALF_LIFE_DAYS = 7.0


# ============================================================================
# HELPERS
# ============================================================================

def bootstrap_ci(values, n_bootstrap=1000, alpha=0.05, seed=42):
    """Bootstrap 95% CI for the mean."""
    values = np.asarray(values, dtype=float)
    n = len(values)
    if n == 0:
        return 0.0, 0.0, 0.0
    rng = np.random.default_rng(seed)
    means = np.empty(n_bootstrap)
    for i in range(n_bootstrap):
        idx = rng.integers(0, n, size=n)
        means[i] = np.mean(values[idx])
    lower = float(np.quantile(means, alpha / 2))
    upper = float(np.quantile(means, 1 - alpha / 2))
    return float(np.mean(values)), lower, upper


def compute_groups(group_ids_slice):
    """Convert a sequence of group ids into contiguous group sizes."""
    sizes = []
    if len(group_ids_slice) == 0:
        return np.array([], dtype=np.int32)
    current = group_ids_slice[0]
    cnt = 1
    for gid in group_ids_slice[1:]:
        if gid == current:
            cnt += 1
        else:
            sizes.append(cnt)
            current = gid
            cnt = 1
    sizes.append(cnt)
    return np.array(sizes, dtype=np.int32)


def calc_pnl(mfe, stop_pct=0.35, take1_pct=0.9, take2_pct=1.7, fees=0.04):
    """
    Two-level take profit: half position closed at take1, half at take2.
    - mfe >= take2: half at take1, half at take2
    - take1 <= mfe < take2: half at take1, half at stop
    - mfe < take1: full stop
    """
    if mfe >= take2_pct:
        return 0.5 * take1_pct + 0.5 * take2_pct - fees     # +1.26%
    if mfe >= take1_pct:
        return 0.5 * take1_pct + 0.5 * (-stop_pct) - fees   # +0.235%
    return -stop_pct - fees                                  # -0.39%


def get_feature_columns(data: pd.DataFrame) -> list:
    """
    Discover feature columns from prefixes. The exact set of features used
    in production is project-specific and not disclosed.
    """
    feature_cols = [c for c in data.columns if c.startswith('pre_')]
    # Scalar features appended by the aggregator
    for col in ('amplitude_pct', 'hour', 'inst_id_code'):
        if col in data.columns:
            feature_cols.append(col)
    return feature_cols


# ============================================================================
# DATA LOADING
# ============================================================================

def load_data(data_dir):
    csv_files = glob.glob(os.path.join(data_dir, "signal_log_*.csv"))
    if not csv_files:
        print("No signal log files found")
        sys.exit(1)

    df_list = []
    for f in csv_files:
        try:
            df = pd.read_csv(f)
            if not df.empty:
                df_list.append(df)
                print(f"Read {len(df)} rows from {f}")
        except Exception as e:
            print(f"Error reading {f}: {e}")

    if not df_list:
        print("No valid data")
        sys.exit(1)

    data = pd.concat(df_list, ignore_index=True)
    print(f"Total rows: {len(data)}")

    if 'signal_time' in data.columns:
        data = data.sort_values('signal_time').reset_index(drop=True)

    if 'mfe_pct' not in data.columns:
        print("Column 'mfe_pct' not found")
        sys.exit(1)

    mfe = data['mfe_pct'].values
    data['label'] = (mfe >= WINNER_MFE_THRESHOLD).astype(int)
    print(f"Label split: winners={data['label'].sum()}, "
          f"losers={len(data) - data['label'].sum()}")

    # Winner distribution across amplitude buckets (diagnostic)
    if 'amplitude_pct' in data.columns:
        print("\n--- Winners by amplitude bucket ---")
        buckets = [(1.2, 2.0), (2.0, 2.5), (2.5, 3.5), (3.5, 5.0), (5.0, 100.0)]
        for lo, hi in buckets:
            m = (data['amplitude_pct'] >= lo) & (data['amplitude_pct'] < hi)
            n = int(m.sum())
            if n < 20:
                continue
            w = int(data.loc[m, 'label'].sum())
            print(f"  amp [{lo:>4.1f}, {hi:>5.1f}): n={n:>5}  "
                  f"winners={w:>4} ({w/n*100:>5.1f}%)")

    # Time bucket for grouping (used only as fallback; burst_id used by default)
    data['hour_bucket'] = data['signal_time'] // (3600 * 1_000_000)

    feature_cols = get_feature_columns(data)

    categorical_cols = []
    if 'inst_id_code' in data.columns:
        data['inst_id_code'] = data['inst_id_code'].astype('category')
        categorical_cols.append('inst_id_code')

    if 'signal_time' in data.columns:
        data['hour'] = pd.to_datetime(data['signal_time'], unit='us').dt.hour
        if 'hour' not in feature_cols:
            feature_cols.append('hour')

    missing = [c for c in feature_cols if c not in data.columns]
    if missing:
        print(f"Missing columns: {missing}")
        sys.exit(1)

    X = data[feature_cols].copy()
    for col in feature_cols:
        if col not in categorical_cols:
            X[col] = X[col].fillna(0).astype(np.float64)
        else:
            X[col] = X[col].astype('category')

    y = data['label'].values
    mfe = data['mfe_pct'].values

    # Sample weights with exponential decay
    sample_weights = None
    if 'signal_time' in data.columns:
        max_t = data['signal_time'].max()
        age_days = (max_t - data['signal_time']) / (86400.0 * 1e6)
        sample_weights = np.exp(-age_days / HALF_LIFE_DAYS).values
        print(f"Sample weights: min={sample_weights.min():.4f}, "
              f"max={sample_weights.max():.4f}, mean={sample_weights.mean():.4f}")
        print(f"  (half-life = {HALF_LIFE_DAYS} days, span = {age_days.max():.1f} days)")

    print(f"Feature matrix shape: {X.shape}")
    print(f"Class distribution: losers={(y==0).sum()}, winners={(y==1).sum()}")

    hour_buckets = data['hour_bucket'].values
    return X, y, mfe, feature_cols, categorical_cols, sample_weights, hour_buckets


def load_burst_ids(data_dir):
    """
    Assign burst_id to each signal. Signals on the same (symbol, direction)
    within BURST_WINDOW_SEC share a burst_id. Two consecutive bursts on the
    same key are separated if the gap exceeds the window.
    """
    files = glob.glob(os.path.join(data_dir, "signal_log_*.csv"))
    dfs = []
    for f in files:
        try:
            df = pd.read_csv(f)
            if df.empty:
                continue
            df['symbol'] = os.path.basename(f).replace("signal_log_", "").replace(".csv", "")
            dfs.append(df)
        except Exception:
            continue

    data = pd.concat(dfs, ignore_index=True).sort_values('signal_time').reset_index(drop=True)

    burst_ids = np.full(len(data), -1, dtype=np.int64)
    last_per_key = {}
    next_bid = 0
    for i in range(len(data)):
        sym = data.at[i, 'symbol']
        d = data.at[i, 'direction']
        t = data.at[i, 'signal_time']
        key = (sym, d)
        if key in last_per_key:
            last_bid, last_time = last_per_key[key]
            dt = (t - last_time) / 1e6
            if dt > BURST_WINDOW_SEC:
                next_bid += 1
                bid = next_bid
            else:
                bid = last_bid
        else:
            next_bid += 1
            bid = next_bid
        burst_ids[i] = bid
        last_per_key[key] = (bid, t)
    return burst_ids


def walk_forward_burst_splits(burst_ids, n_splits=10):
    """Walk-forward split by unique burst_id: train/test do not share bursts."""
    unique = pd.unique(burst_ids)
    n = len(unique)
    fold = n // (n_splits + 1)
    if fold < 1:
        return []
    out = []
    for i in range(1, n_splits + 1):
        tr_k = unique[:i * fold]
        te_k = unique[i * fold:(i + 1) * fold]
        if len(te_k) == 0:
            continue
        tr = np.where(np.isin(burst_ids, tr_k))[0]
        te = np.where(np.isin(burst_ids, te_k))[0]
        out.append((tr, te))
    return out


# ============================================================================
# MAIN
# ============================================================================

def main():
    if len(sys.argv) < 2:
        print("Usage: python train_lightgbm.py <data_dir> [model_path]")
        sys.exit(1)

    data_dir = sys.argv[1]
    model_path = sys.argv[2] if len(sys.argv) > 2 else "model_final_reg.txt"

    X, y, mfe, features, categorical_cols, sample_weights, hour_buckets = load_data(data_dir)
    burst_ids = load_burst_ids(data_dir)
    if len(burst_ids) != len(X):
        print(f"ERROR: burst_ids len {len(burst_ids)} != X len {len(X)}")
        sys.exit(1)
    print(f"Unique bursts: {len(np.unique(burst_ids))}")

    # ------------------------------------------------------------------
    # LightGBM regression on log(1 + mfe)
    # ------------------------------------------------------------------
    params = {
        'objective': 'regression',
        'metric': 'l2',
        'boosting': 'gbdt',
        'num_iterations': 300,
        'learning_rate': 0.01,
        'num_leaves': 20,
        'max_depth': 5,
        'min_data_in_leaf': 25,
        'lambda_l1': 1.0,
        'lambda_l2': 0.3,
        'bagging_fraction': 0.75,
        'feature_fraction': 0.65,
        'extra_trees': True,
        'max_bin': 127,
        'verbosity': -1,
        'seed': 42,
    }

    y_reg = np.log1p(mfe)

    # ==================================================================
    # WALK-FORWARD
    # ==================================================================
    tscv_splits = walk_forward_burst_splits(burst_ids, n_splits=10)

    fold_info = []
    print("\n=== Walk-Forward (grouped by burst_id) ===")
    print(f"Pass-rate grid: {[f'{p*100:.0f}%' for p in PASS_RATES]}\n")

    for fold, (train_idx, test_idx) in enumerate(tscv_splits, 1):
        X_tr, X_te = X.iloc[train_idx], X.iloc[test_idx]
        y_tr_reg = y_reg[train_idx]
        y_te_mfe = mfe[test_idx]
        w_tr = sample_weights[train_idx] if sample_weights is not None else None

        lgb_tr = lgb.Dataset(X_tr, y_tr_reg, weight=w_tr,
                             categorical_feature=categorical_cols)
        model_fold = lgb.train(params, lgb_tr, num_boost_round=params['num_iterations'])
        score_te = model_fold.predict(X_te)

        corr = np.corrcoef(score_te, y_te_mfe)[0, 1] if len(score_te) > 5 else 0.0

        row = {'fold': fold, 'total': len(test_idx), 'corr': corr}
        for pr in PASS_RATES:
            k = max(1, int(len(score_te) * pr))
            top_idx = np.argsort(-score_te)[:k]
            top_mfe = y_te_mfe[top_idx]
            pnls = np.array([calc_pnl(m) for m in top_mfe])
            row[f'prec15_{int(pr*100)}'] = (top_mfe >= 1.5).mean() * 100
            row[f'avgPnL_{int(pr*100)}'] = pnls.mean()
            row[f'total_{int(pr*100)}'] = pnls.sum()
        fold_info.append(row)

        print(f"Fold {fold:>2}: n_te={row['total']:>4}, corr={corr:+.4f} | "
              f"top15% avgPnL={row['avgPnL_15']:+.4f}%")

    # ==================================================================
    # FOLD AGGREGATION
    # ==================================================================
    print("\n" + "=" * 78)
    print("FOLD SUMMARY (mean ± std across 10 folds)")
    print("=" * 78)
    print(f"  {'pass%':>6} {'prec@1.5%':>12} {'avgPnL':>12} {'total PnL':>12}")
    for pr in PASS_RATES:
        key_p15 = f'prec15_{int(pr*100)}'
        key_ap = f'avgPnL_{int(pr*100)}'
        key_tp = f'total_{int(pr*100)}'

        p15s = [f[key_p15] for f in fold_info]
        aps = [f[key_ap] for f in fold_info]
        tps = [f[key_tp] for f in fold_info]

        p15_m, p15_lo, p15_hi = bootstrap_ci(p15s)
        ap_m, ap_lo, ap_hi = bootstrap_ci(aps)
        tp_m, _, _ = bootstrap_ci(tps)

        print(f"  {pr*100:>5.0f}% {p15_m:>7.1f}% [{p15_lo:>4.1f},{p15_hi:>4.1f}] "
              f"{ap_m:>+0.4f}% [{ap_lo:>+0.4f},{ap_hi:>+0.4f}]  {tp_m:>+0.1f}%")

    # Select best pass-rate by mean total PnL across folds
    print("\n--- Best pass-rate selection ---")
    best_pr, best_total = None, -np.inf
    for pr in PASS_RATES:
        tps = [f[f'total_{int(pr*100)}'] for f in fold_info]
        mean_total = np.mean(tps)
        print(f"  pass={pr*100:>4.0f}%: mean total PnL = {mean_total:+.2f}%")
        if mean_total > best_total:
            best_total = mean_total
            best_pr = pr
    print(f"\n  Selected pass-rate: {best_pr*100:.0f}% (mean total = {best_total:+.2f}%)")

    BEST_PR = best_pr if best_pr is not None else 0.15

    corrs = [f['corr'] for f in fold_info]
    avg_pnls = [f[f'avgPnL_{int(BEST_PR*100)}'] for f in fold_info]
    corr_mean, corr_lo, corr_hi = bootstrap_ci(corrs)
    pnl_mean, pnl_lo, pnl_hi = bootstrap_ci(avg_pnls)
    print(f"\n  Corr(score, mfe): {corr_mean:+.4f} [{corr_lo:+.4f}, {corr_hi:+.4f}]")
    print(f"  Top{BEST_PR*100:.0f}% avgPnL: {pnl_mean:+.4f}% [{pnl_lo:+.4f}, {pnl_hi:+.4f}]")

    # ==================================================================
    # EVAL MODEL (70/15/15)
    # ==================================================================
    n = len(X)
    train_end = int(n * 0.70)
    valid_end = int(n * 0.85)

    X_train = X.iloc[:train_end]
    y_train_reg = y_reg[:train_end]
    w_train = sample_weights[:train_end] if sample_weights is not None else None
    X_valid = X.iloc[train_end:valid_end]
    mfe_valid = mfe[train_end:valid_end]
    X_test = X.iloc[valid_end:]
    mfe_test = mfe[valid_end:]

    lgb_train = lgb.Dataset(X_train, y_train_reg, weight=w_train,
                            categorical_feature=categorical_cols)
    model = lgb.train(params, lgb_train, num_boost_round=params['num_iterations'])

    score_valid = model.predict(X_valid)
    score_test = model.predict(X_test)

    # Threshold search around BEST_PR
    print("\n" + "=" * 70)
    print(f"THRESHOLD SEARCH (±5 p.p. around {int(BEST_PR*100)}%)")
    print("=" * 70)

    baseline_prec = (mfe_valid >= 1.5).mean() * 100
    print(f"Baseline precision (mfe>=1.5): {baseline_prec:.2f}%")

    lo_pct = max(0.03, BEST_PR - 0.05)
    hi_pct = min(0.30, BEST_PR + 0.05)

    print(f"\n{'pass%':>7} {'N':>6} {'prec1.5%':>10} {'avgPnL':>10} {'total':>10} {'lift':>6}")
    best_threshold = float(np.median(score_valid))
    best_combined = -np.inf
    for pass_pct in np.arange(lo_pct, hi_pct + 0.001, 0.01):
        k = max(1, int(len(score_valid) * pass_pct))
        top_idx = np.argsort(-score_valid)[:k]
        top_mfe = mfe_valid[top_idx]
        prec15 = (top_mfe >= 1.5).mean() * 100
        pnls = np.array([calc_pnl(m) for m in top_mfe])
        lift = prec15 / baseline_prec if baseline_prec > 0 else 0.0
        print(f"  {pass_pct*100:>5.1f}% {k:>6d} {prec15:>9.1f}% "
              f"{pnls.mean():>+0.4f}% {pnls.sum():>+0.1f}% {lift:>5.2f}")
        combined = pnls.sum() * (prec15 / 100)
        if combined > best_combined and k >= 15:
            best_combined = combined
            best_threshold = float(score_valid[top_idx[-1]])

    print(f"\nBest threshold: {best_threshold:.4f}")
    print(f"  → approximately {int(BEST_PR*100)}% signals pass")

    # Test evaluation at multiple pass-rates
    print("\n--- TEST RESULTS ---")
    print(f"{'pass%':>7} {'N':>6} {'prec1.5%':>10} {'avgPnL':>10} {'total':>10} {'median_mfe':>12}")
    for pass_pct in [0.05, 0.10, 0.15, 0.20, 0.30]:
        k = max(1, int(len(score_test) * pass_pct))
        top_idx = np.argsort(-score_test)[:k]
        top_mfe = mfe_test[top_idx]
        prec15 = (top_mfe >= 1.5).mean() * 100
        pnls = np.array([calc_pnl(m) for m in top_mfe])
        print(f"  {pass_pct*100:>5.0f}% {k:>6d} {prec15:>9.1f}% "
              f"{pnls.mean():>+0.4f}% {pnls.sum():>+0.1f}% {np.median(top_mfe):>11.3f}")

    k_best = max(1, int(len(score_test) * BEST_PR))
    top_best = np.argsort(-score_test)[:k_best]
    mfe_best = mfe_test[top_best]
    pnls_best = np.array([calc_pnl(m) for m in mfe_best])

    print(f"\n--- FINAL: top {BEST_PR*100:.0f}% ---")
    print(f"  N = {k_best}")
    print(f"  prec@1.5%: {(mfe_best>=1.5).mean()*100:.1f}%")
    print(f"  median MFE: {np.median(mfe_best):.3f}%")
    print(f"  avgPnL: {pnls_best.mean():+.4f}%")
    print(f"  total: {pnls_best.sum():+.1f}%")

    # ==================================================================
    # SCATTER PLOT
    # ==================================================================
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    test_results = pd.DataFrame({
        'true_mfe': mfe_test,
        'score': score_test,
        'actual_class': np.select(
            [mfe_test < 0.5, (mfe_test >= 0.5) & (mfe_test < 1.5), mfe_test >= 1.5],
            [0, 1, 2]
        )
    })
    test_results.to_csv('test_predictions.csv', index=False)

    plt.figure(figsize=(10, 6))
    colors = {0: 'red', 1: 'orange', 2: 'green'}
    plt.scatter(test_results['score'], test_results['true_mfe'],
                c=test_results['actual_class'].map(colors), alpha=0.7, edgecolors='k')
    plt.xlabel('Predicted score (regression on log(1+mfe))')
    plt.ylabel('Actual MFE (%)')
    plt.title('Model score vs Actual MFE (Test Set)')
    plt.grid(True, linestyle='--', alpha=0.5)
    plt.axvline(x=best_threshold, color='blue', linestyle='--',
                label=f'Threshold={best_threshold:.2f}')
    plt.legend()
    plt.tight_layout()
    plt.savefig('model_confidence_scatter.png', dpi=150)

    # ==================================================================
    # FINAL TRAINING ON 100%
    # ==================================================================
    print("\n" + "=" * 60)
    print("FINAL TRAINING ON 100% OF DATA")
    print("=" * 60)

    eval_model_path = model_path.replace('.txt', '_eval.txt')
    model.save_model(eval_model_path)

    lgb_full = lgb.Dataset(X, y_reg, weight=sample_weights,
                           categorical_feature=categorical_cols)
    final_model = lgb.train(params, lgb_full, num_boost_round=params['num_iterations'])
    final_model.save_model(model_path)
    print(f"Final model saved: {model_path}")
    print(f"Recommended threshold: {best_threshold:.4f}")

    # ==================================================================
    # SHAP
    # ==================================================================
    try:
        import shap
        print("\n=== SHAP Analysis ===")
        n_shap = min(2000, len(X_test))
        X_shap = X_test.iloc[:n_shap].copy()
        explainer = shap.TreeExplainer(model)
        shap_values = explainer.shap_values(X_shap)
        sv = np.asarray(shap_values)
        if sv.ndim == 3:
            sv = sv[..., 0]
        if sv.ndim != 2:
            raise ValueError(f"Unexpected SHAP shape: {sv.shape}")

        mean_abs = np.abs(sv).mean(axis=0)
        importance = pd.DataFrame({
            'feature': features,
            'mean_abs_shap': mean_abs,
        }).sort_values('mean_abs_shap', ascending=False)

        print("\n--- Top-30 Features by mean(|SHAP|) ---")
        print(importance.head(30).to_string(index=False))
        importance.to_csv('shap_importance.csv', index=False)

    except ImportError:
        print("\n[SHAP] not installed")
    except Exception as e:
        print(f"\n[SHAP] error: {e}")


if __name__ == "__main__":
    main()