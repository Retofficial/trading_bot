#include "websocket_client_live.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

LiveWebSocketClient::LiveWebSocketClient(const std::string& symbol, const std::string& exchange)
    : symbol_(symbol), exchange_(exchange) {}

LiveWebSocketClient::~LiveWebSocketClient() { stop(); }

void LiveWebSocketClient::set_callback(MessageCallback cb) {
    callback_ = std::move(cb);
}

void LiveWebSocketClient::start() {
    running_ = true;
    thread_ = std::make_unique<std::thread>(&LiveWebSocketClient::run, this);
}

void LiveWebSocketClient::stop() {
    running_ = false;
    if (thread_) {
        try {
            if (thread_->joinable()) thread_->join();
        } catch (...) {
            std::cerr << "WS thread join failed" << std::endl;
        }
    }
}

std::string LiveWebSocketClient::make_stream_url() const {
    std::string base = symbol_;
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    if (exchange_ == "binance") {
        // Комбинированный URL для Binance
        return "/stream?streams=" + base + "@depth20@100ms/" +
               base + "@trade/" +
               base + "@kline_1m/" +
               base + "@forceOrder";
    } else if (exchange_ == "okx") {
        return "";
    }
    return "";
}

void LiveWebSocketClient::run() {
    while (running_) {
        if (!connect_and_read()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

bool LiveWebSocketClient::connect_and_read() {
    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);

        std::string host = (exchange_ == "binance") ? "fstream.binance.com" : "ws.okx.com";
        std::string port = "443";

        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve(host, port);
        auto ep = net::connect(beast::get_lowest_layer(ws), results);

        ws.next_layer().handshake(ssl::stream_base::client);

        std::string target = make_stream_url();
        if (target.empty()) return false;
        ws.handshake(host, target);

        std::cout << "[LiveWS] Connected, reading..." << std::endl;

        beast::flat_buffer buffer;
        while (running_) {
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());
            buffer.clear();

            // Определяем тип сообщения
            std::string type;
            if (msg.find("\"e\":\"depthUpdate\"") != std::string::npos) type = "depth20";
            else if (msg.find("\"e\":\"aggTrade\"") != std::string::npos) type = "trade";
            else if (msg.find("\"e\":\"trade\"") != std::string::npos) type = "trade";
            else if (msg.find("\"e\":\"kline\"") != std::string::npos) type = "kline_1m";
            else if (msg.find("\"e\":\"forceOrder\"") != std::string::npos) type = "forceOrder";
            else continue;

            if (callback_) {
                try {
                    callback_(type, msg);
                } catch (const std::exception& e) {
                    std::cerr << "[LiveWS] callback error: " << e.what() << std::endl;
                }
            }
        }
        return true;
    } catch (std::exception const& e) {
        std::cerr << "[LiveWS] Error: " << e.what() << std::endl;
        return false;
    }
}