#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

int kernel_s318( int inc)
real_t s318(struct args_t * func_args)
{

//    reductions
//    isamax, max absolute value, increments not equal to 1

    int inc = *(int*)func_args->arg_info;

    initialise_arrays("s318");
    gettimeofday(&func_args->t1, NULL);

    int k, index;
    real_t max, chksum;
    for (int nl = 0; nl < iterations/2; nl++) {
        k = 0;
        index = 0;
        max = ABS(a[0]);
        k += inc;
        for (int i = 1; i < LEN_1D; i++) {
            if (ABS(a[k]) <= max) {
                goto L5;
            }
            index = i;
            max = ABS(a[k]);
L5:
            k += inc;
        }
        chksum = max + (real_t) index;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    gettimeofday(&func_args->t2, NULL);
    return max + index + 1;
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

  struct args_t func_args = {.arg_info = &n1};

  polybench_start_instruments;
  real_t chk = kernel_s318(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}
