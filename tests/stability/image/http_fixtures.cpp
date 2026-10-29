// Generate bounded, synthetic input files with the same real OpenCV as the app.
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    cv::Mat image(64, 64, CV_8UC3);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 3, y * 3, (x + y) * 2);
        }
    }
    auto write = [&](const char* name, const std::vector<int>& options = {}) {
        if (!cv::imwrite((out / name).string(), image, options)) {
            throw std::runtime_error("Synthetic image encoding failed");
        }
    };
    write("normal.png");
    write("baseline.jpg");
    write("progressive.jpg", {cv::IMWRITE_JPEG_PROGRESSIVE, 1});
    write("unsupported.bmp");
}
