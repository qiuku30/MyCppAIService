#include <cpuinfo.h>
#include <stddef.h>

__attribute__((visibility("default")))
const struct cpuinfo_processor* cpuinfo_probe_initialize(
    uint32_t* cores, uint32_t* uarchs) {
    if (!cpuinfo_initialize()) return NULL;
    *cores = cpuinfo_get_cores_count();
    *uarchs = cpuinfo_get_uarchs_count();
    return cpuinfo_get_processor(0);
}
