#pragma once
#include "ImageResource.h"
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

class ImageRecognizer {
public:
    struct Prediction {
        std::string label;
        // Top-1 softmax probability over all model logits; not calibrated accuracy.
        double confidence = 0.0;
    };

    // One float32 [1,3,224,224] input, one float32 [1,1000] output,
    // and one label for each output class are required.
    explicit ImageRecognizer(const std::string& model_path,
        const std::string& label_path = "/root/imagenet_classes.txt");

    std::string PredictFromFile(const std::string& image_path);
    std::string PredictFromBuffer(const std::vector<unsigned char>& image_data);
    std::string PredictFromMat(const cv::Mat& img);

    Prediction PredictFromBufferWithConfidence(const std::vector<unsigned char>& image_data);
    // Mat input must be nonempty, two-dimensional, 8-bit BGR (CV_8UC3).
    Prediction PredictFromMatWithConfidence(const cv::Mat& img);

private:
    Prediction PredictBufferGuarded(const std::vector<unsigned char>& image_data);
    Prediction PredictMatGuarded(const cv::Mat& img);
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

    std::string input_name;
    std::string output_name;
    std::vector<int64_t> input_shape;
    int input_height{}, input_width{};

    std::vector<std::string> labels;
    void LoadLabels(const std::string& label_path);
};
