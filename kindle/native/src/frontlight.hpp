#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace kindle {

class Frontlight {
public:
    static int getIntensity() {
        FILE* fp = popen("lipc-get-prop com.lab126.powerd flIntensity 2>/dev/null", "r");
        if (!fp) return 12;
        char buf[32] = {0};
        int val = 12;
        if (fgets(buf, sizeof(buf), fp)) {
            val = std::atoi(buf);
        }
        pclose(fp);
        return val;
    }

    static void setIntensity(int val) {
        if (val < 0) val = 0;
        if (val > 24) val = 24;
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "lipc-set-prop com.lab126.powerd flIntensity %d 2>/dev/null", val);
        int ret = std::system(cmd);
        (void)ret;
    }
};

} // namespace kindle
