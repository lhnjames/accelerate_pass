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


/* ==== aes.c ==== */

 /*
   -----------------------------------------------------------------------
   Copyright (c) 2001 Dr Brian Gladman <brg@gladman.uk.net>, Worcester, UK
   
   TERMS

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   This software is provided 'as is' with no guarantees of correctness or
   fitness for purpose.
   -----------------------------------------------------------------------

   1. FUNCTION
 
   The AES algorithm Rijndael implemented for block and key sizes of 128,
   192 and 256 bits (16, 24 and 32 bytes) by Brian Gladman.

   This is an implementation of the AES encryption algorithm (Rijndael)
   designed by Joan Daemen and Vincent Rijmen. This version is designed
   to provide both fixed and dynamic block and key lengths and can also 
   run with either big or little endian internal byte order (see aes.h). 
   It inputs block and key lengths in bytes with the legal values being 
   16, 24 and 32.
 
   2. CONFIGURATION OPTIONS (see also aes.h)
 
   a.  Define UNROLL for full loop unrolling in encryption and decryption.
   b.  Define PARTIAL_UNROLL to unroll two loops in encryption and decryption.
   c.  Define FIXED_TABLES for compiled rather than dynamic tables.
   d.  Define FF_TABLES to use tables for field multiplies and inverses.
   e.  Define ARRAYS to use arrays to hold the local state block. If this
       is not defined, individually declared 32-bit words are used.
   f.  Define FAST_VARIABLE if a high speed variable block implementation
       is needed (essentially three separate fixed block size code sequences)
   g.  Define either ONE_TABLE or FOUR_TABLES for a fast table driven 
       version using 1 table (2 kbytes of table space) or 4 tables (8
       kbytes of table space) for higher speed.
   h.  Define either ONE_LR_TABLE or FOUR_LR_TABLES for a further speed 
       increase by using tables for the last rounds but with more table
       space (2 or 8 kbytes extra).
   i.  If neither ONE_TABLE nor FOUR_TABLES is defined, a compact but 
       slower version is provided.
   j.  If fast decryption key scheduling is needed define ONE_IM_TABLE
       or FOUR_IM_TABLES for higher speed (2 or 8 kbytes extra).

   3. USE OF DEFINES
  
   NOTE: some combinations of the following defines are disabled below.

   UNROLL or PARTIAL_UNROLL control the extent to which loops are unrolled
   in the main encryption and decryption routines. UNROLL does a complete
   unroll while PARTIAL_UNROLL uses a loop with two rounds in it.
 
#define UNROLL
#define PARTIAL_UNROLL
 
   If FIXED_TABLES is defined, the tables are comipled statically into the 
   code, otherwise they are computed once when the code is first used.
 
#define FIXED_TABLES
 
   If FF_TABLES is defined faster finite field arithmetic is performed by 
   using tables.
 
#define FF_TABLES

   If ARRAYS is defined the state variables for encryption are defined as
   arrays, otherwise they are defined as individual variables. The latter
   is useful on machines where these variables can be mapped to registers. 
 
#define ARRAYS

   If FAST_VARIABLE is defined with variable block length, faster but larger
   code is used for encryption and decryption.

#define FAST_VARIABLE
 */

#define UNROLL
#define FIXED_TABLES
#define FF_TABLES
#define ARRAYS
#define FAST_VARIABLE

 /*
   This code uses three sets of tables, each of which can be a single table
   or four sub-tables to gain a further speed advantage.

   The defines ONE_TABLE and FOUR_TABLES control the use of tables in the 
   main encryption rounds and have the greatest impact on speed.  If neither
   is defined, tables are not used and the resulting code is then very slow.
   Defining ONE_TABLE gives a substantial speed increase using 2 kbytes of 
   table space; FOUR_TABLES gives a further speed increase but uses 8 kbytes
   of table space.
   
#define ONE_TABLE
#define FOUR_TABLES

   The defines ONE_LR_TABLE and FOUR_LR_TABLES apply to the last round only
   and their impact on speed is hence less. It is unlikely to be sensible to
   apply these options unless the correspnding option above is also used.    

#define ONE_LR_TABLE
#define FOUR_LR_TABLES

   The ONE_IM_TABLE and FOUR_IM_TABLES options use tables to speed up the 
   generation of the decryption key schedule. This will only be useful in
   limited situations where decryption speed with frequent re-keying is
   needed.

#define ONE_IM_TABLE
#define FOUR_IM_TABLES
 */

#include "aes.h"

#define FOUR_TABLES
#define FOUR_LR_TABLES
#define FOUR_IM_TABLES

 /*
   In this implementation the columns of the state array are each held in
   32-bit words. The state array can be held in various ways: in an array
   of words, in a number of individual word variables or in a number of 
   processor registers. The following define maps a variable name x and
   a column number c to the way the state array variable is to be held.
 */

#if defined(ARRAYS)
#define s(x,c) x[c]
#else
#define s(x,c) x##c
#endif

#if defined(BLOCK_SIZE) && (BLOCK_SIZE == 20 || BLOCK_SIZE == 28)
#error an illegal block size has been specified
#endif  

#if defined(UNROLL) && defined (PARTIAL_UNROLL)
#error both UNROLL and PARTIAL_UNROLL are defined
#endif

#if defined(ONE_TABLE) && defined (FOUR_TABLES)
#error both ONE_TABLE and FOUR_TABLES are defined
#endif

#if defined(ONE_LR_TABLE) && defined (FOUR_LR_TABLES)
#error both ONE_LR_TABLE and FOUR_LR_TABLES are defined
#endif

#if defined(ONE_IM_TABLE) && defined (FOUR_IM_TABLES)
#error both ONE_IM_TABLE and FOUR_IM_TABLES are defined
#endif

/* End of configuration options */
/* Disable at least some poor combinations of options */

#if !defined(ONE_TABLE) && !defined(FOUR_TABLES)
#define FIXED_TABLES
#undef  UNROLL
#undef  ONE_LR_TABLE
#undef  FOUR_LR_TABLES
#undef  ONE_IM_TABLE
#undef  FOUR_IM_TABLES
#elif !defined(FOUR_TABLES)
#ifdef  FOUR_LR_TABLES
#undef  FOUR_LR_TABLES
#define ONE_LR_TABLE
#endif
#ifdef  FOUR_IM_TABLES
#undef  FOUR_IM_TABLES
#define ONE_IM_TABLE
#endif
#elif !defined(BLOCK_SIZE)
#if defined(UNROLL)
#define PARTIAL_UNROLL
#undef UNROLL
#endif
#endif

/* the finite field modular polynomial and elements */

#define ff_poly 0x011b
#define ff_hi   0x80

/* multiply four bytes in GF(2^8) by 'x' {02} in parallel */

#define m1  0x80808080
#define m2  0x7f7f7f7f
#define m3  0x0000001b
#define FFmulX(x)  ((((x) & m2) << 1) ^ ((((x) & m1) >> 7) * m3))

 /* 
   The following defines provide alternative definitions of FFmulX that might
   give improved performance if a fast 32-bit multiply is not available. Note
   that a temporary variable u needs to be defined where FFmulX is used.

#define FFmulX(x) (u = (x) & m1, u |= (u >> 1), ((x) & m2) << 1) ^ ((u >> 3) | (u >> 6)) 
#define m4  0x1b1b1b1b
#define FFmulX(x) (u = (x) & m1, ((x) & m2) << 1) ^ ((u - (u >> 7)) & m4) 

 */

/* perform column mix operation on four bytes in parallel */

#define fwd_mcol(x) (f2 = FFmulX(x), f2 ^ upr(x ^ f2,3) ^ upr(x,2) ^ upr(x,1))

#if defined(FIXED_TABLES)

#include "aestab.h"

#else

static byte  s_box[256];
static byte  inv_s_box[256];
static word  rcon_tab[RC_LENGTH];

#if defined(ONE_TABLE)
static word  ft_tab[256];
static word  it_tab[256];
#elif defined(FOUR_TABLES)
static word  ft_tab[4][256];
static word  it_tab[4][256];
#endif

#if defined(ONE_LR_TABLE)
static word  fl_tab[256];
static word  il_tab[256];
#elif defined(FOUR_LR_TABLES)
static word  fl_tab[4][256];
static word  il_tab[4][256];
#endif

#if defined(ONE_IM_TABLE)
static word  im_tab[256];
#elif defined(FOUR_IM_TABLES)
static word  im_tab[4][256];
#endif

#if !defined(FF_TABLES)

/*
   Generate the tables for the dynamic table option

   It will generally be sensible to use tables to compute finite 
   field multiplies and inverses but where memory is scarse this 
   code might sometimes be better.

   return 2 ^ (n - 1) where n is the bit number of the highest bit
   set in x with x in the range 1 < x < 0x00000200.   This form is
   used so that locals within FFinv can be bytes rather than words
*/

static byte hibit(const word x)
{   byte r = (byte)((x >> 1) | (x >> 2));
    
    r |= (r >> 2);
    r |= (r >> 4);
    return (r + 1) >> 1;
}

/* return the inverse of the finite field element x */

static byte FFinv(const byte x)
{   byte    p1 = x, p2 = 0x1b, n1 = hibit(x), n2 = 0x80, v1 = 1, v2 = 0;

    if(x < 2) return x;

    for(;;)
    {
        if(!n1) return v1;

        while(n2 >= n1)
        {   
            n2 /= n1; p2 ^= p1 * n2; v2 ^= v1 * n2; n2 = hibit(p2);
        }
        
        if(!n2) return v2;

        while(n1 >= n2)
        {   
            n1 /= n2; p1 ^= p2 * n1; v1 ^= v2 * n1; n1 = hibit(p1);
        }
    }
}

/* define the finite field multiplies required for Rijndael */

#define FFmul02(x)  ((((x) & 0x7f) << 1) ^ ((x) & 0x80 ? 0x1b : 0))
#define FFmul03(x)  ((x) ^ FFmul02(x))
#define FFmul09(x)  ((x) ^ FFmul02(FFmul02(FFmul02(x))))
#define FFmul0b(x)  ((x) ^ FFmul02((x) ^ FFmul02(FFmul02(x))))
#define FFmul0d(x)  ((x) ^ FFmul02(FFmul02((x) ^ FFmul02(x))))
#define FFmul0e(x)  FFmul02((x) ^ FFmul02((x) ^ FFmul02(x)))

#else

#define FFinv(x)    ((x) ? pow[255 - log[x]]: 0)

#define FFmul02(x) (x ? pow[log[x] + 0x19] : 0)
#define FFmul03(x) (x ? pow[log[x] + 0x01] : 0)
#define FFmul09(x) (x ? pow[log[x] + 0xc7] : 0)
#define FFmul0b(x) (x ? pow[log[x] + 0x68] : 0)
#define FFmul0d(x) (x ? pow[log[x] + 0xee] : 0)
#define FFmul0e(x) (x ? pow[log[x] + 0xdf] : 0)

#endif

/* The forward and inverse affine transformations used in the S-box */

#define fwd_affine(x)     (w = (word)x, w ^= (w<<1)^(w<<2)^(w<<3)^(w<<4), 0x63^(byte)(w^(w>>8)))

#define inv_affine(x)     (w = (word)x, w = (w<<1)^(w<<3)^(w<<6), 0x05^(byte)(w^(w>>8)))

static void gen_tabs(void)
{   word  i, w;

#if defined(FF_TABLES)

    byte  pow[512], log[256];

    /*
       log and power tables for GF(2^8) finite field with
       0x011b as modular polynomial - the simplest primitive
       root is 0x03, used here to generate the tables
    */

    i = 0; w = 1; 
    do
    {   
        pow[i] = (byte)w;
        pow[i + 255] = (byte)w;
        log[w] = (byte)i++;
        w ^=  (w << 1) ^ (w & ff_hi ? ff_poly : 0);
    }
    while (w != 1);

#endif

    for(i = 0, w = 1; i < RC_LENGTH; ++i)
    {
        rcon_tab[i] = bytes2word(w, 0, 0, 0);
        w = (w << 1) ^ (w & ff_hi ? ff_poly : 0);
    }

    for(i = 0; i < 256; ++i)
    {   byte    b;

        s_box[i] = b = fwd_affine(FFinv((byte)i));

        w = bytes2word(b, 0, 0, 0);
#if defined(ONE_LR_TABLE)
        fl_tab[i] = w;
#elif defined(FOUR_LR_TABLES)
        fl_tab[0][i] = w;
        fl_tab[1][i] = upr(w,1);
        fl_tab[2][i] = upr(w,2);
        fl_tab[3][i] = upr(w,3);
#endif
        w = bytes2word(FFmul02(b), b, b, FFmul03(b));
#if defined(ONE_TABLE)
        ft_tab[i] = w;
#elif defined(FOUR_TABLES)
        ft_tab[0][i] = w;
        ft_tab[1][i] = upr(w,1);
        ft_tab[2][i] = upr(w,2);
        ft_tab[3][i] = upr(w,3);
#endif
        inv_s_box[i] = b = FFinv(inv_affine((byte)i));

        w = bytes2word(b, 0, 0, 0);
#if defined(ONE_LR_TABLE)
        il_tab[i] = w;
#elif defined(FOUR_LR_TABLES)
        il_tab[0][i] = w;
        il_tab[1][i] = upr(w,1);
        il_tab[2][i] = upr(w,2);
        il_tab[3][i] = upr(w,3);
#endif
        w = bytes2word(FFmul0e(b), FFmul09(b), FFmul0d(b), FFmul0b(b));
#if defined(ONE_TABLE)
        it_tab[i] = w;
#elif defined(FOUR_TABLES)
        it_tab[0][i] = w;
        it_tab[1][i] = upr(w,1);
        it_tab[2][i] = upr(w,2);
        it_tab[3][i] = upr(w,3);
#endif
#if defined(ONE_IM_TABLE)
        im_tab[b] = w;
#elif defined(FOUR_IM_TABLES)
        im_tab[0][b] = w;
        im_tab[1][b] = upr(w,1);
        im_tab[2][b] = upr(w,2);
        im_tab[3][b] = upr(w,3);
#endif

    }
}

#endif

#define no_table(x,box,vf,rf,c) bytes2word(     box[bval(vf(x,0,c),rf(0,c))],     box[bval(vf(x,1,c),rf(1,c))],     box[bval(vf(x,2,c),rf(2,c))],     box[bval(vf(x,3,c),rf(3,c))])

#define one_table(x,op,tab,vf,rf,c)  (     tab[bval(vf(x,0,c),rf(0,c))]   ^ op(tab[bval(vf(x,1,c),rf(1,c))],1)   ^ op(tab[bval(vf(x,2,c),rf(2,c))],2)   ^ op(tab[bval(vf(x,3,c),rf(3,c))],3))

#define four_tables(x,tab,vf,rf,c)  (  tab[0][bval(vf(x,0,c),rf(0,c))]   ^ tab[1][bval(vf(x,1,c),rf(1,c))]   ^ tab[2][bval(vf(x,2,c),rf(2,c))]   ^ tab[3][bval(vf(x,3,c),rf(3,c))])

#define vf1(x,r,c)  (x)
#define rf1(r,c)    (r)
#define rf2(r,c)    ((r-c)&3)

#if defined(FOUR_LR_TABLES)
#define ls_box(x,c)     four_tables(x,fl_tab,vf1,rf2,c)
#elif defined(ONE_LR_TABLE)
#define ls_box(x,c)     one_table(x,upr,fl_tab,vf1,rf2,c)
#else
#define ls_box(x,c)     no_table(x,s_box,vf1,rf2,c)
#endif

#if defined(FOUR_IM_TABLES)
#define inv_mcol(x)     four_tables(x,im_tab,vf1,rf1,0)
#elif defined(ONE_IM_TABLE)
#define inv_mcol(x)     one_table(x,upr,im_tab,vf1,rf1,0)
#else
#define inv_mcol(x)     (f9 = (x),f2 = FFmulX(f9), f4 = FFmulX(f2), f8 = FFmulX(f4), f9 ^= f8,     f2 ^= f4 ^ f8 ^ upr(f2 ^ f9,3) ^ upr(f4 ^ f9,2) ^ upr(f9,1))
#endif

 /* 
   Subroutine to set the block size (if variable) in bytes, legal
   values being 16, 24 and 32.
 */

#if defined(BLOCK_SIZE)
#define nc   (Ncol)
#else
#define nc   (cx->Ncol)

cf_dec c_name(set_blk)(const word n_bytes, c_name(aes) *cx)
{
#if !defined(FIXED_TABLES)
    if(!(cx->mode & 0x08)) { gen_tabs(); cx->mode = 0x08; }
#endif

    if((n_bytes & 7) || n_bytes < 16 || n_bytes > 32) 
    {     
        return (n_bytes ? cx->mode &= ~0x07, aes_bad : (aes_ret)(nc << 2));
    }

    cx->mode = cx->mode & ~0x07 | 0x04;
    nc = n_bytes >> 2;
    return aes_good;
}

#endif

 /*
   Initialise the key schedule from the user supplied key. The key
   length is now specified in bytes - 16, 24 or 32 as appropriate.
   This corresponds to bit lengths of 128, 192 and 256 bits, and
   to Nk values of 4, 6 and 8 respectively.
 */

#define mx(t,f) (*t++ = inv_mcol(*f),f++)
#define cp(t,f) *t++ = *f++

#if   BLOCK_SIZE == 16
#define cpy(d,s)    cp(d,s); cp(d,s); cp(d,s); cp(d,s)
#define mix(d,s)    mx(d,s); mx(d,s); mx(d,s); mx(d,s)
#elif BLOCK_SIZE == 24
#define cpy(d,s)    cp(d,s); cp(d,s); cp(d,s); cp(d,s);                     cp(d,s); cp(d,s)
#define mix(d,s)    mx(d,s); mx(d,s); mx(d,s); mx(d,s);                     mx(d,s); mx(d,s)
#elif BLOCK_SIZE == 32
#define cpy(d,s)    cp(d,s); cp(d,s); cp(d,s); cp(d,s);                     cp(d,s); cp(d,s); cp(d,s); cp(d,s)
#define mix(d,s)    mx(d,s); mx(d,s); mx(d,s); mx(d,s);                     mx(d,s); mx(d,s); mx(d,s); mx(d,s)
#else

#define cpy(d,s) switch(nc) {   case 8: cp(d,s); cp(d,s);     case 6: cp(d,s); cp(d,s);     case 4: cp(d,s); cp(d,s);             cp(d,s); cp(d,s); }

#define mix(d,s) switch(nc) {   case 8: mx(d,s); mx(d,s);     case 6: mx(d,s); mx(d,s);     case 4: mx(d,s); mx(d,s);             mx(d,s); mx(d,s); }

#endif

cf_dec c_name(set_key)(const byte in_key[], const word n_bytes, const enum aes_key f, c_name(aes) *cx)
{   word    *kf, *kt, rci;

#if !defined(FIXED_TABLES)
    if(!(cx->mode & 0x08)) { gen_tabs(); cx->mode = 0x08; }
#endif

#if !defined(BLOCK_SIZE)
    if(!(cx->mode & 0x04)) c_name(set_blk)(16, cx);
#endif

    if ((n_bytes & 7) || (n_bytes < 16) || (n_bytes > 32) || (!(f & 1) && !(f & 2))) 
    {     
        return (n_bytes ? cx->mode &= ~0x03, aes_bad : (aes_ret)(cx->Nkey << 2));
    }

    cx->mode = (cx->mode & ~0x03) | ((byte)f & 0x03);
    cx->Nkey = n_bytes >> 2;
    cx->Nrnd = Nr(cx->Nkey, nc);

    cx->e_key[0] = word_in(in_key     );
    cx->e_key[1] = word_in(in_key +  4);
    cx->e_key[2] = word_in(in_key +  8);
    cx->e_key[3] = word_in(in_key + 12);

    kf = cx->e_key; 
    kt = kf + nc * (cx->Nrnd + 1) - cx->Nkey; 
    rci = 0;

    switch(cx->Nkey)
    {
    case 4: do
            {   kf[4] = kf[0] ^ ls_box(kf[3],3) ^ rcon_tab[rci++];
                kf[5] = kf[1] ^ kf[4];
                kf[6] = kf[2] ^ kf[5];
                kf[7] = kf[3] ^ kf[6];
                kf += 4;
            }
            while(kf < kt);
            break;

    case 6: cx->e_key[4] = word_in(in_key + 16);
            cx->e_key[5] = word_in(in_key + 20);
            do
            {   kf[ 6] = kf[0] ^ ls_box(kf[5],3) ^ rcon_tab[rci++];
                kf[ 7] = kf[1] ^ kf[ 6];
                kf[ 8] = kf[2] ^ kf[ 7];
                kf[ 9] = kf[3] ^ kf[ 8];
                kf[10] = kf[4] ^ kf[ 9];
                kf[11] = kf[5] ^ kf[10];
                kf += 6;
            }
            while(kf < kt);
            break;

    case 8: cx->e_key[4] = word_in(in_key + 16);
            cx->e_key[5] = word_in(in_key + 20);
            cx->e_key[6] = word_in(in_key + 24);
            cx->e_key[7] = word_in(in_key + 28);
            do
            {   kf[ 8] = kf[0] ^ ls_box(kf[7],3) ^ rcon_tab[rci++];
                kf[ 9] = kf[1] ^ kf[ 8];
                kf[10] = kf[2] ^ kf[ 9];
                kf[11] = kf[3] ^ kf[10];
                kf[12] = kf[4] ^ ls_box(kf[11],0);
                kf[13] = kf[5] ^ kf[12];
                kf[14] = kf[6] ^ kf[13];
                kf[15] = kf[7] ^ kf[14];
                kf += 8;
            }
            while (kf < kt);
            break;
    }

    if((cx->mode & 3) != enc)
    {   word    i;
        
        kt = cx->d_key + nc * cx->Nrnd;
        kf = cx->e_key;
        
        cpy(kt, kf); kt -= 2 * nc;

        for(i = 1; i < cx->Nrnd; ++i)
        { 
#if defined(ONE_TABLE) || defined(FOUR_TABLES)
#if !defined(ONE_IM_TABLE) && !defined(FOUR_IM_TABLES)
            word    f2, f4, f8, f9;
#endif
            mix(kt, kf);
#else
            cpy(kt, kf);
#endif
            kt -= 2 * nc;
        }
        
        cpy(kt, kf);
    }

    return aes_good;
}

 /*
   I am grateful to Frank Yellin for the following constructions
   which, given the column (c) of the output state variable, give
   the input state variables which are needed for each row (r) of 
   the state.

   For the fixed block size options, compilers should reduce these 
   two expressions to fixed variable references. But for variable 
   block size code conditional clauses will sometimes be returned.

   y = output word, x = input word, r = row, c = column for r = 0, 
   1, 2 and 3 = column accessed for row r.
 */

#define unused  77  /* Sunset Strip */

#define fwd_var(x,r,c)  ( r==0 ?               ( c==0 ? s(x,0)     : c==1 ? s(x,1)     : c==2 ? s(x,2)     : c==3 ? s(x,3)     : c==4 ? s(x,4)     : c==5 ? s(x,5)     : c==6 ? s(x,6)     : s(x,7))        : r==1 ?               ( c==0 ? s(x,1)     : c==1 ? s(x,2)     : c==2 ? s(x,3)     : c==3 ? nc==4 ? s(x,0) : s(x,4)     : c==4 ? s(x,5)     : c==5 ? nc==8 ? s(x,6) : s(x,0)     : c==6 ? s(x,7)     : s(x,0))        : r==2 ?               ( c==0 ? nc==8 ? s(x,3) : s(x,2)     : c==1 ? nc==8 ? s(x,4) : s(x,3)     : c==2 ? nc==4 ? s(x,0) : nc==8 ? s(x,5) : s(x,4)     : c==3 ? nc==4 ? s(x,1) : nc==8 ? s(x,6) : s(x,5)     : c==4 ? nc==8 ? s(x,7) : s(x,0)     : c==5 ? nc==8 ? s(x,0) : s(x,1)     : c==6 ? s(x,1)     : s(x,2))        :                      ( c==0 ? nc==8 ? s(x,4) : s(x,3)     : c==1 ? nc==4 ? s(x,0) : nc==8 ? s(x,5) : s(x,4)     : c==2 ? nc==4 ? s(x,1) : nc==8 ? s(x,6) : s(x,5)     : c==3 ? nc==4 ? s(x,2) : nc==8 ? s(x,7) : s(x,0)     : c==4 ? nc==8 ? s(x,0) : s(x,1)     : c==5 ? nc==8 ? s(x,1) : s(x,2)     : c==6 ? s(x,2)     : s(x,3)))

#define inv_var(x,r,c)  ( r==0 ?               ( c==0 ? s(x,0)     : c==1 ? s(x,1)     : c==2 ? s(x,2)     : c==3 ? s(x,3)     : c==4 ? s(x,4)     : c==5 ? s(x,5)     : c==6 ? s(x,6)     : s(x,7))        : r==1 ?               ( c==0 ? nc==4 ? s(x,3) : nc==8 ? s(x,7) : s(x,5)     : c==1 ? s(x,0)     : c==2 ? s(x,1)     : c==3 ? s(x,2)     : c==4 ? s(x,3)     : c==5 ? s(x,4)     : c==6 ? s(x,5)     : s(x,6))        : r==2 ?               ( c==0 ? nc==4 ? s(x,2) : nc==8 ? s(x,5) : s(x,4)     : c==1 ? nc==4 ? s(x,3) : nc==8 ? s(x,6) : s(x,5)     : c==2 ? nc==8 ? s(x,7) : s(x,0)     : c==3 ? nc==8 ? s(x,0) : s(x,1)     : c==4 ? nc==8 ? s(x,1) : s(x,2)     : c==5 ? nc==8 ? s(x,2) : s(x,3)     : c==6 ? s(x,3)     : s(x,4))        :                      ( c==0 ? nc==4 ? s(x,1) : nc==8 ? s(x,4) : s(x,3)     : c==1 ? nc==4 ? s(x,2) : nc==8 ? s(x,5) : s(x,4)     : c==2 ? nc==4 ? s(x,3) : nc==8 ? s(x,6) : s(x,5)     : c==3 ? nc==8 ? s(x,7) : s(x,0)     : c==4 ? nc==8 ? s(x,0) : s(x,1)     : c==5 ? nc==8 ? s(x,1) : s(x,2)     : c==6 ? s(x,2)     : s(x,3)))

#define si(y,x,k,c) s(y,c) = word_in(x + 4 * c) ^ k[c]
#define so(y,x,c)   word_out(y + 4 * c, s(x,c))

#if defined(FOUR_TABLES)
#define fwd_rnd(y,x,k,c)    s(y,c)= (k)[c] ^ four_tables(x,ft_tab,fwd_var,rf1,c)
#define inv_rnd(y,x,k,c)    s(y,c)= (k)[c] ^ four_tables(x,it_tab,inv_var,rf1,c)
#elif defined(ONE_TABLE)
#define fwd_rnd(y,x,k,c)    s(y,c)= (k)[c] ^ one_table(x,upr,ft_tab,fwd_var,rf1,c)
#define inv_rnd(y,x,k,c)    s(y,c)= (k)[c] ^ one_table(x,upr,it_tab,inv_var,rf1,c)
#else
#define fwd_rnd(y,x,k,c)    s(y,c) = fwd_mcol(no_table(x,s_box,fwd_var,rf1,c)) ^ (k)[c]
#define inv_rnd(y,x,k,c)    s(y,c) = inv_mcol(no_table(x,inv_s_box,inv_var,rf1,c) ^ (k)[c])
#endif

#if defined(FOUR_LR_TABLES)
#define fwd_lrnd(y,x,k,c)   s(y,c)= (k)[c] ^ four_tables(x,fl_tab,fwd_var,rf1,c)
#define inv_lrnd(y,x,k,c)   s(y,c)= (k)[c] ^ four_tables(x,il_tab,inv_var,rf1,c)
#elif defined(ONE_LR_TABLE)
#define fwd_lrnd(y,x,k,c)   s(y,c)= (k)[c] ^ one_table(x,ups,fl_tab,fwd_var,rf1,c)
#define inv_lrnd(y,x,k,c)   s(y,c)= (k)[c] ^ one_table(x,ups,il_tab,inv_var,rf1,c)
#else
#define fwd_lrnd(y,x,k,c)   s(y,c) = no_table(x,s_box,fwd_var,rf1,c) ^ (k)[c]
#define inv_lrnd(y,x,k,c)   s(y,c) = no_table(x,inv_s_box,inv_var,rf1,c) ^ (k)[c]
#endif

#if BLOCK_SIZE == 16

#if defined(ARRAYS)
#define locals(y,x)     x[4],y[4]
#else
#define locals(y,x)     x##0,x##1,x##2,x##3,y##0,y##1,y##2,y##3
 /* 
   the following defines prevent the compiler requiring the declaration
   of generated but unused variables in the fwd_var and inv_var macros
 */
#define b04 unused
#define b05 unused
#define b06 unused
#define b07 unused
#define b14 unused
#define b15 unused
#define b16 unused
#define b17 unused
#endif
#define l_copy(y, x)    s(y,0) = s(x,0); s(y,1) = s(x,1);                         s(y,2) = s(x,2); s(y,3) = s(x,3);
#define state_in(y,x,k) si(y,x,k,0); si(y,x,k,1); si(y,x,k,2); si(y,x,k,3)
#define state_out(y,x)  so(y,x,0); so(y,x,1); so(y,x,2); so(y,x,3)
#define round(rm,y,x,k) rm(y,x,k,0); rm(y,x,k,1); rm(y,x,k,2); rm(y,x,k,3)

#elif BLOCK_SIZE == 24

#if defined(ARRAYS)
#define locals(y,x)     x[6],y[6]
#else
#define locals(y,x)     x##0,x##1,x##2,x##3,x##4,x##5,                         y##0,y##1,y##2,y##3,y##4,y##5
#define b06 unused
#define b07 unused
#define b16 unused
#define b17 unused
#endif
#define l_copy(y, x)    s(y,0) = s(x,0); s(y,1) = s(x,1);                         s(y,2) = s(x,2); s(y,3) = s(x,3);                         s(y,4) = s(x,4); s(y,5) = s(x,5);
#define state_in(y,x,k) si(y,x,k,0); si(y,x,k,1); si(y,x,k,2);                         si(y,x,k,3); si(y,x,k,4); si(y,x,k,5)
#define state_out(y,x)  so(y,x,0); so(y,x,1); so(y,x,2);                         so(y,x,3); so(y,x,4); so(y,x,5)
#define round(rm,y,x,k) rm(y,x,k,0); rm(y,x,k,1); rm(y,x,k,2);                         rm(y,x,k,3); rm(y,x,k,4); rm(y,x,k,5)
#else

#if defined(ARRAYS)
#define locals(y,x)     x[8],y[8]
#else
#define locals(y,x)     x##0,x##1,x##2,x##3,x##4,x##5,x##6,x##7,                         y##0,y##1,y##2,y##3,y##4,y##5,y##6,y##7
#endif
#define l_copy(y, x)    s(y,0) = s(x,0); s(y,1) = s(x,1);                         s(y,2) = s(x,2); s(y,3) = s(x,3);                         s(y,4) = s(x,4); s(y,5) = s(x,5);                         s(y,6) = s(x,6); s(y,7) = s(x,7);

#if BLOCK_SIZE == 32

#define state_in(y,x,k) si(y,x,k,0); si(y,x,k,1); si(y,x,k,2); si(y,x,k,3);                         si(y,x,k,4); si(y,x,k,5); si(y,x,k,6); si(y,x,k,7)
#define state_out(y,x)  so(y,x,0); so(y,x,1); so(y,x,2); so(y,x,3);                         so(y,x,4); so(y,x,5); so(y,x,6); so(y,x,7)
#define round(rm,y,x,k) rm(y,x,k,0); rm(y,x,k,1); rm(y,x,k,2); rm(y,x,k,3);                         rm(y,x,k,4); rm(y,x,k,5); rm(y,x,k,6); rm(y,x,k,7)
#else

#define state_in(y,x,k) switch(nc) {   case 8: si(y,x,k,7); si(y,x,k,6);     case 6: si(y,x,k,5); si(y,x,k,4);     case 4: si(y,x,k,3); si(y,x,k,2);             si(y,x,k,1); si(y,x,k,0); }

#define state_out(y,x) switch(nc) {   case 8: so(y,x,7); so(y,x,6);     case 6: so(y,x,5); so(y,x,4);     case 4: so(y,x,3); so(y,x,2);             so(y,x,1); so(y,x,0); }

#if defined(FAST_VARIABLE)

#define round(rm,y,x,k) switch(nc) {   case 8: rm(y,x,k,7); rm(y,x,k,6);             rm(y,x,k,5); rm(y,x,k,4);             rm(y,x,k,3); rm(y,x,k,2);             rm(y,x,k,1); rm(y,x,k,0);             break;     case 6: rm(y,x,k,5); rm(y,x,k,4);             rm(y,x,k,3); rm(y,x,k,2);             rm(y,x,k,1); rm(y,x,k,0);             break;     case 4: rm(y,x,k,3); rm(y,x,k,2);             rm(y,x,k,1); rm(y,x,k,0);             break; }
#else

#define round(rm,y,x,k) switch(nc) {   case 8: rm(y,x,k,7); rm(y,x,k,6);     case 6: rm(y,x,k,5); rm(y,x,k,4);     case 4: rm(y,x,k,3); rm(y,x,k,2);             rm(y,x,k,1); rm(y,x,k,0); }

#endif

#endif
#endif

cf_dec c_name(encrypt)(const byte in_blk[], byte out_blk[], const c_name(aes) *cx)
{   word        locals(b0, b1);
    const word  *kp = cx->e_key;

#if !defined(ONE_TABLE) && !defined(FOUR_TABLES)
    word        f2;
#endif

    if(!(cx->mode & 0x01)) return aes_bad;

    state_in(b0, in_blk, kp); kp += nc;

#if defined(UNROLL)

    switch(cx->Nrnd)
    {
    case 14:    round(fwd_rnd,  b1, b0, kp         ); 
                round(fwd_rnd,  b0, b1, kp + nc    ); kp += 2 * nc;
    case 12:    round(fwd_rnd,  b1, b0, kp         ); 
                round(fwd_rnd,  b0, b1, kp + nc    ); kp += 2 * nc;
    case 10:    round(fwd_rnd,  b1, b0, kp         );             
                round(fwd_rnd,  b0, b1, kp +     nc);
                round(fwd_rnd,  b1, b0, kp + 2 * nc); 
                round(fwd_rnd,  b0, b1, kp + 3 * nc);
                round(fwd_rnd,  b1, b0, kp + 4 * nc); 
                round(fwd_rnd,  b0, b1, kp + 5 * nc);
                round(fwd_rnd,  b1, b0, kp + 6 * nc); 
                round(fwd_rnd,  b0, b1, kp + 7 * nc);
                round(fwd_rnd,  b1, b0, kp + 8 * nc);
                round(fwd_lrnd, b0, b1, kp + 9 * nc);
    }
#elif defined(PARTIAL_UNROLL)
    {   word    rnd;

        for(rnd = 0; rnd < (cx->Nrnd >> 1) - 1; ++rnd)
        {
            round(fwd_rnd, b1, b0, kp); 
            round(fwd_rnd, b0, b1, kp + nc); kp += 2 * nc;
        }

        round(fwd_rnd,  b1, b0, kp);
        round(fwd_lrnd, b0, b1, kp + nc);
    }
#else
    {   word    rnd;

        for(rnd = 0; rnd < cx->Nrnd - 1; ++rnd)
        {
            round(fwd_rnd, b1, b0, kp); 
            l_copy(b0, b1); kp += nc;
        }

        round(fwd_lrnd, b0, b1, kp);
    }
#endif

    state_out(out_blk, b0);
    return aes_good;
}

cf_dec c_name(decrypt)(const byte in_blk[], byte out_blk[], const c_name(aes) *cx)
{   word        locals(b0, b1);
    const word  *kp = cx->d_key;

#if !defined(ONE_TABLE) && !defined(FOUR_TABLES)
    word        f2, f4, f8, f9; 
#endif

    if(!(cx->mode & 0x02)) return aes_bad;

    state_in(b0, in_blk, kp); kp += nc;

#if defined(UNROLL)

    switch(cx->Nrnd)
    {
    case 14:    round(inv_rnd,  b1, b0, kp         );
                round(inv_rnd,  b0, b1, kp + nc    ); kp += 2 * nc;
    case 12:    round(inv_rnd,  b1, b0, kp         );
                round(inv_rnd,  b0, b1, kp + nc    ); kp += 2 * nc;
    case 10:    round(inv_rnd,  b1, b0, kp         );             
                round(inv_rnd,  b0, b1, kp +     nc);
                round(inv_rnd,  b1, b0, kp + 2 * nc); 
                round(inv_rnd,  b0, b1, kp + 3 * nc);
                round(inv_rnd,  b1, b0, kp + 4 * nc); 
                round(inv_rnd,  b0, b1, kp + 5 * nc);
                round(inv_rnd,  b1, b0, kp + 6 * nc); 
                round(inv_rnd,  b0, b1, kp + 7 * nc);
                round(inv_rnd,  b1, b0, kp + 8 * nc);
                round(inv_lrnd, b0, b1, kp + 9 * nc);
    }
#elif defined(PARTIAL_UNROLL)
    {   word    rnd;

        for(rnd = 0; rnd < (cx->Nrnd >> 1) - 1; ++rnd)
        {
            round(inv_rnd, b1, b0, kp); 
            round(inv_rnd, b0, b1, kp + nc); kp += 2 * nc;
        }

        round(inv_rnd,  b1, b0, kp);
        round(inv_lrnd, b0, b1, kp + nc);
    }
#else
    {   word    rnd;

        for(rnd = 0; rnd < cx->Nrnd - 1; ++rnd)
        {
            round(inv_rnd, b1, b0, kp); 
            l_copy(b0, b1); kp += nc;
        }

        round(inv_lrnd, b0, b1, kp);
    }
#endif

    state_out(out_blk, b0);
    return aes_good;
}
