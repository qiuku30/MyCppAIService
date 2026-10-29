#include <cpuinfo.h>
#include <cstdio>

extern "C" const cpuinfo_processor* cpuinfo_probe_initialize(
    uint32_t* cores, uint32_t* uarchs);

int main() {
    uint32_t cores = 0, uarchs = 0;
    const auto* processor = cpuinfo_probe_initialize(&cores, &uarchs);
    if (!processor || !processor->core || !cores || !uarchs) return 2;
    std::printf("Real cpuinfo initialized: cores=%u uarchs=%u first_uarch=%u\n",
                cores, uarchs, static_cast<unsigned>(processor->core->uarch));
    return 0;
}
