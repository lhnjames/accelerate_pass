/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* cholesky.c: this file is part of PolyBench/C */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

/* Include polybench common header. */
#include <polybench.h>

/* Include benchmark-specific header. */
#include "cholesky.h"


/* Array initialization. */
static
void init_array(int n,
		DATA_TYPE POLYBENCH_2D(A,N,N,n,n))
{
  int i, j;

  for (i = 0; i < n; i++)
    {
      for (j = 0; j <= i; j++)
	A[i][j] = (DATA_TYPE)(-j % n) / n + 1;
      for (j = i+1; j < n; j++) {
	A[i][j] = 0;
      }
      A[i][i] = 1;
    }

  /* Make the matrix positive semi-definite. */
  int r,s,t;
  POLYBENCH_2D_ARRAY_DECL(B, DATA_TYPE, N, N, n, n);
  for (r = 0; r < n; ++r)
    for (s = 0; s < n; ++s)
      (POLYBENCH_ARRAY(B))[r][s] = 0;
  for (t = 0; t < n; ++t)
    for (r = 0; r < n; ++r)
      for (s = 0; s < n; ++s)
	(POLYBENCH_ARRAY(B))[r][s] += A[r][t] * A[s][t];
    for (r = 0; r < n; ++r)
      for (s = 0; s < n; ++s)
	A[r][s] = (POLYBENCH_ARRAY(B))[r][s];
  POLYBENCH_FREE_ARRAY(B);

}


/* DCE code. Must scan the entire live-out data.
   Can be used also to check the correctness of the output. */
static
void print_array(int n,
		 DATA_TYPE POLYBENCH_2D(A,N,N,n,n))

{
  int i, j;

  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("A");
  for (i = 0; i < n; i++)
    for (j = 0; j <= i; j++) {
    if ((i * n + j) % 20 == 0) fprintf (POLYBENCH_DUMP_TARGET, "\n");
    fprintf (POLYBENCH_DUMP_TARGET, DATA_PRINTF_MODIFIER, A[i][j]);
  }
  POLYBENCH_DUMP_END("A");
  POLYBENCH_DUMP_FINISH;
}


/* Main computational kernel. The whole function will be timed,
   including the call and return. */
static void kernel_cholesky(int n,
		     DATA_TYPE POLYBENCH_2D(A,N,N,n,n))
{
    int i, j, k, ii;
    const int TILE_SZ = 64;

    for (ii = 0; ii < n; ii += TILE_SZ) {
        int i_end = ii + TILE_SZ < n ? ii + TILE_SZ : n;
        for (i = ii; i < i_end; i++) {
            /* Off‑diagonal: j < i */
            for (j = 0; j < i; j++) {
                double acc = 0.0;
                DATA_TYPE * restrict Ai = A[i];
                DATA_TYPE * restrict Aj = A[j];
                #pragma clang loop vectorize(enable) interleave(enable) unroll_count(8)
                for (k = 0; k < j; k++) {
                    acc += Ai[k] * Aj[k];
                }
                A[i][j] = (A[i][j] - acc) / A[j][j];
            }
            /* Diagonal */
            {
                double acc = 0.0;
                DATA_TYPE * restrict Ai = A[i];
                #pragma clang loop vectorize(enable) interleave(enable) unroll_count(8)
                for (k = 0; k < i; k++) {
                    acc += Ai[k] * Ai[k];
                }
                A[i][i] = sqrt(A[i][i] - acc);
            }
        }
    }
}


int main(int argc, char** argv)
{
  /* Retrieve problem size. */
  int n = N;

  /* Variable declaration/allocation. */
  POLYBENCH_2D_ARRAY_DECL(A, DATA_TYPE, N, N, n, n);

  /* Initialize array(s). */
  init_array (n, POLYBENCH_ARRAY(A));

  /* Start timer. */
  polybench_start_instruments;

  /* Run kernel. */
  kernel_cholesky (n, POLYBENCH_ARRAY(A));

  /* Stop and print timer. */
  polybench_stop_instruments;
  polybench_print_instruments;

  /* Prevent dead-code elimination. All live-out data must be printed
     by the function call in argument. */
  polybench_prevent_dce(print_array(n, POLYBENCH_ARRAY(A)));

  /* Be clean. */
  POLYBENCH_FREE_ARRAY(A);

  return 0;
}
