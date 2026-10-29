// Real-library image contract. Build this and ImageRecognizer.cpp with
// ORT_API_MANUAL_INIT so the observer can record, then forward, the real Run.
#include "AIUtil/ImageRecognizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

#ifndef ORT_API_MANUAL_INIT
#error "Build every translation unit with ORT_API_MANUAL_INIT"
#endif

namespace {
using Json = nlohmann::json;
const OrtApi* real_api = nullptr;
bool observe_run = false;

struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> values;
};

struct Observation {
    Tensor input;
    Tensor output;
    size_t completed_runs = 0;
    size_t attempted_runs = 0;
} observation;

OrtStatus* ReadTensor(const OrtValue* value, Tensor& result) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    OrtStatus* status = real_api->GetTensorTypeAndShape(value, &info);
    if (status) return status;

    size_t rank = 0;
    size_t count = 0;
    ONNXTensorElementDataType type;
    status = real_api->GetTensorElementType(info, &type);
    if (!status) status = real_api->GetDimensionsCount(info, &rank);
    if (!status) {
        result.shape.resize(rank);
        status = real_api->GetDimensions(info, result.shape.data(), rank);
    }
    if (!status) status = real_api->GetTensorShapeElementCount(info, &count);
    real_api->ReleaseTensorTypeAndShapeInfo(info);
    if (status) return status;
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        return real_api->CreateStatus(ORT_FAIL, "Image contract expected float32 tensor");
    }
    void* raw = nullptr;
    status = real_api->GetTensorMutableData(const_cast<OrtValue*>(value), &raw);
    if (status) return status;
    const float* data = static_cast<const float*>(raw);
    result.values.assign(data, data + count);
    return nullptr;
}

OrtStatus* ORT_API_CALL ObserveRun(
    OrtSession* session, const OrtRunOptions* options,
    const char* const* input_names, const OrtValue* const* inputs, size_t input_count,
    const char* const* output_names, size_t output_count, OrtValue** outputs) noexcept {
    try {
        if (observe_run) {
            ++observation.attempted_runs;
            if (input_count != 1 || output_count != 1) {
                return real_api->CreateStatus(ORT_FAIL, "Image contract expected one input and output");
            }
            if (auto* status = ReadTensor(inputs[0], observation.input)) return status;
        }

        // No fake result: the unmodified input reaches the actual ONNX runtime.
        auto* status = real_api->Run(session, options, input_names, inputs, input_count,
                                    output_names, output_count, outputs);
        if (!status && observe_run) {
            status = ReadTensor(outputs[0], observation.output);
            if (!status) ++observation.completed_runs;
        }
        return status;
    } catch (...) {
        return real_api->CreateStatus(ORT_RUNTIME_EXCEPTION, "Image observer failed to record real tensors");
    }
}

class BudgetAllocator final : public cv::MatAllocator {
public:
    explicit BudgetAllocator(cv::MatAllocator* actual) : actual_(actual) {}

    cv::UMatData* allocate(int dims, const int* sizes, int type, void* data,
                          size_t* step, cv::AccessFlag flags,
                          cv::UMatUsageFlags usage) const override {
        if (!data) {
            size_t bytes = CV_ELEM_SIZE(type);
            for (int i = 0; i != dims; ++i) {
                if (sizes[i] <= 0 || static_cast<size_t>(sizes[i]) >
                        std::numeric_limits<size_t>::max() / bytes) {
                    bytes = std::numeric_limits<size_t>::max();
                    break;
                }
                bytes *= static_cast<size_t>(sizes[i]);
            }
            CheckAllocation(bytes);
        }
        return actual_->allocate(dims, sizes, type, data, step, flags, usage);
    }

    bool allocate(cv::UMatData* data, cv::AccessFlag flags,
                  cv::UMatUsageFlags usage) const override {
        if (data) CheckAllocation(data->size);
        return actual_->allocate(data, flags, usage);
    }

    void deallocate(cv::UMatData* data) const override { actual_->deallocate(data); }

    static constexpr size_t limit = 64u * 1024u * 1024u;
    mutable std::atomic<size_t> largest_requested{0};
    mutable std::atomic<size_t> largest_forwarded{0};
    mutable std::atomic<size_t> forwarded_requests{0};
    mutable std::atomic<size_t> blocked_requests{0};

private:
    static void UpdateMaximum(std::atomic<size_t>& target, size_t value) {
        size_t current = target.load();
        while (current < value && !target.compare_exchange_weak(current, value)) {}
    }

    void CheckAllocation(size_t bytes) const {
        UpdateMaximum(largest_requested, bytes);
        if (bytes > limit) {
            ++blocked_requests;
            throw std::runtime_error("Image contract allocator blocked a request above 64 MiB");
        }
        ++forwarded_requests;
        UpdateMaximum(largest_forwarded, bytes);
    }

    cv::MatAllocator* actual_;
};

class DefaultAllocatorScope {
public:
    explicit DefaultAllocatorScope(cv::MatAllocator* allocator)
        : previous_(cv::Mat::getDefaultAllocator()) {
        cv::Mat::setDefaultAllocator(allocator);
    }
    ~DefaultAllocatorScope() { cv::Mat::setDefaultAllocator(previous_); }
    DefaultAllocatorScope(const DefaultAllocatorScope&) = delete;
    DefaultAllocatorScope& operator=(const DefaultAllocatorScope&) = delete;
private:
    cv::MatAllocator* previous_;
};

int RunResource(const std::string& model_path, const std::string& label_path,
                const std::filesystem::path& out_dir) {
    std::filesystem::create_directories(out_dir);
    ImageRecognizer recognizer(model_path, label_path);
    cv::Mat extreme(1, 16384, CV_8UC3, cv::Scalar(17, 83, 241));
    BudgetAllocator allocation(cv::Mat::getDefaultAllocator());
    observation = Observation{};
    bool rejected = false;
    std::string reason;
    {
        // Install as the default, not merely on one Mat. OpenCV 4.5.4
        // Mat::create rethrows when a == its default fallback allocator.
        DefaultAllocatorScope scope(&allocation);
        observe_run = true;
        try {
            recognizer.PredictFromMat(extreme);
        } catch (const std::exception& error) {
            rejected = true;
            reason = error.what();
        }
        observe_run = false;
    }
    const bool passed = rejected && allocation.blocked_requests.load() == 0 &&
                        observation.attempted_runs == 0 && observation.completed_runs == 0;
    Json receipt = {{"stage", "image_resource"}, {"status", passed ? "passed" : "failed"},
                    {"failures", passed ? 0 : 1}, {"source_shape", {1, 16384, 3}},
                    {"source_bytes", extreme.total() * extreme.elemSize()},
                    {"expected_resize_shape", {256, 4194304, 3}},
                    {"expected_resize_bytes", size_t{3221225472}},
                    {"allocator_limit_bytes", BudgetAllocator::limit},
                    {"largest_requested_bytes", allocation.largest_requested.load()},
                    {"largest_forwarded_bytes", allocation.largest_forwarded.load()},
                    {"forwarded_requests", allocation.forwarded_requests.load()},
                    {"blocked_oversize_requests", allocation.blocked_requests.load()},
                    {"rejected", rejected}, {"reason", reason},
                    {"run_attempts", observation.attempted_runs},
                    {"real_production_runs", observation.completed_runs},
                    {"onnxruntime", OrtGetApiBase()->GetVersionString()}, {"opencv", CV_VERSION}};
    std::ofstream report(out_dir / "resource.json");
    if (!report) throw std::runtime_error("Could not open resource contract receipt");
    report << receipt.dump(2) << '\n';
    report.close();
    if (!report) throw std::runtime_error("Could not save resource contract receipt");
    std::cout << receipt.dump(2) << '\n';
    return passed ? 0 : 1;
}

std::vector<std::string> LoadLabels(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open real ImageNet labels");
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) if (!line.empty()) result.push_back(line);
    if (result.size() != 1000) throw std::runtime_error("Expected exactly 1000 real ImageNet labels");
    return result;
}

// Independent reference for the ONNX Model Zoo MobileNet contract:
// https://github.com/onnx/models/blob/c5cae0f1942c8d7727372c7b1f6b485aa39e4421/
// validated/vision/classification/imagenet_preprocess.py
std::vector<float> ReferenceInput(const cv::Mat& bgr) {
    const double scale = 256.0 / std::min(bgr.rows, bgr.cols);
    cv::Mat resized;
    cv::resize(bgr, resized,
               cv::Size(static_cast<int>(std::lround(bgr.cols * scale)),
                        static_cast<int>(std::lround(bgr.rows * scale))),
               0.0, 0.0, cv::INTER_LINEAR);
    const int x0 = resized.cols / 2 - 112;
    const int y0 = resized.rows / 2 - 112;
    constexpr double mean[] = {0.485, 0.456, 0.406};
    constexpr double stddev[] = {0.229, 0.224, 0.225};
    std::vector<float> tensor(3 * 224 * 224);
    for (int channel = 0; channel != 3; ++channel) {
        for (int row = 0; row != 224; ++row) {
            for (int col = 0; col != 224; ++col) {
                const auto pixel = resized.at<cv::Vec3b>(y0 + row, x0 + col);
                tensor[channel * 224 * 224 + row * 224 + col] = static_cast<float>(
                    (pixel[2 - channel] / 255.0 - mean[channel]) / stddev[channel]);
            }
        }
    }
    return tensor;
}

cv::Mat Pattern(int rows, int cols) {
    cv::Mat result(rows, cols, CV_8UC3);
    for (int y = 0; y != rows; ++y) {
        for (int x = 0; x != cols; ++x) {
            result.at<cv::Vec3b>(y, x) = {
                static_cast<unsigned char>((3 * x + y) % 256),
                static_cast<unsigned char>((x + 5 * y) % 256),
                static_cast<unsigned char>((7 * x + 2 * y + 31) % 256)};
        }
    }
    return result;
}

struct Top1 {
    size_t index;
    double confidence;
};

Top1 SoftmaxTop1(const std::vector<float>& logits) {
    if (logits.size() != 1000 || !std::all_of(logits.begin(), logits.end(),
            [](float x) { return std::isfinite(x); })) {
        throw std::runtime_error("Real model did not produce 1000 finite logits");
    }
    const auto top = std::max_element(logits.begin(), logits.end());
    double denominator = 0.0;
    for (float value : logits) denominator += std::exp(static_cast<double>(value) - *top);
    return {static_cast<size_t>(top - logits.begin()), 1.0 / denominator};
}

std::vector<float> ReferenceLogits(Ort::Session& session, std::vector<float>& input) {
    const int64_t dims[] = {1, 3, 224, 224};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), dims, 4);
    const char* inputs[] = {"data"};
    const char* outputs[] = {"mobilenetv20_output_flatten0_reshape0"};
    auto values = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    auto info = values[0].GetTensorTypeAndShapeInfo();
    if (info.GetShape() != std::vector<int64_t>{1, 1000}) {
        throw std::runtime_error("Real model output shape differs from official asset");
    }
    const float* data = values[0].GetTensorData<float>();
    return {data, data + 1000};
}

int Run(const std::string& model_path, const std::string& label_path,
        const std::filesystem::path& out_dir, bool oracle_only) {
    const auto labels = LoadLabels(label_path);
    std::filesystem::create_directories(out_dir);
    Ort::Env reference_env(ORT_LOGGING_LEVEL_WARNING, "image-contract-reference");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    Ort::Session reference_session(reference_env, model_path.c_str(), options);
    ImageRecognizer recognizer(model_path, label_path);

    const std::vector<std::pair<std::string, cv::Mat>> cases = {
        {"solid_rgb_channels", cv::Mat(224, 224, CV_8UC3, cv::Scalar(17, 83, 241))},
        {"landscape_resize_crop", Pattern(240, 360)},
        {"portrait_resize_crop", Pattern(360, 240)}};

    Json receipt = {{"schema", 1}, {"onnxruntime", OrtGetApiBase()->GetVersionString()},
                    {"opencv", CV_VERSION}, {"mode", oracle_only ? "oracle" : "contract"},
                    {"input_shape", {1, 3, 224, 224}}, {"cases", Json::array()},
                    {"label_contracts", Json::array()}};
    size_t failures = 0;
    for (size_t i = 0; i != cases.size(); ++i) {
        const auto& name = cases[i].first;
        const auto& image = cases[i].second;
        const std::string filename = name + ".png";
        const auto file_path = out_dir / filename;
        if (!cv::imwrite(file_path.string(), image)) throw std::runtime_error("Could not write PNG fixture");
        auto expected_input = ReferenceInput(image);
        const auto expected = SoftmaxTop1(ReferenceLogits(reference_session, expected_input));
        Json entry = {{"name", name}, {"filename", filename}, {"class_index", expected.index},
                      {"class_name", labels[expected.index]}, {"confidence", expected.confidence}};

        if (!oracle_only) {
            observation = Observation{};
            observe_run = true;
            std::string actual_label;
            try {
                if (i == 0) actual_label = recognizer.PredictFromMat(image);
                if (i == 1) {
                    std::vector<unsigned char> encoded;
                    if (!cv::imencode(".png", image, encoded)) throw std::runtime_error("Could not encode PNG");
                    actual_label = recognizer.PredictFromBuffer(encoded);
                }
                if (i == 2) actual_label = recognizer.PredictFromFile(file_path.string());
            } catch (...) {
                observe_run = false;
                throw;
            }
            observe_run = false;
            if (observation.completed_runs != 1 || observation.input.shape != std::vector<int64_t>{1, 3, 224, 224}
                || observation.output.shape != std::vector<int64_t>{1, 1000}
                || observation.input.values.size() != expected_input.size()) {
                throw std::runtime_error("Observer did not record a complete real production inference");
            }

            double max_error = 0.0;
            size_t first_mismatch = expected_input.size();
            for (size_t j = 0; j != expected_input.size(); ++j) {
                const double error = std::abs(static_cast<double>(observation.input.values[j]) - expected_input[j]);
                max_error = std::max(max_error, error);
                if (error > 1e-5 && first_mismatch == expected_input.size()) first_mismatch = j;
            }
            const bool tensor_matches = max_error <= 1e-5;
            const bool label_matches = actual_label == labels[expected.index];
            entry.update({{"actual_class_name", actual_label}, {"actual_softmax_confidence", SoftmaxTop1(observation.output.values).confidence},
                          {"real_production_runs", observation.completed_runs}, {"tensor_matches", tensor_matches},
                          {"label_matches", label_matches}, {"max_tensor_error", max_error}});
            if (!tensor_matches) {
                entry["first_mismatch"] = {{"offset", first_mismatch}, {"expected", expected_input[first_mismatch]},
                                            {"actual", observation.input.values[first_mismatch]}};
                ++failures;
            }
            if (!label_matches) ++failures;
            if (i < 2) {
                observation = Observation{};
                observe_run = true;
                ImageRecognizer::Prediction scored;
                try {
                    if (i == 0) scored = recognizer.PredictFromMatWithConfidence(image);
                    else {
                        std::vector<unsigned char> encoded;
                        if (!cv::imencode(".png", image, encoded)) throw std::runtime_error("Could not encode scored PNG");
                        scored = recognizer.PredictFromBufferWithConfidence(encoded);
                    }
                } catch (...) {
                    observe_run = false;
                    throw;
                }
                observe_run = false;
                const bool score_matches = observation.completed_runs == 1 &&
                    scored.label == labels[expected.index] && std::isfinite(scored.confidence) &&
                    scored.confidence >= 0.0 && scored.confidence <= 1.0 &&
                    std::abs(scored.confidence - expected.confidence) <= 5e-5;
                entry["confidence_api"] = {{"label", scored.label}, {"confidence", scored.confidence},
                                            {"real_production_runs", observation.completed_runs}, {"matches", score_matches}};
                if (!score_matches) ++failures;
            }
        }
        receipt["cases"].push_back(std::move(entry));
    }
    if (!oracle_only) {
        // Do not execute prediction with excessive labels: the baseline uses
        // labels.size() as a raw output-data bound. Reject the invalid asset
        // pairing before inference instead of reproducing undefined behavior.
        for (const size_t count : {size_t{999}, size_t{1001}}) {
            const auto invalid_path = out_dir / ("image_labels_" + std::to_string(count) + ".txt");
            {
                std::ofstream invalid(invalid_path);
                if (!invalid) throw std::runtime_error("Could not open mismatched labels fixture");
                for (size_t i = 0; i != count; ++i) invalid << labels[i % labels.size()] << '\n';
                invalid.close();
                if (!invalid) throw std::runtime_error("Could not write mismatched labels fixture");
            }
            bool rejected = false;
            std::string reason;
            try {
                ImageRecognizer invalid_pair(model_path, invalid_path.string());
            } catch (const std::exception& error) {
                reason = error.what();
                rejected = reason.find("label") != std::string::npos || reason.find("Label") != std::string::npos;
                if (!rejected) throw;  // An unrelated load failure is not product RED evidence.
            }
            receipt["label_contracts"].push_back({{"labels", count}, {"model_classes", 1000},
                                                  {"rejected_before_prediction", rejected}, {"reason", reason}});
            if (!rejected) ++failures;
        }
    }
    receipt["failures"] = failures;
    receipt["status"] = failures == 0 ? "passed" : "failed";
    std::ofstream report(out_dir / "oracle.json");
    if (!report) throw std::runtime_error("Could not open image contract receipt");
    report << receipt.dump(2) << '\n';
    report.close();
    if (!report) throw std::runtime_error("Could not save image contract receipt");
    std::cout << receipt.dump(2) << '\n';
    return failures == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 || (std::string(argv[4]) != "contract" && std::string(argv[4]) != "oracle" && std::string(argv[4]) != "resource")) {
        std::cerr << "Usage: image_contract MODEL LABELS OUTPUT_DIR contract|oracle|resource\n";
        return 2;
    }
    real_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!real_api) {
        std::cerr << "ONNX Runtime does not provide the compiled C API version\n";
        return 2;
    }
    OrtApi observed_api = *real_api;
    observed_api.Run = ObserveRun;
    Ort::InitApi(&observed_api);
    try {
        if (std::string(argv[4]) == "resource") return RunResource(argv[1], argv[2], argv[3]);
        return Run(argv[1], argv[2], argv[3], std::string(argv[4]) == "oracle");
    } catch (const std::exception& error) {
        std::cerr << Json({{"status", "unavailable"}, {"reason", error.what()}}).dump() << '\n';
        return 2;
    }
}
