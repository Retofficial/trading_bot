#include "trade_telemetry.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>

TradeTelemetry& TradeTelemetry::instance() {
    static TradeTelemetry inst;
    return inst;
}

TradeTelemetry::TradeTelemetry() {
    start_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

TradeTelemetry::~TradeTelemetry() {
    flush();
}

void TradeTelemetry::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled && !enabled_) {
        csv_file_.open("trade_telemetry.csv", std::ios::app);
        if (!csv_file_.is_open()) {
            std::cerr << "[TradeTelemetry] Failed to open CSV file\n";
            return;
        }
        write_header_if_needed();
    }
    enabled_ = enabled;
    if (!enabled_ && csv_file_.is_open()) {
        csv_file_.flush();
        csv_file_.close();
    }
}

void TradeTelemetry::begin_spike(uint64_t spike_id, const std::string& symbol,
                                 const std::string& direction, double amplitude_pct) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    auto& rec = records_[spike_id];
    rec.spike_id = spike_id;
    rec.symbol = symbol;
    rec.direction = direction;
    rec.amplitude_pct = amplitude_pct;
    rec.spike_detected_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - start_time_us_;
    rec.ended = false;
}

void TradeTelemetry::mark_stage(uint64_t spike_id, const std::string& stage,
                                const std::string& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    auto it = records_.find(spike_id);
    if (it == records_.end()) return;
    auto& rec = it->second;
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - start_time_us_;

    if (stage == "pause_start") rec.pause_start_us = now_us;
    else if (stage == "pause_end") rec.pause_end_us = now_us;
    else if (stage == "distance_check") rec.distance_check_us = now_us;
    else if (stage == "ml_start") rec.ml_start_us = now_us;
    else if (stage == "ml_end") rec.ml_end_us = now_us;
    else if (stage == "signal_generated") rec.signal_generated_us = now_us;
    else if (stage == "entry_done") rec.entry_done_us = now_us;
    else if (stage == "fok_sent") rec.fok_sent_us = now_us;
    else if (stage == "fill_received") rec.fill_received_us = now_us;
    else if (stage == "stop_sent") rec.stop_sent_us = now_us;
    else if (stage == "take1_sent") rec.take1_sent_us = now_us;
    else if (stage == "take2_sent") rec.take2_sent_us = now_us;
    else if (stage == "close_sent") rec.close_sent_us = now_us;
}

void TradeTelemetry::set_field(uint64_t spike_id, const std::string& key, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    auto it = records_.find(spike_id);
    if (it == records_.end()) return;
    auto& rec = it->second;
    if (key == "distance_pct") rec.distance_pct = value;
    else if (key == "ml_prob0") rec.ml_prob0 = value;
    else if (key == "ml_prob1") rec.ml_prob1 = value;
    else if (key == "ml_prob2") rec.ml_prob2 = value;
    else if (key == "entry_price") rec.entry_price = value;
    else if (key == "mfe_pct") rec.mfe_pct = value;
    else if (key == "mae_pct") rec.mae_pct = value;
    else if (key == "real_trade") rec.real_trade = (value != 0.0);
}

void TradeTelemetry::set_string(uint64_t spike_id, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    auto it = records_.find(spike_id);
    if (it == records_.end()) return;
    auto& rec = it->second;
    if (key == "distance_result") rec.distance_result = value;
    else if (key == "ml_result") rec.ml_result = value;
    else if (key == "outcome") rec.outcome = value;
    else if (key == "symbol") rec.symbol = value;
}

void TradeTelemetry::end_spike(uint64_t spike_id, const std::string& outcome,
                               double mfe, double mae) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    auto it = records_.find(spike_id);
    if (it == records_.end()) return;
    auto& rec = it->second;
    if (rec.ended) return;
    rec.outcome = outcome;
    rec.mfe_pct = mfe;
    rec.mae_pct = mae;
    rec.resolution_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - start_time_us_;
    rec.ended = true;
    write_record(rec);
    records_.erase(it);
}

void TradeTelemetry::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    for (auto& [id, rec] : records_) {
        if (!rec.ended) {
            rec.resolution_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - start_time_us_;
            rec.ended = true;
            write_record(rec);
        }
    }
    records_.clear();
    if (csv_file_.is_open()) csv_file_.flush();
}

void TradeTelemetry::write_header_if_needed() {
    if (header_written_ || !csv_file_.is_open()) return;
    csv_file_ << "spike_id,symbol,direction,amplitude_pct,start_us,spike_detected_us,pause_start_us,pause_end_us,"
                 "distance_check_us,distance_result,distance_pct,ml_start_us,ml_end_us,ml_prob0,ml_prob1,ml_prob2,"
                 "ml_result,signal_generated_us,entry_done_us,entry_price,resolution_us,outcome,mfe_pct,mae_pct,"
                 "real_trade,fok_sent_us,fill_received_us,stop_sent_us,take1_sent_us,take2_sent_us,close_sent_us\n";
    header_written_ = true;
}

void TradeTelemetry::write_record(const TelemetryRecord& rec) {
    if (!csv_file_.is_open()) return;
    csv_file_ << rec.spike_id << ","
              << rec.symbol << ","
              << rec.direction << ","
              << rec.amplitude_pct << ","
              << "0,"
              << rec.spike_detected_us << ","
              << rec.pause_start_us << ","
              << rec.pause_end_us << ","
              << rec.distance_check_us << ","
              << rec.distance_result << ","
              << rec.distance_pct << ","
              << rec.ml_start_us << ","
              << rec.ml_end_us << ","
              << rec.ml_prob0 << ","
              << rec.ml_prob1 << ","
              << rec.ml_prob2 << ","
              << rec.ml_result << ","
              << rec.signal_generated_us << ","
              << rec.entry_done_us << ","
              << rec.entry_price << ","
              << rec.resolution_us << ","
              << rec.outcome << ","
              << rec.mfe_pct << ","
              << rec.mae_pct << ","
              << (rec.real_trade ? "1" : "0") << ","
              << rec.fok_sent_us << ","
              << rec.fill_received_us << ","
              << rec.stop_sent_us << ","
              << rec.take1_sent_us << ","
              << rec.take2_sent_us << ","
              << rec.close_sent_us << "\n";
    csv_file_.flush();
}