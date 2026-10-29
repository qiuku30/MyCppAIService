#pragma once
#include <string>
#include <type_traits>
#include <vector>
struct CURL { size_t (*write)(void*, size_t, size_t, void*) = nullptr; void* data = nullptr; };
struct curl_slist {};
using CURLcode = int;
inline constexpr CURLcode CURLE_OK = 0;
enum { CURLOPT_URL, CURLOPT_CUSTOMREQUEST, CURLOPT_FOLLOWLOCATION, CURLOPT_DEFAULT_PROTOCOL,
       CURLOPT_HTTPHEADER, CURLOPT_POSTFIELDS, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, CURLOPT_POST };
inline std::vector<std::string> curlResponses;
inline CURL* curl_easy_init() { return new CURL; }
template<class T> inline void curl_easy_setopt(CURL* curl, int option, T value) {
    if constexpr (std::is_same_v<T, decltype(CURL::write)>) { if (option == CURLOPT_WRITEFUNCTION) curl->write = value; }
    else if constexpr (std::is_convertible_v<T, void*>) { if (option == CURLOPT_WRITEDATA) curl->data = value; }
}
inline CURLcode curl_easy_perform(CURL* curl) {
    const auto text = curlResponses.front(); curlResponses.erase(curlResponses.begin());
    curl->write(const_cast<char*>(text.data()), 1, text.size(), curl->data); return CURLE_OK;
}
inline void curl_easy_cleanup(CURL* curl) { delete curl; }
inline curl_slist* curl_slist_append(curl_slist*, const char*) { return nullptr; }
inline void curl_slist_free_all(curl_slist*) {}
