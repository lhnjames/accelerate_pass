#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

int kernel_s332( real_t t)
real_t s332(struct args_t * func_args)
{

//    search loops
//    first value greater than threshold

    int t = *(int*)func_args->arg_info;

    initialise_arrays("s332");
    gettimeofday(&func_args->t1, NULL);

    int index;
    real_t value;
    real_t chksum;
    for (int nl = 0; nl < iterations; nl++) {
        index = -2;
        value = -1.;
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > t) {
                index = i;
                value = a[i];
                goto L20;
            }
        }
L20:
        chksum = value + (real_t) index;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    gettimeofday(&func_args->t2, NULL);
    return value;
}

static void print_checksum(real_t chk)
{
  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("checksum");
  fprintf(POLYBENCH_DUMP_TARGET, "%.6f", chk);
  POLYBENCH_DUMP_END("checksum");
  POLYBENCH_DUMP_FINISH;
}

int main(int argc, char** argv)
{
  int n1 = 1;
  int n3 = 1;
  int* ip;
  real_t s1, s2;
  init(&ip, &s1, &s2);

  struct args_t func_args = {.arg_info = &s1};

  polybench_start_instruments;
  real_t chk = kernel_s332(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}
