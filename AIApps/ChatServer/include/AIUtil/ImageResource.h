#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Process policy is read once on first use. Configure the environment before
// starting the service; invalid values fail closed. BGR bytes are a projected
// output-buffer bound, NOT a bound on codec internals, model memory or RSS.
namespace image_resource {
class Error : public std::runtime_error {
public:
    Error(int status, const char* message) : std::runtime_error(message), status(status) {}
    const int status;
};
struct Policy {
    size_t encoded = 16u*1024u*1024u;
    size_t pixels = 16u*1024u*1024u;
    size_t bgr = 64u*1024u*1024u;
    size_t jobs = 2;
};
inline size_t Setting(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    size_t result=0;
    if (!*value) throw std::runtime_error("Empty image resource configuration");
    for (; *value; ++value) {
        if (*value<'0' || *value>'9' || result>(std::numeric_limits<size_t>::max()-(*value-'0'))/10)
            throw std::runtime_error("Invalid image resource configuration");
        result=result*10+(*value-'0');
    }
    if (!result) throw std::runtime_error("Zero image resource configuration");
    return result;
}
inline const Policy& GetPolicy() {
    static const Policy policy=[] {
        Policy p;
        p.encoded=Setting("IMAGE_MAX_ENCODED_BYTES",p.encoded);
        p.pixels=Setting("IMAGE_MAX_ORIGINAL_PIXELS",p.pixels);
        p.bgr=Setting("IMAGE_MAX_BGR_BYTES",p.bgr);
        p.jobs=Setting("IMAGE_MAX_CONCURRENT_JOBS",p.jobs);
        if (p.encoded>static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Image encoded limit exceeds decoder representation");
        return p;
    }();
    return policy;
}
inline std::atomic<size_t>& ActiveJobs() { static std::atomic<size_t> active{0}; return active; }
class Permit {
public:
    Permit() {
        const size_t limit=GetPolicy().jobs;
        auto& active=ActiveJobs();
        size_t current=active.load();
        do { if (current>=limit) throw Error(503,"Image processing is busy; retry later"); }
        while (!active.compare_exchange_weak(current,current+1));
    }
    ~Permit() { ActiveJobs().fetch_sub(1); }
    Permit(const Permit&)=delete;
    Permit& operator=(const Permit&)=delete;
};
struct Shape { uint32_t width, height; bool jpeg = false; };
inline void CheckEncoded(size_t size) {
    if(size>GetPolicy().encoded) throw Error(413,"Encoded image exceeds resource budget");
    if(!size) throw Error(400,"Image data is empty");
}
inline void CheckShape(uint32_t width,uint32_t height) {
    if(!width || !height || width>static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        height>static_cast<uint32_t>(std::numeric_limits<int>::max()))
        throw Error(400,"Invalid image dimensions");
    const auto& p=GetPolicy();
    if (width>p.pixels/height || width>p.bgr/3/height)
        throw Error(413,"Original decoded image exceeds resource budget");
}
inline uint32_t BE32(const unsigned char* p) {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
inline uint32_t CRC(const unsigned char* p,size_t n) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<n;++i) {
        crc^=p[i];
        for(int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return crc^0xffffffffu;
}
inline Shape Preflight(const std::vector<unsigned char>& bytes) {
    CheckEncoded(bytes.size());
    const auto* p=bytes.data(); const size_t n=bytes.size();
    const unsigned char png[]={137,80,78,71,13,10,26,10};
    if(n>=8 && std::equal(png,png+8,p)) {
        if(n<33 || BE32(p+8)!=13 || std::string(reinterpret_cast<const char*>(p+12),4)!="IHDR" ||
           CRC(p+12,17)!=BE32(p+29)) throw Error(400,"Invalid PNG header");
        Shape shape{BE32(p+16),BE32(p+20)};
        const int depth=p[24],color=p[25];
        const bool valid=(color==0 && (depth==1||depth==2||depth==4||depth==8||depth==16)) ||
            (color==3 && (depth==1||depth==2||depth==4||depth==8)) ||
            ((color==2||color==4||color==6) && (depth==8||depth==16));
        if(!valid || p[26] || p[27] || p[28]>1) throw Error(400,"Unsupported PNG header");
        CheckShape(shape.width,shape.height);
        bool data=false;
        for(size_t pos=33;pos<n;) {
            if(n-pos<12) throw Error(400,"Truncated PNG chunk");
            const size_t len=BE32(p+pos);
            if(len>n-pos-12) throw Error(400,"Invalid PNG chunk length");
            const std::string type(reinterpret_cast<const char*>(p+pos+4),4);
            if(type=="IHDR") throw Error(400,"Duplicate PNG header");
            if(type=="IDAT") data=true;
            if(type=="IEND") {
                if(len || !data || pos+12!=n) throw Error(400,"Invalid PNG end");
                return shape;
            }
            pos+=len+12;
        }
        throw Error(400,"Truncated PNG image");
    }
    if(n>=2 && p[0]==255 && p[1]==216) {
        Shape shape{}; bool frame=false, scan=false; size_t pos=2;
        while(pos<n) {
            if(p[pos++]!=255) throw Error(400,"Invalid JPEG marker");
            while(pos<n && p[pos]==255) ++pos;
            if(pos==n) break;
            const unsigned marker=p[pos++];
            if(marker==217) { if(frame && scan) return shape; break; }
            if(marker==0 || marker==216 || (marker>=208 && marker<=215)) throw Error(400,"Invalid JPEG marker sequence");
            if(n-pos<2) break;
            const size_t len=(size_t(p[pos])<<8)|p[pos+1];
            if(len<2 || len>n-pos) throw Error(400,"Invalid JPEG segment length");
            if((marker>=192 && marker<=207) && marker!=196 && marker!=200 && marker!=204) {
                if((marker!=192 && marker!=193 && marker!=194) || frame || len<8 || p[pos+2]!=8 ||
                   (p[pos+7]!=1 && p[pos+7]!=3 && p[pos+7]!=4) || len!=size_t(8+3*p[pos+7]))
                    throw Error(400,"Unsupported JPEG frame");
                shape={uint32_t((unsigned(p[pos+5])<<8)|p[pos+6]), uint32_t((unsigned(p[pos+3])<<8)|p[pos+4])};
                shape.jpeg=true;
                CheckShape(shape.width,shape.height); frame=true;
            }
            if(marker==218) {
                if(!frame || len<6 || len!=size_t(6+2*p[pos+2])) throw Error(400,"Invalid JPEG scan");
                scan=true; pos+=len;
                while(pos<n) {
                    if(p[pos]!=255) {++pos;continue;}
                    size_t next=pos+1;
                    while(next<n && p[next]==255) ++next;
                    if(next==n) {pos=n;break;}
                    if(p[next]==0 || (p[next]>=208 && p[next]<=215)) {pos=next+1;continue;}
                    break;
                }
            } else pos+=len;
        }
        throw Error(400,"Truncated JPEG image");
    }
    throw Error(400,"Only JPEG and PNG images are supported");
}
} // namespace image_resource
