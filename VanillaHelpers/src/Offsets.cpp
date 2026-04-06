// This file is part of VanillaHelpers.
//
// VanillaHelpers is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// VanillaHelpers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lessed General Public License along with
// VanillaHelpers. If not, see <https://www.gnu.org/licenses/>.

#include "Offsets.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Offsets {

// Define all offsets with compiled-in defaults (WoW 1.12.1.5875).
#define X(name, default_val) uintptr_t name = default_val;
OFFSET_LIST
#undef X

bool LoadFromFile(const char *dllDir) {
    std::string path = std::string(dllDir) + "offsets.ini";
    FILE *f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path.c_str(), "r");
#else
    f = fopen(path.c_str(), "r");
#endif
    if (!f)
        return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Skip leading whitespace.
        char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;

        // Skip comments and blank lines.
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        // Parse "KEY = 0xHEX" or "KEY = DECIMAL".
        char key[256] = {};
        char val[64] = {};
#ifdef _MSC_VER
        if (sscanf_s(p, "%255[A-Za-z0-9_] = %63s", key, (unsigned)sizeof(key), val, (unsigned)sizeof(val)) != 2)
            continue;
#else
        if (sscanf(p, "%255[A-Za-z0-9_] = %63s", key, val) != 2)
            continue;
#endif

        unsigned long value = 0;
        if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
            if (sscanf(val, "%lx", &value) != 1)
                continue;
        } else {
            if (sscanf(val, "%lu", &value) != 1)
                continue;
        }

        // Match key against all known offset names.
#define X(name, default_val) \
        if (strcmp(key, #name) == 0) { name = value; continue; }
        OFFSET_LIST
#undef X
    }

    fclose(f);
    return true;
}

} // namespace Offsets
