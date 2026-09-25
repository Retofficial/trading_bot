#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <cstdint>

struct TelemetryRecord {
    uint64_t spike_id = 0;
    std::string symbol;
    std::string direction;
    double amplitude_pct = 0.0;

    uint64_t spike_detected_us = 0;
    uint64_t pause_start_us = 0;
    uint64_t pause_end_us = 0;
    uint64_t distance_check_us = 0;
    std::string distance_result;
    double distance_pct = 0.0;
    uint64_t ml_start_us = 0;
    uint64_t ml_end_us = 0;
    double ml_prob0 = 0.0, ml_prob1 = 0.0, ml_prob2 = 0.0;
    std::string ml_result;
    uint64_t signal_generated_us = 0;
    uint64_t entry_done_us = 0;
    double entry_price = 0.0;
    uint64_t resolution_us = 0;
    std::string outcome;
    double mfe_pct = 0.0;
    double mae_pct = 0.0;

    bool real_trade = false;
    uint64_t fok_sent_us = 0;
    uint64_t fill_received_us = 0;
    uint64_t stop_sent_us = 0;
    uint64_t take1_sent_us = 0;
    uint64_t take2_sent_us = 0;
    uint64_t close_sent_us = 0;

    bool ended = false;
};

class TradeTelemetry {
public:
    static TradeTelemetry& instance();

    void begin_spike(uint64_t spike_id, const std::string& symbol,
                     const std::string& direction, double amplitude_pct);
    void mark_stage(uint64_t spike_id, const std::string& stage,
                    const std::string& info = "");
    void set_field(uint64_t spike_id, const std::string& key, double value);
    void set_string(uint64_t spike_id, const std::string& key, const std::string& value);
    void end_spike(uint64_t spike_id, const std::string& outcome,
                   double mfe, double mae);
    void flush();
    void set_enabled(bool enabled);

private:
    TradeTelemetry();
    ~TradeTelemetry();

    void write_header_if_needed();
    void write_record(const TelemetryRecord& rec);

    std::unordered_map<uint64_t, TelemetryRecord> records_;
    std::mutex mutex_;
    std::ofstream csv_file_;
    bool enabled_ = false;
    bool header_written_ = false;
    uint64_t start_time_us_ = 0;
};