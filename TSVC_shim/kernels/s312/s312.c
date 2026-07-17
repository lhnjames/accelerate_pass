#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include "common.h"
#include "array_defs.h"

real_t kernel_s312(struct args_t * func_args)
{

//    reductions
//    product reduction

    initialise_arrays("s312");
    gettimeofday(&func_args->t1, NULL);

    real_t prod;
    for (int nl = 0; nl < 10*iterations; nl++) {
        prod = (real_t)1.;
        for (int i = 0; i < LEN_1D; i++) {
            prod *= a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, prod);
    }

    gettimeofday(&func_args->t2, NULL);
    return prod;
}

int main(int argc, char** argv)
{
  int n1 = 1;
  int n3 = 1;
  int* ip;
  real_t s1, s2;
  init(&ip, &s1, &s2);

  struct args_t func_args = {.arg_info = NULL};

  real_t chk = kernel_s312(&func_args);
  printf("checksum: %.6f\n", chk);

  free(ip);
  return 0;
}
