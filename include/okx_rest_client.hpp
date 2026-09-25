#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "irest_client.hpp"
#include "market_types.hpp"
#include "json.hpp"

using json = nlohmann::json;

struct OkxRestResponse {
    bool success = false;
    int code = 0;
    std::string msg;
    json data;
};

class OkxRestClient : public IRestClient {
public:
    OkxRestClient();
    ~OkxRestClient();

    // Публичные методы для рыночных данных (существующие)
    std::vector<TickerInfo> fetch_tickers();
    std::vector<OHLCV> fetch_ohlcv(const std::string& symbol,
                                   const std::string& interval = "1m",
                                   int limit = 6);
    double fetch_last_price(const std::string& symbol);
    LotInfo fetch_lot_size(const std::string& symbol);
    double fetch_tick_size(const std::string& symbol) override;

    std::vector<Kline> fetch_klines(const std::string& symbol,
                                    const std::string& bar = "1m",
                                    int limit = 300);

    // Новые методы для приватных запросов
    void set_credentials(const std::string& api_key,
                         const std::string& api_secret,
                         const std::string& passphrase);

    void place_order(const json& body,
                     std::function<void(const OkxRestResponse&)> cb);

    void place_algo_order(const json& body,
                          std::function<void(const OkxRestResponse&)> cb);

    void get_algo_order_by_clord(const std::string& algoClOrdId,
                                 const std::string& instId,
                                 std::function<void(const OkxRestResponse&)> cb);

    void cancel_algo_order(const std::string& algoId,
                           const std::string& instId,
                           std::function<void(const OkxRestResponse&)> cb);

    void place_batch_orders(const json& body,
                            std::function<void(const OkxRestResponse&)> cb);

private:
    struct Request {
        std::string method;   // "GET" или "POST"
        std::string path;     // путь с query параметрами для GET
        json body;            // тело для POST (для GET пустое)
        std::function<void(const OkxRestResponse&)> cb;
    };

    void process_requests();
    OkxRestResponse send_signed_request(const std::string& method,
                                        const std::string& path,
                                        const json& body);
    std::string make_signature(const std::string& data) const;
    std::string get_timestamp_iso() const;
    void enqueue_callback(std::function<void()> cb);

    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;

    std::thread request_thread_;
    std::atomic<bool> request_thread_running_{false};
    std::queue<Request> request_queue_;
    std::mutex request_mutex_;
    std::condition_variable request_cv_;

    std::thread callback_thread_;
    std::atomic<bool> callback_thread_running_{false};
    std::queue<std::function<void()>> callback_queue_;
    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;

    void* curl_handle_ = nullptr;

    // Существующие поля
    std::unordered_map<std::string, double> ct_val_cache_;
    void ensure_ct_vals_loaded();
    double get_ct_val(const std::string& symbol);
};