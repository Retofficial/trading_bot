# HFT Infrastructure for Crypto Derivatives

Low-latency trading system for OKX USDT-M futures.
Focus: event-driven architecture, sub-millisecond inference, production-grade resilience.

## Architecture

text
[SymbolManager] → [Screener] → [MarketStreamAggregator × N]
                                        │
                                        ├── [OrderBook] (incremental)
                                        ├── [FeatureEngine] → ML model
                                        └── [Execution WS Client]
                                                             │
                                                             ├── [OkxTrader]
                                                             └── [RealTradeTracker]

## Technical Highlights

- **WebSocket clients** with auto-reconnect, backpressure, priority queues (Boost.Asio + Beast)
- **Incremental order book** maintenance (O(log n) level updates)
- **ML model inference** via Treelite: 20–50 µs per prediction
- **Hot-swap model reload** via mtime watcher — no restart
- **Custom JSON parsing** with simdjson (string-number direct parse)
- **Separate REST thread** for order management, pinned to dedicated core
- **Real-time trade tracking** via private WS (fills, positions, crossings)

## Stack

`C++17` `CMake` `Boost.Asio` `Boost.Beast` `simdjson` `Eigen` `libcurl` `OpenSSL`
`LightGBM` `Treelite` `nlohmann/json`
`Python` (training): `pandas` `numpy` `LightGBM` `SHAP`

## Status

Active development. Training pipeline in production; live trading on test volume.
Strategy details and trained models are not included.

## Structure

text
include/       Public headers
src/           Implementation
CMakeLists.txt Build configuration

## Note

This repository contains **infrastructure only**.
Alpha (feature engineering, model, strategy logic) is intentionally omitted.

---

*Built solo, from scratch. No CS degree, no prior industry experience.*
