#pragma once
#include "feature_vector.hpp"
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <mutex>

class CsvFeatureLogger {
public:
    explicit CsvFeatureLogger(const std::string& symbol);
    ~CsvFeatureLogger();

    void write(double price, const FeatureVector& features, uint64_t timestamp_us);

private:
    std::string symbol_;
    std::ofstream file_;
    std::string filename_;
    std::mutex mutex_;

    static std::string make_filename(const std::string& symbol);
    void write_header();
};