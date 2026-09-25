#include "execution_ws_client.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <simdjson.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <boost/beast/core/buffers_to_string.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// -------------------- Вспомогательные функции --------------------
static std::string HmacSha256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), result, &len);
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) os << std::setw(2) << static_cast<int>(result[i]);
    return os.str();
}

static std::string GetTimestampMs() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(ms);
}

// -------------------- Конструктор / деструктор --------------------
ExecutionWsClient::ExecutionWsClient(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {}

ExecutionWsClient::~ExecutionWsClient() { stop(); }

// -------------------- Запуск / остановка --------------------
void ExecutionWsClient::start() {
    if (running_) return;
    running_ = true;
    // Запускаем поток обработки колбэков (чтобы не блокировать сетевой поток)
    callback_thread_ = std::thread([this]() {
        while (running_) {
            std::function<void()> cb;
            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() { return !callback_queue_.empty() || !running_; });
                if (!running_) break;
                cb = std::move(callback_queue_.front());
                callback_queue_.pop();
            }
            cb();
        }
    });
    // Запускаем сетевой поток
    io_thread_ = std::thread(&ExecutionWsClient::run, this);
}

void ExecutionWsClient::stop() {
    running_ = false;
    queue_cv_.notify_all();
    if (callback_thread_.joinable()) callback_thread_.join();
    if (ws_) {
        try { ws_->close(websocket::close_code::normal); } catch (...) {}
    }
    ioc_.stop();
    if (io_thread_.joinable()) io_thread_.join();
}

// -------------------- Основной цикл переподключения --------------------
void ExecutionWsClient::run() {
    while (running_) {
        if (connect_and_login()) {
            read_loop();
        }
        if (running_) {
            std::cerr << "[ExecWS] Disconnected, reconnecting in 2s...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// -------------------- Подключение и логин --------------------
bool ExecutionWsClient::connect_and_login() {
    try {
        ioc_.restart();
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);
        tcp::resolver resolver(ioc_);
        auto const results = resolver.resolve("ws-fapi.binance.com", "443");
        auto ep = net::connect(beast::get_lowest_layer(*ws_), results);
        ws_->next_layer().handshake(ssl::stream_base::client);
        ws_->handshake("ws-fapi.binance.com", "/ws-fapi/v1/private");
        // Читаем ответ на логин
        beast::flat_buffer buffer;
        ws_->read(buffer);
        std::string resp = beast::buffers_to_string(buffer.data());
        buffer.clear();

        // Парсим JSON
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(resp);
        bool logged_in = false;
        if (!doc.error()) {
            auto obj = doc.get_object();
            std::string_view result;
            if (!obj["result"].get(result) && result == "SUCCESS") {
                logged_in = true;
            }
        }

        if (logged_in) {
            std::cerr << "[ExecWS] Logged in successfully\n";
            connected_ = true;
            return true;
        } else {
            std::cerr << "[ExecWS] Login failed: " << resp << std::endl;
            return false;
        }
    } catch (std::exception const& e) {
        std::cerr << "[ExecWS] Connection error: " << e.what() << std::endl;
        return false;
    }
}

void ExecutionWsClient::send_login() {
    std::string timestamp = GetTimestampMs();
    std::string signature = HmacSha256(api_secret_, "timestamp=" + timestamp);
    std::ostringstream login_msg;
    login_msg << R"({"method":"REQUEST","params":[{"apiKey":")" << api_key_
              << R"(","signature":")" << signature
              << R"(","timestamp":)" << timestamp << "}],\"id\":999}";
    send_message(login_msg.str());
}

// -------------------- Отправка команд --------------------
int ExecutionWsClient::place_order(const OrderParams& params, OrderCallback callback) {
    std::lock_guard lock(mutex_);
    int id = next_request_id_++;
    pending_requests_[id] = std::move(callback);

    std::ostringstream msg;
    std::string type;
    if (params.cmd == OrderCommand::PLACE_MARKET) type = "MARKET";
    else if (params.cmd == OrderCommand::PLACE_LIMIT) type = "LIMIT";
    else if (params.cmd == OrderCommand::PLACE_STOP_MARKET) type = "STOP_MARKET";
    else if (params.cmd == OrderCommand::PLACE_TAKE_PROFIT_MARKET) type = "TAKE_PROFIT_MARKET";

    msg << R"({"method":"order.place","params":{)"
        << R"("symbol":")" << params.symbol
        << R"(","side":")" << params.side
        << R"(","type":")" << type
        << R"(","quantity":)" << params.quantity;
    if (params.price > 0.0) msg << R"(,"price":")" << std::fixed << std::setprecision(8) << params.price << R"(")";
    if (params.stop_price > 0.0) msg << R"(,"stopPrice":")" << std::fixed << std::setprecision(8) << params.stop_price << R"(")";
    if (params.close_position) msg << R"(,"closePosition":true)";
    if (params.reduce_only) msg << R"(,"reduceOnly":true)";
    if (!params.time_in_force.empty()) msg << R"(,"timeInForce":")" << params.time_in_force << R"(")";
    msg << R"(,"newClientOrderId":")" << params.client_order_id << R"(")";
    msg << R"(},"id":)" << id << "}";

    send_message(msg.str());

    // Запускаем таймер (в этом примере таймаут обрабатывается упрощённо)
    // В реальном коде нужно запланировать проверку таймаута и повторную отправку.
    return id;
}

int ExecutionWsClient::cancel_order(const std::string& symbol, const std::string& order_id,
                                    OrderCallback callback) {
    std::lock_guard lock(mutex_);
    int id = next_request_id_++;
    pending_requests_[id] = std::move(callback);

    std::ostringstream msg;
    msg << R"({"method":"order.cancel","params":{)"
        << R"("symbol":")" << symbol
        << R"(","orderId":")" << order_id
        << R"("},"id":)" << id << "}";
    send_message(msg.str());
    return id;
}

// -------------------- Чтение ответов --------------------
void ExecutionWsClient::read_loop() {
    beast::flat_buffer buffer;
    while (running_ && connected_) {
        try {
            ws_->read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());
            buffer.clear();
            handle_response(msg);
        } catch (std::exception const& e) {
            std::cerr << "[ExecWS] Read error: " << e.what() << std::endl;
            connected_ = false;
            break;
        }
    }
}

void ExecutionWsClient::handle_response(const std::string& json) {
    // Парсим id и результат (упрощённо, без полноценного simdjson)
    // В реальном коде нужно использовать simdjson для надёжности.
    // Здесь – демонстрация концепции.
    auto pos_id = json.find("\"id\":");
    if (pos_id == std::string::npos) return;
    int id = std::stoi(json.substr(pos_id + 5));

    OrderCallback cb;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_requests_.find(id);
        if (it == pending_requests_.end()) return;
        cb = std::move(it->second);
        pending_requests_.erase(it);
    }

    OrderResponse resp;
    resp.id = id;
    // Заполняем поля ответа (в реальном коде – из JSON)
    resp.success = (json.find("\"code\":0") != std::string::npos);
    resp.raw_json = json;

    // Передаём колбэк в очередь обработки
    enqueue_callback([cb = std::move(cb), resp = std::move(resp)]() {
        cb(resp);
    });
}

// -------------------- Вспомогательные методы --------------------
void ExecutionWsClient::send_message(const std::string& msg) {
    if (ws_ && ws_->is_open()) {
        ws_->write(net::buffer(msg));
    }
}

std::string ExecutionWsClient::generate_client_order_id(const std::string& symbol) const {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return "silence_" + symbol + "_" + std::to_string(us);
}

void ExecutionWsClient::enqueue_callback(std::function<void()> cb) {
    {
        std::lock_guard lock(queue_mutex_);
        callback_queue_.push(std::move(cb));
    }
    queue_cv_.notify_one();
}