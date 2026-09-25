#pragma once
#include <string>
#include <vector>
#include "irest_client.hpp"
#include "market_types.hpp"

class BinanceRestClient : public IRestClient {
public:
    BinanceRestClient();
    std::vector<TickerInfo> fetch_tickers();          // все USDT‑фьючерсы
    std::vector<OHLCV> fetch_ohlcv(const std::string& symbol, 
                                   const std::string& interval = "1m", 
                                   int limit = 6);
    double fetch_last_price(const std::string& symbol);
    LotInfo fetch_lot_size(const std::string& symbol);
    double fetch_tick_size(const std::string& symbol) override;
};