#pragma once
#include <vector>
#include <string>
#include <memory>
#include "market_types.hpp"

struct TickerInfo;
struct OHLCV;
struct LotInfo;

class IRestClient {
public:
    virtual ~IRestClient() = default;
    virtual std::vector<TickerInfo> fetch_tickers() = 0;
    virtual std::vector<OHLCV> fetch_ohlcv(const std::string& symbol,
                                           const std::string& interval = "1m",
                                           int limit = 6) = 0;
    virtual double fetch_last_price(const std::string& symbol) = 0;
    virtual LotInfo fetch_lot_size(const std::string& symbol) = 0;
    virtual double fetch_tick_size(const std::string& symbol) = 0;
};