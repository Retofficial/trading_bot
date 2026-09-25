#pragma once
#include "irest_client.hpp"
#include <vector>
#include <optional>

struct SymbolMetrics {
    std::string symbol;
    double volume_usdt;
    double natr_pct;
    double trades_5min_est;
    double price;
};

class Screener {
public:
    Screener(double min_volume, double min_natr, double min_trades, std::shared_ptr<IRestClient> client);
    std::optional<SymbolMetrics> check_symbol(const TickerInfo& ticker);  // делаем public
    std::vector<SymbolMetrics> get_top_symbols(const std::vector<TickerInfo>& tickers, int top_n = 5);
private:
    double min_volume_, min_natr_, min_trades_;
    std::shared_ptr<IRestClient> rest_client_;
    double calc_natr(const std::vector<OHLCV>& ohlcv);
};