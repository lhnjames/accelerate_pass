/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* polybench.c: this file is part of PolyBench/C */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <math.h>
#ifdef _OPENMP
# include <omp.h>
#endif

#if defined(POLYBENCH_PAPI)
# undef POLYBENCH_PAPI
# include "polybench.h"
# define POLYBENCH_PAPI
#else
# include "polybench.h"
#endif

/* By default, collect PAPI counters on thread 0. */
#ifndef POLYBENCH_THREAD_MONITOR
# define POLYBENCH_THREAD_MONITOR 0
#endif

/* Total LLC cache size. By default 32+MB.. */
#ifndef POLYBENCH_CACHE_SIZE_KB
# define POLYBENCH_CACHE_SIZE_KB 32770
#endif


int polybench_papi_counters_threadid = POLYBENCH_THREAD_MONITOR;
double polybench_program_total_flops = 0;

#ifdef POLYBENCH_PAPI
# include <papi.h>
# define POLYBENCH_MAX_NB_PAPI_COUNTERS 96
  char* _polybench_papi_eventlist[] = {
#include "papi_counters.list"
    NULL
  };
  int polybench_papi_eventset;
  int polybench_papi_eventlist[POLYBENCH_MAX_NB_PAPI_COUNTERS];
  long_long polybench_papi_values[POLYBENCH_MAX_NB_PAPI_COUNTERS];

#endif

/*
 * Allocation table, to enable inter-array padding. All data allocated
 * with polybench_alloc_data should be freed with polybench_free_data.
 *
 */
#define NB_INITIAL_TABLE_ENTRIES 512
struct polybench_data_ptrs
{
  void** user_view;
  void** real_ptr;
  int nb_entries;
  int nb_avail_entries;
};
static struct polybench_data_ptrs* _polybench_alloc_table = NULL;
static size_t polybench_inter_array_padding_sz = 0;

/* Timer code (gettimeofday). */
double polybench_t_start, polybench_t_end;
/* Timer code (RDTSC). */
unsigned long long int polybench_c_start, polybench_c_end;

static
double rtclock()
{
#if defined(POLYBENCH_TIME) || defined(POLYBENCH_GFLOPS)
    struct timeval Tp;
    int stat;
    stat = gettimeofday (&Tp, NULL);
    if (stat != 0)
      printf ("Error return from gettimeofday: %d", stat);
    return (Tp.tv_sec + Tp.tv_usec * 1.0e-6);
#else
    return 0;
#endif
}


#ifdef POLYBENCH_CYCLE_ACCURATE_TIMER
static
unsigned long long int rdtsc()
{
  unsigned long long int ret = 0;
  unsigned int cycles_lo;
  unsigned int cycles_hi;
  __asm__ volatile ("RDTSC" : "=a" (cycles_lo), "=d" (cycles_hi));
  ret = (unsigned long long int)cycles_hi << 32 | cycles_lo;

  return ret;
}
#endif

void polybench_flush_cache()
{
  int cs = POLYBENCH_CACHE_SIZE_KB * 1024 / sizeof(double);
  double* flush = (double*) calloc (cs, sizeof(double));
  int i;
  double tmp = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+:tmp) private(i)
#endif
  for (i = 0; i < cs; i++)
    tmp += flush[i];
  assert (tmp <= 10.0);
  free (flush);
}


#ifdef POLYBENCH_LINUX_FIFO_SCHEDULER
void polybench_linux_fifo_scheduler()
{
  /* Use FIFO scheduler to limit OS interference. Program must be run
     as root, and this works only for Linux kernels. */
  struct sched_param schedParam;
  schedParam.sched_priority = sched_get_priority_max (SCHED_FIFO);
  sched_setscheduler (0, SCHED_FIFO, &schedParam);
}


void polybench_linux_standard_scheduler()
{
  /* Restore to standard scheduler policy. */
  struct sched_param schedParam;
  schedParam.sched_priority = sched_get_priority_max (SCHED_OTHER);
  sched_setscheduler (0, SCHED_OTHER, &schedParam);
}
#endif

#ifdef POLYBENCH_PAPI

static
void test_fail(char *file, int line, char *call, int retval)
{
  char buf[128];

  memset(buf, '\0', sizeof(buf));
  if (retval != 0)
    fprintf (stdout,"%-40s FAILED\nLine # %d\n", file, line);
  else
    {
      fprintf (stdout,"%-40s SKIPPED\n", file);
      fprintf (stdout,"Line # %d\n", line);
    }
  if (retval == PAPI_ESYS)
    {
      sprintf (buf, "System error in %s", call);
      perror (buf);
    }
  else if (retval > 0)
    fprintf (stdout,"Error: %s\n", call);
  else if (retval == 0)
    fprintf (stdout,"Error: %s\n", call);
  else
    {
      char errstring[PAPI_MAX_STR_LEN];
      // PAPI 5.4.3 has changed the API for PAPI_perror.
      #if defined (PAPI_VERSION) && ((PAPI_VERSION_MAJOR(PAPI_VERSION) == 5 && PAPI_VERSION_MINOR(PAPI_VERSION) >= 4) || PAPI_VERSION_MAJOR(PAPI_VERSION) > 5)
      fprintf (stdout, "Error in %s: %s\n", call, PAPI_strerror(retval));
      #else
      PAPI_perror (retval, errstring, PAPI_MAX_STR_LEN);
      fprintf (stdout,"Error in %s: %s\n", call, errstring);
      #endif
    }
  fprintf (stdout,"\n");
  if (PAPI_is_initialized ())
    PAPI_shutdown ();
  exit (1);
}


void polybench_papi_init()
{
# ifdef _OPENMP
#pragma omp parallel
  {
#pragma omp master
    {
      if (omp_get_max_threads () < polybench_papi_counters_threadid)
	polybench_papi_counters_threadid = omp_get_max_threads () - 1;
    }
#pragma omp barrier

    if (omp_get_thread_num () == polybench_papi_counters_threadid)
      {
# endif
	int retval;
	polybench_papi_eventset = PAPI_NULL;
	if ((retval = PAPI_library_init (PAPI_VER_CURRENT)) != PAPI_VER_CURRENT)
	  test_fail (__FILE__, __LINE__, "PAPI_library_init", retval);
	if ((retval = PAPI_create_eventset (&polybench_papi_eventset))
	    != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_create_eventset", retval);
	int k;
	for (k = 0; _polybench_papi_eventlist[k]; ++k)
	  {
	    if ((retval =
		 PAPI_event_name_to_code (_polybench_papi_eventlist[k],
					  &(polybench_papi_eventlist[k])))
		!= PAPI_OK)
	      test_fail (__FILE__, __LINE__, "PAPI_event_name_to_code", retval);
	  }
	polybench_papi_eventlist[k] = 0;


# ifdef _OPENMP
      }
  }
#pragma omp barrier
# endif
}


void polybench_papi_close()
{
# ifdef _OPENMP
#pragma omp parallel
  {
    if (omp_get_thread_num () == polybench_papi_counters_threadid)
      {
# endif
	int retval;
	if ((retval = PAPI_destroy_eventset (&polybench_papi_eventset))
	    != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_destroy_eventset", retval);
	if (PAPI_is_initialized ())
	  PAPI_shutdown ();
# ifdef _OPENMP
      }
  }
#pragma omp barrier
# endif
}

int polybench_papi_start_counter(int evid)
{
# ifndef POLYBENCH_NO_FLUSH_CACHE
    polybench_flush_cache();
# endif

# ifdef _OPENMP
# pragma omp parallel
  {
    if (omp_get_thread_num () == polybench_papi_counters_threadid)
      {
# endif

	int retval = 1;
	char descr[PAPI_MAX_STR_LEN];
	PAPI_event_info_t evinfo;
	PAPI_event_code_to_name (polybench_papi_eventlist[evid], descr);
	if (PAPI_add_event (polybench_papi_eventset,
			    polybench_papi_eventlist[evid]) != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_add_event", 1);
	if (PAPI_get_event_info (polybench_papi_eventlist[evid], &evinfo)
	    != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_get_event_info", retval);
	if ((retval = PAPI_start (polybench_papi_eventset)) != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_start", retval);
# ifdef _OPENMP
      }
  }
#pragma omp barrier
# endif
  return 0;
}


void polybench_papi_stop_counter(int evid)
{
# ifdef _OPENMP
# pragma omp parallel
  {
    if (omp_get_thread_num () == polybench_papi_counters_threadid)
      {
# endif
	int retval;
	long_long values[1];
	values[0] = 0;
	if ((retval = PAPI_read (polybench_papi_eventset, &values[0]))
	    != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_read", retval);

	if ((retval = PAPI_stop (polybench_papi_eventset, NULL)) != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_stop", retval);

	polybench_papi_values[evid] = values[0];

	if ((retval = PAPI_remove_event
	     (polybench_papi_eventset,
	      polybench_papi_eventlist[evid])) != PAPI_OK)
	  test_fail (__FILE__, __LINE__, "PAPI_remove_event", retval);
# ifdef _OPENMP
      }
  }
#pragma omp barrier
# endif
}


void polybench_papi_print()
{
  int verbose = 0;
# ifdef _OPENMP
# pragma omp parallel
  {
    if (omp_get_thread_num() == polybench_papi_counters_threadid)
      {
#ifdef POLYBENCH_PAPI_VERBOSE
	verbose = 1;
#endif
	if (verbose)
	  printf ("On thread %d:\n", polybench_papi_counters_threadid);
#endif
	int evid;
	for (evid = 0; polybench_papi_eventlist[evid] != 0; ++evid)
	  {
	    if (verbose)
	      printf ("%s=", _polybench_papi_eventlist[evid]);
	    printf ("%llu ", polybench_papi_values[evid]);
	    if (verbose)
	      printf ("\n");
	  }
	printf ("\n");
# ifdef _OPENMP
      }
  }
#pragma omp barrier
# endif
}

#endif
/* ! POLYBENCH_PAPI */

void polybench_prepare_instruments()
{
#ifndef POLYBENCH_NO_FLUSH_CACHE
  polybench_flush_cache ();
#endif
#ifdef POLYBENCH_LINUX_FIFO_SCHEDULER
  polybench_linux_fifo_scheduler ();
#endif
}


void polybench_timer_start()
{
  polybench_prepare_instruments ();
#ifndef POLYBENCH_CYCLE_ACCURATE_TIMER
  polybench_t_start = rtclock ();
#else
  polybench_c_start = rdtsc ();
#endif
}


void polybench_timer_stop()
{
#ifndef POLYBENCH_CYCLE_ACCURATE_TIMER
  polybench_t_end = rtclock ();
#else
  polybench_c_end = rdtsc ();
#endif
#ifdef POLYBENCH_LINUX_FIFO_SCHEDULER
  polybench_linux_standard_scheduler ();
#endif
}


void polybench_timer_print()
{
#ifdef POLYBENCH_GFLOPS
      if  (polybench_program_total_flops == 0)
	{
	  printf ("[PolyBench][WARNING] Program flops not defined, use polybench_set_program_flops(value)\n");
	  printf ("%0.6lf\n", polybench_t_end - polybench_t_start);
	}
      else
	printf ("%0.2lf\n",
		(polybench_program_total_flops /
		 (double)(polybench_t_end - polybench_t_start)) / 1000000000);
#else
# ifndef POLYBENCH_CYCLE_ACCURATE_TIMER
      printf ("%0.6f\n", polybench_t_end - polybench_t_start);
# else
      printf ("%Ld\n", polybench_c_end - polybench_c_start);
# endif
#endif
}

/*
 * These functions are used only if the user defines a specific
 * inter-array padding. It grows a global structure,
 * _polybench_alloc_table, which keeps track of the data allocated via
 * polybench_alloc_data (on which inter-array padding is applied), so
 * that the original, non-shifted pointer can be recovered when
 * calling polybench_free_data.
 *
 */
#ifdef POLYBENCH_ENABLE_INTARRAY_PAD
static
void grow_alloc_table()
{
  if (_polybench_alloc_table == NULL ||
      (_polybench_alloc_table->nb_entries % NB_INITIAL_TABLE_ENTRIES) != 0 ||
      _polybench_alloc_table->nb_avail_entries != 0)
    {
      /* Should never happen if the API is properly used. */
      fprintf (stderr, "[ERROR] Inter-array padding requires to use polybench_alloc_data and polybench_free_data\n");
      exit (1);
    }
  size_t sz = _polybench_alloc_table->nb_entries;
  sz += NB_INITIAL_TABLE_ENTRIES;
  _polybench_alloc_table->user_view =
    realloc (_polybench_alloc_table->user_view, sz * sizeof(void*));
  assert(_polybench_alloc_table->user_view != NULL);
  _polybench_alloc_table->real_ptr =
    realloc (_polybench_alloc_table->real_ptr, sz * sizeof(void*));
  assert(_polybench_alloc_table->real_ptr != NULL);
  _polybench_alloc_table->nb_avail_entries = NB_INITIAL_TABLE_ENTRIES;
}

static
void* register_padded_pointer(void* ptr, size_t orig_sz, size_t padded_sz)
{
  if (_polybench_alloc_table == NULL)
    {
      fprintf (stderr, "[ERROR] Inter-array padding requires to use polybench_alloc_data and polybench_free_data\n");
      exit (1);
    }
  if (_polybench_alloc_table->nb_avail_entries == 0)
    grow_alloc_table ();
  int id = _polybench_alloc_table->nb_entries++;
  _polybench_alloc_table->real_ptr[id] = ptr;
  _polybench_alloc_table->user_view[id] = ptr + (padded_sz - orig_sz);

  return _polybench_alloc_table->user_view[id];
}


static
void
free_data_from_alloc_table (void* ptr)
{
  if (_polybench_alloc_table != NULL && _polybench_alloc_table->nb_entries > 0)
    {
      int i;
      for (i = 0; i < _polybench_alloc_table->nb_entries; ++i)
	if (_polybench_alloc_table->user_view[i] == ptr ||
	    _polybench_alloc_table->real_ptr[i] == ptr)
	  break;
      if (i != _polybench_alloc_table->nb_entries)
	{
	  free (_polybench_alloc_table->real_ptr[i]);
	  for (; i < _polybench_alloc_table->nb_entries - 1; ++i)
	    {
	      _polybench_alloc_table->user_view[i] =
		_polybench_alloc_table->user_view[i + 1];
	      _polybench_alloc_table->real_ptr[i] =
		_polybench_alloc_table->real_ptr[i + 1];
	    }
	  _polybench_alloc_table->nb_entries--;
	  _polybench_alloc_table->nb_avail_entries++;
	  if (_polybench_alloc_table->nb_entries == 0)
	    {
	      free (_polybench_alloc_table->user_view);
	      free (_polybench_alloc_table->real_ptr);
	      free (_polybench_alloc_table);
	      _polybench_alloc_table = NULL;
	    }
	}
    }
}

static
void check_alloc_table_state()
{
  if (_polybench_alloc_table == NULL)
    {
      _polybench_alloc_table = (struct polybench_data_ptrs*)
	malloc (sizeof(struct polybench_data_ptrs));
      assert(_polybench_alloc_table != NULL);
      _polybench_alloc_table->user_view =
	(void**) malloc (sizeof(void*) * NB_INITIAL_TABLE_ENTRIES);
      assert(_polybench_alloc_table->user_view != NULL);
      _polybench_alloc_table->real_ptr =
	(void**) malloc (sizeof(void*) * NB_INITIAL_TABLE_ENTRIES);
      assert(_polybench_alloc_table->real_ptr != NULL);
      _polybench_alloc_table->nb_entries = 0;
      _polybench_alloc_table->nb_avail_entries = NB_INITIAL_TABLE_ENTRIES;
    }
}

#endif // !POLYBENCH_ENABLE_INTARRAY_PAD


static
void*
xmalloc(size_t alloc_sz)
{
  void* ret = NULL;
  /* By default, post-pad the arrays. Safe behavior, but likely useless. */
  polybench_inter_array_padding_sz += POLYBENCH_INTER_ARRAY_PADDING_FACTOR;
  size_t padded_sz = alloc_sz + polybench_inter_array_padding_sz;
  int err = posix_memalign (&ret, 4096, padded_sz);
  if (! ret || err)
    {
      fprintf (stderr, "[PolyBench] posix_memalign: cannot allocate memory");
      exit (1);
    }
  /* Safeguard: this is invoked only if polybench.c has been compiled
     with inter-array padding support from polybench.h. If so, move
     the starting address of the allocation and return it to the
     user. The original pointer is registered in an allocation table
     internal to polybench.c. Data must then be freed using
     polybench_free_data, which will inspect the allocation table to
     free the original pointer.*/
#ifdef POLYBENCH_ENABLE_INTARRAY_PAD
  /* This moves the 'ret' pointer by (padded_sz - alloc_sz) positions, and
  registers it in the lookup table for future free using
  polybench_free_data. */
  ret = register_padded_pointer(ret, alloc_sz, padded_sz);
#endif

  return ret;
}


void polybench_free_data(void* ptr)
{
#ifdef POLYBENCH_ENABLE_INTARRAY_PAD
  free_data_from_alloc_table (ptr);
#else
  free (ptr);
#endif
}


void* polybench_alloc_data(unsigned long long int n, int elt_size)
{
#ifdef POLYBENCH_ENABLE_INTARRAY_PAD
  check_alloc_table_state ();
#endif

  /// FIXME: detect overflow!
  size_t val = n;
  val *= elt_size;
  void* ret = xmalloc (val);

  return ret;
}


/* ==== appended from TSVC (UoB-HPC/TSVC_2): common.c + dummy.c, unmodified ==== */

/* ==== TSVC global array storage (moved out of tsvc.c's main translation unit) ==== */
#include "common.h"
#include "array_defs.h"

__attribute__((aligned(ARRAY_ALIGNMENT))) real_t flat_2d_array[LEN_2D*LEN_2D];
__attribute__((aligned(ARRAY_ALIGNMENT))) real_t x[LEN_1D];
__attribute__((aligned(ARRAY_ALIGNMENT))) real_t a[LEN_1D],b[LEN_1D],c[LEN_1D],d[LEN_1D],e[LEN_1D],
                                   aa[LEN_2D][LEN_2D],bb[LEN_2D][LEN_2D],cc[LEN_2D][LEN_2D],tt[LEN_2D][LEN_2D];
__attribute__((aligned(ARRAY_ALIGNMENT))) int indx[LEN_1D];
real_t* __restrict__ xx;
real_t* yy;



#include "common.h"
#include "array_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

void set_1d_array(real_t * arr, int length, real_t value, int stride);
void set_2d_array(real_t arr[LEN_2D][LEN_2D], real_t value, int stride);

enum {SET1D_RECIP_IDX = -1, SET1D_RECIP_IDX_SQ = -2};

real_t sum1d(real_t arr[LEN_1D]);
real_t sum2d(real_t arr[LEN_2D][LEN_2D]);

real_t sum_x();
real_t sum_a();
real_t sum_b();
real_t sum_c();
real_t sum_e();

real_t sum_half_xx();

real_t sum_a_aa();

real_t sum_aa();
real_t sum_bb();
real_t sum_cc();
real_t sum_xx();

real_t sum_aa_bb();

real_t sum_flat_2d_array();

real_t sum1d(real_t arr[LEN_1D]){
    real_t ret = 0.;
    for (int i = 0; i < LEN_1D; i++)
        ret += arr[i];
    return ret;
}

real_t sum2d(real_t arr[LEN_2D][LEN_2D]){
    real_t sum = 0.;
    for (int i = 0; i < LEN_2D; i++){
        for (int j = 0; j < LEN_2D; j++){
            sum += arr[i][j];
        }
    }

    return sum;
}

real_t sum_x()
{
    return sum1d(x);
}

real_t sum_xx()
{
    return sum1d(xx);
}

real_t sum_a()
{
    return sum1d(a);
}

real_t sum_b()
{
    return sum1d(b);
}

real_t sum_a_aa()
{
    return sum1d(a) + sum2d(aa);
}

real_t sum_c()
{
    return sum1d(c);
}

real_t sum_e()
{
    return sum1d(e);
}

real_t sum_aa()
{
    return sum2d(aa);
}

real_t sum_bb()
{
    return sum2d(bb);
}

real_t sum_aa_bb()
{
    return sum2d(aa) + sum2d(bb);
}

real_t sum_cc()
{
    return sum2d(cc);
}

real_t sum_half_xx()
{
    real_t temp = 00;

    for (int i = 0; i < LEN_1D/2; i++){
        temp += xx[i];
    }

    return temp;
}

real_t sum_flat_2d_array()
{
    real_t sum = 0.;

    for (int i = 0; i < LEN_2D*LEN_2D; i++){
        sum += flat_2d_array[i];
    }

    return sum;
}


void set_1d_array(real_t * arr, int length, real_t value, int stride)
{
    if (stride == SET1D_RECIP_IDX) {
        for (int i = 0; i < length; i++) {
            arr[i] = 1. / (real_t) (i+1);
        }
    } else if (stride == SET1D_RECIP_IDX_SQ) {
        for (int i = 0; i < length; i++) {
            arr[i] = 1. / (real_t) ((i+1) * (i+1));
        }
    } else {
        for (int i = 0; i < length; i += stride) {
            arr[i] = value;
        }
    }
}

void set_2d_array(real_t arr[LEN_2D][LEN_2D], real_t value, int stride)
{
    for (int i = 0; i < LEN_2D; i++) {
        set_1d_array(arr[i], LEN_2D, value, stride);
    }
}

void init(int** ip, real_t* s1, real_t* s2){
    xx = (real_t*) memalign(ARRAY_ALIGNMENT, LEN_1D*sizeof(real_t));
    *ip = (int *) memalign(ARRAY_ALIGNMENT, LEN_1D*sizeof(real_t));

    for (int i = 0; i < LEN_1D; i = i+5){
        (*ip)[i]   = (i+4);
        (*ip)[i+1] = (i+2);
        (*ip)[i+2] = (i);
        (*ip)[i+3] = (i+3);
        (*ip)[i+4] = (i+1);
    }

    set_1d_array(a, LEN_1D, 1.,1);
    set_1d_array(b, LEN_1D, 1.,1);
    set_1d_array(c, LEN_1D, 1.,1);
    set_1d_array(d, LEN_1D, 1.,1);
    set_1d_array(e, LEN_1D, 1.,1);
    set_1d_array(x, LEN_1D, 1.,1);
    set_2d_array(aa, 0.,SET1D_RECIP_IDX);
    set_2d_array(bb, 0.,SET1D_RECIP_IDX);
    set_2d_array(cc, 0.,SET1D_RECIP_IDX);

    for (int i = 0; i < LEN_1D; i++) {
        indx[i] = (i+1) % 4+1;
    }

    *s1 = 1.0;
    *s2 = 2.0;
}

int initialise_arrays(const char* name)
{
    real_t any=0.;
    real_t zero=0.;
    real_t half=.5;
    real_t one=1.;
    real_t two=2.;
    real_t small = .000001;
    int unit =1;
    int frac = SET1D_RECIP_IDX;
    int frac2 = SET1D_RECIP_IDX_SQ;

    printf("%5s\t", name);

    if    (!strcmp(name, "s000")) {
      for (int i = 0; i < LEN_1D; i++) {
            a[i] = 1+i;
            b[i] = 2+i;
            c[i] = 3+i;
            d[i] = 4+i;
            e[i] = 5+i;
          }
    } else if (!strcmp(name, "s111")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
        set_1d_array(c, LEN_1D, any,frac2);
        set_1d_array(d, LEN_1D, any,frac2);
        set_1d_array(e, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s112")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s113")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s114")) {
        set_2d_array(aa, any,frac);
        set_2d_array(bb, any,frac2);
    } else if (!strcmp(name, "s115")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_2d_array(aa,small,unit);
        set_2d_array(bb,small,unit);
        set_2d_array(cc,small,unit);
    } else if (!strcmp(name, "s116")) {
        set_1d_array(a, LEN_1D, one,unit);
    } else if (!strcmp(name, "s118")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_2d_array(bb,small,unit);
    } else if (!strcmp(name, "s119")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any,frac2);
    } else if (!strcmp(name, "s121")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s122")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s123")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s124")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s125")) {
        set_1d_array(flat_2d_array, LEN_2D*LEN_2D,zero,unit);
        set_2d_array(aa, one,unit);
        set_2d_array(bb,half,unit);
        set_2d_array(cc, two,unit);
    } else if (!strcmp(name, "s126")) {
        set_2d_array(bb, one,unit);
        set_1d_array( flat_2d_array, LEN_2D*LEN_2D,any,frac);
        set_2d_array(cc, any,frac);
    } else if (!strcmp(name, "s127")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s128")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, two,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, one,unit);
    } else if (!strcmp(name, "s131")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s132")) {
        set_2d_array(aa, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s141")) {
        set_1d_array( flat_2d_array, LEN_2D*LEN_2D, one,unit);
        set_2d_array(bb, any,frac2);
    } else if (!strcmp(name, "s151")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s152")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D,zero,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s161")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array( &b[0], LEN_1D/2, one,2);
        set_1d_array( &b[1], LEN_1D/2,-one,2);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s162")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s171")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s172")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s173")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s174")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s175")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s176")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s211")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s212")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s221")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s222")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
    } else if (!strcmp(name, "s231")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any,frac2);
    } else if (!strcmp(name, "s232")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb,zero,unit);
    } else if (!strcmp(name, "s233")) {
        set_2d_array(aa, any,frac);
        set_2d_array(bb, any,frac);
        set_2d_array(cc, any,frac);
    } else if (!strcmp(name, "s234")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any,frac);
        set_2d_array(cc, any,frac);
    } else if (!strcmp(name, "s235")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any, frac2);
    } else if (!strcmp(name, "s241")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, one,unit);
    } else if (!strcmp(name, "s242")) {
        set_1d_array(a, LEN_1D,small,unit);
        set_1d_array(b, LEN_1D,small,unit);
        set_1d_array(c, LEN_1D,small,unit);
        set_1d_array(d, LEN_1D,small,unit);
    } else if (!strcmp(name, "s243")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s244")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D,small,unit);
        set_1d_array(d, LEN_1D,small,unit);
    } else if (!strcmp(name, "s251")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s252")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
    } else if (!strcmp(name, "s253")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D,small,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s254")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
    } else if (!strcmp(name, "s255")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
    } else if (!strcmp(name, "s256")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_2d_array(aa, two,unit);
        set_2d_array(bb, one,unit);
    } else if (!strcmp(name, "s257")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_2d_array(aa, two,unit);
        set_2d_array(bb, one,unit);
    } else if (!strcmp(name, "s258")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D,zero,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D,zero,unit);
        set_2d_array(aa, any,frac);
    } else if (!strcmp(name, "s261")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
        set_1d_array(c, LEN_1D, any,frac2);
        set_1d_array(d, LEN_1D, one,unit);
    } else if (!strcmp(name, "s271")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s272")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, two,unit);
    } else if (!strcmp(name, "s273")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D,small,unit);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s274")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s275")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb,small,unit);
        set_2d_array(cc,small,unit);
    } else if (!strcmp(name, "s276")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s277")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array( b, LEN_1D/2, one,unit);
        set_1d_array( &b[LEN_1D/2], LEN_1D/2,-one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s278")) {
        set_1d_array( a, LEN_1D/2,-one,unit);
        set_1d_array( &a[LEN_1D/2], LEN_1D/2,one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s279")) {
        set_1d_array( a, LEN_1D/2,-one,unit);
        set_1d_array( &a[LEN_1D/2], LEN_1D/2,one,unit);
//        set_1d_array(a, LEN_1D, -one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s2710")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s2711")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s2712")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s281")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
    } else if (!strcmp(name, "1s281")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, one,unit);
        set_1d_array(e, LEN_1D, one,unit);
        set_1d_array(x, LEN_1D, one,unit);
    } else if (!strcmp(name, "s291")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
    } else if (!strcmp(name, "s292")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
    } else if (!strcmp(name, "s293")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s2101")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any,frac);
        set_2d_array(cc, any,frac);
    } else if (!strcmp(name, "s2102")) {
        set_2d_array(aa,zero,unit);
    } else if (!strcmp(name, "s2111")) {
        set_2d_array(aa, small,unit);
    } else if (!strcmp(name, "s311")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s312")) {
        set_1d_array(a, LEN_1D,1.000001,unit);
    } else if (!strcmp(name, "s313")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s314")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s315")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s316")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s317")) {
    } else if (!strcmp(name, "s318")) {
        set_1d_array(a, LEN_1D, any,frac);
        a[LEN_1D-1] = -two;
    } else if (!strcmp(name, "s319")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D,zero,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s3110")) {
        set_2d_array(aa, any,frac);
        aa[LEN_2D-1][LEN_2D-1] = two;
    } else if (!strcmp(name, "s3111")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "s3112")) {
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(b, LEN_1D,zero,unit);
    } else if (!strcmp(name, "s3113")) {
        set_1d_array(a, LEN_1D, any,frac);
        a[LEN_1D-1] = -two;
    } else if (!strcmp(name, "s321")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D,zero,unit);
    } else if (!strcmp(name, "s322")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D,zero,unit);
        set_1d_array(c, LEN_1D,zero,unit);
    } else if (!strcmp(name, "s323")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s331")) {
        set_1d_array(a, LEN_1D, any,frac);
        a[LEN_1D-1] = -one;
    } else if (!strcmp(name, "s332")) {
        set_1d_array(a, LEN_1D, any,frac2);
        a[LEN_1D-1] = two;
    } else if (!strcmp(name, "s341")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s342")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s343")) {
        set_2d_array(aa, any,frac);
        set_2d_array(bb, one,unit);
    } else if (!strcmp(name, "s351")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        c[0] = 1.;
    } else if (!strcmp(name, "s352")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s353")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        c[0] = 1.;
    } else if (!strcmp(name, "s411")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s412")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s413")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s414")) {
        set_2d_array(aa, one,unit);
        set_2d_array(bb, any,frac);
        set_2d_array(cc, any,frac);
    } else if (!strcmp(name, "s415")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        a[LEN_1D-1] = -one;
    } else if (!strcmp(name, "s421")) {
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(flat_2d_array, LEN_1D, one, unit);
    } else if (!strcmp(name, "s422")) {
        set_1d_array(flat_2d_array, LEN_1D,one,unit);
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(flat_2d_array, LEN_1D, zero, unit);
    } else if (!strcmp(name, "s1421")) {
        set_1d_array(b, LEN_1D, one, unit);
    } else if (!strcmp(name, "s423")) {
        set_1d_array(flat_2d_array, LEN_1D,zero,unit);
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(flat_2d_array, LEN_1D, one, unit);
    } else if (!strcmp(name, "s424")) {
        set_1d_array(flat_2d_array, LEN_1D,one,unit);
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(flat_2d_array, LEN_1D, zero, unit);
    } else if (!strcmp(name, "s431")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s432")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s441")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(&d[0],             LEN_1D/3  , -one,unit);
        set_1d_array(&d[LEN_1D/3],      LEN_1D/3  , zero,unit);
        set_1d_array(&d[(2*LEN_1D/3)],  LEN_1D/3+1, one,unit);
    } else if (!strcmp(name, "s442")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
    } else if (!strcmp(name, "s443")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s451")) {
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s452")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D,small,unit);
    } else if (!strcmp(name, "s453")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s471")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, one,unit);
        set_1d_array(d, LEN_1D, any,frac);
        set_1d_array(e, LEN_1D, any,frac);
        set_1d_array(x, LEN_1D, zero, unit);
    } else if (!strcmp(name, "s481")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s482")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "s491")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s4112")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s4113")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac2);
    } else if (!strcmp(name, "s4114")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s4115")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "s4116")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_2d_array(aa, any,frac);
    } else if (!strcmp(name, "s4117")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D, any,frac);
        set_1d_array(d, LEN_1D, any,frac);
    } else if (!strcmp(name, "s4121")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "va")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vag")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vas")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vif")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vpv")) {
        set_1d_array(a, LEN_1D,zero,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vtv")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, one,unit);
    } else if (!strcmp(name, "vpvtv")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, any,frac);
    } else if (!strcmp(name, "vpvts")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, any,frac2);
    } else if (!strcmp(name, "vpvpv")) {
        set_1d_array(a, LEN_1D, any,frac2);
        set_1d_array(b, LEN_1D, one,unit);
        set_1d_array(c, LEN_1D,-one,unit);
    } else if (!strcmp(name, "vtvtv")) {
        set_1d_array(a, LEN_1D, one,unit);
        set_1d_array(b, LEN_1D, two,unit);
        set_1d_array(c, LEN_1D,half,unit);
    } else if (!strcmp(name, "vsumr")) {
        set_1d_array(a, LEN_1D, any,frac);
    } else if (!strcmp(name, "vdotr")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
    } else if (!strcmp(name, "vbor")) {
        set_1d_array(a, LEN_1D, any,frac);
        set_1d_array(b, LEN_1D, any,frac);
        set_1d_array(c, LEN_1D, one,frac);
        set_1d_array(d, LEN_1D, two,frac);
        set_1d_array(e, LEN_1D,half,frac);
        set_2d_array(aa, any,frac);
    } else {
    }

    return 0;
}

real_t calc_checksum(const char * name)
{
    if (!strcmp(name, "s000")) {
        return sum_a();
    } else if (!strcmp(name, "s111")) {
        return sum_a();
    } else if (!strcmp(name, "s1111")) {
        return sum_a();
    } else if (!strcmp(name, "s112")) {
        return sum_a();
    } else if (!strcmp(name, "s1112")) {
        return sum_a();
    } else if (!strcmp(name, "s113")) {
        return sum_a();
    } else if (!strcmp(name, "s1113")) {
        return sum_a();
    } else if (!strcmp(name, "s114")) {
        return sum_aa();
    } else if (!strcmp(name, "s115")) {
        return sum_a();
    } else if (!strcmp(name, "s1115")) {
        return sum_aa();
    } else if (!strcmp(name, "s116")) {
        return sum_a();
    } else if (!strcmp(name, "s118")) {
        return sum_a();
    } else if (!strcmp(name, "s119")) {
        return sum_aa();
    } else if (!strcmp(name, "s1119")) {
        return sum_aa();
    } else if (!strcmp(name, "s121")) {
        return sum_a();
    } else if (!strcmp(name, "s122")) {
        return sum_a();
    } else if (!strcmp(name, "s123")) {
        return sum_a();
    } else if (!strcmp(name, "s124")) {
        return sum_a();
    } else if (!strcmp(name, "s125")) {
        return sum_flat_2d_array();
    } else if (!strcmp(name, "s126")) {
        return sum_bb();
    } else if (!strcmp(name, "s127")) {
        return sum_a();
    } else if (!strcmp(name, "s128")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s131")) {
        return sum_a();
    } else if (!strcmp(name, "s132")) {
        return sum_aa();
    } else if (!strcmp(name, "s141")) {
        return sum_flat_2d_array();
    } else if (!strcmp(name, "s151")) {
        return sum_a();
    } else if (!strcmp(name, "s152")) {
        return sum_a();
    } else if (!strcmp(name, "s161")) {
        return sum_a() + sum_c();
    } else if (!strcmp(name, "s1161")) {
        return sum_a() + sum_c();
    } else if (!strcmp(name, "s162")) {
        return sum_a();
    } else if (!strcmp(name, "s171")) {
        return sum_a();
    } else if (!strcmp(name, "s172")) {
        return sum_a();
    } else if (!strcmp(name, "s173")) {
        return sum_a();
    } else if (!strcmp(name, "s174")) {
        return sum_a();
    } else if (!strcmp(name, "s175")) {
        return sum_a();
    } else if (!strcmp(name, "s176")) {
        return sum_a();
    } else if (!strcmp(name, "s211")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s212")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s1213")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s221")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s1221")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s222")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s231")) {
        return sum_aa();
    } else if (!strcmp(name, "s232")) {
        return sum_aa();
    } else if (!strcmp(name, "s1232")) {
        return sum_aa();
    } else if (!strcmp(name, "s233")) {
        return sum_aa_bb();
    } else if (!strcmp(name, "s2233")) {
        return sum_aa_bb();
    } else if (!strcmp(name, "s235")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s241")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s242")) {
        return sum_a();
    } else if (!strcmp(name, "s243")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s244")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s1244")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s2244")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s251")) {
        return sum_a();
    } else if (!strcmp(name, "s1251")) {
        return sum_a();
    } else if (!strcmp(name, "s2251")) {
        return sum_a();
    } else if (!strcmp(name, "s3251")) {
        return sum_a();
    } else if (!strcmp(name, "s252")) {
        return sum_a();
    } else if (!strcmp(name, "s253")) {
        return sum_a() + sum_c();
    } else if (!strcmp(name, "s254")) {
        return sum_a();
    } else if (!strcmp(name, "s255")) {
        return sum_a();
    } else if (!strcmp(name, "s256")) {
        return sum_a_aa();
    } else if (!strcmp(name, "s257")) {
        return sum_a_aa();
    } else if (!strcmp(name, "s258")) {
        return sum_b() + sum_e();
    } else if (!strcmp(name, "s261")) {
        return sum_a() + sum_c();
    } else if (!strcmp(name, "s271")) {
        return sum_a();
    } else if (!strcmp(name, "s272")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s273")) {
        return sum_a() + sum_b() + sum_c();
    } else if (!strcmp(name, "s274")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s275")) {
        return sum_aa();
    } else if (!strcmp(name, "s2275")) {
        return sum_aa();
    } else if (!strcmp(name, "s276")) {
        return sum_a();
    } else if (!strcmp(name, "s277")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s278")) {
        return sum_a() + sum_b() + sum_c();
    } else if (!strcmp(name, "s279")) {
        return sum_a() + sum_b() + sum_c();
    } else if (!strcmp(name, "s1279")) {
        return sum_a() + sum_b() + sum_c();
    } else if (!strcmp(name, "s2710")) {
        return sum_a() + sum_b() + sum_c();
    } else if (!strcmp(name, "s2711")) {
        return sum_a();
    } else if (!strcmp(name, "s2712")) {
        return sum_a();
    } else if (!strcmp(name, "s281")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s1281")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s291")) {
        return sum_a();
    } else if (!strcmp(name, "s292")) {
        return sum_a();
    } else if (!strcmp(name, "s293")) {
        return sum_a();
    } else if (!strcmp(name, "s2101")) {
        return sum_aa();
    } else if (!strcmp(name, "s2102")) {
        return sum_aa();
    } else if (!strcmp(name, "s2111")) {
        return sum_aa();
    } else if (!strcmp(name, "s311")) {
        return sum_a();
    } else if (!strcmp(name, "s31111")) {
        return sum_a();
    } else if (!strcmp(name, "s321")) {
        return sum_a();
    } else if (!strcmp(name, "s322")) {
        return sum_a();
    } else if (!strcmp(name, "s323")) {
        return sum_a() + sum_b();
    } else if (!strcmp(name, "s341")) {
        return sum_a();
    } else if (!strcmp(name, "s342")) {
        return sum_a();
    } else if (!strcmp(name, "s343")) {
        return sum_flat_2d_array();
    } else if (!strcmp(name, "s351")) {
        return sum_a();
    } else if (!strcmp(name, "s1351")) {
        return sum_a();
    } else if (!strcmp(name, "s353")) {
        return sum_a();
    } else if (!strcmp(name, "s421")) {
        return sum_xx();
    } else if (!strcmp(name, "s1421")) {
        return sum_half_xx();
    } else if (!strcmp(name, "s422")) {
        return sum_xx();
    } else if (!strcmp(name, "s423")) {
        return sum_flat_2d_array();
    } else if (!strcmp(name, "s424")) {
        return sum_xx();
    } else if (!strcmp(name, "s431")) {
        return sum_a();
    } else if (!strcmp(name, "s441")) {
        return sum_a();
    } else if (!strcmp(name, "s442")) {
        return sum_a();
    } else if (!strcmp(name, "s443")) {
        return sum_a();
    } else if (!strcmp(name, "s451")) {
        return sum_a();
    } else if (!strcmp(name, "s452")) {
        return sum_a();
    } else if (!strcmp(name, "s453")) {
        return sum_a();
    } else if (!strcmp(name, "s471")) {
        return sum_x() + sum_b();
    } else if (!strcmp(name, "s481")) {
        return sum_a();
    } else if (!strcmp(name, "s482")) {
        return sum_a();
    } else if (!strcmp(name, "s491")) {
        return sum_a();
    } else if (!strcmp(name, "s4112")) {
        return sum_a();
    } else if (!strcmp(name, "s4113")) {
        return sum_a();
    } else if (!strcmp(name, "s4114")) {
        return sum_a();
    } else if (!strcmp(name, "s4117")) {
        return sum_a();
    } else if (!strcmp(name, "s4121")) {
        return sum_a();
    } else if (!strcmp(name, "va")) {
        return sum_a();
    } else if (!strcmp(name, "vag")) {
        return sum_a();
    } else if (!strcmp(name, "vas")) {
        return sum_a();
    } else if (!strcmp(name, "vif")) {
        return sum_a();
    } else if (!strcmp(name, "vpv")) {
        return sum_a();
    } else if (!strcmp(name, "vtv")) {
        return sum_a();
    } else if (!strcmp(name, "vpvtv")) {
        return sum_a();
    } else if (!strcmp(name, "vpvts")) {
        return sum_a();
    } else if (!strcmp(name, "vpvpv")) {
        return sum_a();
    } else if (!strcmp(name, "vtvtv")) {
        return sum_a();
    } else if (!strcmp(name, "vsumr")) {
        return sum_a();
    } else if (!strcmp(name, "vbor")) {
        return sum_x();
    } else {
        fprintf(stderr, "Unknown function name passed to calc_checksum: %s\n", name);
        exit(1);
    }
}




#include "common.h"

int dummy(float a[LEN_1D], float b[LEN_1D], float c[LEN_1D], float d[LEN_1D], float e[LEN_1D], float aa[LEN_2D][LEN_2D], float bb[LEN_2D][LEN_2D], float cc[LEN_2D][LEN_2D], float s){
    // --  called in each loop to make all computations appear required
    return 0;
}

