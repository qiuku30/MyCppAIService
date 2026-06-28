#pragma once

#include <string>
#include <vector>

namespace http 
{
namespace middleware 
{

    //CorsConfig = 跨域的 “权限配置表”  专门控制：谁能访问、能干嘛、能带什么信息
struct CorsConfig 
{
    std::vector<std::string> allowedOrigins;
    std::vector<std::string> allowedMethods;
    std::vector<std::string> allowedHeaders;
    bool allowCredentials = false;
    int maxAge = 3600;
    
    static CorsConfig defaultConfig() 
    {
        CorsConfig config;
        config.allowedOrigins = {"*"};
        config.allowedMethods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"};
        config.allowedHeaders = {"Content-Type", "Authorization"};
        return config;
    }
};

} // namespace middleware
} // namespace http