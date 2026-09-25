#pragma once
#include "market_stream_aggregator.hpp"
#include "screener.hpp"
#include "binance_rest_client.hpp"
#include "okx_rest_client.hpp"
#include "okx_execution_ws_client.hpp"
#include "csv_feature_logger.hpp"
#include "okx_trader_config.hpp"
#include "regime_logger.hpp"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <map>
#include <functional>
#include <mutex>

class SymbolManager {
public:
    using OnFeaturesCallback = std::function<void(const std::string& symbol, const MarketSnapshot& snap, const FeatureVector&)>;

    SymbolManager(const std::string& exchange = "okx",
                  double min_volume = 1'000'000,
                  double min_natr = 1.5,
                  double min_trades = 1000,
                  int top_n = 5,
                  int rescan_interval_sec = 300);

    ~SymbolManager();

    void set_on_features(OnFeaturesCallback cb);
    void start();
    void stop();

private:
    void run();
    void update_symbols();
    std::unique_ptr<MarketStreamAggregator> create_aggregator(const std::string& symbol);

    void start_symbol(const std::string& sym);
    void stop_symbol(const std::string& sym);

    std::string exchange_;
    double min_volume_, min_natr_, min_trades_;
    int top_n_;
    int rescan_interval_sec_;

    std::optional<Screener> screener_;
    std::shared_ptr<OkxRestClient> rest_client_;

    std::map<std::string, std::unique_ptr<MarketStreamAggregator>> aggregators_;
    std::map<std::string, std::unique_ptr<CsvFeatureLogger>> loggers_;
    std::mutex mutex_;

    // Конфиг трейдера (заполняется один раз при первом update_symbols)
    OkxTraderConfig okx_cfg_;
    bool okx_cfg_loaded_ = false;
    std::shared_ptr<OkxExecutionWsClient> okx_exec_ws_;

    RegimeLogger regime_logger_;
    bool regime_logger_started_ = false;

    OnFeaturesCallback on_features_;

    std::atomic<bool> running_{false};
    std::thread thread_;
};