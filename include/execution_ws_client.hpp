#pragma once
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <condition_variable> 
#include <boost/circular_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/ssl.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// Типы команд
enum class OrderCommand {
    PLACE_MARKET,
    PLACE_LIMIT,
    PLACE_STOP_MARKET,
    PLACE_TAKE_PROFIT_MARKET,
    CANCEL
};

// Параметры для размещения ордера
struct OrderParams {
    OrderCommand cmd;
    std::string symbol;
    std::string side;    // BUY / SELL
    double quantity = 0.0;
    double price = 0.0;
    double stop_price = 0.0;
    bool close_position = false;
    bool reduce_only = false;
    std::string time_in_force = "GTC";   // для лимитных
    std::string client_order_id;
};

// Ответ от биржи
struct OrderResponse {
    int id = -1;
    bool success = false;
    int error_code = 0;
    std::string error_msg;
    std::string order_id;
    std::string status;
    double avg_price = 0.0;
    double executed_qty = 0.0;
    std::string raw_json;   // для диагностики
};

// Колбэк, вызываемый при получении ответа (или таймауте)
using OrderCallback = std::function<void(const OrderResponse&)>;

class ExecutionWsClient {
public:
    ExecutionWsClient(const std::string& api_key, const std::string& api_secret);
    ~ExecutionWsClient();

    // Запуск соединения (поток чтения + пинг)
    void start();
    void stop();

    // Асинхронная отправка команды. Возвращает id запроса (для сопоставления).
    int place_order(const OrderParams& params, OrderCallback callback);
    int cancel_order(const std::string& symbol, const std::string& order_id,
                     OrderCallback callback);

    std::string generate_client_order_id(const std::string& symbol) const;

private:
    void run();
    bool connect_and_login();
    void send_login();
    void read_loop();
    void send_message(const std::string& msg);
    void handle_response(const std::string& json);
    void process_pending_requests();
    void on_connection_failure();

    std::string make_signature(const std::string& data) const;
    std::string get_timestamp_ms() const;

    std::string api_key_;
    std::string api_secret_;

    // WebSocket
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    net::io_context ioc_;
    ssl::context ctx_{ssl::context::tlsv12_client};
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Запросы/ответы
    std::mutex mutex_;
    std::unordered_map<int, OrderCallback> pending_requests_;
    int next_request_id_ = 1;

    // Потокобезопасная очередь для колбэков (передача в RealTrader)
    std::queue<std::function<void()>> callback_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread callback_thread_;

    // Параметры
    static constexpr int PING_INTERVAL_SEC = 30;
    static constexpr int REQUEST_TIMEOUT_SEC = 5;
    static constexpr int MAX_RETRIES = 3;

    // Вспомогательные
    void enqueue_callback(std::function<void()> cb);
};