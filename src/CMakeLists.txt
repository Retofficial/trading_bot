cmake_minimum_required(VERSION 3.16)
project(MarketStreamAggregator LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

find_package(Boost REQUIRED)
find_package(simdjson REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# --- Treelite runtime (установлен из исходников в /usr/local) ---
find_library(TREELITE_RUNTIME_LIB NAMES treelite_runtime treelite PATHS /usr/local/lib)

add_library(market_aggregator SHARED
    src/market_stream_aggregator.cpp
    src/websocket_client_live.cpp
    src/okx_live_websocket_client.cpp
    src/okx_execution_ws_client.cpp
    src/okx_trader.cpp
    src/c_api.cpp
    src/spike_signal_detector.cpp
    src/treelite_model.cpp            
    src/binance_rest_client.cpp
    src/okx_rest_client.cpp
    src/screener.cpp
    src/csv_feature_logger.cpp
    src/symbol_manager.cpp
    src/binance_trader.cpp
    src/execution_ws_client.cpp
    src/trade_telemetry.cpp
    src/real_trade_tracker.cpp
    src/regime_logger.cpp
)

target_include_directories(market_aggregator PUBLIC include)
target_include_directories(market_aggregator PRIVATE /usr/local/include)

target_link_libraries(market_aggregator PUBLIC
    simdjson::simdjson
    Eigen3::Eigen
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ${TREELITE_RUNTIME_LIB}
)

add_executable(live_tool test/live_tool.cpp)
target_link_libraries(live_tool market_aggregator)