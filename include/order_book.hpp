#pragma once
#include <map>
#include <cstdint>

struct OrderBook {
    std::map<int64_t, double> bids;   // price_tick -> volume
    std::map<int64_t, double> asks;
    double tick_size = 0.0;
};