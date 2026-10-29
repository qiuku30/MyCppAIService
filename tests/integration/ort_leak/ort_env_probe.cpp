#include <onnxruntime_cxx_api.h>
#include <cstdio>

int main() {
    // Repeated lifetimes exercise initialization and destruction; LSan runs
    // naturally at process exit and must remain enabled by the caller.
    for (int i = 0; i != 10; ++i) {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ort-env-ownership-probe");
    }
    std::fprintf(stderr, "10 Ort::Env lifetimes completed; runtime=%s\n",
                 OrtGetApiBase()->GetVersionString());
}
