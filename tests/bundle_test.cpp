#include "platform/bundle.hpp"
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s <bundle> <dest>\n", argv[0]); return 2; }
    std::vector<bodyclock::platform::BundleEntry> entries;
    unsigned long long total = 0;
    if (!bodyclock::platform::bundle_list(argv[1], &entries, &total)) return 1;
    std::printf("table: %d entries, %.2f GB\n", (int)entries.size(), total / 1073741824.0);
    if (!bodyclock::platform::bundle_extract(argv[1], argv[2], 0, 0)) return 1;
    std::printf("extracted ok\n");
    return 0;
}
