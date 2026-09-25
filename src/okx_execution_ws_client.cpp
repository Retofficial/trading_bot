#include "okx_execution_ws_client.hpp"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <simdjson.h>
#include "json.hpp"
#include <boost/beast/http.hpp>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// ---------- Вспомогательные функции ----------
static std::string HmacSha256Base64(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), result, &len);

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, result, len);
    BIO_flush(b64);
    BUF_MEM* buf_mem;
    BIO_get_mem_ptr(b64, &buf_mem);
    std::string base64_str(buf_mem->data, buf_mem->length);
    BIO_free_all(b64);
    return base64_str;
}

static std::string GetTimestampSec() {
    auto now = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return std::to_string(sec);
}

static std::string buffer_to_string(const beast::flat_buffer& buf) {
    auto data = buf.cdata();
    return {static_cast<const char*>(data.data()), data.size()};
}

static std::string get_timestamp_iso() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::time_t sec = std::chrono::system_clock::to_time_t(now);
    std::ostringstream iso;
    iso << std::put_time(std::gmtime(&sec), "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << (ms % 1000) << 'Z';
    return iso.str();
}

// ---------- Конструктор / деструктор ----------
OkxExecutionWsClient::OkxExecutionWsClient(const std::string& api_key,
                                           const std::string& api_secret,
                                           const std::string& passphrase)
    : api_key_(api_key), api_secret_(api_secret), passphrase_(passphrase) {}

OkxExecutionWsClient::~OkxExecutionWsClient() { stop(); }

// ---------- Запуск / останов ----------
void OkxExecutionWsClient::start() {
    if (running_) return;
    running_ = true;
    io_thread_ = std::thread(&OkxExecutionWsClient::run, this);
}

void OkxExecutionWsClient::stop() {
    running_ = false;
    if (keepalive_timer_) {
        keepalive_timer_->cancel();
    }
    if (ws_) {
        try { ws_->close(websocket::close_code::normal); } catch (...) {}
    }
    ioc_.stop();
    if (io_thread_.joinable()) io_thread_.join();

    // Вызываем все ожидающие колбэки с ошибкой, чтобы не было утечек
    {
        std::lock_guard lock(request_mutex_);
        for (auto& [id, cb] : pending_requests_) {
            OkxOrderResponse resp;
            resp.success = false;
            resp.code = -1;
            resp.msg = "Client stopped";
            cb(resp);
        }
        pending_requests_.clear();
    }
}

// ---------- Основной цикл ----------
void OkxExecutionWsClient::run() {
    while (running_) {
        if (!connect_and_login()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        subscribe_private_channels();
        async_mode_ = true;

        net::post(strand_, [this] {
            do_read();
            start_keepalive_timer();
        });

        ioc_.run();

        connected_ = false;
        login_sent_ = false;
        async_mode_ = false;
    }
}

// ---------- Подключение и вход ----------
bool OkxExecutionWsClient::connect_and_login() {
    try {
        ioc_.restart();
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);
        tcp::resolver resolver(ioc_);
        std::string host = "ws.okx.com";
        std::string port = "8443";
        auto const results = resolver.resolve(host, port);
        auto ep = net::connect(beast::get_lowest_layer(*ws_), results);
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str()))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                         net::error::get_ssl_category()));
        ws_->next_layer().handshake(ssl::stream_base::client);
        ws_->handshake(host, "/ws/v5/private");

        send_login();

        beast::flat_buffer buf;
        ws_->read(buf);
        std::string resp = buffer_to_string(buf);
        buf.clear();
        std::cerr << "[OkxExecWS] Login response: " << resp << std::endl;
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(resp);
        bool ok = false;
        if (!doc.error()) {
            std::string_view event, code;
            if (!doc["event"].get(event) && event == "login" &&
                !doc["code"].get(code) && code == "0") {
                ok = true;
            } else {
                std::string_view msg;
                doc["msg"].get(msg);
                std::cerr << "[OkxExecWS] Login error: code=" << code << " msg=" << msg << std::endl;
            }
        }
        if (ok) {
            std::cerr << "[OkxExecWS] Logged in successfully\n";
            connected_ = true;
            fetch_inst_id_codes();
            return true;
        } else {
            std::cerr << "[OkxExecWS] Login failed: " << resp << std::endl;
            return false;
        }
    } catch (std::exception const& e) {
        std::cerr << "[OkxExecWS] Connection error: " << e.what() << std::endl;
        return false;
    }
}

void OkxExecutionWsClient::send_login() {
    std::string timestamp = get_timestamp_sec();
    std::string sign = make_signature(timestamp + "GET" + "/users/self/verify");

    json msg = {
        {"op", "login"},
        {"args", {{
            {"apiKey", api_key_},
            {"passphrase", passphrase_},
            {"timestamp", timestamp},
            {"sign", sign}
        }}}
    };
    send_message(msg.dump());
}

// ---------- Подписка ----------
void OkxExecutionWsClient::subscribe_private_channels() {
    // Обычные ордера
    json order_sub = {
        {"op", "subscribe"},
        {"args", {{
            {"channel", "orders"},
            {"instType", "SWAP"}
        }}}
    };
    send_message(order_sub.dump());

    // Позиции
    json pos_sub = {
        {"op", "subscribe"},
        {"args", {{
            {"channel", "positions"},
            {"instType", "SWAP"}
        }}}
    };
    send_message(pos_sub.dump());

    std::cerr << "[OkxExecWS] Subscribed to orders and positions\n";
}

// ---------- Асинхронное чтение ----------
void OkxExecutionWsClient::do_read() {
    auto buffer = std::make_shared<beast::flat_buffer>();
    ws_->async_read(*buffer,
        net::bind_executor(strand_,
            [this, buffer](beast::error_code ec, std::size_t) {
                if (ec == websocket::error::closed) {
                    std::cerr << "[OkxExecWS] Server closed\n";
                    connected_ = false;
                    ioc_.stop();
                    return;
                }
                if (ec) {
                    std::cerr << "[OkxExecWS] Read error: " << ec.message() << std::endl;
                    connected_ = false;
                    ioc_.stop();
                    return;
                }
                std::string msg = buffer_to_string(*buffer);
                handle_message(msg);
                do_read();
            }));
}

// ---------- Keep-alive (WebSocket ping) ----------
void OkxExecutionWsClient::start_keepalive_timer() {
    keepalive_timer_ = std::make_unique<net::steady_timer>(ioc_);
    keepalive_timer_->expires_after(std::chrono::seconds(25));
    keepalive_timer_->async_wait(
        net::bind_executor(strand_,
            [this](const boost::system::error_code& ec) {
                if (ec || !running_ || !connected_) return;
                // Шлём пинг только если очередь пуста — иначе ждём, чтобы не мешать ордерам
                if (!write_in_progress_ &&
                    high_priority_queue_.empty() &&
                    low_priority_queue_.empty()) {
                    ws_->async_ping({},
                        net::bind_executor(strand_,
                            [](beast::error_code ec) {
                                (void)ec;
                            }));
                }
                start_keepalive_timer();
            }));
}

// ---------- Очередь записи ----------
void OkxExecutionWsClient::send_message(const std::string& msg, bool high_priority) {
    if (!ws_ || !ws_->is_open()) return;

    if (!async_mode_) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        beast::error_code ec;
        ws_->write(net::buffer(msg), ec);
        if (ec) std::cerr << "[ExecWS] Write error: " << ec.message() << std::endl;
    } else {
        net::post(strand_, [this, msg, high_priority] {
            if (high_priority) {
                // FOK / cancel никогда не теряем
                high_priority_queue_.push_back(msg);
            } else {
                // Low-priority: ограничиваем 50, при переполнении выкидываем старый
                if (low_priority_queue_.size() >= 50) {
                    low_priority_queue_.pop_front();
                }
                low_priority_queue_.push_back(msg);
            }
            if (!write_in_progress_) {
                do_write();
            }
        });
    }
}

void OkxExecutionWsClient::do_write() {
    // Сначала high priority (FOK, cancel), потом всё остальное
    std::string msg;
    if (!high_priority_queue_.empty()) {
        msg = std::move(high_priority_queue_.front());
        high_priority_queue_.pop_front();
    } else if (!low_priority_queue_.empty()) {
        msg = std::move(low_priority_queue_.front());
        low_priority_queue_.pop_front();
    } else {
        write_in_progress_ = false;
        return;
    }

    write_in_progress_ = true;
    // msg должен жить до конца async_write — используем shared_ptr
    auto msg_ptr = std::make_shared<std::string>(std::move(msg));

    ws_->async_write(net::buffer(*msg_ptr),
        net::bind_executor(strand_,
            [this, msg_ptr](beast::error_code ec, std::size_t) {
                if (ec) std::cerr << "[ExecWS] Write error: " << ec.message() << std::endl;
                do_write();
            }));
}

// ---------- Загрузка instIdCode ----------
void OkxExecutionWsClient::fetch_inst_id_codes() {
    try {
        net::io_context rest_ioc;
        ssl::context rest_ctx{ssl::context::tlsv12_client};
        net::ssl::stream<net::ip::tcp::socket> stream(rest_ioc, rest_ctx);

        tcp::resolver resolver(rest_ioc);
        auto const results = resolver.resolve("www.okx.com", "443");
        net::connect(stream.next_layer(), results);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), "www.okx.com"))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                        net::error::get_ssl_category()));
        stream.handshake(net::ssl::stream_base::client);

        http::request<http::empty_body> req{http::verb::get, "/api/v5/public/instruments?instType=SWAP", 11};
        req.set(http::field::host, "www.okx.com");

        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);

        json instruments = json::parse(res.body());
        if (instruments.contains("data")) {
            for (const auto& inst : instruments["data"]) {
                std::string id = inst["instId"];
                if (inst.contains("instIdCode") && !inst["instIdCode"].is_null()) {
                    if (inst["instIdCode"].is_number()) {
                        inst_id_codes_[id] = std::to_string(inst["instIdCode"].get<int>());
                    } else if (inst["instIdCode"].is_string()) {
                        inst_id_codes_[id] = inst["instIdCode"].get<std::string>();
                    }
                }
            }
        }
        std::cerr << "[OkxExecWS] Loaded " << inst_id_codes_.size() << " instIdCodes\n";
        beast::error_code ec;
        stream.shutdown(ec);
    } catch (std::exception const& e) {
        std::cerr << "[OkxExecWS] Warning: failed to fetch instIdCodes: " << e.what() << "\n";
    }
}

// ---------- Обработка сообщений ----------
void OkxExecutionWsClient::handle_message(const std::string& msg) {
    // Ответ на серверный ping
    if (msg.find("\"op\":\"ping\"") != std::string::npos) {
        send_message(R"({"op":"pong"})");
        return;
    }
    if (msg.find("\"event\":\"subscribe\"") != std::string::npos ||
        msg.find("\"event\":\"channel-conn-count\"") != std::string::npos ||
        msg.find("\"op\":\"pong\"") != std::string::npos) {
        return;
    }

    // Push канала orders
    if (msg.find("\"channel\":\"orders\"") != std::string::npos) {
        try {
            json j = json::parse(msg);
            auto data_arr = j["data"];
            if (data_arr.is_array() && !data_arr.empty()) {
                std::string instId;
                if (j.contains("arg") && j["arg"].is_object() &&
                    j["arg"].contains("instId") && j["arg"]["instId"].is_string()) {
                    instId = j["arg"]["instId"].get<std::string>();
                } else if (data_arr[0].contains("instId") && data_arr[0]["instId"].is_string()) {
                    instId = data_arr[0]["instId"].get<std::string>();
                }

                for (const auto& item : data_arr) {
                    std::string ordId = item.value("clOrdId", "");
                    if (ordId.empty()) ordId = item.value("ordId", "");
                    std::string state = item.value("state", "");
                    std::string side  = item.value("side", "");
                    double fillSz = 0.0, avgPx = 0.0, pnl = 0.0, fee = 0.0;
                    if (item.contains("accFillSz")) {
                        std::string val = item["accFillSz"].get<std::string>();
                        if (!val.empty()) fillSz = std::stod(val);
                    }
                    if (item.contains("avgPx")) {
                        std::string val = item["avgPx"].get<std::string>();
                        if (!val.empty()) avgPx = std::stod(val);
                    }
                    if (item.contains("pnl")) {
                        std::string val = item["pnl"].get<std::string>();
                        if (!val.empty()) pnl = std::stod(val);
                    }
                    if (item.contains("fee")) {
                        std::string val = item["fee"].get<std::string>();
                        if (!val.empty()) fee = std::stod(val);
                    }
                    for (auto& cb : order_update_callbacks_) {
                        cb(instId, ordId, state, fillSz, avgPx, side, pnl, fee);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[OkxExecWS] Error parsing order update: " << e.what() << std::endl;
        }
        return;
    }

    // Push канала positions
    if (msg.find("\"arg\":{\"channel\":\"positions\"") != std::string::npos) {
        auto safe_stod = [](const std::string& s) -> double {
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };
        try {
            json j = json::parse(msg);
            auto data_arr = j["data"];
            if (data_arr.is_array() && !data_arr.empty()) {
                for (const auto& item : data_arr) {
                    std::string instId = item.value("instId", "");
                    std::string posStr = item.value("pos", "0");
                    std::string avgPxStr = item.value("avgPx", "0");
                    std::string markPxStr = item.value("markPx", "0");
                    double pos = safe_stod(posStr);
                    double avgPx = safe_stod(avgPxStr);
                    double markPx = safe_stod(markPxStr);
                    for (auto& cb : position_callbacks_) {
                        cb(instId, pos, avgPx, markPx);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[OkxExecWS] Error parsing positions update: " << e.what() << std::endl;
        }
        return;
    }

    // Обработка ответов на place_order
    try {
        json j = json::parse(msg);
        if (j.contains("id")) {
            int id = std::stoi(j["id"].get<std::string>());
            OkxOrderCallback cb;
            {
                std::lock_guard lock(request_mutex_);
                auto it = pending_requests_.find(id);
                if (it != pending_requests_.end()) {
                    cb = std::move(it->second);
                    pending_requests_.erase(it);
                }
            }

            OkxOrderResponse resp;
            resp.raw_json = msg;
            std::string code_str = j.value("code", "1");
            resp.success = (code_str == "0");
            resp.code = std::stoi(code_str);
            resp.msg = j.value("msg", "");
            auto data_arr = j["data"];
            if (data_arr.is_array() && !data_arr.empty()) {
                auto first = data_arr[0];
                resp.ordId = first.value("ordId", "");
                resp.clOrdId = first.value("clOrdId", "");
            }
            if (cb) cb(resp);
        }
    } catch (...) {}
}

// ---------- Вспомогательные методы ----------
std::string OkxExecutionWsClient::generate_client_order_id(const std::string& symbol) const {
    static std::atomic<uint64_t> seq{0};
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return "okx" + std::to_string(us) + std::to_string(++seq);
}

std::string OkxExecutionWsClient::make_signature(const std::string& data) const {
    return HmacSha256Base64(api_secret_, data);
}

std::string OkxExecutionWsClient::get_timestamp_sec() const {
    return GetTimestampSec();
}

void OkxExecutionWsClient::add_order_update_callback(OrderUpdateCallback cb) {
    order_update_callbacks_.push_back(std::move(cb));
}

void OkxExecutionWsClient::add_position_callback(PositionCallback cb) {
    position_callbacks_.push_back(std::move(cb));
}

void OkxExecutionWsClient::ensure_leverage_set(const std::string& instId) {
    // Метод больше не используется, можно оставить пустым или удалить
}

std::string OkxExecutionWsClient::get_inst_id_code(const std::string& instId) const {
    auto it = inst_id_codes_.find(instId);
    return (it != inst_id_codes_.end()) ? it->second : std::string{};
}

int OkxExecutionWsClient::place_order(const OkxOrderParams& params, OkxOrderCallback callback) {
    std::lock_guard lock(request_mutex_);
    auto it = inst_id_codes_.find(params.instId);
    if (it == inst_id_codes_.end()) {
        std::cerr << "[OkxExecWS] No instIdCode for " << params.instId << ", order skipped\n";
        return -1;
    }

    int id = next_request_id_++;
    if (pending_requests_.size() >= 50) {
        std::cerr << "[OkxExecWS] Too many pending requests, dropping new order\n";
        return -1;
    }
    pending_requests_[id] = std::move(callback);

    std::string msg;
    msg.reserve(512);
    msg += R"({"id":")";
    msg += std::to_string(id);
    msg += R"(","op":"order","args":[{"instId":")";
    msg += params.instId;
    msg += R"(","instIdCode":")";
    msg += it->second;
    msg += R"(","tdMode":")";
    msg += params.tdMode;
    msg += R"(","side":")";
    msg += params.side;
    msg += R"(","ordType":")";
    msg += params.ordType;
    msg += R"(")";
    if (params.sz > 0) {
        msg += R"(,"sz":")";
        msg += std::to_string(params.sz);
        msg += R"(")";
    }
    if (params.px != 0.0) {
        msg += R"(,"px":")";
        msg += std::to_string(params.px);
        msg += R"(")";
    }
    if (params.reduceOnly) {
        msg += R"(,"reduceOnly":"true")";
    }
    if (!params.clOrdId.empty()) {
        msg += R"(,"clOrdId":")";
        msg += params.clOrdId;
        msg += R"(")";
    }
    msg += R"(}]})";

    send_message(msg, /*high_priority=*/true);
    return id;
}