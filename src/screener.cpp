#include "screener.hpp"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <sstream>

static std::string format_with_separator(double value, int decimals) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    std::string s = oss.str();
    // Вставляем '.' каждые три символа перед запятой, если есть дробная часть
    size_t dot_pos = s.find('.');
    if (dot_pos == std::string::npos) dot_pos = s.size();
    for (int i = static_cast<int>(dot_pos) - 3; i > 0; i -= 3) {
        s.insert(i, ".");
    }
    return s;
}

Screener::Screener(double min_volume, double min_natr, double min_trades,
                   std::shared_ptr<IRestClient> client)
    : min_volume_(min_volume), min_natr_(min_natr), min_trades_(min_trades),
      rest_client_(std::move(client)) {}

double Screener::calc_natr(const std::vector<OHLCV>& ohlcv) {
    if (ohlcv.size() < 6) return 0.0;
    double prev_close = ohlcv[0].close;
    double sum_tr = 0.0, sum_price = 0.0;
    for (int i = 1; i <= 5; ++i) {
        const auto& c = ohlcv[i];
        double tr = std::max({c.high - c.low, std::abs(c.high - prev_close), std::abs(c.low - prev_close)});
        sum_tr += tr;
        sum_price += c.close;
        prev_close = c.close;
    }
    double avg_tr = sum_tr / 5.0;
    double avg_price = sum_price / 5.0;
    return avg_price > 0 ? (avg_tr / avg_price) * 100.0 : 0.0;
}

std::optional<SymbolMetrics> Screener::check_symbol(const TickerInfo& ticker) {
    auto ohlcv = rest_client_->fetch_ohlcv(ticker.symbol, "1m", 6);
    if (ohlcv.size() < 6) return std::nullopt;

    double volume_usdt = 0.0;
    for (int i = 1; i <= 5; ++i) volume_usdt += ohlcv[i].volume;   // уже в USDT
    double natr = calc_natr(ohlcv);
    double trades_5min_est = ticker.count / (24.0 * 12.0);  // 24h * 12 5‑мин интервалов

    std::cerr << "  [" << ticker.symbol << "] Vol5m=" << format_with_separator(volume_usdt, 0)
              << " NATR=" << std::fixed << std::setprecision(2) << natr
              << " Trades5m=" << format_with_separator(trades_5min_est, 1);
    if (volume_usdt >= min_volume_ && natr >= min_natr_ && trades_5min_est >= min_trades_) {
        std::cerr << " PASSED" << std::endl;
    } else {
        std::cerr << " FAILED" << std::endl;
    }

    if (volume_usdt >= min_volume_ && natr >= min_natr_ && trades_5min_est >= min_trades_) {
        return SymbolMetrics{ticker.symbol, volume_usdt, natr, trades_5min_est, ohlcv[5].close};
    }
    return std::nullopt;
}

std::vector<SymbolMetrics> Screener::get_top_symbols(const std::vector<TickerInfo>& tickers, int top_n) {
    std::vector<SymbolMetrics> results;
    std::vector<TickerInfo> sorted = tickers;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.quoteVolume > b.quoteVolume;
    });
    int limit = std::min(50, (int)sorted.size());
    std::cerr << "[Screener] checking " << limit << " candidates" << std::endl;    
    for (int i = 0; i < limit; ++i) {
        std::cerr << "[Screener] checking " << i+1 << "/" << limit << ": " << sorted[i].symbol << std::endl;
        auto metrics = check_symbol(sorted[i]);
        if (metrics) results.push_back(*metrics);
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.volume_usdt > b.volume_usdt;
    });
    if ((int)results.size() > top_n) results.resize(top_n);
    return results;
}