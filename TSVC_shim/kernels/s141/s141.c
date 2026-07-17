#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

real_t kernel_s141(struct args_t * func_args)
{

//    nonlinear dependence testing
//    walk a row in a symmetric packed array
//    element a(i,j) for (int j>i) stored in location j*(j-1)/2+i

    initialise_arrays("s141");
    gettimeofday(&func_args->t1, NULL);

    int k;
    for (int nl = 0; nl < 200*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            k = (i+1) * ((i+1) - 1) / 2 + (i+1)-1;
            for (int j = i; j < LEN_2D; j++) {
                flat_2d_array[k] += bb[j][i];
                k += j+1;
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    gettimeofday(&func_args->t2, NULL);
    return calc_checksum("s141");
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
  real_t chk = kernel_s141(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}
