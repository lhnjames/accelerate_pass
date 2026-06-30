/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* correlation.c: this file is part of PolyBench/C */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

/* Include polybench common header. */
#include <polybench.h>

/* Include benchmark-specific header. */
#include "correlation.h"


/* Array initialization. */
static
void init_array (int m,
		 int n,
		 DATA_TYPE *float_n,
		 DATA_TYPE POLYBENCH_2D(data,N,M,n,m))
{
  int i, j;

  *float_n = (DATA_TYPE)N;

  for (i = 0; i < N; i++)
    for (j = 0; j < M; j++)
      data[i][j] = (DATA_TYPE)(i*j)/M + i;

}


/* DCE code. Must scan the entire live-out data.
   Can be used also to check the correctness of the output. */
static
void print_array(int m,
		 DATA_TYPE POLYBENCH_2D(corr,M,M,m,m))

{
  int i, j;

  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("corr");
  for (i = 0; i < m; i++)
    for (j = 0; j < m; j++) {
      if ((i * m + j) % 20 == 0) fprintf (POLYBENCH_DUMP_TARGET, "\n");
      fprintf (POLYBENCH_DUMP_TARGET, DATA_PRINTF_MODIFIER, corr[i][j]);
    }
  POLYBENCH_DUMP_END("corr");
  POLYBENCH_DUMP_FINISH;
}


/* Main computational kernel. The whole function will be timed,
   including the call and return. */
static
void kernel_correlation(int m, int n,
                        DATA_TYPE float_n,
                        DATA_TYPE POLYBENCH_2D(data,N,M,n,m),
                        DATA_TYPE POLYBENCH_2D(corr,M,M,m,m),
                        DATA_TYPE POLYBENCH_1D(mean,M,m),
                        DATA_TYPE POLYBENCH_1D(stddev,M,m))
{
    int i, j, k;
    DATA_TYPE eps = 0.1f;

    DATA_TYPE (*restrict data_ptr)[M] = (DATA_TYPE (*)[M]) data;
    DATA_TYPE (*restrict corr_ptr)[M] = (DATA_TYPE (*)[M]) corr;
    DATA_TYPE *restrict mean_ptr = (DATA_TYPE *restrict) mean;
    DATA_TYPE *restrict stddev_ptr = (DATA_TYPE *restrict) stddev;

    /* Step 1: Compute mean and stddev with row-major access */
    {
        DATA_TYPE sum[_PB_M];
        DATA_TYPE sumsq[_PB_M];
        for (j = 0; j < _PB_M; j++) {
            sum[j] = 0.0;
            sumsq[j] = 0.0;
        }

        for (i = 0; i < _PB_N; i++) {
            #pragma clang loop vectorize(enable)
            for (j = 0; j < _PB_M; j++) {
                DATA_TYPE val = data_ptr[i][j];
                sum[j] += val;
                sumsq[j] += val * val;
            }
        }

        #pragma clang loop vectorize(enable)
        for (j = 0; j < _PB_M; j++) {
            DATA_TYPE mean_val = sum[j] / float_n;
            DATA_TYPE variance = sumsq[j] / float_n - mean_val * mean_val;
            DATA_TYPE stdev;
            stdev = sqrt(variance);
            if (stdev <= eps) stdev = 1.0f;
            mean_ptr[j] = mean_val;
            stddev_ptr[j] = stdev;
        }
    }

    /* Step 2: Center and reduce with row-major access */
    {
        DATA_TYPE sqrt_float_n = sqrt(float_n);
        DATA_TYPE inv_factor[_PB_M];
        for (j = 0; j < _PB_M; j++) {
            inv_factor[j] = 1.0f / (sqrt_float_n * stddev_ptr[j]);
        }

        for (i = 0; i < _PB_N; i++) {
            #pragma clang loop vectorize(enable)
            for (j = 0; j < _PB_M; j++) {
                data_ptr[i][j] = (data_ptr[i][j] - mean_ptr[j]) * inv_factor[j];
            }
        }
    }

    /* Step 3: Initialize correlation matrix to zero */
    for (i = 0; i < _PB_M; i++) {
        #pragma clang loop vectorize(enable)
        for (j = 0; j < _PB_M; j++) {
            corr_ptr[i][j] = 0.0;
        }
    }

    /* Step 4: Compute correlation via outer product. 
       Hoist current row into a local array to reduce DRAM traffic.
       Tile the i and j loops to improve cache reuse on corr. */
    {
        const int T = 64;  /* tile size, fits in L1 */
        for (k = 0; k < _PB_N; k++) {
            DATA_TYPE row[_PB_M];
            /* Load the entire row once */
            for (j = 0; j < _PB_M; j++) {
                row[j] = data_ptr[k][j];
            }
            /* Tile over i and j */
            for (int ii = 0; ii < _PB_M - 1; ii += T) {
                int i_end = (ii + T < _PB_M) ? ii + T : _PB_M;
                for (int jj = ii; jj < _PB_M; jj += T) {
                    int j_end = (jj + T < _PB_M) ? jj + T : _PB_M;
                    for (i = ii; i < i_end; i++) {
                        DATA_TYPE row_i = row[i];
                        int j_start = (i + 1 > jj) ? i + 1 : jj; /* lower triangular */
                        #pragma clang loop vectorize(enable)
                        for (j = j_start; j < j_end; j++) {
                            corr_ptr[i][j] += row_i * row[j];
                        }
                    }
                }
            }
        }
    }

    /* Step 5: Set diagonal elements to 1.0 */
    for (i = 0; i < _PB_M; i++) {
        corr_ptr[i][i] = 1.0f;
    }

    /* Step 6: Fill symmetric lower triangle.
       Interchange loops to make stores contiguous (stride-1) and enable vectorization.
       Also tile to improve read locality. */
    {
        const int T = 64;
        for (int jj = 1; jj < _PB_M; jj += T) {
            int j_end = (jj + T < _PB_M) ? jj + T : _PB_M;
            for (int ii = 0; ii < j_end; ii += T) {
                /* j-loop outer, i-loop inner -> stores contiguous */
                for (j = jj; j < j_end; j++) {
                    int i_max = (ii + T < j) ? ii + T : j;  /* i < j */
                    #pragma clang loop vectorize(enable)
                    for (i = ii; i < i_max; i++) {
                        corr_ptr[j][i] = corr_ptr[i][j];
                    }
                }
            }
        }
    }
}


int main(int argc, char** argv)
{
  /* Retrieve problem size. */
  int n = N;
  int m = M;

  /* Variable declaration/allocation. */
  DATA_TYPE float_n;
  POLYBENCH_2D_ARRAY_DECL(data,DATA_TYPE,N,M,n,m);
  POLYBENCH_2D_ARRAY_DECL(corr,DATA_TYPE,M,M,m,m);
  POLYBENCH_1D_ARRAY_DECL(mean,DATA_TYPE,M,m);
  POLYBENCH_1D_ARRAY_DECL(stddev,DATA_TYPE,M,m);

  /* Initialize array(s). */
  init_array (m, n, &float_n, POLYBENCH_ARRAY(data));

  /* Start timer. */
  polybench_start_instruments;

  /* Run kernel. */
  kernel_correlation (m, n, float_n,
		      POLYBENCH_ARRAY(data),
		      POLYBENCH_ARRAY(corr),
		      POLYBENCH_ARRAY(mean),
		      POLYBENCH_ARRAY(stddev));

  /* Stop and print timer. */
  polybench_stop_instruments;
  polybench_print_instruments;

  /* Prevent dead-code elimination. All live-out data must be printed
     by the function call in argument. */
  polybench_prevent_dce(print_array(m, POLYBENCH_ARRAY(corr)));

  /* Be clean. */
  POLYBENCH_FREE_ARRAY(data);
  POLYBENCH_FREE_ARRAY(corr);
  POLYBENCH_FREE_ARRAY(mean);
  POLYBENCH_FREE_ARRAY(stddev);

  return 0;
}
