// Runs the firmware's Core.h on a laptop so tests/test_bracelets.py can check
// the real C++ logic (alone, and mixed with the Python simulator's copy).
// Build: g++ -std=c++17 -o native_bracelet tests/native_main.cpp
//
// stdin lines:  @<ms>   set the clock        !cmd   wearer input (see Core.h)
//               W26 ... packet from another bracelet
//               ?       run one tick, print everything, then "# sync"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../firmware/bracelet/Core.h"

using namespace w26;

int main(int argc, char** argv) {
  uint16_t id = argc > 1 ? (uint16_t)atoi(argv[1]) : 1234;
  uint32_t now = 0;
  Core core;
  core.begin(id, now, (uint16_t)(id * 100));
  setvbuf(stdout, nullptr, _IOLBF, 0);

  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    char reply[96];
    if (line[0] == '@') {
      now = (uint32_t)strtoul(line + 1, nullptr, 10);
    } else if (line[0] == '?') {
      core.tick(now);
      char out[W26_LINE];
      while (core.popLine(out, sizeof(out))) printf("%s\n", out);
      for (Pattern p; (p = core.popPattern()) != P_NONE;) printf("# vibe %s\n", patternName(p));
      printf("# sync\n");
    } else if (handleCommand(core, line, now, reply, sizeof(reply))) {
      if (reply[0]) printf("# %s\n", reply);
    } else {
      core.onLine(line, now);
    }
  }
  return 0;
}
