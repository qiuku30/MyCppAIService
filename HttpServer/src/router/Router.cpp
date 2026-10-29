#include "../../include/router/Router.h"
#include <muduo/base/Logging.h>

namespace http
{
namespace router
{

void Router::registerHandler(HttpRequest::Method method, const std::string &path, HandlerPtr handler)
{
    RouteKey key{method, path};
    handlers_[key] = std::move(handler);
}

void Router::registerCallback(HttpRequest::Method method, const std::string &path, const HandlerCallback &callback)
{
    RouteKey key{method, path};
    callbacks_[key] = std::move(callback);
}

bool Router::route(const HttpRequest &req, HttpResponse *resp)
{
    RouteKey key{req.method(), req.path()};

    // 查找处理器
    auto handlerIt = handlers_.find(key);
    if (handlerIt != handlers_.end())
    {
        resp->setLogRoute(handlerIt->first.path);
        handlerIt->second->handle(req, resp);
        return true;
    }

    // 查找回调函数
    auto callbackIt = callbacks_.find(key);
    if (callbackIt != callbacks_.end())
    {
        resp->setLogRoute(callbackIt->first.path);
        callbackIt->second(req, resp);
        return true;
    }

    // 查找动态路由处理器
    for (size_t index = 0; index < regexHandlers_.size(); ++index)
    {
        const auto &[method, pathRegex, handler] = regexHandlers_[index];
        std::smatch match;
        std::string pathStr(req.path());
        // 如果方法匹配并且动态路由匹配，则执行处理器
        if (method == req.method() && std::regex_match(pathStr, match, pathRegex))
        {
            // Extract path parameters and add them to the request
            HttpRequest newReq(req); // 因为这里需要用这一次所以是可以改的
            extractPathParameters(match, newReq);
            
            resp->setLogRoute(regexHandlerLabels_[index]);
            handler->handle(newReq, resp);
            return true;
        }
    }

    // 查找动态路由回调函数
    for (size_t index = 0; index < regexCallbacks_.size(); ++index)
    {
        const auto &[method, pathRegex, callback] = regexCallbacks_[index];
        std::smatch match;
        std::string pathStr(req.path());
        // 如果方法匹配并且动态路由匹配，则执行回调函数
        if (method == req.method() && std::regex_match(pathStr, match, pathRegex))
        {
             // Extract path parameters and add them to the request
            HttpRequest newReq(req); // 因为这里需要用这一次所以是可以改的
            extractPathParameters(match, newReq);

            resp->setLogRoute(regexCallbackLabels_[index]);
            callback(newReq, resp);  // 修复：传注入了路径参数的 newReq
            return true;
        }
    }

    return false;
}

} // namespace router
} // namespace http