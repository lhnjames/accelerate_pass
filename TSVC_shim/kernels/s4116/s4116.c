#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

int kernel_s4116(int* __restrict__ ip, int j, int inc)
real_t s4116(struct args_t * func_args)
{

//    indirect addressing
//    more complicated sparse sdot
//    gather is required

    struct{int * __restrict__ a;int b;int c;} * x = func_args->arg_info;
    int * __restrict__ ip = x->a;
    int j = x->b;
    int inc = x->c;

    initialise_arrays("s4116");
    gettimeofday(&func_args->t1, NULL);

    real_t sum;
    int off;
    for (int nl = 0; nl < 100*iterations; nl++) {
        sum = 0.;
        for (int i = 0; i < LEN_2D-1; i++) {
            off = inc + i;
            sum += a[off] * aa[j-1][ip[i]];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    gettimeofday(&func_args->t2, NULL);
    return sum;
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

  struct args_t func_args = {.arg_info = &(struct{int * a; int b; int c;}){ip, LEN_2D/2, n1}};

  polybench_start_instruments;
  real_t chk = kernel_s4116(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}
