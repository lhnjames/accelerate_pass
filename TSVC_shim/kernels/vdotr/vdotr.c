#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

real_t kernel_vdotr(struct args_t * func_args)
{

//    control loops
//    vector dot product reduction

    initialise_arrays("vdotr");
    gettimeofday(&func_args->t1, NULL);

    real_t dot;
    for (int nl = 0; nl < iterations*10; nl++) {
        dot = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            dot += a[i] * b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, dot);
    }

    gettimeofday(&func_args->t2, NULL);
    return dot;
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

  struct args_t func_args = {.arg_info = NULL};

  polybench_start_instruments;
  real_t chk = kernel_vdotr(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}
