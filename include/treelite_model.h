#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

class TreeliteModel {
public:
    explicit TreeliteModel(const std::string& model_path);
    ~TreeliteModel();

    // Мультикласс: возвращает вероятности [класс0, класс1, класс2]
    std::vector<float> predict_probabilities(const std::vector<float>& features);

    // Регрессия: возвращает единственное число (log(1+mfe))
    float predict_single(const std::vector<float>& features);

    // Watcher: следит за mtime файла, при изменении — перезагружает в фоне
    void start_watcher(int poll_interval_sec = 60);
    void stop_watcher();

    const std::string& model_file() const { return model_file_; }

private:
    struct Impl;

    void watcher_loop(int poll_interval_sec);
    void load_new_impl(const std::string& path);

    std::string model_file_;
    std::shared_ptr<Impl> impl_;   // атомарно свапается
    std::mutex load_mutex_;        // защищает только процесс загрузки

    std::thread watcher_thread_;
    std::atomic<bool> watcher_running_{false};
    int64_t last_mtime_ = 0;
};