#include "binance_rest_client.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <simdjson.h>
#include <iostream>
#include <stdexcept>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

static std::string https_get(const std::string& host, const std::string& target) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_verify_mode(ssl::verify_none); // dev only
    beast::ssl_stream<tcp::socket> stream(ioc, ctx);
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, "443");
    net::connect(beast::get_lowest_layer(stream), results);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                     net::error::get_ssl_category()));
    stream.handshake(ssl::stream_base::client);
    http::request<http::empty_body> req(http::verb::get, target, 11);
    req.set(http::field::host, host);
    http::write(stream, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    return res.body();
}

BinanceRestClient::BinanceRestClient() {}

std::vector<TickerInfo> BinanceRestClient::fetch_tickers() {
    std::string body = https_get("fapi.binance.com", "/fapi/v1/ticker/24hr");
    std::vector<TickerInfo> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) {
        std::cerr << "[BinanceRest] fetch_tickers parse error" << std::endl;
        return result;
    }
    for (auto item : doc.get_array()) {
        simdjson::dom::object obj = item.get_object().value_unsafe();
        std::string_view sym_sv = obj["symbol"].get_string().value_unsafe();
        std::string symbol(sym_sv);
        if (symbol.find("USDT") == std::string::npos || symbol.find("USDC") != std::string::npos) continue;
        double qv = 0.0;
        auto qv_elem = obj["quoteVolume"];
        if (qv_elem.is_string()) {
            std::string_view sv = qv_elem.get_string().value_unsafe();
            qv = std::stod(std::string(sv));
        } else if (qv_elem.is_double()) {
            qv = qv_elem.get_double().value_unsafe();
        }
        int count = 0;
        auto cnt_elem = obj["count"];
        if (cnt_elem.is_int64()) {
            count = static_cast<int>(cnt_elem.get_int64().value_unsafe());
        } else if (cnt_elem.is_string()) {
            count = std::stoi(std::string(cnt_elem.get_string().value_unsafe()));
        }
        result.push_back({symbol, qv, count});
    }
    return result;
}

std::vector<OHLCV> BinanceRestClient::fetch_ohlcv(const std::string& symbol, 
                                                  const std::string& interval, int limit) {
    std::string target = "/fapi/v1/klines?symbol=" + symbol + "&interval=" + interval +
                         "&limit=" + std::to_string(limit);
    std::string body = https_get("fapi.binance.com", target);
    std::vector<OHLCV> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return result;
    for (auto candle : doc.get_array()) {
        simdjson::dom::array arr = candle.get_array().value_unsafe();
        OHLCV o;
        o.open  = std::stod(std::string(arr.at(1).get_string().value_unsafe()));
        o.high  = std::stod(std::string(arr.at(2).get_string().value_unsafe()));
        o.low   = std::stod(std::string(arr.at(3).get_string().value_unsafe()));
        o.close = std::stod(std::string(arr.at(4).get_string().value_unsafe()));
        o.volume = std::stod(std::string(arr.at(5).get_string().value_unsafe()));
        result.push_back(o);
    }
    return result;
}

double BinanceRestClient::fetch_last_price(const std::string& symbol) {
    std::string target = "/fapi/v1/ticker/price?symbol=" + symbol;
    std::string body = https_get("fapi.binance.com", target);
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return 0.0;
    return std::stod(std::string(doc["price"].get_string().value_unsafe()));
}

LotInfo BinanceRestClient::fetch_lot_size(const std::string& symbol) {
    LotInfo info{0.001, 0.01, 0.001};   // безопасные значения по умолчанию
    std::string body = https_get("fapi.binance.com", "/fapi/v1/exchangeInfo?symbol=" + symbol);
    if (body.empty()) {
        std::cerr << "[BinanceRest] fetch_lot_size: empty response\n";
        return info;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return info;

    simdjson::dom::object root;
    if (doc.get(root)) return info;
    auto symbols_arr = root["symbols"];
    if (symbols_arr.error()) return info;

    for (auto sym : symbols_arr.get_array()) {
        std::string_view sym_name;
        if (sym["symbol"].get(sym_name) || sym_name != symbol) continue;
        auto filters = sym["filters"];
        if (filters.error()) continue;

        for (auto filter : filters.get_array()) {
            simdjson::dom::object f;
            if (filter.get(f)) continue;
            std::string_view filterType;
            if (f["filterType"].get(filterType)) continue;

            if (filterType == "LOT_SIZE") {
                std::string_view stepSize;
                if (!f["stepSize"].get(stepSize))
                    info.step_size = std::stod(std::string(stepSize));
                std::string_view minQty;
                if (!f["minQty"].get(minQty))
                    info.min_qty = std::stod(std::string(minQty));
            } else if (filterType == "PRICE_FILTER") {
                std::string_view tickSize;
                if (!f["tickSize"].get(tickSize))
                    info.tick_size = std::stod(std::string(tickSize));
            }
        }
        break;   // нашли нужный символ – выходим
    }
    return info;
}

double BinanceRestClient::fetch_tick_size(const std::string& symbol) {
    return fetch_lot_size(symbol).tick_size;
}