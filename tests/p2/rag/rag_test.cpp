#include "AIUtil/AIStrategy.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <cstdlib>
#include <vector>
using CURLcode=int;
constexpr int CURLE_OK=0,CURLE_WRITE_ERROR=23;
constexpr int CURLOPT_URL=1,CURLOPT_HTTPHEADER=2,CURLOPT_POSTFIELDS=3,CURLOPT_WRITEFUNCTION=4,CURLOPT_WRITEDATA=5,CURLINFO_RESPONSE_CODE=6;
using Write=size_t(*)(void*,size_t,size_t,void*);
struct curl_slist{std::vector<std::string> values;};
struct CURL{Write write=nullptr;void* data=nullptr;};
std::vector<std::string> feed,seen_headers;
std::vector<CURL*> handles;
std::vector<curl_slist*> lists;
int append_calls=0,fail_append_at=0,perform_calls=0;
std::string seen_url,seen_payload;
int allocations=0;long http_status=200;CURLcode transport_status=CURLE_OK;
CURL* curl_easy_init(){++allocations;auto* c=new CURL;handles.push_back(c);return c;}
void curl_easy_cleanup(CURL* c){handles.erase(std::find(handles.begin(),handles.end(),c));delete c;}
curl_slist* curl_slist_append(curl_slist* list,const char* text){
 if(++append_calls==fail_append_at)return nullptr;
 if(!list){list=new curl_slist;lists.push_back(list);}
 list->values.push_back(text);return list;
}
void curl_slist_free_all(curl_slist* list){if(list){lists.erase(std::find(lists.begin(),lists.end(),list));delete list;}}
const char* curl_easy_strerror(CURLcode){return "fixture transport error";}
template<class T>CURLcode curl_easy_setopt(CURL* c,int key,T value){
 if constexpr(std::is_convertible_v<T,Write>){if(key==CURLOPT_WRITEFUNCTION)c->write=value;}
 else if constexpr(std::is_same_v<T,curl_slist*>){if(key==CURLOPT_HTTPHEADER)seen_headers=value?value->values:std::vector<std::string>{};}
 else if constexpr(std::is_convertible_v<T,const char*>){if(key==CURLOPT_URL)seen_url=value;if(key==CURLOPT_POSTFIELDS)seen_payload=value;}
 else if constexpr(std::is_pointer_v<T>){if(key==CURLOPT_WRITEDATA)c->data=value;}
 return CURLE_OK;
}
CURLcode curl_easy_getinfo(CURL*,int,long* value){*value=http_status;return CURLE_OK;}
CURLcode curl_easy_perform(CURL* c){++perform_calls;for(auto part:feed){if(c->write(part.data(),1,part.size(),c->data)!=part.size())return CURLE_WRITE_ERROR;}return transport_status;}
class AIHelper{std::shared_ptr<AIStrategy> strategy;public:
 explicit AIHelper(std::shared_ptr<AIStrategy> s):strategy(std::move(s)){}
 json executeCurl(const json&);
 void executeCurlStream(const json&,std::function<void(const std::string&)>,std::function<void()>,std::function<void(const std::string&)>);
 static size_t WriteCallback(void*,size_t,size_t,void*);
};
// @CURL_STREAM@
// @CURL_SYNC@
// @WRITE_CALLBACK@

int main(){
 int failures=0;
 auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';failures+=!ok;};
 setenv("DASHSCOPE_API_KEY","fixture-key",1);unsetenv("Knowledge_Base_ID");setenv("KNOWLEDGE_BASE_ID","canonical-app",1);
 auto rag=std::make_shared<AliyunRAGStrategy>();
 auto url=[&](){try{return rag->getApiUrl();}catch(...){return std::string{};}};
 check(url()=="https://dashscope.aliyuncs.com/api/v1/apps/canonical-app/completion","canonical RAG application environment");
 setenv("Knowledge_Base_ID","legacy-app",1);check(url().find("canonical-app")!=std::string::npos,"canonical variable wins over legacy");
 unsetenv("KNOWLEDGE_BASE_ID");check(url().find("legacy-app")!=std::string::npos,"legacy variable remains compatible");
 setenv("KNOWLEDGE_BASE_ID","",1);check(url().empty(),"empty canonical app ID fails closed");
 setenv("KNOWLEDGE_BASE_ID","bad/app?x=1",1);check(url().empty(),"application ID cannot modify URL path");
 setenv("KNOWLEDGE_BASE_ID","canonical-app",1);setenv("Knowledge_Base_ID","canonical-app",1);
 auto payload=rag->buildStreamRequest({{"question",1}});
 check(payload.contains("parameters")&&payload["parameters"].value("incremental_output",false),"RAG requests incremental output in parameters");
 check(!payload.contains("stream"),"HTTP streaming uses provider header instead of SDK stream field");
 auto stream=[&](std::vector<std::string> chunks,bool expected_success,std::string expected,const char* name){
  feed=std::move(chunks);seen_headers.clear();http_status=200;transport_status=CURLE_OK;
  AIHelper helper(rag);int done=0,error=0;std::string output;
  try{helper.executeCurlStream(payload,[&](const std::string& s){output+=s;},[&]{++done;},[&](const std::string&){++error;});}catch(...){++error;}
  check(output==expected&&done==(expected_success?1:0)&&error==(expected_success?0:1),name);
 };
 const std::string first="id:1\r\nevent:result\r\ndata:{\"output\":{\"text\":\"第一\",\"finish_reason\":\"null\"}}\r\n\r\n";
 const std::string last="data:{\"output\":{\"text\":\"第二\",\"finish_reason\":\"stop\"}}\n\n";
 stream({first.substr(0,23),first.substr(23),last},true,"第一第二","fragmented RAG stop frame completes with final content");
 check(std::find(seen_headers.begin(),seen_headers.end(),"X-DashScope-SSE: enable")!=seen_headers.end(),"HTTP enables DashScope SSE header");
 stream({first},false,"第一","RAG clean EOF without terminal reason rejects");
 stream({first,"data:{\"code\":\"QuotaExceeded\",\"message\":\"fixture\"}\n\n"},false,"第一","RAG provider code error rejects");
 stream({first,"data:{\"output\":{\"text\":\"\",\"finish_reason\":\"length\"}}\n\n"},false,"第一","truncated finish reason rejects");
 stream({"data:{\"output\":{\"text\":\"完整\",\"finish_reason\":null}}\n\n","data:{\"output\":{\"finish_reason\":\"stop\"}}\n\n"},true,"完整","empty final RAG frame still completes once");
 stream({first,"data:{\"output\":{\"text\":7,\"finish_reason\":\"stop\"}}\n\n"},false,"第一","numeric final output text rejects instead of completing partial answer");
 stream({first,"data:{\"output\":{\"text\":{},\"finish_reason\":\"null\"}}\n\n",last},false,"第一","malformed intermediate output text rejects before later stop");
 auto base=std::make_shared<AliyunStrategy>();AIHelper generic(base);feed={"data: {\"choices\":[{\"delta\":{\"content\":\"cloud\"}}]}\n\n","data: [DONE]\n\n"};
 int done=0,error=0;std::string answer;generic.executeCurlStream(base->buildStreamRequest({{"q",0}}),[&](const auto& s){answer+=s;},[&]{++done;},[&](const auto&){++error;});
 check(answer=="cloud"&&done==1&&error==0,"OpenAI-compatible stream behavior preserved");
 unsetenv("KNOWLEDGE_BASE_ID");unsetenv("Knowledge_Base_ID");int before=allocations;
 try{AIHelper helper(rag);helper.executeCurlStream(payload,{}, {},{});}catch(...){}
 check(before==allocations,"invalid RAG configuration rejected before curl allocation");
 setenv("KNOWLEDGE_BASE_ID","canonical-app",1);setenv("Knowledge_Base_ID","canonical-app",1);http_status=503;feed={"{\"output\":{\"text\":\"misleading answer\"}}"};bool rejected=false;
 try{AIHelper helper(rag);helper.executeCurl(rag->buildRequest({{"q",0}}));}catch(...){rejected=true;}
 check(rejected,"nonstream HTTP errors cannot become successful model responses");
 http_status=200;feed={"{\"code\":\"QuotaExceeded\",\"message\":\"fixture upstream detail\"}"};
 std::string failure;try{AIHelper helper(rag);helper.executeCurl(rag->buildRequest({{"q",0}}));}catch(const std::exception& e){failure=e.what();}
 check(failure=="Upstream model reported an error","application errors preserve failure classification without raw upstream body");
 bool malformed_safe=false;std::string text="stale";try{malformed_safe=!rag->parseStreamChunk("malformed",text)&&text.empty();}catch(...){}
 check(malformed_safe,"RAG chunk parser rejects malformed data without stale text");
 for(int fail_at=1;fail_at<=4;++fail_at){
  append_calls=0;fail_append_at=fail_at;http_status=200;transport_status=CURLE_OK;feed={last};
  int done_count=0,error_count=0,before_perform=perform_calls;
  try{AIHelper helper(rag);helper.executeCurlStream(payload,{},[&]{++done_count;},[&](const auto&){++error_count;});}catch(...){++error_count;}
  std::string name="stream header allocation failure "+std::to_string(fail_at)+" rejects before transfer and releases resources";
  bool resource_ok=done_count==0&&error_count==1&&perform_calls==before_perform&&handles.empty()&&lists.empty();
  if(!resource_ok)std::cerr<<"RESOURCE_STREAM append="<<fail_at<<" transfers="<<(perform_calls-before_perform)<<" handles="<<handles.size()<<" lists="<<lists.size()<<" done="<<done_count<<" errors="<<error_count<<'\n';
  check(resource_ok,name.c_str());
  for(auto* list:lists)delete list;lists.clear();
  for(auto* c:handles)delete c;handles.clear();fail_append_at=0;
 }
 for(int fail_at=1;fail_at<=2;++fail_at){
  append_calls=0;fail_append_at=fail_at;http_status=200;transport_status=CURLE_OK;
  feed={"{\"output\":{\"text\":\"fixture answer\",\"finish_reason\":\"stop\"}}"};
  bool rejected_allocation=false;int before_perform=perform_calls;
  try{AIHelper helper(rag);helper.executeCurl(rag->buildRequest({{"q",0}}));}catch(...){rejected_allocation=true;}
  std::string name="sync header allocation failure "+std::to_string(fail_at)+" rejects before transfer and releases resources";
  bool resource_ok=rejected_allocation&&perform_calls==before_perform&&handles.empty()&&lists.empty();
  if(!resource_ok)std::cerr<<"RESOURCE_SYNC append="<<fail_at<<" transfers="<<(perform_calls-before_perform)<<" handles="<<handles.size()<<" lists="<<lists.size()<<" rejected="<<rejected_allocation<<'\n';
  check(resource_ok,name.c_str());
  for(auto* list:lists)delete list;lists.clear();
  for(auto* c:handles)delete c;handles.clear();fail_append_at=0;
 }
 for(auto* c:handles)delete c;handles.clear();
 std::cout<<"RAG_FAILURES="<<failures<<'\n';return failures?1:0;
}
