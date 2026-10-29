#include "../include/handlers/AIUploadSendHandler.h"


void AIUploadSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        int userId = std::stoi(session->getValue("userId"));
        auto body = req.getBody();
        std::string filename;
        std::string imageBase64;
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("filename")) filename = j["filename"];
            if (j.contains("image")) imageBase64 = j["image"];
        }
        if (imageBase64.empty())
        {
            throw std::runtime_error("No image data provided");
        }

        const size_t maxEncoded = image_resource::GetPolicy().encoded;
        if (imageBase64.size() > ((maxEncoded + 2) / 3) * 4)
            throw image_resource::Error(413, "Encoded image exceeds resource budget");
        std::string decodedData = base64_decode(imageBase64);
        std::vector<uchar> imgData(decodedData.begin(), decodedData.end());

        // Header preflight is cheap and precedes per-user model construction.
        image_resource::Preflight(imgData);
        std::shared_ptr<ImageRecognizer> ImageRecognizerPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForImageRecognizerMap);
            if (server_->ImageRecognizerMap.find(userId) == server_->ImageRecognizerMap.end()) {
                server_->ImageRecognizerMap.emplace(userId,
                    std::make_shared<ImageRecognizer>("/root/models/mobilenetv2/mobilenetv2-7.onnx"));
            }
            ImageRecognizerPtr = server_->ImageRecognizerMap[userId];
        }
        const auto prediction = ImageRecognizerPtr->PredictFromBufferWithConfidence(imgData);


        json successResp;
        successResp["success"] = "ok";
        successResp["filename"] = filename;
        successResp["class_name"] = prediction.label;

        successResp["confidence"] = prediction.confidence;


        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
        return;

    }
    catch (const std::exception& e)
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        const auto* resourceError = dynamic_cast<const image_resource::Error*>(&e);
        const int status = resourceError ? resourceError->status : 400;
        const auto code = status == 413 ? http::HttpResponse::k413PayloadTooLarge :
            status == 503 ? http::HttpResponse::k503ServiceUnavailable : http::HttpResponse::k400BadRequest;
        resp->setVersion(req.getVersion());
        resp->setStatusCode(code);
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}



