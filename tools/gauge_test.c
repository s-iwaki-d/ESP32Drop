/* Host replay of the REAL firmware gauge code (awdl_gauge.h) against a recorded
 * PSF trace. Prints one line per frame; tools/score_gauge.py grades the output with
 * the same reference gauge the Python bake-off used, so the numbers are directly
 * comparable to the candidate designs.
 *
 *   cc -O2 -I.. -o /tmp/gauge_test tools/gauge_test.c && /tmp/gauge_test .build/trace3.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include "../ESP32Drop/src/awdl/core/awdl_gauge.h"

static ga_t G;

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : ".build/trace3.txt";
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return 1; }
  ga_reset(&G);
  char line[512];
  long n = 0;
  uint32_t t_first = 0, t0 = 0, t_lock = 0;
  while (fgets(line, sizeof(line), f)) {
    char mac[16]; long rem, aws, rx, ptx, ttx;
    if (sscanf(line, "PS %12s rem=%ld aws=%ld rx=%ld ptx=%ld ttx=%ld",
               mac, &rem, &aws, &rx, &ptx, &ttx) != 6) continue;
    uint8_t m[6];
    for (int i = 0; i < 6; i++) {
      char b[3] = { mac[2*i], mac[2*i+1], 0 };
      m[i] = (uint8_t)strtol(b, NULL, 16);
    }
    if (!t0) t0 = (uint32_t)rx;
    uint32_t out = ga_on_frame(&G, m, (uint32_t)rx, (uint32_t)ptx, (uint16_t)aws);
    if (G.locked && !t_lock) t_lock = (uint32_t)rx;
    if (out != GA_NONE && !t_first) t_first = (uint32_t)rx;
    if (out == GA_NONE) printf("OUT %lu %s NONE\n", (unsigned long)rx, mac);
    else                printf("OUT %lu %s %lu\n", (unsigned long)rx, mac, (unsigned long)out);
    n++;
  }
  fclose(f);
  fprintf(stderr, "# lock at %.2fs, first phase sample at %.2fs (from the first frame)\n",
          (t_lock ? (t_lock - t0) / 1e6 : -1.0), (t_first ? (t_first - t0) / 1e6 : -1.0));
  fprintf(stderr, "# %ld frames; locked=%d c_off=%lu c_rate=%ldppb march=%ld\n"
                  "# reacq=%lu awfix=%lu suff=%lu veto=%lu sick=%lu quar=%lu alarm=%lu op=%u/%u\n",
          n, G.locked, (unsigned long)G.c_off, (long)G.c_rate, (long)G.march,
          (unsigned long)G.n_reacq, (unsigned long)G.n_awfix, (unsigned long)G.n_suff,
          (unsigned long)G.n_veto, (unsigned long)G.n_sick, (unsigned long)G.n_quar,
          (unsigned long)G.n_alarm, G.n_op, G.n_active);
  return 0;
}
