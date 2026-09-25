#pragma once
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

class LiveWebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;

    LiveWebSocketClient(const std::string& symbol, const std::string& exchange);
    ~LiveWebSocketClient();

    void set_callback(MessageCallback cb);
    void start();
    void stop();

private:
    void run();
    bool connect_and_read();
    std::string make_stream_url() const;

    std::string symbol_;
    std::string exchange_;
    MessageCallback callback_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
};