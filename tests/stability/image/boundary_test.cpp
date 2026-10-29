#define main original_image_contract_main
#include "../../integration/image_contract.cpp"
#undef main
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sys/resource.h>
namespace {
std::mutex gateMutex;
std::condition_variable gate;
bool holdRuns=false, releaseRuns=false;
std::atomic<int> entered{0}, actualRuns{0};
std::atomic<bool> failRun{false};
OrtStatus* ORT_API_CALL ResourceRun(OrtSession* session,const OrtRunOptions* options,
    const char* const* names,const OrtValue* const* values,size_t count,
    const char* const* outputs,size_t outputCount,OrtValue** results) noexcept {
    if(failRun.exchange(false)) return real_api->CreateStatus(ORT_FAIL,"intentional inference error for release test");
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if(holdRuns) { ++entered; gate.notify_all(); gate.wait(lock,[]{return releaseRuns;}); }
    }
    auto* status=real_api->Run(session,options,names,values,count,outputs,outputCount,results);
    if(!status) ++actualRuns;
    return status;
}
}
int main(int argc, char** argv) {
    if(argc!=4) return 2;
    real_api=OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtApi api=*real_api; api.Run=ResourceRun; Ort::InitApi(&api);
    Json cases=Json::array(); size_t failures=0;
    auto check=[&](const std::string& name,bool pass){cases.push_back({{"name",name},{"pass",pass}});if(!pass)++failures;};
    try {
        ImageRecognizer recognizer(argv[1], argv[2]), other(argv[1],argv[2]);
        auto read=[&](const std::string& name) {std::ifstream f(std::string(argv[3])+"/"+name,std::ios::binary);return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),{});};
        auto data=read("oversize.png"); if(data.empty()) throw std::runtime_error("missing oversize fixture");
        BudgetAllocator allocation(cv::Mat::getDefaultAllocator());
        bool rejected=false; std::string reason;
        { DefaultAllocatorScope scope(&allocation);
          try { recognizer.PredictFromBuffer(data); } catch(const std::exception& e) {rejected=true; reason=e.what();}
        }
        check("oversize_png_before_decoder",rejected && allocation.blocked_requests==0 && actualRuns==0);
        std::cout<<Json({{"reason",reason},{"largest_requested",allocation.largest_requested.load()}, {"blocked",allocation.blocked_requests.load()},{"runs",actualRuns.load()}}).dump()<<'\n';
#ifdef IMAGE_BOUNDARY_FULL
        auto rejects=[&](const std::string& name,auto action,int status) {
            bool ok=false; const int before=actualRuns;
            try {action();} catch(const image_resource::Error& e){ok=e.status==status;} catch(...){ok=status==400;}
            check(name,ok && actualRuns==before);
        };
        for (const auto& name : {"zero.png", "overflow.png"})
            rejects(name,[&]{recognizer.PredictFromBuffer(read(name));},400);
        rejects("file_oversize",[&]{recognizer.PredictFromFile(std::string(argv[3])+"/oversize.png");},413);
        rejects("encoded_budget",[&]{recognizer.PredictFromBuffer(std::vector<unsigned char>(16*1024*1024+1));},413);
        unsigned char pixel=0;cv::Mat external(5000,5000,CV_8UC3,&pixel);
        rejects("mat_original_budget",[&]{recognizer.PredictFromMatWithConfidence(external);},413);
        cv::Mat extreme(1,16384,CV_8UC3,cv::Scalar(1,2,3));
        rejects("extreme_resize_remains400",[&]{recognizer.PredictFromMat(extreme);},400);
        cv::Mat image=Pattern(240,360);
        std::vector<std::pair<std::string,std::vector<unsigned char>>> images;
        for(const bool progressive : {false,true}) {
            std::vector<unsigned char> encoded;
            cv::imencode(".jpg",image,encoded,{cv::IMWRITE_JPEG_PROGRESSIVE,int(progressive)});
            images.push_back({progressive?"jpeg_progressive":"jpeg_baseline",encoded});
        }
        // EXIF little-endian TIFF orientation6: OpenCV must rotate 90 degrees.
        const std::vector<unsigned char> exif={255,225,0,34,'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,1,0,18,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
        auto oriented=images.front().second;
        oriented.insert(oriented.begin()+2,exif.begin(),exif.end());
        auto orientedDecoded=cv::imdecode(oriented,cv::IMREAD_COLOR);
        check("exif6_actual_decoder_transposes",orientedDecoded.rows==image.cols && orientedDecoded.cols==image.rows);
        images.push_back({"jpeg_exif6",oriented});
        cv::Mat gray;cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
        std::vector<unsigned char> jpeg;cv::imencode(".jpg",gray,jpeg);images.push_back({"jpeg_gray",jpeg});
        for(int type : {CV_8UC1,CV_8UC3,CV_8UC4,CV_16UC1,CV_16UC3,CV_16UC4}) {
            cv::Mat png(250,300,type,cv::Scalar::all(47));std::vector<unsigned char> encoded;
            cv::imencode(".png",png,encoded);images.push_back({"png_type_"+std::to_string(type),encoded});
        }
        for(const auto& name:{"palette.png","gray1.png","rgba_interlaced.png"}) images.push_back({name,read(name)});
        for(const auto& entry:images) {
            auto decoded=cv::imdecode(entry.second,cv::IMREAD_COLOR);
            auto expected=recognizer.PredictFromMatWithConfidence(decoded);
            auto actual=recognizer.PredictFromBufferWithConfidence(entry.second);
            check(entry.first,actual.label==expected.label && std::abs(actual.confidence-expected.confidence)<1e-10);
        }
        for(const auto& entry:images) {
            for(size_t len:{size_t(0),size_t(1),size_t(7),entry.second.size()/2,entry.second.size()-2}) {
                auto truncated=entry.second;truncated.resize(len);
                rejects("truncated_"+entry.first+"_"+std::to_string(len),[&]{recognizer.PredictFromBuffer(truncated);},400);
            }
        }
        std::vector<unsigned char> unsupported;cv::imencode(".bmp",image,unsupported);
        rejects("unsupported_bmp",[&]{recognizer.PredictFromBuffer(unsupported);},400);
        auto png=images[4].second;
        for(unsigned i=0;i<32;++i) {
            auto malformed=png;malformed[8+i%8]^=static_cast<unsigned char>(1u<<(i%8));
            rejects("malformed_png_header_"+std::to_string(i),[&]{recognizer.PredictFromBuffer(malformed);},400);
        }
        auto corrupt=png;
        for(size_t pos=8;pos+12<=corrupt.size();) {
            const size_t len=image_resource::BE32(corrupt.data()+pos);
            if(std::string(reinterpret_cast<char*>(corrupt.data()+pos+4),4)=="IDAT") {corrupt[pos+8]^=127;break;}
            pos+=len+12;
        }
        rejects("decoder_failure",[&]{recognizer.PredictFromBuffer(corrupt);},400);
        failRun=true;
        rejects("inference_failure",[&]{recognizer.PredictFromMat(image);},400);
        // Two real public calls occupy the shared quota at the actual Run boundary.
        {std::lock_guard<std::mutex> lock(gateMutex);holdRuns=true;}
        std::exception_ptr failure1,failure2;
        std::thread first([&]{try{recognizer.PredictFromBuffer(images[0].second);}catch(...){failure1=std::current_exception();}});
        std::thread second([&]{try{other.PredictFromMat(image);}catch(...){failure2=std::current_exception();}});
        bool reached=false;
        {std::unique_lock<std::mutex> lock(gateMutex);reached=gate.wait_for(lock,std::chrono::seconds(30),[]{return entered==2;});}
        if(reached) {
            rejects("global_busy_buffer",[&]{other.PredictFromBufferWithConfidence(images[0].second);},503);
            rejects("global_busy_mat",[&]{other.PredictFromMatWithConfidence(image);},503);
            rejects("file_permit_before_read",[&]{other.PredictFromFile("/nonexistent/image");},503);
        }
        {std::lock_guard<std::mutex> lock(gateMutex);releaseRuns=true;gate.notify_all();}
        first.join();second.join();
        {std::lock_guard<std::mutex> lock(gateMutex);holdRuns=false;}
        check("two_distinct_recognizers_real_Run",reached && !failure1 && !failure2);
        check("permits_released_after_all_failures",recognizer.PredictFromMat(image).size()>0);
        // Bounded fuzz corpus against the production preflight, not a duplicate parser.
        for(unsigned i=0;i<256;++i) {
            auto mutated=images[i%images.size()].second;
            mutated[(i*7919u)%mutated.size()]^=static_cast<unsigned char>(1u<<(i%8));
            try {image_resource::Preflight(mutated);}catch(const image_resource::Error&){}
        }
        check("bounded_header_mutation_corpus",true);
#endif
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        Json receipt={{"failures",failures},{"cases",cases},{"real_runs",actualRuns.load()},
            {"opencv",CV_VERSION},{"onnxruntime",OrtGetApiBase()->GetVersionString()},{"max_rss_kib",usage.ru_maxrss}};
        std::ofstream report(std::string(argv[3])+"/boundary.json");report<<receipt.dump(2)<<'\n';
        std::cout<<receipt.dump(2)<<'\n';return failures?1:0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 2;}
}
