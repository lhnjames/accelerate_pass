#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include "common.h"
#include "array_defs.h"

real_t kernel_s112(struct args_t * func_args)
{

//    linear dependence testing
//    loop reversal

    initialise_arrays("s112");
    gettimeofday(&func_args->t1, NULL);

    for (int nl = 0; nl < 3*iterations; nl++) {
        for (int i = LEN_1D - 2; i >= 0; i--) {
            a[i+1] = a[i] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    gettimeofday(&func_args->t2, NULL);
    return calc_checksum("s112");
}

int main(int argc, char** argv)
{
  int n1 = 1;
  int n3 = 1;
  int* ip;
  real_t s1, s2;
  init(&ip, &s1, &s2);

  struct args_t func_args = {.arg_info = NULL};

  real_t chk = kernel_s112(&func_args);
  printf("checksum: %.6f\n", chk);

  free(ip);
  return 0;
}
