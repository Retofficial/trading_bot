#pragma once
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <deque>
#include <unordered_map>
#include <condition_variable>
#include <set>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

using OrderUpdateCallback = std::function<void(
    const std::string& instId, const std::string& ordId,
    const std::string& state, double filled_qty, double avg_px,
    const std::string& side, double pnl, double fee)>;

using PositionCallback = std::function<void(
    const std::string& instId, double pos, double avgPx, double markPx)>;

// Перед классом OkxExecutionWsClient
struct OkxOrderParams {
    std::string instId;
    std::string ordType;
    std::string side;
    double sz = 0.0;
    double px = 0.0;
    std::string tdMode = "cross";
    bool reduceOnly = false;
    std::string clOrdId;
};

struct OkxOrderResponse {
    bool success = false;
    int code = 0;
    std::string msg;
    std::string ordId;
    std::string clOrdId;
    std::string raw_json;
};

using OkxOrderCallback = std::function<void(const OkxOrderResponse&)>;

class OkxExecutionWsClient {
public:
    OkxExecutionWsClient(const std::string& api_key,
                         const std::string& api_secret,
                         const std::string& passphrase);
    ~OkxExecutionWsClient();

    void start();
    void stop();
    void subscribe_private_channels();

    void add_order_update_callback(OrderUpdateCallback cb);
    void add_position_callback(PositionCallback cb);

    std::string generate_client_order_id(const std::string& symbol) const;
    std::string get_inst_id_code(const std::string& instId) const;
    bool are_inst_id_codes_loaded() const { return !inst_id_codes_.empty(); }
    void fetch_inst_id_codes();
    void ensure_leverage_set(const std::string& instId);

    int place_order(const OkxOrderParams& params, OkxOrderCallback callback);

private:
    void run();
    bool connect_and_login();
    void send_login();
    void handle_message(const std::string& msg);
    void send_message(const std::string& msg, bool high_priority = false);
    std::string make_signature(const std::string& data) const;
    std::string get_timestamp_sec() const;
    void do_read();
    void start_keepalive_timer();
    void do_write();

    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;

    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    net::io_context ioc_;
    net::strand<net::io_context::executor_type> strand_{net::make_strand(ioc_.get_executor())};
    ssl::context ctx_{ssl::context::tlsv12_client};
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex write_mutex_;
    bool login_sent_ = false;
    bool async_mode_ = false;

    std::unique_ptr<net::steady_timer> keepalive_timer_;
    std::deque<std::string> high_priority_queue_;   // order / cancel-order
    std::deque<std::string> low_priority_queue_;    // subscribe / ping / pong / login
    bool write_in_progress_ = false;

    std::unordered_map<std::string, std::string> inst_id_codes_;
    std::set<std::string> leverage_set_;
    std::mutex leverage_mutex_;

    std::vector<OrderUpdateCallback> order_update_callbacks_;
    std::vector<PositionCallback> position_callbacks_;

    // Для отправки ордеров (вход, рыночное закрытие)
    std::unordered_map<int, OkxOrderCallback> pending_requests_;
    int next_request_id_ = 1;
    std::mutex request_mutex_;
};