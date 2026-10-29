#include "../include/AIUtil/ImageRecognizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

ImageRecognizer::ImageRecognizer(const std::string& model_path,
    const std::string& label_path)
    : env(ORT_LOGGING_LEVEL_WARNING, "ImageRecognizer")
{
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    if (session->GetInputCount() != 1 || session->GetOutputCount() != 1) {
        throw std::runtime_error("Image model must have one input and one output");
    }

    // Keep each owning TypeInfo alive while using its tensor-info view.
    auto input_type = session->GetInputTypeInfo(0);
    auto output_type = session->GetOutputTypeInfo(0);
    if (input_type.GetONNXType() != ONNX_TYPE_TENSOR || output_type.GetONNXType() != ONNX_TYPE_TENSOR) {
        throw std::runtime_error("Image model input and output must be tensors");
    }
    auto input_info = input_type.GetTensorTypeAndShapeInfo();
    auto output_info = output_type.GetTensorTypeAndShapeInfo();
    input_shape = input_info.GetShape();
    if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        input_shape != std::vector<int64_t>{1, 3, 224, 224}) {
        throw std::runtime_error("Image model input must be float32 [1,3,224,224]");
    }
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        output_info.GetShape() != std::vector<int64_t>{1, 1000}) {
        throw std::runtime_error("Image model output must be float32 [1,1000]");
    }
    input_height = static_cast<int>(input_shape[2]);
    input_width = static_cast<int>(input_shape[3]);
    input_name = session->GetInputNameAllocated(0, *allocator).get();
    output_name = session->GetOutputNameAllocated(0, *allocator).get();

    LoadLabels(label_path);
    if (labels.size() != output_info.GetElementCount()) {
        throw std::runtime_error("Image label count does not match model output classes");
    }
}

void ImageRecognizer::LoadLabels(const std::string& label_path) {
    std::ifstream infile(label_path);
    if (!infile.is_open()) {
        throw std::runtime_error("Failed to open label file: " + label_path);
    }
    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) labels.push_back(line);
    }
    if (labels.empty()) {
        throw std::runtime_error("No labels loaded from file: " + label_path);
    }
}

std::string ImageRecognizer::PredictFromFile(const std::string& image_path) {
    image_resource::Permit permit;
    std::ifstream file(image_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Failed to open image file");
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("Failed to determine image file size");
    image_resource::CheckEncoded(static_cast<size_t>(end));
    std::vector<unsigned char> bytes(static_cast<size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || file.peek() != EOF)
        throw std::runtime_error("Image file changed or could not be read completely");
    return PredictBufferGuarded(bytes).label;
}

std::string ImageRecognizer::PredictFromBuffer(const std::vector<unsigned char>& image_data) {
    return PredictFromBufferWithConfidence(image_data).label;
}

std::string ImageRecognizer::PredictFromMat(const cv::Mat& img) {
    return PredictFromMatWithConfidence(img).label;
}

ImageRecognizer::Prediction ImageRecognizer::PredictFromBufferWithConfidence(
    const std::vector<unsigned char>& image_data) {
    image_resource::Permit permit;
    return PredictBufferGuarded(image_data);
}

ImageRecognizer::Prediction ImageRecognizer::PredictBufferGuarded(
    const std::vector<unsigned char>& image_data) {
    const auto shape = image_resource::Preflight(image_data);
    cv::Mat img = cv::imdecode(image_data, cv::IMREAD_COLOR);
    if (img.empty()) throw std::runtime_error("Failed to decode image from buffer");
    // OpenCV applies EXIF orientation, which may exchange width and height.
    if (!((img.cols == static_cast<int>(shape.width) && img.rows == static_cast<int>(shape.height)) ||
          (shape.jpeg && img.rows == static_cast<int>(shape.width) && img.cols == static_cast<int>(shape.height))))
        throw image_resource::Error(400, "Decoded image shape differs from header");
    return PredictMatGuarded(img);
}

ImageRecognizer::Prediction ImageRecognizer::PredictFromMatWithConfidence(const cv::Mat& img_raw) {
    image_resource::Permit permit;
    return PredictMatGuarded(img_raw);
}

ImageRecognizer::Prediction ImageRecognizer::PredictMatGuarded(const cv::Mat& img_raw) {
    if (img_raw.empty()) throw std::runtime_error("Input image is empty");
    if (img_raw.dims != 2 || img_raw.type() != CV_8UC3) {
        throw std::runtime_error("Input image must be a two-dimensional CV_8UC3 BGR image");
    }

    image_resource::CheckShape(static_cast<uint32_t>(img_raw.cols), static_cast<uint32_t>(img_raw.rows));

    // ONNX Model Zoo MobileNetV2: RGB, short side 256, center crop 224,
    // then ImageNet mean/std normalization and NCHW packing.
    const double scale = 256.0 / std::min(img_raw.rows, img_raw.cols);
    const double resized_width = std::round(img_raw.cols * scale);
    const double resized_height = std::round(img_raw.rows * scale);
    if (resized_width > std::numeric_limits<int>::max() || resized_height > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Image dimensions are too large for preprocessing");
    }
    constexpr double max_resized_pixels = 16.0 * 1024.0 * 1024.0;
    if (resized_width > max_resized_pixels / resized_height) {
        throw std::runtime_error("Image preprocessing exceeds the resized pixel resource budget");
    }
    cv::Mat rgb;
    cv::cvtColor(img_raw, rgb, cv::COLOR_BGR2RGB);
    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(static_cast<int>(resized_width), static_cast<int>(resized_height)),
               0.0, 0.0, cv::INTER_LINEAR);
    const cv::Rect center(resized.cols / 2 - input_width / 2,
                          resized.rows / 2 - input_height / 2, input_width, input_height);
    cv::Mat normalized;
    resized(center).convertTo(normalized, CV_32F, 1.0 / 255.0);
    cv::Mat channels[3];
    cv::split(normalized, channels);
    constexpr double mean[] = {0.485, 0.456, 0.406};
    constexpr double stddev[] = {0.229, 0.224, 0.225};
    for (int channel = 0; channel != 3; ++channel) {
        channels[channel] = (channels[channel] - mean[channel]) / stddev[channel];
    }
    cv::merge(channels, 3, normalized);
    cv::Mat blob;
    cv::dnn::blobFromImage(normalized, blob);

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, blob.ptr<float>(),
        blob.total(), input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name.c_str()};
    const char* output_names[] = {output_name.c_str()};
    auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);
    if (outputs.size() != 1 || !outputs[0].IsTensor()) {
        throw std::runtime_error("Image model returned an invalid output tensor");
    }
    auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
    const size_t count = output_info.GetElementCount();
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        output_info.GetShape() != std::vector<int64_t>{1, 1000} || count != labels.size()) {
        throw std::runtime_error("Image output shape or label count does not match model contract");
    }
    const float* logits = outputs[0].GetTensorData<float>();
    if (!std::all_of(logits, logits + count, [](float value) { return std::isfinite(value); })) {
        throw std::runtime_error("Image model returned non-finite logits");
    }
    const float* top = std::max_element(logits, logits + count);
    double denominator = 0.0;
    for (size_t i = 0; i != count; ++i) {
        denominator += std::exp(static_cast<double>(logits[i]) - *top);
    }
    return {labels[static_cast<size_t>(top - logits)], 1.0 / denominator};
}
