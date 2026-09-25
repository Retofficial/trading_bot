// ==================== include/c_api.h ====================
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ----- Фиксированные размеры для C-совместимых структур -----
#define MAX_BIDS_ASKS 20
#define MAX_RECENT_TRADES 5000
#define MAX_LIQ_LEVELS 100

// Плоская структура MarketSnapshot для передачи через ctypes
typedef struct {
    double price;
    double spread_pct;
    double best_bid;
    double best_ask;
    double bid_vol_top10;
    double ask_vol_top10;
    // Массивы стаканов: [цена, объём]
    double bids_prices[MAX_BIDS_ASKS];
    double bids_volumes[MAX_BIDS_ASKS];
    int bids_count;
    double asks_prices[MAX_BIDS_ASKS];
    double asks_volumes[MAX_BIDS_ASKS];
    int asks_count;
    double imbalance;
    double large_bid_usdt;
    double large_ask_usdt;
    double bid_wall_usdt;
    double ask_wall_usdt;
    double open_interest;
    double oi_change_pct;
    int oi_level;
    double taker_buy_vol;
    double taker_sell_vol;
    double taker_total_vol;
    double buy_pressure;
    // Трейды (последние 5000)
    double trade_price[MAX_RECENT_TRADES];
    double trade_volume[MAX_RECENT_TRADES];
    double trade_usd_volume[MAX_RECENT_TRADES];
    int trade_is_maker[MAX_RECENT_TRADES];
    int trade_side[MAX_RECENT_TRADES];
    double trade_time[MAX_RECENT_TRADES];
    int trades_count;
    // Ликвидации (последние 100)
    double liq_price[MAX_LIQ_LEVELS];
    double liq_qty[MAX_LIQ_LEVELS];
    int liq_side[MAX_LIQ_LEVELS];
    double liq_time[MAX_LIQ_LEVELS];
    int liq_count;
    double timestamp;
    int trend;
    double ema_value;
} CMarketSnapshot;

// Вектор признаков (123 double)
typedef struct {
    double features[123];
} CFeatureVector;

// Сигнал для отправки
typedef struct {
    int side;        // 0=long, 1=short
    double stop;
    double take;
    double take_prices[3];
    double spike;
    const char* reason; // временная строка, копируется внутри
    int x_risk;
    CFeatureVector rl_state;
} CSignal;

// Непрозрачный хендл агрегатора
typedef void* AggregatorHandle;

// ----- API функции -----
// Создать/удалить агрегатор по JSON-конфигу
AggregatorHandle CreateAggregator(const char* config_json);
void DestroyAggregator(AggregatorHandle handle);

// Получить копию снимка
int GetSnapshot(AggregatorHandle handle, CMarketSnapshot* snapshot);

// Получить последний вектор признаков
int ComputeFeatures(AggregatorHandle handle, CFeatureVector* features);

// Текущее и пиковое MSI
double GetCurrentMSI(AggregatorHandle handle);
double GetPeakMSI(AggregatorHandle handle);

// Передать сигнал в разделяемую память
int PushSignal(AggregatorHandle handle, const CSignal* signal);

// Запуск/останов
int StartAggregator(AggregatorHandle handle);
int StopAggregator(AggregatorHandle handle);

#ifdef __cplusplus
}
#endif