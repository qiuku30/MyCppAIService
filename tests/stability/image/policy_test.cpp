#include "AIUtil/ImageResource.h"
#include <iostream>
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"defaults";
    try {
        const auto& p=image_resource::GetPolicy();
        if(mode=="invalid") return 1;
        if(mode=="defaults") return p.encoded==16777216 && p.pixels==16777216 && p.bgr==67108864 && p.jobs==2 ? 0:1;
        if(mode=="bgr") {
            try{image_resource::CheckShape(2,2);}catch(const image_resource::Error& e){return e.status==413?0:1;}
            return 1;
        }
        if(mode=="quota") {
            image_resource::Permit first;
            try{image_resource::Permit second;}catch(const image_resource::Error& e){return e.status==503?0:1;}
            return 1;
        }
        return 2;
    } catch(const std::runtime_error&) {return mode=="invalid"?0:2;}
}
