#pragma once
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

class OkxLiveWebSocketClient {
public:
    // callback принимает тип стрима ("depth20", "trade", "kline_1m", "forceOrder") и JSON-строку
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;

    OkxLiveWebSocketClient(const std::string& symbol);
    ~OkxLiveWebSocketClient();

    void set_callback(MessageCallback cb);
    void start();
    void stop();

private:
    void run();
    bool connect_and_subscribe();

    std::string symbol_;
    MessageCallback callback_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
};