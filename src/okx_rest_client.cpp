#include "okx_rest_client.hpp"
#include <curl/curl.h>
#include <simdjson.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <unordered_map>
#include <chrono>
#include <ctime>
#include <cmath>
#include <algorithm>

#ifdef __linux__
#include <pthread.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

class CurlSlistGuard {
public:
    CurlSlistGuard() : list_(nullptr) {}
    ~CurlSlistGuard() { if (list_) curl_slist_free_all(list_); }
    void append(const std::string& str) {
        list_ = curl_slist_append(list_, str.c_str());
    }
    curl_slist* get() const { return list_; }
private:
    curl_slist* list_;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

static std::string https_get(const std::string& host, const std::string& target) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string url = "https://" + host + target;
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? response : "";
}

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

static std::string GetTimestampIso() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::time_t sec = std::chrono::system_clock::to_time_t(now);
    std::ostringstream iso;
    iso << std::put_time(std::gmtime(&sec), "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << (ms % 1000) << 'Z';
    return iso.str();
}

OkxRestClient::OkxRestClient() {
    request_thread_running_ = true;
    request_thread_ = std::thread(&OkxRestClient::process_requests, this);

    callback_thread_running_ = true;
    callback_thread_ = std::thread([this]() {
        while (callback_thread_running_) {
            std::function<void()> cb;
            {
                std::unique_lock lock(callback_mutex_);
                callback_cv_.wait(lock, [this] { return !callback_queue_.empty() || !callback_thread_running_; });
                if (!callback_thread_running_) break;
                cb = std::move(callback_queue_.front());
                callback_queue_.pop();
            }
            cb();
        }
    });
}

OkxRestClient::~OkxRestClient() {
    request_thread_running_ = false;
    request_cv_.notify_all();
    if (request_thread_.joinable()) request_thread_.join();

    // Вызываем колбэки невыполненных запросов с ошибкой
    {
        std::lock_guard lock(request_mutex_);
        while (!request_queue_.empty()) {
            Request req = std::move(request_queue_.front());
            request_queue_.pop();
            if (req.cb) {
                OkxRestResponse resp;
                resp.success = false;
                resp.code = -1;
                resp.msg = "Client destroyed";
                req.cb(resp);
            }
        }
    }

    callback_thread_running_ = false;
    callback_cv_.notify_all();
    if (callback_thread_.joinable()) callback_thread_.join();

    // Выполняем оставшиеся колбэки из callback_queue_ (обычно пусто)
    {
        std::lock_guard lock(callback_mutex_);
        while (!callback_queue_.empty()) {
            auto cb = std::move(callback_queue_.front());
            callback_queue_.pop();
            cb();
        }
    }
}

void OkxRestClient::set_credentials(const std::string& api_key,
                                    const std::string& api_secret,
                                    const std::string& passphrase) {
    api_key_ = api_key;
    api_secret_ = api_secret;
    passphrase_ = passphrase;
}

void OkxRestClient::place_order(const json& body, std::function<void(const OkxRestResponse&)> cb) {
    {
        std::lock_guard lock(request_mutex_);
        if (request_queue_.size() >= 50) {
            request_queue_.pop();  
        }
        request_queue_.push({"POST", "/api/v5/trade/order", body, std::move(cb)});
    }
    request_cv_.notify_one();
}

void OkxRestClient::place_algo_order(const json& body, std::function<void(const OkxRestResponse&)> cb) {
    {
        std::lock_guard lock(request_mutex_);
        if (request_queue_.size() >= 50) {
            request_queue_.pop();  
        }
        request_queue_.push({"POST", "/api/v5/trade/order-algo", body, std::move(cb)});
    }
    request_cv_.notify_one();
}

void OkxRestClient::place_batch_orders(const json& body,
                                       std::function<void(const OkxRestResponse&)> cb) {
    {
        std::lock_guard lock(request_mutex_);
        if (request_queue_.size() >= 50) {
            request_queue_.pop();
        }
        request_queue_.push({"POST", "/api/v5/trade/batch-orders", body, std::move(cb)});
    }
    request_cv_.notify_one();
}

void OkxRestClient::get_algo_order_by_clord(const std::string& algoClOrdId,
                                            const std::string& instId,
                                            std::function<void(const OkxRestResponse&)> cb) {
    std::string path = "/api/v5/trade/order-algo?instId=" + instId + "&algoClOrdId=" + algoClOrdId;
    {
        std::lock_guard lock(request_mutex_);
        if (request_queue_.size() >= 50) {
            request_queue_.pop();  
        }
        request_queue_.push({"GET", path, json::object(), std::move(cb)});
    }
    request_cv_.notify_one();
}

void OkxRestClient::cancel_algo_order(const std::string& algoId,
                                      const std::string& instId,
                                      std::function<void(const OkxRestResponse&)> cb) {
    json body = {
        {"algoId", algoId},
        {"instId", instId}
    };
    {
        std::lock_guard lock(request_mutex_);
        if (request_queue_.size() >= 50) {
            request_queue_.pop();  
        }
        request_queue_.push({"POST", "/api/v5/trade/cancel-algo", body, std::move(cb)});
    }
    request_cv_.notify_one();
}

void OkxRestClient::process_requests() {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    nice(10);
#endif
    curl_handle_ = curl_easy_init();
    if (!curl_handle_) return;

    // Статические опции: устанавливаются один раз
    curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, WriteCallback);

    while (request_thread_running_) {
        Request req;
        {
            std::unique_lock lock(request_mutex_);
            request_cv_.wait(lock, [this] { return !request_queue_.empty() || !request_thread_running_; });
            if (!request_thread_running_) break;
            req = std::move(request_queue_.front());
            request_queue_.pop();
        }

        OkxRestResponse resp = send_signed_request(req.method, req.path, req.body);
        if (req.cb) {
            enqueue_callback([cb = std::move(req.cb), resp = std::move(resp)]() { cb(resp); });
        }
    }
    curl_easy_cleanup(curl_handle_);
}

OkxRestResponse OkxRestClient::send_signed_request(const std::string& method,
                                                   const std::string& path,
                                                   const json& body) {
    OkxRestResponse resp;
    if (api_key_.empty()) {
        resp.success = false;
        resp.code = -2;
        resp.msg = "Credentials not set";
        return resp;
    }

    std::string timestamp = GetTimestampIso();
    std::string body_str = (method == "POST") ? body.dump() : "";
    std::string sign = HmacSha256Base64(api_secret_, timestamp + method + path + body_str);

    std::string url = "https://www.okx.com" + path;
    std::string response;

    // Сброс метода
    curl_easy_setopt(curl_handle_, CURLOPT_POST, 0L);
    curl_easy_setopt(curl_handle_, CURLOPT_HTTPGET, 0L);

    if (method == "POST") {
        curl_easy_setopt(curl_handle_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE, body_str.size());
    } else {
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPGET, 1L);
    }

    CurlSlistGuard headers;
    headers.append("Content-Type: application/json");
    headers.append("OK-ACCESS-KEY: " + api_key_);
    headers.append("OK-ACCESS-SIGN: " + sign);
    headers.append("OK-ACCESS-TIMESTAMP: " + timestamp);
    headers.append("OK-ACCESS-PASSPHRASE: " + passphrase_);
    curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, headers.get());

    curl_easy_setopt(curl_handle_, CURLOPT_URL, url.c_str());
    response.clear();
    curl_easy_setopt(curl_handle_, CURLOPT_WRITEDATA, &response);   // меняется на каждый запрос

    CURLcode res = curl_easy_perform(curl_handle_);

    if (res != CURLE_OK) {
        resp.success = false;
        resp.code = -1;
        resp.msg = curl_easy_strerror(res);
        return resp;
    }

    try {
        json j = json::parse(response);
        std::string code_str = j.value("code", "1");
        resp.code = std::stoi(code_str);
        resp.msg = j.value("msg", "");
        resp.success = (code_str == "0");
        if (j.contains("data")) resp.data = j["data"];
    } catch (const std::exception& e) {
        resp.success = false;
        resp.code = -3;
        resp.msg = std::string("Parse error: ") + e.what() + ", raw=" + response;
    }
    return resp;
}

void OkxRestClient::enqueue_callback(std::function<void()> cb) {
    {
        std::lock_guard lock(callback_mutex_);
        if (callback_queue_.size() >= 50) {
            callback_queue_.pop();  // отбрасываем самый старый колбэк
        }
        callback_queue_.push(std::move(cb));
    }
    callback_cv_.notify_one();
}
// =====================================================================
// Существующие публичные методы (без изменений)
// =====================================================================

std::vector<TickerInfo> OkxRestClient::fetch_tickers() {
    std::string body = https_get("www.okx.com", "/api/v5/market/tickers?instType=SWAP&limit=300");
    std::vector<TickerInfo> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) {
        return result;
    }
    auto data = doc["data"];
    ensure_ct_vals_loaded();  
    if (data.error()) return result;
    for (auto item : data.get_array()) {
        simdjson::dom::object obj = item.get_object().value_unsafe();
        std::string_view sym_sv = obj["instId"].get_string().value_unsafe();
        std::string symbol(sym_sv);
        if (symbol.find("-USDT-") == std::string::npos) continue;

        double baseVol = 0.0, last = 0.0, quoteVol = 0.0;
        auto parse_double = [](simdjson::dom::element val) -> double {
            if (val.is_string()) {
                std::string_view sv = val.get_string().value_unsafe();
                if (sv.empty()) return 0.0;
                try { return std::stod(std::string(sv)); } catch (...) { return 0.0; }
            } else if (val.is_double()) {
                return val.get_double().value_unsafe();
            }
            return 0.0;
        };
        baseVol = parse_double(obj["vol24h"]);
        last = parse_double(obj["last"]);
        double ctVal = get_ct_val(symbol);
        quoteVol = baseVol * ctVal * last;
        result.push_back({symbol, quoteVol, 0});
    }
    return result;
}

std::vector<OHLCV> OkxRestClient::fetch_ohlcv(const std::string& symbol,
                                              const std::string& interval, int limit) {
    std::string target = "/api/v5/market/candles?instId=" + symbol + "&bar=" + interval +
                         "&limit=" + std::to_string(limit);
    std::string body = https_get("www.okx.com", target);
    std::vector<OHLCV> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return result;
    auto data = doc["data"];
    if (data.error()) return result;
    for (auto candle : data.get_array()) {
        simdjson::dom::array arr = candle.get_array().value_unsafe();
        OHLCV o;
        o.open  = std::stod(std::string(arr.at(1).get_string().value_unsafe()));
        o.high  = std::stod(std::string(arr.at(2).get_string().value_unsafe()));
        o.low   = std::stod(std::string(arr.at(3).get_string().value_unsafe()));
        o.close = std::stod(std::string(arr.at(4).get_string().value_unsafe()));
        o.volume = std::stod(std::string(arr.at(7).get_string().value_unsafe()));
        result.push_back(o);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

void OkxRestClient::ensure_ct_vals_loaded() {
    if (!ct_val_cache_.empty()) return;
    std::string body = https_get("www.okx.com", "/api/v5/public/instruments?instType=SWAP&limit=500");
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return;
    auto data = doc["data"];
    if (data.error()) return;
    for (auto item : data.get_array()) {
        simdjson::dom::object obj = item.get_object().value_unsafe();
        std::string_view sym_sv = obj["instId"].get_string().value_unsafe();
        double ctVal = 1.0;
        auto ct = obj["ctVal"];
        if (ct.is_string()) {
            try { ctVal = std::stod(std::string(ct.get_string().value_unsafe())); } catch (...) { ctVal = 1.0; }
        }
        else if (ct.is_double()) ctVal = ct.get_double().value_unsafe();
        ct_val_cache_[std::string(sym_sv)] = ctVal;
    }
}

double OkxRestClient::get_ct_val(const std::string& symbol) {
    auto it = ct_val_cache_.find(symbol);
    return (it != ct_val_cache_.end()) ? it->second : 1.0;
}

double OkxRestClient::fetch_last_price(const std::string& symbol) {
    std::string target = "/api/v5/market/ticker?instId=" + symbol;
    std::string body = https_get("www.okx.com", target);
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return 0.0;
    auto data = doc["data"];
    if (data.error()) return 0.0;
    auto obj = data.get_array().at(0).get_object().value_unsafe();
    return std::stod(std::string(obj["last"].get_string().value_unsafe()));
}

LotInfo OkxRestClient::fetch_lot_size(const std::string& symbol) {
    LotInfo info{0.001, 0.01, 0.001};
    std::string target = "/api/v5/public/instruments?instType=SWAP&instId=" + symbol;
    std::string body = https_get("www.okx.com", target);
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return info;
    auto data = doc["data"];
    if (data.error()) return info;
    if (data.get_array().size() == 0) return info;
    auto obj = data.get_array().at(0).get_object().value_unsafe();
    std::string_view step_sv, tick_sv, min_sv;
    if (!obj["lotSz"].get(step_sv)) info.step_size = std::stod(std::string(step_sv));
    if (!obj["tickSz"].get(tick_sv)) info.tick_size = std::stod(std::string(tick_sv));
    if (!obj["minSz"].get(min_sv)) info.min_qty = std::stod(std::string(min_sv));
    std::string_view ct_val_sv;
    if (!obj["ctVal"].get(ct_val_sv)) info.ct_val = std::stod(std::string(ct_val_sv));
    return info;
}

std::vector<Kline> OkxRestClient::fetch_klines(const std::string& symbol,
                                                const std::string& bar, int limit) {
    std::string target = "/api/v5/market/candles?instId=" + symbol +
                         "&bar=" + bar + "&limit=" + std::to_string(limit);
    std::string body = https_get("www.okx.com", target);
    std::vector<Kline> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return result;
    auto data = doc["data"];
    if (data.error()) return result;
    for (auto candle : data.get_array()) {
        simdjson::dom::array arr = candle.get_array().value_unsafe();
        Kline k;
        k.time = std::stod(std::string(arr.at(0).get_string().value_unsafe())) / 1000.0;
        k.open   = std::stod(std::string(arr.at(1).get_string().value_unsafe()));
        k.high   = std::stod(std::string(arr.at(2).get_string().value_unsafe()));
        k.low    = std::stod(std::string(arr.at(3).get_string().value_unsafe()));
        k.close  = std::stod(std::string(arr.at(4).get_string().value_unsafe()));
        k.volume = std::stod(std::string(arr.at(5).get_string().value_unsafe()));
        k.quote_volume = std::stod(std::string(arr.at(7).get_string().value_unsafe()));
        result.push_back(k);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

double OkxRestClient::fetch_tick_size(const std::string& symbol) {
    return fetch_lot_size(symbol).tick_size;
}