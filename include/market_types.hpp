#pragma once
#include <string>
#include <vector>

struct TickerInfo {
    std::string symbol;
    double quoteVolume;
    int count;
};

struct OHLCV {
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct LotInfo {
    double step_size;
    double tick_size;
    double min_qty;
    double ct_val = 1.0;   // контрактный множитель
};

struct Kline {
    double time;          // Unix timestamp в секундах
    double open;
    double high;
    double low;
    double close;
    double volume;
    double quote_volume;
};