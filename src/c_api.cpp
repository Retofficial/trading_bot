#include "c_api.h"
#include "market_stream_aggregator.hpp"
#include "market_snapshot.hpp"
#include "feature_vector.hpp"
#include <cstring>
#include <stdexcept>

// Преобразование внутреннего MarketSnapshot в плоскую C-структуру
static void copy_snapshot_to_c(const MarketSnapshot& src, CMarketSnapshot* dst) {
    dst->price = src.price;
    dst->spread_pct = src.spread_pct;
    dst->best_bid = src.best_bid;
    dst->best_ask = src.best_ask;
    dst->bid_vol_top10 = src.bid_vol_top10;
    dst->ask_vol_top10 = src.ask_vol_top10;

    // Стаканы
    dst->bids_count = std::min((size_t)MAX_BIDS_ASKS, src.bids_snapshot.size());
    for (int i = 0; i < dst->bids_count; ++i) {
        dst->bids_prices[i] = src.bids_snapshot[i].first;
        dst->bids_volumes[i] = src.bids_snapshot[i].second;
    }
    dst->asks_count = std::min((size_t)MAX_BIDS_ASKS, src.asks_snapshot.size());
    for (int i = 0; i < dst->asks_count; ++i) {
        dst->asks_prices[i] = src.asks_snapshot[i].first;
        dst->asks_volumes[i] = src.asks_snapshot[i].second;
    }

    dst->imbalance = src.imbalance;
    dst->large_bid_usdt = src.large_bid_usdt;
    dst->large_ask_usdt = src.large_ask_usdt;
    dst->bid_wall_usdt = src.bid_wall_usdt;
    dst->ask_wall_usdt = src.ask_wall_usdt;
    dst->open_interest = src.open_interest;
    dst->oi_change_pct = src.oi_change_pct;
    dst->oi_level = src.oi_level;
    dst->taker_buy_vol = src.taker_buy_vol;
    dst->taker_sell_vol = src.taker_sell_vol;
    dst->taker_total_vol = src.taker_total_vol;
    dst->buy_pressure = src.buy_pressure;

    // Трейды
    dst->trades_count = std::min((size_t)MAX_RECENT_TRADES, src.recent_trades.size());
    for (int i = 0; i < dst->trades_count; ++i) {
        dst->trade_price[i] = src.recent_trades[i].price;
        dst->trade_volume[i] = src.recent_trades[i].volume;
        dst->trade_usd_volume[i] = src.recent_trades[i].usd_volume;
        dst->trade_is_maker[i] = src.recent_trades[i].is_maker ? 1 : 0;
        dst->trade_side[i] = src.recent_trades[i].side;
        dst->trade_time[i] = src.recent_trades[i].time;
    }

    // Ликвидации
    dst->liq_count = std::min((size_t)MAX_LIQ_LEVELS, src.liquidation_levels.size());
    for (int i = 0; i < dst->liq_count; ++i) {
        dst->liq_price[i] = src.liquidation_levels[i].price;
        dst->liq_qty[i] = src.liquidation_levels[i].qty;
        dst->liq_side[i] = src.liquidation_levels[i].side;
        dst->liq_time[i] = src.liquidation_levels[i].time;
    }

    dst->timestamp = src.timestamp;
    dst->trend = src.trend;
    dst->ema_value = src.ema_value;
}

AggregatorHandle CreateAggregator(const char* config_json) {
    try {
        auto agg = MarketStreamAggregator::create(config_json);
        return static_cast<AggregatorHandle>(agg.release());
    } catch (...) {
        return nullptr;
    }
}

void DestroyAggregator(AggregatorHandle handle) {
    delete static_cast<MarketStreamAggregator*>(handle);
}

int GetSnapshot(AggregatorHandle handle, CMarketSnapshot* snapshot) {
    if (!handle || !snapshot) return -1;
    try {
        auto* agg = static_cast<MarketStreamAggregator*>(handle);
        MarketSnapshot internal = agg->get_snapshot();
        copy_snapshot_to_c(internal, snapshot);
        return 0;
    } catch (...) {
        return -1;
    }
}

int ComputeFeatures(AggregatorHandle handle, CFeatureVector* features) {
    if (!handle || !features) return -1;
    try {
        auto* agg = static_cast<MarketStreamAggregator*>(handle);
        FeatureVector vec = agg->compute_features();
        std::copy(vec.features.begin(), vec.features.end(), features->features);
        return 0;
    } catch (...) {
        return -1;
    }
}

double GetCurrentMSI(AggregatorHandle handle) {
    if (!handle) return 0.0;
    auto* agg = static_cast<MarketStreamAggregator*>(handle);
    return agg->get_current_msi();
}

double GetPeakMSI(AggregatorHandle handle) {
    if (!handle) return 0.0;
    auto* agg = static_cast<MarketStreamAggregator*>(handle);
    return agg->get_peak_msi();
}

int PushSignal(AggregatorHandle handle, const CSignal* signal) {
    if (!handle || !signal) return -1;
    try {
        Signal sig;
        sig.side = (signal->side == 0) ? Signal::Side::LONG : Signal::Side::SHORT;
        sig.stop = signal->stop;
        sig.take = signal->take;
        sig.take_prices = {signal->take_prices[0], signal->take_prices[1], signal->take_prices[2]};
        sig.spike = signal->spike;
        sig.reason = signal->reason ? signal->reason : "";
        sig.x_risk = signal->x_risk;
        std::copy(signal->rl_state.features, signal->rl_state.features + 123, sig.rl_state.features.begin());
        auto* agg = static_cast<MarketStreamAggregator*>(handle);
        agg->push_signal(sig);
        return 0;
    } catch (...) {
        return -1;
    }
}

int StartAggregator(AggregatorHandle handle) {
    if (!handle) return -1;
    try {
        auto* agg = static_cast<MarketStreamAggregator*>(handle);
        agg->start();
        return 0;
    } catch (...) {
        return -1;
    }
}

int StopAggregator(AggregatorHandle handle) {
    if (!handle) return -1;
    try {
        auto* agg = static_cast<MarketStreamAggregator*>(handle);
        agg->stop();
        return 0;
    } catch (...) {
        return -1;
    }
}