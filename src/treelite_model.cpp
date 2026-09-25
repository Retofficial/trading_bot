#include "treelite_model.h"
#include <treelite/c_api.h>
#include <sys/stat.h>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <thread>

struct TreeliteModel::Impl {
    TreeliteModelHandle model = nullptr;
    TreeliteGTILConfigHandle config = nullptr;
    int num_feature = 0;
    int num_class = 0;

    ~Impl() {
        if (config) TreeliteGTILDeleteConfig(config);
        if (model)  TreeliteFreeModel(model);
    }
};

static int64_t get_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtime);
}

TreeliteModel::TreeliteModel(const std::string& model_path)
    : model_file_(model_path) {
    load_new_impl(model_path);
}

TreeliteModel::~TreeliteModel() {
    stop_watcher();
}

void TreeliteModel::load_new_impl(const std::string& path) {
    auto new_impl = std::make_shared<Impl>();

    // Загрузка десериализованной модели (.zip)
    int ret = TreeliteDeserializeModelFromFile(path.c_str(), &new_impl->model);
    if (ret != 0 || !new_impl->model) {
        throw std::runtime_error("TreeliteDeserializeModelFromFile failed: " + std::to_string(ret));
    }

    // Конфиг GTIL: default = sum over trees + post-processing (softmax для multiclass)
    const char* config_json = R"({"predict_type":"default","nthread":1})";
    ret = TreeliteGTILParseConfig(config_json, &new_impl->config);
    if (ret != 0 || !new_impl->config) {
        throw std::runtime_error("TreeliteGTILParseConfig failed: " + std::to_string(ret));
    }

    // Число фичей
    int nf = 0;
    if (TreeliteQueryNumFeature(new_impl->model, &nf) == 0) {
        new_impl->num_feature = nf;
    }

    // Число классов через форму выхода
    uint64_t const* shape = nullptr;
    uint64_t ndim = 0;
    ret = TreeliteGTILGetOutputShape(new_impl->model, 1, new_impl->config, &shape, &ndim);

    std::cerr << "[TreeliteModel] GetOutputShape ret=" << ret << " ndim=" << ndim;
    if (ret == 0 && shape) {
        for (uint64_t i = 0; i < ndim; ++i)
            std::cerr << " shape[" << i << "]=" << shape[i];
    }
    std::cerr << std::endl;

    if (ret == 0 && shape && ndim >= 1) {
        // num_class == 1 означает регрессию (output shape = [1] или [1,1])
        new_impl->num_class = static_cast<int>(shape[ndim - 1]);
        // Если это не регрессия — оставляем как есть. Multiclass даст shape[-1] = num_classes > 1.
    } else {
        std::cerr << "[TreeliteModel] Cannot get output shape, defaulting to regression" << std::endl;
        new_impl->num_class = 1;
    }

    {
        std::lock_guard lock(load_mutex_);
        std::atomic_store(&impl_, new_impl);
    }

    std::cerr << "[TreeliteModel] Loaded model from " << path
              << " (features=" << new_impl->num_feature
              << ", classes=" << new_impl->num_class << ")" << std::endl;
}

std::vector<float> TreeliteModel::predict_probabilities(const std::vector<float>& features) {
    auto impl = std::atomic_load(&impl_);
    if (!impl || !impl->model || !impl->config) {
        return std::vector<float>(3, 0.0f);
    }

    if (static_cast<int>(features.size()) != impl->num_feature) {
        std::cerr << "[TreeliteModel] Feature size mismatch: got "
                  << features.size() << ", expected " << impl->num_feature << std::endl;
        return std::vector<float>(impl->num_class, 0.0f);
    }

    std::vector<float> output(impl->num_class, 0.0f);

    int ret = TreeliteGTILPredict(
        impl->model,
        features.data(),
        "float32",
        1,                     // одна строка
        output.data(),
        impl->config);

    if (ret != 0) {
        std::cerr << "[TreeliteModel] GTILPredict failed: " << ret << std::endl;
        return std::vector<float>(impl->num_class, 0.0f);
    }

    return output;
}

float TreeliteModel::predict_single(const std::vector<float>& features) {
    auto impl = std::atomic_load(&impl_);
    if (!impl || !impl->model || !impl->config) {
        return 0.0f;
    }
    if (static_cast<int>(features.size()) != impl->num_feature) {
        std::cerr << "[TreeliteModel] Feature size mismatch: got "
                  << features.size() << ", expected " << impl->num_feature << std::endl;
        return 0.0f;
    }

    float output = 0.0f;
    int ret = TreeliteGTILPredict(
        impl->model,
        features.data(),
        "float32",
        1,
        &output,
        impl->config);

    if (ret != 0) {
        std::cerr << "[TreeliteModel] GTILPredict failed: " << ret << std::endl;
        return 0.0f;
    }
    return output;
}

void TreeliteModel::start_watcher(int poll_interval_sec) {
    if (watcher_running_) return;
    last_mtime_ = get_mtime(model_file_);
    watcher_running_ = true;
    watcher_thread_ = std::thread(&TreeliteModel::watcher_loop, this, poll_interval_sec);
}

void TreeliteModel::stop_watcher() {
    if (!watcher_running_) return;
    watcher_running_ = false;
    if (watcher_thread_.joinable()) watcher_thread_.join();
}

void TreeliteModel::watcher_loop(int poll_interval_sec) {
    while (watcher_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(poll_interval_sec));
        if (!watcher_running_) break;

        int64_t m = get_mtime(model_file_);
        if (m != 0 && m != last_mtime_) {
            std::cerr << "[TreeliteModel] Model file changed, reloading..." << std::endl;
            try {
                load_new_impl(model_file_);
                last_mtime_ = m;
                std::cerr << "[TreeliteModel] Hot-swap complete" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[TreeliteModel] Hot-swap failed: " << e.what() << std::endl;
            }
        }
    }
}