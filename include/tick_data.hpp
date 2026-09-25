#pragma once
#include <cstdint>
#include <array>

struct TickData {
    uint64_t timestamp_us;
    double price;
    double best_bid;
    double best_ask;
    std::array<double, 123> features;
};