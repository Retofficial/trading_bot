#include "okx_live_websocket_client.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

OkxLiveWebSocketClient::OkxLiveWebSocketClient(const std::string& symbol)
    : symbol_(symbol) {}

OkxLiveWebSocketClient::~OkxLiveWebSocketClient() { stop(); }

void OkxLiveWebSocketClient::set_callback(MessageCallback cb) {
    callback_ = std::move(cb);
}

void OkxLiveWebSocketClient::start() {
    running_ = true;
    thread_ = std::make_unique<std::thread>(&OkxLiveWebSocketClient::run, this);
}

void OkxLiveWebSocketClient::stop() {
    running_ = false;
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void OkxLiveWebSocketClient::run() {
    while (running_) {
        if (!connect_and_subscribe()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

bool OkxLiveWebSocketClient::connect_and_subscribe() {
    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_verify_mode(ssl::verify_none);
        ctx.set_options(ssl::context::default_workarounds |
                        ssl::context::no_sslv2 |
                        ssl::context::no_sslv3);
        websocket::stream<beast::ssl_stream<tcp::socket>> ws(ioc, ctx);

        tcp::resolver resolver(ioc);
        std::string host = "ws.okx.com";
        std::string port = "8443";
        auto const results = resolver.resolve(host, port);
        auto ep = net::connect(beast::get_lowest_layer(ws), results);

        // SNI – обязательно для OKX
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            throw beast::system_error(beast::error_code(
                static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()));
        }

        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake(host, "/ws/v5/public");

        // Подписка на каналы
        std::ostringstream sub;
        sub << R"({"op":"subscribe","args":[)"
            << R"({"channel":"books","instId":")" << symbol_ << R"("},)"
            << R"({"channel":"bbo-tbt","instId":")" << symbol_ << R"("},)"
            << R"({"channel":"trades","instId":")" << symbol_ << R"("},)"
            << R"({"channel":"candle1m","instId":")" << symbol_ << R"("},)"
            << R"({"channel":"force-orders","instId":")" << symbol_ << R"("})"
            << R"(]})";
        ws.write(net::buffer(sub.str()));

        std::cerr << "[OkxLiveWS] Connected and subscribed\n";

        beast::flat_buffer buffer;
        while (running_) {
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());
            buffer.clear();

            std::string type;
            if (msg.find("\"channel\":\"bbo-tbt\"") != std::string::npos) type = "bbo";
            else if (msg.find("\"channel\":\"books\"") != std::string::npos) type = "depth20";
            else if (msg.find("\"channel\":\"trades\"") != std::string::npos) type = "trade";
            else if (msg.find("\"channel\":\"candle1m\"") != std::string::npos) type = "kline_1m";
            else if (msg.find("\"channel\":\"force-orders\"") != std::string::npos) type = "forceOrder";
            else continue;

            if (callback_) callback_(type, msg);
        }
        return true;
    } catch (std::exception const& e) {
        std::cerr << "[OkxLiveWS] Error: " << e.what() << std::endl;
        return false;
    }
}