#include "../../../include/middleware/cors/CorsMiddleware.h"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <muduo/base/Logging.h>

namespace http 
{
namespace middleware 
{

CorsMiddleware::CorsMiddleware(const CorsConfig& config) : config_(config) {}

void CorsMiddleware::before(HttpRequest& request) 
{
    LOG_DEBUG << "CorsMiddleware::before - Processing request";
    
    //判断是不是 OPTIONS 请求
    if (request.method() == HttpRequest::Method::kOptions) 
    {
        LOG_INFO << "Processing CORS preflight request";
        HttpResponse response;
        // 给这个响应加上跨域头
        handlePreflightRequest(request, response);
         // 直接抛出响应 → 停止执行，直接返回给浏览器
        throw response;
    }
}

void CorsMiddleware::after(HttpResponse& response) 
{
    LOG_DEBUG << "CorsMiddleware::after - Processing response";
    
    // 直接添加CORS头，简化处理逻辑
    if (!config_.allowedOrigins.empty()) 
    {
        // 如果允许所有源
        if (std::find(config_.allowedOrigins.begin(), config_.allowedOrigins.end(), "*") 
            != config_.allowedOrigins.end()) 
        {
            addCorsHeaders(response, "*");
        } 
        else 
        {
            // 添加第一个允许的源
            addCorsHeaders(response, config_.allowedOrigins[0]);
        }
    }
}

bool CorsMiddleware::isOriginAllowed(const std::string& origin) const 
{
    return config_.allowedOrigins.empty() || 
           std::find(config_.allowedOrigins.begin(), 
                    config_.allowedOrigins.end(), "*") != config_.allowedOrigins.end() ||
           std::find(config_.allowedOrigins.begin(), 
                    config_.allowedOrigins.end(), origin) != config_.allowedOrigins.end();
}

//专门处理 CORS 预检请求（OPTIONS 请求），判断请求的来源是否被允许，并生成对应的响应
void CorsMiddleware::handlePreflightRequest(const HttpRequest& request, 
                                          HttpResponse& response) 
{
    //获取请求源标识
    const std::string& origin = request.getHeader("Origin");
    
    //校验请求源合法性
    if (!isOriginAllowed(origin)) 
    {
        LOG_WARN << "Origin not allowed: " << origin;
        response.setStatusCode(HttpResponse::k403Forbidden);
        return;
    }

    //写入 CORS 响应头
    addCorsHeaders(response, origin);
    //设置预检请求标准状态码
    response.setStatusCode(HttpResponse::k204NoContent);
    LOG_INFO << "Preflight request processed successfully";
}

//为响应对象添加全套 CORS 响应头
void CorsMiddleware::addCorsHeaders(HttpResponse& response, 
                                  const std::string& origin) 
{
    try 
    {
        // 【核心】告诉浏览器：允许哪个源（域名）读取本响应
        response.addHeader("Access-Control-Allow-Origin", origin);
        
        // 如果配置允许携带凭证（Cookie、Authorization 等），则添加对应头
        if (config_.allowCredentials) 
        {
            response.addHeader("Access-Control-Allow-Credentials", "true");
        }
        
        // 如果配置了允许的 HTTP 方法列表，拼接后告知浏览器
        if (!config_.allowedMethods.empty()) 
        {
            response.addHeader("Access-Control-Allow-Methods", 
                             join(config_.allowedMethods, ", "));
        }
        
        // 如果配置了允许的请求头列表，拼接后告知浏览器
        if (!config_.allowedHeaders.empty()) 
        {
            response.addHeader("Access-Control-Allow-Headers", 
                             join(config_.allowedHeaders, ", "));
        }
        
        // 设置预检结果缓存时间（秒），避免浏览器频繁发送 OPTIONS 请求
        response.addHeader("Access-Control-Max-Age", 
                          std::to_string(config_.maxAge));
        
        LOG_DEBUG << "CORS headers added successfully";
    } 
    catch (const std::exception& e) 
    {
        // 添加头部过程中出现任何异常都会被捕获，避免影响正常响应流程
        LOG_ERROR << "Error adding CORS headers: " << e.what();
    }
}

// 工具函数：将字符串数组连接成单个字符串
std::string CorsMiddleware::join(const std::vector<std::string>& strings, const std::string& delimiter) 
{
    std::ostringstream result;
    for (size_t i = 0; i < strings.size(); ++i) 
    {
        if (i > 0) result << delimiter;
        result << strings[i];
    }
    return result.str();
}

} // namespace middleware
} // namespace http