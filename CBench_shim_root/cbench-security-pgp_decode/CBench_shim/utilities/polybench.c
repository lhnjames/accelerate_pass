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


/* ==== armor.c ==== */
/*      armor.c  - ASCII/binary encoding/decoding based partly on
   PEM RFC1113 and MIME standards.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "mpilib.h"
#include "fileio.h"
#include "mpiio.h"
#include "language.h"
#include "pgp.h"
#include "charset.h"
#include "crypto.h"
#include "armor.h"
#include "keymgmt.h"
#ifdef MACTC5
#include "Macutil2.h"
#include "Macutil3.h"
#endif

static int darmor_file(char *infile, char *outfile);
static crcword crchware(byte ch, crcword poly, crcword accum);
static int armordecode(FILE * in, FILE * out, int *warned);
static void mk_crctbl(crcword poly);
static boolean is_armorfile(char *infile);

/*      Begin ASCII armor routines.
   This converts a binary file into printable ASCII characters, in a
   radix-64 form mostly compatible with the MIME format.
   This makes it easier to send encrypted files over a 7-bit channel.
 */

/* Index this array by a 6 bit value to get the character corresponding
 * to that value.
 */
static
unsigned char bintoasc[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Index this array by a 7 bit value to get the 6-bit binary field
 * corresponding to that value.  Any illegal characters return high bit set.
 */
static
unsigned char asctobin[] =
{
#ifdef EBCDIC
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128, 62,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128, 63,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128, 26, 27, 28, 29, 30, 31, 32,  33, 34,128,128,128,128,128,128,
    128, 35, 36, 37, 38, 39, 40, 41,  42, 43,128,128,128,128,128,128,
    128,128, 44, 45, 46, 47, 48, 49,  50, 51,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
    128,  0,  1,  2,  3,  4,  5,  6,   7,  8,128,128,128,128,128,128,
    128,  9, 10, 11, 12, 13, 14, 15,  16, 17,128,128,128,128,128,128,
    128,128, 18, 19, 20, 21, 22, 23,  24, 25,128,128,128,128,128,128,
     52, 53, 54, 55, 56, 57, 58, 59,  60, 61,128,128,128,128,128,128
#else
    0200, 0200, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0200, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0200, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0200, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0200, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0200, 0200, 0076, 0200, 0200, 0200, 0077,
    0064, 0065, 0066, 0067, 0070, 0071, 0072, 0073,
    0074, 0075, 0200, 0200, 0200, 0200, 0200, 0200,
    0200, 0000, 0001, 0002, 0003, 0004, 0005, 0006,
    0007, 0010, 0011, 0012, 0013, 0014, 0015, 0016,
    0017, 0020, 0021, 0022, 0023, 0024, 0025, 0026,
    0027, 0030, 0031, 0200, 0200, 0200, 0200, 0200,
    0200, 0032, 0033, 0034, 0035, 0036, 0037, 0040,
    0041, 0042, 0043, 0044, 0045, 0046, 0047, 0050,
    0051, 0052, 0053, 0054, 0055, 0056, 0057, 0060,
    0061, 0062, 0063, 0200, 0200, 0200, 0200, 0200
#endif
};

/* Current line number for mult decodes */
#ifdef MACTC5
long	infile_line;		/* Current line number for mult decodes */
#else
static long infile_line;
#endif

/************************************************************************/

/* CRC Routines. */
/*      These CRC functions are derived from code in chapter 19 of the book 
 *    "C Programmer's Guide to Serial Communications", by Joe Campbell.
 *      Generalized to any CRC width by Philip Zimmermann.
 */

#define byte unsigned char

#define CRCBITS 24		/* may be 16, 24, or 32 */
/* #define maskcrc(crc) ((crcword)(crc)) *//* if CRCBITS is 16 or 32 */
#define maskcrc(crc) ((crc) & 0xffffffL)	/* if CRCBITS is 24 */
#define CRCHIBIT ((crcword) (1L<<(CRCBITS-1)))	/* 0x8000 if CRCBITS is 16 */
#define CRCSHIFTS (CRCBITS-8)

/*
 * Notes on making a good 24-bit CRC--
 * The primitive irreducible polynomial of degree 23 over GF(2),
 * 040435651 (octal), comes from Appendix C of "Error Correcting Codes,
 * 2nd edition" by Peterson and Weldon, page 490.  This polynomial was
 * chosen for its uniform density of ones and zeros, which has better
 * error detection properties than polynomials with a minimal number of
 * nonzero terms.  Multiplying this primitive degree-23 polynomial by
 * the polynomial x+1 yields the additional property of detecting any
 * odd number of bits in error, which means it adds parity.  This 
 * approach was recommended by Neal Glover.
 *
 * To multiply the polynomial 040435651 by x+1, shift it left 1 bit and
 * bitwise add (xor) the unshifted version back in.  Dropping the unused 
 * upper bit (bit 24) produces a CRC-24 generator bitmask of 041446373 
 * octal, or 0x864cfb hex.  
 *
 * You can detect spurious leading zeros or framing errors in the 
 * message by initializing the CRC accumulator to some agreed-upon 
 * nonzero value, but the value used is a bit nonstandard.  
 */

#define CCITTCRC 0x1021		/* CCITT's 16-bit CRC generator polynomial */
#define PRZCRC 0x864cfbL	/* PRZ's 24-bit CRC generator polynomial */
#define CRCINIT 0xB704CEL	/* Init value for CRC accumulator */

static crcword crctable[256];	/* Table for speeding up CRC's */

/*
 * mk_crctbl derives a CRC lookup table from the CRC polynomial.
 * The table is used later by the crcbytes function given below.
 * mk_crctbl only needs to be called once at the dawn of time.
 *
 * The theory behind mk_crctbl is that table[i] is initialized
 * with the CRC of i, and this is related to the CRC of i>>1,
 * so the CRC of i>>1 (pointed to by p) can be used to derive
 * the CRC of i (pointed to by q).
 */
static void
mk_crctbl(crcword poly)
{
    int i;
    crcword t, *p, *q;
    p = q = crctable;
    *q++ = 0;
    *q++ = poly;
    for (i = 1; i < 128; i++) {
	t = *++p;
	if (t & CRCHIBIT) {
	    t <<= 1;
	    *q++ = t ^ poly;
	    *q++ = t;
	} else {
	    t <<= 1;
	    *q++ = t;
	    *q++ = t ^ poly;
	}
    }
}

/*
 * Accumulate a buffer's worth of bytes into a CRC accumulator,
 * returning the new CRC value.
 */
crcword
crcbytes(byte * buf, unsigned len, register crcword accum)
{
    do {
	accum = accum << 8 ^ crctable[(byte) (accum >> CRCSHIFTS) ^ *buf++];
    } while (--len);
    return maskcrc(accum);
}				/* crcbytes */

/* Initialize the CRC table using our codes */
void
init_crc(void)
{
    mk_crctbl(PRZCRC);
}


/************************************************************************/


/* ENC is the basic 1 character encoding function to make a char printing */
#define ENC(c) ((int)bintoasc[((c) & 077)])
#define PAD		'='

/*
 * output one group of up to 3 bytes, pointed at by p, on file f.
 * if fewer than 3 are present, the 1 or two extras must be zeros.
 */
static void
outdec(char *p, FILE *f, int count)
{
    int c1, c2, c3, c4;

    c1 = *p >> 2;
    c2 = ((*p << 4) & 060) | ((p[1] >> 4) & 017);
    c3 = ((p[1] << 2) & 074) | ((p[2] >> 6) & 03);
    c4 = p[2] & 077;
    putc(ENC(c1), f);
    putc(ENC(c2), f);
    if (count == 1) {
	putc(PAD, f);
	putc(PAD, f);
    } else {
	putc(ENC(c3), f);
	if (count == 2)
	    putc(PAD, f);
	else
	    putc(ENC(c4), f);
    }
}				/* outdec */


/* Output the CRC value, MSB first per normal CRC conventions */
static void
outcrc(word32 crc, FILE *outFile)
{
    char crcbuf[4];
    crcbuf[0] = (crc >> 16) & 0xff;
    crcbuf[1] = (crc >> 8) & 0xff;
    crcbuf[2] = (crc >> 0) & 0xff;
    putc(PAD, outFile);
    outdec(crcbuf, outFile, 3);
    putc('\n', outFile);
}				/* outcrc */

/* Return filename for output (text mode), but replace last letter(s) of
 * filename with the ascii for num.  It will use the appropriate number
 * of digits for ofnum when converting num, so if ofnum < 10, use 1 digit,
 * >= 10 and < 100 use 2 digits, >= 100 and < 1000 use 3 digits.  If its
 * >= 1000, then we have other problems to worry about, and this might do
 * weird things.
 */
static char *
numFilename(char *fname, int num, int ofnum)
{
    static char fnamenum[MAX_PATH];
    int len;
    int offset = 1;

    strcpy(fnamenum, fname);
    len = strlen(fnamenum);
    do {
	fnamenum[len - offset] = '0' + (num % 10);
	num /= 10;
	ofnum /= 10;
	offset++;
    } while (ofnum >= 1 && offset < 4);
    return fnamenum;
}

/*
 * Reads and discards a line from the given file.  Returns -1 on error or
 * EOF, 0 if the line is blank, and 1 if the line is not blank.
 */
static int
skipline(FILE * f)
{
    int state, flag, c;

    state = 0;
    flag = 0;
    for (;;) {
	c = getc(f);
	if (c == '\n')
	    return flag;
	if (state) {
	    ungetc(c, f);
	    return flag;
	}
	if (c == EOF)
	    return -1;
	if (c == '\r')
	    state = 1;
	else if (c != ' ')
	    flag = 1;
    }
}				/* skipline */


/*
 * Copies a line from the input file to the output.  Does NOT copy the
 * trailing newline.  Returns -1 on EOF or error, 0 if the line was terminated
 * by EOF, and 1 if the line was terminated with a newline sequence.
 */
static int
copyline(FILE * in, FILE * out)
{
    int state, flag, c;

    state = 0;
    for (;;) {
	c = getc(in);
	if (c == '\n')
	    return 1;
	if (state) {
	    ungetc(c, in);
	    return 1;
	}
	if (c == EOF)
	    return 0;
	if (c == '\r')
	    state = 1;
	else
	    putc(c, out);
    }
}				/* copyline */

/*
 * Reads a line from file f, up to the size of the buffer.  The line in the
 * buffer will NOT include line termination, although any of (CR, LF, CRLF)
 * is accepted on input.  The return value is -1 on error, 0 if the line
 * was terminated abnormally (EOF, error, or out of buffer space), and
 * 1 if the line was terminated normally.
 *
 * Passing in a buffer less than 2 characters long is not a terribly bright
 * idea.
 */
#if defined(WINDOWS)
int
getline(char *buf, int n, FILE * f)
{
    int state;
    char *p;
    int c;

    state = 0;
    p = buf;
    for (;;) {
	c = getc(f);
	if (c == '\n') {
	    *p = 0;
	    return 1;		/* Line terminated with \n or \r\n */
	}
	if (state) {
	    ungetc(c, f);
	    *p = 0;
	    return 1;		/* Line terminated with \r */
	}
	if (c == EOF) {
	    *p = 0;
	    return (p == buf) ? -1 : 0;		/* Error */
	}
	if (c == '\r')
	    state = 1;
	else if (--n > 0) {
	    *p++ = c;
	} else {
	    ungetc(c, f);
	    *p = 0;
	    return 0;		/* Out of buffer space */
	}
    }				/* for (;;) */
}				/* getline */
#endif

#if 1
/* This limit is advisory only; longer lines are handled properly.
 * The only requirement is that this be at least as long as the longest
 * delimiter string used by PGP
 * (e.g. "-----BEGIN PGP MESSAGE, PART %02d/%02d-----\n")
 */
#define MAX_LINE_SIZE 80
#else
#ifdef MSDOS			/* limited stack space */
#define MAX_LINE_SIZE	256
#else
#define MAX_LINE_SIZE	1024
#endif
#endif

/*
 * Read a line from file f, buf must be able to hold at least MAX_LINE_SIZE
 * characters.  Anything after that is ignored.  Strips trailing spaces and
 * line terminator, can read LF, CRLF and CR textfiles.  It can't be ASCII
 * armor anyway.
 */
static char *
get_armor_line(char *buf, FILE * f)
{
    int c, n = MAX_LINE_SIZE-1;
    char *p = buf;

    do {
	c = getc(f);
	if (c == '\n' || c == '\r' || c == EOF)
	    break;
	*p++ = c;
    } while (--n > 0);
    if (p == buf && c == EOF) {
	*buf = '\0';
	return NULL;
    }
    /*
     * Skip to end of line, setting n to non-zero if anything trailing is
     * not a space (meaning that any trailing whitespace in the buffer is
     * not trailing whitespace on the line and should not be stripped).
     */
    n = 0;
    while (c != '\n' && c != '\r' && c != EOF) {
        n |= c ^ ' ';
	c = getc(f);
    }
    if (c == '\r' && (c = getc(f)) != '\n')
	ungetc(c, f);
    if (!n) {	/* Skip trailing whitespace, as described above */
	while (p > buf && p[-1] == ' ')
	    --p;
    }
    *p = '\0';
    return buf;
}


/*
 * Encode a file in sections.  64 ASCII bytes * 720 lines = 46K, 
 * recommended max.  Usenet message size is 50K so this leaves a nice 
 * margin for .signature.  In the interests of orthogonality and 
 * programmer laziness no check is made for a message containing only 
 * a few lines (or even just an 'end')  after a section break. 
 */
#define LINE_LEN	48L
int pem_lines = 720;
#define BYTES_PER_SECTION	(LINE_LEN * pem_lines)

#if defined(VMS) || defined(C370)
#define FOPRARMOR	FOPRTXT
#else
/* armored files are opened in binary mode so that CRLF/LF/CR files
   can be handled by all systems */
#define	FOPRARMOR	FOPRBIN
#endif

extern boolean verbose;		/* Undocumented command mode in PGP.C */
extern boolean filter_mode;

/*
 * Copy from infilename to outfilename, ASCII armoring as you go along,
 * and with breaks every pem_lines lines.
 * If clearfilename is non-NULL, first output that file preceded by a
 * special delimiter line.  filename is the original filename, used
 * only for debugging.
 */
int
armor_file(char *infilename, char *outfilename, char *filename,
	char *clearfilename, boolean kv_label)
{
    char buffer[MAX_LINE_SIZE];
    int i, rc, bytesRead, lines = 0;
    int noSections, currentSection = 1;
    long fileLen;
    crcword crc;
    FILE *inFile, *outFile, *clearFile;
    char *tempf;
    char *blocktype = "MESSAGE";
#ifdef MACTC5
    char curOutFile[256]="";
#endif

    if (verbose)
	fprintf(pgpout,
"armor_file: infile = %s, outfile = %s, filename = %s, clearname = %s\n",
		infilename, outfilename, filename,
		clearfilename == NULL ? "" : clearfilename);

    /* open input file as binary */
    if ((inFile = fopen(infilename, FOPRBIN)) == NULL)
	return 1;

    if (!outfilename || pem_lines == 0) {
	noSections = 1;
    } else {
	/* Evaluate how many parts this file will comprise */
	fseek(inFile, 0L, SEEK_END);
	fileLen = ftell(inFile);
	rewind(inFile);
	noSections = (fileLen + BYTES_PER_SECTION - 1) /
	    BYTES_PER_SECTION;
	if (noSections > 99) {
	    pem_lines = ((fileLen + LINE_LEN - 1) / LINE_LEN + 98) / 99;
	    noSections = (fileLen + BYTES_PER_SECTION - 1) /
		BYTES_PER_SECTION;
	    fprintf(pgpout,
	    "value for \"armorlines\" is too low, using %d\n", pem_lines);
	}
    }

    if (clearfilename)
      tempf = tempfile(TMP_WIPE);
    else
      tempf = outfilename;

    if (outfilename == NULL) {
	outFile = stdout;
    } else {
	if (noSections > 1) {
            do {
                char *t;
                force_extension(outfilename, ASC_EXTENSION);
                strcpy(outfilename, numFilename(outfilename, 1, noSections));
                if (!file_exists(outfilename)) break;
                t = ck_dup_output(outfilename, TRUE, TRUE);
                if (t==NULL) user_error();
                strcpy(outfilename,t);
            } while (TRUE);
            outFile = fopen(tempf, FOPWTXT);
	} else
	    outFile = fopen(tempf, FOPWTXT);
#ifdef MACTC5
		strcpy(curOutFile,outfilename);
#endif
    }

    if (outFile == NULL) {
	fclose(inFile);
	return 1;
    }
    if (clearfilename) {
	if ((clearFile = fopen(clearfilename, FOPRTXT)) == NULL) {
	    fclose(inFile);
	    if (outFile != stdout)
		fclose(outFile);
	    return 1;
	}
	fprintf(outFile, "-----BEGIN PGP SIGNED MESSAGE-----\n\n");
	while ((i = getline(buffer, sizeof buffer, clearFile)) >= 0) {
	    /* Quote lines beginning with '-' as per RFC1113;
	     * Also quote lines beginning with "From "; this is
	     * for Unix mailers which add ">" to such lines.
	     */
	    if (buffer[0] == '-' || strncmp(buffer, "From ", 5) == 0)
		fputs("- ", outFile);
	    fputs(buffer, outFile);
	    /* If there is more on this line, copy it */
	    if (i == 0)
		if (copyline(clearFile, outFile) <= 0)
		    break;
	    fputc('\n', outFile);
	}
	fclose(clearFile);
	putc('\n', outFile);
	blocktype = "SIGNATURE";
    }
    if (noSections == 1) {
	byte ctb = 0;
        int keycounter = 0;
        int status;
	ctb = getc(inFile);
	if (is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE)) {
	    blocktype = "PUBLIC KEY BLOCK";
            if (kv_label) {
                kv_title(outFile);     /* Title line */
                rewind(inFile);        /* Back over CTB */
                status = kvformat_keypacket(inFile, outFile, TRUE, "", infilename,
                                            FALSE, FALSE, &keycounter);
	        fprintf(outFile, "\n");
            }
	} else if (is_ctb_type(ctb, CTB_CERT_SECKEY_TYPE)) {
            blocktype = "SECRET KEY BLOCK";
            if (kv_label) {
                kv_title(outFile);     /* Title line */
                rewind(inFile);        /* Back over CTB */
                status = kvformat_keypacket(inFile, outFile, TRUE, "", infilename,
                                            FALSE, FALSE, &keycounter);
	        fprintf(outFile, "\n");
            }
	}
	fprintf(outFile, "-----BEGIN PGP %s-----\n", blocktype);
	rewind(inFile);
    } else {
	fprintf(outFile,
		"-----BEGIN PGP MESSAGE, PART %02d/%02d-----\n",
		1, noSections);
    }
    fprintf(outFile, "Version: %s\n", LANG(rel_version));
    if (clearfilename)
	fprintf(outFile, "Charset: %s\n", charset);
    if (globalCommentString[0])
	fprintf(outFile, "Comment: %s\n", globalCommentString);
    fprintf(outFile, "\n");

    init_crc();
    crc = CRCINIT;

    while ((bytesRead = fread(buffer, 1, LINE_LEN, inFile)) > 0) {
	/* Munge up LINE_LEN characters */
	if (bytesRead < LINE_LEN)
	    fill0(buffer + bytesRead, LINE_LEN - bytesRead);

	crc = crcbytes((byte *) buffer, bytesRead, crc);
	for (i = 0; i < bytesRead - 3; i += 3)
	    outdec(buffer + i, outFile, 3);
	outdec(buffer + i, outFile, bytesRead - i);
	putc('\n', outFile);
#ifdef MACTC5
		mac_poll_for_break();
#endif

	if (++lines == pem_lines && currentSection < noSections) {
	    lines = 0;
	    outcrc(crc, outFile);
	    fprintf(outFile,
		    "-----END PGP MESSAGE, PART %02d/%02d-----\n\n",
		    currentSection, noSections);
	    if (write_error(outFile)) {
		fclose(outFile);
		return -1;
 	    }
	    fclose(outFile);
#ifdef MACTC5
		PGPSetFinfo(curOutFile,'TEXT','MPGP');
#endif
	    outFile = fopen(numFilename(outfilename,
					++currentSection,
					noSections), FOPWTXT);
#ifdef MACTC5
		strcpy(curOutFile,numFilename (outfilename,currentSection,noSections));
#endif
	    if (outFile == NULL) {
		fclose(inFile);
		return -1;
	    }
	    fprintf(outFile,
		    "-----BEGIN PGP MESSAGE, PART %02d/%02d-----\n",
		    currentSection, noSections);
	    fprintf(outFile, "\n");
	    crc = CRCINIT;
	}
    }
    outcrc(crc, outFile);

    if (noSections == 1)
	fprintf(outFile, "-----END PGP %s-----\n", blocktype);
    else
	fprintf(outFile, "-----END PGP MESSAGE, PART %02d/%02d-----\n",
		noSections, noSections);

    /* Done */
    fclose(inFile);
    rc = write_error(outFile);
    if (outFile == stdout)
	return rc;
#ifdef MACTC5
	PGPSetFinfo(curOutFile,'TEXT','MPGP');
#endif
    fclose(outFile);
    if (clearfilename) {
        remove(outfilename);
        savetemp(tempf,outfilename);
    }

    if (rc)
	return -1;

    if (clearfilename) {
	fprintf(pgpout,
		LANG("\nClear signature file: %s\n"), outfilename);
    } else if (noSections == 1) {
	fprintf(pgpout,
		LANG("\nTransport armor file: %s\n"), outfilename);
    } else {
	fprintf(pgpout, LANG("\nTransport armor files: "));
	for (i = 1; i <= noSections; ++i)
	    fprintf(pgpout, "%s%s",
		    numFilename(outfilename, i, noSections),
		    i == noSections ? "\n" : ", ");
    }
    return 0;
}				/* armor_file */

/*      End ASCII armor encode routines. */


/*
 * ASCII armor decode routines.
 */
static int
darmor_buffer(char *inbuf, char *outbuf, int *outlength)
{
    unsigned char *bp;
    int length;
    unsigned int c1, c2, c3, c4;
    register int j;

    length = 0;
    bp = (unsigned char *) inbuf;

    /* FOUR input characters go into each THREE output charcters */

    while (*bp != '\0') {
#ifdef EBCDIC
	if ((c1 = asctobin[*bp]) & 0x80)
	    return -1;
	++bp;
	if ((c2 = asctobin[*bp]) & 0x80)
	    return -1;
#else
	if (*bp & 0x80 || (c1 = asctobin[*bp]) & 0x80)
	    return -1;
	++bp;
	if (*bp & 0x80 || (c2 = asctobin[*bp]) & 0x80)
	    return -1;
#endif
	if (*++bp == PAD) {
	    c3 = c4 = 0;
	    length += 1;
	    if (c2 & 15)
		return -1;
	    if (strcmp((char *) bp, "==") == 0)
		bp += 1;
	    else if (strcmp((char *) bp, "=3D=3D") == 0)
		bp += 5;
	    else
		return -1;
#ifdef EBCDIC
	} else if ((c3 = asctobin[*bp]) & 0x80) {
#else
	} else if (*bp & 0x80 || (c3 = asctobin[*bp]) & 0x80) {
#endif
	    return -1;
	} else {
	    if (*++bp == PAD) {
		c4 = 0;
		length += 2;
		if (c3 & 3)
		    return -1;
		if (strcmp((char *) bp, "=") == 0);	/* All is well */
		else if (strcmp((char *) bp, "=3D") == 0)
		    bp += 2;
		else
		    return -1;
#ifdef EBCDIC
	    } else if ((c4 = asctobin[*bp]) & 0x80) {
#else
	    } else if (*bp & 0x80 || (c4 = asctobin[*bp]) & 0x80) {
#endif
		return -1;
	    } else {
		length += 3;
	    }
	}
	++bp;
	j = (c1 << 2) | (c2 >> 4);
	*outbuf++ = j;
	j = (c2 << 4) | (c3 >> 2);
	*outbuf++ = j;
	j = (c3 << 6) | c4;
	*outbuf++ = j;
    }

    *outlength = length;
    return 0;			/* normal return */

}				/* darmor_buffer */

static char armorfilename[MAX_PATH];
/*
 * try to open the next file of a multi-part armored file
 * the sequence number is expected at the end of the file name
 */
static FILE *
open_next(void)
{
    char *p, *s, c;
    FILE *fp;

    p = armorfilename + strlen(armorfilename);
    while (--p >= armorfilename && isdigit(*p)) {
	if (*p != '9') {
	    ++*p;
	    return fopen(armorfilename, FOPRARMOR);
	}
	*p = '0';
    }

    /* need an extra digit */
    if (p >= armorfilename) {
	/* try replacing character ( .as0 -> .a10 ) */
	c = *p;
	*p = '1';
	if ((fp = fopen(armorfilename, FOPRARMOR)) != NULL)
	    return fp;
	*p = c;			/* restore original character */
    }
    ++p;
    for (s = p + strlen(p); s >= p; --s)
	s[1] = *s;
    *p = '1';			/* insert digit ( fn0 -> fn10 ) */

#if defined(MSDOS) && !defined(BUG)
    /* if the resulting filename has more than three
       characters after the first dot, don't even try to open it */
    s = strchr(armorfilename, '.');
    if (s != NULL)
       if (strlen(s) > 3)
          return NULL;
#endif /* MSDOS */

    return fopen(armorfilename, FOPRARMOR);
}

/*
 * Returns -1 if the line given is does not begin as a valid ASCII
 * armor header line (something of the form "Label: ", where "Label"
 * must begin with a letter followed by letters, numbers, or hyphens,
 * followed immediately by a colon and a space), 0 if it is a familiar
 * label, and the length of the label if it is an unfamiliar label
 * (E.g. not "Version" or "Comment");
 */
static int
isheaderline(char const *buf)
{
	int i;

	if (!isalpha(*buf))
		return -1;	/* Not a label */

	for (i = 1; isalnum(buf[i]) || i == '-'; i++)
		;
	if (buf[i] != ':' || buf[i+1] != ' ')
		return -1;	/* Not a label */

	if (memcmp(buf, "Charset", i) == 0) {
		if (use_charset_header) strcpy(charset_header,buf+9);
		return 0;
	}
	if (memcmp(buf, "Version", i) == 0 ||
	    memcmp(buf, "Comment", i) == 0)
		return 0;	/* Familiar label */
	return i;	/* Unfamiliar label */
}

/* 
 * Skips a bunch of headers, either returning 0, or printing
 * an error message and returning -1.
 * If it encounters an unfamiliar label and *warned is not set,
 * prints a warning and sets *warned.
 * NOTE that file read errors are NOT printed or reported in the
 * return code.  It is assumed that the following read will
 * notice the error and do something appropriate.
 */
static int
skipheaders(FILE *in, char *buf, int *warned, int armorfollows)
{
    int label_seen = 0;
    int i;
#ifndef STRICT_ARMOR	/* Allow no space */
    long fpos;
    char outbuf[(MAX_LINE_SIZE*3+3)/4];
    int n;
#endif

    for (;;) {
	++infile_line;
#ifndef STRICT_ARMOR
	fpos = ftell(in);
#endif
	if (get_armor_line(buf, in) == NULL)	/* Error */
	    return 0;	/* See comment above */
	if (buf[0] == '\0')	/* Blank line */
	    return 0;	/* Success */
	if (label_seen && (buf[0] == ' ' || buf[0] == '\t'))
	    continue;	/* RFC-822-style continuation line */
	i = isheaderline(buf);
	if (i < 0) {	/* Not a legal header line */
#ifndef STRICT_ARMOR	/* If it's as ASCII armor line, accept it */
	    if (armorfollows && darmor_buffer(buf, outbuf, &n) == 0 && n == 48)
	    {
		fseek(in, fpos, SEEK_SET);
		--infile_line;
		return 0;	/* Consider this acceptable */
	    }
#else
	    (void)armorfollows;	/* Stop compiler complaints */
#endif
	    fprintf(pgpout,
LANG("Invalid ASCII armor header line: \"%.40s\"\n\
ASCII armor corrupted.\n"), buf);
	    return -1;
	}
	if (i > 0 && !*warned) {
		fprintf(pgpout,
LANG("Warning: Unrecognized ASCII armor header label \"%.*s:\" ignored.\n"),
		        i, buf);
		*warned = 1;
	}
	label_seen = 1;	/* Continuation lines are now legal */
    }
}

/*
 * Copy from in to out, decoding as you go, with handling for multiple
 * 500-line blocks of encoded data.  This function also knows how to
 * go past the end of one part to the beginning of the next in a multi-part
 * file.  (As you can see from some ugliness below, this is not the best
 * place to do it, since the caller is responsible for closing the
 * "original_in" file.)
 */
static int
armordecode(FILE *original_in, FILE *out, int *warned)
{
    char inbuf[MAX_LINE_SIZE];
    char outbuf[MAX_LINE_SIZE];

    int i, n, status;
    int line;
    int section, currentSection = 1;
    int noSections = 0;
    int gotcrc = 0;
    long crc = CRCINIT, chkcrc = -1;
    char crcbuf[4];
    int ret_code = 0;
    int end_of_message;
    FILE *in = original_in;

    init_crc();

    for (line = 1;; line++) {	/* for each input line */
	if (get_armor_line(inbuf, in) == NULL) {
	    end_of_message = 1;
	} else {
	    end_of_message =
		(strncmp(inbuf, "-----END PGP MESSAGE,", 21) == 0);
	    ++infile_line;
	}

	if (currentSection != noSections && end_of_message) {
	    /* End of this section */
	    if (gotcrc) {
		if (chkcrc != crc) {
		    fprintf(pgpout,
 LANG("ERROR: Bad ASCII armor checksum in section %d.\n"), currentSection);
/* continue with decoding to see if there are other bad parts */
		    ret_code = -1;
		}
	    }
	    gotcrc = 0;
	    crc = CRCINIT;
	    section = 0;

	    /* Try and find start of next section */
	    do {
		if (get_armor_line(inbuf, in) == NULL) {
		    if (in != original_in)
			fclose(in);
		    if ((in = open_next()) != NULL)
			continue;	/* Keep working on new in */
		    fprintf(pgpout,
		    LANG("Can't find section %d.\n"), currentSection + 1);
		    return -1;
		}
		++infile_line;
	    }
	    while (strncmp(inbuf, "-----BEGIN PGP MESSAGE", 22));

	    /* Make sure this section is the correct one */
	    if (2 != sscanf(inbuf,
			    "-----BEGIN PGP MESSAGE, PART %d/%d",
			    &section, &noSections)) {
		fprintf(pgpout,
			LANG("Badly formed section delimiter, part %d.\n"),
			currentSection + 1);
		goto error;
	    }
	    if (section != ++currentSection) {
		fprintf(pgpout,
LANG("Sections out of order, expected part %d"), currentSection);
		if (section)
		    fprintf(pgpout,
			    LANG(", got part %d\n"), section);
		else
		    fputc('\n', pgpout);
		goto error;
	    }
	    /* Skip header after BEGIN line */
	    if (skipheaders(in, inbuf, warned, 1) < 0)
		goto error;
	    if (feof(in)) {
		fprintf(pgpout,
		   LANG("ERROR: Hit EOF in header of section %d.\n"),
			currentSection);
		goto error;
	    }
		
	    /* Continue decoding */
	    continue;
	}
#ifdef MACTC5
	mac_poll_for_break();
#endif

/* Quit when hit the -----END PGP MESSAGE----- line or a blank,
 * or handle checksum
 */
	if (inbuf[0] == PAD) {	/* Checksum lines start
				   with PAD char */
	    /* If the already-armored file is sent through MIME
	     * and gets armored again, '=' will become '=3D'.
	     * To make life easier, we detect and work around this
	     * idiosyncracy.
	     */
	    if (strlen(inbuf) == 7 &&
		inbuf[1] == '3' && inbuf[2] == 'D')
		status = darmor_buffer(inbuf + 3, crcbuf, &n);
	    else
		status = darmor_buffer(inbuf + 1, crcbuf, &n);
	    if (status < 0 || n != 3) {
		fprintf(pgpout,
LANG("ERROR: Badly formed ASCII armor checksum, line %d.\n"), line);
                goto error;
	    }
	    chkcrc = (((long) crcbuf[0] << 16) & 0xff0000L) +
		((crcbuf[1] << 8) & 0xff00L) + (crcbuf[2] & 0xffL);
	    gotcrc = 1;
	    continue;
	}
	if (inbuf[0] == '\0') {
	    fprintf(pgpout,
		    LANG("WARNING: No ASCII armor `END' line.\n"));
	    break;
	}
	if (strncmp(inbuf, "-----END PGP ", 13) == 0)
	    break;

	status = darmor_buffer(inbuf, outbuf, &n);

	if (status == -1) {
	    fprintf(pgpout,
	     LANG("ERROR: Bad ASCII armor character, line %d.\n"), line);
	    gotcrc = 1;		/* this will print part number,
				   continue with next part */
	    ret_code = -1;
	}
	if (n > sizeof outbuf) {
	    fprintf(pgpout,
	     LANG("ERROR: Bad ASCII armor line length %d on line %d.\n"),
		    n, line);
	    goto error;
	}
	crc = crcbytes((byte *) outbuf, n, crc);
	if (fwrite(outbuf, 1, n, out) != n) {
	    ret_code = -1;
	    break;
	}
    }				/* line */

    if (gotcrc) {
	if (chkcrc != crc) {
	    fprintf(pgpout,
		    LANG("ERROR: Bad ASCII armor checksum"));
	    if (noSections > 0)
		fprintf(pgpout,
			LANG(" in section %d"), noSections);
	    fputc('\n', pgpout);
	    goto error;
	}
    } else {
	fprintf(pgpout,
		LANG("Warning: Transport armor lacks a checksum.\n"));
    }

    if (in != original_in)
	fclose(in);
    return ret_code;		/* normal return */
error:
    if (in != original_in)
	fclose(in);
    return -1;			/* error return */
}				/* armordecode */

static boolean
is_armorfile(char *infile)
{
    FILE *in;
    char inbuf[MAX_LINE_SIZE];
    char outbuf[MAX_LINE_SIZE];
    int n;
    long il;

    in = fopen(infile, FOPRARMOR);
    if (in == NULL)
	return FALSE;	/* can't open file */
   
    /* Read to infile_line before we begin looking */
    for (il = 0; il < infile_line; ++il) {
	if (get_armor_line(inbuf, in) == NULL) {
	    fclose(in);
	    return FALSE;
	}
    }

    /* search file for delimiter line */
    for (;;) {
	if (get_armor_line(inbuf, in) == NULL)
	    break;
	if (strncmp(inbuf, "-----BEGIN PGP ", 15) == 0) {
	    if (strncmp(inbuf,
		    "-----BEGIN PGP SIGNED MESSAGE-----", 34) == 0) {
		fclose(in);
		return TRUE;
	    }
	    n = 1;	/* Don't print warnings yet */
	    if (skipheaders(in, inbuf, &n, 1) < 0 ||
	        get_armor_line(inbuf, in) == NULL ||
	        darmor_buffer(inbuf, outbuf, &n) < 0)
		break;
	    fclose(in);
	    return TRUE;
	}
    }

    fclose(in);
    return FALSE;
}				/* is_armorfile */

static int
darmor_file(char *infile, char *outfile)
{
    FILE *in, *out;
    char buf[MAX_LINE_SIZE];
    char outbuf[(MAX_LINE_SIZE*3+3)/4];
    int status, n;
    long il, fpos;
    char *litfile = NULL;
    int header_warned = 0;	/* Complained about unknown header */

    if ((in = fopen(infile, FOPRARMOR)) == NULL) {
	fprintf(pgpout, LANG("ERROR: Can't find file %s\n"), infile);
	return 10;
    }
    strcpy(armorfilename, infile);	/* store filename for multi-parts */

    /* Skip to infile_line */
    for (il = 0; il < infile_line; ++il) {
	if (get_armor_line(buf, in) == NULL) {
	    fclose(in);
	    return -1;
	}
    }

    /* Loop through file, searching for delimiter.  Decode anything with a
       delimiter, complain if there were no delimiter. */

    /* search file for delimiter line */
    for (;;) {
	++infile_line;
	if (get_armor_line(buf, in) == NULL) {
	    fprintf(pgpout,
		    LANG("ERROR: No ASCII armor `BEGIN' line!\n"));
	    fclose(in);
	    return 12;
	}
	if (strncmp(buf, "-----BEGIN PGP ", 15) == 0)
	    break;
    }
    if (strncmp(buf, "-----BEGIN PGP SIGNED MESSAGE-----", 34) == 0) {
	FILE *litout;
	char *p;
	int nline;

	/*
	 * It would be nice to allow headers here, as we could add
	 * additional information to PGP messages, but it appears to
	 * be too easy to spoof, given standard text viewers.  So,
	 * forbid it outright until we sit down and figure out how to
	 * thwart all the ways of faking an end-of-headers.  The
	 * possibilities are:
	 * - Enough trailing whitespace on a valid-looking line to force a
	 *   line wrap.  The 80 column case is tricky, as the classical
	 *   Big Iron IBM mainframe pads to 80 columns, and some terminal
	 *   and text viewer combinations cause a blank line, while others
	 *   don't.  A line that is exactly 80 columns wide but ends in
	 *   a non-blank would do, too.
	 * - A big pile of whitespace within a line, enough to 
	 *   produce something that looks like a blank line between
	 *   the beginning and end parts.
	 * - Various cursor-control sequences.
	 * Basically, it's a nasty problem.  A very strong case can be made
	 * for the argument that it's the text viewer's problem, and outside
	 * PGP's jurisdiction, but that has a few conflicts with reality.
	 */
	charset_header[0] = '\0';
	if (get_armor_line(buf, in) == NULL) {
		fprintf(pgpout,
LANG("ERROR: ASCII armor decode input ended unexpectedly!\n"));
		fclose(in);
		return 12;
	}
	if (buf[0] != '\0') {
		fprintf(pgpout,
LANG("ERROR: Header line added to ASCII armor: \"%s\"\n\
ASCII armor corrupted.\n"), buf);
		fclose(in);
		return -1;
		
	}
		
	litfile = tempfile(TMP_WIPE | TMP_TMPDIR);
	if ((litout = fopen(litfile, FOPWTXT)) == NULL) {
	    fprintf(pgpout,
LANG("\n\007Unable to write ciphertext output file '%s'.\n"), litfile);
	    fclose(in);
	    return -1;
	}

	status = 0;
	for (;;) {
	    ++infile_line;
	    nline = status;
	    status = getline(buf, sizeof buf, in);
	    if (status < 0) {
		fprintf(pgpout,
LANG("ERROR: ASCII armor decode input ended unexpectedly!\n"));
		fclose(in);
		fclose(litout);
		rmtemp(litfile);
		return 12;
	    }
	    if (strncmp(buf, "-----BEGIN PGP ", 15) == 0)
		break;
	    if (nline)
		putc('\n', litout);
	    /* De-quote lines starting with '- ' */
	    fputs(buf + ((buf[0] == '-' && buf[1] == ' ') ? 2 : 0), litout);
	    /* Copy trailing part of line, if any. */
	    if (!status)
		status = copyline(in, litout);
	    /* Ignore error; getline will discover it again */
	}
	fflush(litout);
	if (ferror(litout)) {
	    fclose(litout);
	    fclose(in);
	    rmtemp(litfile);
	    return -1;
	}
	fclose(litout);
    }
    /* Skip header after BEGIN line */
    if (skipheaders(in, buf, &header_warned, 1) < 0) {
	fclose(in);
	return -1;
    }
    if (feof(in)) {
	fprintf(pgpout, LANG("ERROR: Hit EOF in header.\n"));
	fclose(in);
	return 13;
    }

    if ((out = fopen(outfile, FOPWBIN)) == NULL) {
	fprintf(pgpout,
LANG("\n\007Unable to write ciphertext output file '%s'.\n"), outfile);
	fclose(in);
	return -1;
    }
    status = armordecode(in, out, &header_warned);

    if (litfile) {
	char *canonfile, hold_charset[16];
	char lit_mode = MODE_TEXT;
	word32 dummystamp = 0;
	FILE *f;

	/* Convert clearsigned message to internal character set */
	canonfile = tempfile(TMP_WIPE | TMP_TMPDIR);
	strip_spaces = TRUE;
        if (charset_header[0]) {
            strcpy(hold_charset, charset);
            strcpy(charset, charset_header);
            init_charset();
        }
	make_canonical(litfile, canonfile);
	rmtemp(litfile);
	litfile = canonfile;
        if (charset_header[0]) {
            strcpy(charset, hold_charset);
            init_charset();
	}
	/* Glue the literal file read above to the signature */
        f = fopen(litfile, FOPRBIN);

	write_ctb_len(out, CTB_LITERAL2_TYPE, fsize(f) + 6, FALSE);
	fwrite(&lit_mode, 1, 1, out);	/* write lit_mode */
	fputc('\0', out);	/* No filename */
	fwrite(&dummystamp, 1, sizeof(dummystamp), out);
	/* dummy timestamp */
	copyfile(f, out, -1L);	/* Append literal file */
	fclose(f);
	rmtemp(litfile);
    }
    if (write_error(out))
	status = -1;
    fclose(out);
    fclose(in);
    return status;
}				/* darmor_file */

/* Entry points for generic interface names */

int de_armor_file(char *infile, char *outfile, long *curline)
{
    int status;

    if (verbose)
	fprintf(pgpout,
	     "de_armor_file: infile = %s, outfile = %s, curline = %ld\n",
		infile, outfile, *curline);
    infile_line = (curline ? *curline : 0);
    status = darmor_file(infile, outfile);
    if (curline)
	*curline = infile_line;
    return status;
}

boolean
is_armor_file(char *infile, long startline)
{
    infile_line = startline;
    return is_armorfile(infile);
}


/* ==== charset.c ==== */
/*
 * charset.c
 *
 * Conversion tables and routines to support different character sets.
 * The PGP internal format is latin-1.
 *
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Code that has been incorporated into PGP from other sources was
 * either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "usuals.h"
#include "language.h"
#include "charset.h"
#include "system.h"

#ifndef NULL
#define	NULL	0
#endif

#define UNK	'?'

static unsigned char
intern2ascii[] = { /* ISO 8859-1 Latin Alphabet 1 to US ASCII */
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
 32,  33,  99,  35,  36,  89, 124,  80,  34,  67,  97,  34, 126,  45,  82,  95,
111, UNK,  50,  51,  39, 117,  45,  45,  44,  49, 111,  34, UNK, UNK, UNK,  63,
 65,  65,  65,  65,  65,  65,  65,  67,  69,  69,  69,  69,  73,  73,  73,  73,
 68,  78,  79,  79,  79,  79,  79, 120,  79,  85,  85,  85,  85,  89,  84, 115,
 97,  97,  97,  97,  97,  97,  97,  99, 101, 101, 101, 101, 105, 105, 105, 105,
100, 110, 111, 111, 111, 111, 111,  47, 111, 117, 117, 117, 117, 121, 116, 121
};

static unsigned char
intern2cp850[] = { /* ISO 8859-1 Latin Alphabet 1 (Latin-1)
		      to IBM Code Page 850 (International) */
186, 205, 201, 187, 200, 188, 204, 185, 203, 202, 206, 223, 220, 219, 254, 242,
179, 196, 218, 191, 192, 217, 195, 180, 194, 193, 197, 176, 177, 178, 213, 159,
255, 173, 189, 156, 207, 190, 221, 245, 249, 184, 166, 174, 170, 240, 169, 238,
248, 241, 253, 252, 239, 230, 244, 250, 247, 251, 167, 175, 172, 171, 243, 168,
183, 181, 182, 199, 142, 143, 146, 128, 212, 144, 210, 211, 222, 214, 215, 216,
209, 165, 227, 224, 226, 229, 153, 158, 157, 235, 233, 234, 154, 237, 232, 225,
133, 160, 131, 198, 132, 134, 145, 135, 138, 130, 136, 137, 141, 161, 140, 139,
208, 164, 149, 162, 147, 228, 148, 246, 155, 151, 163, 150, 129, 236, 231, 152
};

static unsigned char
cp8502intern[] = { /* IBM Code Page 850 to Latin-1 */
199, 252, 233, 226, 228, 224, 229, 231, 234, 235, 232, 239, 238, 236, 196, 197,
201, 230, 198, 244, 246, 242, 251, 249, 255, 214, 220, 248, 163, 216, 215, 159,
225, 237, 243, 250, 241, 209, 170, 186, 191, 174, 172, 189, 188, 161, 171, 187,
155, 156, 157, 144, 151, 193, 194, 192, 169, 135, 128, 131, 133, 162, 165, 147,
148, 153, 152, 150, 145, 154, 227, 195, 132, 130, 137, 136, 134, 129, 138, 164,
240, 208, 202, 203, 200, 158, 205, 206, 207, 149, 146, 141, 140, 166, 204, 139,
211, 223, 212, 210, 245, 213, 181, 254, 222, 218, 219, 217, 253, 221, 175, 180,
173, 177, 143, 190, 182, 167, 247, 184, 176, 168, 183, 185, 179, 178, 142, 160
};

static unsigned char
intern2cp852[] = { /* ISO 8859-2 Latin Alphabet 2 (Latin-2)
		      to IBM Code Page 852 (Eastern Europe) */
186, 205, 201, 187, 200, 188, 204, 185, 203, 202, 206, 223, 220, 219, 254, UNK,
179, 196, 218, 191, 192, 217, 195, 180, 194, 193, 197, 176, 177, 178, UNK, UNK,
255, 164, 244, 157, 207, 149, 151, 245, 249, 230, 184, 155, 141, 240, 166, 189,
248, 165, 247, 136, 239, 150, 151, 243, 242, 231, 173, 156, 171, 241, 167, 190,
232, 181, 182, 198, 142, 145, 143, 128, 172, 144, 168, 211, 183, 214,  73, 210,
209, 227, 213, 224, 226, 138, 153, 158, 252, 222, 233, 235, 154, 237, 221, 225,
234, 160, 131, 199, 132, 146, 134, 135, 159, 130, 169, 137, 216, 161, 140, 212,
208, 228, 229, 162, 147, 139, 148, 246, 253, 133, 163, 251, 129, 236, 238, 250
};

static unsigned char
cp8522intern[] = { /* IBM Code Page 852 to Latin-2 */
199, 252, 233, 226, 228, 249, 230, 231, 179, 235, 213, 245, 238, 172, 196, 198,
201, 197, 229, 244, 246, 165, 181, 166, 182, 214, 220, 171, 187, 163, 215, 232,
225, 237, 243, 250, 161, 177, 174, 190, 202, 234, UNK, 188, 200, 186,  60,  62,
155, 156, 157, 144, 151, 193, 194, 204, 170, 135, 128, 131, 133, 175, 191, 147,
148, 153, 152, 150, 145, 154, 195, 227, 132, 130, 137, 136, 134, 129, 138, 164,
240, 208, 207, 203, 239, 210, 205,  85, 236, 149, 146, 141, 140, 222, 217, 139,
211, 223, 212, 209, 241, 242, 169, 185, 192, 218, 224, 219, 253, 221, 254, 180,
173, 189, 184, 183, 162, 167, 247, 178, 176, 168, 255, 251, 216, 248, 142, 160
};

static unsigned char
intern2cp860[] = { /* ISO 8859-1 Latin Alphabet 1 (Latin-1)
                      to IBM Code Page 860 (Portuguese) */
186, 205, 201, 187, 200, 188, 204, 185, 203, 202, 206, 223, 220, 219, 254,  95,
179, 196, 218, 191, 192, 217, 195, 180, 194, 193, 197, 176, 177, 178, UNK, UNK,
255, 173, 155, 156,  36,  89, 124,  80,  34,  67, 166, 174, 170,  45,  82,  95,
248, 241, 253,  51,  39, 230,  45, 250,  44,  49, 167, 175, 172, 171, UNK, 168,
145, 134, 143, 142,  65,  65,  65, 128, 146, 144, 137,  69, 152, 139,  73,  73,
 68, 165, 169, 159, 140, 153,  79, 120,  79, 157, 150,  85, 154,  89,  84, 225,
133, 160, 131, 132,  97,  97,  97, 135, 138, 130, 136, 101, 141, 161, 105, 105,
100, 164, 149, 162, 147, 148, 111, 246, 111, 151, 163, 117, 129, 121, 116, 121
};

static unsigned char
cp8602intern[] = { /* IBM Code Page 860 to Latin-1 */
199, 252, 233, 226, 227, 224, 193, 231, 234, 202, 232, 205, 212, 236, 195, 194,
201, 192, 200, 244, 245, 242, 218, 249, 204, 213, 220, 162, 163, 217, 164, 211,
225, 237, 243, 250, 241, 209, 170, 186, 191, 210, 172, 189, 188, 161, 171, 187,
155, 156, 157, 144, 151, 151, 135, 147, 131, 135, 128, 131, 133, 149, 133, 147,
148, 153, 152, 150, 145, 154, 150, 134, 132, 130, 137, 136, 134, 129, 138, 137,
153, 136, 152, 148, 132, 130, 146, 154, 138, 149, 146, 141, 140, 141, 141, 139,
UNK, 223, UNK, UNK, UNK, UNK, 181, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
 61, UNK, UNK, UNK, UNK, UNK, 247, 126, 176, 183, 183, UNK, 110, 178, 142, 160
};

static unsigned char
intern2keybcs[] = { /* ISO 8859-2 Latin Alphabet 2 (Latin-2)
 		       to KEYBCS2 (Eastern Europe) */
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK,  65, UNK,  76, UNK, 156,  83, 173, UNK, 155,  83, 134,  90, UNK, 146,  90,
248, UNK, UNK, 108, UNK, 140, 115, UNK, UNK, 168, 115, 159, 122, UNK, 145, 122,
171, 143,  65,  65, 142, 138,  67,  67, 128, 144,  69,  69, 137, 139,  73, 133,
 68,  78, 165, 149, 167,  79, 153, UNK, 158, 166, 151,  85, 154, 157,  84, 225,
170, 160,  97,  97, 132, 141,  99,  99, 135, 130, 101, UNK, 136, 161, 105, 131,
100, 110, 164, 162, 147, 111, 148, UNK, 169, 150, 163, 117, 129, 152, 116, UNK
};

static unsigned char
keybcs2intern[] = { /* KEYBCS2 to Latin-2 */
200, 252, 233, 239, 228, 207, 171, 232, 236, 204, 197, 205, 181, 229, 196, 193,
201, 190, 174, 244, 246, 211, 249, 218, 253, 214, 220, 169, 165, 221, 216, 187,
225, 237, 243, 250, 242, 210, 217, 212, 185, 248, 224, 192, UNK, 167, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, 223, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, 176, UNK, UNK, UNK, UNK, UNK, UNK, UNK
};

static unsigned char
intern2next[] = { /* ISO 8859-1 Latin Alphabet 1 (Latin-1)
                     to NeXTSTEP char set */
 UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,  UNK,
0300, 0301, 0302, 0303, 0304, 0305, 0306, 0307, 0310,  UNK, 0312, 0313,  UNK, 0272, 0316, 0247,
 UNK, 0241, 0242, 0243, 0250, 0245, 0265,  UNK, 0310, 0240, 0343, 0253, 0276, 0261, 0260, 0320,
0312, 0321, 0311, 0314, 0270, 0235, 0266, 0267, 0313, 0300,  UNK, 0273, 0322, 0323, 0324, 0277,
0201, 0202, 0203, 0204, 0205, 0206, 0341, 0207, 0210, 0211, 0212, 0213, 0214, 0215, 0216, 0217,
0220, 0221, 0222, 0223, 0224, 0225, 0226, 0236, 0351, 0227, 0230, 0231, 0232, 0233, 0234, 0373,
0325, 0326, 0327, 0330, 0331, 0332, 0361, 0333, 0334, 0335, 0336, 0337, 0340, 0342, 0344, 0345,
0346, 0347, 0354, 0355, 0356, 0357, 0360, 0237, 0371, 0362, 0363, 0364, 0366, 0367, 0374, 0375
};

static unsigned char
next2intern[] = { /* NeXTSTEP char set to Latin-1 */
 UNK, 0300, 0301, 0302, 0303, 0304, 0305, 0307, 0310, 0311, 0312, 0313, 0314, 0315, 0316, 0317,
0320, 0321, 0322, 0323, 0324, 0325, 0326, 0331, 0332, 0333, 0334, 0335, 0336, 0265, 0337, 0267,
0251, 0241, 0242, 0243, 0057, 0245, 0146, 0247, 0244, 0140, 0042, 0253, 0074, 0076,  UNK,  UNK,
0256, 0255,  UNK,  UNK, 0056, 0246, 0266, 0267, 0054, 0042, 0235, 0273,  UNK,  UNK, 0254, 0277,
0220, 0221, 0222, 0223, 0224, 0225, 0226, 0232, 0230, 0262, 0227, 0270, 0263, 0042, 0236, 0226,
0257, 0261, 0274, 0275, 0276, 0340, 0341, 0342, 0343, 0344, 0345, 0347, 0350, 0351, 0352, 0353,
0354, 0306, 0355, 0252, 0356, 0357, 0365, 0361, 0243, 0330,  UNK, 0272, 0362, 0363, 0364, 0365,
0366, 0346, 0371, 0372, 0373, 0151, 0374, 0375, 0154, 0370,  UNK, 0337, 0376, 0377, UNK, UNK
};

#ifdef MACTC5
Boolean iso_latin1 = false;
#endif

static unsigned char
intern2mac[] = { /* ISO 8859-1 Latin Alphabet 1 (Latin1)
                    to Macintosh Geneva/Monaco */
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
202, 193, 162, 163, 217, 180, 124, 164, 172, 169, 187, 199, 194, 209, 168,  95,
188, 177,  50,  51, 171, 181, 166, 165,  44,  49, 161, 200, UNK, UNK, UNK, 192,
203, 135, 129, 204, 128, 129, 174, 130, 143, 131, 144, 145, 147, 146, 148, 149,
 68, 132, 152, 151, 153, 205, 133, 120, 175, 157, 156, 158, 134,  89,  84, 167,
136, 135, 137, 139, 138, 140,  97, 141, 143, 142, 144, 145, 147, 146, 148, 149,
100, 150, 152, 151, 153, 155, 154, 214, 191, 157, 156, 158, 159, 121, 116, 216
};

static unsigned char
mac2intern[] = { /* Macintosh Geneva/Monaco to Latin-1 */
196, 197, 199, 201, 209, 214, 220, 225, 224, 226, 228, 227, 229, 231, 233, 232,
234, 235, 237, 236, 238, 239, 241, 243, 242, 244, 246, 245, 250, 249, 251, 252,
UNK, 186, 162, 163, 167, 183, 182, 223, 174, 169, UNK, 180, 168, UNK, 198, 216,
UNK, 177, UNK, UNK, 165, 181, 100,  83,  80, 112,  83, 170, 176,  79, 230, 248,
191, 161, 172, UNK, 102, 126,  68, 171, 187, UNK, 160, 192, 195, 213,  79, 111,
 45, 173,  34,  34,  96,  39, 247, UNK, 255, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK,
UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK, UNK
};

/* Russian language specific conversation section */
/* Two point-to-point charset decode tables       */
/* produced by Andrew A. Chernov                  */
/* Decode single char from KOI8-R to ALT-CODES, if present */
static unsigned char intern2alt[] = {
	0xc4, 0xb3, 0xda, 0xbf, 0xc0, 0xd9, 0xc3, 0xb4,
	0xc2, 0xc1, 0xc5, 0xdf, 0xdc, 0xdb, 0xdd, 0xde,
	0xb0, 0xb1, 0xb2, 0xf4, 0xfe, 0xf9, 0xfb, 0xf7,
	0xf3, 0xf2, 0xff, 0xf5, 0xf8, 0xfd, 0xfa, 0xf6,
	0xcd, 0xba, 0xd5, 0xf1, 0xd6, 0xc9, 0xb8, 0xb7,
	0xbb, 0xd4, 0xd3, 0xc8, 0xbe, 0xbd, 0xbc, 0xc6,
	0xc7, 0xcc, 0xb5, 0xf0, 0xb6, 0xb9, 0xd1, 0xd2,
	0xcb, 0xcf, 0xd0, 0xca, 0xd8, 0xd7, 0xce, 0xfc,
	0xee, 0xa0, 0xa1, 0xe6, 0xa4, 0xa5, 0xe4, 0xa3,
	0xe5, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae,
	0xaf, 0xef, 0xe0, 0xe1, 0xe2, 0xe3, 0xa6, 0xa2,
	0xec, 0xeb, 0xa7, 0xe8, 0xed, 0xe9, 0xe7, 0xea,
	0x9e, 0x80, 0x81, 0x96, 0x84, 0x85, 0x94, 0x83,
	0x95, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
	0x8f, 0x9f, 0x90, 0x91, 0x92, 0x93, 0x86, 0x82,
	0x9c, 0x9b, 0x87, 0x98, 0x9d, 0x99, 0x97, 0x9a
};

/* Decode single char from ALT-CODES, if present, to KOI8-R */
static unsigned char alt2intern[] = {
	0xe1, 0xe2, 0xf7, 0xe7, 0xe4, 0xe5, 0xf6, 0xfa,
	0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
	0xf2, 0xf3, 0xf4, 0xf5, 0xe6, 0xe8, 0xe3, 0xfe,
	0xfb, 0xfd, 0xff, 0xf9, 0xf8, 0xfc, 0xe0, 0xf1,
	0xc1, 0xc2, 0xd7, 0xc7, 0xc4, 0xc5, 0xd6, 0xda,
	0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
	0x90, 0x91, 0x92, 0x81, 0x87, 0xb2, 0xb4, 0xa7,
	0xa6, 0xb5, 0xa1, 0xa8, 0xae, 0xad, 0xac, 0x83,
	0x84, 0x89, 0x88, 0x86, 0x80, 0x8a, 0xaf, 0xb0,
	0xab, 0xa5, 0xbb, 0xb8, 0xb1, 0xa0, 0xbe, 0xb9,
	0xba, 0xb6, 0xb7, 0xaa, 0xa9, 0xa2, 0xa4, 0xbd,
	0xbc, 0x85, 0x82, 0x8d, 0x8c, 0x8e, 0x8f, 0x8b,
	0xd2, 0xd3, 0xd4, 0xd5, 0xc6, 0xc8, 0xc3, 0xde,
	0xdb, 0xdd, 0xdf, 0xd9, 0xd8, 0xdc, 0xc0, 0xd1,
	0xb3, 0xa3, 0x99, 0x98, 0x93, 0x9b, 0x9f, 0x97,
	0x9c, 0x95, 0x9e, 0x96, 0xbf, 0x9d, 0x94, 0x9a
};

/*
 * Most Unixes has KOI8, and DOS has ALT_CODES
 * If your Unix is non-standard, set CHARSET to "alt_codes"
 * in config.txt
 */

#ifndef	DEFAULT_RU_CSET
#ifdef MSDOS
#define DEFAULT_RU_CSET "alt_codes"
#else
#define DEFAULT_RU_CSET "koi8"
#endif
#endif

/* End of Russian section */

#ifndef	DEFAULT_CSET
#if defined(MSDOS) || defined(OS2)
#define DEFAULT_CSET    "cp850"
#elif defined(NEXT)
#define	DEFAULT_CSET	"next"
#elif defined(MACTC5)
#define	DEFAULT_CSET	"mac"
#else
#define	DEFAULT_CSET	"noconv"
#endif
#endif

#ifdef EBCDIC
/* ebcdic-ascii converting, accustom to your local MVS-settings */
/* in this case it's taken from c370.c                          */
#define ebcdic__ascii ebcdic_ascii
#define ascii__ebcdic ascii_ebcdic
#endif /* EBCDIC */

int CONVERSION = NO_CONV;      /* None text file conversion at start time */

unsigned char *ext_c_ptr;
static unsigned char *int_c_ptr;

char charset[16] = "";

void
init_charset(void)
{
	ext_c_ptr = NULL;	/* NULL means latin1 or KOI8
				   (internal format) */
	int_c_ptr = NULL;

	if (charset[0] == '\0') {
		/* use default character set for this system */
#ifdef MACTC5
		if (iso_latin1)
			strcpy(charset, DEFAULT_CSET);
		else
			strcpy(charset, "noconv");
#else
		if (strcmp(language, "ru") == 0)
			strcpy(charset, DEFAULT_RU_CSET);
		else
			strcpy(charset, DEFAULT_CSET);
#endif
	} else {
		strlwr(charset);
	}

	/* latin-1 and KOI8 are in internal format: no conversion needed */
	if (!strcmp(charset, "latin1") || !strcmp(charset, "koi8") ||
		!strcmp(charset, "noconv"))
		return;

	if (!strcmp(charset, "cp850")) {
		ext_c_ptr = intern2cp850;
		int_c_ptr = cp8502intern;
	} else if (!strcmp(charset, "cp852")) {
		ext_c_ptr = intern2cp852;
		int_c_ptr = cp8522intern;
	} else if (!strcmp(charset, "cp860")) {
		ext_c_ptr = intern2cp860;
		int_c_ptr = cp8602intern;
 	} else if (!strcmp(charset, "cp866")) {
 		ext_c_ptr = intern2alt;
 		int_c_ptr = alt2intern;
	} else if (!strcmp(charset, "alt_codes")) {
		ext_c_ptr = intern2alt;
		int_c_ptr = alt2intern;
	} else if (!strcmp(charset, "keybcs2"))  {
	        ext_c_ptr = intern2keybcs;
		int_c_ptr = keybcs2intern;
	} else if (!strcmp(charset, "next"))  {
	        ext_c_ptr = intern2next;
		int_c_ptr = next2intern;
	} else if (!strcmp(charset, "mac"))  {
	        ext_c_ptr = intern2mac;
		int_c_ptr = mac2intern;
	} else if (!strcmp(charset, "ascii")) {
		ext_c_ptr = intern2ascii;
	} else {
		fprintf(stderr, LANG("Unsupported character set: '%s'\n"),
			charset);
		strcpy(charset, "noconv");
	}
}

#ifdef EBCDIC
char EXT_C(char c)  { return ascii__ebcdic[c]; }
char INT_C(char c)  { return ebcdic__ascii[c]; }
#else /* !EBCDIC */
char
EXT_C(char c)
{
 	if (!(c & 0x80) || !ext_c_ptr)
		return c;
	return ext_c_ptr[c & 0x7f];
}

char
INT_C(char c)
{
 	if (!(c & 0x80) || !int_c_ptr)
		return c;
	return int_c_ptr[c & 0x7f];
}
#endif /* !EBCDIC */

/*
 * to_upper() and to_lower(), replacement for toupper() and tolower(),
 * calling to_upper() on uppercase or to_lower on lowercase characters
 * is handled correctly.
 * 
 * XXX: should handle local characterset when 8-bit userID's are allowed
 */
#ifdef EBCDIC
/* With EBCDIC-charset things like (c >= 'a' && c <= 'z') do not work!!!
 * Therefor use the appropriate ctype-functions
 */
#include <ctype.h>
int to_upper(int c) { return toupper(c); }
int to_lower(int c) { return tolower(c); }
#else /* !EBCDIC */
int
to_upper(int c)
{
 	c &= 0xFF;
 	if (islower(c))
 		return (toupper(c));
 	return c;
}

int
to_lower(int c)
{
 	c &= 0xFF;
 	if (isupper(c))
 		return (tolower(c));
 	return c;
}
#endif /* !EBCDIC */

#ifdef EBCDIC
void CONVERT_TO_CANONICAL_CHARSET(char *s) /* String to internal string (at same place) */
{
	for (; *s; s++) *s = INT_C(*s);
}

static char buf[128];
char * LOCAL_CHARSET( char *s)             /* String to external string (at extra place) */
{
	strcpy( buf, s );
	for (s=buf; *s; s++) *s = EXT_C(*s);
	return buf;
}
#endif /* EBCDIC */


/* ==== config.c ==== */
/*	config.c  - config file parser by Peter Gutmann
	Parses config file for PGP

	(c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
	The author assumes no liability for damages resulting from the use
	of this software, even if the damage results from defects in this
	software.  No warranty is expressed or implied.

	Note that while most PGP source modules bear Philip Zimmermann's
	copyright notice, many of them have been revised or entirely written
	by contributors who frequently failed to put their names in their
	code.  Code that has been incorporated into PGP from other authors
	was either originally published in the public domain or is used with
	permission from the various authors.

	PGP is available for free to the public under certain restrictions.
	See the PGP User's Guide (included in the release package) for
	important information about licensing, patent restrictions on
	certain algorithms, trademarks, copyrights, and export controls.

	Modified 24 Jun 92 - HAJK
	Misc fixes for VAX C restrictions

	Updated by Peter Gutmann to only warn about unrecognized options,
	so future additions to the config file will give old versions a
	chance to still run.  A number of code cleanups, too.  */

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "usuals.h"
#include "fileio.h"
#include "language.h"	/*added by Naoki */
#include "pgp.h"
#include "config.h"
#include "charset.h"

/* Various maximum/minimum allowable settings for config options */

#define MIN_MARGINALS	1
#define MIN_COMPLETE	1
#define MAX_COMPLETE	4
#define MIN_CERT_DEPTH	0
#define MAX_CERT_DEPTH	8

/* Prototypes for local functions */

static int lookup( char *key, int keyLength, char *keyWords[], int range );
static int extractToken( char *buffer, int *endIndex, int *length );
static int getaString( char *buffer, int *endIndex );
static int getAssignment( char *buffer, int *endIndex, INPUT_TYPE settingType );
static void processAssignment( int intrinsicIndex );

/* The external config variables we can set here are referenced in pgp.h */

/* Return values */

#define ERROR	-1
#define OK		0

/* The types of error we check for */

enum { NO_ERROR, ILLEGAL_CHAR_ERROR, LINELENGTH_ERROR };

#define CPM_EOF		0x1A	/* ^Z = CPM EOF char */

#define MAX_ERRORS	3	/* Max.no.errors before we give up */

#define LINEBUF_SIZE	100	/* Size of input buffer */

static int line;		/* The line on which an error occurred */
static int errCount;		/* Total error count */
static boolean hasError;	/* Whether this line has an error in it */

/* The settings parsed out by getAssignment() */

static char str[ LINEBUF_SIZE ];
static int value;
static char *errtag;		/* Prefix for printing error messages */
static char optstr[ 100 ];	/* Option being processed */
#ifdef MACTC5
extern boolean use_ftypes, wipe_warning, recycle_passwd;
#endif

/* A .CFG file roughly follows the format used in the world-famous HPACK
   archiver and is as follows:

	- Leading spaces/tabs (whitespace) are ignored.

	- Lines with a '#' as the first non-whitespace character are treated
	  as comment lines.

	- All other lines are treated as config options for the program.

	- Lines may be terminated by either linefeeds, carriage returns, or
	  carriage return/linefeed pairs (the latter being the DOS default
	  method of storing text files).

	- Config options have the form:

	  <option> '=' <setting>

	  where <setting> may be 'on', 'off', a numeric value, or a string
	  value.

	- If strings have spaces or the '#' character inside them they must be
	  surrounded by quote marks '"' */

/* Intrinsic variables */

#define NO_INTRINSICS		(sizeof(intrinsics) / sizeof(intrinsics[0]))
#define CONFIG_INTRINSICS	BATCHMODE

enum {
	ARMOR, COMPRESS, SHOWPASS, KEEPBINARY, LANGUAGE,
	MYNAME, TEXTMODE, TMP, TZFIX, VERBOSE, BAKRING,
	ARMORLINES, COMPLETES_NEEDED, MARGINALS_NEEDED, PAGER,
	CERT_DEPTH, CHARSET, CLEARSIG, SELF_ENCRYPT,
	INTERACTIVE, PUBRING, SECRING, RANDSEED,
	COMMENT, AUTOSIGN,
        LEGAL_KLUDGE,
#ifdef MACTC5
	FILE_TYPES, WIPE_WARNING, RECYCLE_PASSWD, MULTIPLE_RECIPIENTS,
#endif
	/* options below this line can only be used as command line
	 * "long" options */
	BATCHMODE, FORCE, NOMANUAL, MAKERANDOM
	};

static char *intrinsics[] = {
	"ARMOR", "COMPRESS", "SHOWPASS", "KEEPBINARY", "LANGUAGE",
	"MYNAME", "TEXTMODE", "TMP", "TZFIX", "VERBOSE", "BAKRING",
	"ARMORLINES", "COMPLETES_NEEDED", "MARGINALS_NEEDED", "PAGER",
	"CERT_DEPTH", "CHARSET", "CLEARSIG", "ENCRYPTTOSELF", 
	"INTERACTIVE", "PUBRING", "SECRING", "RANDSEED",
	"COMMENT", "AUTOSIGN", 
        "LEGAL_KLUDGE",
#ifdef MACTC5
	"FILE_TYPES", "WIPE_WARNING", "RECYCLE_PASSWORDS", "MULTIPLE_RECIPIENTS",
#endif
	/* command line only */
	"BATCHMODE", "FORCE", "NOMANUAL", "MAKERANDOM"
	};

static INPUT_TYPE intrinsicType[] = {
	BOOL, BOOL, BOOL, BOOL, STRING,
	STRING, BOOL, STRING, NUMERIC, NUMERIC, STRING,
	NUMERIC, NUMERIC, NUMERIC, STRING,
	NUMERIC, STRING, BOOL, BOOL,
	BOOL, STRING, STRING, STRING,
	STRING, BOOL,
        BOOL,
#ifdef MACTC5
	BOOL, BOOL, BOOL, BOOL,
#endif
	/* command line only */
	BOOL, BOOL, BOOL, NUMERIC
	};

/* Possible settings for variables */

#define NO_SETTINGS			2

static char *settings[] = { "OFF", "ON" };

/* Search a list of keywords for a match */

static int lookup( char *key, int keyLength, char *keyWords[], int range )
{
	int index, position = 0, noMatches = 0;

	strncpy( optstr, key, keyLength );
	optstr[ keyLength ] = '\0';

	/* Make the search case insensitive */
	for( index = 0; index < keyLength; index++ )
		key[ index ] = to_upper( key[ index ] );

	for( index = 0; index < range; index++ )
		if( !strncmp( key, keyWords[ index ], keyLength ) )
			{
			if( strlen( keyWords[ index ] ) == keyLength )
				return index;	/* exact match */
			position = index;
			noMatches++;
			}

	switch( noMatches )
		{
		case 0:
			fprintf( stderr, "%s: unknown keyword: \"%s\"\n",
					 errtag, optstr );
			break;
		case 1:
			return( position );	/* Match succeeded */
		default:
			fprintf( stderr, "%s: \"%s\" is ambiguous\n",
					 errtag, optstr );
		}
	return ERROR;
}

/* Extract a token from a buffer */

static int extractToken( char *buffer, int *endIndex, int *length )
{
	int index = 0, tokenStart;
	char ch;

	/* Skip whitespace */
	for( ch = buffer[ index ]; ch && ( ch == ' ' || ch == '\t' );
		 ch = buffer[ index ] )
		index++;
	tokenStart = index;

	/* Find end of setting */
	while( index < LINEBUF_SIZE && ( ch = buffer[ index ] ) != '\0'
		   && ch != ' ' && ch != '\t' )
		index++;
	*endIndex += index;
	*length = index - tokenStart;

	/* Return start position of token in buffer */
	return tokenStart;
}

/* Get a string constant */

static int getaString( char *buffer, int *endIndex )
	{
	boolean noQuote = FALSE;
	int stringIndex = 0, bufferIndex = 1;
	char ch = *buffer;

	/* Skip whitespace */
	while( ch && ( ch == ' ' || ch == '\t' ) )
		ch = buffer[ bufferIndex++ ];

	/* Check for non-string */
	if( ch != '\"' )
		{
		*endIndex += bufferIndex;

		/* Check for special case of null string */
		if( !ch )
			{
			*str = '\0';
			return OK;
			}

		/* Use nasty non-rigorous string format */
		noQuote = TRUE;
		}

	/* Get first char of string */
	if( !noQuote )
		ch = buffer[ bufferIndex++ ];

	/* Get string into string */
	while( ch && ch != '\"' )
		{
		/* Exit on '#' if using non-rigorous format */
		if( noQuote && ch == '#' )
			break;

		str[ stringIndex++ ] = ch;
		ch = buffer[ bufferIndex++ ];
		}

	/* If using the non-rigorous format, stomp trailing spaces */
	if( noQuote )
		while( stringIndex > 0 && str[ stringIndex - 1 ] == ' ' )
			stringIndex--;

	str[ stringIndex++ ] = '\0';
	*endIndex += bufferIndex;

	/* Check for missing string terminator */
	if( ch != '\"' && !noQuote )
		{
		if( line )
			fprintf( stderr, "%s: unterminated string in line %d\n",
					 errtag, line );
		else
			fprintf( stderr, "unterminated string: '\"%s'\n", str );
		hasError = TRUE;
		errCount++;
		return ERROR;
		}

	return OK;
}

/* Get an assignment to an intrinsic */

static int getAssignment( char *buffer, int *endIndex, INPUT_TYPE settingType )
{
	int settingIndex = 0, length;
	long longval;
	char *p;

	buffer += extractToken( buffer, endIndex, &length );

	/* Check for an assignment operator */
	if( *buffer != '=' )
		{
		if( line )
			fprintf( stderr, "%s: expected '=' in line %d\n",
					 errtag, line );
		else
			fprintf( stderr, "%s: expected '=' after \"%s\"\n",
					 errtag, optstr);
		hasError = TRUE;
		errCount++;
		return ERROR;
		}
	buffer++;	/* Skip '=' */

	buffer += extractToken( buffer, endIndex, &length );

	switch( settingType )
		{
		case BOOL:
			/* Check for known intrinsic - really more general
			   than just checking for TRUE or FALSE */
			settingIndex = lookup( buffer, length, settings,
			                       NO_SETTINGS );
			if( settingIndex == ERROR )
				{
				hasError = TRUE;
				errCount++;
				return ERROR;
				}

			value = ( settingIndex == 0 ) ? FALSE : TRUE;
			break;

		case STRING:
			/* Get a string */
			getaString( buffer, &length );
			break;

		case NUMERIC:
			longval = strtol(buffer, &p, 0);
			if (p == buffer+length &&
			    longval <= INT_MAX && longval >= INT_MIN) {
				value = (int)longval;
				break;
			}
			if( line )
				fprintf( stderr,
				  "%s: numeric argument expected in line %d\n",
						 errtag, line );
			else
				fprintf( stderr,
				   "%s: numeric argument required for \"%s\"\n",
						 errtag, optstr);
			hasError = TRUE;
			errCount++;
			return ERROR;
		}

	return settingIndex;
}

/* Process an assignment */

static void processAssignment( int intrinsicIndex )
	{
	if( !hasError )
		switch( intrinsicIndex )
			{
			case ARMOR:
				emit_radix_64 = value;
				break;

			case ARMORLINES:
				pem_lines = value;
				break;

			case AUTOSIGN:
				sign_new_userids = value;
				break;

			case BAKRING:
				strcpy( floppyring, str );
				break;

			case BATCHMODE:
				batchmode = value;
				break;

			case CERT_DEPTH:
				max_cert_depth = value;
				if( max_cert_depth < MIN_CERT_DEPTH )
					max_cert_depth = MIN_CERT_DEPTH;
				if( max_cert_depth > MAX_CERT_DEPTH )
					max_cert_depth = MAX_CERT_DEPTH;
				break;

			case CHARSET:
				strncpy( charset, str, 16 );
				break;

			case CLEARSIG:
				clear_signatures = value;
				break;

			case COMMENT:
				strcpy( globalCommentString, str );
				break;

			case COMPLETES_NEEDED:
				compl_min = value;
				/* Keep within range */
				if( compl_min < MIN_COMPLETE )
					compl_min = MIN_COMPLETE;
				if( compl_min > MAX_COMPLETE )
					compl_min = MAX_COMPLETE;
				break;

			case COMPRESS:
				compress_enabled = value;
				break;

			case FORCE:
				force_flag = value;
				break;

			case INTERACTIVE:
				interactive_add = value;
				break;

			case KEEPBINARY:
				keepctx = value;
				break;

			case LANGUAGE:
				strncpy( language, str, 15 );
				break;

			case LEGAL_KLUDGE:
				if (!value)
#ifdef USA
                                        fprintf(stdout,
LANG("The legal_kludge cannot be disabled in US version.\n"));
#else
					version_byte = VERSION_BYTE_OLD;
#endif
				break;

			case MAKERANDOM:
				makerandom = value;
				break;
#ifdef MACTC5				
			case FILE_TYPES:
				use_ftypes = value;
				break;
			
			case WIPE_WARNING:
				wipe_warning = value;
				break;
			
			case RECYCLE_PASSWD:
				recycle_passwd = value;
				break;

			case MULTIPLE_RECIPIENTS:
				fprintf(stdout, LANG("The multiple_recipients flag is unnecessary in this \
version of MacPGP.\
\nPlease remove this entry from your configuration file.\n"));
				break;
#endif

			case MARGINALS_NEEDED:
				marg_min = value;
				/* Keep within range */
				if( marg_min < MIN_MARGINALS )
					marg_min = MIN_MARGINALS;
				break;

			case MYNAME:
				strcpy( my_name, str );
#ifdef EBCDIC
    CONVERT_TO_CANONICAL_CHARSET(my_name);
#endif
				break;

			case NOMANUAL:
				nomanual = value;
				break;

			case PAGER:
				strcpy( pager, str );
				break;

			case PUBRING:
				strcpy( globalPubringName, str );
				break;

			case RANDSEED:
				strcpy( globalRandseedName, str );
				break;

			case SECRING:
				strcpy( globalSecringName, str );
				break;

			case SELF_ENCRYPT:
				encrypt_to_self = value;
				break;

			case SHOWPASS:
				showpass = value;
				break;

			case TEXTMODE:
				if( value )
					literal_mode = MODE_TEXT;
				else
					literal_mode = MODE_BINARY;
				break;

			case TMP:
				/* directory pathname to store temp files */
				settmpdir( str );
				break;

			case TZFIX:
				/* How many hours to add to time() to get GMT.
				   We just compute the seconds from hours to
				   get the GMT shift */
				timeshift = 3600L * ( long ) value;
				break;

			case VERBOSE:
				if( value < 1 )
					{
					quietmode = TRUE;
					verbose = FALSE;
					}
				else
					if( value == 1 )
						{
						quietmode = FALSE;
						verbose = FALSE;
						}
					else
						{
						/* Value > 1 */
						quietmode = FALSE;
						verbose = TRUE;
						}
				break;

			}
}

/* Process an option on a line by itself.  This expects options which are
   taken from the command-line, and is less finicky about errors than the
   config-file version */

int processConfigLine( char *option )
{
	int index, intrinsicIndex;
	char ch;

	/* Give it a pseudo-linenumber of 0 */
	line = 0;

	errtag = "pgp";
	errCount = 0;
	for( index = 0;
		 index < LINEBUF_SIZE && ( ch = option[ index ] ) != '\0' &&
				ch != ' ' && ch != '\t' && ch != '=';
		 index++ );
	if( ( intrinsicIndex = lookup( ( char * ) option, index, intrinsics,
				      NO_INTRINSICS ) ) == ERROR )
		return -1;
	if( option[ index ] == '\0' && intrinsicType[ intrinsicIndex ] == BOOL)
		{
		/* Boolean option, no '=' means TRUE */
		value = TRUE;
		processAssignment( intrinsicIndex );
		}
	else
		/* Get the value to set to, either as a string, a numeric
		   value, or a boolean flag */
		if( getAssignment( ( char * ) option + index,
			   &index, intrinsicType[ intrinsicIndex ] ) != ERROR )
			processAssignment( intrinsicIndex );

	return errCount ? -1 : 0;
}

/* Process a configuration file */

int processConfigFile( char *configFileName )
{
	FILE *configFilePtr;
	int ch = 0, theChar;
	int errType, errPos = 0, lineBufCount, intrinsicIndex;
	int index;
	char inBuffer[ LINEBUF_SIZE ];

	line = 1;
	errCount = 0;
	errtag = file_tail( configFileName );

	if( ( configFilePtr = fopen( configFileName, FOPRTXT ) ) == NULL )
		{
		fprintf( stderr, "Cannot open configuration file %s\n",
				 configFileName );
		return OK;	/* Treat it as if it were an empty file */
		}

	/* Process each line in the configFile */
	while( ch != EOF )
		{
		/* Skip whitespace */
		while( ( ( ch = getc( configFilePtr ) ) == ' ' || ch == '\t' )
		      && ch != EOF )
			;

		/* Get a line into the inBuffer */
		hasError = FALSE;
		lineBufCount = 0;
		errType = NO_ERROR;
		while( ch != '\r' && ch != '\n' && ch != CPM_EOF && ch != EOF )
			{
			/* Check for an illegal char in the data */
#ifdef EBCDIC
			if( iscntrl(ch) && !isspace(ch) && ch != EOF )
#else
			if( ( ch < ' ' || ch > '~' ) &&
				  ch != '\r' && ch != '\n' &&
				  ch != ' ' && ch != '\t' && ch != CPM_EOF &&
				  ch != EOF )
#endif
				{
				if( errType == NO_ERROR )
					/* Save pos of first illegal char */
					errPos = lineBufCount;
				errType = ILLEGAL_CHAR_ERROR;
				}

			/* Make sure the path is of the correct length.  Note
			   that the code is ordered so that a LINELENGTH_ERROR
			   takes precedence over an ILLEGAL_CHAR_ERROR */
			if( lineBufCount > LINEBUF_SIZE )
				errType = LINELENGTH_ERROR;
			else
				inBuffer[ lineBufCount++ ] = ch;

			if( ( ch = getc( configFilePtr ) ) == '#' )
				{
				/* Skip comment section and trailing
				   whitespace */
				while( ch != '\r' && ch != '\n' &&
					   ch != CPM_EOF && ch != EOF )
				  ch = getc( configFilePtr );
				break;
				}
			}

		/* Skip trailing whitespace and add der terminador */
		while( lineBufCount &&
		       ( ( theChar = inBuffer[ lineBufCount - 1 ] ) == ' ' ||
			   theChar == '\t' ) )
		  lineBufCount--;
		inBuffer[ lineBufCount ] = '\0';

		/* Process the line unless its a blank or comment line */
		if( lineBufCount && *inBuffer != '#' )
			{
			switch( errType )
				{
				case LINELENGTH_ERROR:
					fprintf( stderr,
					    "%s: line '%.30s...' too long\n",
							 errtag, inBuffer );
					errCount++;
					break;

				case ILLEGAL_CHAR_ERROR:
					fprintf( stderr, "> %s\n  ", inBuffer );
					fprintf( stderr, "%*s^\n", errPos, "" );
					fprintf( stderr,
				    "%s: bad character in command on line %d\n",
							 errtag, line );
					errCount++;
					break;

				default:
					for( index = 0;
					     index < LINEBUF_SIZE &&
					     ( ch = inBuffer[ index ] ) != '\0'
					     && ch != ' ' && ch != '\t'
					     && ch != '=';
					     index++ )
						/*Do nothing*/ ;

					/* Try and find the intrinsic.  We
					   don't treat unknown intrinsics as
					   an error to allow older versions to
					   be used with new config files */
					intrinsicIndex = lookup(inBuffer,
						index, intrinsics,
						CONFIG_INTRINSICS );
				
					if( intrinsicIndex == ERROR )
						break;

					/* Get the value to set to, either as
					   a string, a numeric value, or a
					   boolean flag */
					getAssignment( inBuffer + index, &index,
					     intrinsicType[ intrinsicIndex ] );
					processAssignment( intrinsicIndex );
					break;
				}
			}

		/* Handle special-case of ^Z if configFile came off an
		   MSDOS system */
		if( ch == CPM_EOF )
			ch = EOF;

		/* Exit if there are too many errors */
		if( errCount >= MAX_ERRORS )
			break;

		line++;
		}

	fclose( configFilePtr );

	/* Exit if there were errors */
	if( errCount )
		{
		fprintf( stderr, "%s: %s%d error(s) detected\n\n",
				 configFileName, ( errCount >= MAX_ERRORS ) ?
				 "Maximum level of " : "", errCount );
		return ERROR;
		}

	return OK;
}


/* ==== crypto.c ==== */
/*	crypto.c  - Cryptographic routines for PGP.
	PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.
	
	(c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
	The author assumes no liability for damages resulting from the use
	of this software, even if the damage results from defects in this
	software.  No warranty is expressed or implied.
	
	Note that while most PGP source modules bear Philip Zimmermann's
	copyright notice, many of them have been revised or entirely written
	by contributors who frequently failed to put their names in their
	code.  Code that has been incorporated into PGP from other authors
	was either originally published in the public domain or is used with
	permission from the various authors.
	
	PGP is available for free to the public under certain restrictions.
	See the PGP User's Guide (included in the release package) for
	important information about licensing, patent restrictions on
	certain algorithms, trademarks, copyrights, and export controls.
	
 	Modified: 12-Nov-92 HAJK
 	Add FDL stuff for VAX/VMS local mode. 
	Reopen temporary files rather than create new version.
	
	Modified: 13-Dec-92 Derek Atkins <warlord@MIT.EDU)
	Added Multiple Recipients
	
	Modified 25-Feb-93 Colin Plumb
	Improved security of randseed.bin in strong_pseudorandom.
	Thoroughly revamped make_random_ideakey.
	
	Modified  6-May-93 Colin Plumb
	Changed to use the entry points in rsaglue.c.
	*/

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mpilib.h"
#include "mpiio.h"
#include "random.h"
#include "crypto.h"
#include "keymgmt.h"
#include "keymaint.h"
#include "mdfile.h"
#include "fileio.h"
#include "charset.h"
#include "language.h"
#include "pgp.h"
#include "exitpgp.h"
#include "zipup.h"
#include "rsaglue.h"
#include "idea.h"
#ifdef MACTC5
#include "Macutil.h"
#include "Macutil2.h"
#include "Macutil3.h"
#include "Aestuff.h"
#include "MyBufferedStdio.h"
#include "ReplaceStdio.h"
#undef fopen
extern long GMTTimeshift(void);
extern unsigned char passhash[];
#endif

#define ENCRYPT_IT FALSE	/* to pass to idea_file */
#define DECRYPT_IT TRUE		/* to pass to idea_file */

#define	USE_LITERAL2


/* This variable stores the md5 hash of the current file, if it is
   available.  It is used in make_random_ideakey. */
static unsigned char md5buf[16];

/* This flag is set if the buffer above has been filled. */
static char already_have_md5 = 0;


/* Used by encryptfile */
static int encryptkeyintofile(FILE *g, char *mcguffin, byte *keybuf,
	char *keyfile, int ckp_length, int keys_used);

#ifdef  M_XENIX
long time();
#endif

/*--------------------------------------------------------------------------*/

#ifdef MACTC5
void CToPascal(char *s)
{
	/* "xyz\0" --> "\3xyz" ... converts C string to Pascal string */
	int i,j;
	j = string_length(s);
	for (i=j; i!=0; i--)
		s[i] = s[i-1];	/* move everything 1 byte to the right */
	s[0] = j;		/* Pascal length byte at beginning */
}	/* CToPascal */

void PascalToC( char *s )
{
	/* "\3xyz" --> "xyz\0" ... converts Pascal string to C string */
	int i,j;
	for (i=0,j=((byte *) s)[0]; i<j; i++)
		s[i] = s[i+1];	/* move everything 1 byte to the left */
	s[i] = '\0';		/* append C string terminator */
}	/* PascalToC */

#else
void CToPascal(char *s)
{
	/* "xyz\0" --> "\3xyz" ... converts C string to Pascal string */
	int i,j;
	j = string_length(s);
	for (i=j; i!=0; i--)
		s[i] = s[i-1];	/* move everything 1 byte to the right */
	s[0] = j;		/* Pascal length byte at beginning */
}	/* CToPascal */


void PascalToC( char *s )
{
	/* "\3xyz" --> "xyz\0" ... converts Pascal string to C string */
	int i,j;
	for (i=0,j=((byte *) s)[0]; i<j; i++)
		s[i] = s[i+1];	/* move everything 1 byte to the left */
	s[i] = '\0';		/* append C string terminator */
}	/* PascalToC */

#endif
/* 
	Note:  On MSDOS, the time() function calculates GMT as the local
	system time plus a built-in timezone correction, which defaults to
	adding 7 hours (PDT) in the summer, or 8 hours (PST) in the winter,
	assuming the center of the universe is on the US west coast. Really--
	I'm not making this up!  The only way to change this is by setting 
	the MSDOS environmental variable TZ to reflect your local time zone,
	for example "set TZ=MST7MDT".  This means add 7 hours during standard
	time season, or 6 hours during daylight time season, and use MST and 
	MDT for the two names of the time zone.  If you live in a place like 
	Arizona with no daylight savings time, use "set TZ=MST7".  See the
	Microsoft C function tzset().  Just in case your local software
	environment is too weird to predict how to set environmental
	variables for this, PGP also uses its own TZFIX variable in
	config.pgp to optionally correct this problem further.  For example,
	set TZFIX=-1 in config.pgp if you live in Colorado and the TZ
	variable is undefined.
*/
word32 get_timestamp(byte *timestamp)
/*	Return current timestamp as a byte array in internal byteorder,
	and as a 32-bit word */
{
	word32 t;
	t = 0xdeadbeef/* time(NULL) */;    /* returns seconds since GMT 00:00 1 Jan 1970 */

#ifdef _MSC_VER
#if (_MSC_VER == 700)
	/*  Under MSDOS and MSC 7.0, time() returns elapsed time since
	 *  GMT 00:00 31 Dec 1899, instead of Unix's base date of 1 Jan 1970.
	 *  So we must subtract 70 years worth of seconds to fix this.
	 *  6/19/92  rgb 
	*/
#define	LEAP_DAYS	(((unsigned long)70L/4)+1)
#define CALENDAR_KLUDGE ((unsigned long)86400L * (((unsigned long)365L \
						   * 70L) + LEAP_DAYS))
   	t -= CALENDAR_KLUDGE;
#endif
#endif
#ifdef MACTC5
	t -= GMTTimeshift();
#endif

	t += timeshift; /* timeshift derived from TZFIX in config.pgp */

	if (timestamp != NULL) {
		/* first, fill array in external byte order: */
		put_word32(t, timestamp);
		convert_byteorder(timestamp,4);
				/* convert to internal byteorder */
	}

	return t;	/* return 32-bit timestamp integer */
}	/* get_timestamp */


/*	Given timestamp as seconds elapsed since 1970 Jan 1 00:00:00,
	returns year (1970-2106), month (1-12), day (1-31).
	Not valid for dates after 2100 Feb 28 (no leap day that year).
	Also returns day of week (0-6) as functional return.
*/
static int date_ymd(word32 *tstamp, int *year, int *month, int *day)
{
	word32 days,y;
	int m,d,i;
	static short mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
	days = (*tstamp)/(unsigned long)86400L;	/* day 0 is 1970/1/1 */
	days -= 730L;	/* align days relative to 1st leap year, 1972 */
	y = ((days*4)/(unsigned long)1461L);	/* 1972 is year 0 */
	/* reduce to days elapsed since 1/1 last leap year: */
	d = (int) (days - ((y/4)*1461L));
	*year = (int)(y+1972);
	for (i=0; i<48; i++) {	/* count months 0-47 */
		m = i % 12;
		d -= mdays[m] + (i==1);	/* i==1 is the only leap month */
		if (d < 0) {
			d += mdays[m] + (i==1);
			break;
		}
	}
	*month = m+1;
	*day = d+1;
	i = (int)((days-2) % (unsigned long)7L); /* compute day of week 0-6 */
	return i;	/* returns weekday 0-6; 0=Sunday, 6=Saturday */
}	/* date_ymd */


/*	Return date string, given pointer to 32-bit timestamp */
char *cdate(word32 *tstamp)
{
	int month,day,year;
	static char datebuf[20];
	if (*tstamp == 0)
		return "          ";
	(void) date_ymd(tstamp,&year,&month,&day);
	sprintf(datebuf,"%4d/%02d/%02d", year, month, day);
	return (datebuf);
}	/* cdate */


/*	Return date and time string, given pointer to 32-bit timestamp */
char *ctdate(word32 *tstamp)
{
	int hours,minutes;
	static char tdatebuf[40];
	long seconds;
	seconds = (*tstamp) % (unsigned long)86400L;
				/* seconds past midnight today */
	minutes = (int)((seconds+30L) / 60L);
				/* round off to minutes past midnight */
	hours = minutes / 60;	/* hours past midnight */
	minutes = minutes % 60;	/* minutes past the hour */
	sprintf(tdatebuf,"%s %02d:%02d GMT", cdate(tstamp), hours, minutes);
	return (tdatebuf);
}	/* ctdate */



/* Warn user he if key in keyfile at position fp of length pktlen, belonging
 * to userid, is untrusted.  Return -1 if the user doesn't want to proceed.
 */
static int warn_signatures(char *keyfile, long fp,
			   char *userid, boolean warn_only)
{
     FILE		*f;
     long		fpusr;
     int			usrpktlen;
     byte		keyctrl;
     int			trust_status = -1;

     keyctrl = KC_LEGIT_UNKNOWN; /* Assume the worst */
     if (getpubuserid (keyfile, fp, (byte *) userid, &fpusr,
		       &usrpktlen, FALSE) >= 0)
       {
	    f = fopen(keyfile, FOPRBIN);
	    fseek (f, fpusr+usrpktlen, SEEK_SET);
	    /* Read trust byte */
	    trust_status = read_trust(f, &keyctrl);
	    fseek(f, fp, SEEK_SET);
	    if (is_compromised(f)) {
		 CToPascal(userid);
		 fprintf(pgpout, "\n");
		 show_key(f, fp, 0);
		 fclose (f);
		 fprintf(pgpout,
   LANG("\007\nWARNING:  This key has been revoked by its owner,\n\
possibly because the secret key was compromised.\n"));
		 if (warn_only) {
		      /* this is only for checking signatures */
		      fprintf(pgpout,
   LANG("This could mean that this signature is a forgery.\n"));
		      return 1;
		 } else {	/* don't use it for encryption */
		      fprintf(pgpout,
   LANG("You cannot use this revoked key.\n"));
		      return -1;
		 }
	    }
	    fclose (f);
       }
     CToPascal(userid);
     if ((keyctrl & KC_LEGIT_MASK) != KC_LEGIT_COMPLETE) {
	  byte userid0[256];
	  PascalToC(userid);
	  strcpy ((char *) userid0, userid);
	  CToPascal(userid);

	  if ((keyctrl & KC_LEGIT_MASK) == KC_LEGIT_UNKNOWN)
	    fprintf(pgpout,
   LANG("\007\nWARNING:  Because this public key is not certified with \
a trusted\nsignature, it is not known with high confidence that this \
public key\nactually belongs to: \"%s\".\n"),
		    LOCAL_CHARSET((char *)userid0));

	  if ((keyctrl & KC_LEGIT_MASK) == KC_LEGIT_UNTRUSTED)
	    fprintf(pgpout,
   LANG("\007\nWARNING:  This public key is not trusted to actually belong \
to:\n\"%s\".\n"), LOCAL_CHARSET((char *)userid0));

		if ((keyctrl & KC_LEGIT_MASK) == KC_LEGIT_MARGINAL)
			fprintf(pgpout,
   LANG("\007\nWARNING:  Because this public key is not certified with enough \
trusted\nsignatures, it is not known with high confidence that this \
public key\nactually belongs to: \"%s\".\n"),
				LOCAL_CHARSET((char *)userid0));

		if (keyctrl & KC_WARNONLY) {
				/* KC_WARNONLY bit already set,
				   user must have approved before. */
			fprintf(pgpout,
   LANG("But you previously approved using this public key anyway.\n"));
		}

		if (!filter_mode && !batchmode && !warn_only
		    && !(keyctrl & KC_WARNONLY))
		{
			fprintf(pgpout,
   LANG("\nAre you sure you want to use this public key (y/N)? "));
			if (!getyesno('n'))
				return -1;
			if (trust_status == 0
			    && (f = fopen(keyfile, FOPRWBIN)) != NULL)
			{
				fseek (f, fpusr+usrpktlen, SEEK_SET);
				keyctrl |= KC_WARNONLY;
				write_trust(f, keyctrl);
				fclose(f);
			}
		}
	}
	return 0;
}	/* warn_signatures */


/* Used to determine if nesting should be allowed. */
boolean legal_ctb(byte ctb)
{
	boolean legal;
	byte ctbtype;
	if (!is_ctb(ctb))		/* not even a bonafide CTB */
		return FALSE;
	/* Sure hope CTB internal bit definitions don't change... */
	ctbtype = (ctb & CTB_TYPE_MASK) >> 2;
	/* Only allow these CTB types to be nested... */
	legal = ( (ctbtype==CTB_PKE_TYPE)
		|| (ctbtype==CTB_SKE_TYPE)
		|| (ctbtype==CTB_CERT_SECKEY_TYPE)
		|| (ctbtype==CTB_CERT_PUBKEY_TYPE)
		|| (ctbtype==CTB_LITERAL_TYPE)
		|| (ctbtype==CTB_LITERAL2_TYPE)
		|| (ctbtype==CTB_COMPRESSED_TYPE)
		|| (ctbtype==CTB_CKE_TYPE)
		 );
	return legal;
}	/* legal_ctb */


/* Return nonzero if val doesn't match checkval, after printing a
 * warning.
 */
int
version_error(int val, int checkval)
{
	if (val != checkval) {
		fprintf (pgpout,
LANG("\n\007Unsupported packet format - you need a newer version of PGP \
for this file.\n"));
		return 1;
	}
	return 0;
}

int
version_byte_error(int val)
{
	if (val != VERSION_BYTE_OLD && val != VERSION_BYTE_NEW) {
		fprintf (pgpout,
LANG("\n\007Unsupported packet format - you need a newer version of PGP \
for this file.\n"));
		return 1;
	}
	return 0;
}
		

/*-------------------------------------------------------------------------*/

#define RAND_PREFIX_LENGTH 8	/* Length of IV for IDEA encryption */

/*
 * Make a random IDEA key.  Returns its length (the constant 16).
 * It also generates a random IV, which is placed in the key array
 * after the key proper, but is not counted in the length.
 * Reads IDEA random key and random number seed from file, cranks the
 * seed through the IDEA strong pseudorandom number generator,
 * and writes them back out.  This is used for generation of
 * cryptographically strong pseudorandom numbers.  This is mainly to
 * save the user the trouble of having to type in lengthy keyboard
 * sequences for generation of truly random numbers every time we want
 * to make a random session key.  This pseudorandom generator will only
 * work if the file containing the random seed exists and is not empty.
 * If this is not the case, it will be automatically created.
 *
 * The MD5 of the current file is used to "prewash" the random numbers,
 * to make it more difficult for an attacker to predict the output.
 *
 * The "skip" parameter says to skip that many bytes at the beginning,
 * used to generate a random IV only for conventional encryption.
 */
static int make_random_ideakey(byte key[IDEAKEYSIZE+RAND_PREFIX_LENGTH],
			       int skip)
{
	int count;
	struct IdeaCfbContext cfb;
	byte buf[10];

	ideaCfbInit(&cfb, md5buf);
	burn(md5buf);

	if (cryptRandOpen(&cfb) < 0) {
		fprintf(pgpout,LANG("Preparing random session key..."));

		 /* get some random key bits */
		trueRandAccum((IDEAKEYSIZE+RAND_PREFIX_LENGTH)*8);

		cryptRandInit(&cfb);
	}

	/*
	 * Generate a good random IDEA key and initial vector.  If we have
	 * no random bytes, the trueRandByte() part will be useless
	 */
	count = IDEAKEYSIZE+RAND_PREFIX_LENGTH;
	for (count = skip; count < IDEAKEYSIZE+RAND_PREFIX_LENGTH; count++)
		key[count] = cryptRandByte() ^ trueRandByte();

	/*
	 * Write out a new randseed.bin.  It is encrypted in precisely the
	 * same manner as the message itself, although the leading
	 * IV and check bytes are discarded.  This "postwash" is to
	 * ensure that it's easier to decrypt the message directly than to
	 * try to figure out what the key was by examining the entrails
	 * of the random number generator state in randseed.bin.
	 */
	ideaCfbInit(&cfb, key);
	memcpy(buf, key, 8);
	buf[8] = buf[6];
	buf[9] = buf[7];
	ideaCfbEncrypt(&cfb, buf, buf, 10);
	ideaCfbSync(&cfb);

	/* Save out the washed session key */
	cryptRandSave(&cfb);

	ideaCfbDestroy(&cfb);

	return IDEAKEYSIZE;
}


/*	Returns the length of a packet according to the CTB and
	the length field. */
word32 getpastlength(byte ctb, FILE *f)
{
	word32 length;
	unsigned int llength;	/* length of length */
	byte buf[8];

	fill0(buf,sizeof(buf));
	length = 0L;
	/* Use ctb length-of-length field... */
	llength = ctb_llength(ctb);	/* either 1, 2, 4, or 8 */
	if (llength==8) /* 8 means no length field, assume huge length */
		return -1L;	/* return huge length */

	/* now read in the actual length field... */
	if (fread((byteptr) buf,1,llength,f) < llength)
		return (-2L); /* error -- read failure or premature eof */
	/* convert length from external byteorder... */
	if (llength==1)
		length = (word32) buf[0];
	if (llength==2)
		length = (word32) fetch_word16(buf);
	if (llength==4)
		length = fetch_word32(buf);
	return length;
} /* getpastlength */


/* Write a CTB with the appropriate length field.  If big is true,
 * always use a four-byte length field.
 */
void write_ctb_len (FILE *f, byte ctb_type, word32 length, boolean big)
{
	int	llength, llenb;
	byte	ctb;
	byte	buf[4];

	if (big || (length > 0xFFFFL)) {
		llength = 4;
		llenb = 2;
	} else if ((word16)length > 0xFF) {
		llength = 2;
		llenb = 1;
	} else {
		llength = 1;
		llenb = 0;
	}
	
	putc(CTB_BYTE(ctb_type, llenb), f);
	/* convert length to external byteorder... */
	if (llength==1)
		buf[0] = length;
	if (llength==2)
		put_word16((word16) length, buf);
	if (llength==4)
		put_word32(length, buf);
	fwrite( buf, 1, llength, f );
} /* write_ctb_len */

/*
 * Use IDEA in cipher feedback (CFB) mode to encrypt or decrypt a file. 
 * The encrypted material starts out with a 64-bit random prefix, which
 * serves as an encrypted random CFB initialization vector, and
 * following that is 16 bits of "key check" material, which is a
 * duplicate of the last 2 bytes of the random prefix.  Encrypted key
 * check bytes detect if correct IDEA key was used to decrypt ciphertext.
 */
static
int idea_file(byte *ideakey, boolean decryp, FILE *f, FILE *g, word32 lenfile)
{
	int count, status = 0;
	extern byte textbuf[DISKBUFSIZE];
	struct IdeaCfbContext cfb;
#define RAND_PREFIX_LENGTH 8

	/* init CFB key */
	ideaCfbInit(&cfb, ideakey);

	if (!decryp) {	/* encrypt-- insert key check bytes */
		/* There is a random prefix followed by 2 key check bytes */

		memcpy(textbuf, ideakey+IDEAKEYSIZE, RAND_PREFIX_LENGTH);
    /* key check bytes are simply duplicates of final 2 random bytes */
		textbuf[RAND_PREFIX_LENGTH] = textbuf[RAND_PREFIX_LENGTH-2];
		textbuf[RAND_PREFIX_LENGTH+1] = textbuf[RAND_PREFIX_LENGTH-1];

		ideaCfbEncrypt(&cfb, textbuf, textbuf, RAND_PREFIX_LENGTH+2);
		fwrite(textbuf,1,RAND_PREFIX_LENGTH+2,g);
	} else { /* decrypt-- check for key check bytes */
		/* See if the redundancy is present after the random prefix */
		count = fread(textbuf,1,RAND_PREFIX_LENGTH+2,f);
		lenfile -= count;
		if (count==(RAND_PREFIX_LENGTH+2)) {
			ideaCfbDecrypt(&cfb, textbuf, textbuf,
				       RAND_PREFIX_LENGTH+2);
			if ((textbuf[RAND_PREFIX_LENGTH] !=
			     textbuf[RAND_PREFIX_LENGTH-2])
				|| (textbuf[RAND_PREFIX_LENGTH+1] !=
				    textbuf[RAND_PREFIX_LENGTH-1]))
			{
				status = -2;		/* bad key error */
			}
		} else	/* file too short for key check bytes */
			status = -3;		/* error of the weird kind */
	}

	ideaCfbSync(&cfb);

	/* read and write the whole file in CFB mode... */
	count = (lenfile < DISKBUFSIZE) ? (int)lenfile : DISKBUFSIZE;
	while (count && status == 0) {
		if ((count = fread(textbuf,1,count,f)) <= 0) {
			status = -3;
			break;
		}
		lenfile -= count;
		if (decryp)
			ideaCfbDecrypt(&cfb, textbuf, textbuf, count);
		else
			ideaCfbEncrypt(&cfb, textbuf, textbuf, count);
		if (fwrite(textbuf,1,count,g) != count)
			status = -3;
		count = (lenfile < DISKBUFSIZE) ? (int)lenfile : DISKBUFSIZE;
#ifdef MACTC5
		mac_poll_for_break();
#endif
	}

	ideaCfbDestroy(&cfb);	/* Clean up data structures */
	burn(textbuf);	/* burn sensitive data on stack */
	return status;	/* should always take normal return */
}	/* idea_file */


/* Checksum maintained as a running sum by read_mpi and write_mpi.
 * The checksum is maintained based on the plaintext values being
 * read and written.  To use it, store a 0 to it before doing a set
 * of read_mpi's or write_mpi's.  Then read it aftwerwards.
 */
word16	mpi_checksum;

/*
 * Read a mutiprecision integer from a file.
 * adjust_precision is TRUE iff we should call set_precision to the 
 * size of the number read in.
 * scrambled is TRUE iff field is encrypted (protects secret key fields).
 * Returns the bitcount of the number read in, or returns a negative
 * number if an error is detected.
 */
int read_mpi(unitptr r, FILE *f, boolean adjust_precision,
             struct IdeaCfbContext *cfb)
{
	byte buf[MAX_BYTE_PRECISION+2];
	unsigned int count;
	word16 bytecount,bitcount;

	mp_init(r,0);

	if ((count = fread(buf,1,2,f)) < 2)
		return (-1); /* error -- read failure or premature eof */

	bitcount = fetch_word16(buf);
	if (bits2units(bitcount) > global_precision)
		return -1;	/* error -- possible corrupted bitcount */

	bytecount = bits2bytes(bitcount);

	count = fread(buf+2,1,bytecount,f);
	if (count < bytecount)
		return -1;	/* error -- premature eof */

	if (cfb) {	/* decrypt the field */
		ideaCfbSync(cfb);
		ideaCfbDecrypt(cfb, buf+2, buf+2, bytecount);
	}

	/* Update running checksum, in case anyone cares... */
	mpi_checksum += checksum (buf, bytecount+2);

	/*	We assume that the bitcount prefix we read is an exact
		bitcount, not rounded up to the next byte boundary.
		Otherwise we would have to call mpi2reg, then call
		countbits, then call set_precision, then recall mpi2reg
		again.
	*/
	if (adjust_precision && bytecount) {
		/* set the precision to that specified by the number read. */
		if (bitcount > MAX_BIT_PRECISION-SLOP_BITS)
			return -1;
		set_precision(bits2units(bitcount+SLOP_BITS));
		/* Now that precision is optimally set, call mpi2reg */
	}

	if (mpi2reg(r,buf) == -1)	/* convert to internal format */
		return -1;
	burn(buf);	/* burn sensitive data on stack */
	return (bitcount);
}	/* read_mpi */



/*
 * Write a multiprecision integer to a file.
 * scrambled is TRUE iff we should scramble field on the way out,
 * which is used to protect secret key fields.
 */
void write_mpi(unitptr n, FILE *f, struct IdeaCfbContext *cfb)
{
	byte buf[MAX_BYTE_PRECISION+2];
	short bytecount;
	bytecount = reg2mpi(buf,n);
	mpi_checksum += checksum (buf, bytecount+2);
	if (cfb) { /* encrypt the field, skipping over the bitcount */
		ideaCfbSync(cfb);
		ideaCfbEncrypt(cfb, buf+2, buf+2, bytecount);
	}
	fwrite(buf,1,bytecount+2,f); 
	burn(buf);	/* burn sensitive data on stack */
}	/* write_mpi */

/*======================================================================*/

/*	Reads the first count bytes from infile into header. */
int get_header_info_from_file(char *infile,  byte *header, int count)
{
	FILE *f;
	fill0(header,count);
	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL)
		return -1;
	/* read Cipher Type Byte, and maybe more */
	count = fread(header,1,count,f);
	fclose(f);
	return count;	/* normal return */
}	/* get_header_info_from_file */


/* System clock must be broken if it isn't past this date: */
#define REASONABLE_DATE ((unsigned long) 0x27804180L)  /* 91 Jan 01 00:00:00 */


/*	Constructs a signed message digest in a signature certificate.
	Returns total certificate length in bytes, or returns negative
	error status.
*/
static
int make_signature_certificate(byte *certificate, struct MD5Context *MD,
   byte class, unitptr e, unitptr d, unitptr p, unitptr q, unitptr u,
			       unitptr n)
{
	byte inbuf[MAX_BYTE_PRECISION], outbuf[MAX_BYTE_PRECISION+2];
	int i, j, certificate_length, blocksize,bytecount;
	word16 ske_length;
	word32 tstamp; byte *timestamp = (byte *) &tstamp;
	byte keyID[KEYFRAGSIZE];
	byte val;
	int mdlen = 5;	/* length of class plus timestamp, for adding to MD */

	/*	Note that RSA key must be at least big enough to encipher a
		complete message digest packet in a single RSA block. */

		blocksize = countbytes(n)-1;	/* size of a plaintext block */
		if (blocksize < 31) {
			fprintf(pgpout,
   "\n\007Error: RSA key length must be at least 256 bits.\n");
			return -1;
		}

		get_timestamp(timestamp); /* Timestamp when signature was
					     made */
		if (tstamp < REASONABLE_DATE) {
			/* complain about bad time/date setting */
			fprintf(pgpout,
   LANG("\n\007Error: System clock/calendar is set wrong.\n"));
			return -1;
		}
		convert_byteorder(timestamp,4); /* convert to external form */

	/* Finish off message digest calculation with this information */
	MD_addbuffer (MD, &class, 1, 0);
	MD_addbuffer (MD, timestamp, 4, md5buf);
/* We wrote the digest to a static variable because we want to keep it around
   for random number generation later.   Also make a note of that fact. */
	already_have_md5 = 1;

	if (!quietmode) {
		fprintf(pgpout,LANG("Just a moment...")); /* RSA will take
							     a while. */
		fflush(pgpout);
	}

	/* do RSA signature calculation: */
	i = rsa_private_encrypt((unitptr)outbuf, md5buf, sizeof(md5buf),
	                        e, d, p, q, u, n);
	if (i < 0) {
		if (i == -4) {
			fprintf(pgpout,
   "\n\007Error: RSA key length must be at least 256 bits.\n");
		} else if (i == -3) {
			fputs(
"\a\nError: key is too large.  RSA keys may be no longer than 1024 bits\
,\ndue to limitations imposed by software provided by RSADSI.\n", pgpout);
		} else {
			fprintf(pgpout,"\a\nUnexpected error %d signing\n", i);
		}
		return i;
	}

	/* bytecount does not include the 2 prefix bytes */
	bytecount = reg2mpi(outbuf,(unitptr)outbuf); /* convert to external
							format */
	/*	outbuf now contains a message digest in external byteorder 
		form.  Now make a complete signature certificate from this.
		(Note that the first two bytes of md5buf are used below as
		part of the certificate.)
	*/

	certificate_length = 0;

   /* SKE is Secret Key Encryption (signed).  Append CTB for signed msg. */
	certificate[certificate_length++] = CTB_SKE;

   /* SKE packet length does not include itself or CTB prefix: */
	ske_length = 1 + 1 	  /* version and mdlen byte */
	  + mdlen		  /* class, timestamp and validation period */ 
	    + KEYFRAGSIZE + 1 + 1 /* Key ID and 2 algorithm bytes */
	      + 2 + bytecount+2;  /* 2 MD bytes and RSA MPI w/bitcount */
	put_word16((word16) ske_length, certificate+certificate_length);
	certificate_length+=2;	/* advance past word */

	certificate[certificate_length++] = version_byte;

	/* Begin fields that are included in MD calculation... */

	certificate[certificate_length++] =  mdlen; /* mdlen is length
						       of MD-extras */

	certificate[certificate_length++] =  class & 0xff;

	/* timestamp already in external format */
	for (j=0; j<SIZEOF_TIMESTAMP; j++)
		certificate[certificate_length++] =  timestamp[j];
 
	/* ...end of fields that are included in MD calculation */

	/* Now append keyID... */
	extract_keyID(keyID, n);	/* gets keyID */
	for (i=0; i<KEYFRAGSIZE; i++)
		certificate[certificate_length++] = keyID[i];

	certificate[certificate_length++] = RSA_ALGORITHM_BYTE;
	certificate[certificate_length++] = MD5_ALGORITHM_BYTE;

	/* Now append first two bytes of message digest */
	certificate[certificate_length++] = md5buf[0];
	certificate[certificate_length++] = md5buf[1];;

	/* Now append the RSA-signed message digest packet: */
	for (i=0; i<bytecount+2; i++)
		certificate[certificate_length++] = outbuf[i];

	if (!quietmode)
		fputc('.',pgpout);	/* Signal RSA signature completion. */

	burn(inbuf);	/* burn sensitive data on stack */
	burn(outbuf);	/* burn sensitive data on stack */



	return certificate_length; /* return length of certificate in bytes */

}	/* make_signature_certificate */


#ifdef VMS
/*
 * Local mode VMS, we write out the word VMS to say who owns the data then
 * we follow that with the file's FDL generated earlier by fdl_generate().
 * This FDL is preceded by a sixteen bit size. The file follows.
 */
void write_litlocal(FILE *g, char *fdl, short fdl_len)
{
    fputc('\0', g); /* Kludge for null literal file name (supplied by FDL) */
    fputs("VMS ", g);
    fwrite(&fdl_len, 2, 1, g); /* Byte order *not* important,
				  only VMS reads this!*/
    fwrite(fdl, 1, fdl_len, g);
}
#endif /* VMS */

/*======================================================================*/


/*	Write an RSA-signed message digest of input file to specified
	output file, and append input file to output file.
	separate_signature is TRUE iff we should not append the 
	plaintext to the output signature certificate.
	If lit_mode is MODE_TEXT, we know the infile is in canonical form.
	We create a CTB_LITERAL packet for the plaintext data.
*/
int signfile(boolean nested, boolean separate_signature,
		char *mcguffin, char *infile, char *outfile,
		char lit_mode, char *literalfile)
{
	FILE *f;
	FILE *g;
	int certificate_length;	/* signature certificate length */
	byte certificate[MAX_SIGCERT_LENGTH];
	char lfile[MAX_PATH];
	byte signature_class;
#ifdef VMS
	char *fdl;
	short fdl_len;
#endif /* VMS */


	{	/* temporary scope for some buffers */
		word32 tstamp; byte *timestamp = (byte *) &tstamp;
                  /* key certificate timestamp */
		byte userid[256];
		char keyfile[MAX_PATH];
		int status;
		struct MD5Context MD;
		byte keyID[KEYFRAGSIZE];
		unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
		unit d[MAX_UNIT_PRECISION];
		unit p[MAX_UNIT_PRECISION], q[MAX_UNIT_PRECISION];
		unit u[MAX_UNIT_PRECISION];

		set_precision(MAX_UNIT_PRECISION);/* safest opening
						     assumption */

		if (verbose)
			fprintf(pgpout,
 "signfile: infile = '%s', outfile = '%s', mode = '%c', literalfile = '%s'\n",
			infile,outfile,EXT_C(lit_mode),literalfile);

		if (MDfile(&MD, infile) < 0)
			return -1; /* problem with input file.  error return */

		userid[0] = '\0';
		if (mcguffin)
			strcpy((char *) userid,mcguffin); /* Who we are
							     looking for */

		if (getsecretkey(0, NULL, NULL, timestamp, NULL, NULL,
						 userid, n, e, d, p, q, u) < 0)
			return -1; /* problem with secret key file.
				      error return. */
		extract_keyID(keyID, n);
		strcpy(keyfile, globalPubringName); /* use default pathname */
		if ((status = getpublickey(GPK_NORVK, keyfile,
					   NULL, NULL, keyID,
				timestamp, userid, n, e)) < 0)
			return -1;	/* problem with public key file.
					   error return. */

		if (lit_mode==MODE_TEXT) signature_class = SM_SIGNATURE_BYTE;
		else signature_class = SB_SIGNATURE_BYTE;

		certificate_length = make_signature_certificate(certificate,
								&MD,
			signature_class, e, d, p, q, u, n);
		if (certificate_length < 0)
			return -1;	/* error return from
					   make_signature_certificate() */
	}	/* end of scope for some buffers */

	/* open file f for read, in binary (not text) mode...*/
#ifdef VMS
	if (lit_mode == MODE_LOCAL) {
	    if (!(fdl_generate(infile, &fdl, &fdl_len ) & 01)) {
		fprintf(pgpout,
   LANG("\n\007Can't open input plaintext file '%s'\n"),infile);
		return -1;
	    }
	}
#endif /* VMS */
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't open plaintext file '%s'\n"),infile);
		return -1;
	}

	/* open file g for write, in binary (not text) mode...*/
	if ((g = fopen(outfile,FOPWBIN)) == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't create signature file '%s'\n"),outfile);
		fclose(f);
		return -1;
	}

	/* write out certificate record to outfile ... */
	fwrite(certificate,1,certificate_length,g);

	if (literalfile == NULL) {
		/* Put in a zero byte to indicate no filename */
		lfile[0] = '\0';
	} else {
		strcpy( lfile, literalfile );
		file_to_canon( lfile );
		CToPascal( lfile );
	}

	if (!separate_signature) {
		if (!nested) {
			word32 flen = fsize(f);
			word32 dummystamp = 0;
			if (lit_mode == MODE_LOCAL)
#ifdef VMS
				write_ctb_len(g, CTB_LITERAL2_TYPE,
				    flen + fdl_len + sizeof(fdl_len) + 6,
					      TRUE);
#else
				/* debug check: should never get here */
			    fprintf(pgpout, "signfile: invalid mode\n");
#endif
			else {
#ifdef USE_LITERAL2
			    write_ctb_len (g, CTB_LITERAL2_TYPE,
					   flen + (unsigned char) lfile[0]
					   + 6, FALSE);
#else
			    write_ctb_len (g, CTB_LITERAL_TYPE, flen, FALSE);
#endif /* USE_LITERAL2 */
			}
			putc(lit_mode, g); 	/*	write lit_mode */
			if (lit_mode == MODE_LOCAL) {
#ifdef VMS
			    write_litlocal( g, fdl, fdl_len);
			    free(fdl);
#endif /* VMS */
			} else {
			    /* write literalfile name */
				fwrite (lfile, 1, (unsigned char) lfile[0]+1,
					g);
			    /* Dummy file creation timestamp */
			    fwrite ( &dummystamp, 1, sizeof(dummystamp), g);
			}
		}
		copyfile(f,g,-1L); /* copy rest of file from file f to g */
	}

	fclose(f);
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose(g);
	return 0;	/* normal return */

}	/* signfile */

/*======================================================================*/

int compromise(byte *keyID, char *keyfile)
{
	FILE *f, *g;
	byte ctb;	/* Cipher Type Byte */
	int certificate_length;	/* signature certificate length */
	byte certificate[MAX_SIGCERT_LENGTH];
	word32 tstamp; byte *timestamp = (byte *) &tstamp;
	byte userid[256];
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	struct MD5Context MD;
	unit d[MAX_UNIT_PRECISION];
	unit p[MAX_UNIT_PRECISION], q[MAX_UNIT_PRECISION];
	unit u[MAX_UNIT_PRECISION];
	long fp, insertpos;
	int pktlen;
	int prec;
	char *scratchf;

	setoutdir(keyfile);
	scratchf = tempfile(0);

	if (getsecretkey(0, NULL, keyID, timestamp, NULL, NULL,
					 userid, n, e, d, p, q, u) < 0)
		return -1;	/* problem with secret key file.
				   error return. */

	if (getpublickey(0, keyfile, &fp, &pktlen, keyID,
			timestamp, userid, n, e) < 0)
		return -1;

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(keyfile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't open key ring file '%s'\n"),keyfile);
		return -1;
	}

	fseek (f, fp+pktlen, SEEK_SET);
	nextkeypacket(f, &ctb);
	if (ctb == CTB_KEYCTRL) {
		insertpos = ftell(f);
		nextkeypacket(f, &ctb);
	} else {
		insertpos = fp + pktlen;
	}

	if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
		fprintf(pgpout, LANG("This key has already been revoked.\n"));
		fclose(f);
		return -1;
	}

	prec = global_precision;
	set_precision(MAX_UNIT_PRECISION);	/* safest opening assumption */

	fseek(f, fp, SEEK_SET);
	/* Calculate signature */
	if (MDfile0_len(&MD, f, pktlen) < 0) {
		fclose(f);
		return -1;	/* problem with input file.  error return */
	}
	set_precision(prec);

	certificate_length = make_signature_certificate(certificate, &MD,
		KC_SIGNATURE_BYTE, e, d, p, q, u, n);
	if (certificate_length < 0) {
		fclose(f);
		return -1;	/* error return from
				   make_signature_certificate() */
	}


	/* open file g for write, in binary (not text) mode...*/
	if ((g = fopen(scratchf,FOPWBIN)) == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't create output file to update key ring.\n"));
		fclose(f);
		return -1;
	}

	/* Copy pre-key and key to file g */
	rewind(f);
	copyfile (f, g, insertpos);

	/* write out certificate record to outfile ... */
	fwrite(certificate,1,certificate_length,g);

	/* Copy the remainder from file f to file g */
	copyfile (f, g, -1L);

	fclose(f);
	
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose(g);

	savetempbak(scratchf,keyfile);

	fprintf(pgpout, LANG("\nKey compromise certificate created.\n"));
	return 0;	/* normal return */
}	/* compromise */

/*======================================================================*/

int do_sign(char *keyfile, long fp, int pktlen, byte *userid, byte *keyID,
            char *sigguffin, boolean batchmode)
{
	FILE *f;
	FILE *g;
	byte ctb;	/* Cipher Type Byte */
	word32 tstamp; byte *timestamp = (byte *) &tstamp;
	byte keyID2[KEYFRAGSIZE];
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	int certificate_length;	/* signature certificate length */
	byte certificate[MAX_SIGCERT_LENGTH];
	long fpusr;
	int usrpktlen, usrctrllen;
	char *tempring;
	int status;

	status = getpubuserid(keyfile, fp, userid, &fpusr,
	                      &usrpktlen, FALSE);
	if (status < 0)
		return -1;

	/* open file f for read, in binary (not text) mode...*/
	f = fopen(keyfile,FOPRBIN);
	if (f == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't open key ring file '%s'\n"),keyfile);
		return -1;
	}

	/* See if there is another signature with this keyID already */
	fseek (f, fpusr+usrpktlen, SEEK_SET);
	nextkeypacket(f, &ctb);		/* Add key control packet to len */
	usrctrllen = 0;
	if (ctb != CTB_KEYCTRL)
		fseek(f,fpusr+usrpktlen,SEEK_SET);
	else
		usrctrllen = (int) (ftell(f) - (fpusr+usrpktlen));
	for (;;) {
		status = readkeypacket(f,FALSE,&ctb,NULL,NULL,NULL,NULL,
					NULL,NULL,NULL,NULL,keyID2,NULL);
		if (status < 0  ||  is_key_ctb (ctb)  ||  ctb==CTB_USERID)
			break;
		if (equal_buffers(keyID, keyID2, KEYFRAGSIZE)) {
			fprintf(pgpout,
   LANG("\n\007Key is already signed by user '%s'.\n"),
				LOCAL_CHARSET(sigguffin));
			fclose(f);
			return -1;
		}
	}
	rewind(f);

	if (!batchmode) {
		fprintf(pgpout,
LANG("\n\nREAD CAREFULLY:  Based on your own direct first-hand knowledge, \
are\nyou absolutely certain that you are prepared to solemnly certify \
that\nthe above public key actually belongs to the user specified by \
the\nabove user ID (y/N)? "));
		if (!getyesno('n')) {
			fclose(f);
			return -1;
		}
	}

	{	/* temporary scope for some buffers */
		struct MD5Context MD;
		unit d[MAX_UNIT_PRECISION], p[MAX_UNIT_PRECISION];
		unit q[MAX_UNIT_PRECISION], u[MAX_UNIT_PRECISION];

		set_precision(MAX_UNIT_PRECISION); /* safest opening
						      assumption */

		if ((g = fopen(keyfile,FOPRBIN)) == NULL) {
			fclose(f);
			fprintf(pgpout,
   LANG("\n\007Can't open key ring file '%s'\n"),keyfile);
			return -1;
		}
		fseek(g, fp, SEEK_SET);
		/* Calculate signature */
		if (MDfile0_len(&MD, g, pktlen) < 0) {
			fclose(f);
			fclose(g);
			return -1; /* problem with input file.
				      error return */
		}
		fclose(g);

		/* Add data from user id */
		CToPascal((char *)userid);
		MD5Update(&MD, userid+1, (int)(unsigned char)userid[0]);

		strcpy((char *)userid,sigguffin); /* Who we are looking for */

		/* Make sure that we DONT use the internal password to
		 * get the secret key!  This way you need to type your
		 * pass phrase every time you come to this point!
		 * Derek Atkins		<warlord@MIT.EDU>	93-02-25
		 *
		 * If batchmode, then let it use the passed-in password,
		 * for signing agents.
		 * Derek Atkins		<warlord@MIT.EDU>	94-06-20
		 */
#ifdef MACTC5
		passhash[0]='\0';
#endif
		if (getsecretkey((batchmode ? 0 : GPK_ASKPASS), NULL, NULL, 
				 timestamp, NULL, NULL,
		                 userid, n, e, d, p, q, u) < 0)
		{
			fclose(f);
			return -1; /* problem with secret key file.
				      error return. */
		}

		certificate_length =
		  make_signature_certificate(certificate, &MD,
					     K0_SIGNATURE_BYTE, e, d, p, q,
					     u, n);
		if (certificate_length < 0) {
			fclose(f);
			return -1; /* error return from
				      make_signature_certificate() */
		}

	}	/* end of scope for some buffers */

	/* open file g for write, in binary (not text) mode...*/
	tempring = tempfile(TMP_TMPDIR);
	if ((g = fopen(tempring,FOPWBIN)) == NULL) {
		fprintf(pgpout,
   LANG("\n\007Can't create output file to update key ring.\n"));
		fclose(f);
		return -1;
	}

	/* Copy pre-key and key to file g */
	copyfile (f, g, fpusr+usrpktlen+usrctrllen);

	/* write out certificate record to outfile ... */
	fwrite(certificate,1,certificate_length,g);

	/* Add "trusty" control packet */
	write_trust (g, KC_SIGTRUST_ULTIMATE|KC_CONTIG|KC_SIG_CHECKED);

	/* Copy the remainder from file f to file g */
	copyfile (f, g, -1L);
	
	fclose(f);
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose(g);

	savetempbak(tempring,keyfile);

	fprintf(pgpout, LANG("\nKey signature certificate added.\n"));

        return 0;  /* normal return */

}       /* do_sign */


/*
 * Write an RSA-signed message digest of key for user keyguffin in
 * keyfile, using signature from user sigguffin.  Append
 * the signature right after the key.
 */
int signkey(char *keyguffin, char *sigguffin, char *keyfile)
{
	byte keyID[KEYFRAGSIZE];
	word32 tstamp; byte *timestamp = (byte *) &tstamp;
	byte userid[256];
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	long fp;
	int pktlen;
	int status;

	/* Get signature key ID */
	strcpy((char *)userid,sigguffin);	/* Who we are looking for */
	status = getsecretkey(0, NULL, NULL, timestamp, NULL, NULL,
	                      userid, n, e, NULL, NULL, NULL, NULL);
	if (status < 0)
		return -1; /* problem with secret key file. error return. */
	extract_keyID(keyID, n);	/* Remember signer key ID */

	/* Check that the public key exists in the destination keyring */
	status = getpublickey(GPK_NORVK|GPK_GIVEUP, keyfile, &fp, &pktlen,
				 keyID, timestamp, userid, n, e);
	if (status < 0) {
		PascalToC((char *)userid);
		fprintf(pgpout, LANG("\nError: Key for signing userid '%s'\n\
does not appear in public keyring '%s'.\n\
Thus, a signature made with this key cannot be checked on this keyring.\n"), 
		LOCAL_CHARSET((char *)userid), keyfile);
		return -1;	/* problem with public key file.
				   error return. */
	}

	strcpy((char *)userid, keyguffin);
	fprintf(pgpout, LANG("\nLooking for key for user '%s':\n"), 
		LOCAL_CHARSET((char *)userid));

	status = getpublickey(GPK_SHOW|GPK_NORVK, keyfile, &fp, &pktlen, NULL,
	                      timestamp, userid, n, e);
	if (status < 0)
	    return -1;
	showKeyHash(n, e);

	PascalToC((char *) userid);
        if (do_sign(keyfile, fp, pktlen, userid, keyID, sigguffin, batchmode) < 0)
            return -1;

	return 0;	/* normal return */

}	/* signkey */

/*======================================================================*/

/* Check signature in infile for validity.  Strip off the signature
 * and write the remaining packet to outfile.  If strip_signature,
 * also write the signature to outfile.sig.
 * the original filename is stored in preserved_name
 */
int check_signaturefile(char *infile, char *outfile, boolean strip_signature,
			char *preserved_name)
{
	byte ctb,ctb2=0;	/* Cipher Type Bytes */
	char keyfile[MAX_PATH];	/* for getpublickey */
	char sigfile[MAX_PATH]; /* .sig file if strip_signature */
	char plainfile[MAX_PATH]; /* buffer for getstring() */
	char *tempFileName;	/* Name for temporary uncanonicalized file */
	FILE *tempFile;
	long fp;
	FILE *f;
	FILE *g;
	long start_text;	/* marks file position */
	int i,count;
	word16 cert_length;
	byte certbuf[MAX_SIGCERT_LENGTH];
	byteptr certificate; /* for parsing certificate buffer */
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	byte inbuf[MAX_BYTE_PRECISION];
	byte outbuf[MAX_BYTE_PRECISION];
	byte keyID[KEYFRAGSIZE];
	word32 tstamp;
	byte *timestamp = (byte *) &tstamp; /* key certificate timestamp */
	word32 dummystamp;
	byte userid[256];
	struct MD5Context MD;
	byte digest[16];
	boolean separate_signature;
	boolean fixedLiteral = FALSE; /* Whether it's a fixed literal2
					 packet */
	extern char **myArgv;
	extern int myArgc;
	char lit_mode = MODE_BINARY;
	unsigned char litfile[MAX_PATH];
	word32 text_len = -1;
	int	status;
	byte	*mdextras;
	byte	mdlensave;
	byte	version;
	byte	mdlen;	/* length of material to be added to MD calculation */
	byte	class;
	byte	algorithm;
	byte	mdlow2[2];
	char	org_sys[5];	    /* Name of originating system */
        boolean retry = TRUE;
#ifdef VMS
	char	*fdl;
	short	fdl_len;
#endif
#ifdef MACTC5
	extern Boolean bad_separate_signature;
#endif
	int		outbufoffset;

	fill0( keyID, KEYFRAGSIZE );

	set_precision(MAX_UNIT_PRECISION);	/* safest opening assumption */

	strcpy(keyfile, globalPubringName); /* use default pathname */

	if (verbose)
		fprintf(pgpout,
			"check_signaturefile: infile = '%s', outfile = '%s'\n",
		infile,outfile);

	if (preserved_name)
		*preserved_name = '\0';

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
		      LANG("\n\007Can't open ciphertext file '%s'\n"),infile);
		return -1;
	}

   /******************** Read header CTB and length field ******************/

	fread(&ctb,1,1,f);	/* read certificate CTB byte */
	certificate = certbuf;
	*certificate++ = ctb;	/* copy ctb into certificate */

	if (!is_ctb(ctb) || !is_ctb_type(ctb,CTB_SKE_TYPE))
		goto badcert;	/* complain and return bad status */

	cert_length = getpastlength(ctb, f); /* read certificate length */
	certificate += ctb_llength(ctb);	/* either 1, 2, 4, or 8 */
	if (cert_length > MAX_SIGCERT_LENGTH-3)	/* Huge packet length */
		goto badcert;	/* complain and return bad status */

	/* read whole certificate: */
	if (fread((byteptr) certificate, 1, cert_length, f) < cert_length)
		/* bad packet length field */
		goto badcert;	/* complain and return bad status */

	version = *certificate++;
	if (version_byte_error(version))
		goto err1;

	mdlensave = mdlen = *certificate++; /* length of material to
					       be added to MD */
	mdextras = certificate;	/* pointer to extra material for
				   MD calculation */

	class = *certificate++;
	if (class != SM_SIGNATURE_BYTE  &&  class != SB_SIGNATURE_BYTE) {
		(void) version_error(class, SM_SIGNATURE_BYTE);
		goto err1;
	}
	mdlen--;

	if (mdlen>0) {	/* if more MD material is included... */
		for (i=0; i<SIZEOF_TIMESTAMP; ++i) {
			timestamp[i] = *certificate++;
			mdlen--;
		}
	}

	if (mdlen>0) {	/* if more MD material is included... */
		certificate+=2;	/* skip past unused validity period field */
		mdlen-=2;
	}

	for (i=0; i<KEYFRAGSIZE; i++)
		keyID[i] = *certificate++; /* copy rest of key fragment */

	algorithm = *certificate++;
	if (version_error(algorithm, RSA_ALGORITHM_BYTE))
		goto err1;

	algorithm = *certificate++;
	if (version_error(algorithm, MD5_ALGORITHM_BYTE))
		goto err1;

	mdlow2[0] = *certificate++;
	mdlow2[1] = *certificate++;

	/* getpublickey() sets precision for mpi2reg, if key not found: use
	   maximum precision to avoid error return from mpi2reg() */
	if (getpublickey(0, keyfile, &fp, NULL, keyID,
			(byte *)&dummystamp, userid, n, e) < 0) {
		set_precision(MAX_UNIT_PRECISION); /* safest opening
						      assumption */
		if (filter_mode || batchmode) retry = FALSE;
	}

	if (mpi2reg((unitptr)inbuf,certificate) == -1) /* get signed message
							  digest */
		goto err1;
	certificate += countbytes((unitptr)inbuf)+2;

	if ((certificate-certbuf) != cert_length+3)
		/*	Bad length in signature certificate.  Off by 
			((certificate-certbuf) - (cert_length+3)) */
		goto badcert;	/* complain and return bad status */

	start_text = ftell(f);	/* mark position of text for later */

	if (fread(outbuf,1,1,f) < 1) {	/* see if any plaintext is there */
		/*	Signature certificate has no plaintext following it.
			Must be in another file.  Go look. */
		separate_signature = TRUE;
		if (preserved_name) /* let caller know there is
				       no output file */
			strcpy(preserved_name, "/dev/null");
		fclose(f);
		fprintf(pgpout,
   LANG("\nFile '%s' has signature, but with no text."),infile);
		if (myArgc > 3 && file_exists(myArgv[3])) {
			outfile = myArgv[3];
			fprintf(pgpout,
   LANG("\nText is assumed to be in file '%s'.\n"),outfile);
		} else {
			strcpy(plainfile, outfile);
			outfile = plainfile;
			drop_extension(outfile);

			if (file_exists(outfile)) {
				fprintf(pgpout,
   LANG("\nText is assumed to be in file '%s'.\n"),outfile);
			} else {
				if (batchmode)
					return -1;
				fprintf(pgpout,
   LANG("\nPlease enter filename of material that signature applies to: "));
#ifdef MACTC5
				if (!GetFilePath(LANG("File signature applies to?"),outfile,GETFILE))
					strcpy(outfile, "");
				else
					fprintf(pgpout, "%s\n",outfile);
#else
				getstring(outfile,59,TRUE); /* echo keyboard */
#endif
				if ((int)strlen(outfile) == 0)
					return -1;
			}
		}
		/* open file f for read, in binary (not text) mode...*/
		if ((f = fopen(outfile,FOPRBIN)) == NULL) {
			fprintf(pgpout,
   LANG("\n\007Can't open file '%s'\n"),outfile);
			return -1;
		}
		start_text = ftell(f);	/* mark position of text for later */
 		text_len = fsize(f);	/* remember length of text */
	} else {
		separate_signature = FALSE;
		/* We just read 1 byte, so outbuf[0] should contain a ctb, 
		   maybe a CTB_LITERAL byte. */
		ctb2 = outbuf[0];
		fixedLiteral = is_ctb_type(ctb2,CTB_LITERAL2_TYPE);
		if (is_ctb(ctb2) && (is_ctb_type(ctb2,CTB_LITERAL_TYPE)
				     ||fixedLiteral))
		{	/* Read literal data */
			text_len = getpastlength(ctb2, f); /* read packet
							      length */
			lit_mode = '\0';
			fread (&lit_mode,1,1,f); /* get literal packet
						    mode byte */
			if (lit_mode != MODE_TEXT
			    && lit_mode != MODE_BINARY &&
				lit_mode != MODE_LOCAL)
			{
				fprintf(pgpout,
   "\n\007Error: Illegal mode byte %02x in literal packet.\n",
					lit_mode); /* English-only diagnostic
						      for debugging */
				(void) version_error(lit_mode, MODE_BINARY);
				goto err1;
			}
			if (verbose)
				fprintf(pgpout,
   LANG("File type: '%c'\n"), EXT_C(lit_mode));
			/* Read literal file name, use it if possible */
			litfile[0] = 0;
			fread (litfile,1,1,f);
			if( fixedLiteral )
				/* Get corrected text_len value by subtracting
				   the length of the filename and the
				   timestamp and mode byte and litfile
				   length byte */
				text_len -= litfile[0]
				  + sizeof(dummystamp) + 2;
			if (litfile[0] > 0)
			{
				if ((int)litfile[0] >= MAX_PATH) {
					fseek(f, litfile[0], SEEK_CUR);
					litfile[0] = 0;
				} else {
					 fread (litfile+1,1,litfile[0],f);
				}
			}
				/* Use litfile if it's writeable and he
				   didn't say an outfile */
			if (litfile[0]) {
				PascalToC( (char *)litfile );
#ifdef EBCDIC
				file_from_canon( (char *)litfile );
#endif
				if (verbose)
					fprintf(pgpout,
   LANG("Original plaintext file name was: '%s'\n"), litfile);
				if (preserved_name)
					strcpy(preserved_name,
					       (char *) litfile);
			}
			if (lit_mode == MODE_LOCAL) {
			    fread(org_sys, 1, 4, f); org_sys[4] = '\0';
#ifdef VMS
#define LOCAL_TEST !strncmp("VMS ",org_sys,4)
#else
#define LOCAL_TEST FALSE
#endif
			    if (LOCAL_TEST) {
#ifdef VMS
					fread(&fdl_len, 2, 1, f);
					fdl = (char *) malloc(fdl_len);
					fread(fdl, 1, fdl_len, f);
					if ((g = 
					     fdl_create( fdl, fdl_len,
							outfile,
							(char *) litfile))
					    == NULL)
					{
						fprintf(pgpout,
   "\n\007Unable to create file %s\n", outfile);
						return -1;
					}
					free(fdl);
					if (preserved_name)
						strcpy(preserved_name,
						       (char *) litfile);
					text_len -= (fdl_len
						     + sizeof(fdl_len));
#endif /* VMS */
			    } else {
					fprintf(pgpout,
   "\n\007Unrecognised local binary type %s\n",org_sys);
					return -1;
			    }
			} else {
			    /* Discard file creation timestamp for now */
			    fread (&dummystamp, 1, sizeof(dummystamp), f);
			}
			start_text = ftell(f);	/* mark position of
						   text for later */
		}	/* packet is CTB_LITERAL_TYPE */
	}

	/* Use keyID prefix to look up key... */

	/*	Get and validate public key from a key file: */
	if (!retry || getpublickey(0, keyfile, &fp, NULL, keyID,
			(byte *)&dummystamp, userid, n, e) < 0)
	{			/* Can't get public key.  Complain and
				   process file copy anyway. */
		fprintf(pgpout,
   LANG("\nWARNING: Can't find the right public key-- can't check signature \
integrity.\n"));
		goto outsig;
	}	/* Can't find public key */

	count = rsa_public_decrypt(outbuf, (unitptr)inbuf, e, n);

	if (!quietmode)
		fputc('.',pgpout);	/* Signal RSA completion. */

	/* outbuf should contain message digest packet */
	/*==================================================================*/
	/* Look at nested stuff within RSA block... */

	if (count == -7 || (count > 0 && count != sizeof(digest)))
	{
		fputs(LANG("\007\nUnrecognized message digest algorithm.\n\
This may require a newer version of PGP.\n\
Can't check signature integrity.\n"), pgpout);
		goto outsig;	/* Output data anyway */
	}
	if (count == -5) {	/* RSAREF returned malformed */
		fputs(
LANG("\a\nMalformed or obsolete signature.  Can't check signature \
integrity.\n"),
			pgpout);
		goto outsig;
	}
	if (count == -3) {	/* Key too big */
		fputs(
LANG("\a\nSigning key is too large.  Can't check signature integrity.\n"), pgpout);
		goto outsig;
	}
	if (count < 0) {	/* Catch-all */
		fprintf(pgpout,
LANG("\n\007Error: RSA-decrypted block is corrupted.\n\
This may be caused either by corrupted data or by using the wrong RSA key.\n\
"));
		goto outsig;	/* Output data anyway */
	}

	/* Distinguish PKCS-compatible from pre-3.3 which has an extra byte */
	outbufoffset = (count==sizeof(digest)) ? 0 : 1;

	if (outbuf[outbufoffset] != mdlow2[0]  ||
		outbuf[outbufoffset+1] != mdlow2[1])
	{
		fprintf(pgpout,
   LANG("\n\007Error: RSA-decrypted block is corrupted.\n\
This may be caused either by corrupted data or by using the wrong RSA key.\n\
"));
		goto outsig;	/* Output data anyway */
	}

	/* Reposition file to where that plaintext begins... */
	fseek(f,start_text,SEEK_SET); /* reposition file from last ftell */

	MDfile0_len(&MD,f,text_len); /* compute a message digest from
					rest of file */

	MD_addbuffer (&MD, mdextras, mdlensave, digest); /* Finish message
							    digest */

	convert_byteorder(timestamp,4); /* convert timestamp from external
					   form */
	PascalToC((char *)userid);	/* for display */

	/* now compare computed MD with claimed MD */
/* Assume MSB external byte ordering */
	if (!equal_buffers(digest, outbuf+outbufoffset, 16)) {
		/* IF the signature is bad, AND this machine does not use
		   MSDOS-stype canonical text as its native text format, AND
		   this is a detached signature certificate, AND this file
		   appears to contain non-canonical ASCII text, THEN we
		   convert the file to canonical text form and check the
		   signature again.  This is because a detached signature
		   certificate probably means the file is not currently in
		   a canonical text packet, but it was in canonical text form
		   when the signature was created, so by re-canonicalizing
		   it we can check the signature. */
		if (class == SM_SIGNATURE_BYTE && separate_signature
		    && is_text_file(outfile))
		{	/* Reposition file to where the plaintext begins
			   and canonicalize it */
			rewind( f );
			tempFileName = tempfile( TMP_WIPE | TMP_TMPDIR );
			if (verbose)
				fprintf(stderr,
   "signature checking failed, trying in canonical mode\n");
			make_canonical(outfile,tempFileName);
			if( ( tempFile = fopen( tempFileName, FOPRBIN ) )
			   != NULL )
			{
			    	/* Now check the signature */
				MDfile0_len(&MD, tempFile, -1L );
				MD_addbuffer(&MD, mdextras, mdlensave,
					     digest);

				/* Clean up behind us */
				fclose( tempFile );
				rmtemp( tempFileName );

				/* Check if the signature is OK this time
				   round */
/* Assume MSB external byte ordering */
				if(equal_buffers(digest, outbuf+outbufoffset,
						 16))
					goto goodsig;
			}
		}

		if (checksig_pass == 1) { /* Bad signature - try one more pass with other charset */
			checksig_pass++;
			return -1;
		}
#ifdef MACTC5
		if (separate_signature) bad_separate_signature = true;
#endif
/* FGG		fprintf(pgpout,"\007\n");
		fprintf(pgpout,
LANG("WARNING: Bad signature, doesn't match file contents!"));
		fprintf(pgpout,"\007\n");
		fprintf(pgpout,LANG("\nBad signature from user \"%s\".\n"),
			LOCAL_CHARSET((char *)userid));
		fprintf(pgpout,
LANG("Signature made %s using %d-bit key, key ID %s\n"),
                ctdate((word32 *)timestamp), countbits(n), key2IDstring(n)); */
		if (moreflag && !batchmode) {
			/* more will scroll the message off the screen */
			fprintf(pgpout, LANG("\nPress ENTER to continue..."));
			fflush(pgpout);
#ifdef MACTC5
			BailoutAlert(LANG("WARNING: Bad signature, doesn't match file contents!"));
#else
			getyesno('n');
#endif
		}
		goto warnsig;	/* Output data anyway */
	}

goodsig:
	signature_checked = TRUE;	/* set flag for batch processing */
	fprintf(pgpout,LANG("\nGood signature from user \"%s\".\n"),
		LOCAL_CHARSET((char *)userid));
	fprintf(pgpout,
LANG("Signature made %s using %d-bit key, key ID %s\n"),
                ctdate((word32 *)timestamp), countbits(n), key2IDstring(n));
#ifdef MACTC5
	AddResult((char *)userid);
#endif

warnsig:
	/* warn only, don't ask if user wants to use the key */
	warn_signatures(keyfile, fp, (char *)userid, TRUE);

outsig:
	/* Reposition file to where that plaintext begins... */
	fseek(f,start_text,SEEK_SET); /* reposition file from last ftell */

	if (separate_signature)
	{
		if (!quietmode)
			fprintf(pgpout,
LANG("\nSignature and text are separate.  No output file produced. "));
	} else {
		/* signature precedes plaintext in file... */
		/* produce a plaintext output file from signature file */
		/* open file g for write, in binary or text mode...*/
		if (lit_mode==MODE_LOCAL) {
#ifdef VMS
			if (status = fdl_copyfile2bin( f, g, text_len)) {
			     /*  Copy ok? */
				if (status > 0)
					fprintf(stderr,
					    "\n...copying to literal file\n");
				else
					perror(
					    "\nError copying from work file");
				fdl_close(g);
				goto err1;
			}
			fdl_close(g);
#endif /*VMS */
		} else {
			if (lit_mode == MODE_BINARY)
				g = fopen(outfile, FOPWBIN);
			else
				g = fopen(outfile, FOPWTXT);
			if (g == NULL) {
				fprintf(pgpout,
LANG("\n\007Can't create plaintext file '%s'\n"),outfile);
				goto err1;
			}
			CONVERSION = (lit_mode==MODE_TEXT)?EXT_CONV:NO_CONV;
			if (lit_mode == MODE_BINARY)
			    status = copyfile(f, g, text_len);
			else
			    status = copyfile_from_canon(f, g, text_len);
			CONVERSION = NO_CONV;
			if (write_error(g) || status < 0) {
				fclose(g);
				goto err1;
			}
			fclose(g);
		}

		if (strip_signature) {
			/* Copy signature to a .sig file */
			strcpy (sigfile, outfile);
			force_extension(sigfile,SIG_EXTENSION);
			if (!force_flag && file_exists(sigfile)) {
			fprintf(pgpout,
LANG("\n\007Signature file '%s' already exists.  Overwrite (y/N)? "),
					sigfile);
				if (!getyesno('n'))
					goto err1;
			}
			if ((g = fopen(sigfile,FOPWBIN)) == NULL) {
				fprintf(pgpout,
LANG("\n\007Can't create signature file '%s'\n"),sigfile);
				goto err1;
			}
			fseek (f,0L,SEEK_SET);
			copyfile (f,g,(unsigned long)(cert_length
						      +ctb_llength(ctb)+1));
			if (write_error(g)) {
				fclose(g);
				goto err1;
			}
			fclose(g);
			if (!quietmode)
				fprintf(pgpout,
LANG("\nWriting signature certificate to '%s'\n"),sigfile);
		}
	}

	burn(inbuf);	/* burn sensitive data on stack */
	burn(outbuf);	/* burn sensitive data on stack */
	fclose(f);
	if (separate_signature)
		return 0;	/* normal return, no nested info */
	if (is_ctb(ctb2) && (is_ctb_type(ctb2,CTB_LITERAL_TYPE)
			     || fixedLiteral))
				/* we already stripped away the CTB_LITERAL */
		return 0;	/* normal return, no nested info */
				/* Otherwise, it's best to assume a
				   nested CTB */
	return 1;		/* nested information return */

badcert:	/* Bad packet.  Complain. */
	fprintf(pgpout,
LANG("\n\007Error: Badly-formed or corrupted signature certificate.\n"));
	fprintf(pgpout,
LANG("File \"%s\" does not have a properly-formed signature.\n"),infile);
	/* Now just drop through to error exit... */

err1:
	burn(inbuf);	/* burn sensitive data on stack */
	burn(outbuf);	/* burn sensitive data on stack */
	fclose(f);
	return -1;	/* error return */

}	/* check_signaturefile */

/*
 * Check signature of key in file fkey at position fpkey, using signature
 * in file fsig and position fpsig.  keyfile tells the file to use to
 * look for the public key in to check the sig.  Return 0 if OK,
 * -1 generic error
 * -2 Can't find key
 * -3 Key too big
 * -4 Key too small
 * -5 Maybe malformed RSA
 * -6 Unknown PK algorithm
 * -7 Unknown conventional algorithm
 * -8 Unknown version
 * -9 Malformed RSA packet
 * -10 Malformed packet
 * -20 BAD SIGNATURE
 */
int check_key_sig(FILE *fkey, long fpkey, int keypktlen, char *keyuserid,
	 FILE *fsig, long fpsig, char *keyfile, char *siguserid,
		  byte *xtimestamp, byte *sigclass)
{
	byte ctb;	/* Cipher Type Bytes */
	long fp;
	word16 cert_length;
	int i, count;
	byte certbuf[MAX_SIGCERT_LENGTH];
	byteptr certificate; /* for parsing certificate buffer */
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	byte inbuf[MAX_BYTE_PRECISION];
	byte outbuf[MAX_BYTE_PRECISION];
	byte keyID[KEYFRAGSIZE];
	struct MD5Context MD;
	byte digest[16];
	byte *mdextras;
	word32 tstamp;
	byte *timestamp = (byte *) &tstamp; /* key certificate timestamp */
	byte	version;
	byte	mdlen;	/* length of material to be added to MD calculation */
	byte	class;
	byte	algorithm;
	byte	mdlow2[2];

	fill0( keyID, KEYFRAGSIZE );

	set_precision(MAX_UNIT_PRECISION);	/* safest opening assumption */

   /******************** Read header CTB and length field ******************/

	fseek(fsig, fpsig, SEEK_SET);
	fread(&ctb,1,1,fsig);	/* read certificate CTB byte */
	certificate = certbuf;
	*certificate++ = ctb;	/* copy ctb into certificate */

	if (!is_ctb(ctb) || !is_ctb_type(ctb,CTB_SKE_TYPE))
		goto badcert;	/* complain and return bad status */

	cert_length = getpastlength(ctb, fsig); /* read certificate length */
	certificate += ctb_llength(ctb);	/* either 1, 2, 4, or 8 */
	if (cert_length > MAX_SIGCERT_LENGTH-3)	/* Huge packet length */
		goto badcert;	/* complain and return bad status */

	/* read whole certificate: */
	if (fread((byteptr) certificate, 1, cert_length, fsig) < cert_length)
		/* bad packet length field */
		goto badcert;	/* complain and return bad status */

	version = *certificate++;
	if (version_byte_error(version))
		return -8;

	mdlen = *certificate++;	/* length of material to be added to MD */
	if (version_error(mdlen, 5))
		return -8;

	mdextras = certificate;	/* pointer to extra material for MD
				   calculation */

	*sigclass = class = *certificate++;
	if (class != K0_SIGNATURE_BYTE  &&  class != K1_SIGNATURE_BYTE &&
		class != K2_SIGNATURE_BYTE  &&  class != K3_SIGNATURE_BYTE &&
		class != KC_SIGNATURE_BYTE)
	{
		(void)version_error(class, K0_SIGNATURE_BYTE);
		return -8;
	}

	for (i=0; i<SIZEOF_TIMESTAMP; ++i)
		timestamp[i] = *certificate++;

	for (i=0; i<KEYFRAGSIZE; i++)
		keyID[i] = *certificate++; /* copy rest of key fragment */

	algorithm = *certificate++;
	if (version_error(algorithm, RSA_ALGORITHM_BYTE))
		return -6;

	algorithm = *certificate++;
	if (version_error(algorithm, MD5_ALGORITHM_BYTE))
		return -7;

	/* Grab 1st 2 bytes of message digest */
	mdlow2[0] = *certificate++;
	mdlow2[1] = *certificate++;

	/* We used to set precision here based on certificate value,
	 * but it was sometimes less than that based on n.  Read public
	 * key here to set precision, before we go on.
	 */
	/* This sets precision, too, based on n. */
	if (getpublickey(GPK_GIVEUP, keyfile, &fp, NULL, keyID,
			xtimestamp, (unsigned char *)siguserid, n, e) < 0)
		return -2;

	if (mpi2reg((unitptr)inbuf,certificate) == -1) /* get signed message
							  digest */
		return -10;
	certificate += countbytes((unitptr)inbuf)+2;

	if ((certificate-certbuf) != cert_length+3)
		/*	Bad length in signature certificate.  Off by 
			((certificate-certbuf) - (cert_length+3)) */
		return -10;	/* complain and return bad status */

	count = rsa_public_decrypt(outbuf, (unitptr)inbuf, e, n);

	if (count < 0)
		return count;

	if (count != sizeof(digest))
		return -9;	/* Bad RSA decrypt.  Corruption,
				   or wrong key. */

	/* outbuf should contain message digest packet */
	/*==================================================================*/
	/* Look at nested stuff within RSA block... */

/* Assume MSB external byte ordering */
	if (outbuf[0] != mdlow2[0]  || outbuf[1] != mdlow2[1])
		return -9;	/* Bad RSA decrypt.  Corruption,
				   or wrong key. */

	/* Position file to where that plaintext begins... */
	fseek(fkey,fpkey,SEEK_SET);

	/* compute a message digest from key packet */
	MDfile0_len(&MD,fkey,keypktlen);
	/* Add data from user id */
	if (class != KC_SIGNATURE_BYTE)
		MD5Update(&MD, (unsigned char *) keyuserid+1,
			  (int)(unsigned char)keyuserid[0]);
	/* Add time and class data */
	MD_addbuffer (&MD, mdextras, mdlen, digest); /* Finish message
							digest */

	/* now compare computed MD with claimed MD */
/* Assume MSB external byte ordering */
	if (!equal_buffers(digest, outbuf, 16))
		return -20;	/* BAD SIGNATURE */

	convert_byteorder(timestamp,4); /* convert timestamp from external
					   form */
	memcpy (xtimestamp, timestamp, 4); /* Return signature timestamp */

	return 0;	/* normal return */

badcert:	/* Bad packet.  Complain. */
	fprintf(pgpout,
LANG("\n\007Error: Badly-formed or corrupted signature certificate.\n"));
	return -10;
} /* check_key_sig */

/*======================================================================*/
static int squish_and_idea_file(byte *ideakey, FILE *f, FILE *g, 
	boolean attempt_compression)
{
	FILE *t;
	char *tempf = NULL;
	word32 fpos, fpos0;
	extern char plainfile[];

	/*
	**  If the caller specified that we should attempt compression, we
	**  create a temporary file 't' and compress our input file 'f' into
	**  't'.  Ideally, we would see if we get a good compression ratio 
	**  and if we did, then use file 't' for input and write a 
	**  CTB_COMPRESSED prefix.  But in this implementation we just always
	**  use the compressed output, even if it didn't compress well.
	*/

	rewind( f );

	if (!attempt_compression)
		t = f;	/* skip compression attempt */
	else	/* attempt compression-- get a tempfile */ 
		if ((tempf = tempfile(TMP_TMPDIR|TMP_WIPE)) == NULL ||
			(t = fopen(tempf, FOPWPBIN)) == NULL)
		  /* error: no tempfile */
			t = f;	/* skip compression attempt */
		else	/* attempt compression */ 
		{
			extern int zipup( FILE *, FILE * );


			if (verbose) fprintf(pgpout,
"\nCompressing [%s] ", plainfile);

			/* We don't put a length field on CTB_COMPRESSED yet */
			
			putc(CTB_COMPRESSED, t); /* write CTB_COMPRESSED */
				/* No CTB packet length specified
				   means indefinite length. */
			putc(ZIP2_ALGORITHM_BYTE, t); /* write ZIP algorithm
							 byte */

			/* Compression the file */
			zipup( f, t);
			if (write_error(t)) {
				fclose(t);
				if (tempf)
					rmtemp(tempf);
				return -1;
			}
			if (verbose) fprintf(pgpout, LANG("compressed.  ") );
			else if (!quietmode)
				fputc('.',pgpout);	/* show progress */
			rewind( t );
	  	}

	/*	Now write out file thru IDEA cipher... */

	/* Write CTB prefix, leave 4 bytes for later length */
	fpos0 = ftell(g);
	write_ctb_len (g, CTB_CKE_TYPE, 0L, TRUE);
	fpos = ftell(g) - fpos0;

	idea_file( ideakey, ENCRYPT_IT, t, g, fsize(t) );

	/* Now re-write CTB prefix, this time with length */
	fseek(g,fpos0,SEEK_SET);
	write_ctb_len (g, CTB_CKE_TYPE, fsize(g)-fpos, TRUE);

	if (t != f) {
		fclose( t );  /* close and remove the temporary file */
		if (tempf)
			rmtemp(tempf);
	}

	return 0;	/* normal return */

}	/* squish_and_idea_file */

int squish_file(char *infile, char *outfile)
{
	FILE *f, *g;
	extern int zip( FILE *, FILE * );

	if (verbose)
		fprintf(pgpout,"squish_file: infile = '%s', outfile = '%s'\n",
		infile,outfile);

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen( infile, FOPRBIN )) == NULL) {
		fprintf(pgpout,LANG("\n\007Can't open file '%s'\n"), infile );
		return -1;
	}

	/* open file g for write, in binary (not text) mode...*/
	if ((g = fopen( outfile, FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create compressed file '%s'\n"), outfile );
		fclose(f);
		return -1;
	}


	if (verbose) fprintf(pgpout, LANG("Compressing file..."));

	/* We don't put a length field on CTB_COMPRESSED yet */
	putc(CTB_COMPRESSED, g);	/* use compression prefix CTB */
	/* No CTB packet length specified means indefinite length. */
	putc(ZIP2_ALGORITHM_BYTE, g); 	/* use ZIP compression */

	/* Compress/store the file */
	zipup( f, g );
	if (verbose) fprintf(pgpout, LANG("compressed.  ") );

	fclose (f);
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose (g);
	return 0;
}   /* squish_file */

#define NOECHO1 1	/* Disable password from being displayed on screen */
#define NOECHO2 2	/* Disable password from being displayed on screen */

int idea_encryptfile(char *infile, char *outfile,
	boolean attempt_compression)
{
	FILE *f;	/* input file */
	FILE *g;	/* output file */
	byte ideakey[24];
	struct hashedpw *hpw;

	if (verbose)
		fprintf(pgpout,
"idea_encryptfile: infile = '%s', outfile = '%s'\n",
		infile,outfile);

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen( infile, FOPRBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open plaintext file '%s'\n"), infile );
		return -1;
	}

	/* open file g for write, in binary (not text) mode...*/
	if ((g = fopen( outfile, FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create ciphertext file '%s'\n"), outfile );
		fclose(f);
		return -1;
	}

	/* Get IDEA password, hashed to a key */
	if (passwds) {
		memcpy(ideakey, passwds->hash, sizeof(ideakey));
		memset(passwds->hash, 0, sizeof(passwds->hash));
		hpw = passwds;
		passwds = passwds->next;
		free(hpw);
	} else {
#ifdef MACTC5
 		byte savepass[20];
 		memcpy(savepass, passhash, 20);
		passhash[0] = '\0';
#endif
		if (!quietmode)
			fprintf(pgpout,
LANG("\nYou need a pass phrase to encrypt the file. "));
		if (batchmode
		    || GetHashedPassPhrase((char *)ideakey,NOECHO2) <= 0)
		{
			fclose(f);
			fclose(g);
#ifdef MACTC5
			memcpy(passhash, savepass, 20);
#endif
			return -1;
		}
#ifdef MACTC5
			memcpy(passhash, savepass, 20);
#endif
	}
	/*
	 * Get an initial vector, and write out a new randseed.bin.
	 * We do this now so that we can use the random bytes from the
	 * user's keystroke timings.
	 */
	make_random_ideakey(ideakey, 16);

	if (!quietmode) {
		fprintf(pgpout,
LANG("Just a moment..."));  /* this may take a while */
		fflush(pgpout);
	}

	/* Now compress the plaintext and encrypt it with IDEA... */
	squish_and_idea_file( ideakey, f, g, attempt_compression );

	burn(ideakey);	/* burn sensitive data on stack */

	fclose(f);
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose(g);

	return 0;

}	/* idea_encryptfile */

/*======================================================================*/

static byte (*keyID_list)[KEYFRAGSIZE] = NULL;

static int getmyname(char *userid) {
        char keyfile[MAX_PATH];
        unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
        word32 tstamp; byte *timestamp = (byte *) &tstamp;
        long fp;
        int pktlen;

	strcpy(keyfile, globalSecringName);

	getpublickey(GPK_SECRET, keyfile, &fp,
		     NULL, NULL, timestamp, (unsigned char *)userid, n, e);

        PascalToC((char *)userid);

        return(0);
}

int encryptfile(char **mcguffins, char *infile, char *outfile, 
	boolean attempt_compression)
{
	int i,ckp_length;
	FILE *f;
	FILE *g;
	byte keybuf[MAX_BYTE_PRECISION]; /* This keeps our IDEA to encrypt */
	byte ideakey[24]; /* must be big enough for make_random_ideakey */
	word32 chksum;
	char keyfile[MAX_PATH];
	int keys_used = 0;

	if (mcguffins == NULL || *mcguffins == NULL || **mcguffins == '\0') {
		/* Well, we haven't gotten a user, lets die here */
		return -1;	
	}

	if (verbose)
		fprintf(pgpout,"encryptfile: infile = %s, outfile = %s\n",
		infile,outfile);

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen( infile, FOPRBIN )) == NULL)
	{
		fprintf(pgpout,
LANG("\n\007Can't open plaintext file '%s'\n"), infile );
		return -1;
	}

	/* open file g for write, in binary (not text) mode...*/
	if ((g = fopen( outfile, FOPWBIN )) == NULL)
	{
		fprintf(pgpout,
LANG("\n\007Can't create ciphertext file '%s'\n"), outfile );
		fclose(f);
		return -1;
	}

	/*	Now we have to generate a random session key and IV.
		As part of this computation, we use the MD5 hash of the
		current file, if it has previously been obtained due to a
		signing operation.  If it has not been obtained, we hash
		the first 2K (for efficiency reasons) for input into
		the key generatrion process.  This is to ensure that
		capturing a randseed.bin file will not allow reconstruction
		of subsequent session keys without knowing the message
		that was encrypted.  (A session key only protects a
		single message, so it is reasonable to assume that an
		opponent trying to obtain a session key is trying to
		obtain, and thus is ignorant of, the message it encrypts.)

		This is not perfect, but it's an improvement on how session
		keys used to be generated, and can be changed in future
		without compatibility worries.
	*/

	if (!already_have_md5) {
		/* Obtain some random bits from the input file */
		struct MD5Context MD;

		MD5Init(&MD);
		MDfile0_len(&MD, f, 4096); /* Ignore errors - what could be
					      done? */
		MD5Final(md5buf, &MD);
		already_have_md5 = 1;

		fseek(f, 0, SEEK_SET); /* Get back to the beginning for
					  encryption */
	}

	ckp_length = make_random_ideakey(ideakey, 0);
	/* Returns a 24 byte random IDEA key */

/* Assume MSB external byte ordering */
	/* Prepend identifier byte to key */
	keybuf[0] = IDEA_ALGORITHM_BYTE;
	for (i=0; i<ckp_length; ++i)
		keybuf[i+1] = ideakey[i];
	/* Compute and append checksum to the key */
	chksum = checksum (keybuf+1, ckp_length);
	ckp_length++;
	put_word16((word16) chksum, keybuf+ckp_length);
	ckp_length += 2;

	/* Ok, we now have our IDEA key which we are going to use
	 * to encrypt our packet.  We have stuffed it into a packet
	 * which we can now encrypt in the Public Key of EACH USER
	 * which we want to be able to decrypt this message.  Now we
	 * will walk down mcguffins until we hit a NULL or NULL string,
	 * and we will encrypt for each user in the list, and write
	 * that out to the output file.
	 *
	 * -derek	<warlord@MIT.EDU>	13 Dec 1992
	 */

	for (i = 0; mcguffins[i] != NULL; ++i)
		;
	if (encrypt_to_self)
		++i;
	keyID_list = xmalloc(i * KEYFRAGSIZE);
	/* Iterate through users */
	for (; *mcguffins && **mcguffins ; ++mcguffins) {
		strcpy(keyfile, globalPubringName);
		/* use default pathname */

		keys_used =
			encryptkeyintofile(g, *mcguffins, keybuf,
					   keyfile, ckp_length, keys_used);
	} /* for */

	if (!keys_used) {
		fclose(f);
		fclose(g);
		free(keyID_list);
		return -1;
	}

	/* encrypt to myself if need be */
	if (encrypt_to_self) {
	        if (!*my_name)
		        /* Find our name from our keyring */
		       getmyname(my_name);
		if (*my_name)
			/* If we were successful */
		  keys_used = 
		    encryptkeyintofile(g, my_name, keybuf,
				       keyfile, ckp_length, keys_used);
	}
	free(keyID_list);

	/* Finished with RSA block containing IDEA key. */

	/* Now compress the plaintext and encrypt it with IDEA... */
	squish_and_idea_file( ideakey, f, g, attempt_compression );

	burn(keybuf);	/* burn the Idea Key Packet */
	burn(ideakey);	/* burn sensitive data on stack */

	fclose(f);
	if (write_error(g)) {
		fclose(g);
		return -1;
	}
	fclose(g);

	return 0;
}	/* encryptfile */

static int
encryptkeyintofile(FILE *g, char *mcguffin, byte *keybuf,
		   char *keyfile, int ckp_length, int keys_used) {
	int i;
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	byte keyID[KEYFRAGSIZE];
	byte inbuf[MAX_BYTE_PRECISION];
	byte outbuf[MAX_BYTE_PRECISION];
	word32 tstamp; byte *timestamp = (byte *) &tstamp; /* key certificate
							      timestamp */
	byte userid[256];
	long fp;
	int blocksize;
	byte (*keyp)[KEYFRAGSIZE];
	

	/* This "loop" is so we can break out at opportune moments */
	do {
		userid[0] = '\0';
		
		strcpy((char *)userid,mcguffin);
		/* Who we are looking for (C string) */
		
		/* Get and validate public key from a key file: 
		* We will be nice and ask the user ONCE (and ONLY once)
		* for a keyfile if its not in the default. 
		*/
		
		if (getpublickey((quietmode?0:GPK_SHOW)|GPK_NORVK,
				 keyfile, &fp, NULL, NULL,
				timestamp, userid, n, e) < 0)
		{
			fprintf(pgpout,
LANG("\n\007Cannot find the public key matching userid '%s'\n\
This user will not be able to decrypt this message.\n"), 
			LOCAL_CHARSET(mcguffin));
			continue;
		}
		
		/* Make sure we haven't already used this key */
		extract_keyID(keyID, n);
		for (keyp = keyID_list; keyp < keyID_list+keys_used; ++keyp) {
			if (!memcmp(keyp, keyID, KEYFRAGSIZE)) 
				break;
		}

		if (keyp < keyID_list + keys_used) {
				/* This key was already specified.
				   Quietly ignore it. */
			continue;
		}
		
		/* Add this keyID to the list of keys used so far */
		memcpy(keyp, keyID, KEYFRAGSIZE);
		
		PascalToC((char *)userid);
		if (warn_signatures(keyfile, fp, (char *)userid, 
				    FALSE) < 0) {
			fprintf(pgpout, LANG("Skipping userid %s\n"), mcguffin);
			continue;
		}
		
		/* set_precision has been properly called by getpublickey */
		
		/*	Note that RSA key must be at least big enough
			to encipher a complete conventional key packet 
			in a single RSA block.
		*/
		
		blocksize = countbytes(n)-1;	
		/* size of a plaintext block */
		
		if (blocksize < 31) {
			fprintf(pgpout,
"\n\007Error: RSA key length must be at least 256 bits.\n");
			fprintf(pgpout, "Skipping userid %s\n", mcguffin);
			continue;
		}
		
#ifdef MR_DEBUG
		/* XXX This is dangerous... This will print out the
		 * IDEA Key, which is a breach of security!
		 */
		fprintf(pgpout, "Idea Key: ");
		for (i = 0; i < ckp_length; i++)
			fprintf(pgpout, "%02X ", keybuf[i]);
		fprintf(pgpout, "\n");
#endif
		i = rsa_public_encrypt((unitptr)outbuf, keybuf,
				       ckp_length, e, n);
		if (i < 0) {
			if (i == -4) {
				fprintf(pgpout,
"\n\007Error: RSA key length must be at least 256 bits.\n");
			} else if (i == -3) {
				fputs(
"\a\nError: key is too large.  RSA keys may be no longer than 1024 bits\
,\ndue to limitations imposed by software provided by RSADSI.\n", pgpout);
			} else {
				fprintf(pgpout,
"\a\nUnexpected error %d encrypting\n", i);
			}
			fprintf(pgpout,
LANG("Skipping userid %s\n"), mcguffin);
			continue;
		}
		
		/* write out header record to outfile ... */
		
		/* PKE is Public Key Encryption */
		write_ctb_len (g, CTB_PKE_TYPE,
			       1+KEYFRAGSIZE+1+2+countbytes((unitptr)outbuf), 
			       FALSE);
		
		/* Write version byte */
		putc(version_byte, g);
		
		writekeyID( n, g );	
		/* write msg prefix fragment of modulus n */
		
		/* Write algorithm byte */
		putc(RSA_ALGORITHM_BYTE, g);
		
		/* convert RSA ciphertext block via reg2mpi and 
		* write to file
		*/
		
		write_mpi( (unitptr)outbuf, g, FALSE );
		
		burn(inbuf);	/* burn sensitive data on stack */
		burn(outbuf);	/* burn sensitive data on stack */
		++keys_used;

	} while (0);

	return keys_used;
}		/* encryptkeyintofile */

/*======================================================================*/

/*
 * Prepend a CTB_LITERAL prefix to a file.  Convert to canonical form if
 * lit_mode is MODE_TEXT.
 */
int make_literal(char *infile, char *outfile, char lit_mode, char *literalfile)
{
	char lfile[MAX_PATH];
	FILE *f;
	FILE *g;
	int status = 0;
#ifdef VMS
	char *fdl;
	short fdl_len;
#endif /* VMS */

	word32 flen, fpos;
	word32 dummystamp = 0;

	if (verbose)
		fprintf(pgpout,
"make_literal: infile = %s, outfile = %s, mode = '%c', literalfile = '%s'\n",
		infile,outfile,EXT_C(lit_mode),literalfile);

	/* open file f for read, in binary or text mode...*/

#ifdef VMS
	if (lit_mode == MODE_LOCAL) {
	    if (!(fdl_generate(infile, &fdl, &fdl_len ) & 01)) {
		fprintf(pgpout,
LANG("\n\007Can't open input plaintext file '%s'\n"),infile);
		return -1;
	    }
	}
#endif /*VMS*/
	if (lit_mode == MODE_TEXT)
		f = fopen(infile, FOPRTXT);
	else
		f = fopen(infile, FOPRBIN);
	if (f == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open input plaintext file '%s'\n"),infile);
		return -1;
	}
	flen = fsize(f);

	/* 	open file g for write, in binary (not text) mode... */
	if ((g = fopen( outfile,FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create plaintext file '%s'\n"), outfile );
		goto err1;
	}

	if (literalfile == NULL) {
		/* Put in a zero byte to indicate no filename */
		lfile[0] = '\0';
	} else {
		strcpy( lfile, literalfile );
		file_to_canon( lfile );
		CToPascal( lfile );
	}

#ifdef USE_LITERAL2
#define	LENGTH_FIELD		(flen + (unsigned char) lfile[0] + 6)
#define	LIT_TYPE	CTB_LITERAL2_TYPE
#else
#define	LENGTH_FIELD	flen
#define	LIT_TYPE	CTB_LITERAL_TYPE
#endif
	if (lit_mode == MODE_BINARY)
		write_ctb_len (g, LIT_TYPE, LENGTH_FIELD, FALSE);
#ifdef VMS
	else if (lit_mode == MODE_LOCAL)
		write_ctb_len (g, CTB_LITERAL2_TYPE, flen
			       + fdl_len + sizeof(fdl_len) + 6, TRUE);
#endif /* VMS */
	else /* Will put in size field later for text mode */
		write_ctb_len (g, LIT_TYPE, 0L, TRUE);
#ifdef USE_LITERAL2
	fpos = ftell(g);
#endif
	putc(lit_mode, g);

	if (lit_mode == MODE_LOCAL) {
#ifdef VMS
	    write_litlocal( g, fdl, fdl_len);
	    free(fdl);
#else
	    ;   /*  Null statement if we don't have anything to do! */
#endif /* VMS */
	} else {
	    /* write literalfile name */
		fwrite (lfile, 1, (unsigned char) lfile[0]+1, g);
	    /* Dummy file creation timestamp */
	    fwrite ( &dummystamp, 1, sizeof(dummystamp), g);
	}
#ifndef USE_LITERAL2
	fpos = ftell(g);
#endif

	if ((lit_mode == MODE_BINARY) || (lit_mode == MODE_LOCAL)) {
		if (copyfile( f, g, -1L )) {
		    fprintf(pgpout,
"\n\007Unable to append to literal plaintext file");
		    perror("\n");
		    fclose(g);
		    goto err1;
		}
	} else {
		CONVERSION = (lit_mode == MODE_TEXT) ? INT_CONV : NO_CONV;
		status = copyfile_to_canon( f, g, -1L );
		CONVERSION = NO_CONV;
		/* Re-write CTB with correct length info */
		rewind (g);
		write_ctb_len (g, LIT_TYPE, fsize(g)-fpos, TRUE);
	}
	if (write_error(g) || status < 0) {
		fclose(g);
		goto err1;
	}
	fclose(g);
	fclose(f);
	return 0;	/* normal return */

err1:
	fclose(f);
	return -1;	/* error return */

}	/* make_literal */
#undef LENGTH_FIELD
#undef LIT_TYPE

/*======================================================================*/

/*
 * Strip off literal prefix from infile, copying to outfile.
 * Get lit_mode and literalfile info from
 * the prefix.  Replace outfile with literalfile unless
 * literalfile is illegal
 * the original filename is stored in preserved_name
 * If lit_mode is MODE_TEXT, convert from canonical form as we
 * copy the data.
 */
int strip_literal(char *infile, char *outfile, char *preserved_name,
		char *lit_mode)
{
	byte ctb;	/* Cipher Type Byte */
	FILE *f;
	FILE *g;
	word32 LITlength = 0;
	unsigned char litfile[MAX_PATH];
	word32 dummystamp;
	char	org_sys[5];	    /* Name of originating system */
	int	status;
#ifdef VMS
	char	*fdl;
	short	fdl_len;
#endif
	*lit_mode = MODE_BINARY;
	if (verbose)
		fprintf(pgpout,"strip_literal: infile = %s, outfile = %s\n",
		infile,outfile);

	if (preserved_name)
		*preserved_name = '\0';

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open input plaintext file '%s'\n"),infile);
		return -1;
	}

	fread(&ctb,1,1,f);	/* read Cipher Type Byte */

	if (!is_ctb(ctb) || !(is_ctb_type(ctb,CTB_LITERAL_TYPE) ||
	    is_ctb_type(ctb,CTB_LITERAL2_TYPE)))
	{
		/* debug message in English only -- something got corrupted */
		fprintf(pgpout,
"\n\007'%s' is not a literal plaintext file.\n",infile);
		fclose(f);
		return -1;
	}

	LITlength = getpastlength(ctb, f); /* read packet length */

	/* Read literal data */
	*lit_mode = '\0';
	fread (lit_mode,1,1,f);
	if ((*lit_mode != MODE_BINARY) && (*lit_mode != MODE_TEXT)
	    && (*lit_mode != MODE_LOCAL))
	{
		(void) version_error(*lit_mode, MODE_TEXT);
		fclose(f);
		return -1;
	}
	if (verbose)
		fprintf(pgpout, LANG("File type: '%c'\n"), EXT_C(*lit_mode));
	/* Read literal file name, use it if possible */
	litfile[0] = 0;
	fread (litfile,1,1,f); 
	if (is_ctb_type(ctb, CTB_LITERAL2_TYPE)) {
				/* subtract header length: namelength
				   + lengthbyte + modebyte + stamp */
		LITlength -= litfile[0] + 2 + sizeof(dummystamp);
	}
	/* Use litfile if it's writeable and he didn't say an outfile */
	if (litfile[0] > 0) {
		if ((int)litfile[0] >= MAX_PATH) {
			fseek(f, litfile[0], SEEK_CUR);
			litfile[0] = 0;
		} else {
			fread (litfile+1,1,litfile[0],f);
		}
	}
	if (litfile[0]) {
		PascalToC( (char *)litfile );
#ifdef EBCDIC
	       	file_from_canon( (char *)litfile );
#endif
		if (verbose)
			fprintf(pgpout,
LANG("Original plaintext file name was: '%s'\n"), litfile);
		if (preserved_name)
			strcpy(preserved_name, (char *) litfile);
	}
	if (*lit_mode == MODE_LOCAL) {
		fread(org_sys, 1, 4, f); org_sys[4] = '\0';
#ifdef VMS
#define LOCAL_TEST !strncmp("VMS ",org_sys,4)
#else
#define LOCAL_TEST FALSE
#endif
		if (LOCAL_TEST) {
#ifdef VMS
			remove(outfile); /*  Prevent litter, we recreate
					     the file with correct chars. */
			fread(&fdl_len, 2, 1, f);
			fdl = (char *) malloc(fdl_len);
			fread(fdl, 1, fdl_len, f);
			if ((g = fdl_create( fdl, fdl_len, outfile,
					    (char *) litfile)) == NULL) {
				fprintf(pgpout,
"\n\007Unable to create file %s\n", outfile);
				return -1;
			}
			free(fdl);
			if (preserved_name)
				strcpy(preserved_name, (char *) litfile);
			LITlength -= (fdl_len + sizeof(fdl_len));
#endif /* VMS */
		} else {
			fprintf(pgpout,
"\n\007Unrecognised local binary type %s\n",org_sys);
			return -1;
		}
	} else {
	    /* Discard file creation timestamp for now */
	    fread (&dummystamp, 1, sizeof(dummystamp), f);
	}

	if (*lit_mode==MODE_LOCAL) {
#ifdef VMS
		if (status = fdl_copyfile2bin( f, g, LITlength)) {
				/*  Copy ok? */
			if (status > 0)
				fprintf(stderr,
"\n...copying to literal file\n");
			else
				perror("\nError copying from work file");
			fdl_close(g);
			goto err1;
		}
		fdl_close(g);
#endif /*VMS */
	} else {
	    if (*lit_mode == MODE_TEXT)
			g = fopen(outfile, FOPWTXT);
	    else
			g = fopen(outfile, FOPWBIN);
	    if (g == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create plaintext file '%s'\n"), outfile );
		    goto err1;
	    }
	    /* copy rest of literal plaintext file */
	    CONVERSION = (*lit_mode == MODE_TEXT) ? EXT_CONV : NO_CONV;
	    if (*lit_mode == MODE_BINARY)
		    status = copyfile(f, g, LITlength);
		else
		    status = copyfile_from_canon(f, g, LITlength);
	    CONVERSION = NO_CONV;
		if (write_error(g) || status < 0) {
			fclose(g);
			goto err1;
		}
		fclose(g);
	}

	fclose(f);
	return 0;	/* normal return */

err1:
	fclose(f);
	return -1;	/* error return */

}	/* strip_literal */

/*======================================================================*/

int decryptfile(char *infile, char *outfile)
{
	byte ctb;	/* Cipher Type Byte */
	byte ctbCKE; /* Cipher Type Byte */
	FILE *f;
	FILE *g;
	int count = 0, status, thiskey, gotkey, end_of_pkes;
	unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
	unit d[MAX_UNIT_PRECISION];
	unit p[MAX_UNIT_PRECISION], q[MAX_UNIT_PRECISION];
	unit u[MAX_UNIT_PRECISION];
	byte inbuf[MAX_BYTE_PRECISION];
	byte outbuf[MAX_BYTE_PRECISION];
	byte keyID[KEYFRAGSIZE];
	word32 tstamp; byte *timestamp = (byte *) &tstamp; /* key certificate
							      timestamp */
	byte userid[256];
	word32 flen;
	word32 fpos = 0;
	byte ver, alg;
	short realprecision = 0;
	word16 chksum;
	struct nkey {
		byte keyID[KEYFRAGSIZE];
		struct nkey *next;
	} *nkey, *nkeys = NULL;

	if (verbose)
		fprintf(pgpout,"decryptfile: infile = %s, outfile = %s\n",
		infile,outfile);

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open ciphertext file '%s'\n"),infile);
		return -1;
	}

	/* Now we have to keep reading in packets until we either get
	 * to a non PKE-type packet or we find our own...  Once we find
	 * our own, we're gonna have to get our private key, and then
	 * keep going until we find the end of the PKE packets
	 *
	 * -derek	<warlord@MIT.EDU>	13 Dec 1992
	 */

	gotkey = end_of_pkes = 0; /* Set this flag now. */
	do {
		thiskey = 0;

		set_precision(MAX_UNIT_PRECISION);
		/* Need to set this EACH TIME...   Sigh.  This is because
		 * read_mpi needs to have a global_precision which is
		 * >= the size of the key.  Therefore once we find the
		 * real key, we save off the precision and then we'll
		 * reset it later.	-derek
		 */

		fread(&ctb,1,1,f);	/* read Cipher Type Byte */
		if (!is_ctb(ctb)) {
			fprintf(pgpout,
LANG("\n\007'%s' is not a cipher file.\n"),infile);
			fclose(f);
			return -1;
		}

		/* PKE is Public Key Encryption */
		if (!is_ctb_type(ctb,CTB_PKE_TYPE)) {
			end_of_pkes = 1;
			continue;
		}

		getpastlength(ctb, f); /* read packet length */

		/* Read and check version */
		ver = getc(f);
		if (version_byte_error(ver)) {
			fclose (f);
			return (-1);
		}

		fread(keyID,1,KEYFRAGSIZE,f); /* read key ID */
		/* Use keyID prefix to look up key. */

		/* Add this keyID to the list of keys read in */
		nkey = (struct nkey *) malloc(sizeof(struct nkey));
		if (nkey == NULL) {
			fprintf(stderr, LANG("\n\007Out of memory.\n"));
			exitPGP(7);
		}
		memcpy(nkey->keyID, keyID, KEYFRAGSIZE);
		nkey->next = nkeys;
		nkeys = nkey;

		/* Read and check algorithm */
		alg = getc(f);
		if (version_error(alg, RSA_ALGORITHM_BYTE)) {
			fclose (f);
			return (-1);
		}

		if (!gotkey)		/* Only do this if we havent already */
			/*	Get and validate secret key from a key file: */
			if (getsecretkey(GPK_GIVEUP|(quietmode?0:GPK_SHOW),
					 NULL, keyID, timestamp, NULL, NULL,
					 userid, n, e, d, p, q, u) == 0)
				{	
					thiskey = gotkey = 1;
					realprecision = global_precision;
				} else {
					set_precision(MAX_UNIT_PRECISION);
				}					
		/* DAMN this... This is REALLY frustrating, that I have to
		 * do this...  Basically, if I go to getsecretkey, it will
		 * set the precision, and the precision might NOT be correct
		 * if the key I get is not correct, so I have to set the
		 * precision NUMEROUS times in this loop..  This sucks, 
		 * but its the only way.  Sigh.
		 *
		 * -derek	<warlord@MIT.EDU>	13 Dec 1992
		 */

		/*	Note that RSA key must be at least big enough 
			to encipher a complete conventional key packet in 
			a single RSA block. */

		/*========================================================*/
		/* read ciphertext block, converting to internal format: */
		read_mpi((unitptr)inbuf, f, FALSE, FALSE);

		if (thiskey) {
			if (!quietmode) {
				fprintf(pgpout,LANG("Just a moment...")); 
				/* RSA will take a while. */
				fflush(pgpout);
			}
			count = rsa_private_decrypt(outbuf, (unitptr)inbuf,
			                            e, d, p, q, u, n);
			if (count < 0) {
				if (count == -3) {
					fputs(
"\a\nError: key is too large.  RSA keys may be no longer than 1024 bits\
,\ndue to limitations imposed by software provided by RSADSI.\n", pgpout);
				} else if (count == -9 || count == -7) {
					fprintf(pgpout,
LANG("\n\007Error: RSA-decrypted block is corrupted.\n\
This may be caused either by corrupted data or by using the wrong RSA key.\n\
"));
				} else if (count == -5) {
				        fprintf(pgpout,
LANG("\n\007Error: RSA block is possibly malformed.  Old format, maybe?\n"));
				} else {
					fprintf(pgpout,
"\a\nUnexpected error %d decrypting\n", count);
				}
				fclose(f);
				return count;
			}
		
			if (!quietmode)
				fputc('.',pgpout);	
					/* Signal RSA completion. */
		}

		fpos = ftell(f);	/* Save this position */

	} while (!end_of_pkes);		/* Loop until end of PKE packets */

	/* Should we list the recipients? */
	if (!gotkey || verbose) {
		char *user;

		setkrent(NULL);
		init_userhash();
		if (gotkey)	/* verbose flag */
			fprintf(pgpout,"\nRecipients:\n");
		else
			fprintf(pgpout,
LANG("\nThis message can only be read by:\n"));

		for (nkey = nkeys; nkey; nkey = nkey->next) {
			if ((user = user_from_keyID(nkey->keyID)) == NULL)
				fprintf(pgpout,
LANG("  keyID: %s\n"), keyIDstring(nkey->keyID));
			else
				fprintf(pgpout, "  %s\n", LOCAL_CHARSET(user));
		}
		endkrent();
	}
	for (nkey = nkeys; nkey; ) {
		nkey = nkey->next;
		free(nkeys);
		nkeys = nkey;
	}

	/* Ok, Now lets clean up, and continue on to the rest of the file so
	 * that it can be decrypted properly.  Things should be ok once I
	 * reset some stuff here...	-derek
	 */
	if (gotkey) {
		fseek(f, fpos, SEEK_SET); /* Get back to the Real McCoy! */
		set_precision(realprecision); /* reset the precision */
	} else {
		/* No secret key, exit gracefully (NOT!) */
		fprintf(pgpout,
LANG("\n\007You do not have the secret key needed to decrypt this file.\n"));
		fclose(f);
		return -1;
	}
	/* Verify that top of buffer has correct algorithm byte */
	--count;	/* one less byte to drop algorithm byte */
/* Assume MSB external byte ordering */
	if (version_error(outbuf[0], IDEA_ALGORITHM_BYTE)) {
		fclose(f);
		return -1;
	}

	/* Verify checksum */
	count -= 2;	/* back up before checksum */
/* Assume MSB external byte ordering */
	chksum = fetch_word16(outbuf+1+count);
	if (chksum != checksum(outbuf+1, count)) {
		fprintf(pgpout,
LANG("\n\007Error: RSA-decrypted block is corrupted.\n\
This may be caused either by corrupted data or by using the wrong RSA key.\n\
"));
		fclose(f);
		return -1;
	}

	/* outbuf should contain random IDEA key packet */
	/*==================================================================*/

	/* 	open file g for write, in binary (not text) mode... */

	if ((g = fopen( outfile, FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create plaintext file '%s'\n"), outfile );
		goto err1;
	}

	fread(&ctbCKE,1,1,f);	/* read Cipher Type Byte, should be CTB_CKE */
	if (!is_ctb(ctbCKE) || !is_ctb_type(ctbCKE,CTB_CKE_TYPE)) {
		/* Should never get here. */
		fprintf(pgpout,"\007\nBad or missing CTB_CKE byte.\n");
		goto err1;	/* Abandon ship! */
	}

	flen = getpastlength(ctbCKE, f); /* read packet length */

	/* Decrypt ciphertext file */
/* Assume MSB external byte ordering */
	status = idea_file( outbuf+1, DECRYPT_IT, f, g, flen );
	if (status < 0) {
		fprintf(pgpout,
LANG("\n\007Error: Decrypted plaintext is corrupted.\n"));
	}
	if (!quietmode)
		fputc('.',pgpout);	/* show progress */

	if (write_error(g)) {
		fclose(g);
		goto err1;
	}
	fclose(g);
	fclose(f);
	burn(inbuf);	/* burn sensitive data on stack */
	burn(outbuf);	/* burn sensitive data on stack */
	mp_burn(d);	/* burn sensitive data on stack */
	mp_burn(p);	/* burn sensitive data on stack */
	mp_burn(q);	/* burn sensitive data on stack */
	mp_burn(u);	/* burn sensitive data on stack */
	if (status < 0)	/* if idea_file failed, then error return */
		return status;
	return 1;	/* always indicate output file has
			   nested stuff in it. */

err1:
	fclose(f);
	burn(inbuf);	/* burn sensitive data on stack */
	burn(outbuf);	/* burn sensitive data on stack */
	mp_burn(d);	/* burn sensitive data on stack */
	mp_burn(p);	/* burn sensitive data on stack */
	mp_burn(q);	/* burn sensitive data on stack */
	mp_burn(u);	/* burn sensitive data on stack */
	return -1;	/* error return */

}	/* decryptfile */

int idea_decryptfile(char *infile, char *outfile)
{
	byte ctb;	/* Cipher Type Byte */
	FILE *f;
	FILE *g;
	byte ideakey[16];
	int status, retries = 0;
	struct hashedpw *hpw, **hpwp;
	word32 flen;

	if (verbose)
		fprintf(pgpout,"idea_decryptfile: infile = %s, outfile = %s\n",
		infile,outfile);

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open ciphertext file '%s'\n"),infile);
		return -1;
	}

	/* 	open file g for write, in binary (not text) mode... */
	if ((g = fopen( outfile, FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create plaintext file '%s'\n"), outfile );
		goto err1;
	}

	/* First, try all pre-specified passwords */
	hpwp = &passwds;
	hpw = *hpwp;

	do /* while pass phrase is bad */
	{
		fread(&ctb,1,1,f); /* read Cipher Type Byte,
				      should be CTB_CKE */

		if (!is_ctb(ctb) || !is_ctb_type(ctb,CTB_CKE_TYPE)) {
			/* Should never get here. */
			fprintf(pgpout,"\007\nBad or missing CTB_CKE byte.\n");
			fclose(g);
			goto err1;	/* Abandon ship! */
		}
		flen = getpastlength(ctb, f); /* read packet length */

		/* Get IDEA password, hashed */
		if (hpw) {
			/* first try environment passwords */
			memcpy(ideakey, hpw->hash, sizeof(ideakey));
		} else {
#ifdef MACTC5
 			byte savepass[20];
 			memcpy(savepass, passhash, 20);
			passhash[0] = '\0';
#endif
			fprintf(pgpout,
LANG("\nYou need a pass phrase to decrypt this file. "));
			if (batchmode
			    || GetHashedPassPhrase((char *)ideakey,NOECHO1)
			    <= 0)
			{
				fclose(f);
				fclose(g);
#ifdef MACTC5
	 			memcpy(savepass, passhash, 20);
#endif
				return -1;
			}
#ifdef MACTC5
	 			memcpy(savepass, passhash, 20);
#endif
		}

		if (!quietmode) {
			fprintf(pgpout,LANG("Just a moment..."));
				/* this may take a while */
			fflush(pgpout);
		}

		status = idea_file( ideakey, DECRYPT_IT, f, g, flen );
		if (status == 0) {
			if (hpw) {
				/* "Use up" password. */
				*hpwp = hpw->next;
				memset(hpw->hash, 0, sizeof(hpw->hash));
				free(hpw);
			}
			break;
		}
		if (hpw) {
			/* Go to next available password */
			hpwp = &hpw->next;
			hpw = *hpwp;
		} else {
			++retries;
			fprintf(pgpout,
LANG("\n\007Error:  Bad pass phrase.\n"));
#ifdef MACTC5
			passhash[0] = '\0';
#endif
		}

		rewind(f);
		rewind(g);
	} while (status == -2 && retries < 2);

	burn(ideakey);	/* burn sensitive data on stack */

	if (status == 0 && !quietmode)
		fputc('.',pgpout);	/* show progress */

	if (write_error(g)) {
		fclose(g);
		goto err1;
	}
	fclose(g);
	fclose(f);

	if (status < 0) {	/* if idea_file failed, then complain */
		remove(outfile);	/* throw away our mistake */
		return status;		/* error return */
	}
	if (!quietmode)
		fprintf(pgpout,LANG("Pass phrase appears good. "));
	return 1;		/* always indicate output file has
				   nested stuff in it. */

err1:
	fclose(f);
	return -1;	/* error return */

}	/* idea_decryptfile */

int decompress_file(char *infile, char *outfile)
{
	byte ctb;
	FILE *f;
	FILE *g;
	extern void lzhDecode( FILE *, FILE * );
	extern int unzip( FILE *, FILE * );
	if (verbose) fprintf(pgpout, LANG("Decompressing plaintext...") );

	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(infile,FOPRBIN)) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't open compressed file '%s'\n"),infile);
		return -1;
	}

	fread(&ctb,1,1,f);	/* read and skip over Cipher Type Byte */
	if (!is_ctb_type( ctb, CTB_COMPRESSED_TYPE )) {
		/* Shouldn't get here, or why were we called to begin with? */
		fprintf(pgpout,"\007\nBad or missing CTB_COMPRESSED byte.\n");
		goto err1;	/* Abandon ship! */
	}

	getpastlength(ctb, f); /* read packet length */
	/* The packet length is ignored.  Assume it's huge. */

	fread(&ctb,1,1,f);	/* read and skip over compression
				   algorithm byte */
	if (ctb != ZIP2_ALGORITHM_BYTE) {
		/* We only know how to uncompress deflate-compressed data.  We
		   may hit imploded or Lharc'ed data but treat it as an error
		   just the same */
		fprintf(pgpout,
LANG("\007\nUnrecognized compression algorithm.\n\
This may require a newer version of PGP.\n"));
		goto err1;	/* Abandon ship! */
	}

	/* 	open file g for write, in binary (not text) mode... */
	if ((g = fopen( outfile, FOPWBIN )) == NULL) {
		fprintf(pgpout,
LANG("\n\007Can't create decompressed file '%s'\n"), outfile );
		goto err1;
	}

	if (unzip( f, g )) {
		fprintf(pgpout,
LANG("\n\007Decompression error.  Probable corrupted input.\n"));
		goto err2;
	}

	if (verbose)
		fprintf(pgpout, LANG("done.  ") );
	else if (!quietmode)
		fputc('.',pgpout);	/* show progress */

	if (write_error(g))
		goto err2;

	fclose(g);
	fclose(f);
	return 1;		/* always indicate output file
				   has nested stuff in it. */

err2:
	fclose(g);
err1:
	fclose(f);
	return -1;	/* error return */

} /* decompress_file */


/* ==== fileio.c ==== */
/*      fileio.c  - I/O routines for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.

   Modified 16 Apr 92 - HAJK
   Mods for support of VAX/VMS file system

   Modified 17 Nov 92 - HAJK
   Change to temp file stuff for VMS.
 */

#if defined(WINDOWS)
#define WIN32
#else
#define UNIX
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _BSD
#include <sys/param.h>
#endif
extern int errno;
#endif				/* UNIX */
#ifdef VMS
#include <file.h>
#include <assert.h>
#endif
#include "random.h"
#include "mpilib.h"
#include "mpiio.h"
#include "fileio.h"
#include "language.h"
#include "pgp.h"
#include "exitpgp.h"
#include "charset.h"
#include "system.h"
#if defined(MSDOS) || defined(OS2) || defined (WIN32)
#include <io.h>
#include <fcntl.h>
#endif
#ifdef MACTC5
#include "crypto.h" 	/* for get_header_info_from_file() */
#include "AEStuff.h"
#include "AppGlobals.h"
#include "MacPGP.h"
#include "Macutil2.h"
#include "Macutil3.h"
#define MULTIPLE_DOTS
extern Boolean AEProcessing;
pascal Boolean idleProc(EventRecord * eventIn, long *sleep, RgnHandle * mouseRgn);
#endif

char *ck_dup_output(char *, boolean, boolean);

#ifndef F_OK
#define F_OK	0
#define X_OK	1
#define W_OK	2
#define R_OK	4
#endif				/* !F_OK */

/*
 * DIRSEPS is a string of possible directory-separation characters
 * The first one is the preferred one, which goes in between
 * PGPPATH and the file name if PGPPATH is not terminated with a
 * directory separator.
 */


#if defined(MSDOS) || defined(__MSDOS__) || defined(OS2) || defined (WIN32)
static char const DIRSEPS[] = "\\/:";
#define BSLASH

#elif defined(ATARI)
static char const DIRSEPS[] = "\\/:";
#define BSLASH

#elif defined(UNIX)
static char const DIRSEPS[] = "/";
#define MULTIPLE_DOTS

#elif defined(AMIGA)
static char const DIRSEPS[] = "/:";
#define MULTIPLE_DOTS

#elif defined(VMS)
static char const DIRSEPS[] = "]:";

#elif defined(EBCDIC)
static char const DIRSEPS[] = "("; 	/* Any more? */
#define MULTIPLE_DOTS

#elif defined(MACTC5)
#define MULTIPLE_DOTS
static char const DIRSEPS[] = ":";

#else
/* FGG: Consider as UNIX */
static char const DIRSEPS[] = "/";
#endif

#ifdef __PUREC__
#include <ext.h>
int access(const char *name,int flag)
{
struct ffblk dummy;
	return findfirst(name,&dummy,-1);
}
#endif

/* 1st character of temporary file extension */
#define	TMP_EXT	'$'		/* extensions are '.$##' */

/* The PGPPATH environment variable */

static char PGPPATH[] = "PGPPATH";

/* Disk buffers, used here and in crypto.c */
byte textbuf[DISKBUFSIZE];
static unsigned textbuf2[2 * DISKBUFSIZE / sizeof(unsigned)];

boolean file_exists(char *filename)
/*      Returns TRUE iff file exists. */
{
#ifdef MACTC5
	FILE *f;
	/* open file f for read, in binary (not text) mode...*/
	if ((f = fopen(filename,FOPRBIN)) == NULL)
		return(FALSE);
	fclose(f);
	return(TRUE);
#else
    return access(filename, F_OK) == 0;
#endif
}				/* file_exists */

static boolean is_regular_file(char *filename)
{
#ifdef S_ISREG
    struct stat st;
    return stat(filename, &st) != -1 && S_ISREG(st.st_mode);
#else
    return TRUE;
#endif
}


/*
 * This wipes a file with pseudo-random data.  The purpose of this is to
 * make sure no sensitive information is left on the disk.  The use
 * of pseudo-random data is to defeat disk compression drivers (such as
 * Stacker and dblspace) so that we are guaranteed that the entire file
 * has been overwritten.
 *
 * Note that the file MUST be open for read/write.
 *
 * It may not work to eliminate everything from non-volatile storage
 * if the OS you're using does its own paging or swapping.  Then
 * it's an issue of how the OS's paging device is wiped, and you can
 * only hope that the space will be reused within a few seconds.
 *
 * Also, some file systems (in particular, the Logging File System
 * for Sprite) do not write new data in the same place as old data,
 * defeating this wiping entirely.  Fortunately, such systems 
 * usually don't need a swap file, and for small temp files, they
 * do write-behind, so if you create and delete a file fast enough,
 * it never gets written to disk at all.
 */

/*
 * The data is randomly generated with the size of the file as a seed.
 * The data should be random and not leak information.  If someone is
 * examining deleted files, presumably they can reconstruct the file size,
 * so that's not a secret.  H'm... this wiping algorithm makes it easy to,
 * given a block of data, find the size of the file it came from
 * and the offset of this block within it.  That in turn reveals
 * something about the state of the disk's allocation tables when the
 * file was used, possibly making it easier to find other files created
 * at neaby times - such as plaintext files.  Is this acceptable?
 */

/*
 * Theory of operation: We use the additive congruential RNG
 * r[i] = r[i-24] + r[i-55], from Knuth, Vol. 2.  This is fast
 * and has a long enough period that there should be no repetitions
 * in even a huge file.  It is seeded with r[-55] through r[-1]
 * using another polynomial-based RNG.  We seed a linear feedback
 * shift register (CRC generator) with the size of the swap file,
 * and clock in 0 bits.  Each 32 bits, the value of the generator is
 * taken as the next integer.  This is just to ensure a reasonably
 * even mix of 1's and 0's in the initialization vector.
 */

/*
 * This is the CRC-32 polynomial, which should be okay for random
 * number generation.
 * x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1
 * = 1 0000 0100 1100 0001 0001 1101 1011 0111
 * = 0x04c11db7
 */
#define POLY 0x04c11db7

static void wipeout(FILE * f)
{
    unsigned *p1, *p2, *p3;
    unsigned long len;
    unsigned long t;
    int i;

    /* Get the file size */
    fseek(f, 0L, SEEK_END);
    len = ftell(f);
#ifdef MACTC5 
	len = len + 4096 - (len % 4096);
#endif 
    rewind(f);

    /* Seed of first RNG.  Inverted to get more 1 bits */
    t = ~len;

    /* Initialize first 55 words of buf with pseudo-random stuff */
    p1 = (unsigned *) textbuf2 + 55;
    do {
	for (i = 0; i < 32; i++)
	    t = (t & 0x80000000) ? t << 1 ^ POLY : t << 1;
	*--p1 = (unsigned) t;
    } while (p1 > (unsigned *) textbuf2);

    while (len) {
	/* Fill buffer with pseudo-random integers */

	p3 = (unsigned *) textbuf2 + 55;
	p2 = (unsigned *) textbuf2 + 24;
	p1 = (unsigned *) textbuf2 + sizeof(textbuf2) / sizeof(*p1);
	do {
	    *--p1 = *--p2 + *--p3;
	} while (p2 > (unsigned *) textbuf2);

	p2 = (unsigned *) textbuf2 + sizeof(textbuf2) / sizeof(*p1);
	do {
	    *--p1 = *--p2 + *--p3;
	} while (p3 > (unsigned *) textbuf2);

	p3 = (unsigned *) textbuf2 + sizeof(textbuf2) / sizeof(*p3);
	do {
	    *--p1 = *--p2 + *--p3;
	} while (p1 > (unsigned *) textbuf2);

	/* Write it out - yes, we're ignoring errors */
	if (len > sizeof(textbuf2)) {
	    fwrite((char const *) textbuf2, sizeof(textbuf2), 1, f);
	    len -= sizeof(textbuf2);
#ifdef MACTC5
		mac_poll_for_break();
#endif
	} else {
	    fwrite((char const *) textbuf2, len, 1, f);
	    len = 0;
	}
    }
}


/*
 * Completely overwrite and erase file, so that no sensitive
 * information is left on the disk.
 */
int wipefile(char *filename)
{
    FILE *f;
    /* open file f for read/write, in binary (not text) mode... */
    if ((f = fopen(filename, FOPRWBIN)) == NULL)
	return -1;		/* error - file can't be opened */
    wipeout(f);
    fclose(f);
    return 0;			/* normal return */
}				/* wipefile */

/*
 * Returns the part of a filename after all directory specifiers.
 */
char *file_tail(char *filename)
{
    char *p;
    char const *s = DIRSEPS;

    while (*s) {
	p = strrchr(filename, *s);
	if (p)
	    filename = p + 1;
	s++;
    }

    return filename;
}


/* return TRUE if extension matches the end of filename */
boolean has_extension(char *filename, char *extension)
{
    int lf = strlen(filename);
    int lx = strlen(extension);

    if (lf <= lx)
	return FALSE;
    return !strcmp(filename + lf - lx, extension);
}

/* return TRUE if path is a filename created by tempfile() */
/* Filename matches "*.$[0-9][0-9]" */
boolean is_tempfile(char *path)
{
    char *p = strrchr(path, '.');

    return p != NULL && p[1] == TMP_EXT &&
	isdigit(p[2]) && isdigit(p[3]) && p[4] == '\0';
}

/*
 * Returns TRUE if user left off file extension, allowing default.
 * Note that the name is misleading if multiple dots are allowed.
 * not_pgp_extension or something would be better.
 */
boolean no_extension(char *filename)
{
#ifdef MULTIPLE_DOTS		/* filename can have more than one dot */
    if (has_extension(filename, ASC_EXTENSION) ||
	has_extension(filename, PGP_EXTENSION) ||
	has_extension(filename, SIG_EXTENSION) ||
#ifdef MACTC5
		has_extension(filename,".tmp") ||
#endif
	is_tempfile(filename))
	return FALSE;
    else
	return TRUE;
#else
    filename = file_tail(filename);

    return strrchr(filename, '.') == NULL;
#endif
}				/* no_extension */


/* deletes trailing ".xxx" file extension after the period. */
void drop_extension(char *filename)
{
    if (!no_extension(filename))
	*strrchr(filename, '.') = '\0';
}				/* drop_extension */


/* append filename extension if there isn't one already. */
void default_extension(char *filename, char *extension)
{
    if (no_extension(filename))
	strcat(filename, extension);
}				/* default_extension */

#ifndef MAX_NAMELEN
#if defined(AMIGA) || defined(NeXT) || (defined(BSD) && BSD > 41) || (defined(sun) && defined(i386))
#define	MAX_NAMELEN	255
#else
#ifdef MACTC5
#define MAX_NAMELEN 31
#else
#include <limits.h>
#endif
#endif
#endif

/* truncate the filename so that an extension can be tacked on. */
static void truncate_name(char *path, int ext_len)
{
#ifdef UNIX			/* for other systems this is a no-op */
    char *p;
#ifdef MAX_NAMELEN		/* overrides the use of pathconf() */
    int namemax = MAX_NAMELEN;
#else
    int namemax;
#ifdef _PC_NAME_MAX
    char dir[MAX_PATH];

    strcpy(dir, path);
    if ((p = strrchr(dir, '/')) == NULL) {
	strcpy(dir, ".");
    } else {
	if (p == dir)
	    ++p;
	*p = '\0';
    }
    if ((namemax = pathconf(dir, _PC_NAME_MAX)) <= ext_len)
	return;
#else
#ifdef NAME_MAX
    namemax = NAME_MAX;
#else
    namemax = 14;
#endif				/* NAME_MAX */
#endif				/* _PC_NAME_MAX */
#endif				/* MAX_NAMELEN */

    if ((p = strrchr(path, '/')) == NULL)
	p = path;
    else
	++p;
    if (strlen(p) > namemax - ext_len) {
	if (verbose)
	    fprintf(pgpout, "Truncating filename '%s' ", path);
	p[namemax - ext_len] = '\0';
	if (verbose)
	    fprintf(pgpout, "to '%s'\n", path);
    }
#else
#ifdef MACTC5
	char *p;
	p = file_tail(path);
	if (verbose)
		fprintf(pgpout, LANG("Truncating filename '%s' "), path);
	if (strlen(p) + ext_len > MAX_NAMELEN) p[MAX_NAMELEN - ext_len] = '\0';
#endif  /* MACTC5 */ 
#endif				/* UNIX */
}

/* change the filename extension. */
void force_extension(char *filename, char *extension)
{
    drop_extension(filename);	/* out with the old */
    truncate_name(filename, strlen(extension));
    strcat(filename, extension);	/* in with the new */
}				/* force_extension */


/*
 * Get yes/no answer from user, returns TRUE for yes, FALSE for no. 
 * First the translations are checked, if they don't match 'y' and 'n'
 * are tried.
 */
#ifdef MACTC5

boolean getyesno(char default_answer)
{ 
  extern FILE *logfile;
  short  alertid,i,large,err;
  char dfault[8], ndfault[8];
  ProcessSerialNumber psn;
  if (batchmode)
  	return(default_answer == 'y' ? TRUE : FALSE);
  if (strlen(Yes_No_Message)<72) large=0;
  else large=100;
  strcpy(dfault,default_answer == 'y' ? LANG("y") : LANG("n"));
  strcpy(ndfault,default_answer == 'n' ? LANG("y") : LANG("n"));
  for(i=0;i<strlen(Yes_No_Message);i++)
  if (Yes_No_Message[i]<' ' && Yes_No_Message[i]>=0) Yes_No_Message[i]=' ';	/* It's a signed char! */
  InitCursor();
  alertid=(default_answer == 'n' ? 211+large : 212+large);
  c2pstr(Yes_No_Message);
  ParamText((uchar *)Yes_No_Message,(uchar *)"", \
  		(uchar *)"",(uchar *)"");
  if (AEProcessing) {
  	if (gHasProcessManager)
  		GetFrontProcess(&psn);
  	if(MyInteractWithUser())
  		return default_answer;
  	if (gHasProcessManager)
    	SetFrontProcess(&psn);
  }
  if (CautionAlert(alertid,nil)==1){
   p2cstr((uchar *)Yes_No_Message);
   fputs(strcat(Yes_No_Message,dfault),stderr);
   fputc('\n',stderr);
   fflush(stderr);
   return(default_answer == 'y' ? TRUE : FALSE);
   }
  p2cstr((uchar *)Yes_No_Message);
  fputs(strcat(Yes_No_Message,ndfault),stderr);
  fputc('\n',stderr);
  fflush(stderr);
  return(default_answer == 'n' ? TRUE : FALSE);
  } 
#else

boolean getyesno(char default_answer)
{
    char buf[8];
    static char yes[8], no[8];

    if (yes[0] == '\0') {
	strncpy(yes, LANG("y"), 7);
	strncpy(no, LANG("n"), 7);
    }
    if (!batchmode) {		/* return default answer in batchmode */
	getstring(buf, 6, TRUE);	/* echo keyboard input */
	strlwr(buf);
	if (!strncmp(buf, no, strlen(no)))
	    return FALSE;
	if (!strncmp(buf, yes, strlen(yes)))
	    return TRUE;
	if (buf[0] == 'n')
	    return FALSE;
	if (buf[0] == 'y')
	    return TRUE;
    }
    return default_answer == 'y' ? TRUE : FALSE;
}				/* getyesno */
#endif

/* if user consents to it, change the filename extension. */
char *maybe_force_extension(char *filename, char *extension)
{
    static char newname[MAX_PATH];
    if (!has_extension(filename, extension)) {
	strcpy(newname, filename);
	force_extension(newname, extension);
	if (!file_exists(newname)) {
	    fprintf(pgpout, LANG("\nShould '%s' be renamed to '%s' (Y/n)? "),
		    filename, newname);
	    if (getyesno('y'))
		return newname;
	}
    }
    return NULL;
}				/* maybe_force_extension */

/*
 * Add a trailing directory separator to a name, if absent.
 */
static void addslash(char *name)
{
    int i = strlen(name);

    if (i != 0 && !strchr(DIRSEPS, name[i - 1])) {
	name[i] = DIRSEPS[0];
	name[i + 1] = '\0';
    }
}

/*
 * Builds a filename with a complete path specifier from the environmental
 * variable PGPPATH.
 */
char *buildfilename(char *result, char *fname)
{
#ifdef MACTC5
	char const *s;
#else
    char const *s = "."; /* getenv(PGPPATH); */
#endif
    result[0] = '\0';
#ifdef MACTC5
    return(strcpy(result,fname));
#endif

    if (s && strlen(s) <= 50) {
	strcpy(result, s);
    }
#ifdef UNIX
    /* On Unix, default to $HOME/.pgp, otherwise, current directory. */
    else {
	s = getenv("HOME");
	if (s && strlen(s) <= 50) {
	    strcpy(result, s);
	    addslash(result);
	    strcat(result, ".pgp");
	}
    }
#endif				/* UNIX */

    addslash(result);
    strcat(result, fname);
    return result;
}				/* buildfilename */

char *buildsysfilename(char *result, char *fname)
{
#ifdef PGP_SYSTEM_DIR
    strcpy(result, PGP_SYSTEM_DIR);
    strcat(result, fname);
    if (file_exists(result))
	return result;
#endif
    buildfilename(result, fname);	/* Put name back for error */
    return result;
}


/* Convert filename to canonical form, with slashes as separators */
void file_to_canon(char *filename)
{
#ifdef EBCDIC
    CONVERT_TO_CANONICAL_CHARSET(filename);
#endif
#ifdef BSLASH
    while (*filename) {
	if (*filename == '\\')
	    *filename = '/';
	++filename;
    }
#else	/* 203a */
#ifdef MACTC5
	while (*filename) {
		if (*filename == ':')
			*filename = '/';
		++filename;
		}
#endif
#endif
}

#ifdef EBCDIC
/* Convert filename from canonical form */
void file_from_canon(char *filename)
{
   strcpy( filename, LOCAL_CHARSET(filename) );
}
#endif /* EBCDIC */


int write_error(FILE * f)
{
    fflush(f);
    if (ferror(f)) {
#ifdef ENOSPC
	if (errno == ENOSPC)
	    fprintf(pgpout, LANG("\nDisk full.\n"));
	else
#endif
	    fprintf(pgpout, LANG("\nFile write error.\n"));
	return -1;
    }
    return 0;
}

/* copy file f to file g, for longcount bytes */
int copyfile(FILE * f, FILE * g, word32 longcount)
{
    int count, status = 0;
    do {			/* read and write the whole file... */
	if (longcount < (word32) DISKBUFSIZE)
	    count = (int) longcount;
	else
	    count = DISKBUFSIZE;
	count = fread(textbuf, 1, count, f);
	if (count > 0) {
	    if (CONVERSION != NO_CONV) {
		int i;
		for (i = 0; i < count; i++)
		    textbuf[i] = (CONVERSION == EXT_CONV) ?
			EXT_C(textbuf[i]) :
			INT_C(textbuf[i]);
	    }
	    if (fwrite(textbuf, 1, count, g) != count) {
		/* Problem: return error value */
		status = -1;
		break;
	    }
	    longcount -= count;
#ifdef MACTC5
			mac_poll_for_break();
#endif
	}
	/* if text block was short, exit loop */
    } while (count == DISKBUFSIZE);
    burn(textbuf);		/* burn sensitive data on stack */
    return status;
}				/* copyfile */

/*
 * Like copyfile, but takes a position for file f.  Returns with
 * f and g pointing just past the copied data.
 */
int copyfilepos(FILE * f, FILE * g, word32 longcount, word32 fpos)
{
    fseek(f, fpos, SEEK_SET);
    return copyfile(f, g, longcount);
}


/* copy file f to file g, for longcount bytes.  Convert to
 * canonical form as we go.  f is open in text mode.  Canonical
 * form uses crlf's as line separators.
 */
int copyfile_to_canon(FILE * f, FILE * g, word32 longcount)
{
    int count, status = 0;
    byte c, *tb1, *tb2;
    int i, nbytes;
    int nspaces = 0;
#ifdef MACTC5
    Boolean warning = true; /* MACTC5 */
#endif
    do {			/* read and write the whole file... */
	if (longcount < (word32) DISKBUFSIZE)
	    count = (int) longcount;
	else
	    count = DISKBUFSIZE;
	count = fread(textbuf, 1, count, f);
	if (count > 0) {
	    /* Convert by adding CR before LF */
	    tb1 = textbuf;
	    tb2 = (byte *) textbuf2;
	    for (i = 0; i < count; ++i) {
		switch (CONVERSION) {
		case EXT_CONV:
		    c = EXT_C(*tb1++);
		    break;
		case INT_CONV:
		    c = INT_C(*tb1++);
		    break;
		default:
		    c = *tb1++;
		}
#ifdef MACTC5
		if ( (((uchar) c) < ' ') && (c != '\n') && (c != '\t') && warning) {
			warning = false;
			fprintf(stdout, "\aWarning text file contains control characters!\n");
		}
#endif
		if (strip_spaces) {
		    if (c == ' ') {
			/* Don't output spaces yet */
			nspaces += 1;
		    } else {
			if (c == '\n') {
			    *tb2++ = '\r';
			    nspaces = 0;	/* Delete trailing spaces */
			}
			if (nspaces) {
			    /* Put out spaces now */
			    do
				*tb2++ = ' ';
			    while (--nspaces);
			}
			*tb2++ = c;
		    }
		} else {
		    if (c == '\n')
			*tb2++ = '\r';
		    *tb2++ = c;
		}
	    }
	    nbytes = tb2 - (byte *) textbuf2;
	    if (fwrite(textbuf2, 1, nbytes, g) != nbytes) {
		/* Problem: return error value */
		status = -1;
		break;
	    }
	    longcount -= count;
	}
	/* if text block was short, exit loop */
    } while (count == DISKBUFSIZE);
    burn(textbuf);		/* burn sensitive data on stack */
    burn(textbuf2);
    return status;
}				/* copyfile_to_canon */


/* copy file f to file g, for longcount bytes.  Convert from
 * canonical to local form as we go.  g is open in text mode.  Canonical
 * form uses crlf's as line separators.
 */
int copyfile_from_canon(FILE * f, FILE * g, word32 longcount)
{
    int count, status = 0;
    byte c, *tb1, *tb2;
    int i, nbytes;
    do {			/* read and write the whole file... */
	if (longcount < (word32) DISKBUFSIZE)
	    count = (int) longcount;
	else
	    count = DISKBUFSIZE;
	count = fread(textbuf, 1, count, f);
	if (count > 0) {
	    /* Convert by removing CR's */
	    tb1 = textbuf;
	    tb2 = (byte *) textbuf2;
	    for (i = 0; i < count; ++i) {
		switch (CONVERSION) {
		case EXT_CONV:
		    c = EXT_C(*tb1++);
		    break;
		case INT_CONV:
		    c = INT_C(*tb1++);
		    break;
		default:
		    c = *tb1++;
		}
		if (c != '\r')
		    *tb2++ = c;
	    }
	    nbytes = tb2 - (byte *) textbuf2;
	    if (fwrite(textbuf2, 1, nbytes, g) != nbytes) {
		/* Problem: return error value */
		status = -1;
		break;
	    }
	    longcount -= count;
	}
	/* if text block was short, exit loop */
    } while (count == DISKBUFSIZE);
    burn(textbuf);		/* burn sensitive data on stack */
    burn(textbuf2);
    return status;
}				/* copyfile_from_canon */

/*      Copy srcFile to destFile  */
int copyfiles_by_name(char *srcFile, char *destFile)
{
    FILE *f, *g;
    int status = 0;
    long fileLength;

    f = fopen(srcFile, FOPRBIN);
    if (f == NULL)
	return -1;
    g = fopen(destFile, FOPWBIN);
    if (g == NULL) {
	fclose(f);
	return -1;
    }
    /* Get file length and copy it */
    fseek(f, 0L, SEEK_END);
    fileLength = ftell(f);
    rewind(f);
    status = copyfile(f, g, fileLength);
    fclose(f);
    if (write_error(g))
	status = -1;
    fclose(g);
    return status;
}				/* copyfiles_by_name */

/* Copy srcFile to destFile, converting to canonical text form  */
int make_canonical(char *srcFile, char *destFile)
{
    FILE *f, *g;
    int status = 0;
    long fileLength;

    if (((f = fopen(srcFile, FOPRTXT)) == NULL) ||
	((g = fopen(destFile, FOPWBIN)) == NULL))
	/* Can't open files */
	return -1;

    /* Get file length and copy it */
    fseek(f, 0L, SEEK_END);
    fileLength = ftell(f);
    rewind(f);
    CONVERSION = INT_CONV;
    status = copyfile_to_canon(f, g, fileLength);
    CONVERSION = NO_CONV;
    fclose(f);
    if (write_error(g))
	status = -1;
    fclose(g);
    return status;
}				/* make_canonical */

/*
 * Like rename() but will try to copy the file if the rename fails.
 * This is because under OS's with multiple physical volumes if the
 * source and destination are on different volumes the rename will fail
 */
int rename2(char *srcFile, char *destFile)
{
    FILE *f, *g;
#ifdef MACTC5
	int copy=-1;
#endif
    int status = 0;
    long fileLength;

#ifdef MACTC5
	copy=MoveRename(srcFile,destFile);
if (copy)
#else
#if defined(VMS) || defined(C370)
    if (rename(srcFile, destFile) != 0)
#else
    if (rename(srcFile, destFile) == -1)
#endif
#endif
    {
	/* Rename failed, try a copy */
	if (((f = fopen(srcFile, FOPRBIN)) == NULL) ||
	    ((g = fopen(destFile, FOPWBIN)) == NULL))
	    /* Can't open files */
	    return -1;

#ifdef MACTC5
		{
		FInfo finderInfo;
		c2pstr(srcFile);
		c2pstr(destFile);
		if(GetFInfo((uchar *)srcFile,0,&finderInfo)==0)
			SetFInfo((uchar *)destFile,0,&finderInfo);
		p2cstr((uchar *)srcFile);
		p2cstr((uchar *)destFile);
		}
#endif

	/* Get file length and copy it */
	fseek(f, 0L, SEEK_END);
	fileLength = ftell(f);
	rewind(f);
	status = copyfile(f, g, fileLength);
	if (write_error(g))
	    status = -1;

	/* Zap source file if the copy went OK, otherwise zap the (possibly
	   incomplete) destination file */
	if (status >= 0) {
	    wipeout(f);		/* Zap source file */
	    fclose(f);
	    remove(srcFile);
	    fclose(g);
	} else {
	    if (is_regular_file(destFile)) {
		wipeout(g);	/* Zap destination file */
		fclose(g);
		remove(destFile);
	    } else {
		fclose(g);
	    }
	    fclose(f);
	}
    }
    return status;
}

/* read the data from stdin to the phantom input file */
int readPhantomInput(char *filename)
{
    FILE *outFilePtr;
    byte buffer[512];
    int bytesRead, status = 0;

    if (verbose)
	fprintf(pgpout, "writing stdin to file %s\n", filename);
    if ((outFilePtr = fopen(filename, FOPWBIN)) == NULL)
	return -1;

#if defined(MSDOS) || defined(OS2) || defined (WIN32)
    /* Under DOS must set input stream to binary mode to avoid data mangling */
    setmode(fileno(stdin), O_BINARY);
#endif				/* MSDOS || OS2 */
    while ((bytesRead = fread(buffer, 1, 512, stdin)) > 0)
	if (fwrite(buffer, 1, bytesRead, outFilePtr) != bytesRead) {
	    status = -1;
	    break;
	}
    if (write_error(outFilePtr))
	status = -1;
    fclose(outFilePtr);
#if defined(MSDOS) || defined(OS2) || defined (WIN32)
    setmode(fileno(stdin), O_TEXT);	/* Reset stream */
#endif				/* MSDOS || OS2 */
    return status;
}

/* write the data from the phantom output file to stdout */
int writePhantomOutput(char *filename)
{
    FILE *outFilePtr;
    byte buffer[512];
    int bytesRead, status = 0;

    if (verbose)
	fprintf(pgpout, "writing file %s to stdout\n", filename);
    /* this can't fail since we just created the file */
    outFilePtr = fopen(filename, FOPRBIN);

#if defined(MSDOS) || defined(OS2) || defined (WIN32)
    setmode(fileno(stdout), O_BINARY);
#endif				/* MSDOS || OS2 */
    while ((bytesRead = fread(buffer, 1, 512, outFilePtr)) > 0)
	if (fwrite(buffer, 1, bytesRead, stdout) != bytesRead) {
	    status = -1;
	    break;
	}
    fclose(outFilePtr);
    fflush(stdout);
    if (ferror(stdout)) {
	status = -1;
	fprintf(pgpout, LANG("\007Write error on stdout.\n"));
    }
#if defined(MSDOS) || defined(OS2) || defined (WIN32)
    setmode(fileno(stdout), O_TEXT);
#endif				/* MSDOS || OS2 */

    return status;
}

/* Return the size from the current position of file f to the end */
word32 fsize(FILE * f)
{
    long fpos = ftell(f);
    long fpos2;

    fseek(f, 0L, SEEK_END);
    fpos2 = ftell(f);
    fseek(f, fpos, SEEK_SET);
    return (word32) (fpos2 - fpos);
}

/* Return TRUE if file filename looks like a pure text file */
int is_text_file (char *filename) /* EWS */
{
    FILE *f = fopen(filename,"r");      /* FOPRBIN gives problem with VMS */
    int i, n, lfctr = 0;
    unsigned char buf[512];
    unsigned char *bufptr = buf;
    unsigned char c;

    if (!f)
        return FALSE;          /* error opening it, so not a text file */
    i = n = fread (buf, 1, sizeof(buf), f);
    fclose(f);
    if (n <= 0)
        return FALSE;          /* empty file or error, not a text file */
    if (compressSignature(buf) >= 0)
        return FALSE;
    while (i--) {
        c = *bufptr++;
        if (c == '\n' || c == '\r')
            lfctr=0;
        else /* allow BEL BS HT LF VT FF CR EOF ESC control characters */
        {
#ifdef EBCDIC
	    if (iscntrl(c) && c!=BEL && c!=BS && c!=HT && c!=LF && c!=VT && c!=FF && c!=CR && c!=EOF && c!=ESC)
#else
            if (c < '\007' || (c > '\r' && c < ' ' && c != '\032' && c != '\033'))
#endif
                return FALSE;  /* not a text file */
            lfctr++;
/*          if (lfctr>132) return FALSE; /* line too long. Not a text file */
        }
    }
    return TRUE;
}                               /* is_text_file */

VOID *xmalloc(unsigned size)
{
    VOID *p;
    if (size == 0)
	++size;
    p = malloc(size);
    if (p == NULL) {
	fprintf(stderr, LANG("\n\007Out of memory.\n"));
	exitPGP(1);
    }
    return p;
}

/*----------------------------------------------------------------------
 *	temporary file routines
 */


#define MAXTMPF 8

#define	TMP_INUSE	2

static struct {
    char path[MAX_PATH];
    int flags;
    int num;
} tmpf[MAXTMPF];

static char tmpdir[256];	/* temporary file directory */
static char outdir[256];	/* output directory */
static char tmpbasename[64] = "pgptemp";	/* basename for
						   temporary files */


/*
 * set directory for temporary files.  path will be stored in
 * tmpdir[] with an appropriate trailing path separator.
 */
void settmpdir(char *path)
{
    char *p;

    if (path == NULL || *path == '\0') {
	tmpdir[0] = '\0';
	return;
    }
    strcpy(tmpdir, path);
    p = tmpdir + strlen(tmpdir) - 1;
#ifdef MACTC5
	if (*p != '/' && *p != '\\' && *p != ']' && *p != ':')
	{	/* append path separator, either / or \ */
		if ((p = strchr(tmpdir, '/')) == NULL &&
			(p = strchr(tmpdir, '\\')) == NULL &&
			(p = strchr(tmpdir, ':')) == NULL)
			p = ":";	/* path did not contain / or \ or :, use : */
		strncat(tmpdir, p, 1);
#else
    if (*p != '/' && *p != '\\' && *p != ']' && *p != ':') {
	/* append path separator, either / or \ */
	if ((p = strchr(tmpdir, '/')) == NULL &&
	    (p = strchr(tmpdir, '\\')) == NULL)
	    p = "/";		/* path did not contain / or \, use / */
	strncat(tmpdir, p, 1);
#endif
    }
}

/*
 * set output directory to avoid a file copy when temp file is renamed to
 * output file.  the argument filename must be a valid path for a file, not
 * a directory.
 */
void setoutdir(char *filename)
{
    char *p;

    if (filename == NULL) {
	strcpy(outdir, tmpdir);
	return;
    }
    strcpy(outdir, filename);
    p = file_tail(outdir);
    strcpy(tmpbasename, p);
    *p = '\0';
    drop_extension(tmpbasename);
#if !defined(BSD42) && !defined(BSD43) && !defined(sun)
    /*
     *  we don't depend on pathconf here, if it returns an incorrect value
     * for NAME_MAX (like Linux 0.97 with minix FS) finding a unique name
     * for temp files can fail.
     */
    tmpbasename[10] = '\0';	/* 14 char limit */
#endif
}

/*
 * return a unique temporary file name
 */
char *tempfile(int flags)
{
    int i, j;
    int num;
    int fd;
#ifndef UNIX
    FILE *fp;
#endif

    for (i = 0; i < MAXTMPF; ++i)
	if (tmpf[i].flags == 0)
	    break;

    if (i == MAXTMPF) {
	/* message only for debugging, no need for LANG */
	fprintf(stderr, "\n\007Out of temporary files\n");
	return NULL;
    }
  again:
    num = 0;
    do {
	for (j = 0; j < MAXTMPF; ++j)
	    if (tmpf[j].flags && tmpf[j].num == num)
		break;
	if (j < MAXTMPF)
	    continue;		/* sequence number already in use */
	sprintf(tmpf[i].path, "%s%s.%c%02d",
		((flags & TMP_TMPDIR) && *tmpdir ? tmpdir : outdir),
		tmpbasename, TMP_EXT, num);
	if (!file_exists(tmpf[i].path))
	    break;
    }
    while (++num < 100);

    if (num == 100) {
	fprintf(pgpout, "\n\007tempfile: cannot find unique name\n");
	return NULL;
    }
#if defined(UNIX) || defined(VMS)
    if ((fd = open(tmpf[i].path, O_EXCL | O_RDWR | O_CREAT, 0600)) != -1)
	close(fd);
#else
    if ((fp = fopen(tmpf[i].path, "w")) != NULL)
	fclose(fp);
    fd = (fp == NULL ? -1 : 0);
#endif

    if (fd == -1) {
	if (!(flags & TMP_TMPDIR)) {
	    flags |= TMP_TMPDIR;
	    goto again;
	}
#ifdef UNIX
	else if (tmpdir[0] == '\0') {
	    strcpy(tmpdir, "/tmp/");
	    goto again;
	}
#endif
    }
    if (fd == -1) {
	fprintf(pgpout, LANG("\n\007Cannot create temporary file '%s'\n"),
		tmpf[i].path);
	user_error();
    }
#if defined(VMS) || defined(C370)
    remove(tmpf[i].path);
#endif

    tmpf[i].num = num;
    tmpf[i].flags = flags | TMP_INUSE;
    if (verbose)
	fprintf(pgpout, "tempfile: created '%s'\n", tmpf[i].path);
    return tmpf[i].path;
}				/* tempfile */

/*
 * remove temporary file, wipe if necessary.
 */
void rmtemp(char *name)
{
    int i;

    for (i = 0; i < MAXTMPF; ++i)
	if (tmpf[i].flags && strcmp(tmpf[i].path, name) == 0)
	    break;

    if (i < MAXTMPF) {
	if (strlen(name) > 3 && name[strlen(name) - 3] == TMP_EXT) {
	    /* only remove file if name hasn't changed */
	    if (verbose)
		fprintf(pgpout, "rmtemp: removing '%s'\n", name);
	    if (tmpf[i].flags & TMP_WIPE)
		wipefile(name);
	    if (!remove(name)) {
		tmpf[i].flags = 0;
	    } else if (verbose) {
		fprintf(stderr, "\nrmtemp: Failed to remove %s", name);
		perror("\nError");
	    }
	} else if (verbose)
	    fprintf(pgpout, "rmtemp: not removing '%s'\n", name);
    }
}				/* rmtemp */


/*
 * make temporary file permanent, returns the new name.
 */
char *savetemp(char *name, char *newname)
{
    int i, overwrite;

    if (strcmp(name, newname) == 0)
	return name;

    for (i = 0; i < MAXTMPF; ++i)
	if (tmpf[i].flags && strcmp(tmpf[i].path, name) == 0)
	    break;

    if (i < MAXTMPF) {
	if (strlen(name) < 4 || name[strlen(name) - 3] != TMP_EXT) {
	    if (verbose)
		fprintf(pgpout, "savetemp: not renaming '%s' to '%s'\n",
			name, newname);
	    return name;	/* return original file name */
	}
    }

    newname = ck_dup_output(newname, FALSE, TRUE);
    if (newname==NULL)
        return(NULL);

    if (verbose)
	fprintf(pgpout, "savetemp: renaming '%s' to '%s'\n", name, newname);
    if (rename2(name, newname) < 0) {
	/* errorLvl = UNKNOWN_FILE_ERROR; */
	fprintf(pgpout, LANG("Can't create output file '%s'\n"), newname);
	return NULL;
    }
    if (i < MAXTMPF)
	tmpf[i].flags = 0;
    return newname;
} /* savetemp */

char *ck_dup_output(char *newname, boolean notest, boolean delete_dup)
{
    int overwrite;
    static char buf[MAX_PATH];

    while (file_exists(newname)) {
	if (batchmode && !force_flag) {
            fprintf(pgpout,LANG("\n\007Output file '%s' already exists.\n"),
                    newname);
	    return NULL;
	}
	if (is_regular_file(newname)) {	
	    if (force_flag) {
		/* remove without asking */
		if (delete_dup) remove(newname);
		break;
	    }
	    fprintf(pgpout,
	    LANG("\n\007Output file '%s' already exists.  Overwrite (y/N)? "),
		 newname);
	    overwrite = getyesno('n');
	} else {
	    fprintf(pgpout,
		    LANG("\n\007Output file '%s' already exists.\n"),newname);
	    if (force_flag)	/* never remove special file */
		return NULL;
	    overwrite = FALSE;
	}

	if (!overwrite) {
	    fprintf(pgpout, "\n");
	    fprintf(pgpout, LANG("Enter new file name:"));
	    fprintf(pgpout, " ");
#ifdef MACTC5
			if (!GetFilePath(LANG("Enter new file name:"), buf, PUTFILE))
				return(NULL);
			strcpy(newname, buf);
			fprintf(pgpout, "%s\n",buf);
#else
	    getstring(buf, MAX_PATH - 1, TRUE);
	    if (buf[0] == '\0')
		return(NULL);
	    newname = buf;
#endif
	} else if (delete_dup)
            remove(newname);
        else
            break;

        if (notest) break;
    }
    return(newname);
} /* ck_dup_output */

/*
 * like savetemp(), only make backup of destname if it exists
 */
int savetempbak(char *tmpname, char *destname)
{
    char bakpath[MAX_PATH];
#ifdef MACTC5
	byte header[8];
#endif
#ifdef UNIX
    int mode = -1;
#endif

    if (is_tempfile(destname)) {
	remove(destname);
    } else {
	if (file_exists(destname)) {
#ifdef UNIX
	    struct stat st;
	    if (stat(destname, &st) != -1)
		mode = st.st_mode & 07777;
#endif
	    strcpy(bakpath, destname);
	    force_extension(bakpath, BAK_EXTENSION);
	    remove(bakpath);
#if defined(VMS) || defined(C370)
	    if (rename(destname, bakpath) != 0)
#else
	    if (rename(destname, bakpath) == -1)
#endif
		return -1;
#ifdef MACTC5
		get_header_info_from_file(bakpath, header, 8 );
		if (header[0] == CTB_CERT_SECKEY)
			PGPSetFinfo(bakpath,'SKey','MPGP');
		if (header[0] == CTB_CERT_PUBKEY)
			PGPSetFinfo(bakpath,'PKey','MPGP');
#endif
	}
    }
    if (savetemp(tmpname, destname) == NULL)
	return -1;
#if defined(UNIX)
    if (mode != -1)
	chmod(destname, mode);
#elif defined(MACTC5)
	get_header_info_from_file(destname, header, 8 );
	if (header[0] == CTB_CERT_SECKEY)
		PGPSetFinfo(destname,'SKey','MPGP');
	if (header[0] == CTB_CERT_PUBKEY)
		PGPSetFinfo(destname,'PKey','MPGP');
#endif
    return 0;
}

/*
 * remove all temporary files and wipe them if necessary
 */
void cleanup_tmpf(void)
{
    int i;

    for (i = 0; i < MAXTMPF; ++i)
	if (tmpf[i].flags)
	    rmtemp(tmpf[i].path);
}				/* cleanup_tmpf */

#ifdef MACTC5
void mac_cleanup_tmpf(void)
{
	int i,err;
    HFileParam pb;
    char fname[256];
	for (i = 0; i < MAXTMPF; ++i)
		if (tmpf[i].flags)
	       {
	        strcpy(fname,tmpf[i].path);
	        pb.ioCompletion=nil;
	        c2pstr(fname);
	        pb.ioNamePtr=(uchar *)fname;
	        pb.ioVRefNum=0;
	        pb.ioFDirIndex=0;
	        pb.ioFRefNum=0;
	        pb.ioDirID=0;
	        err=PBHGetFInfo((HParmBlkPtr)&pb,false);
	        if (pb.ioFRefNum!=0){
	            strcpy(fname,tmpf[i].path);
	            pb.ioCompletion=nil;
	            c2pstr(fname);
	            pb.ioNamePtr=(uchar *)fname;
	            pb.ioVRefNum=0;
	            pb.ioDirID=0;
	            err=PBClose((ParmBlkPtr)&pb,false);
	            }
			rmtemp(tmpf[i].path);
			}
}	/* mac_cleanup_tmpf */
#endif

/* 
 * Routines to search for the manuals.
 *
 * Why all this code?
 *
 * Some people may object to PGP insisting on finding the manual somewhere
 * in the neighborhood to generate a key.  They bristle against this
 * seemingly authoritarian attitude.  Some people have even modified PGP
 * to defeat this feature, and redistributed their hotwired version to
 * others.  That creates problems for me (PRZ).
 * 
 * Here is the problem.  Before I added this feature, there were maimed
 * versions of the PGP distribution package floating around that lacked
 * the manual.  One of them was uploaded to Compuserve, and was
 * distributed to countless users who called me on the phone to ask me why
 * such a complicated program had no manual.  It spread out to BBS systems
 * around the country.  And a freeware distributor got hold of the package
 * from Compuserve and enshrined it on CD-ROM, distributing thousands of
 * copies without the manual.  What a mess.
 * 
 * Please don't make my life harder by modifying PGP to disable this
 * feature so that others may redistribute PGP without the manual.  If you
 * run PGP on a palmtop with no memory for the manual, is it too much to
 * ask that you type one little extra word on the command line to do a key
 * generation, a command that is seldom used by people who already know
 * how to use PGP?  If you can't stand even this trivial inconvenience,
 * can you suggest a better method of reducing PGP's distribution without
 * the manual?
 */

static unsigned ext_missing(char *prefix)
{
    static char const *const extensions[] =
#ifdef VMS
	{ ".doc", ".txt", ".man", ".tex", ".", 0 };
#else
	{ ".doc", ".txt", ".man", ".tex", "", 0 };
#endif
    char const *const *p;
    char *end = prefix + strlen(prefix);

    for (p = extensions; *p; p++) {
	strcpy(end, *p);
#if 0				/* Debugging code */
	fprintf(pgpout, "Looking for \"%s\"\n", prefix);
#endif
	if (file_exists(prefix))
	    return 0;
    }
    return 1;
}

/*
 * Returns mask of files missing
 */
static unsigned files_missing(char *prefix)
{
/* This changed to incorporate the changes in the Documentation subdirectory */
#ifdef MACTC5
	static char const *const names[] = 
	{"Volume I", "Volume II", 0};
#else
    static char const *const names[] =
    {"pgpdoc1", "pgpdoc2", 0};
#endif
    char const *const *p;
    unsigned bit, mask = 3;
    int len = strlen(prefix);

#ifndef MACTC5
	/* Cannot do this on the macintosh because file_exists returns false on
	   directories */
#ifndef VMS
    /*
     * Optimization: if directory doesn't exist, stop.  But access()
     * (used internally by file_exists()) doesn't work on dirs under VMS.
     */
    if (prefix[0] && !file_exists(prefix))	/* Directory doesn't exist? */
	return mask;
#endif /* VMS */
#endif /* MACTC5 */
    if (len && strchr(DIRSEPS, prefix[len - 1]) == 0)
	prefix[len++] = DIRSEPS[0];
    for (p = names, bit = 1; *p; p++, bit <<= 1) {
	strcpy(prefix + len, *p);
	if (!ext_missing(prefix))
	    mask &= ~bit;
    }

    return mask;		/* Bitmask of which files exist */
}

/*
 * Search prefix directory and doc subdirectory.
 */
static unsigned doc_missing(char *prefix)
{
    unsigned mask;
    int len = strlen(prefix);

    mask = files_missing(prefix);
    if (!mask)
	return 0;
#if defined(VMS)
    if (len && prefix[len - 1] == ']') {
	strcpy(prefix + len - 1, ".doc]");
    } else {
	assert(!len || prefix[len - 1] == ':');
	strcpy(prefix + len, "[doc]");
    }
#elif defined(MACTC5)
	/* on the macintosh we must look for the documents in 
	   Documentation:PGP User's Guide: folder  */
	if (len && prefix[len - 1] != DIRSEPS[0])
		prefix[len++] = DIRSEPS[0];
	strcpy(prefix + len, "Documentation");
	len = strlen(prefix);
	mask &= files_missing(prefix);
	if (!mask)
		return 0;
	if (len && prefix[len - 1] != DIRSEPS[0])
		prefix[len++] = DIRSEPS[0];
	strcpy(prefix + len, "PGP User's Guide");
	mask &= files_missing(prefix);
	if (!mask)
		return 0;
#else
    if (len && prefix[len - 1] != DIRSEPS[0])
	prefix[len++] = DIRSEPS[0];
    strcpy(prefix + len, "doc");
#endif

    mask &= files_missing(prefix);

    prefix[len] = '\0';
    return mask;
}

/*
 * Expands a leading environment variable.  Returns 0 on success;
 * <0 if there is an error.
 */
static int expand_env(char const *src, char *dest)
{
    char const *var, *suffix;
    unsigned len;

    if (*src != '$') {
	strcpy(dest, src);
	return 0;
    }
    /* Find end of variable */
    if (src[1] == '{') {	/* ${FOO} form */
	var = src + 2;
	len = strchr(var, '}') - (char*) var;
	suffix = src + 2 + len + 1;
    } else {			/* $FOO form - allow $ for VMS */
	var = src + 1;
	len = strspn(var, "ABCDEFGHIJKLMNOPQRSTUVWXYZ$_");
	suffix = src + 1 + len;
    }

    memcpy(dest, var, len);	/* Copy name */
    dest[len] = '\0';		/* Null-terminate */

    var = getenv(dest);
    if (!var || !*var)
	return -1;		/* No env variable */

    /* Copy expanded form to destination */
    strcpy(dest, var);

    /* Add tail */
    strcat(dest, suffix);

    return 0;
}

/* Don't forget to change 'pgp26' whenever you update rel_version past 2.6 */
char const *const manual_dirs[] =
{
#if defined(VMS)
    "$PGPPATH", "", "[pgp]", "[pgp26]", "[pgp263]",
    PGP_SYSTEM_DIR, "SYS$LOGIN:", "SYS$LOGIN:[pgp]",
    "SYS$LOGIN:[pgp26]", "SYS$LOGIN:[pgp263]", "[-]",
#elif defined(UNIX)
    "$PGPPATH", "", "pgp", "pgp26", "pgp263", PGP_SYSTEM_DIR,
    "$HOME/.pgp", "$HOME", "$HOME/pgp", "$HOME/pgp26", "..",
#elif defined(AMIGA)
    "$PGPPATH", "", "pgp", "pgp26", ":pgp", ":pgp26", ":pgp263", 
    ":", "/",
#else				/* MSDOS or ATARI */
    "$PGPPATH", "", "pgp", "pgp26", "\\pgp", "\\pgp26", "\\pgp263", 
    "\\", "..", "c:\\pgp", "c:\\pgp26",
#endif
    0};

#ifdef MACTC5
extern char appPathName[];
#endif

unsigned manuals_missing(void)
{
    char buf[256];
    unsigned mask = ~((unsigned)0);
    char const *const *p;

#ifdef MACTC5
	strcpy(buf, appPathName);
	mask &= doc_missing(buf);
	return mask;
#endif /* MACTC5 */
    for (p = manual_dirs; *p; p++) {
	if (expand_env(*p, buf) < 0)
	    continue;		/* Ignore */
	mask &= doc_missing(buf);
	if (!mask)
	    break;
    }

    return mask;
}

/* 
 * Why all this code?
 *
 * See block of comments above.
 */


/* ==== genprime.c ==== */
/* genprime.c - C source code for generation of large primes
   used by public-key key generation routines.
   First version 17 Mar 87
   Last revised 2 Jun 91 by PRZ
   24 Apr 93 by CP

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.

   These functions are for the generation of large prime integers and
   for other functions related to factoring and key generation for 
   many number-theoretic cryptographic algorithms, such as the NIST 
   Digital Signature Standard.
 */

#define SHOWPROGRESS

/* Define some error status returns for keygen... */
#define NOPRIMEFOUND -14	/* slowtest probably failed */
#define NOSUSPECTS -13		/* fastsieve probably failed */


#if defined(MSDOS) || defined(WIN32)
#define poll_for_break() {while (kbhit()) getch();}
#endif

#ifndef poll_for_break
#define poll_for_break()	/* stub */
#endif

#ifdef SHOWPROGRESS
#include <stdio.h>		/* needed for putchar() */
#endif

#ifdef MACTC5
extern int  Putchar(int c);
#undef putchar
#define putchar Putchar
#endif

#ifdef EMBEDDED			/* compiling for embedded target */
#define _NOMALLOC		/* defined if no malloc is available. */
#endif				/* EMBEDDED */

/* Decide whether malloc is available.  Some embedded systems lack it. */
#ifndef _NOMALLOC		/* malloc library routine available */
#include <stdlib.h>		/* ANSI C library - for malloc() and free() */
/* #include <alloc.h> *//* Borland Turbo C has malloc in <alloc.h> */
#endif				/* malloc available */

#include "mpilib.h"
#include "genprime.h"
#if (defined(MSDOS) && !defined(__GO32__)) || defined(WIN32)
#include <conio.h>
#endif

#include "random.h"


/* #define STRONGPRIMES *//* if defined, generate "strong" primes for key */
/*
 *"Strong" primes are no longer advantageous, due to the new 
 * elliptical curve method of factoring.  Randomly selected primes 
 * are as good as any.  See "Factoring", by Duncan A. Buell, Journal 
 * of Supercomputing 1 (1987), pages 191-216.
 * This justifies disabling the lengthy search for strong primes.
 *
 * The advice about strong primes in the early RSA literature applies
 * to 256-bit moduli where the attacks were the Pollard rho and P-1
 * factoring algorithms.  Later developments in factoring have entirely
 * supplanted these methods.  The later algorithms are always faster
 * (so we need bigger primes), and don't care about STRONGPRIMES.
 *
 * The early literature was saying that you can get away with small
 * moduli if you choose the primes carefully.  The later developments
 * say you can't get away with small moduli, period.  And it doesn't
 * matter how you choose the primes.
 *
 * It's just taking a heck of a long time for the advice on "strong primes"
 * to disappear from the books.  Authors keep going back to the original
 * documents and repeating what they read there, even though it's out
 * of date.
 */

#define BLUM
/* If BLUM is defined, this looks for prines congruent to 3 modulo 4.
   The product of two of these is a Blum integer.  You can uniquely define
   a square root Cmodulo a Blum integer, which leads to some extra
   possibilities for encryption algorithms.  This shrinks the key space by
   2 bits, which is not considered significant.
 */

#ifdef STRONGPRIMES

static boolean primetest(unitptr p);
	/* Returns TRUE iff p is a prime. */

static int mp_sqrt(unitptr quotient, unitptr dividend);
	/* Quotient is returned as the square root of dividend. */

#endif

static int nextprime(unitptr p);
	/* Find next higher prime starting at p, returning result in p. */

static void randombits(unitptr p, short nbits);
	/* Make a random unit array p with nbits of precision. */

#ifdef DEBUG
#define DEBUGprintf1(x) printf(x)
#define DEBUGprintf2(x,y) printf(x,y)
#define DEBUGprintf3(x,y,z) printf(x,y,z)
#else
#define DEBUGprintf1(x)
#define DEBUGprintf2(x,y)
#define DEBUGprintf3(x,y,z)
#endif


/*      primetable is a table of 16-bit prime numbers used for sieving 
   and for other aspects of public-key cryptographic key generation */

static word16 primetable[] =
{
    2, 3, 5, 7, 11, 13, 17, 19,
    23, 29, 31, 37, 41, 43, 47, 53,
    59, 61, 67, 71, 73, 79, 83, 89,
    97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173,
    179, 181, 191, 193, 197, 199, 211, 223,
    227, 229, 233, 239, 241, 251, 257, 263,
    269, 271, 277, 281, 283, 293, 307, 311,
#ifndef EMBEDDED		/* not embedded, use larger table */
    313, 317, 331, 337, 347, 349, 353, 359,
    367, 373, 379, 383, 389, 397, 401, 409,
    419, 421, 431, 433, 439, 443, 449, 457,
    461, 463, 467, 479, 487, 491, 499, 503,
    509, 521, 523, 541, 547, 557, 563, 569,
    571, 577, 587, 593, 599, 601, 607, 613,
    617, 619, 631, 641, 643, 647, 653, 659,
    661, 673, 677, 683, 691, 701, 709, 719,
    727, 733, 739, 743, 751, 757, 761, 769,
    773, 787, 797, 809, 811, 821, 823, 827,
    829, 839, 853, 857, 859, 863, 877, 881,
    883, 887, 907, 911, 919, 929, 937, 941,
    947, 953, 967, 971, 977, 983, 991, 997,
    1009, 1013, 1019, 1021, 1031, 1033, 1039, 1049,
    1051, 1061, 1063, 1069, 1087, 1091, 1093, 1097,
    1103, 1109, 1117, 1123, 1129, 1151, 1153, 1163,
    1171, 1181, 1187, 1193, 1201, 1213, 1217, 1223,
    1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283,
    1289, 1291, 1297, 1301, 1303, 1307, 1319, 1321,
    1327, 1361, 1367, 1373, 1381, 1399, 1409, 1423,
    1427, 1429, 1433, 1439, 1447, 1451, 1453, 1459,
    1471, 1481, 1483, 1487, 1489, 1493, 1499, 1511,
    1523, 1531, 1543, 1549, 1553, 1559, 1567, 1571,
    1579, 1583, 1597, 1601, 1607, 1609, 1613, 1619,
    1621, 1627, 1637, 1657, 1663, 1667, 1669, 1693,
    1697, 1699, 1709, 1721, 1723, 1733, 1741, 1747,
    1753, 1759, 1777, 1783, 1787, 1789, 1801, 1811,
    1823, 1831, 1847, 1861, 1867, 1871, 1873, 1877,
    1879, 1889, 1901, 1907, 1913, 1931, 1933, 1949,
    1951, 1973, 1979, 1987, 1993, 1997, 1999, 2003,
    2011, 2017, 2027, 2029, 2039, 2053, 2063, 2069,
    2081, 2083, 2087, 2089, 2099, 2111, 2113, 2129,
    2131, 2137, 2141, 2143, 2153, 2161, 2179, 2203,
    2207, 2213, 2221, 2237, 2239, 2243, 2251, 2267,
    2269, 2273, 2281, 2287, 2293, 2297, 2309, 2311,
    2333, 2339, 2341, 2347, 2351, 2357, 2371, 2377,
    2381, 2383, 2389, 2393, 2399, 2411, 2417, 2423,
    2437, 2441, 2447, 2459, 2467, 2473, 2477, 2503,
    2521, 2531, 2539, 2543, 2549, 2551, 2557, 2579,
    2591, 2593, 2609, 2617, 2621, 2633, 2647, 2657,
    2659, 2663, 2671, 2677, 2683, 2687, 2689, 2693,
    2699, 2707, 2711, 2713, 2719, 2729, 2731, 2741,
    2749, 2753, 2767, 2777, 2789, 2791, 2797, 2801,
    2803, 2819, 2833, 2837, 2843, 2851, 2857, 2861,
    2879, 2887, 2897, 2903, 2909, 2917, 2927, 2939,
    2953, 2957, 2963, 2969, 2971, 2999, 3001, 3011,
    3019, 3023, 3037, 3041, 3049, 3061, 3067, 3079,
    3083, 3089, 3109, 3119, 3121, 3137, 3163, 3167,
    3169, 3181, 3187, 3191, 3203, 3209, 3217, 3221,
    3229, 3251, 3253, 3257, 3259, 3271, 3299, 3301,
    3307, 3313, 3319, 3323, 3329, 3331, 3343, 3347,
    3359, 3361, 3371, 3373, 3389, 3391, 3407, 3413,
    3433, 3449, 3457, 3461, 3463, 3467, 3469, 3491,
    3499, 3511, 3517, 3527, 3529, 3533, 3539, 3541,
    3547, 3557, 3559, 3571, 3581, 3583, 3593, 3607,
    3613, 3617, 3623, 3631, 3637, 3643, 3659, 3671,
    3673, 3677, 3691, 3697, 3701, 3709, 3719, 3727,
    3733, 3739, 3761, 3767, 3769, 3779, 3793, 3797,
    3803, 3821, 3823, 3833, 3847, 3851, 3853, 3863,
    3877, 3881, 3889, 3907, 3911, 3917, 3919, 3923,
    3929, 3931, 3943, 3947, 3967, 3989, 4001, 4003,
    4007, 4013, 4019, 4021, 4027, 4049, 4051, 4057,
    4073, 4079, 4091, 4093, 4099, 4111, 4127, 4129,
    4133, 4139, 4153, 4157, 4159, 4177, 4201, 4211,
    4217, 4219, 4229, 4231, 4241, 4243, 4253, 4259,
    4261, 4271, 4273, 4283, 4289, 4297, 4327, 4337,
    4339, 4349, 4357, 4363, 4373, 4391, 4397, 4409,
    4421, 4423, 4441, 4447, 4451, 4457, 4463, 4481,
    4483, 4493, 4507, 4513, 4517, 4519, 4523, 4547,
    4549, 4561, 4567, 4583, 4591, 4597, 4603, 4621,
    4637, 4639, 4643, 4649, 4651, 4657, 4663, 4673,
    4679, 4691, 4703, 4721, 4723, 4729, 4733, 4751,
    4759, 4783, 4787, 4789, 4793, 4799, 4801, 4813,
    4817, 4831, 4861, 4871, 4877, 4889, 4903, 4909,
    4919, 4931, 4933, 4937, 4943, 4951, 4957, 4967,
    4969, 4973, 4987, 4993, 4999, 5003, 5009, 5011,
    5021, 5023, 5039, 5051, 5059, 5077, 5081, 5087,
    5099, 5101, 5107, 5113, 5119, 5147, 5153, 5167,
    5171, 5179, 5189, 5197, 5209, 5227, 5231, 5233,
    5237, 5261, 5273, 5279, 5281, 5297, 5303, 5309,
    5323, 5333, 5347, 5351, 5381, 5387, 5393, 5399,
    5407, 5413, 5417, 5419, 5431, 5437, 5441, 5443,
    5449, 5471, 5477, 5479, 5483, 5501, 5503, 5507,
    5519, 5521, 5527, 5531, 5557, 5563, 5569, 5573,
    5581, 5591, 5623, 5639, 5641, 5647, 5651, 5653,
    5657, 5659, 5669, 5683, 5689, 5693, 5701, 5711,
    5717, 5737, 5741, 5743, 5749, 5779, 5783, 5791,
    5801, 5807, 5813, 5821, 5827, 5839, 5843, 5849,
    5851, 5857, 5861, 5867, 5869, 5879, 5881, 5897,
    5903, 5923, 5927, 5939, 5953, 5981, 5987, 6007,
    6011, 6029, 6037, 6043, 6047, 6053, 6067, 6073,
    6079, 6089, 6091, 6101, 6113, 6121, 6131, 6133,
    6143, 6151, 6163, 6173, 6197, 6199, 6203, 6211,
    6217, 6221, 6229, 6247, 6257, 6263, 6269, 6271,
    6277, 6287, 6299, 6301, 6311, 6317, 6323, 6329,
    6337, 6343, 6353, 6359, 6361, 6367, 6373, 6379,
    6389, 6397, 6421, 6427, 6449, 6451, 6469, 6473,
    6481, 6491, 6521, 6529, 6547, 6551, 6553, 6563,
    6569, 6571, 6577, 6581, 6599, 6607, 6619, 6637,
    6653, 6659, 6661, 6673, 6679, 6689, 6691, 6701,
    6703, 6709, 6719, 6733, 6737, 6761, 6763, 6779,
    6781, 6791, 6793, 6803, 6823, 6827, 6829, 6833,
    6841, 6857, 6863, 6869, 6871, 6883, 6899, 6907,
    6911, 6917, 6947, 6949, 6959, 6961, 6967, 6971,
    6977, 6983, 6991, 6997, 7001, 7013, 7019, 7027,
    7039, 7043, 7057, 7069, 7079, 7103, 7109, 7121,
    7127, 7129, 7151, 7159, 7177, 7187, 7193, 7207,
    7211, 7213, 7219, 7229, 7237, 7243, 7247, 7253,
    7283, 7297, 7307, 7309, 7321, 7331, 7333, 7349,
    7351, 7369, 7393, 7411, 7417, 7433, 7451, 7457,
    7459, 7477, 7481, 7487, 7489, 7499, 7507, 7517,
    7523, 7529, 7537, 7541, 7547, 7549, 7559, 7561,
    7573, 7577, 7583, 7589, 7591, 7603, 7607, 7621,
    7639, 7643, 7649, 7669, 7673, 7681, 7687, 7691,
    7699, 7703, 7717, 7723, 7727, 7741, 7753, 7757,
    7759, 7789, 7793, 7817, 7823, 7829, 7841, 7853,
    7867, 7873, 7877, 7879, 7883, 7901, 7907, 7919,
    7927, 7933, 7937, 7949, 7951, 7963, 7993, 8009,
    8011, 8017, 8039, 8053, 8059, 8069, 8081, 8087,
    8089, 8093, 8101, 8111, 8117, 8123, 8147, 8161,
    8167, 8171, 8179, 8191,
#endif				/* not EMBEDDED, use larger table */
    0};			/* null-terminated list, with only one null at end */



#ifdef UNIT8
static word16 bottom16(unitptr r)
/* Called from nextprime and primetest.  Returns low 16 bits of r. */
{
    make_lsbptr(r, (global_precision - ((2 / BYTES_PER_UNIT) - 1)));
    return *(word16 *) r;
}				/* bottom16 */
#else				/* UNIT16 or UNIT32 */
#define bottom16(r) ((word16) lsunit(r))
/* or UNIT32 could mask off lower 16 bits, instead of typecasting it. */
#endif				/* UNIT16 or UNIT32 */


/*
 * This routine tests p for primality by applying Fermat's theorem:
 * For any x, if ((x**(p-1)) mod p) != 1, then p is not prime.
 * By trying a few values for x, we can determine if p is "probably" prime.
 *
 * Because this test is so slow, it is recommended that p be sieved first
 * to weed out numbers that are obviously not prime.
 *
 * Contrary to what you may have read in the literature, empirical evidence
 * shows this test weeds out a LOT more than 50% of the composite candidates
 * for each trial x.  Each test catches nearly all the composites.
 *
 * Some people have questioned whether four Fermat tests is sufficient.
 * See "Finding Four Million Large Random Primes", by Ronald Rivest,
 * in Advancess in Cryptology: Proceedings of Crypto '91.  He used a
 * small-divisor test similar to PGP's, then a Fermat test to the base 2,
 * and then 8 iterarions of a Miller-Rabin test.  About 718 million random
 * 256-bit integers were generated, 43,741,404 passed the small divisor test,
 * 4,058,000 passed the Fermat test, and all 4,058,000 passed all 8
 * iterations of the Miller-Rabin test, proving their primality beyond most
 * reasonable doubts.  This is strong experimental evidence that the odds
 * of getting a non-prime are less than one in a million (10^-6).
 *
 * He also gives a theoretical argument that the chances of finding a
 * 256-bit non-prime which satisfies one Fermat test to the base 2 is less
 * than 10^-22.  The small divisor test improves this number, and if the
 * numbers are 512 bits (as needed for a 1024-bit key) the odds of failure
 * shrink to about 10^-44.  Thus, he concludes, for practical purposes one
 * Fermat test to the base 2 is sufficient.
 */
static boolean slowtest(unitptr p)
{
    unit x[MAX_UNIT_PRECISION], is_one[MAX_UNIT_PRECISION];
    unit pminus1[MAX_UNIT_PRECISION];
    short i;

    mp_move(pminus1, p);
    mp_dec(pminus1);

    for (i = 0; i < 4; i++) {	/* Just do a few tests. */
	poll_for_break(); /* polls keyboard, allows ctrl-C to abort program */
	mp_init(x, primetable[i]);	/* Use any old random trial x */
	/* if ((x**(p-1)) mod p) != 1, then p is not prime */
	if (mp_modexp(is_one, x, pminus1, p) < 0)	/* modexp error? */
	    return FALSE;	/* error means return not prime status */
	if (testne(is_one, 1))	/* then p is not prime */
	    return FALSE;	/* return not prime status */
#ifdef SHOWPROGRESS
	putchar('*');		/* let user see how we are progressing */
	fflush(stdout);
#endif				/* SHOWPROGRESS */
    }

    /* If it gets to this point, it's very likely that p is prime */
    mp_burn(x);			/* burn the evidence on the stack... */
    mp_burn(is_one);
    mp_burn(pminus1);
    return TRUE;
}				/* slowtest -- fermattest */


#ifdef STRONGPRIMES

static boolean primetest(unitptr p)
/*
 * Returns TRUE iff p is a prime.
 * If p doesn't pass through the sieve, then p is definitely NOT a prime.
 * If p is small enough for the sieve to prove  primality or not, 
 * and p passes through the sieve, then p is definitely a prime.
 * If p is large and p passes through the sieve and may be a prime,
 * then p is further tested for primality with a slower test.
 */
{
    short i;
    static word16 lastprime = 0;	/* last prime in primetable */
    word16 sqrt_p;	/* to limit sieving past sqrt(p), for small p's */

    if (!lastprime) {		/* lastprime still undefined. So define it. */
	/* executes this code only once, then skips it next time */
	for (i = 0; primetable[i]; i++);	/* seek end of primetable */
	lastprime = primetable[i - 1];	/* get last prime in table */
    }
    if (significance(p) <= (2 / BYTES_PER_UNIT))	/* if p <= 16 bits */
	/* p may be in primetable.  Search it. */
	if (bottom16(p) <= lastprime)
	    for (i = 0; primetable[i]; i++) {
		/* scan until null-terminator */
		if (primetable[i] == bottom16(p))
		    return TRUE;	/* yep, definitely a prime. */
		if (primetable[i] > bottom16(p))	/* we missed. */
		    return FALSE;	/* definitely NOT a prime. */
	    }		/* We got past the whole primetable without a hit. */
    /* p is bigger than any prime in primetable, so let's sieve. */
    if (!(lsunit(p) & 1))	/* if least significant bit is 0... */
	return FALSE;		/* divisible by 2, not prime */

    if (mp_tstminus(p))		/* error if p<0 */
	return FALSE;		/* not prime if p<0 */

    /*
     * Optimization for small (32 bits or less) p's.  
     * If p is small, compute sqrt_p = sqrt(p), or else 
     * if p is >32 bits then just set sqrt_p to something 
     * at least as big as the largest primetable entry.
     */
    if (significance(p) <= (4 / BYTES_PER_UNIT)) {	/* if p <= 32 bits */
	unit sqrtp[MAX_UNIT_PRECISION];
	/* Just sieve up to sqrt(p) */
	if (mp_sqrt(sqrtp, p) == 0)	/* 0 means p is a perfect square */
	    return FALSE;	/* perfect square is not a prime */
	/* we know that sqrtp <= 16 bits because p <= 32 bits */
	sqrt_p = bottom16(sqrtp);
    } else {
	/* p > 32 bits, so obviate sqrt(p) test. */
	sqrt_p = lastprime;	/* ensures that we do ENTIRE sieve. */
    }

    /* p is assumed odd, so begin sieve at 3 */
    for (i = 1; primetable[i]; i++) {
	/* Compute p mod (primetable[i]).  If it divides evenly... */
	if (mp_shortmod(p, primetable[i]) == 0)
	    return FALSE;	/* then p is definitely NOT prime */
	if (primetable[i] > sqrt_p)	/* fully sieved p? */
	    return TRUE; /* yep, fully passed sieve, definitely a prime. */
    }
    /* It passed the sieve, so p is a suspected prime. */

    /*  Now try slow complex primality test on suspected prime. */
    return slowtest(p);		/* returns TRUE or FALSE */
}				/* primetest */

#endif

/*
 * Used in conjunction with fastsieve.  Builds a table of remainders 
 * relative to the random starting point p, so that fastsieve can 
 * sequentially sieve for suspected primes quickly.  Call buildsieve 
 * once, then call fastsieve for consecutive prime candidates.
 * Note that p must be odd, because the sieve begins at 3. 
 */
static void buildsieve(unitptr p, word16 remainders[])
{
    short i;
    for (i = 1; primetable[i]; i++) {
	remainders[i] = mp_shortmod(p, primetable[i]);
    }
}				/* buildsieve */

/*
   Fast prime sieving algorithm by Philip Zimmermann, March 1987.
   This is the fastest algorithm I know of for quickly sieving for 
   large (hundreds of bits in length) "random" probable primes, because 
   it uses only single-precision (16-bit) arithmetic.  Because rigorous 
   prime testing algorithms are very slow, it is recommended that 
   potential prime candidates be quickly passed through this fast 
   sieving algorithm first to weed out numbers that are obviously not 
   prime.

   This algorithm is optimized to search sequentially for a large prime 
   from a random starting point.  For generalized nonsequential prime 
   testing, the slower  conventional sieve should be used, as given 
   in primetest(p).

   This algorithm requires a fixed table (called primetable) of the 
   first hundred or so small prime numbers. 
   First we select a random odd starting point (p) for our prime 
   search.  Then we build a table of 16-bit remainders calculated 
   from that initial p.  This table of 16-bit remainders is exactly 
   the same length as the table of small 16-bit primes.  Each 
   remainders table entry contains the remainder of p divided by the 
   corresponding primetable entry.  Then we begin sequentially testing 
   all odd integers, starting from the initial odd random p.  The 
   distance we have searched from the huge random starting point p is 
   a small 16-bit number, pdelta.  If pdelta plus a remainders table 
   entry is evenly divisible by the corresponding primetable entry, 
   then p+pdelta is factorable by that primetable entry, which means 
   p+pdelta is not prime.
 */

/*      Fastsieve is used for searching sequentially from a random starting
   point for a suspected prime.  Requires that buildsieve be called 
   first, to build a table of remainders relative to the random starting 
   point p.  
   Returns true iff pdelta passes through the sieve and thus p+pdelta 
   may be a prime.  Note that p must be odd, because the sieve begins 
   at 3.
 */
static boolean fastsieve(word16 pdelta, word16 remainders[])
{
    short i;
    for (i = 1; primetable[i]; i++) {
	/*
	 * If pdelta plus a remainders table entry is evenly 
	 * divisible by the corresponding primetable entry,
	 * then p+pdelta is factorable by that primetable entry, 
	 * which means p+pdelta is not prime.
	 */
	if ((pdelta + remainders[i]) % primetable[i] == 0)
	    return FALSE;	/* then p+pdelta is not prime */
    }
    /* It passed the sieve.  It is now a suspected prime. */
    return TRUE;
}				/* fastsieve */


#define numberof(x) (sizeof(x)/sizeof(x[0]))	/* number of table entries */


static int nextprime(unitptr p)
/*
 * Find next higher prime starting at p, returning result in p. 
 * Uses fast prime sieving algorithm to search sequentially.
 * Returns 0 for normal completion status, < 0 for failure status.
 */
{
    word16 pdelta, range;
    short oldprecision;
    short i, suspects;

    /* start search at candidate p */
    mp_inc(p);	/* remember, it's the NEXT prime from p, noninclusive. */
    if (significance(p) <= 1) {
	/*
	 * p might be smaller than the largest prime in primetable.
	 * We can't sieve for primes that are already in primetable.
	 * We will have to directly search the table.
	 */
	/* scan until null-terminator */
	for (i = 0; primetable[i]; i++) {
	    if (primetable[i] >= lsunit(p)) {
		mp_init(p, primetable[i]);
		return 0;	/* return next higher prime from primetable */
	    }
	}		/* We got past the whole primetable without a hit. */
    }	      /* p is bigger than any prime in primetable, so let's sieve. */
    if (mp_tstminus(p)) {	/* error if p<0 */
	mp_init(p, 2);		/* next prime >0 is 2 */
	return 0;		/* normal completion status */
    }
#ifndef BLUM
    lsunit(p) |= 1;		/* set candidate's lsb - make it odd */
#else
    lsunit(p) |= 3;		/* Make candidate ==3 mod 4 */
#endif

    /* Adjust the global_precision downward to the optimum size for p... */
    oldprecision = global_precision;	/* save global_precision */
    /* We will need 2-3 extra bits of precision for the falsekeytest. */
    set_precision(bits2units(countbits(p) + 4 + SLOP_BITS));
    /* Rescale p to global_precision we just defined */
    rescale(p, oldprecision, global_precision);

    {
#ifdef _NOMALLOC /* No malloc and free functions available.  Use stack. */
	word16 remainders[numberof(primetable)];
#else			/* malloc available, we can conserve stack space. */
	word16 *remainders;
	/* Allocate some memory for the table of remainders: */
	remainders = (word16 *) malloc(sizeof(primetable));
#endif				/* malloc available */

	/* Build remainders table relative to initial p: */
	buildsieve(p, remainders);
	pdelta = 0;		/* offset from initial random p */
	/* Sieve preparation complete.  Now for some fast fast sieving... */
	/* slowtest will not be called unless fastsieve is true */

	/* range is how far to search before giving up. */
#ifndef BLUM
	range = 4 * units2bits(global_precision);
#else
	/* Twice as many because step size is twice as large, */
	range = 8 * units2bits(global_precision);
#endif
	suspects = 0;	/* number of suspected primes and slowtest trials */
	for (;;) {
	    /* equivalent to:  if (primetest(p)) break; */
	    if (fastsieve(pdelta, remainders)) { /* found suspected prime */
		suspects++;	/* tally for statistical purposes */
#ifdef SHOWPROGRESS
		putchar('.');	/* let user see how we are progressing */
		fflush(stdout);
#endif				/* SHOWPROGRESS */
		if (slowtest(p))
		    break;	/* found a prime */
	    }
#ifndef BLUM
	    pdelta += 2;	/* try next odd number */
#else
	    pdelta += 4;
	    mp_inc(p);
	    mp_inc(p);
#endif
	    mp_inc(p);
	    mp_inc(p);

	    if (pdelta > range)	/* searched too many candidates? */
		break;	/* something must be wrong--bail out of search */

	}			/* while (TRUE) */

#ifdef SHOWPROGRESS
	putchar(' ');		/* let user see how we are progressing */
#endif				/* SHOWPROGRESS */

	for (i = 0; primetable[i]; i++)	/* scan until null-terminator */
	    remainders[i] = 0;	/* don't leave remainders exposed in RAM */
#ifndef _NOMALLOC
	free(remainders);	/* free allocated memory */
#endif				/* not _NOMALLOC */
    }

    set_precision(oldprecision);	/* restore precision */

    if (pdelta > range) {	/* searched too many candidates? */
	if (suspects < 1)	/* unreasonable to have found no suspects */
	    return NOSUSPECTS;	/* fastsieve failed, probably */
	return NOPRIMEFOUND;	/* return error status */
    }
    return 0;			/* return normal completion status */

}				/* nextprime */


/* We will need a series of truly random bits for key generation.
   In most implementations, our random number supply is derived from
   random keyboard delays rather than a hardware random number
   chip.  So we will have to ensure we have a large enough pool of
   accumulated random numbers from the keyboard.  trueRandAccum()
   performs this operation.  
 */

/* Fills 1 unit with random bytes, and returns unit. */
static unit randomunit(void)
{
    unit u = 0;
    byte i;
    i = BYTES_PER_UNIT;
    do
	u = (u << 8) + trueRandByte();
    while (--i != 0);
    return u;
}				/* randomunit */

/*
 * Make a random unit array p with nbits of precision.  Used mainly to 
 * generate large random numbers to search for primes.
 */
static void randombits(unitptr p, short nbits)
{
    mp_init(p, 0);
    make_lsbptr(p, global_precision);

    /* Add whole units of randomness */
    while (nbits >= UNITSIZE) {
	*post_higherunit(p) = randomunit();
	nbits -= UNITSIZE;
    }

    /* Add most-significant partial unit (if any) */
    if (nbits)
	*p = randomunit() & (power_of_2(nbits) - 1);
}				/* randombits */

/*
 * Makes a "random" prime p with nbits significant bits of precision.
 * Since these primes are used to compute a modulus of a guaranteed 
 * length, the top 2 bits of the prime are set to 1, so that the
 * product of 2 primes (the modulus) is of a deterministic length.
 * Returns 0 for normal completion status, < 0 for failure status.
 */
int randomprime(unitptr p, short nbits)
{
    DEBUGprintf2("\nGenerating a %d-bit random prime. ", nbits);
    /* Get an initial random candidate p to start search. */
    randombits(p, nbits - 2);	/* 2 less random bits for nonrandom top bits */
    /* To guarantee exactly nbits of significance, set the top 2 bits to 1 */
    mp_setbit(p, nbits - 1);	/* highest bit is nonrandom */
    mp_setbit(p, nbits - 2);	/* next highest bit is also nonrandom */
    return nextprime(p);	/* search for next higher prime
				   from starting point p */
}				/* randomprime */


#ifdef STRONGPRIMES		/* generate "strong" primes for keys */

#define log_1stprime 6		/* log base 2 of firstprime */

/* 1st primetable entry used by tryprime */
#define firstprime (1<<log_1stprime)

/* This routine attempts to generate a prime p such that p-1 has prime p1
   as its largest factor.  Prime p will have no more than maxbits bits of
   significance.  Prime p1 must be less than maxbits-log_1stprime in length.  
   This routine is called only from goodprime.
 */
static boolean tryprime(unitptr p, unitptr p1, short maxbits)
{
    int i;
    unit i2[MAX_UNIT_PRECISION];
    /* Generate p such that p = (i*2*p1)+1, for i=1,2,3,5,7,11,13,17...
       and test p for primality for each small prime i.
       It's better to start i at firstprime rather than at 1,
       because then p grows slower in significance.
       Start looking for small primes that are > firstprime...
     */
    if ((countbits(p1) + log_1stprime) >= maxbits) {
	DEBUGprintf1("\007[Error: overconstrained prime]");
	return FALSE;		/* failed to make a good prime */
    }
    for (i = 0; primetable[i]; i++) {
	if (primetable[i] < firstprime)
	    continue;
	/* note that mp_init doesn't extend sign bit for >32767 */
	mp_init(i2, primetable[i] << 1);
	mp_mult(p, p1, i2);
	mp_inc(p);
	if (countbits(p) > maxbits)
	    break;
	DEBUGprintf1(".");
	if (primetest(p))
	    return TRUE;
    }
    return FALSE;		/* failed to make a good prime */
}				/* tryprime */


/*
 * Make a "strong" prime p with at most maxbits and at least minbits of 
 * significant bits of precision.  This algorithm is called to generate
 * a high-quality prime p for key generation purposes.  It must have 
 * special characteristics for making a modulus n that is hard to factor.
 * Returns 0 for normal completion status, < 0 for failure status.
 */
int goodprime(unitptr p, short maxbits, short minbits)
{
    unit p1[MAX_UNIT_PRECISION];
    short oldprecision, midbits;
    int status;

    mp_init(p, 0);
    /* Adjust the global_precision downward to the optimum size for p... */
    oldprecision = global_precision;	/* save global_precision */
    /* We will need 2-3 extra bits of precision for the falsekeytest. */
    set_precision(bits2units(maxbits + 4 + SLOP_BITS));
    /* rescale p to global_precision we just defined */
    rescale(p, oldprecision, global_precision);

    minbits -= 2 * log_1stprime;	/* length of p" */
    midbits = (maxbits + minbits) / 2;	/* length of p' */
    DEBUGprintf3("\nGenerating a %d-%d bit refined prime. ",
		 minbits + 2 * log_1stprime, maxbits);
    do {
	do {
	    status = randomprime(p, minbits - 1);
	    if (status < 0)
		return status;	/* failed to find a random prime */
	    DEBUGprintf2("\n(p\042=%d bits)", countbits(p));
	} while (!tryprime(p1, p, midbits));
	DEBUGprintf2("(p'=%d bits)", countbits(p1));
    } while (!tryprime(p, p1, maxbits));
    DEBUGprintf2("\n\007(p=%d bits) ", countbits(p));
    mp_burn(p1);		/* burn the evidence on the stack */
    set_precision(oldprecision);	/* restore precision */
    return 0;			/* normal completion status */
}				/* goodprime */

#endif				/* STRONGPRIMES */


#define iplus1  ( i==2 ? 0 : i+1 )	/* used by Euclid algorithms */
#define iminus1 ( i==0 ? 2 : i-1 )	/* used by Euclid algorithms */

/* Computes greatest common divisor via Euclid's algorithm. */
void mp_gcd(unitptr result, unitptr a, unitptr n)
{
    short i;
    unit gcopies[3][MAX_UNIT_PRECISION];
#define g(i) (  &(gcopies[i][0])  )
    mp_move(g(0), n);
    mp_move(g(1), a);

    i = 1;
    while (testne(g(i), 0)) {
	mp_mod(g(iplus1), g(iminus1), g(i));
	i = iplus1;
    }
    mp_move(result, g(iminus1));
    mp_burn(g(iminus1));	/* burn the evidence on the stack... */
    mp_burn(g(iplus1));
#undef g
}				/* mp_gcd */

/*
 * Euclid's algorithm extended to compute multiplicative inverse.
 * Computes x such that a*x mod n = 1, where 0<a<n
 *
 * The variable u is unnecessary for the algorithm, but is 
 * included in comments for mathematical clarity. 
 */
void mp_inv(unitptr x, unitptr a, unitptr n)
{
    short i;
    unit y[MAX_UNIT_PRECISION], temp[MAX_UNIT_PRECISION];
    unit gcopies[3][MAX_UNIT_PRECISION], vcopies[3][MAX_UNIT_PRECISION];
#define g(i) (  &(gcopies[i][0])  )
#define v(i) (  &(vcopies[i][0])  )
/*      unit ucopies[3][MAX_UNIT_PRECISION]; */
/* #define u(i) (  &(ucopies[i][0])  ) */
    mp_move(g(0), n);
    mp_move(g(1), a);
/*      mp_init(u(0),1); mp_init(u(1),0); */
    mp_init(v(0), 0);
    mp_init(v(1), 1);
    i = 1;
    while (testne(g(i), 0)) {
	/* we know that at this point,  g(i) = u(i)*n + v(i)*a  */
	mp_udiv(g(iplus1), y, g(iminus1), g(i));
	mp_mult(temp, y, v(i));
	mp_move(v(iplus1), v(iminus1));
	mp_sub(v(iplus1), temp);
/*      mp_mult(temp,y,u(i)); mp_move(u(iplus1),u(iminus1));
	mp_sub(u(iplus1),temp); */
	i = iplus1;
    }
    mp_move(x, v(iminus1));
    if (mp_tstminus(x))
	mp_add(x, n);
    mp_burn(g(iminus1));	/* burn the evidence on the stack... */
    mp_burn(g(iplus1));
    mp_burn(v(0));
    mp_burn(v(1));
    mp_burn(v(2));
    mp_burn(y);
    mp_burn(temp);
#undef g
#undef v
}				/* mp_inv */

#ifdef STRONGPRIMES

/*      mp_sqrt - returns square root of a number.
   returns -1 for error, 0 for perfect square, 1 for not perfect square.
   Not used by any RSA-related functions.       Used by factoring algorithms.
   This version needs optimization.
   by Charles W. Merritt  July 15, 1989, refined by PRZ.

   These are notes on computing the square root the manual old-fashioned 
   way.  This is the basis of the fast sqrt algorithm mp_sqrt below:

   1)   Separate the number into groups (periods) of two digits each,
   beginning with units or at the decimal point.
   2)   Find the greatest perfect square in the left hand period & write 
   its  square root as the first figure of the required root.  Subtract
   the square of this number from the left hand period.  Annex to the
   remainder the next group so as to form a dividend.
   3)   Double the root already found and write it as a partial divisor at 
   the left of the new dividend.  Annex one zero digit, making a trial 
   divisor, and divide the new dividend by the trial divisor.
   4)   Write the quotient in the root as the trial term and also substitute 
   this quotient for the annexed zero digit in the partial divisor, 
   making the latter complete.
   5)   Multiply the complete divisor by the figure just obtained and, 
   if possible, subtract the product from the last remainder.
   If this product is too large, the trial term of the quotient
   must be replaced by the next smaller number and the operations
   preformed as before.
   (IN BINARY, OUR TRIAL TERM IS ALWAYS 1 AND WE USE IT OR DO NOTHING.)
   6)   Proceed in this manner until all periods are used.
   If there is still a remainder, it's not a perfect square.
 */

/* Quotient is returned as the square root of dividend. */
static int mp_sqrt(unitptr quotient, unitptr dividend)
{
    register short next2bits;	/* "period", or group of 2 bits of dividend */
    register unit dvdbitmask, qbitmask;
    unit remainder[MAX_UNIT_PRECISION];
    unit rjq[MAX_UNIT_PRECISION], divisor[MAX_UNIT_PRECISION];
    unsigned int qbits, qprec, dvdbits, dprec, oldprecision;
    int notperfect;

    mp_init(quotient, 0);
    if (mp_tstminus(dividend)) {	/* if dividend<0, return error */
	mp_dec(quotient);	/* quotient = -1 */
	return -1;
    }
    /* normalize and compute number of bits in dividend first */
    init_bitsniffer(dividend, dvdbitmask, dprec, dvdbits);
    /* init_bitsniffer returns a 0 if dvdbits is 0 */
    if (dvdbits == 1) {
	mp_init(quotient, 1);	/* square root of 1 is 1 */
	return 0;
    }
    /* rescale quotient to half the precision of dividend */
    qbits = (dvdbits + 1) >> 1;
    qprec = bits2units(qbits);
    rescale(quotient, global_precision, qprec);
    make_msbptr(quotient, qprec);
    qbitmask = power_of_2((qbits - 1) & (UNITSIZE - 1));

    /*
     * Set smallest optimum precision for this square root.
     * The low-level primitives are affected by the call to set_precision.
     * Even though the dividend precision is bigger than the precision
     * we will use, no low-level primitives will be used on the dividend.
     * They will be used on the quotient, remainder, and rjq, which are
     * smaller precision.
     */
    oldprecision = global_precision;	/* save global_precision */
    set_precision(bits2units(qbits + 3));	/* 3 bits of precision slop */

    /* special case: sqrt of 1st 2 (binary) digits of dividend
       is 1st (binary) digit of quotient.  This is always 1. */
    stuff_bit(quotient, qbitmask);
    bump_bitsniffer(quotient, qbitmask);
    mp_init(rjq, 1);		/* rjq is Right Justified Quotient */

    if (!(dvdbits & 1)) {
	/* even number of bits in dividend */
	next2bits = 2;
	bump_bitsniffer(dividend, dvdbitmask);
	dvdbits--;
	if (sniff_bit(dividend, dvdbitmask))
	    next2bits++;
	bump_bitsniffer(dividend, dvdbitmask);
	dvdbits--;
    } else {
	/* odd number of bits in dividend */
	next2bits = 1;
	bump_bitsniffer(dividend, dvdbitmask);
	dvdbits--;
    }

    mp_init(remainder, next2bits - 1);

    /* dvdbits is guaranteed to be even at this point */

    while (dvdbits) {
	next2bits = 0;
	if (sniff_bit(dividend, dvdbitmask))
	    next2bits = 2;
	bump_bitsniffer(dividend, dvdbitmask);
	dvdbits--;
	if (sniff_bit(dividend, dvdbitmask))
	    next2bits++;
	bump_bitsniffer(dividend, dvdbitmask);
	dvdbits--;
	mp_rotate_left(remainder, (boolean) ((next2bits & 2) != 0));
	mp_rotate_left(remainder, (boolean) ((next2bits & 1) != 0));

	/*
	 * "divisor" is trial divisor, complete divisor is 4*rjq 
	 * or 4*rjq+1.
	 * Subtract divisor times its last digit from remainder.
	 * If divisor ends in 1, remainder -= divisor*1,
	 * or if divisor ends in 0, remainder -= divisor*0 (do nothing).
	 * Last digit of divisor inflates divisor as large as possible
	 * yet still subtractable from remainder.
	 */
	mp_move(divisor, rjq);	/* divisor = 4*rjq+1 */
	mp_rotate_left(divisor, 0);
	mp_rotate_left(divisor, 1);
	if (mp_compare(remainder, divisor) >= 0) {
	    mp_sub(remainder, divisor);
	    stuff_bit(quotient, qbitmask);
	    mp_rotate_left(rjq, 1);
	} else {
	    mp_rotate_left(rjq, 0);
	}
	bump_bitsniffer(quotient, qbitmask);
    }
    notperfect = testne(remainder, 0);	/* not a perfect square? */
    set_precision(oldprecision);	/* restore original precision */
    return notperfect;		/* normal return */
}				/* mp_sqrt */
#endif

/*------------------- End of genprime.c -----------------------------*/


/* ==== getopt.c ==== */
/*
   **   @(#)getopt.c    2.5 (smail) 9/15/87
 */

/*
   *  This is the AT&T public domain source for getopt(3).  It is the code
   *  which was given out at the 1985 UNIFORUM conference in Dallas.
   *   
   *  There is no manual page.  That is because the one they gave out at
   *  UNIFORUM was slightly different from the current System V Release 2
   *  manual page.  The difference apparently involved a note about the
   *  famous rules 5 and 6, recommending using white space between an
   *  option and its first argument, and not grouping options that have
   *  arguments.  Getopt itself is currently lenient about both of these
   *  things.  White space is allowed, but not mandatory, and the last option
   *  in a group can have an argument.  That particular version of the man
   *  page evidently has no official existence.  The current SVR2 man page
   *  reflects the actual behavor of this getopt.
 */

#include <string.h>
#include <stdio.h>
#include "getopt.h"

/*LINTLIBRARY */
#ifndef NULL
#define NULL	0
#endif
#define EOF	(-1)
#define ERR(str, chr) (opterr ? \
fprintf(stderr, "%s%s%c\n", argv[0], str, chr) : 0)

int opterr = 1;
int optind = 1;
int optopt = 0;
char *optarg = 0;

int pgp_getopt(int argc, char * const argv[], const char *opts)
{
    static int sp = 1;
    register int c;
    register char *cp;

    if (sp == 1) {
	if (optind >= argc || (argv[optind][0] != '+' &&
		      argv[optind][0] != '-') || argv[optind][1] == '\0')
	    return EOF;
	else if (strcmp(argv[optind], "--") == 0) {
	    optind++;
	    return EOF;
	}
	/* '+' for config options, '+' should not be in the opts list */
	if (argv[optind][0] == '+') {
	    optarg = argv[optind++] + 1;
	    return '+';
	}
    }
    optopt = c = argv[optind][sp];
    if (c == ':' || (cp = strchr(opts, c)) == NULL) {
	ERR(": illegal option -- ", c);
	if (argv[optind][++sp] == '\0') {
	    optind++;
	    sp = 1;
	}
	return '\0';
    }
    if (*++cp == ':') {
	if (argv[optind][sp + 1] != '\0')
	    optarg = &argv[optind++][sp + 1];
	else if (++optind >= argc) {
	    ERR(": option requires an argument -- ", c);
	    sp = 1;
	    return '\0';
	} else
	    optarg = argv[optind++];
	sp = 1;
    } else {
	if (argv[optind][++sp] == '\0') {
	    sp = 1;
	    optind++;
	}
	optarg = NULL;
    }
    return c;
}


/* ==== gettime.c ==== */
/*
 *	Gettimeofday.  Simulate as much as possible.  Only accurate
 *	to nearest second.  tzp is ignored.  Derived from an old
 *	emacs implementation.
 */

#include <sys/types.h>
#if defined(WINDOWS)
#include <time.h>
#else
#include <sys/time.h>
#endif

gettimeofday (tp, tzp)
     struct timeval *tp;
     struct timezone *tzp;
{
#if !defined(WINDOWS)
  extern long time ();
#endif

  tp->tv_sec = time ((long *)0);
  tp->tv_usec = 0;
}


/* ==== keyadd.c ==== */
/*      keyadd.c  - Keyring merging routines for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef UNIX
#include <sys/types.h>
#endif
#include <time.h>
#include "mpilib.h"
#include "crypto.h"
#include "fileio.h"
#include "keymgmt.h"
#include "charset.h"
#include "mpiio.h"
#include "language.h"
#include "pgp.h"
#include "exitpgp.h"
#include "keyadd.h"
#include "keymaint.h"
#ifdef MACTC5
#include "Macutil2.h"
#include "Macutil3.h"
#include "MyBufferedStdio.h"
#include "ReplaceStdio.h"
int _addto_keyring(char *keyfile, char *ringfile);
#endif

void gpk_close(void);
int gpk_open(char *keyfile);
int get_publickey(long *file_position, int *pktlen,
		  byte * keyID, byte * timestamp,
		  byte * userid, unitptr n, unitptr e);

static int ask_to_sign(byte * keyID, char *ringfile);
static boolean ask_first;

static boolean publickey;	/* if TRUE, add trust packets */

static int newkeys, newsigs, newids, newrvks;
static byte mykeyID[KEYFRAGSIZE];

static struct sig_list {
    struct sig_list *next;
    long pos;
} *siglist;
static void sig_list_add(long pos)
{
    struct sig_list *p;
    p = xmalloc(sizeof *p);
    p->pos = pos;
    p->next = siglist;
    siglist = p;
}
static int sig_list_find(long pos)
{
    struct sig_list *p;
    for (p = siglist; p; p = p->next)
	if (p->pos == pos)
	    return 1;
    return 0;
}
static void sig_list_clear(void)
{
    struct sig_list *p, *n;
    for (p = siglist; p; p = n) {
	n = p->next;
	free(p);
    }
    siglist = NULL;
}

/* Merge signatures from userid in fkey (which is keyfile) at keypos with
 * userid from fring (which is ringfile) at ringpos, appending result to out.
 */
static int mergesigs(FILE * fkey, char *keyfile, long keypos, FILE * fring,
		     char *ringfile, long *pringpos, FILE * out)
{
    long ringuseridpos, ringpos;
    int ringpktlen, keypktlen;
    int status;
    byte ctb;
    int copying;
    word32 rstamp, kstamp, xstamp;
    byte keyID[KEYFRAGSIZE];
    char userid[256];

    /* First, copy the userid packet itself, plus any comments or ctrls */
    ringuseridpos = ringpos = *pringpos;
    fseek(fring, ringpos, SEEK_SET);
    (void) readkeypacket(fring, FALSE, &ctb, NULL, userid, NULL, NULL,
			 NULL, NULL, NULL, NULL, NULL, NULL);
    PascalToC(userid);
    ringpktlen = ftell(fring) - ringpos;
    copyfilepos(fring, out, ringpktlen, ringpos);
    for (;;) {
	ringpos = ftell(fring);
	status = nextkeypacket(fring, &ctb);
	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID ||
	    is_ctb_type(ctb, CTB_SKE_TYPE))
	    break;
	ringpktlen = ftell(fring) - ringpos;
	copyfilepos(fring, out, ringpktlen, ringpos);
    }
    fseek(fring, ringpos, SEEK_SET);

    /* Now, ringpos points just past userid packet and ctrl packet. */
    /* Advance keypos to the analogous location. */
    fseek(fkey, keypos, SEEK_SET);
    (void) nextkeypacket(fkey, &ctb);
    for (;;) {
	keypos = ftell(fkey);
	status = nextkeypacket(fkey, &ctb);
	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID ||
	    is_ctb_type(ctb, CTB_SKE_TYPE))
	    break;
    }
    fseek(fkey, keypos, SEEK_SET);

    /* Second, copy all keyfile signatures that aren't in ringfile.
     */

    copying = FALSE;
    for (;;) {
	/* Read next sig from keyfile; see if it is in ringfile;
	 * if it is not a signature, ignore it,
	 * if it is absent from ringfile, copy it,
	 * if it is present, and the timestamp is not newer, ignore it,
	 * if present and newer, replace old with new.
	 * Loop till hit a new key or userid in keyfile, or EOF.
	 */
	keypos = ftell(fkey);
	status = readkeypacket(fkey, FALSE, &ctb, (byte *) & kstamp,
			       NULL, NULL, NULL,
			       NULL, NULL, NULL, NULL, keyID, NULL);
#ifdef MACTC5
	mac_poll_for_break();
#endif
	if (status == -3)	/* unrecoverable error: bad packet
				   length etc. */
	    return status;
	keypktlen = ftell(fkey) - keypos;
	if (status == -1 || is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;		/* EOF or next key/userid */
	if (status < 0)
	    continue;		/* bad packet, skip it */
	if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
	    long sig_pos;
	    int sig_len;
	    /* Set copying true if signature is not in the ringfile */
	    copying = (getpubusersig(ringfile, ringuseridpos,
				     keyID, (byte *) & rstamp,
				     &sig_pos,
				     &sig_len) < 0);
	    if (!copying) {
		long save_pos = ftell(fkey);
		fseek(fkey, keypos + 6, SEEK_SET);
		fread(&kstamp, 1, SIZEOF_TIMESTAMP, fkey);
		fseek(fkey, save_pos, SEEK_SET);
		convert_byteorder((byte *) & kstamp, SIZEOF_TIMESTAMP);
		if (verbose)
		    fprintf(pgpout, "ring: %lx  key: %lx\n", rstamp, kstamp);
		if (kstamp > rstamp) {	/* Update, Maybe */
		    char *signator;
		    if ((signator = user_from_keyID(keyID)) == NULL) {
			fprintf(pgpout,
	       LANG("Replacing signature from keyID %s on userid \"%s\"\n"),
				keyIDstring(keyID), LOCAL_CHARSET(userid));
			/* No pubkey for KeyID, no update! */
		    } else {
			long save_keypos;
			long save_ringpos;
			long KeyIDpos;
			int KeyIDlen;
			byte sigClass;
			fprintf(pgpout,
				LANG("Verifying signature from %s\n"),
				LOCAL_CHARSET(signator));
			fprintf(pgpout, LANG("on userid \"%s\"\n"),
				LOCAL_CHARSET(userid));
			save_keypos = ftell(fkey);
			save_ringpos = ftell(fring);
			status = getpublickey(GPK_GIVEUP, ringfile,
					      &KeyIDpos, &KeyIDlen, NULL,
					      NULL, (byte *) userid, NULL,
					      NULL);
			if (!status)
			    status = check_key_sig(fring,
						   KeyIDpos, KeyIDlen,
						   userid, fkey, keypos,
						   ringfile, NULL,
						   (byte *) & xstamp,
						   &sigClass);
			PascalToC(userid);
			PascalToC(signator);
			if (!status) {
			    fprintf(pgpout,
				    LANG("Replacing signature from %s\n"),
				    LOCAL_CHARSET(signator));
			    fprintf(pgpout,
				    LANG("on userid \"%s\"\n"),
				    LOCAL_CHARSET(userid));
			    sig_list_add(sig_pos);
			    ++newsigs;
			    copying = 1;
			} else
			    fprintf(pgpout, LANG("Verification Failed\n"));
			fseek(fring, save_ringpos, SEEK_SET);
			fseek(fkey, save_keypos, SEEK_SET);
		    }
		}
	    } else {
		char *signator;
		if ((signator = user_from_keyID(keyID)) == NULL)
		    fprintf(pgpout,
		       LANG("New signature from keyID %s on userid \"%s\"\n"),
			    keyIDstring(keyID), LOCAL_CHARSET(userid));
		else {
		    fprintf(pgpout,
			    LANG("New signature from %s\n"),
			    LOCAL_CHARSET(signator));
		    fprintf(pgpout,
			    LANG("on userid \"%s\"\n"), LOCAL_CHARSET(userid));
		}
		++newsigs;
		if (batchmode)
		    show_update(keyIDstring(mykeyID));
	    }
	}
	if (copying && is_ctb_type(ctb, CTB_SKE_TYPE)) {
	    copyfilepos(fkey, out, keypktlen, keypos);
	    if (publickey)
		write_trust(out, KC_SIGTRUST_UNDEFINED);
	}
    }

    /* Third, for all ring sig's which are not replaced, copy to output */
    fseek(fring, ringpos, SEEK_SET);
    for (;;) {
	ringpos = ftell(fring);
	if (sig_list_find(ringpos)) {
	    /* skip signature packet */
	    nextkeypacket(fring, &ctb);
	    ringpos = ftell(fring);
	    /* skip trust packet, if present */
	    if (nextkeypacket(fring, &ctb) < 0 || ctb != CTB_KEYCTRL)
		fseek(fring, ringpos, SEEK_SET);
	    continue;
	}
	status = nextkeypacket(fring, &ctb);
	ringpktlen = ftell(fring) - ringpos;
	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;
	copyfilepos(fring, out, ringpktlen, ringpos);
    }				/* End of loop for each sig in ringfile */
    sig_list_clear();
    fseek(fring, ringpos, SEEK_SET);
    *pringpos = ringpos;
    return 0;
}				/* mergesigs */

/* Merge key from fkey (which is keyfile) at keypos with key from
 * fring (which is ringfile) at ringpos, appending result to out.
 */
static int mergekeys(FILE * fkey, char *keyfile, long keypos, FILE * fring,
		     char *ringfile, long *pringpos, FILE * out)
{
    long ringkeypos, keykeypos, ringpos;
    int ringpktlen, keypktlen;
    int status;
    byte ctb;
    int copying;
    boolean ring_compromise = FALSE;
    byte userid[256];

    /* First, copy the key packet itself, plus any comments or ctrls */
    ringkeypos = ringpos = *pringpos;
    fseek(fring, ringpos, SEEK_SET);
    (void) nextkeypacket(fring, &ctb);
    ringpktlen = ftell(fring) - ringpos;
    copyfilepos(fring, out, ringpktlen, ringpos);
    for (;;) {
	ringpos = ftell(fring);
	status = nextkeypacket(fring, &ctb);
	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;
	if (is_ctb_type(ctb, CTB_SKE_TYPE))
	    ring_compromise = TRUE;	/* compromise cert on keyring */
	ringpktlen = ftell(fring) - ringpos;
	copyfilepos(fring, out, ringpktlen, ringpos);
    }
    fseek(fring, ringpos, SEEK_SET);

    /* Now, ringpos points just past key packet and ctrl packet. */
    /* Advance keypos to the analogous location. */
    fseek(fkey, keypos, SEEK_SET);
    keykeypos = keypos;
    (void) nextkeypacket(fkey, &ctb);
    keypktlen = ftell(fkey) - keypos;	/* for check_key_sig() */
    for (;;) {
	keypos = ftell(fkey);
	status = nextkeypacket(fkey, &ctb);
	if (status < 0 || ctb == CTB_USERID || is_ctb_type(ctb, CTB_SKE_TYPE))
	    break;
    }
    if (!ring_compromise && is_ctb_type(ctb, CTB_SKE_TYPE)) {
	/* found a compromise cert on keyfile that is not in ringfile */
	word32 timestamp;
	byte sig_class;
	int cert_pktlen;

	cert_pktlen = ftell(fkey) - keypos;
	if (check_key_sig(fkey, keykeypos, keypktlen,
			  (char *) userid, fkey, keypos,
			  ringfile, (char *) userid, (byte *) & timestamp,
			  &sig_class) == 0 &&
	    sig_class == KC_SIGNATURE_BYTE) {
	    PascalToC((char *) userid);
	    fprintf(pgpout, LANG("Key revocation certificate from \"%s\".\n"),
		    LOCAL_CHARSET((char *) userid));
	    copyfilepos(fkey, out, cert_pktlen, keypos);
	    /* Show updates */
	    if (batchmode)
		show_key(fring, *pringpos, SHOW_CHANGE);
	    ++newrvks;
	} else
	    fprintf(pgpout,
     LANG("\n\007WARNING:  File '%s' contains bad revocation certificate.\n"),
		    keyfile);
    }
    fseek(fkey, keypos, SEEK_SET);

    /* Second, copy all keyfile userid's plus signatures that aren't
     * in ringfile.
     */

    copying = FALSE;
    for (;;) {
	/* Read next userid from keyfile; see if it is in ringfile;
	 * set copying true/false accordingly.  If copying is true
	 * and it is a userid or a signature, copy it.  Loop till hit
	 * a new key in keyfile, or EOF.
	 */
	keypos = ftell(fkey);
	status = readkeypacket(fkey, FALSE, &ctb, NULL, (char *) userid, NULL,
			       NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	if (status == -3) /* unrecoverable error: bad packet length etc. */
	    return status;
	keypktlen = ftell(fkey) - keypos;
	if (status == -1 || is_key_ctb(ctb))
	    break;		/* EOF or next key */
	if (status < 0)
	    continue;		/* bad packet, skip it */
	if (ctb == CTB_USERID) {
	    long userid_pos;
	    int userid_len;
	    PascalToC((char *) userid);
	    /* Set copying true if userid is not in the ringfile */
	    copying = (getpubuserid(ringfile, ringkeypos, userid, &userid_pos,
				    &userid_len, TRUE) < 0);
	    if (copying) {
		putc('\n', pgpout);
		fprintf(pgpout, LANG("New userid: \"%s\".\n"),
			LOCAL_CHARSET((char *) userid));
		fprintf(pgpout,
			LANG("\nWill be added to the following key:\n"));
		show_key(fring, *pringpos, 0);
		fprintf(pgpout, LANG("\nAdd this userid (y/N)? "));
		if (batchmode || getyesno('n')) {
		    ++newids;
		    /* Show an update string */
		    if (batchmode) {
			fprintf(pgpout, "\n");
			show_key(fring, *pringpos, SHOW_CHANGE);
		    }
		} else {
		    copying = FALSE;
		}
	    }
	}
	if (copying) {
	    if (ctb == CTB_USERID || is_ctb_type(ctb, CTB_SKE_TYPE)) {
		copyfilepos(fkey, out, keypktlen, keypos);
		if (publickey) {
		    if (is_ctb_type(ctb, CTB_SKE_TYPE))
			write_trust(out, KC_SIGTRUST_UNDEFINED);
		    else
			write_trust(out, KC_LEGIT_UNKNOWN);
		}
	    }
	}
    }

    /* Third, for all ring userid's, if not in keyfile, copy the userid
     * plus its dependant signatures.
     */
    fseek(fring, ringpos, SEEK_SET);
    /* Grab the keyID here */
    readkeypacket(fring, FALSE, &ctb, NULL, (char *) userid, NULL, NULL,
		  NULL, NULL, NULL, NULL, NULL, NULL);
    fseek(fring, ringpos, SEEK_SET);
    for (;;) {
	ringpos = ftell(fring);
	status = readkeypacket(fring, FALSE, &ctb, NULL,
			       (char *) userid, NULL, NULL,
			       NULL, NULL, NULL, NULL, NULL, NULL);
	ringpktlen = ftell(fring) - ringpos;
	if (status == -3)
	    return status;
	if (status == -1 || is_key_ctb(ctb))
	    break;
	if (ctb == CTB_USERID) {
	    long userid_pos;
	    int userid_len;
	    /* See if there is a match in keyfile */
	    PascalToC((char *) userid);
	    /* don't use substring match (exact_match = TRUE) */
	    if (getpubuserid(keyfile, keykeypos, userid,
			     &userid_pos, &userid_len, TRUE) >= 0) {
		if ((status = mergesigs(fkey, keyfile, userid_pos,
					fring, ringfile, &ringpos, out)) < 0)
		    return status;
		copying = FALSE;
	    } else {
		copying = TRUE;
	    }
	}
	if (copying) {
	    /* Copy ringfile userid and sigs to out */
	    copyfilepos(fring, out, ringpktlen, ringpos);
	}
    }				/* End of loop for each key in ringfile */
    fseek(fring, ringpos, SEEK_SET);
    *pringpos = ringpos;
    return 0;
}				/* mergekeys */

/* Adds (prepends) key file to key ring file. */
int _addto_keyring(char *keyfile, char *ringfile)
{
    FILE *f, *g, *h;
    long file_position, fp;
    int pktlen;
    byte ctb;
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    unit n1[MAX_UNIT_PRECISION];
    byte keyID[KEYFRAGSIZE];
    byte userid[256];		/* key certificate userid */
    byte userid1[256];
    word32 tstamp;
    byte *timestamp = (byte *) & tstamp;	/* key certificate timestamp */
    boolean userid_seen = FALSE;
    int commonkeys = 0;
    int copying;
    struct newkey *nkey, *nkeys = NULL;
    char *scratchf;

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(keyfile, FOPRBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key file '%s'\n"), keyfile);
	return -1;
    }
    ctb = 0;
    if (fread(&ctb, 1, 1, f) != 1 || !is_key_ctb(ctb)) {
	fclose(f);
	return -1;
    }
    rewind(f);

    setoutdir(ringfile);
    scratchf = tempfile(0);

    /*
     * get userids from both files, maybe should also use the default public
     * keyring if ringfile is not the default ring.
     */
    setkrent(ringfile);
    setkrent(keyfile);
    init_userhash();

    if (!file_exists(ringfile)) {
	/* ringfile does not exist.  Can it be created? */
	/* open file g for writing, in binary (not text) mode... */
	g = fopen(ringfile, FOPWBIN);
	if (g == NULL) {
	    fprintf(pgpout,
		    LANG("\nKey ring file '%s' cannot be created.\n"),
		    ringfile);
	    fclose(f);
	    goto err;
	}
	fclose(g);
    }
    /* Create working output file */
    /* open file g for writing, in binary (not text) mode... */
    if ((g = fopen(scratchf, FOPWBIN)) == NULL) {
	fclose(f);
	goto err;
    }
    newkeys = newsigs = newids = newrvks = 0;

    /* Pass 1 - copy all keys from f which aren't in ring file */
    /* Also copy userid and signature packets. */
    fprintf(pgpout, LANG("\nLooking for new keys...\n"));
    copying = FALSE;
    if (gpk_open(ringfile) < 0) {
	fclose(f);		/* close key file */
	fclose(g);
	goto err;
    }
    for (;;) {
	file_position = ftell(f);

	status = readkeypacket(f, FALSE, &ctb,
			       timestamp, (char *) userid, n, e,
			       NULL, NULL, NULL, NULL, NULL, NULL);
	/* Note that readkeypacket has called set_precision */
	if (status == -1)	/* EOF */
	    break;
	if (status == -2 || status == -3) {
	    fprintf(pgpout,
		    LANG("\n\007Could not read key from file '%s'.\n"),
		    keyfile);
	    fclose(f);		/* close key file */
	    fclose(g);
	    goto err;
	}
	if (status < 0) {
	    copying = FALSE;
	    continue;	/* don't merge keys from unrecognized version */
	}
#ifdef MACTC5
	mac_poll_for_break();
#endif
	/* Check to see if key is already on key ring */
	if (is_key_ctb(ctb)) {
	    extract_keyID(keyID, n);	/* from keyfile, not ringfile */
	    publickey = is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE);

	    /*      Check for duplicate key in key ring: */
	    status = get_publickey(&fp, NULL, keyID, timestamp, userid, n1, e);
	    if (status == 0) {
		/* key in both keyring and keyfile */
		if (mp_compare(n, n1) != 0) {
		    fprintf(pgpout,
LANG("\n\007Warning: Key ID %s matches key ID of key already on \n\
key ring '%s', but the keys themselves differ.\n\
This is highly suspicious.  This key will not be added to ring.\n\
Acknowledge by pressing return: "), keyIDstring(keyID), ringfile);
		    getyesno('n');
		} else {
		    ++commonkeys;
		}
		copying = FALSE;
	    } else if (status == -1) {	/* key NOT in keyring */
		++newkeys;
		if (interactive_add) {
		    if (!show_key(f, file_position, SHOW_ALL)) {
		        fprintf(pgpout,
	       LANG("\nDo you want to add this key to keyring '%s' (y/N)? "),
			    ringfile);
			copying = getyesno('n');
		    } else
		        copying = FALSE;
		} else {
		    if (!show_key(f, file_position, SHOW_LISTFMT)) 
		    	copying = TRUE;
		    else
		    	copying = FALSE;
		}

		/* If batchmode, output an update message */
		if (batchmode)
		    show_key(f, file_position, SHOW_CHANGE);
		if (copying) {
		    nkey = xmalloc(sizeof(*nkey));
		    memcpy(nkey->keyID, keyID, KEYFRAGSIZE);
		    nkey->next = nkeys;
		    nkeys = nkey;
		}
	    } else {
		/* unknown version or bad key */
		copying = FALSE;
	    }
	}
	/*
	 * Now, we copy according to the copying flag
	 * The key is prepended to the ring to give it search
	 *  precedence over other keys with that same userid.
	 */
	if (copying && (is_key_ctb(ctb) || ctb == CTB_USERID ||
			is_ctb_type(ctb, CTB_SKE_TYPE))) {
	    pktlen = (int) (ftell(f) - file_position);
	    copyfilepos(f, g, pktlen, file_position); /* copy packet from f */
	    if (publickey) {
		/* Initialize trust packets after keys and signatures */
		if (is_key_ctb(ctb)) {
		    write_trust(g, KC_OWNERTRUST_UNDEFINED);
		    userid_seen = FALSE;
		} else if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
		    if (userid_seen) {
			write_trust(g, KC_SIGTRUST_UNDEFINED);
		    } else {
	    /* signature certificate before userid must be compromise cert. */
			fprintf(pgpout, LANG("Key has been revoked.\n"));
		    }
		} else if (is_ctb_type(ctb, CTB_USERID_TYPE)) {
		    write_trust(g, KC_LEGIT_UNKNOWN);
		    userid_seen = TRUE;
		}
	    }
	}
    }
    gpk_close();

    /*
     * Now copy the remainder of the ringfile, h, to g.  commonkeys tells
     * how many keys are common to keyfile and ringfile.  As long as that
     * is nonzero we will check each key in ringfile to see if it has a
     * match in keyfile.
     */
    if ((h = fopen(ringfile, FOPRBIN)) != NULL) {
	if (gpk_open(keyfile) < 0) {
	    fclose(f);
	    fclose(g);
	    fclose(h);
	    goto err;
	}
	while (commonkeys) {
	    /* Loop for each key in ringfile */
	    file_position = ftell(h);
	    status = readkeypacket(h, FALSE, &ctb, NULL, (char *) userid, n, e,
				   NULL, NULL, NULL, NULL, NULL, NULL);
	    if (status == -1 || status == -3) {
		if (status == -1)	/* hit EOF */
		    fprintf(pgpout,
LANG("\n\007Key file contains duplicate keys: cannot be added to keyring\n"));
		else
		    fprintf(pgpout,
LANG("\n\007Could not read key from file '%s'.\n"),
			    ringfile);
		fclose(f);
		fclose(g);
		fclose(h);
		goto err;
	    }
	    PascalToC((char *) userid);
	    pktlen = ftell(h) - file_position;
	    if (is_key_ctb(ctb)) {
		long tfp;
/* unknown version or bad data: copy (don't remove packets from ringfile) */
		copying = TRUE;
		if (status == 0) {
		    /* See if there is a match in keyfile */
		    extract_keyID(keyID, n);	/* from ringfile,
						   not keyfile */
		    extract_keyID(mykeyID, n);	/* save this */
		    publickey = is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE);
		    if ((status = get_publickey(&tfp, NULL, keyID,
				      timestamp, userid1, n1, e)) >= 0) {
			if (verbose)
			    fprintf(pgpout, 
				    "Merging key ID: %s\n",
				    keyIDstring(keyID));
			if (mergekeys(f, keyfile, tfp, h,
				      ringfile, &file_position, g) < 0) {
			    fclose(f);
			    fclose(g);
			    fclose(h);
			    goto err;
			}
			copying = FALSE;
			--commonkeys;
		    } else {
		      if (status == -3)
			--commonkeys; /* missing userid packet? */
		    }
		}
	    }
	    if (copying) {
		/* Copy ringfile key to g, without its sigs */
		copyfilepos(h, g, pktlen, file_position);
		file_position += pktlen;
	    }
	}			/* End of loop for each key in ringfile */
	gpk_close();
	copyfile(h, g, -1L);	/* copy rest of file from file h to g */
	fclose(h);
    }
    fclose(f);
    if (write_error(g)) {
	fclose(g);
	goto err;
    }
    fclose(g);
    if (newsigs == 0 && newkeys == 0 && newids == 0 && newrvks == 0) {
	fprintf(pgpout, LANG("No new keys or signatures in keyfile.\n"));
	rmtemp(scratchf);
	endkrent();
	return 0;
    }
    if (status = dokeycheck(NULL, scratchf, CHECK_NEW)) {
	if (verbose)
	    fprintf(pgpout, "addto_keyring: dokeycheck returned %d\n", status);
	goto err;
    }
    endkrent();

    fprintf(pgpout, LANG("\nKeyfile contains:\n"));
    if (newkeys)
	fprintf(pgpout, LANG("%4d new key(s)\n"), newkeys);
    if (newsigs)
	fprintf(pgpout, LANG("%4d new signatures(s)\n"), newsigs);
    if (newids)
	fprintf(pgpout, LANG("%4d new user ID(s)\n"), newids);
    if (newrvks)
	fprintf(pgpout, LANG("%4d new revocation(s)\n"), newrvks);

    ask_first = TRUE;
    status = maint_update(scratchf, nkeys);
    if (status >= 0 && !filter_mode && !batchmode)
	for (nkey = nkeys; nkey; nkey = nkey->next)
	    if (ask_to_sign(nkey->keyID, scratchf) != 0)
		break;
    if (status && verbose)
	fprintf(pgpout, "addto_keyring: maint_update returned %d\n", status);

    free_newkeys(nkeys);

    savetempbak(scratchf, ringfile);

#ifdef MACTC5
{
byte header[8];
	get_header_info_from_file(ringfile, header, 8 );
	if (header[0] == CTB_CERT_SECKEY)
		PGPSetFinfo(ringfile,'SKey','MPGP');
	if (header[0] == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
	}
#endif

    return 0;			/* normal return */

  err:
    gpk_close();		/* save to call if not opened */
    endkrent();
    /* make sure we remove any garbage files we may have created */
    rmtemp(scratchf);
    return -1;
}				/* _addto_keyring */

int addto_keyring(char *keyfile, char *ringfile)
{
    long armorline = 0;
    char *tempf;
    int addflag = 0;

    if (_addto_keyring(keyfile, ringfile) == 0)
	return 0;
    /* check if the keyfile to be added is armored */
    while (is_armor_file(keyfile, armorline)) {
	tempf = tempfile(TMP_TMPDIR | TMP_WIPE);
	if (de_armor_file(keyfile, tempf, &armorline)) {
	    rmtemp(tempf);
	    return -1;
	}
	if (_addto_keyring(tempf, ringfile) == 0)
	    addflag = 1;
	rmtemp(tempf);
    }
    if (!addflag) {
	fprintf(pgpout, LANG("\nNo keys found in '%s'.\n"), keyfile);
	return -1;
    } else {
	return 0;
    }
}

static int ask_to_sign(byte * keyID, char *ringfile)
{
    FILE *f;
    word32 timestamp;
    byte ctb, trust;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte userid[256];
    long fpos;
    int status;
    extern char my_name[];

    if (getpublickey(GPK_GIVEUP, ringfile, &fpos, NULL, keyID,
		     (byte *) & timestamp, userid, n, e) < 0)
	return -1;

    if ((f = fopen(ringfile, FOPRBIN)) == NULL)
	return -1;

    fseek(f, fpos, SEEK_SET);
    if (is_compromised(f)) {
	fclose(f);
	return 0;
    }
    if (nextkeypacket(f, &ctb) < 0) {
	fclose(f);
	return -1;
    }
    if (ctb != CTB_CERT_PUBKEY) {
	fclose(f);
	return 0;		/* don't ask to sign secret key */
    }
    while (nextkeypacket(f, &ctb) == 0 && !is_key_ctb(ctb))
	if (ctb == CTB_USERID)	/* check first userid */
	    break;
    if (ctb != CTB_USERID) {
	fclose(f);
	return -1;
    }
    if ((status = read_trust(f, &trust)) < 0) {
	fclose(f);
	return status;
    }
    if ((trust & KC_LEGIT_MASK) == KC_LEGIT_COMPLETE) {
	fclose(f);
	return 0;
    }
    if (ask_first) {
	/* shortcut for adding big keyfile */
	fprintf(pgpout,
	LANG("\nOne or more of the new keys are not fully certified.\n\
Do you want to certify any of these keys yourself (y/N)? "));
	if (!getyesno('n')) {
	    fclose(f);
	    return 1;
	}
    }
    ask_first = FALSE;
    show_key(f, fpos, SHOW_ALL | SHOW_HASH);
    fclose(f);
    PascalToC((char *) userid);
    fprintf(pgpout,
	    LANG("\nDo you want to certify this key yourself (y/N)? "));
    if (getyesno('n')) {
	if (signkey((char *) userid, my_name, ringfile) == 0)
	    maint_update(ringfile, 0);
    }
    return 0;
}

/**** faster version of getpublickey() ****/

static long find_keyID(byte * keyID);

static FILE *gpkf = NULL;

/*
 * speedup replacement for getpublickey(), does not have the arguments
 * giveup, showkey and keyfile (giveup = TRUE, showkey = FALSE, keyfile
 * is set with gpk_open().
 * only searches on keyID
 */
int get_publickey(long *file_position, int *pktlen, byte * keyID,
		  byte * timestamp, byte * userid, unitptr n, unitptr e)
{
    byte ctb;			/* returned by readkeypacket */
    int status, keystatus = -1;
    long fpos;

    if ((fpos = find_keyID(keyID)) == -1)
	return -1;
    fseek(gpkf, fpos, SEEK_SET);

    for (;;) {
	fpos = ftell(gpkf);
	status = readkeypacket(gpkf, FALSE, &ctb, timestamp,
			       (char *) userid, n, e,
			       NULL, NULL, NULL, NULL, NULL, NULL);
	/* Note that readkeypacket has called set_precision */

	if (status < 0 && status != -4 && status != -6)
	    return status;

	/* Remember packet position and size for last key packet */
	if (is_key_ctb(ctb)) {
	    if (file_position)
		*file_position = fpos;
	    if (pktlen)
		*pktlen = (int) (ftell(gpkf) - fpos);
	    if (keystatus != -1)
		return -3; /* should not happen, probably missing userid pkt */
	    keystatus = status;
	}
	if (ctb == CTB_USERID)
	    return keystatus;
    }
}

#define	PK_HASHSIZE	256	/* must be power of 2 */
#define	PK_HASH(x)		(*(byte *) (x) & (PK_HASHSIZE - 1))
#define	HASH_ALLOC	400

static VOID *allocbuf(int size);
static void freebufpool(void);

static struct hashent {
    struct hashent *next;
    byte keyID[KEYFRAGSIZE];
    long offset;
} **hashtbl = NULL, *hashptr;

static int hashleft = 0;

int gpk_open(char *keyfile)
{
    int status;
    long fpos = 0;
    byte keyID[KEYFRAGSIZE];
    byte ctb;

    if (gpkf) {
	fprintf(pgpout, "gpk_open: already open\n");
	return -1;
    }
    default_extension(keyfile, PGP_EXTENSION);
    if ((gpkf = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;		/* error return */
    hashtbl = allocbuf(PK_HASHSIZE * sizeof(struct hashent *));
    memset(hashtbl, 0, PK_HASHSIZE * sizeof(struct hashent *));
    while ((status = readkpacket(gpkf, &ctb, NULL, keyID, NULL)) != -1) {
	if (status == -2 || status == -3) {
	    fprintf(pgpout, LANG("\n\007Could not read key from file '%s'.\n"),
		    keyfile);
	    fclose(gpkf);	/* close key file */
	    return -1;
	}
	if (is_key_ctb(ctb)) {
	  if (status != -4) {
	    if (find_keyID(keyID) != -1)
		fprintf(pgpout,
			"Warning: duplicate key in keyring '%s'\n", keyfile);
	    if (!hashleft) {
		hashptr = allocbuf(HASH_ALLOC * sizeof(struct hashent));
		hashleft = HASH_ALLOC;
	    }
	    memcpy(hashptr->keyID, keyID, KEYFRAGSIZE);
	    hashptr->offset = fpos;
	    hashptr->next = hashtbl[PK_HASH(keyID)];
	    hashtbl[PK_HASH(keyID)] = hashptr;
	    ++hashptr;
	    --hashleft;
	  }
	}
	fpos = ftell(gpkf);
    }
    return 0;
}

void gpk_close(void)
{
    if (!gpkf)
	return;
    hashleft = 0;
    hashtbl = NULL;
    freebufpool();
    fclose(gpkf);		/* close key file */
    gpkf = NULL;
}

/*
 * Lookup file position in hash table by keyID, returns -1 if not found
 */
static long find_keyID(byte * keyID)
{
    struct hashent *p;

    for (p = hashtbl[PK_HASH(keyID)]; p; p = p->next)
	if (memcmp(keyID, p->keyID, KEYFRAGSIZE) == 0)
	    return p->offset;
    return -1;
}


static struct bufpool {
    struct bufpool *next;
    char buf[1];		/* variable size */
} *bufpool = NULL;

/*
 * allocate buffer, all buffers allocated with this function can be
 * freed with one call to freebufpool()
 */
static VOID *
 allocbuf(int size)
{
    struct bufpool *p;

    p = xmalloc(size + sizeof(struct bufpool *));
    p->next = bufpool;
    bufpool = p;
    return p->buf;
}

/*
 * free all memory obtained with allocbuf()
 */
static void freebufpool(void)
{
    struct bufpool *p;

    while (bufpool) {
	p = bufpool;
	bufpool = bufpool->next;
	free(p);
    }
}


/* ==== keymaint.c ==== */
/*      keymaint.c  - Keyring maintenance pass routines for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.

   keymaint.c implemented by Branko Lankester.
 */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "mpilib.h"
#include "random.h"
#include "crypto.h"
#include "fileio.h"
#include "keymgmt.h"
#include "keymaint.h"
#include "mpiio.h"
#include "charset.h"
#include "language.h"
#include "pgp.h"
#ifdef MACTC5
#include "Macutil3.h"
#include "PGPDialogs.h"
#include "MyBufferedStdio.h"
#include "ReplaceStdio.h"
#endif

#if 1				/* def DEBUG */
#include <assert.h>
#else
#define assert(x)
#endif

/* Helper functions to work on newkey lists */
void free_newkeys(struct newkey *nkeys)
{
    struct newkey *nkey;

    while (nkeys) {
	nkey = nkeys;
	nkeys = nkeys->next;
	free(nkey);
    }
}

int ismember_newkeys(byte const keyid[KEYFRAGSIZE], struct newkey const *nkeys)
{
    while (nkeys) {
	if (memcmp(keyid, nkeys->keyID, KEYFRAGSIZE) == 0)
	    return 1;
	nkeys = nkeys->next;
    }
    return 0;
}

/* The main checking code... */

struct userid;
struct signature;

struct pubkey {
    struct pubkey *pk_next;
    struct pubkey *pk_hash;	/* hash list for keyID */
    struct userid *pk_userids;
    struct signature *pk_signed;	/* signatures this key made */
    byte pk_keyid[KEYFRAGSIZE];
    byte pk_owntrust;
    byte pk_depth;		/* shortest cert. path to buckstop key */
};

struct userid {
    struct userid *uid_next;
    struct pubkey *uid_key;	/* backlink to key */
    struct signature *uid_signatures;
    char *uid_userid;
    byte uid_legit;
};

struct signature {
    struct signature *sig_next;	/* list of signatures on a userid */
    struct userid *sig_uid;	/* the userid it signs */
    struct pubkey *sig_from;	/* key that made this signature */
    /* list of sigs made by the same key (sig_from) */
    struct signature *sig_nextfrom;
    byte sig_trust;
};


int maint_list(char *ringfile);
void init_trust_lst(void);
long lookup_by_keyID(FILE * f, byte * srch_keyID);
void show_userid(FILE * f, byte * keyID);

static int maintenance(char *ringfile, struct newkey const *nkeys);
static int maint_read_data(char *ringfile, struct newkey const *nkeys);
static int maint_trace_chain(void);
static int trace_sig_chain(struct pubkey *pk, int depth);
static int maint_final(char *ringfile);
static struct pubkey *getpubkey(byte * keyID);
static void setup_trust(void);
static int check_secretkey(FILE * f, long keypos, byte keyctrl);
static void maint_init_mem(void);
static void maint_release_mem(void);
static VOID *allocn(int size);
static VOID *allocbuf(int size);
static void freebufpool(void);
static void compute_legit(struct userid *id);


#define	ALLOC_UNIT 4000	/* memory will be allocated in chunks of this size */

#define	MAX_DEPTH  8		/* max. value of max_cert_depth */

/* returned when trying to do a maintenance pass on a
   secret keyring or keyfile */
#define	ERR_NOTRUST	-7

#define TRUST_MASK	7	/* mask for userid/signature trust bytes */
#define SET_TRUST(b,v)	(*(b) = (*(b) & ~TRUST_MASK) | (v))
#define TRUST_LEV(b)	((b) & TRUST_MASK)

#define	TRUST_FAC(x)	(trust_tbl[TRUST_LEV(x)])

#define ctb_type(c)	((c&CTB_TYPE_MASK)>>2)
/*
 * table for tuning user paranoia index.
 * values represent contribution of one signature indexed by the
 * SIGTRUST of a signature
 */
static int trust_tbl[8];

static int marginal_min;
static int complete_min;	/* total count needed for a fully legit key */

int marg_min = 2;		/* number of marginally trusted signatures
				   needed for a fully legit key
				   (can be set in config.pgp). */
int compl_min = 1;		/* number of fully trusted signatures needed */

char trust_lst[8][16] =
{
    "undefined",		/* LANG("undefined") */
    "unknown",			/* LANG("unknown") */
    "untrusted",		/* LANG("untrusted") */
    "<3>",			/* unused */
    "<4>",			/* unused */
    "marginal",			/* LANG("marginal") */
    "complete",			/* LANG("complete") */
    "ultimate",			/* LANG("ultimate") */
};

char legit_lst[4][16] =
{
    "undefined",
    "untrusted",
    "marginal",
    "complete"
};

static int trustlst_len = 9;	/* length of longest trust word */
static int legitlst_len = 9;	/* length of longest legit word */

char floppyring[MAX_PATH] = "";
int max_cert_depth = 4;		/* maximum nesting of signatures */

static boolean check_only = FALSE;
static boolean mverbose;
static FILE *sec_fp;
static FILE *floppy_fp = NULL;
static int undefined_trust;	/* number of complete keys with undef. trust */

/*
 * Update trust parameters in a keyring, should be called after all
 * key management functions which can affect the trust parameters.
 * Changes are done "inplace", the file must be writable.
 *
 * nkeys is a list of new keys.  Any key on this list is checked to
 * see if it on the secret keyring.  If it is, and the BUCKSTOP bit
 * is not set, the user is prompted to set it.
 */
int maint_update(char *ringfile, struct newkey const *nkeys)
{
    check_only = mverbose = FALSE;
    return maintenance(ringfile, nkeys);
}

/*
 * Check trust parameters in ringfile
 * options can be:
 *      MAINT_CHECK     check only, don't ask if keyring should be updated
 *      MAINT_VERBOSE   verbose output, shows signature chains
 */
int maint_check(char *ringfile, int options)
{
    int status;
    char *fixfile;

    mverbose = ((options & MAINT_VERBOSE) != 0);

    if (moreflag)
	open_more();
    if (*floppyring != '\0' && (floppy_fp = fopen(floppyring, FOPRBIN))
	== NULL)
	fprintf(pgpout, LANG("\nCan't open backup key ring file '%s'\n"),
		floppyring);
    check_only = TRUE;
    status = maintenance(ringfile, NULL);
    if (floppy_fp) {
	fclose(floppy_fp);
	floppy_fp = NULL;
    }
    if (status <= 0) {
	if (status == 0)
	    maint_list(ringfile);
	close_more();
	return status;
    }
#ifdef xDEBUG
    if (status > 0 && (options & MAINT_CHECK)) {
	FILE *sav = pgpout;
	if (pgpout = fopen("before.lst", "w")) {
	    maint_list(ringfile);
	    fclose(pgpout);
	}
	pgpout = sav;
    }
#endif
    /* Inform user of trust parameters to be changed... */
    if (undefined_trust) {

	/* If we are just going to check, then exit now... */
	if (options & MAINT_CHECK) {
	    maint_list(ringfile);
	}
	fprintf(pgpout,
		LANG("\n%d \"trust parameter(s)\" need to be changed.\n"),
		undefined_trust);

	if (options & MAINT_CHECK) {
	    close_more();
	    return status;
	}
	fprintf(pgpout, LANG("Continue with '%s' (Y/n)? "),
		ringfile);
	if (!getyesno('y')) {
	    close_more();
	    return status;
	}
    }
    /* do the fixes in a scratch file */
    fixfile = tempfile(0);
    if (copyfiles_by_name(ringfile, fixfile) < 0) {
	close_more();
	return -1;
    }
    check_only = mverbose = FALSE;
    if ((status = maintenance(fixfile, NULL)) >= 0) {
	maint_list(fixfile);
	fprintf(pgpout,
		LANG("\n%d \"trust parameter(s)\" changed.\n"), status);
    }
    close_more();
    if (status > 0 && !(options & MAINT_CHECK)) {
	fprintf(pgpout, LANG("Update public keyring '%s' (Y/n)? "), ringfile);
	if (getyesno('y'))
	    return savetempbak(fixfile, ringfile);
    }
    rmtemp(fixfile);
    return status;
}				/* maint_check */


static int maintenance(char *ringfile, struct newkey const *nkeys)
{
    int status;
    undefined_trust = 0;	/* None so far... */

    if (max_cert_depth > MAX_DEPTH)
	max_cert_depth = MAX_DEPTH;
    if ((sec_fp = fopen(globalSecringName, FOPRBIN)) == NULL)
	fprintf(pgpout, LANG("\nCan't open secret key ring file '%s'\n"),
		globalSecringName);

    setkrent(ringfile);
    setup_trust();
    maint_init_mem();
    if (mverbose || verbose)
	fprintf(pgpout,
	 LANG("\nPass 1: Looking for the \"ultimately-trusted\" keys...\n"));
    status = maint_read_data(ringfile, nkeys);
    if (sec_fp) {
	fclose(sec_fp);
	sec_fp = NULL;
    }
    if (status < 0)
	goto failed;

    if (mverbose || verbose)
	fprintf(pgpout, LANG("\nPass 2: Tracing signature chains...\n"));
    if ((status = maint_trace_chain()) < 0)
	goto failed;

    if (verbose)
	fprintf(pgpout, "\nPass 3: %s keyring...\n",
		(check_only ? "Checking with" : "Updating"));
    if ((status = maint_final(ringfile)) < 0)
	goto failed;

    endkrent();
    maint_release_mem();
    return status + undefined_trust;

  failed:
    if (verbose)
	fprintf(pgpout, "maintenance pass: error exit = %d\n", status);
    endkrent();
    maint_release_mem();
    return status;
}				/* maintenance */


static struct pubkey *pklist, **pkhash = NULL;

#define	PK_HASHSIZE	256	/* must be power of 2 */
#define	PK_HASH(x)		(*(byte *) (x) & (PK_HASHSIZE - 1))

/*
 * get the pubkey struct for keyID from hash table, allocate a new
 * node and insert in hash table if necessary.
 */
static struct pubkey *
 getpubkey(byte * keyID)
{
    struct pubkey *pk;
    for (pk = pkhash[PK_HASH(keyID)]; pk; pk = pk->pk_hash)
	if (memcmp(pk->pk_keyid, keyID, KEYFRAGSIZE) == 0)
	    return pk;
    pk = allocn(sizeof(struct pubkey));
    memset(pk, 0, sizeof(struct pubkey));
    memcpy(pk->pk_keyid, keyID, KEYFRAGSIZE);
    pk->pk_hash = pkhash[PK_HASH(keyID)];
    pkhash[PK_HASH(keyID)] = pk;
    return pk;
}

/*
 * Read in keyring, a graph of keys, userids and signatures is built.
 * Also check if axiomatic keys are present in the secret keyring and
 * compare them with the floppy ring if this is requested.
 */
static int maint_read_data(char *ringfile, struct newkey const *nkeys)
{
    FILE *f;
    int status;
    char userid[256];
    byte keyID[KEYFRAGSIZE];
    byte sigkeyID[KEYFRAGSIZE];
    byte ctb;
    byte keyctrl;
    boolean buckstop = FALSE, show_user = FALSE;
    int buckstopcount = 0;
    long keypos = 0;
    int skip = 0;
    struct pubkey *pk = NULL;
    struct userid *id = NULL;
    struct signature *sig = NULL;

    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	return -1;
    }
    while ((status = readkpacket(f, &ctb, userid, keyID, sigkeyID)) != -1) {
	if (status == -3 || status == -2) {
	    fclose(f);
	    return status;
	}
	if (status < 0 || is_ctb_type(ctb, CTB_CERT_SECKEY_TYPE)) {
	    skip = 1;		/* version error or bad key */
	    continue;
	}
	if (skip) {
	    if (is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE))
		skip = 0;
	    else
		continue;
	}
	if (is_ctb_type(ctb, CTB_COMMENT_TYPE) || ctb == CTB_KEYCTRL)
	    continue;

	if (pk && is_ctb_type(ctb, CTB_SKE_TYPE) && !pk->pk_userids) {
	    /* sig. cert before userids can only be compromise cert. */
	    pk->pk_owntrust = KC_OWNERTRUST_NEVER;
	    continue;
	}
	/* other packets should have trust byte */
	if (read_trust(f, &keyctrl) < 0) {
	    fclose(f);
	    return ERR_NOTRUST;	/* not a public keyring */
	}
	switch (ctb_type(ctb)) {
	case CTB_CERT_PUBKEY_TYPE:
	    if (pk)
		pk = pk->pk_next = getpubkey(keyID);
	    else
		pk = pklist = getpubkey(keyID);

	    if (pk->pk_next) {
		fprintf(pgpout,
			LANG("Keyring contains duplicate key: %s\n"),
			keyIDstring(keyID));
		fclose(f);
		return -1;
	    }
	    if (keyctrl & KC_BUCKSTOP ||
		ismember_newkeys(keyID, nkeys)) {
		if (check_secretkey(f, keypos, keyctrl) == 0) {
		    ++buckstopcount;
		    keyctrl |= KC_BUCKSTOP;
		    SET_TRUST(&keyctrl, KC_OWNERTRUST_ULTIMATE);
		    buckstop = TRUE;
		    if (mverbose)
			fprintf(pgpout, "* %s", keyIDstring(keyID));
		} else {	/* not in secret keyring */
		    keyctrl &= ~KC_BUCKSTOP;
		    if (TRUST_LEV(keyctrl) == KC_OWNERTRUST_ULTIMATE)
			keyctrl = KC_OWNERTRUST_ALWAYS;
		    if (mverbose)
			fprintf(pgpout, ". %s", keyIDstring(keyID));
		}
		show_user = mverbose;
	    } else {
		buckstop = FALSE;
		show_user = FALSE;
	    }
	    pk->pk_owntrust = keyctrl;
	    pk->pk_userids = id = NULL;
	    break;
	case CTB_USERID_TYPE:
#ifdef MACTC5
		mac_poll_for_break();
#endif
	    if (!pk)
		break;
	    if (show_user) {
		if (pk->pk_userids)	/* more than one user ID */
		    fprintf(pgpout, "        ");
		fprintf(pgpout, "  %s\n", LOCAL_CHARSET(userid));
	    }
	    if (id)
		id = id->uid_next = allocn(sizeof(struct userid));
	    else
		id = pk->pk_userids = allocn(sizeof(struct userid));

	    if (mverbose)
		id->uid_userid = store_str(userid);
	    keyctrl &= ~KC_LEGIT_MASK;
	    if (buckstop)
		keyctrl |= KC_LEGIT_COMPLETE;
	    else
		keyctrl |= KC_LEGIT_UNKNOWN;
	    id->uid_next = NULL;
	    id->uid_key = pk;
	    id->uid_legit = keyctrl;
	    id->uid_signatures = sig = NULL;
	    break;
	case CTB_SKE_TYPE:
	    if (!pk || !id)
		break;
	    if (sig)
		sig = sig->sig_next = allocn(sizeof(struct signature));
	    else
		sig = id->uid_signatures = allocn(sizeof(struct signature));
	    sig->sig_next = NULL;
	    sig->sig_uid = id;
	    sig->sig_from = getpubkey(sigkeyID);
	    sig->sig_nextfrom = sig->sig_from->pk_signed;
	    sig->sig_from->pk_signed = sig;
	    sig->sig_trust = keyctrl & KC_SIG_CHECKED;
	    break;
	}			/* switch ctb_type */
	keypos = ftell(f);
    }
    if (buckstopcount == 0 && mverbose)
	fprintf(pgpout, LANG("No ultimately-trusted keys.\n"));
    fclose(f);
    return 0;
}				/* maint_read_data */

/*
 * scan keyring for buckstop keys and start the recursive trace_sig_chain()
 * on them
 */
static int maint_trace_chain(void)
{
    char *userid;
    struct pubkey *pk;

    for (pk = pklist; pk; pk = pk->pk_next) {
	if (!(pk->pk_owntrust & KC_BUCKSTOP))
	    continue;
	if (mverbose)
	    fprintf(pgpout,
		    "* %s\n", LOCAL_CHARSET(pk->pk_userids->uid_userid));
	if (TRUST_LEV(pk->pk_owntrust) == KC_OWNERTRUST_UNDEFINED) {
	    userid = user_from_keyID(pk->pk_keyid);
	    SET_TRUST(&pk->pk_owntrust, ask_owntrust(userid, pk->pk_owntrust));
	}
	trace_sig_chain(pk, 0);
    }
    return 0;
}				/* maint_trace_chain */


/*
 * Find all signatures made with the key pk.
 * If a trusted signature makes a key fully legit then signatures made
 * with this key are also recursively traced on down the tree.
 *
 * depth is the level of recursion, it is used to indent the userIDs
 * and to check if we don't exceed the limit "max_cert_depth"
 *
 * NOTE: a signature made with a key with pk_depth == max_cert_depth will
 * not be counted here to limit the maximum chain length, but will be
 * counted when the validity of a key is computed in maint_final()
 */
static int trace_sig_chain(struct pubkey *pk, int depth)
{
    int d, trust_count = 0;
    int counts[MAX_DEPTH];
    struct signature *sig, *s;
    struct pubkey *p;
    struct userid *id;

    assert(depth <= max_cert_depth);
    if (pk->pk_depth && pk->pk_depth <= depth)
	return 0;
    pk->pk_depth = depth;

    /* Should we ask for trust.  If this key is legit, then go for
     * it!  Ask the user....
     */
    if (TRUST_LEV(pk->pk_owntrust) == KC_OWNERTRUST_UNDEFINED)
	for (id = pk->pk_userids; id; id = id->uid_next) {
	    compute_legit(id);
	    if ((id->uid_legit & KC_LEGIT_MASK) ==
		KC_LEGIT_COMPLETE) {
		SET_TRUST(&pk->pk_owntrust,
			  ask_owntrust(user_from_keyID(pk->pk_keyid),
				       pk->pk_owntrust));
		break;
	    }
	}
    /* Return if I haven't signed anyone's keys, since I
     * don't need to check any further..  -warlord 93-04-11
     */
    if (!pk->pk_signed)
	return 0;

#ifdef DEBUG
    if (mverbose)
	fprintf(pgpout, "%*s%d-v  %s\n", 2 * depth, "",
		depth, pk->pk_userids->uid_userid);
#endif

    /* all keys signed by pk */
    for (sig = pk->pk_signed; sig; sig = sig->sig_nextfrom) {

	/* If signature is good, copy trust from signator */
	/* CONTIG bit currently unused */
	if (sig->sig_trust & KC_SIG_CHECKED) {
	    SET_TRUST(&sig->sig_trust, TRUST_LEV(pk->pk_owntrust));
	    sig->sig_trust |= KC_CONTIG; /* CONTIG bit currently unused */
	    if (mverbose)
		fprintf(pgpout, "%*s  > %s\n", 2 * depth, "",
			LOCAL_CHARSET(sig->sig_uid->uid_userid));
	} else {
	    SET_TRUST(&sig->sig_trust, KC_SIGTRUST_UNTRUSTED);
	    sig->sig_trust &= ~KC_CONTIG;
	    if (mverbose)
		fprintf(pgpout, "%*s  X %s\n", 2 * depth, "",
			LOCAL_CHARSET(sig->sig_uid->uid_userid));
	}

	if (TRUST_FAC(sig->sig_trust) == 0)
	    continue;
	p = sig->sig_uid->uid_key;	/* this key signed by pk */
	if (p->pk_owntrust & KC_BUCKSTOP)
	    continue;		/* will be handled from main loop */
	if (p->pk_depth && p->pk_depth <= depth + 1)
	    continue;		/* already handled this key at a lower level */

	for (d = 0; d < max_cert_depth; ++d)
	    counts[d] = 0;
	for (s = sig->sig_uid->uid_signatures; s; s = s->sig_next) {
	    d = s->sig_from->pk_depth;
	    if (d < max_cert_depth)
		counts[d] += TRUST_FAC(s->sig_trust);
	}
	/*
	 * find a combination of signatures that will make the key
	 * valid through the shortest cert. path.
	 */
	trust_count = 0;
	for (d = 0; d < max_cert_depth; ++d) {
	    trust_count += counts[d];
	    if (trust_count >= complete_min) {
		trace_sig_chain(p, d + 1);
		break;
	    }
	}
    }

#ifdef DEBUG
    if (mverbose)
	fprintf(pgpout, "%*s%d-^  %s\n", 2 * depth, "",
		depth, pk->pk_userids->uid_userid);
#endif
    return 0;
}				/* trace_sig_chain */

/*
 * compute validity of userid/key pair, the number of signatures and the
 * trust level of these signatures determines the validity.
 */
static void compute_legit(struct userid *id)
{
    struct signature *s;
    int trust_count, legit;

    if (id->uid_key->pk_owntrust & KC_BUCKSTOP)
	legit = KC_LEGIT_COMPLETE;
    else {
	trust_count = 0;
	for (s = id->uid_signatures; s; s = s->sig_next)
	    trust_count += TRUST_FAC(s->sig_trust);

	if (trust_count == 0)
	    legit = KC_LEGIT_UNKNOWN;
	else if (trust_count < marginal_min)
	    legit = KC_LEGIT_UNTRUSTED;
	else if (trust_count < complete_min)
	    legit = KC_LEGIT_MARGINAL;
	else
	    legit = KC_LEGIT_COMPLETE;
    }
    id->uid_legit = (id->uid_legit & ~KC_LEGIT_MASK) | legit;
}				/* compute_legit */

/* 
 * check if the maintenance pass changed anything
 * returns 0 if files f and g are equal and the number of changed
 * trust bytes if the files are different or a negative value on error
 */
static int maint_final(char *ringfile)
{
    int status;
    FILE *f;
    long trust_pos = 0;
    char userid[256];
    byte keyID[KEYFRAGSIZE];
    byte sigkeyID[KEYFRAGSIZE];
    byte ctb;
    byte kc_orig, kc_new = 0, mask;
    int changed = 0;
    int skip = 0;
    struct pubkey *pk;
    struct userid *id = NULL;
    struct signature *sig = NULL;

    if (check_only)
	f = fopen(ringfile, FOPRBIN);
    else
	f = fopen(ringfile, FOPRWBIN);
    if (f == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	return -1;
    }
    pk = pklist;
    while ((status = readkpacket(f, &ctb, userid, keyID, sigkeyID)) != -1) {
	if (status == -3 || status == -2)
	    break;
	if (status < 0 || is_ctb_type(ctb, CTB_CERT_SECKEY_TYPE)) {
	    skip = 1;
	    continue;
	}
	if (skip) {
	    if (is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE))
		skip = 0;
	    else
		continue;
	}
	if (is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE) ||
	    is_ctb_type(ctb, CTB_SKE_TYPE) || ctb == CTB_USERID) {
	    trust_pos = ftell(f);
	    if (read_trust(f, &kc_orig) < 0) {
		status = ERR_NOTRUST;
		if (is_ctb_type(ctb, CTB_SKE_TYPE))
		    continue;	/* skip compr. cert. */
		else
		    break;
	    }
	}
	switch (ctb_type(ctb)) {
	case CTB_CERT_PUBKEY_TYPE:
	    assert(pk && !memcmp(pk->pk_keyid, keyID, KEYFRAGSIZE));
	    assert(!sig && !id);
	    id = pk->pk_userids;
	    kc_new = pk->pk_owntrust;
#ifdef DEBUG
	    if (mverbose)
		fprintf(pgpout, "  ------ %d\n", pk->pk_depth);
#endif
	    pk = pk->pk_next;
	    mask = KC_OWNERTRUST_MASK | KC_BUCKSTOP;
	    break;
	case CTB_USERID_TYPE:
	    assert(id && !sig);
#ifdef MACTC5
	    mac_poll_for_break();
#endif
	    sig = id->uid_signatures;
	    compute_legit(id);
	    kc_new = id->uid_legit;
#ifdef DEBUG
	    if (mverbose)
		fprintf(pgpout, "%c %02x  %02x  %s\n",
			' ' + (kc_new != kc_orig),
			kc_orig, kc_new, id->uid_userid);
#endif
	    id = id->uid_next;
	    mask = KC_LEGIT_MASK;
	    break;
	case CTB_SKE_TYPE:
	    assert(sig);
	    assert(!memcmp(sig->sig_from->pk_keyid, sigkeyID, KEYFRAGSIZE));
	    kc_new = sig->sig_trust;
#ifdef DEBUG
	    if (mverbose && sig->sig_from->pk_userids)
		fprintf(pgpout, "%c %02x  %02x    %s\n",
			' ' + (kc_new != kc_orig),
		 kc_orig, kc_new, sig->sig_from->pk_userids->uid_userid);
#endif
	    sig = sig->sig_next;
	    mask = KC_SIGTRUST_MASK | KC_CONTIG;
	    break;
	default:
	    mask = 0;
	}
	if ((kc_new & mask) != (kc_orig & mask)) {
	    if (!check_only)
		write_trust_pos(f, kc_new, trust_pos);
	    ++changed;
	}
    }
    fclose(f);
    if (status < -1)		/* -1 is OK, EOF */
	return status;
    if (pk || sig || id) {
	fprintf(pgpout, "maint_final: internal error\n");
	return -1;
    }
    return changed;
}				/* maint_final */

int maint_list(char *ringfile)
{
    int status;
    FILE *f;
    char userid[256];
    byte keyID[KEYFRAGSIZE];
    byte sigkeyID[KEYFRAGSIZE];
    char *signator;
    char tchar = 0;
    byte ctb, kc;
    int owntrust = 0;
    int usercount = 0;

    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	return -1;
    }
    init_trust_lst();
    setkrent(ringfile);
    init_userhash();

    fprintf(pgpout, LANG("  KeyID    Trust     Validity  User ID\n"));
    while ((status = readkpacket(f, &ctb, userid, keyID, sigkeyID)) != -1) {
	if (status == -3 || status == -2)
	    break;
	if (status < 0)
	    continue;

	if (is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE) ||
	    is_ctb_type(ctb, CTB_SKE_TYPE) || ctb == CTB_USERID) {
	    if (read_trust(f, &kc) < 0) {
		status = ERR_NOTRUST;
		/* compromise cert. don't have trust byte */
		if (!is_ctb_type(ctb, CTB_SKE_TYPE))
		    break;
	    }
	}
#ifdef MACTC5
	{
		extern int BreakCntl;
		if (!BreakCntl) BreakCntl = 35;
		mac_poll_for_break();
	}
#endif

	switch (ctb_type(ctb)) {
	case CTB_CERT_PUBKEY_TYPE:
	    tchar = (kc & KC_BUCKSTOP ? '*' : ' ');
	    owntrust = TRUST_LEV(kc);
	    usercount = 0;
	    userid[0] = '\0';
	    break;
	case CTB_USERID_TYPE:
	    if (!usercount) {	/* first userid */
		fprintf(pgpout, "%c %s ", tchar, keyIDstring(keyID));
		fprintf(pgpout, "%-*s ", trustlst_len, trust_lst[owntrust]);
	    } else
		fprintf(pgpout, "  %s %*s ", blankkeyID, trustlst_len, "");
	    fprintf(pgpout, "%-*s ", legitlst_len,
		    legit_lst[kc & KC_LEGIT_MASK]);
	    if (usercount)
		putc(' ', pgpout);
	    ++usercount;
	    fprintf(pgpout, "%s\n", LOCAL_CHARSET(userid));
	    break;
	case CTB_SKE_TYPE:
	    if (!usercount) {	/* sig before userid: compromise cert. */
		tchar = '#';
		break;
	    }
	    fprintf(pgpout, "%c %s ",
		    (kc & KC_CONTIG) ? 'c' : ' ', blankkeyID);
	    fprintf(pgpout, "%-*s ", trustlst_len, trust_lst[TRUST_LEV(kc)]);
	    fprintf(pgpout, "%*s  ", legitlst_len, "");
	    if ((signator = user_from_keyID(sigkeyID)) == NULL)
		fprintf(pgpout, LANG("(KeyID: %s)\n"), keyIDstring(sigkeyID));
	    else
		fprintf(pgpout, "%s\n", LOCAL_CHARSET(signator));
	    break;
	}
    }
    endkrent();
    fclose(f);
    if (status < -1)		/* -1 is OK, EOF */
	return status;
    return 0;
}				/* maint_list */

/*
 * translate the messages in the arrays trust_lst and legit_lst.
 * trustlst_len and legitlst_len will be set to the length of
 * the longest translated string.
 */
void init_trust_lst(void)
{
    static int initialized = 0;
    int i, len;
    char *s;

    if (initialized)
	return;
    for (i = 0; i < 8; ++i) {
	if (trust_lst[i][0]) {
	    s = LANG(trust_lst[i]);
	    if (s != trust_lst[i])
		strncpy(trust_lst[i], s, sizeof(trust_lst[0]) - 1);
	    len = strlen(s);
	    if (len > trustlst_len)
		trustlst_len = len;
	}
    }
    for (i = 0; i < 4; ++i) {
	s = LANG(legit_lst[i]);
	if (s != legit_lst[i])
	    strncpy(legit_lst[i], s, sizeof(legit_lst[0]) - 1);
	len = strlen(s);
	if (len > legitlst_len)
	    legitlst_len = len;
    }
    initialized = 1;
}				/* init_trust_lst */



/*
 * compare the key in file f at keypos with the matching key in the
 * secret keyring, the global variable sec_fp must contain the file pointer
 * for of the secret keyring.
 *
 * returns 1 if the key was not found, -2 if the keys were different
 * and 0 if the keys compared OK
 */
static int check_secretkey(FILE * f, long keypos, byte keyctrl)
{
    int status = -1;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    unit nsec[MAX_UNIT_PRECISION], esec[MAX_UNIT_PRECISION];
    char userid[256];
    byte keyID[KEYFRAGSIZE];
    long savepos, pktlen;
    byte ctb;

    if (sec_fp == NULL)
	return -1;

    savepos = ftell(f);
    fseek(f, keypos, SEEK_SET);
    if (readkeypacket(f, FALSE, &ctb, NULL, NULL, n, e,
		      NULL, NULL, NULL, NULL, NULL, NULL) < 0)
	goto ex;
    extract_keyID(keyID, n);

    do {			/* get userid */
	status = readkpacket(f, &ctb, userid, NULL, NULL);
	if (status == -1 || status == -3)
	    goto ex;
    } while (ctb != CTB_USERID);

    if (lookup_by_keyID(sec_fp, keyID) < 0) {
#if 0
	if (!check_only) {
	    fprintf(pgpout,
LANG("\nAn \"axiomatic\" key is one which does not need certifying by\n\
anyone else.  Usually this special status is reserved only for your\n\
own keys, which should also appear on your secret keyring.  The owner\n\
of an axiomatic key (who is typically yourself) is \"ultimately trusted\"\n\
by you to certify any or all other keys.\n"));
	    fprintf(pgpout, LANG("\nKey for user ID \"%s\"\n\
is designated as an \"ultimately-trusted\" introducer, but the key\n\
does not appear in the secret keyring.\n\
Use this key as an ultimately-trusted introducer (y/N)? "),
		    LOCAL_CHARSET(userid));
	    status = (getyesno('n') ? 0 : 1);
	}
#else
	status = 1;
#endif
    } else {
	long kpos = ftell(sec_fp);
	if (readkeypacket(sec_fp, FALSE, &ctb, NULL, NULL, nsec, esec,
			  NULL, NULL, NULL, NULL, NULL, NULL) < 0) {
	    fprintf(pgpout, LANG("\n\007Cannot read from secret keyring.\n"));
	    status = -3;
	    goto ex;
	}
	if (mp_compare(n, nsec) || mp_compare(e, esec)) {
	    /* Red Alert! */
	    fprintf(pgpout,
		    LANG("\n\007WARNING: Public key for user ID: \"%s\"\n\
does not match the corresponding key in the secret keyring.\n"),
		    LOCAL_CHARSET(userid));
	    fprintf(pgpout,
LANG("This is a serious condition, indicating possible keyring tampering.\n"));
	    status = -2;
	} else {
	    status = 0;
	}

	/* Okay, key is in secret key ring, and it matches. */
	if (!(keyctrl & KC_BUCKSTOP)) {
	    if (batchmode) {
		status = -1;
	    } else {
		fprintf(pgpout, LANG("\nKey for user ID \"%s\"\n\
also appears in the secret key ring."), LOCAL_CHARSET(userid));
		fputs(
	  LANG("\nUse this key as an ultimately-trusted introducer (y/N)? "),
		      pgpout);
		status = getyesno('n') ? 0 : -1;
	    }
	}
	if (status == 0 && floppy_fp) {
	    if (lookup_by_keyID(floppy_fp, keyID) < 0) {
		fprintf(pgpout, LANG("Public key for: \"%s\"\n\
is not present in the backup keyring '%s'.\n"),
			LOCAL_CHARSET(userid), floppyring);
	    } else {
		pktlen = ftell(sec_fp) - kpos;
		fseek(sec_fp, kpos, SEEK_SET);
		while (--pktlen >= 0 && getc(sec_fp) == getc(floppy_fp));
		if (pktlen != -1) {
		    fprintf(pgpout,
			    LANG("\n\007WARNING: Secret key for: \"%s\"\n\
does not match the key in the backup keyring '%s'.\n"),
			    LOCAL_CHARSET(userid), floppyring);
		    fprintf(pgpout,
LANG("This is a serious condition, indicating possible keyring tampering.\n"));
		    status = -2;
		}
	    }
	}
    }
  ex:
    fseek(f, savepos, SEEK_SET);
    return status;
}				/* check_secretkey */

/*
 * setup tables for trust scoring.
 */
static void setup_trust(void)
{
    /* initialize trust table */
    if (marg_min == 0) {	/* marginally trusted signatures are ignored */
	trust_tbl[5] = 0;
	trust_tbl[6] = 1;
	complete_min = compl_min;
    } else {
	if (marg_min < compl_min)
	    marg_min = compl_min;
	trust_tbl[5] = compl_min;
	trust_tbl[6] = marg_min;
	complete_min = compl_min * marg_min;
    }
    trust_tbl[7] = complete_min;	/* ultimate trust */
    marginal_min = complete_min / 2;
}				/* setup_trust */

/* Ask for a wetware decision from the human on how much to trust 
   this key's owner to certify other keys.  Returns trust value. */
int ask_owntrust(char *userid, byte cur_trust)
{
    char buf[8];

    if (check_only || filter_mode || batchmode) {
	/* not interactive */
	++undefined_trust;	/* We complete/undefined. Why?  */
	return KC_OWNERTRUST_UNDEFINED;
    }
#ifdef MACTC5
	get_trust(buf,userid);
#endif
    fprintf(pgpout,
LANG("\nMake a determination in your own mind whether this key actually\n\
belongs to the person whom you think it belongs to, based on available\n\
evidence.  If you think it does, then based on your estimate of\n\
that person's integrity and competence in key management, answer\n\
the following question:\n"));
    fprintf(pgpout, LANG("\nWould you trust \"%s\"\n\
to act as an introducer and certify other people's public keys to you?\n\
(1=I don't know. 2=No. 3=Usually. 4=Yes, always.) ? "),
	    LOCAL_CHARSET(userid));
    fflush(pgpout);
#ifdef MACTC5
	Putchar(buf[0]);
	Putchar('\n');
#else
    getstring(buf, sizeof(buf) - 1, TRUE);
#endif
    switch (buf[0]) {
    case '1':
	return KC_OWNERTRUST_UNKNOWN;
    case '2':
	return KC_OWNERTRUST_NEVER;
    case '3':
	return KC_OWNERTRUST_USUALLY;
    case '4':
	return KC_OWNERTRUST_ALWAYS;
    default:
	return TRUST_LEV(cur_trust);
    }
}				/* ask_owntrust */

/*
 * scan keyfile f for keyID srch_keyID.
 * returns the file position of the key if it is found, and sets the
 * file pointer to the start of the key packet.
 * returns -1 if the key was not found or < -1 if there was an error
 */
long lookup_by_keyID(FILE * f, byte * srch_keyID)
{
    int status;
    long keypos = 0;
    byte keyID[KEYFRAGSIZE];
    byte ctb;

    rewind(f);
    while ((status = readkpacket(f, &ctb, NULL, keyID, NULL)) != -1) {
	if (status == -3 || status == -2)
	    break;
	if (status < 0)
	    continue;
	if (is_key_ctb(ctb) && memcmp(keyID, srch_keyID, KEYFRAGSIZE) == 0) {
	    fseek(f, keypos, SEEK_SET);
	    return keypos;
	}
	keypos = ftell(f);
    }
    return status;
}				/* lookup_by_keyID */

/*
 * look up the key matching "keyID" and print the first userID
 * of this key.  File position of f is saved.
 */
void show_userid(FILE * f, byte * keyID)
{
    int status;
    long filepos;
    char userid[256];
    byte ctb;

    filepos = ftell(f);
    if (lookup_by_keyID(f, keyID) >= 0)
	while ((status = readkpacket(f, &ctb, userid, NULL, NULL)) != -1
	       && status != -3)
	    if (ctb == CTB_USERID) {
		fprintf(pgpout, "%s\n", LOCAL_CHARSET(userid));
		fseek(f, filepos, SEEK_SET);
		return;
	    }
    fprintf(pgpout, LANG("(KeyID: %s)\n"), keyIDstring(keyID));
    fseek(f, filepos, SEEK_SET);
}				/* show_userid */

/*
 * messages printed by show_key()
 */
static char *owntrust_msg[] =
{
    "",				/* Just don't say anything in this case */
    "",
    _LANG("This user is untrusted to certify other keys.\n"),
    "",				/* reserved */
    "",				/* reserved */
    _LANG("This user is generally trusted to certify other keys.\n"),
    _LANG("This user is completely trusted to certify other keys.\n"),
    _LANG("This axiomatic key is ultimately trusted to certify other keys.\n"),
};
static char *keylegit_msg[] =
{
    _LANG("This key/userID association is not certified.\n"),
    _LANG("This key/userID association is not certified.\n"),
    _LANG("This key/userID association is marginally certified.\n"),
    _LANG("This key/userID association is fully certified.\n"),
};
static char *sigtrust_msg[] =
{
    _LANG("  Questionable certification from:\n  "),
    _LANG("  Questionable certification from:\n  "),
    _LANG("  Untrusted certification from:\n  "),
    "",				/* reserved */
    "",				/* reserved */
    _LANG("  Generally trusted certification from:\n  "),
    _LANG("  Completely trusted certification from:\n  "),
    _LANG("  Axiomatically trusted certification from:\n  "),
};

/*
 * show the key in file f at file position keypos.
 * 'what' controls the info that will be shown:
 *   SHOW_TRUST: show trust byte info
 *   SHOW_SIGS:  show signatures
 *   SHOW_HASH:  show key fingerprint
 * these constants can be or'ed
 *
 * 'what' can also be SHOW_LISTFMT to get the same format as for pgp -kv
 * no signatures or extra userids will be printed in this case.
 *
 * 'what' can be SHOW_CHANGE, in which case it will take the keyID and
 * call show_update();
 */
int show_key(FILE * f, long keypos, int what)
{
    int status, keystatus = -1;
    long filepos;
    char userid[256];
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte sigkeyID[KEYFRAGSIZE];
    word32 timestamp;
    byte ctb, keyctb = 0, keyctrl;
    int userids = 0;
    int keyids = 0;
    byte savekeyID[KEYFRAGSIZE];
    boolean print_trust = FALSE;
    byte hash[16];
    int precision = global_precision;
    int compromised = 0;
    int disabled = 0;

    filepos = ftell(f);
    fseek(f, keypos, SEEK_SET);
    while ((status = readkeypacket(f, FALSE, &ctb, (byte *) & timestamp,
				   userid,
	      n, e, NULL, NULL, NULL, NULL, sigkeyID, &keyctrl)) != -1) {
	if (status == -2 || status == -3)
	    break;
	if (is_key_ctb(ctb)) {

	    if (keyids)
		break;
	    extract_keyID(savekeyID, n);
	    keyids++;
	    if (what & SHOW_HASH)
		getKeyHash(hash, n, e);
	    keyctb = ctb;
	    keystatus = status;	/* remember status, could be version error */

	} else if (ctb == CTB_KEYCTRL) {

	    /* trust bytes only in public keyrings */
	    if (keystatus >= 0 && !userids)	/* key packet trust byte */
		if (keyctrl & KC_DISABLED)
		    disabled = 1;
	    if (what & SHOW_TRUST)
		print_trust = TRUE;

	} else if (ctb == CTB_USERID) {

	    if (userids == 0) {
		PascalToC(userid);	/* for display */
		++userids;
		if (what & SHOW_CHANGE) {
		    show_update(key2IDstring(n));
		    break;
		}
		if (what & SHOW_LISTFMT) {
		    if (is_ctb_type(keyctb, CTB_CERT_PUBKEY_TYPE))
			fprintf(pgpout, LANG("pub"));
		    else if (is_ctb_type(keyctb, CTB_CERT_SECKEY_TYPE))
			fprintf(pgpout, LANG("sec"));
		    else
			fprintf(pgpout, "???");
		    if (keystatus < 0)
			fprintf(pgpout, "? ");
		    else if (compromised)
			fprintf(pgpout, "# ");
		    else if (disabled)
			fprintf(pgpout, "- ");
		    else
			fprintf(pgpout, "  ");
		    fprintf(pgpout, "%4d/%s %s  ",
		       countbits(n), key2IDstring(n), cdate(&timestamp));
		    fprintf(pgpout, "%s\n", LOCAL_CHARSET(userid));
		    break;	/* only print default userid */
		}
		fprintf(pgpout, LANG("\nKey for user ID: %s\n"),
			LOCAL_CHARSET(userid));
		fprintf(pgpout, LANG("%d-bit key, key ID %s, created %s\n"),
			countbits(n), key2IDstring(n), cdate(&timestamp));
		if (keystatus == -4)
		    fprintf(pgpout, LANG("Bad key format.\n"));
		else if (keystatus == -6)
		    fprintf(pgpout, LANG("Unrecognized version.\n"));
		else if (what & SHOW_HASH)
		    printKeyHash(hash, FALSE);
		if (compromised)
		    fprintf(pgpout, LANG("Key has been revoked.\n"));
		if (disabled)
		    fprintf(pgpout, LANG("Key is disabled.\n"));
		if (print_trust && *owntrust_msg[TRUST_LEV(keyctrl)] != '\0')
		    fprintf(pgpout, LANG(owntrust_msg[TRUST_LEV(keyctrl)]));
	    } else {
		PascalToC(userid);
		if (what != 0)
		    fprintf(pgpout, "\n");
		fprintf(pgpout, LANG("Also known as: %s\n"),
			LOCAL_CHARSET(userid));
	    }
	    if (print_trust) {
		read_trust(f, &keyctrl);
		fprintf(pgpout, LANG(keylegit_msg[keyctrl & KC_LEGIT_MASK]));
	    }			/* print_trust */
	} else if (is_ctb_type(ctb, CTB_SKE_TYPE)) {

	    if (userids == 0)
		compromised = 1;
	    if (what & SHOW_CHANGE) {
		show_update(key2IDstring(n));
		break;
	    }
	    if (what & SHOW_SIGS) {
		if (print_trust) {
		    read_trust(f, &keyctrl);
		    fprintf(pgpout, LANG(sigtrust_msg[TRUST_LEV(keyctrl)]));
		} else {
		    fprintf(pgpout, LANG("  Certified by: "));
		}
		show_userid(f, sigkeyID);
	    }
	}
    }
    if (status == -1 && userids)
	status = 0;
    if (!userids && !compromised && (what != SHOW_CHANGE)) {
    	status = -1;
	fprintf(pgpout, LANG("\nWarning: keyid %4d/%s %s  has no user id!\n"),
		countbits(n), keyIDstring(savekeyID), cdate(&timestamp));
    }
    set_precision(precision);
    fseek(f, filepos, SEEK_SET);
    return status;
}				/* show_key */

/* show_update -- this function just prints an update message to
 * pgpout to inform the user that an update happened.
 */
void show_update(char *s)
{
    fprintf(pgpout, LANG("Updated keyID: 0x%s\n"), s);
}

/*
 * stripped down version of readkeypacket(), the output userid
 * is a null terminated string.
 */
int readkpacket(FILE * f, byte * ctb, char *userid,
		byte * keyID, byte * sigkeyID)
{
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];

    status = readkeypacket(f, FALSE, ctb, NULL, userid, n, e,
			   NULL, NULL, NULL, NULL, sigkeyID, NULL);

    if (status < 0) {
#ifdef DEBUG
	if (status < -1)
	    fprintf(stderr, "readkeypacket returned %d\n", status);
#endif
	return status;
    }
    if (keyID && is_key_ctb(*ctb))
	extract_keyID(keyID, n);

    if (userid && *ctb == CTB_USERID)
	PascalToC(userid);

    return 0;
}				/* readkpacket */

/*
 * write trust byte "keyctrl" to file f at file position "pos"
 */
void write_trust_pos(FILE * f, byte keyctrl, long pos)
{
    long fpos;

    fpos = ftell(f);
    fseek(f, pos, SEEK_SET);
    write_trust(f, keyctrl);
    fseek(f, fpos, SEEK_SET);
}				/* write_trust_pos */

/*
 * read a trust byte packet from file f, the trust byte will be
 * stored in "keyctrl".
 * returns -1 on EOF, -3 on corrupt input, and ERR_NOTRUST if
 * the packet was not a trust byte (this can be used to check if
 * a file is a keyring (with trust bytes) or a keyfile).
 * The current file position is left unchanged in this case.
 */
int read_trust(FILE * f, byte * keyctrl)
{
    unsigned char buf[3];

    if (fread(buf, 1, 3, f) != 3)
	return -1;
    if (buf[0] != CTB_KEYCTRL) {
	if (is_ctb(buf[0])) {
	    fseek(f, -3L, SEEK_CUR);
	    return ERR_NOTRUST;
	} else
	    return -3;		/* bad data */
    }
    if (buf[1] != 1)		/* length must be 1 */
	return -3;
    if (keyctrl)
	*keyctrl = buf[2];
    return 0;
}				/* read_trust */



/****** userid lookup ******/

#define	HASH_ALLOC	(ALLOC_UNIT / sizeof(struct hashent))

static VOID *allocbuf(int size);
static void freebufpool();

static struct hashent {
    struct hashent *next;
    byte keyID[KEYFRAGSIZE];
    char *userid;
} **hashtbl = NULL, *hashptr;

static char *strptr;
static int strleft = 0;
static int hashleft = 0;
static int nleft = 0;

#define MAXKR	8	/* max. number of keyrings for user_from_keyID() */
static char *krnames[MAXKR];
static int nkr = 0;

/*
 * Lookup userid by keyID without using the in-memory hash table.
 */
static char *
 _user_from_keyID(byte * srch_keyID)
{
    FILE *f;
    int i, status, found = 0;
    byte keyID[KEYFRAGSIZE];
    static char userid[256];
    byte ctb;

    /* search all keyfiles set with setkrent() */
    for (i = 0; !found && i < nkr; ++i) {
	if ((f = fopen(krnames[i], FOPRBIN)) == NULL)
	    continue;
	while ((status = readkpacket(f, &ctb, userid, keyID, NULL)) != -1) {
	    if (status == -2 || status == -3)
		break;
	    if (is_key_ctb(ctb) && memcmp(keyID, srch_keyID, KEYFRAGSIZE) == 0)
		found = 1;
	    if (found && ctb == CTB_USERID)
		break;
	}
	fclose(f);
    }
    return found ? userid : NULL;
}				/* _user_from_keyID */

/*
 * Lookup userid by keyID, use hash table if initialized.
 */
char *
 user_from_keyID(byte * keyID)
{
    struct hashent *p;

    if (!hashtbl)
	return _user_from_keyID(keyID);
    for (p = hashtbl[PK_HASH(keyID)]; p; p = p->next)
	if (memcmp(keyID, p->keyID, KEYFRAGSIZE) == 0)
	    return p->userid;
    return NULL;
}				/* user_from_keyID */

/*
 * add keyfile to userid hash table, userids are added, endkrent() clears
 * the hash table.
 */
int setkrent(char *keyring)
{
    int i;

    assert(nkr < MAXKR);
    if (keyring == NULL)
	keyring = globalPubringName;
    for (i = 0; i < nkr; ++i)
	if (strcmp(keyring, krnames[i]) == 0)
	    return 0;		/* duplicate name */
    krnames[nkr++] = store_str(keyring);
    return 0;
}				/* setkrent */

void endkrent(void)
{
    hashleft = strleft = 0;
    hashtbl = NULL;
    nkr = 0;
    freebufpool();
}				/* endkrent */

/*
 * create userid hash table, read all files set with setkrent()
 */
int init_userhash(void)
{
    FILE *f;
    int status, i;
    byte keyID[KEYFRAGSIZE];
    char userid[256];
    byte ctb;
    int keyflag;

    if (!hashtbl) {
	hashtbl = allocbuf(PK_HASHSIZE * sizeof(struct hashent *));
	memset(hashtbl, 0, PK_HASHSIZE * sizeof(struct hashent *));
    }
    for (i = 0; i < nkr; ++i) {
	if ((f = fopen(krnames[i], FOPRBIN)) == NULL)
	    continue;
	keyflag = 0;
	while ((status = readkpacket(f, &ctb, userid, keyID, NULL)) != -1) {
	    if (is_key_ctb(ctb) && user_from_keyID(keyID) == NULL)
		keyflag = 1;
	    if (keyflag && ctb == CTB_USERID) {
		if (!hashleft) {
		    hashptr = allocbuf(HASH_ALLOC * sizeof(struct hashent));
		    hashleft = HASH_ALLOC;
		}
		memcpy(hashptr->keyID, keyID, KEYFRAGSIZE);
		hashptr->userid = store_str(userid);
		hashptr->next = hashtbl[PK_HASH(keyID)];
		hashtbl[PK_HASH(keyID)] = hashptr;
		++hashptr;
		--hashleft;
		keyflag = 0;
	    }
	}
	fclose(f);
    }
    return 0;
}				/* init_userhash */

/*
 * memory management routines
 */

static void maint_init_mem(void)
{
    pkhash = allocbuf(PK_HASHSIZE * sizeof(struct pubkey *));
    memset(pkhash, 0, PK_HASHSIZE * sizeof(struct pubkey *));
}

static void maint_release_mem(void)
{
    nleft = 0;
    strleft = 0;
    pkhash = NULL;
    freebufpool();
}

/*
 * allocn() does the same as malloc().  Memory is allocated in chunks
 * of ALLOC_UNIT bytes, all memory can be freed by calling freebufpool().
 */
static VOID *
 allocn(int size)
{
    static char *ptr;
#ifndef MSDOS			/* don't align on MSDOS to save memory */
    size = (size + 3) & ~3;
#endif
    assert(size < ALLOC_UNIT);
    if (size > nleft) {
	ptr = allocbuf(ALLOC_UNIT);
	nleft = ALLOC_UNIT;
    }
    nleft -= size;
    ptr += size;
    return ptr - size;
}				/* allocn */

/*
 * store_str does the same as strdup(), but allocates memory with allocbuf()
 */
char *
 store_str(char *str)
{
    int size = strlen(str) + 1;
    if (size > ALLOC_UNIT) {
	fprintf(stderr, "store_str: string too long\n");
	return NULL;
    }
    if (size > strleft) {
	strptr = allocbuf(ALLOC_UNIT);
	strleft = ALLOC_UNIT;
    }
    strcpy(strptr, str);
    strptr += size;
    strleft -= size;
    return strptr - size;
}				/* store_str */


static struct bufpool {
    struct bufpool *next;
    char buf[1];		/* variable size */
} *bufpool = NULL;

long totalsize = 0;

/*
 * allocate buffer, all buffers allocated with this function can be
 * freed with one call to freebufpool()
 */
static VOID *
 allocbuf(int size)
{
    struct bufpool *p;

    p = xmalloc(size + sizeof(struct bufpool *));
    totalsize += size;
    p->next = bufpool;
    bufpool = p;
    return p->buf;
}				/* allocbuf */

/*
 * free all memory obtained with allocbuf()
 */
static void freebufpool(void)
{
    struct bufpool *p;

    if (verbose)
	fprintf(pgpout, "\nMemory used: %ldk\n", totalsize / 1024);
    totalsize = 0;
    while (bufpool) {
	p = bufpool;
	bufpool = bufpool->next;
	free(p);
    }
    nleft = strleft = hashleft = 0;
}				/* freebufpool */

#ifdef MACTC5
void ReInitKeyMaint(void);

void ReInitKeyMaint(void) {
	trustlst_len = 9;
	legitlst_len = 9;
	strcpy(floppyring,"");
	check_only = FALSE;
	floppy_fp = NULL;
	pkhash = NULL;
	pklist = NULL;
	hashtbl = NULL;
	totalsize = 0;
}
#endif


/* ==== keymgmt.c ==== */
/*      keymgmt.c  - Key management routines for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef UNIX
#include <sys/types.h>
#endif
#include <time.h>
#include <ctype.h>
#include "system.h"
#include "mpilib.h"
#include "random.h"
#include "crypto.h"
#include "fileio.h"
#include "keymgmt.h"
#include "rsagen.h"
#include "mpiio.h"
#include "language.h"
#include "pgp.h"
#include "md5.h"
#include "charset.h"
#include "keymaint.h"
#include "idea.h"
#ifdef MACTC5
#include "Aestuff.h"
#include "MacPGP.h"
#include "Macutil2.h"
#include "Macutil3.h"
#include "PGPDialogs.h"
#include "password.h"
#include "exitpgp.h"
#include "MyBufferedStdio.h"
#include "ReplaceStdio.h"
boolean userid_match(char *userid, char *substr,unitptr n);
void showKeyHash( unitptr n, unitptr e );
int backup_rename(char *scratchfile, char *destfile);
#endif

/*
   **   Convert to or from external byte order.
   **   Note that convert_byteorder does nothing if the external byteorder
   **  is the same as the internal byteorder.
 */
#define convert2(x,lx)	convert_byteorder( (byteptr)&(x), (lx) )
#define convert(x)		convert2( (x), sizeof(x) )


/*
 * check if userid matches the substring, magic characters ^ and $
 * can be used to match start and end of userid.
 * if n is NULL, only return TRUE if substr is an exact match of
 * userid, a substring does not match in this case.
 * the comparison is always case insensitive
 */
#ifdef MACTC5
boolean userid_match(char *userid, char *substr, unitptr n)
#else
static boolean userid_match(char *userid, char *substr, unitptr n)
#endif
{
    boolean match_end = FALSE;
    int id_len, sub_len, i;
    char buf[256], sub[256], *p;
#ifdef MACTC5
	int j;
	char argKeyID[10],curKeyID[10];
	unsigned long tempKeyID[2];
#endif

    if (substr == NULL || *substr == '\0')
	return TRUE;
    if (userid == NULL || *userid == '\0')
	return FALSE;

    /* Check whether we have an ASCII or hex userID to check for */
#ifdef EBCDIC
    /* EBCDIC assertion: to_lower works on EBCDIC (not internal) charset */
    if (n != NULL && EXT_C(substr[0]) == '0' && to_lower(EXT_C(substr[1])) == 'x') {
	userid = key2IDstring(n);
	CONVERT_TO_CANONICAL_CHARSET(userid);
	substr += 2;
    }
    id_len = strlen(userid);
    for (i = 0; i <= id_len; ++i)
	buf[i] = INT_C(to_lower(EXT_C(userid[i])));

    sub_len = strlen(substr);
    for (i = 0; i <= sub_len; ++i)
	sub[i] = INT_C(to_lower(EXT_C(substr[i])));
#else /* !EBCDIC */
    if (n != NULL && substr[0] == '0' && to_lower(substr[1]) == 'x') {
	userid = key2IDstring(n);
	substr += 2;
    }
    id_len = strlen(userid);
    for (i = 0; i <= id_len; ++i)
	buf[i] = to_lower(userid[i]);

    sub_len = strlen(substr);
    for (i = 0; i <= sub_len; ++i)
	sub[i] = to_lower(substr[i]);
#endif /* !EBCDIC */

    if (n == NULL) {
	return !strcmp(buf, sub);
    }
#ifdef MAGIC_MATCH
    if (sub_len > 1 && sub[sub_len - 1] == '$') {
	match_end = TRUE;
	sub[--sub_len] = '\0';
    }
    if (*sub == '^') {
	if (match_end)
	    return !strcmp(buf, sub + 1);
	else
	    return !strncmp(buf, sub + 1, sub_len - 1);
    }
#endif
    if (sub_len > id_len)
	return FALSE;

    if (match_end)
	return !strcmp(buf + id_len - sub_len, sub);

    p = buf;
    while ((p = strchr(p, *sub)) != NULL) {
	if (strncmp(p, sub, sub_len) == 0)
#ifdef MACTC5
		{	if (!argc)
				return true;
			for(j=1; j<100; j++) {
				if (argv[j]==nil) {
					j=100;
					break;
					}
				if (!strcmp(substr,argv[j])) break;
			}
			if (j==100) return TRUE;
			if (arg_keyid[j]==0) return TRUE;
			tempKeyID[0]=0;
			tempKeyID[1]=arg_keyid[j];
			strcpy(argKeyID,keyIDstring((byte *)tempKeyID));
			strcpy(curKeyID,key2IDstring( n ));
			if (strcmp(argKeyID,curKeyID))
				return FALSE;
			else {
				arg_keyid[j]=0;
				return TRUE;
			}
	}
#else
	    return TRUE;
#endif
	++p;
    }
    return FALSE;
}

int is_key_ctb(byte ctb)
{
    return ctb == CTB_CERT_PUBKEY || ctb == CTB_CERT_SECKEY;
}


/*
   **   keyIDstring
   **
   **   Return printable key fragment, which is an abbreviation of the public
   **   key.  Show LEAST significant 32 bits (KEYFRAGSIZE bytes) of modulus,
   **   LSB last.  Yes, that's LSB LAST.
 */

char const blankkeyID[] = "        ";

char *keyIDstring(byte * keyID)
{
    short i;
    char *bufptr;		/* ptr to Key ID string */
    static char keyIDbuf[9];

    /* only show bottom 4 bytes of keyID */

    bufptr = keyIDbuf;

#ifdef XLOWFIRST
    /* LSB-first keyID format */

    for (i = 3; i >= 0; i--) {
	sprintf(bufptr, "%02X", keyID[i]);
	bufptr += 2;
    }
#else
    /* MSB-first keyID format */

    for (i = KEYFRAGSIZE - 4; i < KEYFRAGSIZE; i++) {
	sprintf(bufptr, "%02X", keyID[i]);
	bufptr += 2;
    }
#endif
    *bufptr = '\0';
    return keyIDbuf;
}				/* keyIDstring */



void extract_keyID(byteptr keyID, unitptr n)
/*
 * Extract key fragment from modulus n.  keyID byte array must be
 * at least KEYFRAGSIZE bytes long.
 */
{
    byte buf[MAX_BYTE_PRECISION + 2];
    short i, j;

    fill0(buf, KEYFRAGSIZE + 2);	/* in case n is too short */
    reg2mpi(buf, n);		/* MUST be at least KEYFRAGSIZE long */
#ifdef XLOWFIRST
    i = reg2mpi(buf, n);	/* MUST be at least KEYFRAGSIZE long */
    /* For LSB-first keyID format, start of keyID is: */
    i = 2;			/* skip over the 2 bytes of bitcount */
    for (j = 0; j < KEYFRAGSIZE;)
	keyID[j++] = buf[i++];
#else
    i = reg2mpi(buf, n);	/* MUST be at least KEYFRAGSIZE long */
    /* For MSB-first keyID format, start of keyID is: */
    i = i + 2 - KEYFRAGSIZE;
    for (j = 0; j < KEYFRAGSIZE;)
	keyID[j++] = buf[i++];
#endif

}				/* extract_keyID */



char *key2IDstring(unitptr n)
/*      Derive the key abbreviation fragment from the modulus n,
   and return printable string of key ID.
   n is key modulus from which to extract keyID.
 */
{
    byte keyID[KEYFRAGSIZE];
    extract_keyID(keyID, n);
    return keyIDstring(keyID);
}				/* key2IDstring */



static void showkeyID(byteptr keyID, FILE *pgpout)
/*      Print key fragment, which is an abbreviation of the public key. */
{
    fprintf(pgpout, "%s", keyIDstring(keyID));
}				/* showkeyID */



void writekeyID(unitptr n, FILE * f)
/*      Write message prefix keyID to a file.
   n is key modulus from which to extract keyID.
 */
{
    byte keyID[KEYFRAGSIZE];
    extract_keyID(keyID, n);
    fwrite(keyID, 1, KEYFRAGSIZE, f);
}				/* writekeyID */



static boolean checkkeyID(byte * keyID, unitptr n)
/*      Compare specified keyID with one derived from actual key modulus n. */
{
    byte keyID0[KEYFRAGSIZE];
    if (keyID == NULL)		/* no key ID -- assume a good match */
	return TRUE;
    extract_keyID(keyID0, n);
    return equal_buffers(keyID, keyID0, KEYFRAGSIZE);
}				/* checkkeyID */



/* external function prototype, from mpiio.c */
void dump_unit_array(string s, unitptr r);

void write_trust(FILE * f, byte trustbyte)
/*      Write a key control packet to f, with the specified trustbyte data.
 */
{
    putc(CTB_KEYCTRL, f);	/* Key control header byte */
    putc(1, f);			/* Key control length */
    putc(trustbyte, f);		/* Key control byte */
}

static
short writekeyfile(char *fname, struct IdeaCfbContext *cfb, word32 timestamp,
    byte * userid, unitptr n, unitptr e, unitptr d, unitptr p, unitptr q,
		   unitptr u)
/*      Write key components p, q, n, e, d, and u to specified file.
   hidekey is TRUE iff key should be encrypted.
   userid is a length-prefixed Pascal-type character string. 
   We write three packets: a key packet, a key control packet, and
   a userid packet.  We assume the key being written is our own,
   so we set the control bits for full trust.
 */
{
    FILE *f;
    byte ctb;
    byte alg, version;
    word16 validity;
    word16 cert_length;
    extern word16 mpi_checksum;
    byte iv[8];
    int i;

    /* open file f for write, in binary (not text) mode... */
    if ((f = fopen(fname, FOPWBIN)) == NULL) {
	fprintf(pgpout,
		LANG("\n\007Unable to create key file '%s'.\n"), fname);
	return -1;
    }
/*** Begin key certificate header fields ***/
    if (d == NULL) {
	/* public key certificate */
	ctb = CTB_CERT_PUBKEY;
	cert_length = 1 + SIZEOF_TIMESTAMP + 2 + 1 + (countbytes(n) + 2)
	    + (countbytes(e) + 2);
    } else {
	/* secret key certificate */
	ctb = CTB_CERT_SECKEY;
	cert_length = 1 + SIZEOF_TIMESTAMP + 2 + 1
	    + (countbytes(n) + 2)
	    + (countbytes(e) + 2)
	    + 1 + (cfb ? 8 : 0)	/* IDEA algorithm byte and IV */
	    +(countbytes(d) + 2)
	    + (countbytes(p) + 2) + (countbytes(q) + 2)
	    + (countbytes(u) + 2) + 2;

    }

    fwrite(&ctb, 1, 1, f);	/* write key certificate header byte */
    convert(cert_length);	/* convert to external byteorder */
    fwrite(&cert_length, 1, sizeof(cert_length), f);
    version = version_byte;
    fwrite(&version, 1, 1, f);	/* set version number */
    memcpy(iv, &timestamp, 4);
    convert_byteorder(iv, 4);	/* convert to external form */
    fwrite(iv, 1, 4, f);	/* write certificate timestamp */
    validity = 0;
    fwrite(&validity, 1, sizeof(validity), f);	/* validity period */
    alg = RSA_ALGORITHM_BYTE;
    fwrite(&alg, 1, 1, f);
    write_mpi(n, f, FALSE);
    write_mpi(e, f, FALSE);

    if (is_secret_key(ctb)) {	/* secret key */
	/* Write byte for following algorithm */
	alg = cfb ? IDEA_ALGORITHM_BYTE : 0;
	putc(alg, f);

	if (cfb) {		/* store encrypted IV */
	    for (i = 0; i < 8; i++)
		iv[i] = trueRandByte();
	    ideaCfbEncrypt(cfb, iv, iv, 8);
	    fwrite(iv, 1, 8, f);	/* write out the IV */
	}
	mpi_checksum = 0;
	write_mpi(d, f, cfb);
	write_mpi(p, f, cfb);
	write_mpi(q, f, cfb);
	write_mpi(u, f, cfb);
	/* Write checksum here - based on plaintext values */
	convert(mpi_checksum);
	fwrite(&mpi_checksum, 1, sizeof(mpi_checksum), f);
    } else {
	/* Keyring control packet, public keys only */
	write_trust(f, KC_OWNERTRUST_ULTIMATE | KC_BUCKSTOP);
    }
    /* User ID packet */
    ctb = CTB_USERID;
    fwrite(&ctb, 1, 1, f);	/* write userid header byte */
    fwrite(userid, 1, userid[0] + 1, f);	/* write user ID */
    if (d == NULL)		/* only on public keyring */
	write_trust(f, KC_LEGIT_COMPLETE);
    if (write_error(f)) {
	fclose(f);
	return -1;
    }
    fclose(f);
    if (verbose)
	fprintf(pgpout, "%d-bit %s key written to file '%s'.\n",
		countbits(n),
		is_secret_key(ctb) ? "secret" : "public",
		fname);
    return 0;
}				/* writekeyfile */

#ifdef EBCDIC
/* in RECFM=FB datasets fread() != 0 when eof() cause of padding 0x00 */
#define CTB_EOF 0x00
#else
#define CTB_EOF 0x1A
#endif

/* Return -1 on EOF, else read next key packet, return its ctb, and
 * advance pointer to beyond the packet.
 * This is short of a "short form" of readkeypacket
 */
short nextkeypacket(FILE * f, byte * pctb)
{
    word32 cert_length;
    int count;
    byte ctb;

    *pctb = 0;			/* assume no ctb for caller at first */
    count = fread(&ctb, 1, 1, f);	/* read key certificate CTB byte */
    if (count == 0)
	return -1;		/* premature eof */
    *pctb = ctb;		/* returns type to caller */
    if ((ctb != CTB_CERT_PUBKEY) && (ctb != CTB_CERT_SECKEY) &&
	(ctb != CTB_USERID) && (ctb != CTB_KEYCTRL) &&
	!is_ctb_type(ctb, CTB_SKE_TYPE) &&
	!is_ctb_type(ctb, CTB_COMMENT_TYPE))
	/* Either bad key packet or X/Ymodem padding detected */
	return (ctb == CTB_EOF) ? -1 : -2;

    cert_length = getpastlength(ctb, f);	/* read certificate length */

    if (cert_length > MAX_KEYCERT_LENGTH - 3)
	return -3;		/* bad length */

    fseek(f, cert_length, SEEK_CUR);
    return 0;
}				/* nextkeypacket */

/*
 * Reads a key certificate from the current file position of file f.
 * Depending on the certificate type, it will set the proper fields
 * of the return arguments.  Other fields will not be set.
 * pctb is always set.
 * If the packet is CTB_CERT_PUBKEY or CTB_CERT_SECKEY, it will
 * return timestamp, n, e, and if the secret key components are
 * present and d is not NULL, it will read, decrypt if hidekey is
 * true, and return d, p, q, and u.
 * If the packet is CTB_KEYCTRL, it will return keyctrl as that byte.
 * If the packet is CTB_USERID, it will return userid.
 * If the packet is CTB_COMMENT_TYPE, it won't return anything extra.
 * The file pointer is left positioned after the certificate.
 *
 * If the key could not be read because of a version error or bad
 * data, the return value is -6 or -4, the file pointer will be
 * positioned after the certificate, only the arguments pctb and
 * userid will valid in this case, other arguments are undefined.
 * Return value -3 means the error is unrecoverable.
 */
short readkeypacket(FILE * f, struct IdeaCfbContext *cfb, byte * pctb,
		    byte * timestamp, char *userid,
	unitptr n, unitptr e, unitptr d, unitptr p, unitptr q, unitptr u,
		    byte * sigkeyID, byte * keyctrl)
{
    byte ctb;
    word16 cert_length;
    int count;
    byte version, alg, mdlen;
    word16 validity;
    word16 chksum;
    extern word16 mpi_checksum;
    long next_packet;
    byte iv[8];

/*** Begin certificate header fields ***/
    *pctb = 0;			/* assume no ctb for caller at first */
    count = fread(&ctb, 1, 1, f);	/* read key certificate CTB byte */
    if (count == 0)
	return -1;		/* premature eof */
    *pctb = ctb;		/* returns type to caller */
    if ((ctb != CTB_CERT_PUBKEY) && (ctb != CTB_CERT_SECKEY) &&
	(ctb != CTB_USERID) && (ctb != CTB_KEYCTRL) &&
	!is_ctb_type(ctb, CTB_SKE_TYPE) &&
	!is_ctb_type(ctb, CTB_COMMENT_TYPE))
	/* Either bad key packet or X/Ymodem padding detected */
	return (ctb == CTB_EOF) ? -1 : -2;

    cert_length = getpastlength(ctb, f);	/* read certificate length */

    if (cert_length > MAX_KEYCERT_LENGTH - 3)
	return -3;		/* bad length */

    next_packet = ftell(f) + cert_length;

    /*
     * skip packet and return, keeps us in sync when we hit a
     * version error or bad data.  Implemented oddly to make it
     * only one statement.
     */
#define SKIP_RETURN(x) return fseek(f, next_packet, SEEK_SET), x

    if (ctb == CTB_USERID) {
	if (cert_length > 255)
	    return -3;		/* Bad length error */
	if (userid) {
	    userid[0] = cert_length;	/* Save user ID length */
	    fread(userid + 1, 1, cert_length, f);    /* read rest of user ID */
	} else
	    fseek(f, (long) cert_length, SEEK_CUR);
	return 0;		/* normal return */

    } else if (is_ctb_type(ctb, CTB_SKE_TYPE)) {

	if (sigkeyID) {
	    fread(&version, 1, 1, f);	/* Read version of sig packet */
	    if (version_byte_error(version))
		SKIP_RETURN(-6);	/* Need a later version */
	    /* Skip timestamp, validity period, and type byte */
	    fread(&mdlen, 1, 1, f);
	    fseek(f, (long) mdlen, SEEK_CUR);
	    /* Read and return KEY ID */
	    fread(sigkeyID, 1, KEYFRAGSIZE, f);
	}
	SKIP_RETURN(0);		/* normal return */

    } else if (ctb == CTB_KEYCTRL) {

	if (cert_length != 1)
	    return -3;		/* Bad length error */
	if (keyctrl)
	    fread(keyctrl, 1, cert_length, f);	/* Read key control byte */
	else
	    fseek(f, (long) cert_length, SEEK_CUR);
	return 0;		/* normal return */

    } else if (!is_key_ctb(ctb))	/* comment or other packet */
	SKIP_RETURN(0);		/* normal return */

    /* Here we have a key packet */
    if (n != NULL)
	set_precision(MAX_UNIT_PRECISION);	/* safest opening assumption */
    fread(&version, 1, 1, f);	/* read and check version */
    if (version_byte_error(version))
	SKIP_RETURN(-6);	/* Need a later version */
    if (timestamp) {
	fread(timestamp, 1, SIZEOF_TIMESTAMP, f); /* read certificate
						     timestamp */
	convert_byteorder(timestamp, SIZEOF_TIMESTAMP);	/* convert from
							   external form */
    } else {
	fseek(f, (long) SIZEOF_TIMESTAMP, SEEK_CUR);
    }
    fread(&validity, 1, sizeof(validity), f);	/* Read validity period */
    convert(validity);		/* convert from external byteorder */
    /* We don't use validity period yet */
    fread(&alg, 1, 1, f);
    if (version_error(alg, RSA_ALGORITHM_BYTE))
	SKIP_RETURN(-6);	/* Need a later version */
/*** End certificate header fields ***/

    /* We're past certificate headers, now look at some key material... */

    cert_length -= 1 + SIZEOF_TIMESTAMP + 2 + 1;

    if (n == NULL)		/* Skip key certificate data */
	SKIP_RETURN(0);

    if (read_mpi(n, f, TRUE, FALSE) < 0)
	SKIP_RETURN(-4);	/* data corrupted, return error */

    /* Note that precision was adjusted for n */

    if (read_mpi(e, f, FALSE, FALSE) < 0)
	SKIP_RETURN(-4);	/* data corrupted, error return */

    cert_length -= (countbytes(n) + 2) + (countbytes(e) + 2);

    if (d == NULL) {		/* skip rest of this key certificate */
        if (cert_length && !is_secret_key(ctb))
	    SKIP_RETURN(-4);	/* key w/o userID */
        else
	    SKIP_RETURN(0);	/* Normal return */
    }

    if (is_secret_key(ctb)) {
	fread(&alg, 1, 1, f);
	if (alg && version_error(alg, IDEA_ALGORITHM_BYTE))
	    SKIP_RETURN(-6);	/* Unknown version */

	if (!cfb && alg)
	    /* Don't bother trying if hidekey is false and alg is true */
	    SKIP_RETURN(-5);

	if (alg) {		/* if secret components are encrypted... */
	    /* process encrypted CFB IV before reading secret components */
	    count = fread(iv, 1, 8, f);
	    if (count < 8)
		return -4;	/* data corrupted, error return */

	    ideaCfbDecrypt(cfb, iv, iv, 8);
	    cert_length -= 8;	/* take IV length into account */
	}
	/* Reset checksum before these reads */
	mpi_checksum = 0;

	if (read_mpi(d, f, FALSE, cfb) < 0)
	    return -4;		/* data corrupted, error return */
	if (read_mpi(p, f, FALSE, cfb) < 0)
	    return -4;		/* data corrupted, error return */
	if (read_mpi(q, f, FALSE, cfb) < 0)
	    return -4;		/* data corrupted, error return */

	/* use register 'u' briefly as scratchpad */
	mp_mult(u, p, q);	/* compare p*q against n */
	if (mp_compare(n, u) != 0)	/* bad pass phrase? */
	    return -5;		/* possible bad pass phrase, error return */
	/* now read in real u */
	if (read_mpi(u, f, FALSE, cfb) < 0)
	    return -4;		/* data corrupted, error return */

	/* Read checksum, compare with mpi_checksum */
	fread(&chksum, 1, sizeof(chksum), f);
	convert(chksum);
	if (chksum != mpi_checksum)
	    return -5;		/* possible bad pass phrase */

	cert_length -= 1 + (countbytes(d) + 2) + (countbytes(p) + 2)
	    + (countbytes(q) + 2) + (countbytes(u) + 2) + 2;

    } else {			/* not a secret key */

	mp_init(d, 0);
	mp_init(p, 0);
	mp_init(q, 0);
	mp_init(u, 0);
    }

    if (cert_length != 0) {
	fprintf(pgpout, "\n\007Corrupted key.  Bad length, off by %d bytes.\n",
		(int) cert_length);
	SKIP_RETURN(-4);	/* data corrupted, error return */
    }
    return 0;			/* normal return */

}				/* readkeypacket */

/*
 * keyID contains key fragment we expect to find in keyfile.
 * If keyID is NULL, then userid contains a C string search target of
 * userid to find in keyfile.
 * keyfile is the file to begin search in, and it may be modified
 * to indicate true filename of where the key was found.  It can be
 * either a public key file or a secret key file.
 * file_position is returned as the byte offset within the keyfile
 * that the key was found at.  pktlen is the length of the key packet.
 * These values are for the key packet itself, not including any
 * following userid, control, signature, or comment packets.
 *
 * possible flags:
 * GPK_GIVEUP: we are just going to do a single file search only.
 * GPK_SHOW: show the key if found.
 * GPK_NORVK: skip revoked keys.
 * GPK_DISABLED: don't ignore disabled keys (when doing userid lookup)
 * GPK_SECRET: looking for a secret key
 *
 * Returns -6 if the key was found but the key was not read because of a
 * version error or bad data.  The arguments timestamp, n and e are
 * undefined in this case.
 */
int getpublickey(int flags, char *keyfile, long *_file_position,
	     int *_pktlen, byte * keyID, byte * timestamp, byte * userid,
		 unitptr n, unitptr e)
{
    byte ctb;			/* returned by readkeypacket */
    FILE *f;
    int status, keystatus = -1;
    boolean keyfound = FALSE;
    char matchid[256];		/* C string format */
    long fpos;
    long file_position = 0;
    int pktlen = 0;
    boolean skip = FALSE;	/* if TRUE: skip until next key packet */
    byte keyctrl;
#ifdef MACTC5
	boolean use_pubring2;
	use_pubring2 = (globalPubringName2[0] != '\0');
#endif

    if (keyID == NULL)		/* then userid has search target */
	strcpy(matchid, (char *) userid);
    else
	matchid[0] = '\0';

  top:
    if (strlen(keyfile) == 0)	/* null filename */
	return -1;		/* give up, error return */

    if (!file_exists(keyfile))
        default_extension(keyfile, PGP_EXTENSION);

    if (!file_exists(keyfile)) {
	if (flags & GPK_GIVEUP)
	    return -1;		/* give up, error return */
	fprintf(pgpout, LANG("\n\007Keyring file '%s' does not exist. "),
		keyfile);
        fprintf(pgpout, "\n");
	goto nogood;
    }
    if (verbose) {
	fprintf(pgpout, "searching key ring file '%s' ", keyfile);
	if (keyID)
	    fprintf(pgpout, "for keyID %s\n", keyIDstring(keyID));
	else
	    fprintf(pgpout, "for userid \"%s\"\n", LOCAL_CHARSET(userid));
    }
    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;		/* error return */

    keyfound = FALSE;
    for (;;) {
	fpos = ftell(f);
	status = readkeypacket(f, FALSE, &ctb, timestamp, (char *) userid,
			       n, e, NULL, NULL, NULL, NULL, NULL, NULL);
	/* Note that readkeypacket has called set_precision */

	if (status == -1)	/* end of file */
	    break;

	if (status < -1 && status != -4 && status != -6) {
	    fprintf(pgpout, LANG("\n\007Could not read key from file '%s'.\n"),
		    keyfile);
	    fclose(f);		/* close key file */
	    return status;
	}
	/* Remember packet position and size for last key packet */
	if (is_key_ctb(ctb)) {
	    file_position = fpos;
	    pktlen = (int) (ftell(f) - fpos);
	    keystatus = status;
	    if (!keyID && !(flags & GPK_DISABLED) &&
		(is_ctb_type(ctb, CTB_CERT_SECKEY_TYPE) ||
		 is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE)) &&
		read_trust(f, &keyctrl) == 0 &&
		(keyctrl & KC_DISABLED))
		skip = TRUE;
	    else
		skip = FALSE;
	}
	/* Only check for matches when we find a USERID packet */
	if (!skip && ctb == CTB_USERID) {
#ifdef MACTC5
		mac_poll_for_break();
#endif
  /* keyID contains key fragment.  Check it against n from keyfile. */
	    if (keyID != NULL) {
		if (keystatus == 0)
		    keyfound = checkkeyID(keyID, n);
	    } else {
		/* matchid is already a C string */
		PascalToC((char *) userid);	/* for C string functions */
		/* Accept any matching subset */
		keyfound = userid_match((char *) userid, matchid, n);
		CToPascal((char *) userid);
	    }
	}
	if (keyfound) {
	    if (flags & GPK_SHOW)
		show_key(f, file_position, 0);
	    fseek(f, file_position, SEEK_SET);
	    if ((flags & GPK_NORVK) && keystatus == 0 && is_compromised(f)) {
		if (flags & GPK_SHOW) {		/* already printed user ID */
		    fprintf(pgpout,
	       LANG("\n\007Sorry, this key has been revoked by its owner.\n"));
		} else {
		    PascalToC((char *) userid);
		    fprintf(pgpout, LANG("\nKey for user ID \"%s\"\n\
has been revoked.  You cannot use this key.\n"),
			    LOCAL_CHARSET((char *) userid));
		}
		keyfound = FALSE;
		skip = TRUE;
		/* we're positioned at the key packet, skip it */
		nextkeypacket(f, &ctb);
	    } else {
		/* found key, normal return */
		if (_pktlen)
		    *_pktlen = pktlen;
		if (_file_position)
		    *_file_position = file_position;
		fclose(f);
		return keystatus;
	    }
	}
    }				/* while TRUE */

    fclose(f);			/* close key file */

    if (flags & GPK_GIVEUP)
	return -1;		/* give up, error return */

    if (keyID != NULL) {
	fprintf(pgpout,
       LANG("\n\007Key matching expected Key ID %s not found in file '%s'.\n"),
		keyIDstring(keyID), keyfile);
    } else {
	fprintf(pgpout,
	    LANG("\n\007Key matching userid '%s' not found in file '%s'.\n"),
		LOCAL_CHARSET(matchid), keyfile);
    }

  nogood:
    if (filter_mode || batchmode)
	return -1;		/* give up, error return */

#ifdef MACTC5
	{
	Boolean result;
	if (flags & GPK_SECRET)
		result=GetFilePath(LANG("Enter secret key filename: "), keyfile, GETFILE);
	else if (use_pubring2) {
		strcpy(keyfile,globalPubringName2);
		use_pubring2 = false;
		result = true;
	} else
		result=GetFilePath(LANG("Enter public key filename: "), keyfile, GETFILE);
	if (!result) strcpy(keyfile,"");
	if (flags & GPK_SECRET)
		fprintf(pgpout,LANG("Enter secret key filename: "));
	else
		fprintf(pgpout,LANG("Enter public key filename: "));
	fprintf(pgpout, "%s\n",keyfile);
	}
#else  
    if (flags & GPK_SECRET)
	fprintf(pgpout, LANG("Enter secret key filename: "));
    else
	fprintf(pgpout, LANG("Enter public key filename: "));

    getstring(keyfile, 59, TRUE);	/* echo keyboard input */
#endif
    goto top;

}				/* getpublickey */

/*  Start at key_position in keyfile, and scan for the key packet
   that contains userid.  Return userid_position and userid_len.
   Return 0 if OK, -1 on error.  Userid should be a C string.
   If exact_match is TRUE, the userid must match for full length,
   a substring is not enough.
 */
int getpubuserid(char *keyfile, long key_position, byte * userid,
	     long *userid_position, int *userid_len, boolean exact_match)
{
    unit n[MAX_UNIT_PRECISION];
    unit e[MAX_UNIT_PRECISION];
    byte ctb;			/* returned by readkeypacket */
    FILE *f;
    int status;
    char userid0[256];		/* C string format */
    long fpos;

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;		/* error return */

    /* Start off at correct location */
    fseek(f, key_position, SEEK_SET);
    (void) nextkeypacket(f, &ctb);	/* Skip key */
    for (;;) {
	fpos = ftell(f);
	status = readkeypacket(f, FALSE, &ctb, NULL, (char *) userid0, n, e,
			       NULL, NULL, NULL, NULL, NULL, NULL);

	if (status < 0 || is_key_ctb(ctb)) {
	    fclose(f);		/* close key file */
	    return status ? status : -1;	/* give up, error return */
	}
	/* Only check for matches when we find a USERID packet */
	if (ctb == CTB_USERID) {
	    if (userid[0] == '0' && userid[1] == 'x')
		break;	       /* use first userid if user specified a keyID */
	    /* userid is already a C string */
	    PascalToC((char *) userid0);	/* for C string functions */
	    /* Accept any matching subset if exact_match is FALSE */
	    if (userid_match((char *) userid0, (char *) userid,
			     (exact_match ? NULL : n)))
		break;
	}
    }				/* for(;;) */
    *userid_position = fpos;
    *userid_len = (int) (ftell(f) - fpos);
    fclose(f);
    return 0;			/* normal return */
}				/* getpubuserid */

/*
 * Start at user_position in keyfile, and scan for the signature packet
 * that matches sigkeyID.  Return the signature timestamp, sig_position
 * and sig_len.
 *
 * Return 0 if OK, -1 on error.
 */
int getpubusersig(char *keyfile, long user_position, byte * sigkeyID,
		  byte * timestamp, long *sig_position, int *sig_len)
{
    byte ctb;			/* returned by readkeypacket */
    FILE *f;
    int status;
    byte keyID0[KEYFRAGSIZE];
    long fpos;

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;		/* error return */

    /* Start off at correct location */
    fseek(f, user_position, SEEK_SET);
    (void) nextkeypacket(f, &ctb);	/* Skip userid packet */
    for (;;) {
	fpos = ftell(f);
	status = readkeypacket(f, FALSE, &ctb, NULL, NULL, NULL, NULL,
			       NULL, NULL, NULL, NULL, keyID0, NULL);

	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;

	/* Only check for matches when we find a signature packet */
	if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
	    if (equal_buffers(sigkeyID, keyID0, KEYFRAGSIZE)) {
		*sig_position = fpos;
		*sig_len = (int) (ftell(f) - fpos);
		fseek(f, fpos + 6, SEEK_SET);
		fread(timestamp, 1, SIZEOF_TIMESTAMP, f); /* read certificate
							     timestamp */
		convert_byteorder(timestamp, SIZEOF_TIMESTAMP);	/* convert
							       from external 
							       orm */
		fclose(f);
		return 0;	/* normal return */
	    }
	}
    }				/* for (;;) */

    fclose(f);			/* close key file */
    return status ? status : -1;	/* give up, error return */
}				/* getpubusersig */

#ifdef MACTC5
/* Truncated version of getsecretkey used to get default userid during
   initialization.  Does not annoy user by asking for password. */
int getfirstsecretkey(boolean giveup, boolean showkey, char *keyfile, byte *keyID,
	byte *timestamp, char *passp, boolean *hkey,
	byte *userid, unitptr n, unitptr e, unitptr d, unitptr p, unitptr q,
	unitptr u)
{
	char keyfilename[MAX_PATH];	/* for getpublickey */
	long file_position;
	int pktlen;	/* unused, just to satisfy getpublickey */

	if (keyfile == NULL)
	{	/* use default pathname */
		buildfilename(keyfilename,globalSecringName);
		keyfile = keyfilename;
	}

	return(getpublickey(GPK_GIVEUP, keyfile, &file_position, &pktlen,
			keyID, timestamp, userid, n, e));
}
#endif

/*
 * keyID contains key fragment we expect to find in keyfile.
 * If keyID is NULL, then userid contains search target of
 * userid to find in keyfile.
 * giveup controls whether we ask the user for the name of the
 * secret key file on failure.  showkey controls whether we print
 * out the key information when we find it.  keyfile, if non-NULL,
 * is the name of the secret key file; if NULL, we use the
 * default.  hpass and hkey, if non-NULL, get returned with a copy
 * of the hashed password buffer and hidekey variable.
 */
int getsecretkey(int flags, char *keyfile, byte * keyID,
	   byte * timestamp, byte * hpass, boolean * hkey, byte * userid,
		 unitptr n, unitptr e, unitptr d, unitptr p, unitptr q,
		 unitptr u)
{
    byte ctb;			/* returned by readkeypacket */
    FILE *f;
    char keyfilename[MAX_PATH];	/* for getpublickey */
    long file_position;
    int status;
    boolean hidekey;		/* TRUE iff secret key is encrypted */
    word16 iv[4];		/* initialization vector for encryption */
    byte ideakey[16];
    int guesses;
    struct hashedpw *hpw, **hpwp;
    struct IdeaCfbContext cfb;

    if (keyfile == NULL) {
	/* use default pathname */
	strcpy(keyfilename, globalSecringName);
	keyfile = keyfilename;
    }
    status = getpublickey(flags | GPK_SECRET, keyfile, &file_position,
			  NULL, keyID, timestamp, userid, n, e);
    if (status < 0)
	return status;		/* error return */

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;		/* error return */

    /* First guess is no password */
    hidekey = FALSE;
    fseek(f, file_position, SEEK_SET);	/* reposition file to key */
    status = readkeypacket(f, 0, &ctb, timestamp, (char *) userid,
			   n, e, d, p, q, u, NULL, NULL);
    if (status != -5)		/* Anything except bad password */
	goto done;

    /* If we're not signing a key (when we force asking the user),
     * check the prevosuly known passwords.
     */
    if (!(flags & GPK_ASKPASS)) {
	hidekey = TRUE;
	/* Then come existing key passwords */
	hpw = keypasswds;
	while (hpw) {
	    ideaCfbInit(&cfb, hpw->hash);
	    fseek(f, file_position, SEEK_SET);
	    status = readkeypacket(f, &cfb, &ctb, timestamp,
			  (char *) userid, n, e, d, p, q, u, NULL, NULL);
	    ideaCfbDestroy(&cfb);
	    if (status != -5) {
		memcpy(ideakey, hpw->hash, sizeof(ideakey));
		goto done;
	    }
	    hpw = hpw->next;
	}
	/* Then try "other" passwords" */
	hpwp = &passwds;
	hpw = *hpwp;
	while (hpw) {
	    ideaCfbInit(&cfb, hpw->hash);
	    fseek(f, file_position, SEEK_SET);
	    status = readkeypacket(f, &cfb, &ctb, timestamp,
			  (char *) userid, n, e, d, p, q, u, NULL, NULL);
	    ideaCfbDestroy(&cfb);
	    if (status >= 0) {
		/* Success - move to key password list */
		memcpy(ideakey, hpw->hash, sizeof(ideakey));
		*hpwp = hpw->next;
		hpw->next = keypasswds;
		keypasswds = hpw;
	    }
	    if (status != -5)
		goto done;
	    hpwp = &hpw->next;
	    hpw = *hpwp;
	}
    }
    /* If batchmode, we don't ask the user. */
    if (batchmode) {
	/* PGPPASS (or -z) wrong or not set */
	fprintf(pgpout, LANG("\n\007Error:  Bad pass phrase.\n"));
	fclose(f);		/* close key file */
	return -1;
    }
    /* Finally, prompt the user. */
    fprintf(pgpout,
	    LANG("\nYou need a pass phrase to unlock your RSA secret key. "));
    if (!(flags & GPK_SHOW)) {
	/* let user know for which key he should type his password */
	PascalToC((char *) userid);
	fprintf(pgpout, LANG("\nKey for user ID: %s\n"),
		LOCAL_CHARSET((char *) userid));
	fprintf(pgpout, LANG("%d-bit key, key ID %s, created %s\n"),
		countbits(n), key2IDstring(n), cdate((word32 *) timestamp));
	CToPascal((char *) userid);
    }
    guesses = 0;
    for (;;) {
	if (++guesses > 3)
	    hidekey = 0;
	else
	    hidekey = (GetHashedPassPhrase((char *)ideakey, 1) > 0);
	/*
	 * We've already tried the null password - interpret
	 * a null string as "I dunno".
	 */
	if (!hidekey) {
	    status = -5;	/* Bad passphrase */
	    fputs(LANG("No passphrase; secret key unavailable.\n"),
		  pgpout);
	    break;
	}
	ideaCfbInit(&cfb, ideakey);
	fseek(f, file_position, SEEK_SET);
	status = readkeypacket(f, &cfb, &ctb, timestamp,
			  (char *) userid, n, e, d, p, q, u, NULL, NULL);
	ideaCfbDestroy(&cfb);
	if (status >= 0) {
#ifdef MACTC5
		;
	}
		if (Abort) guesses=1;
#else
	    /* Success - remember this key for later use */
	    if (flags & GPK_ASKPASS) {
		/*
		 * This may be a duplicate because we didn't
		 * search the lists before - check.
		 */
		hpw = passwds;
		while (hpw) {
		    if (memcmp(hpw->hash, ideakey,
			       sizeof(ideakey)) == 0)
			goto done;
		    hpw = hpw->next;
		}
		hpw = keypasswds;
		while (hpw) {
		    if (memcmp(hpw->hash, ideakey,
			       sizeof(ideakey)) == 0)
			goto done;
		    hpw = hpw->next;
		}
	    }
	    /* Insert new key into remember lists. */
	    hpw = (struct hashedpw *) malloc(sizeof(struct hashedpw));
	    if (hpw) {
		/* If malloc fails, just don't remember the phrase */
		memcpy(hpw->hash, ideakey, sizeof(hpw->hash));
		hpw->next = keypasswds;
		keypasswds = hpw;
	    }
	}
#endif /* MACTC5 */
	if (status != -5)
	    goto done;
#ifdef MACTC5
	passhash[0] = '\0';
#endif
	fprintf(pgpout, LANG("\n\007Error:  Bad pass phrase.\n"));
    }
    while (--guesses);
    /* Failed - fall through to done */

  done:
    fclose(f);
    if (hkey)
	*hkey = hidekey;
    if (status == -5)
	return status;
    if (status < 0) {
	fprintf(pgpout, LANG("\n\007Could not read key from file '%s'.\n"),
		keyfile);
	fclose(f);		/* close key file */
	return -1;
    }
    if (hpass)
	memcpy(hpass, ideakey, sizeof(ideakey));
    burn(ideakey);

    /* Note that readkeypacket has called set_precision */

    if (d != NULL) {	/* No effective check of pass phrase if d is NULL */
	if (!quietmode) {
	    if (!hidekey)
		fprintf(pgpout,
LANG("\nAdvisory warning: This RSA secret key is not protected by a \
passphrase.\n"));
	    else
		fprintf(pgpout, LANG("Pass phrase is good.  "));
	}
	if (testeq(d, 0)) {	/* didn't get secret key components */
	    fprintf(pgpout,
		    LANG("\n\007Key file '%s' is not a secret key file.\n"),
		    keyfile);
	    return -1;
	}
    }
    return 0;			/* normal return */

}				/* getsecretkey */

/*
 * check if a key has a compromise certificate, file pointer must
 * be positioned at or right after the key packet.
 */
int is_compromised(FILE * f)
{
    long pos, savepos;
    byte class, ctb;
    int cert_len;
    int status = 0;

    pos = savepos = ftell(f);

    nextkeypacket(f, &ctb);
    if (is_key_ctb(ctb)) {
	pos = ftell(f);
	nextkeypacket(f, &ctb);
    }
    if (ctb != CTB_KEYCTRL)
	fseek(f, pos, SEEK_SET);

    /* file pointer now positioned where compromise cert. should be */
    if (fread(&ctb, 1, 1, f) != 1) {
	status = -1;
	goto ex;
    }
    if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
	cert_len = (int) getpastlength(ctb, f);
	if (cert_len > MAX_SIGCERT_LENGTH) {	/* Huge packet length */
	    status = -1;
	    goto ex;
	}
	/* skip version and mdlen byte */
	fseek(f, 2L, SEEK_CUR);
	if (fread(&class, 1, 1, f) != 1) {
	    status = -1;
	    goto ex;
	}
	status = (class == KC_SIGNATURE_BYTE);
    }
  ex:
    fseek(f, savepos, SEEK_SET);
    return status;
}


/*      Alfred Hitchcock coined the term "mcguffin" for the generic object 
   being sought in his films-- the diamond, the microfilm, etc. 
 */


/*
 * Calculate and display a hash for the public components of the key.
 * The components are converted to their external (big-endian) 
 * representation, concatenated, and an MD5 on the bit values 
 * (i.e. excluding the length value) calculated and displayed in hex.
 *
 * The hash, or "fingerprint", of the key is useful mainly for quickly
 * and easily verifying over the phone that you have a good copy of 
 * someone's public key.  Just read the hash over the phone and have
 * them check it against theirs.
 */
void getKeyHash(byte * hash, unitptr n, unitptr e)
{
    struct MD5Context mdContext;
    byte buffer[MAX_BYTE_PRECISION + 2];
    byte mdBuffer[MAX_BYTE_PRECISION * 2];
    int i, mdIndex = 0, bufIndex;

/* Convert n and e to external (big-endian) byte order and move to mdBuffer */
    i = reg2mpi(buffer, n);
    for (bufIndex = 2; bufIndex < i + 2; bufIndex++)	/* +2 skips count */
	mdBuffer[mdIndex++] = buffer[bufIndex];
    i = reg2mpi(buffer, e);
    for (bufIndex = 2; bufIndex < i + 2; bufIndex++)	/* +2 skips count */
	mdBuffer[mdIndex++] = buffer[bufIndex];

    /* Now evaluate the MD5 for the two MPI's */
    MD5Init(&mdContext);
    MD5Update(&mdContext, mdBuffer, mdIndex);
    MD5Final(hash, &mdContext);

}				/* getKeyHash */


void printKeyHash(byteptr hash, boolean indent)
{
    int i;
#ifdef MACTC5
	char	str[256],*start=str;
#endif

/*      Display the hash.  The format is:
   pub  1024/xxxxxxxx yyyy-mm-dd  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
   Key fingerprint = xx xx xx xx xx xx xx xx  xx xx xx xx xx xx xx xx 
 */
    fprintf(pgpout, "%*s", indent ? 29 : 1, LANG("Key fingerprint ="));
    for (i = 0; i < 8; i++)
	fprintf(pgpout, " %02X", hash[i]);
    putc(' ', pgpout);
    for (i = 8; i < 16; i++)
	fprintf(pgpout, " %02X", hash[i]);
    putc('\n', pgpout);

#ifdef MACTC5
	start+=sprintf(start, "%s", LANG("Key fingerprint =" ) );	/* CP */
	for( i = 0; i < 8; i++ )
		start+=sprintf(start, "%02X ", hash[ i ] );
	*(start++)=' ';
	for( i = 8; i < 16; i++ )
		start+=sprintf(start, "%02X ", hash[ i ] );
	*(--start)=0;					/* Remove trailing space */
	AddResult(str);
#endif
}				/* printKeyHash */


void showKeyHash(unitptr n, unitptr e)
{
    byte hash[16];

    getKeyHash(hash, n, e);	/* compute hash of (n,e) */

    printKeyHash(hash, TRUE);
}				/* showKeyHash */

/*
 * Lists all entries in keyring that have mcguffin string in userid.
 * mcguffin is a null-terminated C string.
 */
int view_keyring(char *mcguffin, char *ringfile, boolean show_signatures,
		 boolean show_hashes)
{
    FILE *f;
    int status;
    char dfltring[MAX_PATH];
    int keycounter = 0;
    FILE *savepgpout;

    /* Default keyring to check signature ID's */
    strcpy(dfltring, globalPubringName);

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	return -1;
    }
    if (show_signatures) {
	setkrent(ringfile);
	setkrent(dfltring);
	init_userhash();
    }
/*      Here's a good format for display of key or signature certificates:
   Type bits/keyID    Date       User ID
   pub  1024/xxxxxxxx yyyy-mm-dd aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
   sec   512/xxxxxxxx yyyy-mm-dd aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
   sig   384/xxxxxxxx yyyy-mm-dd aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
 */

    /* XXX Send this to stdout.  Do we always want to do this?
     * Why couldn't we have a PGPOUT and PGPERR, and then we wouldn't
     * have this problem?  -warlord
     */
    savepgpout = pgpout;
    pgpout = stdout;

    if (moreflag)
	open_more();
    if (!quietmode) {
	fprintf(pgpout, LANG("\nKey ring: '%s'"), ringfile);
	if (mcguffin && strlen(mcguffin) > 0)
	    fprintf(pgpout,
		    LANG(", looking for user ID \"%s\"."),
		    LOCAL_CHARSET(mcguffin));
    }

    fprintf(pgpout, "\n");
    kv_title(pgpout);
    status = kvformat_keypacket(f, pgpout, FALSE, mcguffin, ringfile,
                                show_signatures, show_hashes, &keycounter);

    fclose(f);			/* close key file */
    if (show_signatures)
	endkrent();
    if (keycounter == 1)
	fprintf(pgpout, LANG("1 matching key found.\n"));
    else
	fprintf(pgpout, LANG("%d matching keys found.\n"), keycounter);
    close_more();
    pgpout = savepgpout;

    if (status < 0)
	return status;
    if (mcguffin != NULL && *mcguffin != '\0') {
	/* user specified substring */
	if (keycounter == 0)
	    return 67;		/* user not found */
	else if (keycounter > 1)
	    return 1;		/* more than one match */
    }
    return 0;			/* normal return */

}				/* view_keyring */

/*      Lists all entries in keyring that have mcguffin string in userid.
   mcguffin is a null-terminated C string.
   If options is CHECK_NEW, only new signatures are checked and are
   marked as being checked in the trustbyte (called from addto_keyring).
 */
int dokeycheck(char *mcguffin, char *ringfile, int options)
{
    FILE *f, *fixedf = NULL;
    byte ctb, keyctb = 0;
    long fpsig = 0, fpkey = 0, fixpos = 0, trustpos = -1;
    int status, sigstatus;
    int keypktlen = 0, sigpktlen = 0;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte keyID[KEYFRAGSIZE];
    byte sigkeyID[KEYFRAGSIZE];
    byte keyuserid[256];	/* key certificate userid */
    byte siguserid[256];	/* sig certificate userid */
    char dfltring[MAX_PATH];
    char *tempring = NULL;
    word32 tstamp;
    byte *timestamp = (byte *) & tstamp;	/* key certificate timestamp */
    word32 sigtstamp;
    byte *sigtimestamp = (byte *) & sigtstamp;
    byte sigclass;
    int firstuser = 0;
    int compromised = 0;
    boolean invalid_key = FALSE;	/* unsupported version or bad data */
    boolean failed = FALSE;
    boolean print_userid = FALSE;
    byte sigtrust, newtrust;
    FILE *savepgpout;

    /* Default keyring to check signature ID's */
    strcpy(dfltring, globalPubringName);

    /* open file f, in binary (not text) mode... */
    f = fopen(ringfile, FOPRWBIN);
    if (f == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	return -1;
    }
/*      Here's a good format for display of key or signature certificates:
   Type bits/keyID   Date       User ID
   pub  1024/xxxxxx yyyy-mm-dd  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
   sec   512/xxxxxx yyyy-mm-dd  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
   sig   384/xxxxxx yyyy-mm-dd  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
 */

    /* XXX Send this to stdout.  Do we always want to do this?
     * Why couldn't we have a PGPOUT and PGPERR, and then we wouldn't
     * have this problem?  -warlord
     */
    savepgpout = pgpout;
    pgpout = stdout;

    if (options & CHECK_NEW) {
	fprintf(pgpout, LANG("\nChecking signatures...\n"));
    } else {
	if (moreflag)
	    open_more();
	if (!quietmode) {
	    fprintf(pgpout, LANG("\nKey ring: '%s'"), ringfile);
	    if (mcguffin && strlen(mcguffin) > 0)
		fprintf(pgpout, LANG(", looking for user ID \"%s\"."),
			LOCAL_CHARSET(mcguffin));
	}
        fprintf(pgpout, "\n");
        kv_title(pgpout);
    }
    for (;;) {
	long fpos = ftell(f);
	status = readkeypacket(f, FALSE, &ctb, timestamp, (char *) keyuserid,
			       n, e,
			       NULL, NULL, NULL, NULL, sigkeyID, NULL);
	/* Note that readkeypacket has called set_precision */
	if (status == -1)
	    break;		/* eof reached */
	if (status == -4 || status == -6) {
	    /* only ctb and userid are valid */
	    memset(sigkeyID, 0, KEYFRAGSIZE);
	    tstamp = 0;
	} else if (status < 0) {
	    fprintf(pgpout, LANG("\n\007Could not read key from file '%s'.\n"),
		    ringfile);
	    fclose(f);		/* close key file */
	    return -1;
	}
	if (is_key_ctb(ctb)) {
	    firstuser = 1;
	    keyctb = ctb;
	    fpkey = fpos;
	    keypktlen = (int) (ftell(f) - fpkey);
	    compromised = is_compromised(f);
	    if (status < 0) {
		invalid_key = TRUE;
		memset(keyID, 0, KEYFRAGSIZE);
	    } else {
		invalid_key = FALSE;
		extract_keyID(keyID, n);
	    }
	    if (options & CHECK_NEW)
		print_userid = TRUE;
	}
	if (ctb == CTB_USERID) {
#ifdef MACTC5
		mac_poll_for_break();
#endif
	    PascalToC((char *) keyuserid);
	} else if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
	    fpsig = fpos;
	    sigpktlen = (int) (ftell(f) - fpsig);
	} else {
	    continue;
	}

	trustpos = ftell(f);
	status = read_trust(f, &sigtrust);
	if (status == -1)
	    break;		/* EOF */
	if (status == -7) {
	    trustpos = -1;
	    continue;	    /* not a keyring or this was a compromise cert. */
	}
	if (status < 0) {
	    fclose(f);
	    return status;
	}
	if (options & CHECK_NEW) {
	    if (!is_ctb_type(ctb, CTB_SKE_TYPE))
		continue;
	    if (sigtrust & KC_SIG_CHECKED)
		continue;
	    /* addto_keyring has called setkrent() */
	    if (user_from_keyID(sigkeyID) == NULL)
		continue;	/* unknown signator */
	}
	/* If we don't list the signatures, continue */
	if (!(options & CHECK_NEW) &&
	    !userid_match((char *) keyuserid, mcguffin, n))
	    continue;

	if (ctb == CTB_USERID || print_userid) {
	    /* CHECK_NEW: only print userid if it has new signature */
	    print_userid = FALSE;
	    if (firstuser) {
		if (is_ctb_type(keyctb, CTB_CERT_PUBKEY_TYPE))
		    fprintf(pgpout, LANG("pub"));
		else if (is_ctb_type(keyctb, CTB_CERT_SECKEY_TYPE))
		    fprintf(pgpout, LANG("sec"));
		else
		    fprintf(pgpout, "???");
		if (invalid_key)
		    fprintf(pgpout, "? ");
		else
		    fprintf(pgpout, "  ");
		fprintf(pgpout, "%4d/%s %s ",
			countbits(n), keyIDstring(keyID), cdate(&tstamp));
	    } else {
		fprintf(pgpout, "          %s            ",
			blankkeyID);
	    }
	    if (compromised && firstuser) {
		fprintf(pgpout, LANG("*** KEY REVOKED ***\n"));
		fprintf(pgpout, "          %s              ",
			blankkeyID);
	    }
	    firstuser = 0;
	    fprintf(pgpout, "%s\n", LOCAL_CHARSET((char *) keyuserid));
	}
	/* Ignore comments and anything else */
	if (!is_ctb_type(ctb, CTB_SKE_TYPE))
	    continue;

	/* So now we're checking a signature... */
	/* Try checking signature on either this ring or dflt ring */

	CToPascal((char *) keyuserid);
	sigstatus = check_key_sig(f, fpkey, keypktlen,
				  (char *) keyuserid, f, fpsig,
				  ringfile, (char *) siguserid,
				  sigtimestamp, &sigclass);
	if (sigstatus == -2 && strcmp(ringfile, dfltring) != 0) {
	    sigstatus = check_key_sig(f, fpkey, keypktlen,
				      (char *) keyuserid, f, fpsig,
				      dfltring, (char *) siguserid,
				      sigtimestamp, &sigclass);
	}
	/*
	 * Note: sigstatus has the following values:
	 *   0 Good signature
	 *  -1 Generic error
	 *  -2 Can't find key
	 *  -3 Key too big
	 *  -4 Key too small
	 *  -5 Maybe malformed RSA (RSAREF)
	 *  -6 Unknown PK algorithm
	 *  -7 Unknown conventional algorithm
	 *  -8 Unknown version
	 *  -9 Malformed RSA packet
	 * -10 Malformed packet
	 * -20 BAD SIGNATURE
	 */
	PascalToC((char *) keyuserid);
	fseek(f, fpsig + sigpktlen, SEEK_SET);
	if (sigclass == KC_SIGNATURE_BYTE)
	    fprintf(pgpout, LANG("com"));
	else
	    fprintf(pgpout, LANG("sig"));
	if (sigstatus >= 0)
	    fputs("!      ", pgpout);	/* Good */
	else if (status < 0 || sigstatus == -2 || sigstatus == -3)
	    fputs("?      ", pgpout);	/* Uncheckable */
	else if (sigstatus != -20)
	    fputs("%      ", pgpout);	/* Malformed */
	else
	    fputs("*      ", pgpout);	/* BAD! */

	showkeyID(sigkeyID, pgpout);

	/* If we got a keyID, show it */
	if (sigstatus >= 0 || sigstatus == -3 ||
	    (sigstatus <= -5 && sigstatus >= -9) ||
	    sigstatus == -20) {
	    PascalToC((char *) siguserid);
	    fprintf(pgpout, " %s ", cdate(&sigtstamp));
	    if (sigclass != KC_SIGNATURE_BYTE)
		putc(' ', pgpout);
	    fputs(LOCAL_CHARSET((char *) siguserid), pgpout);
	    putc('\n', pgpout);
	    /* If an error, prepare next line for message */
	    if (sigstatus < 0)
		fprintf(pgpout, "          %s             ",
			blankkeyID);
	} else {
	    /* Indent error messages past date field */
	    fprintf(pgpout, "             ");
	}

	/* Compute new trust */
	newtrust = sigtrust;
	if (sigstatus >= 0) {
	    newtrust |= KC_SIG_CHECKED;
	} else if (sigstatus == -2) {
	    newtrust |= KC_SIG_CHECKED;
	    newtrust &= ~KC_SIGTRUST_MASK;
	} else {
	    newtrust &= ~KC_SIGTRUST_MASK & ~KC_SIG_CHECKED;
	    newtrust |= KC_SIGTRUST_UNTRUSTED;
	}

	/* If it changed, write it out */
	if (trustpos > 0 && newtrust != sigtrust)
	    write_trust_pos(f, newtrust, trustpos);
	if (sigstatus >= 0)
	    continue;		/* Skip error code */

	/* An error: print an appropriate message */
	if (sigstatus == -2)
	    fprintf(pgpout, LANG("(Unknown signator, can't be checked)"));
	else if (sigstatus == -3)
	    fprintf(pgpout, LANG("(Key too long, can't be checked)"));
	else if (sigstatus == -5)
	    fprintf(pgpout, LANG("(Malformed or obsolete signature format)"));
	else if (sigstatus == -6)
	    fprintf(pgpout, LANG("(Unknown public-key algorithm)"));
	else if (sigstatus == -7)
	    fprintf(pgpout, LANG("(Unknown hash algorithm)"));
	else if (sigstatus == -8)
	    fprintf(pgpout, LANG("(Unknown signature packet version)"));
	else if (sigstatus == -9)
	    fprintf(pgpout, LANG("(Malformed signature)"));
	else if (sigstatus == -10)
	    fprintf(pgpout, LANG("(Corrupted signature packet)"));
	else if (sigstatus == -20)
	    fprintf(pgpout, LANG("\007**** BAD SIGNATURE! ****"));
	else
	    fprintf(pgpout, "(Unexpected signature error %d)", sigstatus);
	putc('\n', pgpout);

	/*
	 * If the signature was not too bad, leave it on the key
	 * ring.
	 */
	if (sigstatus == -2 || sigstatus == -3)
	    continue;
	/*
	 * The signature was unacceptable, and
	 * likely to remain that way, so remove it
	 * from the keyring.
	 */
	if (!failed) {
	    /* first bad signature: create scratch file */
	    tempring = tempfile(TMP_TMPDIR);
	    fixedf = fopen(tempring, FOPWBIN);
	    failed = TRUE;
	}
	if (fixedf != NULL) {
	    copyfilepos(f, fixedf, fpsig - fixpos, fixpos);
	    fseek(f, fpsig + sigpktlen, SEEK_SET);
	    if (nextkeypacket(f, &ctb) < 0 || ctb != CTB_KEYCTRL)
		fseek(f, fpsig + sigpktlen, SEEK_SET);
	    fixpos = ftell(f);
	}
    }				/* loop for all packets */

    close_more();
    pgpout = savepgpout;

    if (status < -1) {
	fclose(f);
	return status;
    }
    fputc('\n', pgpout);

    if (failed && fixedf) {
	copyfilepos(f, fixedf, -1L, fixpos);
	fclose(f);
	if (write_error(fixedf)) {
	    fclose(fixedf);
	    return -1;
	}
	fclose(fixedf);
	if (!batchmode)
	    fprintf(pgpout, LANG("Remove bad signatures (Y/n)? "));
	if (batchmode || getyesno('y')) {
	    savetempbak(tempring, ringfile);
	    failed = 0;
	}
    } else {
	fclose(f);
    }

    return 0;			/* normal return */

}				/* dokeycheck */

int backup_rename(char *scratchfile, char *destfile)
{
    /* rename scratchfile to destfile after making a backup file */
    char bakfile[MAX_PATH];

    if (is_tempfile(destfile)) {
	remove(destfile);
    } else {
	if (file_exists(destfile)) {
	    strcpy(bakfile, destfile);
	    force_extension(bakfile, BAK_EXTENSION);
	    remove(bakfile);
	    rename(destfile, bakfile);
	}
    }
    return rename2(scratchfile, destfile);
}

/* Lists all signatures for keys with specified mcguffin string, and asks
 * if they should be removed.
 */
int remove_sigs(char *mcguffin, char *ringfile)
{
    FILE *f, *g;
    byte ctb;
    long fp, fpuser;
    int packetlength;
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte sigkeyID[KEYFRAGSIZE];
    byte userid[256];		/* key certificate userid */
    char dfltring[MAX_PATH];
    word32 tstamp;
    byte *timestamp = (byte *) & tstamp;	/* key certificate timestamp */
    int nsigs = 0, nremoved = 0;
    int keeping;
    char *scratchf;

    /* Default keyring to check signature ID's */
    strcpy(dfltring, globalPubringName);

    if (!mcguffin || strlen(mcguffin) == 0)
	return -1;

    setoutdir(ringfile);
    scratchf = tempfile(0);

    strcpy((char *) userid, mcguffin);

    fprintf(pgpout,
	    LANG("\nRemoving signatures from userid '%s' in key ring '%s'\n"),
	    LOCAL_CHARSET(mcguffin), ringfile);

    status = getpublickey(GPK_GIVEUP | GPK_SHOW, ringfile, &fp,
			  &packetlength, NULL, timestamp, userid, n, e);
    if (status < 0) {
	fprintf(pgpout, LANG("\n\007Key not found in key ring '%s'.\n"),
		ringfile);
	return 0;		/* normal return */
    }
    strcpy((char *) userid, mcguffin);
    getpubuserid(ringfile, fp, userid, &fpuser, &packetlength, FALSE);
    packetlength += (int) (fpuser - fp);

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    /* Count signatures */
    fseek(f, fp + packetlength, SEEK_SET);
    for (;;) {
	status = nextkeypacket(f, &ctb);
	if (status < 0 || is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;
	if (is_ctb_type(ctb, CTB_SKE_TYPE))
	    ++nsigs;
    }

    rewind(f);

    if (nsigs == 0) {
	fprintf(pgpout, LANG("\nKey has no signatures to remove.\n"));
	fclose(f);
	return 0;		/* Normal return */
    }
    fprintf(pgpout, LANG("\nKey has %d signature(s):\n"), nsigs);

    /* open file g for writing, in binary (not text) mode... */
    if ((g = fopen(scratchf, FOPWBIN)) == NULL) {
	fclose(f);
	return -1;
    }
    copyfile(f, g, fp + packetlength);	/* copy file f to g up through key */

    /* Now print out any following sig certs */
    keeping = 1;
    for (;;) {
	fp = ftell(f);
	status = readkeypacket(f, FALSE, &ctb, NULL, NULL, NULL, NULL,
			       NULL, NULL, NULL, NULL, sigkeyID, NULL);
	packetlength = (int) (ftell(f) - fp);
	if ((status < 0 && status != -6 && status != -4) ||
	    is_key_ctb(ctb) || ctb == CTB_USERID)
	    break;
	if (is_ctb_type(ctb, CTB_SKE_TYPE)) {
	    fprintf(pgpout, LANG("sig"));
	    fprintf(pgpout, "%c     ", status < 0 ? '?' : ' ');
	    if (status < 0)
		memset(sigkeyID, 0, KEYFRAGSIZE);
	    showkeyID(sigkeyID, pgpout);
	    fprintf(pgpout, "               ");	/* Indent signator userid */
	    if (getpublickey(GPK_GIVEUP, ringfile, NULL, NULL,
			     sigkeyID, timestamp, userid, n, e) >= 0 ||
		getpublickey(GPK_GIVEUP, dfltring, NULL, NULL,
			     sigkeyID, timestamp, userid, n, e) >= 0) {
		PascalToC((char *) userid);
		fprintf(pgpout, "%s\n", LOCAL_CHARSET((char *) userid));
	    } else {
		fprintf(pgpout,
			LANG("(Unknown signator, can't be checked)\n"));
	    }
	    fprintf(pgpout, LANG("Remove this signature (y/N)? "));
	    if (!(keeping = !getyesno('n')))
		++nremoved;
	}
	if (keeping)
	    copyfilepos(f, g, (long) packetlength, fp);
    }				/* scanning sig certs */
    copyfilepos(f, g, -1L, fp);	/* Copy rest of file */

    fclose(f);			/* close key file */
    if (write_error(g)) {
	fclose(g);
	return -1;
    }
    fclose(g);			/* close scratch file */
    savetempbak(scratchf, ringfile);
    if (nremoved == 0)
	fprintf(pgpout, LANG("\nNo key signatures removed.\n"));
    else
	fprintf(pgpout, LANG("\n%d key signature(s) removed.\n"), nremoved);

    return 0;			/* normal return */

}				/* remove_sigs */

/*
 * Remove the first entry in key ring that has mcguffin string in userid.
 * Or it removes the first matching keyID from the ring.
 * A non-NULL keyID takes precedence over a mcguffin specifier.
 * mcguffin is a null-terminated C string.
 * If secring_too is TRUE, the secret keyring is also checked.
 */
int remove_from_keyring(byte * keyID, char *mcguffin,
			char *ringfile, boolean secring_too)
{
    FILE *f;
    FILE *g;
    long fp, nfp;
    int packetlength;
    byte ctb;
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte userid[256];		/* key certificate userid */
    word32 tstamp;
    byte *timestamp = (byte *) & tstamp;	/* key certificate timestamp */
    int userids;
    boolean rmuserid = FALSE;
    char *scratchf;
    unsigned secflag = 0;

    default_extension(ringfile, PGP_EXTENSION);

    if ((keyID == NULL) && (!mcguffin || strlen(mcguffin) == 0))
	return -1;	/* error, null mcguffin will match everything */

  top:
    if (mcguffin)
	strcpy((char *) userid, mcguffin);

    fprintf(pgpout, LANG("\nRemoving from key ring: '%s'"), ringfile);
    if (mcguffin && strlen(mcguffin) > 0)
	fprintf(pgpout, LANG(", userid \"%s\".\n"),
		LOCAL_CHARSET(mcguffin));

    status = getpublickey(secflag | GPK_GIVEUP | GPK_SHOW, ringfile, &fp,
			  &packetlength, NULL, timestamp, userid, n, e);
    if (status < 0 && status != -4 && status != -6) {
	fprintf(pgpout, LANG("\n\007Key not found in key ring '%s'.\n"),
		ringfile);
	return 0;		/* normal return */
    }
    /* Now add to packetlength the subordinate following certificates */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    fseek(f, fp + packetlength, SEEK_SET);
    userids = 0;
    do {			/* count user ID's, position nfp at next key */
	nfp = ftell(f);
	status = nextkeypacket(f, &ctb);
	if (status == 0 && ctb == CTB_USERID)
	    ++userids;
    } while (status == 0 && !is_key_ctb(ctb));
    if (status < -1) {
	fclose(f);
	return -1;
    }
    if (keyID == NULL) {	/* Human confirmation is required. */
	/* Supposedly the key was fully displayed by getpublickey */
	if (userids > 1) {
	    fprintf(pgpout, LANG("\nKey has more than one user ID.\n\
Do you want to remove the whole key (y/N)? "));
	    if (!getyesno('n')) {
		/* find out which userid should be removed */
		rmuserid = TRUE;
		fseek(f, fp + packetlength, SEEK_SET);
		for (;;) {
		    fp = ftell(f);
		    status = readkpacket(f, &ctb, (char *) userid, NULL, NULL);
		    if (status < 0 && status != -4 && status != -6
			|| is_key_ctb(ctb)) {
			fclose(f);
			fprintf(pgpout, LANG("\nNo more user ID's\n"));
			return -1;
		    }
		    if (ctb == CTB_USERID) {
			fprintf(pgpout, LANG("Remove \"%s\" (y/N)? "), userid);
			if (getyesno('n'))
			    break;
		    }
		}
		do {		/* also remove signatures and trust bytes */
		    nfp = ftell(f);
		    status = nextkeypacket(f, &ctb);
		} while ((status == 0 || status == -4 || status == -6) &&
			 !is_key_ctb(ctb) && ctb != CTB_USERID);
		if (status < -1 && status != -4 && status != -6) {
		    fclose(f);
		    return -1;
		}
	    }
	} else if (!force_flag) {	/* only one user ID */
	    fprintf(pgpout,
	       LANG("\nAre you sure you want this key removed (y/N)? "));
	    if (!getyesno('n')) {
		fclose(f);
		return -1;	/* user said "no" */
	    }
	}
    }
    fclose(f);
    packetlength = (int) (nfp - fp);

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    setoutdir(ringfile);
    scratchf = tempfile(0);
    /* open file g for writing, in binary (not text) mode... */
    if ((g = fopen(scratchf, FOPWBIN)) == NULL) {
	fclose(f);
	return -1;
    }
    copyfilepos(f, g, fp, 0L);	/* copy file f to g up to position fp */
    copyfilepos(f, g, -1L, fp + packetlength);	/* copy rest of file f */
    fclose(f);			/* close key file */
    if (write_error(g)) {
	fclose(g);
	return -1;
    }
    fclose(g);			/* close scratch file */
    if (secring_too)		/* TRUE if this is the public keyring */
	maint_update(scratchf, 0);
    savetempbak(scratchf, ringfile);
    if (rmuserid)
	fprintf(pgpout, LANG("\nUser ID removed from key ring.\n"));
    else
	fprintf(pgpout, LANG("\nKey removed from key ring.\n"));

    if (secring_too) {
	secring_too = FALSE;
	strcpy(ringfile, globalSecringName);
	strcpy((char *) userid, mcguffin);
	if (getpublickey(GPK_GIVEUP | GPK_SECRET, ringfile, NULL,
			 NULL, NULL, timestamp, userid, n, e) == 0) {
	    fprintf(pgpout,
LANG("\nKey or user ID is also present in secret keyring.\n\
Do you also want to remove it from the secret keyring (y/N)? "));
	    if (getyesno('n')) {
		secflag = GPK_SECRET;
		goto top;
	    }
	}
    }
    return 0;			/* normal return */

}				/* remove_from_keyring */

/*
 * Copy the first entry in key ring that has mcguffin string in
 * userid and put it into keyfile.
 * mcguffin is a null-terminated C string.
 */
int extract_from_keyring(char *mcguffin, char *keyfile, char *ringfile,
			 boolean transflag)
{
    FILE *f;
    FILE *g;
    long fp;
    int packetlength = 0;
    byte ctb;
    byte keyctrl;
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte keyID[KEYFRAGSIZE];
    byte userid[256];		/* key certificate userid */
    char fname[MAX_PATH], transfile[MAX_PATH], transname[MAX_PATH];
    char *tempf = NULL;
    word32 tstamp;
    byte *timestamp = (byte *) & tstamp;	/* key cert tstamp */
    boolean append = FALSE;
    boolean whole_ring = FALSE;

    default_extension(ringfile, PGP_EXTENSION);

    if (!mcguffin || strlen(mcguffin) == 0 || strcmp(mcguffin, "*") == 0)
	whole_ring = TRUE;

    /* open file f for read, in binary (not text) mode... */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    if (!whole_ring) {
	strcpy((char *) userid, mcguffin);
	fprintf(pgpout, LANG("\nExtracting from key ring: '%s'"), ringfile);
	fprintf(pgpout, LANG(", userid \"%s\".\n"), LOCAL_CHARSET(mcguffin));

	status = getpublickey(GPK_GIVEUP | GPK_SHOW,
			      ringfile, &fp, &packetlength, NULL,
			      timestamp, userid, n, e);
	if (status < 0 && status != -4 && status != -6) {
	    fprintf(pgpout, LANG("\n\007Key not found in key ring '%s'.\n"),
		    ringfile);
	    fclose(f);
	    return 1;		/* non-normal return */
	}
	extract_keyID(keyID, n);
    } else {
	do			/* set fp to first key packet */
	    fp = ftell(f);
	while ((status = nextkeypacket(f, &ctb)) >= 0 && !is_key_ctb(ctb));
	if (status < 0) {
	    fclose(f);
	    return -1;
	}
	packetlength = (int) (ftell(f) - fp);
    }

    if (!keyfile || strlen(keyfile) == 0) {
	fprintf(pgpout, "\n");
	fprintf(pgpout, LANG("Extract the above key into which file?"));
	fprintf(pgpout, " ");
	if (batchmode)
	    return -1;
#ifdef MACTC5
	if(!GetFilePath(LANG("Extract the above key into which file?"), fname, PUTFILE)) {
		Putchar('\n');
		fname[0]='\0';
		return -1;
		}
	fprintf(pgpout,fname);
#else
	getstring(fname, sizeof(fname) - 4, TRUE);
#endif
	if (*fname == '\0')
	    return -1;
    } else {
	strcpy(fname, keyfile);
    }
    default_extension(fname, PGP_EXTENSION);

    /* If transport armoring, use a dummy file for keyfile */
    if (transflag) {
	strcpy(transname, fname);
	strcpy(transfile, fname);
	force_extension(transfile, ASC_EXTENSION);
	tempf = tempfile(TMP_TMPDIR | TMP_WIPE);
	strcpy(fname, tempf);
    }
    if (file_exists(transflag ? transfile : fname)) {
	if (!transflag && !whole_ring) {
	    /* see if the key is already present in fname */
	    status = getpublickey(GPK_GIVEUP, fname, NULL, NULL, keyID,
				  timestamp, userid, n, e);
	    if (status >= 0 || status == -4 || status == -6) {
		fclose(f);
		fprintf(pgpout,
		  LANG("Key ID %s is already included in key ring '%s'.\n"),
			keyIDstring(keyID), fname);
		return -1;
	    }
	}
	if (whole_ring || transflag || status < -1) {
	    if (!is_tempfile(fname) && !force_flag)
		/* Don't ask this for mailmode or for 
		 * a tempfile, since its ok.
		 */
	    {
/* if status < -1 then fname is not a keyfile,
   ask if it should be overwritten */
		fprintf(pgpout,
	     LANG("\n\007Output file '%s' already exists.  Overwrite (y/N)? "),
			transflag ? transfile : fname);
		if (!getyesno('n')) {
		    fclose(f);
		    return -1;	/* user chose to abort */
		}
	    }
	} else {
	    append = TRUE;
	}
    }
    if (append)
	g = fopen(fname, FOPRWBIN);
    else
	g = fopen(fname, FOPWBIN);
    if (g == NULL) {
	if (append)
	    fprintf(pgpout,
		    LANG("\n\007Can't open key ring file '%s'\n"), ringfile);
	else
	    fprintf(pgpout,
		    LANG("\n\007Unable to create key file '%s'.\n"), fname);
	fclose(f);
	return -1;
    }
    if (append)
	fseek(g, 0L, SEEK_END);
    do {
	/* file f is positioned right after key packet */
	if (whole_ring && read_trust(f, &keyctrl) == 0
	    && (keyctrl & KC_DISABLED)) {
	    do {		/* skip this key */
		fp = ftell(f);
		status = nextkeypacket(f, &ctb);
		packetlength = (int) (ftell(f) - fp);
	    }
	    while (!is_key_ctb(ctb) && status >= 0);
	    continue;
	}
	if (copyfilepos(f, g, (long) packetlength, fp) < 0) {
	    /* Copy key out */
	    status = -2;
	    break;
	}
	/* Copy any following signature or userid packets */
	for (;;) {
	    fp = ftell(f);
	    status = nextkeypacket(f, &ctb);
	    packetlength = (int) (ftell(f) - fp);
	    if (status < 0 || is_key_ctb(ctb))
		break;
	    if (ctb == CTB_USERID || is_ctb_type(ctb, CTB_SKE_TYPE))
		if (copyfilepos(f, g, (long) packetlength, fp) < 0) {
		    status = -2;
		    break;
		}
	}
    }
    while (whole_ring && status >= 0);

    fclose(f);
    if (status < -1 || write_error(g)) {
	fclose(g);
	return -1;
    }
    fclose(g);

    if (transflag) {
        do {
            char *t;
            force_extension(transfile, ASC_EXTENSION);
            if (!file_exists(transfile)) break;
            t=ck_dup_output(transfile, TRUE, TRUE);
            if (t==NULL) user_error();
            strcpy(transfile,t);
        } while (TRUE);
	status = armor_file(fname, transfile, transname, NULL, !whole_ring);
	rmtemp(tempf);
	if (status)
	    return -1;
    }
    fprintf(pgpout, LANG("\nKey extracted to file '%s'.\n"),
	    transflag ? transfile : fname);

    return 0;			/* normal return */
}				/* extract_from_keyring */


/*======================================================================*/

/* Copy the key data in keyfile into ringfile, replacing the data that
   is in ringfile starting at fp and for length packetlength.
   keylen is the number of bytes to copy from keyfile
 */
static int merge_key_to_ringfile(char *keyfile, char *ringfile, long fp,
				 int packetlength, long keylen)
{
    FILE *f, *g, *h;
    char *tempf;
    int rc;

    setoutdir(ringfile);
    tempf = tempfile(TMP_WIPE);
    /* open file f for reading, binary, as keyring file */
    if ((f = fopen(ringfile, FOPRBIN)) == NULL)
	return -1;
    /* open file g for writing, binary, as scratch keyring file */
    if ((g = fopen(tempf, FOPWBIN)) == NULL) {
	fclose(f);
	return -1;
    }
    /* open file h for reading, binary, as key file to be inserted */
    if ((h = fopen(keyfile, FOPRBIN)) == NULL) {
	fclose(f);
	fclose(g);
	return -1;
    }
    /* Copy pre-key keyring data from f to g */
    copyfile(f, g, fp);
    /* Copy temp key data from h to g */
    copyfile(h, g, keylen);
    /* Copy post-key keyring data from f to g */
    copyfilepos(f, g, -1L, fp + packetlength);
    fclose(f);
    rc = write_error(g);
    fclose(g);
    fclose(h);

    if (!rc)
	savetempbak(tempf, ringfile);

    return rc ? -1 : 0;
}				/* merge_key_to_ringfile */

static int insert_userid(char *keyfile, byte * userid, long fpos)
{
    /* insert userid and trust byte at position fpos in file keyfile */
    char *tmpf;
    FILE *f, *g;

    tmpf = tempfile(TMP_TMPDIR);
    if ((f = fopen(keyfile, FOPRBIN)) == NULL)
	return -1;
    if ((g = fopen(tmpf, FOPWBIN)) == NULL) {
	fclose(f);
	return -1;
    }
    copyfile(f, g, fpos);
    putc(CTB_USERID, g);
    fwrite(userid, 1, userid[0] + 1, g);
    write_trust(g, KC_LEGIT_COMPLETE);
    copyfile(f, g, -1L);
    fclose(f);
    if (write_error(g)) {
	fclose(g);
	return -1;
    }
    fclose(g);
    return savetempbak(tmpf, keyfile);
}

int dokeyedit(char *mcguffin, char *ringfile)
/*
 * Edit the userid and/or pass phrase for an RSA key pair, and
 * put them back into the ring files.
 */
{
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION], d[MAX_UNIT_PRECISION],
         p[MAX_UNIT_PRECISION], q[MAX_UNIT_PRECISION], u[MAX_UNIT_PRECISION];
    char *fname, secring[MAX_PATH];
    FILE *f;
    byte userid[256];
    byte userid1[256];
    word32 timestamp;		/* key certificate timestamp */
    byte keyID[KEYFRAGSIZE];
    boolean hidekey;		/* TRUE iff secret key is encrypted */
    boolean changed = FALSE, changeID = FALSE;
    byte ctb;
    int status;
    long fpp, fps, fpu, trust_pos, keylen;
    int pplength = 0, pslength = 0;
    byte ideakey[16];
    byte keyctrl;
    struct IdeaCfbContext cfb;

    if (!ringfile || strlen(ringfile) == 0 || !mcguffin
	|| strlen(mcguffin) == 0)
	return -1;		/* Need ringfile name, user name */

    if (!file_exists(ringfile))
        force_extension(ringfile, PGP_EXTENSION);

    /*
     * Although the name of a secret keyring may change in the future, it
     * is a safe bet that anything named "secring.pgp" will be a secret
     * key ring for the indefinite future.  
     */
    if (!strcmp(file_tail(ringfile), "secring.pgp") ||
	!strcmp(file_tail(ringfile), file_tail(globalSecringName))) {
	fprintf(pgpout,
LANG("\nThis operation may not be performed on a secret keyring.\n\
Defaulting to public keyring."));
	strcpy(ringfile, globalPubringName);
    }
    strcpy((char *) userid, mcguffin);
    fprintf(pgpout, LANG("\nEditing userid \"%s\" in key ring: '%s'.\n"),
	    LOCAL_CHARSET((char *) userid), ringfile);

    if (!file_exists(ringfile)) {
	fprintf(pgpout, LANG("\nCan't open public key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    status = getpublickey(GPK_GIVEUP | GPK_SHOW, ringfile, &fpp, &pplength,
			  NULL, (byte *) & timestamp, userid, n, e);
    if (status < 0) {
	fprintf(pgpout, LANG("\n\007Key not found in key ring '%s'.\n"),
		ringfile);
	return -1;
    }
    /* Now add to pplength any following key control certificate */
    if ((f = fopen(ringfile, FOPRWBIN)) == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open key ring file '%s'\n"),
		ringfile);
	return -1;
    }
    if (fread(&ctb, 1, 1, f) != 1 || !is_ctb_type(ctb, CTB_CERT_PUBKEY_TYPE)) {
	fprintf(pgpout, LANG("\n\007File '%s' is not a public keyring.\n"),
		ringfile);
	fclose(f);
	return -1;
    }
    fseek(f, fpp, SEEK_SET);
    if (is_compromised(f)) {
	fprintf(pgpout,
		LANG("\n\007This key has been revoked by its owner.\n"));
	fclose(f);
	return -1;
    }
    trust_pos = fpp + pplength;
    fseek(f, trust_pos, SEEK_SET);
    if (read_trust(f, &keyctrl) < 0)
	trust_pos = -1;		/* keyfile: no trust byte */

    extract_keyID(keyID, n);

#if 0
    /*
     * Old code: looks in the same directory as the given keyring, but
     * with the secret keyring filename.
     */
    strcpy(secring, ringfile);
    strcpy(file_tail(secring), file_tail(globalSecringName));
    if (!file_exists(secring) && strcmp(file_tail(secring), "secring.pgp")) {
	strcpy(file_tail(secring), "secring.pgp");
    }
#else
    /*
     * What it should do: use the secret keyring, always.
     * Now that the path can be set, this is The Right Thing.
     * It used to be impossible to put the secret and public keyring in
     * different directories, so forcing the same directory name was The
     * Right Thing.  It is no longer.
     */
    strcpy(secring, globalSecringName);
#endif
    if (!file_exists(secring)) {
	fprintf(pgpout, LANG("\nCan't open secret key ring file '%s'\n"),
		secring);
	fclose(f);
	return -1;
    }
    /* Get position of key in secret key file */
    (void) getpublickey(GPK_GIVEUP | GPK_SECRET, secring, &fps, &pslength,
			keyID, (byte *) & timestamp, userid1, n, e);
    /* This was done to get us fps and pslength */
    status = getsecretkey(GPK_GIVEUP, secring, keyID, (byte *) & timestamp,
			  ideakey, &hidekey, userid1, n, e, d, p, q, u);

    if (status < 0) {	/* key not in secret keyring: edit owner trust */
	int i;

	fprintf(pgpout,
LANG("\nNo secret key available.  Editing public key trust parameter.\n"));
	if (trust_pos < 0) {
	    fprintf(pgpout,
		    LANG("\n\007File '%s' is not a public keyring.\n"),
		    ringfile);
	    fclose(f);
	    return -1;
	}
	show_key(f, fpp, SHOW_ALL);

	init_trust_lst();
	fprintf(pgpout, LANG("Current trust for this key's owner is: %s\n"),
		trust_lst[keyctrl & KC_OWNERTRUST_MASK]);

	PascalToC((char *) userid);	/* convert to C string for display */
	i = ask_owntrust((char *) userid, keyctrl);
	if (i == (keyctrl & KC_OWNERTRUST_MASK)) {
	    fclose(f);
	    return 0;		/* unchanged */
	}
	if (i < 0 || i > KC_OWNERTRUST_ALWAYS) {
	    fclose(f);
	    return -1;
	}
	keyctrl = (keyctrl & ~KC_OWNERTRUST_MASK) | i;

	fseek(f, trust_pos, SEEK_SET);
	write_trust(f, keyctrl);
	fclose(f);
	fprintf(pgpout, LANG("Public key ring updated.\n"));
	return 0;
    }
    if (trust_pos > 0 && (keyctrl & (KC_BUCKSTOP | KC_OWNERTRUST_MASK)) !=
	(KC_OWNERTRUST_ULTIMATE | KC_BUCKSTOP)) {
	/* key is in secret keyring but buckstop is not set */
	fprintf(pgpout,
LANG("\nUse this key as an ultimately-trusted introducer (y/N)? "), userid);
	if (getyesno('n')) {
	    fseek(f, trust_pos, SEEK_SET);
	    keyctrl = KC_OWNERTRUST_ULTIMATE | KC_BUCKSTOP;
	    write_trust(f, keyctrl);
	}
    }
    /* Show user her ID again to be clear */
    PascalToC((char *) userid);
    fprintf(pgpout, LANG("\nCurrent user ID: %s"),
	    LOCAL_CHARSET((char *) userid));
    CToPascal((char *) userid);

    fprintf(pgpout, LANG("\nDo you want to add a new user ID (y/N)? "));
    if (getyesno('n')) {	/* user said yes */
	fprintf(pgpout, LANG("\nEnter the new user ID: "));
#ifdef MACTC5
		GetNewUID((char *)userid);
		fprintf(pgpout, "%s\n",userid);
#else
	getstring((char *) userid, 255, TRUE);	/* echo keyboard input */
#endif
	if (userid[0] == '\0') {
	    fclose(f);
	    return -1;
	}
	CONVERT_TO_CANONICAL_CHARSET((char *) userid);
	fprintf(pgpout,
LANG("\nMake this user ID the primary user ID for this key (y/N)? "));
	if (!getyesno('n')) {
	    /* position file pointer at selected user id */
	    int pktlen;
	    long fpuser;

	    strcpy((char *) userid1, mcguffin);
	    if (getpubuserid(ringfile, fpp, userid1, &fpuser, &pktlen,
			     FALSE) < 0) {
		fclose(f);
		return -1;
	    }
	    fseek(f, fpuser, SEEK_SET);
	} else {		/* position file pointer at key packet */
	    fseek(f, fpp, SEEK_SET);
	}
	nextkeypacket(f, &ctb);	/* skip userid or key packet */
	do {			/* new user id will be inserted before next
				   userid or key packet */
	    fpu = ftell(f);
	    if (nextkeypacket(f, &ctb) < 0)
		break;
	} while (ctb != CTB_USERID && !is_key_ctb(ctb));
	CToPascal((char *) userid);	/* convert to length-prefixed string */
	changeID = TRUE;
	changed = TRUE;
    }
    fclose(f);

    fprintf(pgpout, LANG("\nDo you want to change your pass phrase (y/N)? "));
    if (getyesno('n')) {	/* user said yes */
	hidekey = (GetHashedPassPhrase((char *)ideakey, 2) > 0);
	changed = TRUE;
    }
    if (!changed) {
	fprintf(pgpout, LANG("(No changes will be made.)\n"));
	if (hidekey)
	    burn(ideakey);
	goto done;
    }
    /* init CFB IDEA key */
    if (hidekey) {
	ideaCfbInit(&cfb, ideakey);
	burn(ideakey);
    }
    /* First write secret key data to a file */
    fname = tempfile(TMP_TMPDIR | TMP_WIPE);
    writekeyfile(fname, hidekey ? &cfb : 0, timestamp,
		 userid, n, e, d, p, q, u);

    if (hidekey)		/* done with IDEA to protect RSA secret key */
	ideaCfbDestroy(&cfb);

    if (changeID) {
	keylen = -1;
    } else {
	/* don't copy userid */
	f = fopen(fname, FOPRBIN);
	if (f == NULL)
	    goto err;
	nextkeypacket(f, &ctb);	/* skip key packet */
	keylen = ftell(f);
	fclose(f);
    }
    if (merge_key_to_ringfile(fname, secring, fps, pslength, keylen) < 0) {
	fprintf(pgpout, LANG("\n\007Unable to update secret key ring.\n"));
	goto err;
    }
    fprintf(pgpout, LANG("\nSecret key ring updated...\n"));
#ifdef MACTC5
	PGPSetFinfo(secring,'SKey','MPGP');
#endif

    /* Now write public key data to file */
    if (changeID) {
	if (insert_userid(ringfile, userid, fpu) < 0) {
	    fprintf(pgpout, LANG("\n\007Unable to update public key ring.\n"));
	    goto err;
	}

        /* Automatically sign new userid? */
        if (sign_new_userids) {
     	    PascalToC((char *) userid);
            strcpy((char *)userid1, (char *)userid);
            if (do_sign(ringfile, fpp, pplength, userid, keyID, (char *)userid1, TRUE) < 0)
                return -1;
	}

        fprintf(pgpout, LANG("Public key ring updated.\n"));
#ifdef MACTC5
	PGPSetFinfo(ringfile,'PKey','MPGP');
#endif
    } else {
	fprintf(pgpout, LANG("(No need to update public key ring)\n"));
    }

    rmtemp(fname);

  done:
    mp_burn(d);			/* burn sensitive data on stack */
    mp_burn(p);
    mp_burn(q);
    mp_burn(u);
    mp_burn(e);
    mp_burn(n);

    return 0;			/* normal return */
  err:
    mp_burn(d);			/* burn sensitive data on stack */
    mp_burn(p);
    mp_burn(q);
    mp_burn(u);
    mp_burn(e);
    mp_burn(n);

    rmtemp(fname);

    return -1;			/* error return */

}				/* dokeyedit */

int disable_key(char *keyguffin, char *keyfile)
{
    FILE *f;
    byte keyctrl;
    byte keyID[KEYFRAGSIZE];
    byte userid[256];
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    long fp;
    int pktlen;

    strcpy((char *) userid, keyguffin);
    if (getpublickey(GPK_SHOW | GPK_DISABLED, keyfile, &fp, &pktlen, NULL,
		     NULL, userid, n, e) < 0)
	return -1;

    extract_keyID(keyID, n);
    if (getsecretkey(GPK_GIVEUP, NULL, keyID, NULL, NULL, NULL,
		     userid, n, e, NULL, NULL, NULL, NULL) >= 0) {
	/* can only compromise if key also in secring */
	PascalToC((char *) userid);
	fprintf(pgpout,
		LANG("\nDo you want to permanently revoke your public key\n\
by issuing a secret key compromise certificate\n\
for \"%s\" (y/N)? "), LOCAL_CHARSET((char *) userid));
	if (getyesno('n'))
	    return compromise(keyID, keyfile);
    }
    if ((f = fopen(keyfile, FOPRWBIN)) == NULL) {
	fprintf(pgpout,
		LANG("\n\007Can't open key ring file '%s'\n"), keyfile);
	return -1;
    }
    fseek(f, fp + pktlen, SEEK_SET);
    if (read_trust(f, &keyctrl) < 0) {
	fprintf(pgpout,
		LANG("\n\007File '%s' is not a public keyring.\n"), keyfile);
	fprintf(pgpout,
		LANG("You can only disable keys on your public keyring.\n"));
	fclose(f);
	return -1;
    }
    if (keyctrl & KC_DISABLED) {
	fprintf(pgpout, LANG("\nKey is already disabled.\n\
Do you want to enable this key again (y/N)? "));
	keyctrl &= ~KC_DISABLED;
    } else {
	fprintf(pgpout, LANG("\nDisable this key (y/N)? "));
	keyctrl |= KC_DISABLED;
    }
    if (!getyesno('n')) {
	fclose(f);
	return -1;
    }
    write_trust_pos(f, keyctrl, fp + pktlen);
    fclose(f);
    return 0;
}				/* disable_key */


/*======================================================================*/

/*
 * Do an RSA key pair generation, and write them out to the keyring files.
 * numstr is a decimal string, the desired bitcount for the modulus n.
 * numstr2 is a decimal string, the desired bitcount for the exponent e.
 * username is the desired name for the key.
 */
int dokeygen(char *numstr, char *numstr2, char *username)
{
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    unit d[MAX_UNIT_PRECISION], p[MAX_UNIT_PRECISION];
    unit q[MAX_UNIT_PRECISION], u[MAX_UNIT_PRECISION];
    char *fname;
#ifdef MACTC5
    char message[256];
#endif
    word16 iv[4];		/* for IDEA CFB mode, to protect
				   RSA secret key */
    byte userid[256];
    short keybits, ebits;
    word32 tstamp;
    boolean hidekey;		/* TRUE iff secret key is encrypted */
    boolean cryptrandflag;
    byte ideakey[16];
    struct IdeaCfbContext cfb;
    struct hashedpw *hpw;
    byte keyID[KEYFRAGSIZE];
    boolean keygen_OK;

    if (!numstr || strlen(numstr) == 0) {
#ifdef MACTC5
		numstr = (char *)userid;
		if (argc < 5) getRSAkeySize(numstr,numstr2);
#endif
        fputs("\n", pgpout);
	fputs(LANG("Pick your RSA key size:\n\
    1)   512 bits- Low commercial grade, fast but less secure\n\
    2)   768 bits- High commercial grade, medium speed, good security\n\
    3)  1024 bits- \"Military\" grade, slow, highest security\n\
Choose 1, 2, or 3, or enter desired number of bits: "), pgpout);
#ifdef MACTC5
		fprintf(pgpout, "%s\n",numstr);
#else
	numstr = (char *) userid;	/* use userid buffer as scratchpad */
	getstring(numstr, 5, TRUE);	/* echo keyboard */
#endif
    }
#ifdef MACTC5
	else							/* CP: argv[4] contains the new_userid */
		if(argc>4)
			strcpy(new_uid,argv[4]);
#endif
    keybits = 0;
    while ((*numstr >= '0') && (*numstr <= '9'))
	keybits = keybits * 10 + (*numstr++ - '0');

    if (keybits == 0)		/* user entered null response */
	return -1;		/* error return */

    /* Standard default key sizes: */
    if (keybits == 1)
	keybits = 512;		/* Low commercial grade */
    if (keybits == 2)
	keybits = 768;		/* High commercial grade */
    if (keybits == 3)
	keybits = 1024;		/* Military grade */

#ifndef DEBUG
    if (keybits < MIN_KEY_BITS)
	keybits = MIN_KEY_BITS;
    if (keybits > MAX_KEY_BITS)
        keybits = MAX_KEY_BITS;
#else
    if (keybits > MAX_BIT_PRECISION)
	keybits = MAX_BIT_PRECISION;
#endif

    ebits = 0;			/* number of bits in e */
    while ((*numstr2 >= '0') && (*numstr2 <= '9'))
	ebits = ebits * 10 + (*numstr2++ - '0');

    fprintf(pgpout, "\n");
    fprintf(pgpout,
	    LANG("Generating an RSA key with a %d-bit modulus.\n"), keybits);

    if (username == NULL || *username == '\0') {
        /* We need to ask for a username */
        fputs(
LANG("\nYou need a user ID for your public key.  The desired form for this\n\
user ID is your name, followed by your E-mail address enclosed in\n\
<angle brackets>, if you have an E-mail address.\n\
For example:  John Q. Smith <12345.6789@compuserve.com>\n\
Enter a user ID for your public key: \n"), pgpout);
#ifdef VMS
	putch('\n'); /* That last newline was just a return, do a real one */
#endif
#ifdef MACTC5
	strcpy((char *)userid,new_uid);
	fprintf(stdout, "%s\n",new_uid);
#else
	getstring((char *) userid, 255, TRUE);	/* echo keyboard input */
#endif
	if (userid[0] == '\0')	/* user entered null response */
	    return -1;		/* error return */

    } else {
        /* Copy in passed-in username */
        memcpy(userid, username, 255);
	fprintf(pgpout,
	    LANG("Generating RSA key-pair with UserID \"%s\".\n"), userid);
    }
    CONVERT_TO_CANONICAL_CHARSET((char *) userid);
    CToPascal((char *) userid);	/* convert to length-prefixed string */

    fputs(LANG("\nYou need a pass phrase to protect your RSA secret key.\n\
Your pass phrase can be any sentence or phrase and may have many\n\
words, spaces, punctuation, or any other printable characters.\n"), pgpout);
    hidekey = (GetHashedPassPhrase((char *)ideakey, 2) > 0);
    /* init CFB IDEA key */
#ifdef MACTC5
    if (Abort)
    	exitPGP(-2);
#endif
    if (hidekey) {
        /* Remember password - we need it later when we sign the key */
	hpw = (struct hashedpw *) malloc(sizeof(struct hashedpw));
	if (hpw) {
	    /* If malloc fails, just don't remember the phrase */
	    memcpy(hpw->hash, ideakey, sizeof(hpw->hash));
	    hpw->next = keypasswds;
	    keypasswds = hpw;
	}
	ideaCfbInit(&cfb, ideakey);
	trueRandAccumLater(64);	/* IV for encryption */
    }
/* As rsa_keygen does a major accumulation of random bits, if we need
 * any others for a seed file, let's get them at the same time.
 */
    cryptrandflag = (cryptRandOpen((struct IdeaCfbContext *)0) < 0);
    if (cryptrandflag)
	trueRandAccumLater(192);

    fputs(LANG("\nNote that key generation is a lengthy process.\n"), pgpout);

#ifdef MACTC5
	InitCursor();
	strcpy(message,"Now generating RSA key pair.\rType command-period to abort.");
	c2pstr(message);
	ParamText((uchar *)message,(uchar *)"",(uchar *)"",(uchar *)"");
	ProgressDialog=GetNewDialog(161,nil,(WindowPtr)-1);
#endif

    if (rsa_keygen(n, e, d, p, q, u, keybits, ebits) < 0) {
#ifdef MACTC5
		if (Abort)
			fprintf(pgpout, LANG("Key generation stopped at user request.\n"));
		else
			fprintf(pgpout, LANG("\n\007Keygen failed!\n"));
		DisposDialog(ProgressDialog);
		ProgressDialog=NULL;
		return -1;	/* error return */
	}
	DisposDialog(ProgressDialog);
	ProgressDialog=NULL;
#else
	fputs(LANG("\n\007Keygen failed!\n"), pgpout);
	return -1;		/* error return */
    }
#endif
    putc('\n', pgpout);

    if (verbose) {
	fprintf(pgpout, LANG("Key ID %s\n"), key2IDstring(n));

	mp_display(" modulus n = ", n);
	mp_display("exponent e = ", e);

	fputs(LANG("Display secret components (y/N)?"), pgpout);
	if (getyesno('n')) {
	    mp_display("exponent d = ", d);
	    mp_display("   prime p = ", p);
	    mp_display("   prime q = ", q);
	    mp_display(" inverse u = ", u);
	}
    }
    tstamp = get_timestamp(NULL);	/* Timestamp when key was generated */

    fputc('\007', pgpout); /* sound the bell when done with lengthy process */
    fflush(pgpout);

    /* First, write out the secret key... */
    fname = tempfile(TMP_TMPDIR | TMP_WIPE);
    writekeyfile(fname, hidekey ? &cfb : 0, tstamp, userid, n, e, d, p, q, u);

    mp_burn(d);
    mp_burn(p);
    mp_burn(q);
    mp_burn(u);

    if (hidekey)		/* done with IDEA to protect RSA secret key */
	ideaCfbDestroy(&cfb);

    if (file_exists(globalSecringName)) {
	if (!(keygen_OK = (merge_key_to_ringfile(fname, globalSecringName, 0L, 0, -1L) == 0)))
	    fprintf(pgpout, LANG("\n\007Unable to update secret key ring.\n"));
	rmtemp(fname);
    } else {
	keygen_OK = (savetemp(fname, globalSecringName) != NULL);
    }

    /* Second, write out the public key... */
    if (keygen_OK) {
        fname = tempfile(TMP_TMPDIR | TMP_WIPE);
        writekeyfile(fname, NULL, tstamp, userid, n, e, NULL, NULL, NULL, NULL);
        if (file_exists(globalPubringName)) {
	    if (!(keygen_OK = (merge_key_to_ringfile(fname, globalPubringName, 0L, 0, -1L) == 0)))
	        fprintf(pgpout, LANG("\n\007Unable to update public key ring.\n"));
	    rmtemp(fname);
        } else {
	    keygen_OK = (savetemp(fname, globalPubringName) != NULL);
	}
    }

    /* Finally, sign the newly created userid on the public key... */
    if (keygen_OK && sign_new_userids) {
	long fp;
	int pktlen;
	word32 tstamp; byte *timestamp = (byte *) &tstamp;
        byte sigguffin[256];

	PascalToC((char *) userid);
        extract_keyID(keyID, n);
	getpublickey(GPK_GIVEUP, globalPubringName, &fp, &pktlen, NULL,
	             timestamp, userid, n, e);
	PascalToC((char *) userid);
        strcpy((char *)sigguffin, (char *)userid);
        do_sign(globalPubringName, fp, pktlen, userid, keyID, (char *)sigguffin, TRUE);
    }

    mp_burn(e);
    mp_burn(n);

    if (keygen_OK)
        fputs(LANG("\007Key generation completed.\n"), pgpout);
    else
	return -1;		/* error return */

    /*
     *    If we need a seed file, create it now.
     */
    if (cryptrandflag) {
	trueRandConsume(192);
	cryptRandInit((struct IdeaCfbContext *)0);
	/* It will get saved by exitPGP */
    }
    return 0;			/* normal return */
}				/* dokeygen */

/* Does double duty for both -kv[v] and -kxa  */
void kv_title(FILE *fo)
{
    fprintf(fo, LANG("Type Bits/KeyID    Date       User ID\n"));
    return;
} /* kv_title */

/* Does double duty for both -kv[v] and -kxa  */
/* returns status & keycounter*/
int kvformat_keypacket(FILE *f, FILE *pgpout, boolean one_key,
                       char *mcguffin, char *ringfile,
                       boolean show_signatures, boolean show_hashes,
                       int *keycounter)
{
    byte ctb, keyctb=0;
    int status;
    unit n[MAX_UNIT_PRECISION], e[MAX_UNIT_PRECISION];
    byte keyID[KEYFRAGSIZE];
    byte sigkeyID[KEYFRAGSIZE];
    byte userid[256];               /* key certificate userid */
    char *siguserid;        /* signator userid */
    word32 tstamp;
    byte *timestamp = (byte *) &tstamp;             /* key certificate timestamp */
    int firstuser = 0;
    int compromised = 0;
    boolean shownKeyHash=FALSE;
    boolean invalid_key=FALSE;      /* unsupported version or bad data */
    boolean match = FALSE;
    boolean disabled = FALSE;
    boolean first_key= FALSE;

    for (;;) {
	status = readkeypacket(f, FALSE, &ctb, timestamp, (char *) userid,
			       n, e,
			       NULL, NULL, NULL, NULL, sigkeyID, NULL);
	/* Note that readkeypacket has called set_precision */
	if (status == -1) {
	    status = 0;
	    break;		/* eof reached */
	}
	if (status == -4 || status == -6) {
	    /* only ctb and userid are valid */
	    memset(sigkeyID, 0, KEYFRAGSIZE);
	    tstamp = 0;
	} else if (status < 0) {
	    fprintf(pgpout, LANG("\n\007Could not read key from file '%s'.\n"),
		    ringfile);
	    break;
	}
	if (is_key_ctb(ctb)) {
	    byte keyctrl;

	    firstuser = 1;
	    keyctb = ctb;
	    compromised = is_compromised(f);
	    shownKeyHash = FALSE;
	    if (status < 0) {
		invalid_key = TRUE;
		memset(keyID, 0, KEYFRAGSIZE);
	    } else {
		invalid_key = FALSE;
		extract_keyID(keyID, n);
		if (read_trust(f, &keyctrl) == 0 && (keyctrl & KC_DISABLED))
		    disabled = TRUE;
		else
		    disabled = FALSE;
	    }
	}
	if (ctb != CTB_USERID && !is_ctb_type(ctb, CTB_SKE_TYPE))
	    continue;
	if (ctb == CTB_USERID) {
	    PascalToC((char *) userid);
	    match = userid_match((char *) userid, mcguffin, n);
	}
	if (match) {
	    if (ctb == CTB_USERID) {
		if (firstuser) {
		    (*keycounter)++;
		    if (is_ctb_type(keyctb, CTB_CERT_PUBKEY_TYPE))
			fprintf(pgpout, LANG("pub"));
		    else if (is_ctb_type(keyctb, CTB_CERT_SECKEY_TYPE))
			fprintf(pgpout, LANG("sec"));
		    else
			fprintf(pgpout, "???");
		    if (invalid_key)
			fprintf(pgpout, "? ");
		    else if (disabled)
			fprintf(pgpout, "- ");
		    else
			fprintf(pgpout, "  ");
		    fprintf(pgpout, "%4d/%s %s ",
		       countbits(n), keyIDstring(keyID), cdate(&tstamp));
		} else {
		    fprintf(pgpout, "     %s                 ", blankkeyID);
		}
		if (compromised && firstuser) {
		    fprintf(pgpout, LANG("*** KEY REVOKED ***\n"));
		    fprintf(pgpout, "     %s                 ", blankkeyID);
		}
		firstuser = 0;
		fprintf(pgpout, "%s\n", LOCAL_CHARSET((char *) userid));

		/* Display the hashes for n and e if required */
		if (show_hashes && !shownKeyHash) {
		    showKeyHash(n, e);
		    shownKeyHash = TRUE;
		}
	    } else if (show_signatures &&
		       !(firstuser && compromised)) {
		/* Must be sig cert */
		fprintf(pgpout, LANG("sig"));
		fprintf(pgpout, "%c      ", status < 0 ? '?' : ' ');
		showkeyID(sigkeyID, pgpout);
		fprintf(pgpout, "             "); /* Indent signator userid */
		if ((siguserid = user_from_keyID(sigkeyID)) == NULL)
		    fprintf(pgpout,
			    LANG("(Unknown signator, can't be checked)\n"));
		else
		    fprintf(pgpout, "%s\n", LOCAL_CHARSET(siguserid));
	    }			/* printing a sig cert */
	}			/* if it has mcguffin */
    }				/* loop for all packets */
    return(status);
} /* kvformat_keypacket */


/* ==== language.c ==== */
/*
   language.c - Foreign language translation for PGP
   Finds foreign language "subtitles" for English phrases 
   in external foriegn language text file.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "usuals.h"
#include "fileio.h"
#include "language.h"
#include "pgp.h"
#include "charset.h"
#include "armor.h"

#define SUBTITLES_FILE	"language.txt"
#define LANG_INDEXFILE	"language.idx"

#define	STRBUFSIZE		2048

char language[16] = "en";	/* The language code, defaults to English */
static char *strbuf;
static char lang[16];		/* readstr sets this to the language id of
				   the msg it last read */
static int subtitles_available = 0;
static int line = 0;
/*      subtitles_available is used to determine if we know whether the special
   subtitles_file exists.  subtitles_available has the following values:
   0  = first time thru, we don't yet know if subtitles_file exists.
   1  = we have already determined that subtitles_file exists.
   -1 = we have already determined that subtitles_file does not exist.
 */

#define	NEWLINE		0
#define	COMMENT		1
#define	INSTRING	2
#define	ESCAPE		3
#define	IDENT		4
#define	DONE		5
#define	ERROR		6
#define	ERR1		7

/* Look for and return a quoted string from the file.
 * If nlabort is true, return failure if we find a blank line
 * before we find the opening quote.
 */
static char *
 readstr(FILE * f, char *buf, int nlabort)
{
    int c, d;
    char *p = buf;
    int state = NEWLINE;
    int i = 0;

    while ((c = getc(f)) != EOF) {
	if (c == '\r')
	    continue;
	/* line numbers are only incremented when creating index file */
	if (line && c == '\n')
	    ++line;
	switch (state) {
	case NEWLINE:
	    switch (c) {
	    case '#':
		state = COMMENT;
		break;
	    case '"':
		state = INSTRING;
		break;
	    case '\n':
		if (nlabort) {
		    *buf = '\0';
		    return buf;
		}
	    default:
		if (i == 0 && isalnum(c)) {
		    state = IDENT;
		    lang[i++] = c;
		    break;
		}
		if (!isspace(c)) {
		    fprintf(stderr, "language.txt:%d: syntax error\n", line);
		    state = ERROR;
		}
	    }
	    break;
	case COMMENT:
	    if (c == '\n')
		state = NEWLINE;
	    break;
	case INSTRING:
	    switch (c) {
	    case '\\':
		state = ESCAPE;
		break;
	    case '"':
		state = DONE;
		break;
	    default:
		*p++ = c;
	    }
	    break;
	case ESCAPE:
	    switch (c) {
	    case 'n':
		*p++ = '\n';
		break;
	    case 'r':
		*p++ = '\r';
		break;
	    case 't':
		*p++ = '\t';
		break;
	    case 'e':
#ifdef EBCDIC
		*p++ = ESC;
#else
		*p++ = '\033';
#endif
		break;
	    case 'a':
#ifdef EBCDIC
		*p++ = '\a';
#else
		*p++ = '\007';
#endif
		break;
	    case '#':
	    case '"':
	    case '\\':
		*p++ = c;
		break;
	    case '\n':
		break;
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
		d = c - '0';
		while ((c = fgetc(f)) >= '0' && c <= '7')
		    d = 8 * d + c - '0';
#ifdef EBCDIC
/* dirty hack for \007 chars in LANG:    LANG("\n\007....")
   The right way is to replace all \007 by \a in the args of LANG() */
		if (d == 7) d = '\a';
#endif
		*p++ = d;
		ungetc(c, f);
		break;
	    default:
		fprintf(stderr,
			"language.txt:%d: illegal escape sequence: '\\%c'\n",
			line, c);
		break;
	    }
	    state = INSTRING;
	    break;
	case IDENT:		/* language identifier */
	    if (c == ':') {
		state = NEWLINE;
		break;
	    }
	    if (c == '\n' && strncmp(lang, "No translation", 14) == 0) {
		i = 0;
		state = NEWLINE;
		break;
	    }
	    lang[i++] = c;
	    if (i == 15 || !isalnum(c) && !isspace(c)) {
		lang[i] = '\0';
		fprintf(stderr,
			"language.txt:%d: bad language identifier: '%s'\n",
			line, lang);
		state = ERROR;
		i = 0;
	    }
	    break;
	case DONE:
	    if (c == '\n') {
		lang[i] = '\0';
		*p = '\0';
		return buf;
	    }
	    if (!isspace(c)) {
		fprintf(stderr,
			"language.txt:%d: extra characters after '\"'\n",
			line);
		state = ERROR;
	    }
	    break;
	case ERROR:
	    if (c == '\n')
		state = ERR1;
	    break;
	case ERR1:
	    state = (c == '\n' ? NEWLINE : ERROR);
	    break;
	}
    }
    if (state != NEWLINE)
	fprintf(stderr, "language.txt: unexpected EOF\n");
    return NULL;
}

#ifdef TEST
main()
{
    char buf[2048];

    line = 1;
    while (readstr(stdin, buf, 0)) {
	printf("\nen: <%s>\n", buf);
	while (readstr(stdin, buf, 1) && *buf != '\0')
	    printf("%s: <%s>\n", lang, buf);
    }
    exit(0);
}
#else

static struct indx_ent {
    word32 crc;
    long offset;
} *indx_tbl = NULL;

static int max_msgs = 0;
static long nmsg = 0;

static FILE *langf;

static void init_lang(void);

static int make_indexfile(char *);

/*
 * uses 24-bit CRC function from armor.c
 */
static word32
 message_crc(char *s)
{
    return crcbytes((byte *) s, strlen(s), (word32) 0);
}

/*
 * lookup file offset in indx_tbl
 */
static long lookup_offset(word32 crc)
{
    int i;

    for (i = 0; i < nmsg; ++i)
	if (indx_tbl[i].crc == crc)
	    return indx_tbl[i].offset;
    return -1;
}


/*
 * return foreign translation of s
 */
char *
 LANG(char *s)
{
    long filepos;
#ifdef MACTC5
	extern Boolean contains_yesNo, contains_enough, contains_badpass;
	contains_yesNo = (((void *) strstr(s, "(Y/n)?") != NULL) ||
	    ((void *) strstr(s, "(y/N)?") != NULL));
	contains_enough = ((void *) strstr(s, "Enough") != NULL);
	contains_badpass = (strstr(s, "Bad pass phrase.") != NULL);
#endif /* MACTC5 */
    if (subtitles_available == 0)
	init_lang();
    if (subtitles_available < 0)
	return s;

    filepos = lookup_offset(message_crc(s));
    if (filepos == -1) {
	return s;
    } else {
	fseek(langf, filepos, SEEK_SET);
	readstr(langf, strbuf, 1);
    }

    if (strbuf[0] == '\0')
	return s;

#ifndef EBCDIC /* no conversion for ebcdic printf() messages needed */
    for (s = strbuf; *s; ++s)
	*s = EXT_C(*s);
#endif
    return strbuf;
}


static struct {
    long lang_fsize;		/* size of language.txt */
    char lang[16];		/* language identifier */
    long nmsg;			/* number of messages */
} indx_hdr;


/*
 * initialize the index table: read it from language.idx or create
 * a new one and write it to the index file. A new index file is
 * created if the language set in config.pgp doesn't match the one
 * in language.idx or if the size of language.txt has changed.
 */
static void init_lang()
{
    char indexfile[MAX_PATH];
    char subtitles_file[MAX_PATH];
    FILE *indexf;
#ifdef PGP_SYSTEM_DIR 
    int use_system_wide_lang = 0; 
#endif 

    if (strcmp(language, "en") == 0) {
	subtitles_available = -1;
	return;			/* use default messages */
    }
    buildfilename(subtitles_file, SUBTITLES_FILE);
    langf = fopen(subtitles_file, FOPRBIN); /* Open file in binary mode... */
    if (langf == NULL) {
#ifdef PGP_SYSTEM_DIR
	strcpy(subtitles_file, PGP_SYSTEM_DIR);
	strcat(subtitles_file, SUBTITLES_FILE);
	langf = fopen(subtitles_file, FOPRBIN); /* Open file in binary mode... */
	use_system_wide_lang = 1;
	if (langf == NULL)
#endif
	{
	    subtitles_available = -1;
	    return;
	}
    }
    init_crc();
    strbuf = (char *) malloc(STRBUFSIZE);
    if (strbuf == NULL) {
	fprintf(stderr, "Not enough memory for foreign subtitles\n");
	fclose(langf);
	subtitles_available = -1;
	return;
    }
#ifdef PGP_SYSTEM_DIR
    if (use_system_wide_lang) {
	strcpy(indexfile, PGP_SYSTEM_DIR);
	strcat(indexfile, LANG_INDEXFILE);
    } else
#endif
    buildfilename(indexfile, LANG_INDEXFILE);
    indexf = fopen(indexfile, FOPRBIN);
    if (indexf != NULL) {
	if (fread(&indx_hdr, 1, sizeof(indx_hdr),
		  indexf) == sizeof(indx_hdr) &&
	    indx_hdr.lang_fsize == fsize(langf) &&
	    strcmp(indx_hdr.lang, language) == 0) {
	    nmsg = indx_hdr.nmsg;
	    indx_tbl = (struct indx_ent *) malloc(nmsg *
						  sizeof(struct indx_ent));
	    if (indx_tbl == NULL) {
		fprintf(stderr, "Not enough memory for foreign subtitles\n");
		fclose(indexf);
		fclose(langf);
		subtitles_available = -1;
		return;
	    }
	    if (fread(indx_tbl,
		      sizeof(struct indx_ent), nmsg, indexf) != nmsg) {
		free(indx_tbl);	/* create a new one */
		indx_tbl = NULL;
	    }
	}
	fclose(indexf);
    }
    if (indx_tbl == NULL && make_indexfile(indexfile) < 0) {
	fclose(langf);
	subtitles_available = -1;
    } else {
	subtitles_available = 1;
    }
}


static int make_indexfile(char *indexfile)
{
    FILE *indexf;
    long filepos;
    int total_msgs = 0;
    char *res;

    if (verbose)		/* must be set in config.pgp */
	fprintf(stderr,
		"Creating language index file '%s' for language \"%s\"\n",
		indexfile, language);
    rewind(langf);
    indx_hdr.lang_fsize = fsize(langf);
    strncpy(indx_hdr.lang, language, 15);
    init_crc();
    line = 1;
    nmsg = 0;
    while (readstr(langf, strbuf, 0)) {
	if (nmsg == max_msgs) {
	    if (max_msgs) {
		max_msgs *= 2;
		indx_tbl = (struct indx_ent *) realloc(indx_tbl, max_msgs *
						sizeof(struct indx_ent));
	    } else {
		max_msgs = 400;
		indx_tbl = (struct indx_ent *) malloc(max_msgs *
						sizeof(struct indx_ent));
	    }
	    if (indx_tbl == NULL) {
		fprintf(stderr, "Not enough memory for foreign subtitles\n");
		return -1;
	    }
	}
	++total_msgs;
	indx_tbl[nmsg].crc = message_crc(strbuf);
	if (lookup_offset(indx_tbl[nmsg].crc) != -1)
	    fprintf(stderr,
		    "language.txt:%d: Message CRC not unique: \"%s\"\n",
		    line, strbuf);
	do {
	    filepos = ftell(langf);
	    res = readstr(langf, strbuf, 1);	/* Abort if find newline
						   first */
	} while (res && strbuf[0] != '\0' && strcmp(language, lang) != 0);

	if (res == NULL)
	    break;
	if (strbuf[0] == '\0')	/* No translation */
	    continue;

	indx_tbl[nmsg].offset = filepos;
	++nmsg;
	do
	    res = readstr(langf, strbuf, 1);	/* Abort if find newline
						   first */
	while (res && strbuf[0] != '\0');
    }
    line = 0;
    indx_hdr.nmsg = nmsg;
    if (nmsg == 0) {
	fprintf(stderr, "No translations available for language \"%s\"\n\n",
		language);
	return -1;
    }
    if (verbose || total_msgs != nmsg)
	fprintf(stderr, "%d messages, %d translations\n\n", total_msgs, nmsg);

    if ((indexf = fopen(indexfile, FOPWBIN)) == NULL) {
	fprintf(stderr, "Cannot create %s\n", indexfile);
    } else {
	fwrite(&indx_hdr, 1, sizeof(indx_hdr), indexf);
	fwrite(indx_tbl, sizeof(struct indx_ent), nmsg, indexf);
	if (ferror(indexf) || fclose(indexf))
	    fprintf(stderr, "error writing %s\n", indexfile);
    }
    return 0;
}
#endif				/* TEST */


/* ==== md5.c ==== */
/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */
#include <string.h>		/* for memcpy() */
#include "md5.h"

#ifndef HIGHFIRST
#define byteReverse(buf, len)	/* Nothing */
#else
void byteReverse(unsigned char *buf, unsigned longs);

#ifndef ASM_MD5
/*
 * Note: this code is harmless on little-endian machines.
 */
void byteReverse(unsigned char *buf, unsigned longs)
{
    uint32 t;
    do {
	t = (uint32) ((unsigned) buf[3] << 8 | buf[2]) << 16 |
	    ((unsigned) buf[1] << 8 | buf[0]);
	*(uint32 *) buf = t;
	buf += 4;
    } while (--longs);
}
#endif
#endif

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
void MD5Init(struct MD5Context *ctx)
{
    ctx->buf[0] = 0x67452301;
    ctx->buf[1] = 0xefcdab89;
    ctx->buf[2] = 0x98badcfe;
    ctx->buf[3] = 0x10325476;

    ctx->bits[0] = 0;
    ctx->bits[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
void MD5Update(struct MD5Context *ctx, unsigned char const *buf, unsigned len)
{
    uint32 t;

    /* Update bitcount */

    t = ctx->bits[0];
    if ((ctx->bits[0] = t + ((uint32) len << 3)) < t)
	ctx->bits[1]++;		/* Carry from low to high */
    ctx->bits[1] += len >> 29;

    t = (t >> 3) & 0x3f;	/* Bytes already in shsInfo->data */

    /* Handle any leading odd-sized chunks */

    if (t) {
	unsigned char *p = (unsigned char *) ctx->in + t;

	t = 64 - t;
	if (len < t) {
	    memcpy(p, buf, len);
	    return;
	}
	memcpy(p, buf, t);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32 *) ctx->in);
	buf += t;
	len -= t;
    }
    /* Process data in 64-byte chunks */

    while (len >= 64) {
	memcpy(ctx->in, buf, 64);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32 *) ctx->in);
	buf += 64;
	len -= 64;
    }

    /* Handle any remaining bytes of data. */

    memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern 
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
void MD5Final(unsigned char digest[16], struct MD5Context *ctx)
{
    unsigned count;
    unsigned char *p;

    /* Compute number of bytes mod 64 */
    count = (ctx->bits[0] >> 3) & 0x3F;

    /* Set the first char of padding to 0x80.  This is safe since there is
       always at least one byte free */
    p = ctx->in + count;
    *p++ = 0x80;

    /* Bytes of padding needed to make 64 bytes */
    count = 64 - 1 - count;

    /* Pad out to 56 mod 64 */
    if (count < 8) {
	/* Two lots of padding:  Pad the first block to 64 bytes */
	memset(p, 0, count);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32 *) ctx->in);

	/* Now fill the next block with 56 bytes */
	memset(ctx->in, 0, 56);
    } else {
	/* Pad block to 56 bytes */
	memset(p, 0, count - 8);
    }
    byteReverse(ctx->in, 14);

    /* Append length in bits and transform */
    ((uint32 *) ctx->in)[14] = ctx->bits[0];
    ((uint32 *) ctx->in)[15] = ctx->bits[1];

    MD5Transform(ctx->buf, (uint32 *) ctx->in);
    byteReverse((unsigned char *) ctx->buf, 4);
    memcpy(digest, ctx->buf, 16);
    memset(ctx, 0, sizeof(ctx));	/* In case it's sensitive */
}

#ifndef ASM_MD5

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#ifdef __PUREC__
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f /*(x, y, z)*/ + data,  w = w<<s | w>>(32-s),  w += x )
#else
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x )
#endif

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
void MD5Transform(uint32 buf[4], uint32 const in[16])
{
    register uint32 a, b, c, d;

    a = buf[0];
    b = buf[1];
    c = buf[2];
    d = buf[3];

#ifdef __PUREC__	/* PureC Weirdness... (GG) */
    MD5STEP(F1(b,c,d), a, b, c, d, in[0] + 0xd76aa478L, 7);
    MD5STEP(F1(a,b,c), d, a, b, c, in[1] + 0xe8c7b756L, 12);
    MD5STEP(F1(d,a,b), c, d, a, b, in[2] + 0x242070dbL, 17);
    MD5STEP(F1(c,d,a), b, c, d, a, in[3] + 0xc1bdceeeL, 22);
    MD5STEP(F1(b,c,d), a, b, c, d, in[4] + 0xf57c0fafL, 7);
    MD5STEP(F1(a,b,c), d, a, b, c, in[5] + 0x4787c62aL, 12);
    MD5STEP(F1(d,a,b), c, d, a, b, in[6] + 0xa8304613L, 17);
    MD5STEP(F1(c,d,a), b, c, d, a, in[7] + 0xfd469501L, 22);
    MD5STEP(F1(b,c,d), a, b, c, d, in[8] + 0x698098d8L, 7);
    MD5STEP(F1(a,b,c), d, a, b, c, in[9] + 0x8b44f7afL, 12);
    MD5STEP(F1(d,a,b), c, d, a, b, in[10] + 0xffff5bb1L, 17);
    MD5STEP(F1(c,d,a), b, c, d, a, in[11] + 0x895cd7beL, 22);
    MD5STEP(F1(b,c,d), a, b, c, d, in[12] + 0x6b901122L, 7);
    MD5STEP(F1(a,b,c), d, a, b, c, in[13] + 0xfd987193L, 12);
    MD5STEP(F1(d,a,b), c, d, a, b, in[14] + 0xa679438eL, 17);
    MD5STEP(F1(c,d,a), b, c, d, a, in[15] + 0x49b40821L, 22);

    MD5STEP(F2(b,c,d), a, b, c, d, in[1] + 0xf61e2562L, 5);
    MD5STEP(F2(a,b,c), d, a, b, c, in[6] + 0xc040b340L, 9);
    MD5STEP(F2(d,a,b), c, d, a, b, in[11] + 0x265e5a51L, 14);
    MD5STEP(F2(c,d,a), b, c, d, a, in[0] + 0xe9b6c7aaL, 20);
    MD5STEP(F2(b,c,d), a, b, c, d, in[5] + 0xd62f105dL, 5);
    MD5STEP(F2(a,b,c), d, a, b, c, in[10] + 0x02441453L, 9);
    MD5STEP(F2(d,a,b), c, d, a, b, in[15] + 0xd8a1e681L, 14);
    MD5STEP(F2(c,d,a), b, c, d, a, in[4] + 0xe7d3fbc8L, 20);
    MD5STEP(F2(b,c,d), a, b, c, d, in[9] + 0x21e1cde6L, 5);
    MD5STEP(F2(a,b,c), d, a, b, c, in[14] + 0xc33707d6L, 9);
    MD5STEP(F2(d,a,b), c, d, a, b, in[3] + 0xf4d50d87L, 14);
    MD5STEP(F2(c,d,a), b, c, d, a, in[8] + 0x455a14edL, 20);
    MD5STEP(F2(b,c,d), a, b, c, d, in[13] + 0xa9e3e905L, 5);
    MD5STEP(F2(a,b,c), d, a, b, c, in[2] + 0xfcefa3f8L, 9);
    MD5STEP(F2(d,a,b), c, d, a, b, in[7] + 0x676f02d9L, 14);
    MD5STEP(F2(c,d,a), b, c, d, a, in[12] + 0x8d2a4c8aL, 20);

    MD5STEP(F3(b,c,d), a, b, c, d, in[5] + 0xfffa3942L, 4);
    MD5STEP(F3(a,b,c), d, a, b, c, in[8] + 0x8771f681L, 11);
    MD5STEP(F3(d,a,b), c, d, a, b, in[11] + 0x6d9d6122L, 16);
    MD5STEP(F3(c,d,a), b, c, d, a, in[14] + 0xfde5380cL, 23);
    MD5STEP(F3(b,c,d), a, b, c, d, in[1] + 0xa4beea44L, 4);
    MD5STEP(F3(a,b,c), d, a, b, c, in[4] + 0x4bdecfa9L, 11);
    MD5STEP(F3(d,a,b), c, d, a, b, in[7] + 0xf6bb4b60L, 16);
    MD5STEP(F3(c,d,a), b, c, d, a, in[10] + 0xbebfbc70L, 23);
    MD5STEP(F3(b,c,d), a, b, c, d, in[13] + 0x289b7ec6L, 4);
    MD5STEP(F3(a,b,c), d, a, b, c, in[0] + 0xeaa127faL, 11);
    MD5STEP(F3(d,a,b), c, d, a, b, in[3] + 0xd4ef3085L, 16);
    MD5STEP(F3(c,d,a), b, c, d, a, in[6] + 0x04881d05L, 23);
    MD5STEP(F3(b,c,d), a, b, c, d, in[9] + 0xd9d4d039L, 4);
    MD5STEP(F3(a,b,c), d, a, b, c, in[12] + 0xe6db99e5L, 11);
    MD5STEP(F3(d,a,b), c, d, a, b, in[15] + 0x1fa27cf8L, 16);
    MD5STEP(F3(c,d,a), b, c, d, a, in[2] + 0xc4ac5665L, 23);

    MD5STEP(F4(b,c,d), a, b, c, d, in[0] + 0xf4292244L, 6);
    MD5STEP(F4(a,b,c), d, a, b, c, in[7] + 0x432aff97L, 10);
    MD5STEP(F4(d,a,b), c, d, a, b, in[14] + 0xab9423a7L, 15);
    MD5STEP(F4(c,d,a), b, c, d, a, in[5] + 0xfc93a039L, 21);
    MD5STEP(F4(b,c,d), a, b, c, d, in[12] + 0x655b59c3L, 6);
    MD5STEP(F4(a,b,c), d, a, b, c, in[3] + 0x8f0ccc92L, 10);
    MD5STEP(F4(d,a,b), c, d, a, b, in[10] + 0xffeff47dL, 15);
    MD5STEP(F4(c,d,a), b, c, d, a, in[1] + 0x85845dd1L, 21);
    MD5STEP(F4(b,c,d), a, b, c, d, in[8] + 0x6fa87e4fL, 6);
    MD5STEP(F4(a,b,c), d, a, b, c, in[15] + 0xfe2ce6e0L, 10);
    MD5STEP(F4(d,a,b), c, d, a, b, in[6] + 0xa3014314L, 15);
    MD5STEP(F4(c,d,a), b, c, d, a, in[13] + 0x4e0811a1L, 21);
    MD5STEP(F4(b,c,d), a, b, c, d, in[4] + 0xf7537e82L, 6);
    MD5STEP(F4(a,b,c), d, a, b, c, in[11] + 0xbd3af235L, 10);
    MD5STEP(F4(d,a,b), c, d, a, b, in[2] + 0x2ad7d2bbL, 15);
    MD5STEP(F4(c,d,a), b, c, d, a, in[9] + 0xeb86d391L, 21);
#else
    MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
    MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
    MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
    MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
    MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
    MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
    MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
    MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
    MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
    MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

    MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
    MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
    MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
    MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
    MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
    MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
    MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
    MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

    MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
    MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
    MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
    MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
    MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
    MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
    MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
    MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

    MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
    MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
    MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
    MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
    MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);
#endif

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

#endif


/* ==== mdfile.c ==== */
/*      mdfile.c  - Message Digest routines for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */

#include <stdio.h>
#include "mpilib.h"
#include "mdfile.h"
#include "fileio.h"
#include "language.h"
#include "pgp.h"
#ifdef MACTC5
#include "Macutil3.h"
#endif

/* Begin MD5 routines */

/* Note - the routines in this module, except for MD_addbuffer,
 * do not "finish" the MD5 calculation.  MD_addbuffer finishes the
 * calculation in each case, usually to append the timestamp and class info.
 */

/* Computes the message digest for a file from current position for
   longcount bytes.
   Uses the RSA Data Security Inc. MD5 Message Digest Algorithm */
int MDfile0_len(struct MD5Context *mdContext, FILE * f, word32 longcount)
{
    int bytecount;
    unsigned char buffer[1024];

    MD5Init(mdContext);
    /* Process 1024 bytes at a time... */
    do {
	if (longcount < (word32) 1024)
	    bytecount = (int) longcount;
	else
	    bytecount = 1024;
	bytecount = fread(buffer, 1, bytecount, f);
	if (bytecount > 0) {
	    MD5Update(mdContext, buffer, bytecount);
	    longcount -= bytecount;
#ifdef MACTC5
		mac_poll_for_break();
#endif
	}
	/* if text block was short, exit loop */
    } while (bytecount == 1024);
    return 0;
}				/* MDfile0_len */


/* Computes the message digest for a file from current position to EOF.
   Uses the RSA Data Security Inc. MD5 Message Digest Algorithm */

static int MDfile0(struct MD5Context *mdContext, FILE * inFile)
{
    int bytes;
    unsigned char buffer[1024];

    MD5Init(mdContext);
    while ((bytes = fread(buffer, 1, 1024, inFile)) != 0)
#ifdef MACTC5
		{
		mac_poll_for_break();
		MD5Update(mdContext,buffer,bytes);
		}
#else
	MD5Update(mdContext, buffer, bytes);
#endif
    return 0;
}

/* Computes the message digest for a specified file */

int MDfile(struct MD5Context *mdContext, char *filename)
{
    FILE *inFile;
    inFile = fopen(filename, FOPRBIN);

    if (inFile == NULL) {
	fprintf(pgpout, LANG("\n\007Can't open file '%s'\n"), filename);
	return -1;
    }
    MDfile0(mdContext, inFile);
    fclose(inFile);
    return 0;
}

/* Add a buffer's worth of data to the MD5 computation.  If a digest
 * pointer is supplied, complete the computation and write the digest.
 */
void MD_addbuffer(struct MD5Context *mdContext, byte * buf, int buflen,
		  byte digest[16])
{
    MD5Update(mdContext, buf, buflen);
    if (digest) {
	MD5Final(digest, mdContext);
	burn(*mdContext);	/* Paranoia */
    }
}

/* End MD5 routines */


/* ==== more.c ==== */
/*      more.c  - Unix-style "more" paging output for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef UNIX
#include <sys/types.h>
#endif
#ifdef sco
#include <sys/stream.h>
#include <sys/ptem.h>
FILE *popen();
#endif
#include "system.h"
#include "mpilib.h"
#include "language.h"
#include "fileio.h"
#include "pgp.h"
#include "more.h"
#include "charset.h"

#ifdef MACTC5
#include "Macutil3.h"
#include "MacPGP.h"
#define DEFAULT_LINES	23
#define DEFAULT_COLUMNS	77
#else
#if defined(MSDOS) || defined(WIN32)
#ifndef __GO32__
#include <conio.h>
#endif
#define DEFAULT_LINES	25	/* MSDOS actually has a 25-line screen */
#else
#define DEFAULT_LINES	24
#endif				/* MSDOS */
#define DEFAULT_COLUMNS	80
#endif /* MACTC5 */

static int screen_lines = DEFAULT_LINES, screen_columns = DEFAULT_COLUMNS;

#ifndef EBCDIC                  /* already defined in usuals.h */
#define TAB		0x09	/* ASCII tab char */
#define CR		'\r'	/* Carriage return char */
#define LF		'\n'	/* Linefeed */
#endif

/* Get the screen size for 'more'.  The environment variables $LINES and
   $COLUMNS will be used if they exist.  If not, then the TIOCGWINSZ call to
   ioctl() is used (if it is defined).  If not, then the TIOCGSIZE call to
   ioctl() is used (if it is defined).  If not, then the WIOCGETD call to
   ioctl() is used (if it is defined).  If not, then get the info from
   terminfo/termcap (if it is there).  Otherwise, assume we have a 24x80
   model 33.

   That was for Unix.

   For DOS, just assume 24x80. */

#ifdef UNIX
/* Try to access terminfo through the termcap-interface in the curses library
   (which requires linking with -lcurses) or use termcap directly (which
   requires linking with -ltermcap) */

#ifndef USE_TERMCAP
#ifdef USE_TERMINFO
#define USE_TERMCAP
#endif
#ifdef USE_CURSES
#define USE_TERMCAP
#endif
#endif

#ifdef USE_TERMCAP
#define TERMBUFSIZ    1024
#define UNKNOWN_TERM  "unknown"
#define DUMB_TERMBUF  "dumb:co#80:hc:"

extern int tgetent(), tgetnum();
#endif

/* Try to get TIOCGWINSZ from termios.h, then from sys/ioctl.h */
#ifndef NOTERMIO
#ifdef SVR2
#include <termio.h>
#else
#include <termios.h>
#endif				/* SVR2 */
#endif

#ifdef __FreeBSD__
#include <sys/ioctl.h>          /* for ioctl() prototype too */
#else
#ifndef SVR2
#ifndef TIOCGWINSZ
#ifndef TIOCGSIZE
#ifndef WIOCGETD
#include <sys/ioctl.h>
#endif				/* not WIOCGETD */
#endif				/* not TIOCGSIZE */
#endif				/* not TIOCGWINSZ */
#endif

/* If we still dont have TIOCGWINSZ (or TIOCGSIZE) try for WIOCGETD */
#ifndef TIOCGWINSZ
#ifndef TIOCGSIZE
#ifndef WIOCGETD
#include <sgtty.h>
#endif				/* not WIOCGETD */
#endif				/* not TIOCGSIZE */
#endif				/* not TIOCGWINSZ */
#endif				/* not SVR2 */
#endif				/* UNIX */
#ifdef __PUREC__
#include <ext.h>
#endif

static void getScreenSize(void)
{				/* Rot bilong kargo */
    /* Return the screen size */
    char *envLines, *envColumns;
    long rowTemp = 0, colTemp = 0;
#ifdef UNIX
#ifdef USE_TERMCAP
    char termBuffer[TERMBUFSIZ], *termInfo;
#endif
#ifdef TIOCGWINSZ
    struct winsize windowInfo;
#else
#ifdef TIOCGSIZE
    struct ttysize windowInfo;
#else
#ifdef WIOCGETD
    struct uwdata windowInfo;
#endif				/* WIOCGETD */
#endif				/* TIOCGSIZE */
#endif				/* TIOCGWINSZ */

    /* Make sure that we're outputting to a terminal */
    if (!isatty(fileno(stderr))) {
	screen_lines = DEFAULT_LINES;
	screen_columns = DEFAULT_COLUMNS;
	return;
    }
    screen_lines = screen_columns = 0;
#endif				/* UNIX */

    /* LINES & COLUMNS environment variables override everything else */
    envLines = getenv("LINES");
    if (envLines != NULL && (rowTemp = atol(envLines)) > 0)
	screen_lines = (int) rowTemp;

    envColumns = getenv("COLUMNS");
    if (envColumns != NULL && (colTemp = atol(envColumns)) > 0)
	screen_columns = (int) colTemp;

#ifdef UNIX
#ifdef TIOCGWINSZ
    /* See what ioctl() has to say (overrides terminfo & termcap) */
    if ((!screen_lines || !screen_columns) &&
	ioctl(fileno(stderr), TIOCGWINSZ, &windowInfo)
	!= -1) {
	if (!screen_lines && windowInfo.ws_row > 0)
	    screen_lines = (int) windowInfo.ws_row;

	if (!screen_columns && windowInfo.ws_col > 0)
	    screen_columns = (int) windowInfo.ws_col;
    }
#else
#ifdef TIOCGSIZE
    /* See what ioctl() has to say (overrides terminfo & termcap) */
    if ((!screen_lines || !screen_columns) &&
	ioctl(fileno(stderr), TIOCGSIZE, &windowInfo) != -1) {
	if (!screen_lines && windowInfo.ts_lines > 0)
	    screen_lines = (int) windowInfo.ts_lines;

	if (!screen_columns && windowInfo.ts_cols > 0)
	    screen_columns = (int) windowInfo.ts_cols;
    }
#else
#ifdef WIOCGETD
    /* See what ioctl() has to say (overrides terminfo & termcap) */
    if ((!screen_lines || !screen_columns) &&
	ioctl(fileno(stderr), WIOCGETD, &windowInfo) != -1) {
	if (!screen_lines && windowInfo.uw_height > 0)
	    screen_lines = (int) (windowInfo.uw_height / windowInfo.uw_vs);

	if (!screen_columns && windowInfo.uw_width > 0)
	    screen_columns = (int) (windowInfo.uw_width / windowInfo.uw_hs);
    }				/* You are in a twisty maze of standards,
				   all different */
#endif
#endif
#endif

#ifdef USE_TERMCAP
    /* See what terminfo/termcap has to say */
    if (!screen_lines || !screen_columns) {
	if ((termInfo = getenv("TERM")) == (char *) NULL)
	    termInfo = UNKNOWN_TERM;

	if ((tgetent(termBuffer, termInfo) <= 0))
	    strcpy(termBuffer, DUMB_TERMBUF);

	if (!screen_lines && (rowTemp = tgetnum("li")) > 0)
	    screen_lines = (int) rowTemp;

	if (!screen_columns && (colTemp = tgetnum("co")) > 0)
	    screen_columns = (int) colTemp;
    }
#endif
    if (screen_lines == 0)	/* nothing worked, use defaults */
	screen_lines = DEFAULT_LINES;
    if (screen_columns == 0)
	screen_columns = DEFAULT_COLUMNS;
#endif				/* UNIX */
}

#ifdef ATARI
#define reverse_attr()	printf("\033p")
#define norm_attr()	printf("\033q")
#else
#define reverse_attr()
#define norm_attr()
#endif


#ifdef VMS
char pager[80] = "Type/Page";	/* default pager for VMS */
#else				/* not VMS */
char pager[80] = "";
#endif				/* not VMS */

/* Blort a file to the screen with page breaks, intelligent handling of line
   terminators, truncation of overly long lines, and zapping of illegal
   chars */
int more_file(char *fileName, boolean eyes_only)
{
    FILE *inFile;
    int lines = 0, ch, i, chars = 0, c;
    long fileLen;
    char cmd[MAX_PATH];
    char buf[16];
    int lineno;
    char *p;

    if ((inFile = fopen(fileName, FOPRBIN)) == NULL)
	/* Can't see how this could fail since we just created the file */
	return -1;

    fread(buf, 1, 16, inFile);
    if (compressSignature((byte *) buf) >= 0) {
	fprintf(pgpout,
		LANG("\n\007File '%s' is not a text file; cannot display.\n"),
		fileName);
	return -1;
    }
    /* PAGER set in config.txt overrides environment variable, 
       set PAGER in config.txt to 'pgp' to use builtin pager */
    if (pager[0] == '\0') {
	if ((p = getenv("PAGER")) != NULL)
	    strncpy(pager, p, sizeof(pager) - 1);
    }
    if (strcmp(pager, "cat") == 0) {
	fclose(inFile);
	writePhantomOutput(fileName);
	return 0;
    }
    /* Use built-in pager if PAGER is not set or if the message
       is for your eyes only */
    if (!eyes_only && (strlen(pager) != 0) && strcmp("pgp", pager)) {
	fclose(inFile);
#ifdef UNIX
	if (strchr(fileName, '\'') != NULL)
	    return -1;
	sprintf(cmd, "%s '%s'", pager, fileName);
#else
	sprintf(cmd, "%s %s", pager, fileName);
#if defined(MSDOS) || defined(WIN32)
	for (p = cmd; *p; ++p)
	    if (*p == '/')
		*p = '\\';
#endif
#endif
	fflush(pgpout);
#ifdef MACTC5
	return 0;
#else
	return system(cmd);
#endif
    }
#ifdef UNIX
    if (!isatty(fileno(stdout))) {
	fclose(inFile);
	writePhantomOutput(fileName);
	return 0;
    }
#endif				/* UNIX */

    getScreenSize();

    /* Get file length */
    fseek(inFile, 0L, SEEK_END);
    fileLen = ftell(inFile);
    rewind(inFile);
    lineno = 1;

    ttycbreak();

    putchar('\n');
    for (;;) {
	ch = getc(inFile);
	if (ch == LF) {
	    lines++;
	    putchar('\n');
	    chars = 0;
	    ++lineno;
	} else if (ch == CR) {
	    lines++;
	    putchar('\n');
	    chars = 0;
	    ++lineno;

	    /* Skip following LF if there is one */
	    if ((ch = getc(inFile)) != LF && ch != EOF)
		ungetc(ch, inFile);
	} else if (((unsigned char) ch >= ' ' && ch != EOF) || ch == TAB) {
	    /* Legal char or tab, print it */
	    putchar(ch);
	    chars += (ch == TAB) ? 8 : 1;
	}
	/* If we've reach the max.no of columns we can handle, skip the
	   rest of the line */
	if (chars == screen_columns - 1) {
	    chars = 0;
	    while ((ch = getc(inFile)) != CR && ch != LF && ch != EOF);
	    if (ch != EOF)
		ungetc(ch, inFile);
	}
	/* If we've reached the max.no of rows we can handle, wait for the
	   user to hit a key */
	while (ch == EOF || lines == screen_lines - 1) {
	    /* Print prompt at end of screen */
	    reverse_attr();
	    if (ch == EOF)
		printf(LANG("\nDone...hit any key\r"));
	    else
#ifdef MACTC5	/* 203a : ftell() tells us nothing useful */
		printf(LANG("-- More -- Space: next screen, Enter: next line\
, 'B': back, 'Q': quit --\r"));
#else
		printf(
LANG("More -- %d%% -- Space: next screen, Enter: next line\
, 'B': back, 'Q': quit --\r"),
		       (100 * ftell(inFile)) / fileLen);
#endif /* MACTC5 */
	    norm_attr();
	    fflush(stdout);
	    c = getch();
	    c = toupper(c);

	    /* Blank out prompt */
	    for (i = 0; i < 79; i++)
		putchar(' ');
	    putchar('\r');
	    fflush(stdout);
	    if (c == 'B' && lineno > screen_lines) {
		/* go Back a page */
		int seek_line = lineno - 2 * screen_lines + 3;
		lineno = 1;
		rewind(inFile);
		if (seek_line > 1) {
		    printf("...skipping\n");
		    while ((ch = getc(inFile)) != EOF)
			if (ch == '\n')
			    if (++lineno == seek_line)
				break;
		}
		ch = '\0';
		lines = 0;
	    } else {
#if defined(MSDOS) || defined(__MSDOS__)
		if (c == 'Q' || c == 0x1B || ch == EOF)
#else
		if (c == 'Q' || ch == EOF)
#endif
		    goto done;
		if (c == ' ' || c == '\n' || c == '\r' || c == 'J')
		    lines -= (c == ' ') ? screen_lines - 2 : 1; /* Do n more
								   lines */
	    }
	}
    }
  done:
    ttynorm();

    fclose(inFile);
    return 0;
}				/* more_file */


/*
 * open_more() and close_more() redirect pgpout to the pager.
 *
 */

static char *mfile = NULL;
static boolean piping = FALSE;
static FILE *savepgpout;


int open_more(void)
{
#ifdef UNIX
    char *p;
#endif

    if (mfile || piping)
	close_more();

    savepgpout = pgpout;
#ifdef UNIX
    fflush(pgpout);
    if (pager[0] == '\0') {
	if ((p = getenv("PAGER")) != NULL)
	    strncpy(pager, p, sizeof(pager) - 1);
    }
    /* Use built-in pager if PAGER is not set or set to "pgp" */
    if ((strlen(pager) != 0) && strcmp("pgp", pager)) {
	if ((pgpout = popen(pager, "w")) != NULL) {
	    piping = TRUE;
	    return 0;
	}
	perror("popen");
	pgpout = savepgpout;
    }
#endif
    if ((mfile = tempfile(TMP_TMPDIR | TMP_WIPE)) == NULL)
	return -1;
    if ((pgpout = fopen(mfile, FOPWTXT)) == NULL) {
	pgpout = savepgpout;
	rmtemp(mfile);
	return -1;
    }
    /* user will not see anything until close_more() is called */
    fprintf(savepgpout, LANG("Just a moment..."));
    fflush(savepgpout);
    return 0;
}

int close_more(void)
{
    if (!mfile && !piping)
	return 0;

#ifdef UNIX
    if (piping)
	pclose(pgpout);
    else
#endif
	fclose(pgpout);
    pgpout = savepgpout;
    if (mfile) {
	fprintf(pgpout, "\n");
	more_file(mfile, FALSE);
	rmtemp(mfile);
	mfile = NULL;
    }
    piping = FALSE;
    return 0;
}


/* ==== mpiio.c ==== */
/*      mpiio.c - C source code for multiprecision integer I/O routines.
   Implemented Nov 86 by Philip Zimmermann
   Last revised 13 Sep 91 by PRZ

   Boulder Software Engineering
   3021 Eleventh Street
   Boulder, CO 80304
   (303) 541-0140

   (c) Copyright 1986-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   These routines are for multiprecision arithmetic I/O functions for
   number-theoretic cryptographic algorithms such as ElGamal,
   Diffie-Hellman, Rabin, or factoring studies for large composite
   numbers, as well as Rivest-Shamir-Adleman (RSA) public key
   cryptography.

   The external data representation for RSA messages and keys that
   some of these library routines assume is outlined in a paper by 
   Philip Zimmermann, "A Proposed Standard Format for RSA Cryptosystems",
   IEEE Computer, September 1986, Vol. 19 No. 9, pages 21-34.
   Some revisions to this data format have occurred since the paper
   was published.
 */

/* #define DEBUG */


#ifndef EMBEDDED		/* not EMBEDDED - not compiling for
				   embedded target */
#include <stdio.h>		/* for printf, etc. */
#else				/* EMBEDDED - compiling for embedded target */
#define NULL (VOID *)0
#endif

#include "mpilib.h"
#include "mpiio.h"
#include "pgp.h"
#ifdef MACTC5
extern int  Putchar(int c);
#undef putchar
#define putchar Putchar
#endif

static void puthexbyte(byte b);	/* Put out byte in ASCII hex via putchar. */
static
void puthexw16(word16 w);	/* Put out 16-bit word in hex,
				   high byte first. */
static
void putstr(string s);		/* Put out null-terminated ASCII
				   string via putchar. */

/*----------------- Following procedures relate to I/O ------------------*/

/* Returns string length, just like strlen() from <string.h> */
int string_length(char *s)
{
    int i;
    i = 0;
    if (s != NULL)
	while (*s++)
	    i++;
    return (i);
}				/* string_length */

#ifdef DEBUG
/* Returns integer 0-15 if c is an ASCII hex digit, -1 otherwise. */
static int ctox(int c)
{
    if ((c >= '0') && (c <= '9'))
	return (c - '0');
    if ((c >= 'a') && (c <= 'f'))
	return ((c - 'a') + 10);
    if ((c >= 'A') && (c <= 'F'))
	return ((c - 'A') + 10);
    return (-1);		/* error -- not a hex digit */
}				/* ctox */

/* Converts a possibly-signed digit string into a large binary number.
   Returns assumed radix, derived from suffix 'h','o',b','.' */
int str2reg(unitptr reg, string digitstr)
{
    unit temp[MAX_UNIT_PRECISION], base[MAX_UNIT_PRECISION];
    int c, i;
    boolean minus = FALSE;
    short radix;		/* base 2-16 */

    mp_init(reg, 0);

    i = string_length(digitstr);
    if (i == 0)
	return (10);		/* empty string, assume radix 10 */
    c = digitstr[i - 1];	/* get last char in string */

    switch (c) {		/* classify radix select suffix character */
    case '.':
	radix = 10;
	break;
    case 'H':
    case 'h':
	radix = 16;
	break;
    case 'O':
    case 'o':
	radix = 8;
	break;
    case 'B':			/* caution! 'b' is a hex digit! */
    case 'b':
	radix = 2;
	break;
    default:
	radix = 10;
	break;
    }

    mp_init(base, radix);
    if ((minus = (*digitstr == '-')) != 0)
	digitstr++;
    while ((c = *digitstr++) != 0) {
	if (c == ',')
	    continue;		/* allow commas in number */
	c = ctox(c);
	if ((c < 0) || (c >= radix))
	    break;		/* scan terminated by any non-digit */
	mp_mult(temp, reg, base);
	mp_move(reg, temp);
	mp_init(temp, c);
	mp_add(reg, temp);
    }
    if (minus)
	mp_neg(reg);
    return (radix);
}				/* str2reg */

#endif				/* DEBUG */

/* These I/O functions, such as putstr, puthexbyte, and puthexw16, 
   are provided here to avoid the need to link in printf from the 
   C I/O library.  This is handy in an embedded application.
   For embedded applications, use a customized putchar function, 
   separately compiled.
 */

/* Put out null-terminated ASCII string via putchar. */
static void putstr(string s)
{
    while (*s)
	putchar(*s++);
}				/* putstr */

/* Put out byte in ASCII hex via putchar. */
static void puthexbyte(byte b)
{
    static char const nibs[] = "0123456789ABCDEF";

    putchar(nibs[b >> 4]);
    putchar(nibs[b & 0x0F]);
}				/* puthexbyte */

/* Put out 16-bit word in hex, high byte first. */
static void puthexw16(word16 w)
{
    puthexbyte((byte) (w >> 8));
    puthexbyte((byte) (w & 0xFF));
}				/* puthexw16 */

#ifdef UNIT32

/* Puts out 32-bit word in hex, high byte first. */
static void puthexw32(word32 lw)
{
    puthexw16((word16) (lw >> 16));
    puthexw16((word16) (lw & 0xFFFFL));
}				/* puthexw32 */

#endif				/* UNIT32 */


#ifdef UNIT8
#define puthexunit(u) puthexbyte(u)
#endif
#ifdef UNIT16
#define puthexunit(u) puthexw16(u)
#endif
#ifdef UNIT32
#define puthexunit(u) puthexw32(u)
#endif

#ifdef DEBUG
int display_in_base(string s, unitptr n, short radix)
/*
 * Display n in any base, such as base 10.  Returns number of digits.
 * s is string to label the displayed register.
 * n is multiprecision integer.
 * radix is base, 2-16. 
 */
{
    char buf[MAX_BIT_PRECISION + (MAX_BIT_PRECISION / 8) + 2];
    unit r[MAX_UNIT_PRECISION], quotient[MAX_UNIT_PRECISION];
    word16 remainder;
    char *bp = buf;
    char minus = FALSE;
    int places = 0;
    int commaplaces;		/* put commas this many digits apart */
    int i;

    /*      If string s is just an ESC char, don't print it.
       It's just to inhibit the \n at the end of the number.
     */
#ifdef EBCDIC
    if ((s[0] != ESC) || (s[1] != '\0'))
#else
    if ((s[0] != '\033') || (s[1] != '\0'))
#endif
	putstr(s);

    if ((radix < 2) || (radix > 16)) {
	putstr("****\n");	/* radix out of range -- show error */
	return (-1);
    }
    commaplaces = (radix == 10 ? 3 : (radix == 16 ? 4 :
			       (radix == 2 ? 8 : (radix == 8 ? 8 : 1))));
    mp_move(r, n);
    if ((radix == 10) && mp_tstminus(r)) {
	minus = TRUE;
	mp_neg(r);		/* make r positive */
    }
    *bp = '\0';
    do {			/* build backwards number string */
	if (++places > 1)
	    if ((places % commaplaces) == 1)
		*++bp = ',';	/* 000,000,000,000 */
	remainder = mp_shortdiv(quotient, r, radix);
	*++bp = "0123456789ABCDEF"[remainder];	/* Isn't C wonderful? */
	mp_move(r, quotient);
    } while (testne(r, 0));
    if (minus)
	*++bp = '-';

    if (commaplaces != 1)
	while ((++places % commaplaces) != 1)
	    *++bp = ' ';	/* pad to line up commas */

    i = string_length(s);
    while (*bp) {
	putchar(*bp);
	++i;
	if ((*bp == ',') || commaplaces == 1)
	    if (i > (72 - commaplaces)) {
		putchar('\n');
		i = string_length(s);
		while (i--)
		    putchar(' ');
		i = string_length(s);
	    }
	bp--;
    }

    /* show suffix character to designate radix */
    switch (radix) {
    case 10:			/* decimal */
	putchar('.');
	break;
    case 16:			/* hex */
	putchar('h');
	break;
    case 8:			/* octal */
	putchar('o');
	break;
    case 2:			/* binary */
	putchar('b');
	break;
    default:			/* nonstandard radix */
	/* printf("(%d)",radix); */ ;
    }

    if ((s[0] == '\033') && (s[1] == '\0'))
	putchar(' ');		/* supress newline */
    else
	putchar('\n');

    fill0((byteptr) buf, sizeof(buf));	/* burn the evidence on the stack... */
    /* Note that local stack arrays r and quotient are now 0 */
    return (places);
}				/* display_in_base */

#endif				/* DEBUG */

/* Display register r in hex, with prefix string s. */
void mp_display(string s, unitptr r)
{
    short precision;
    int i, j;
    putstr(s);
    normalize(r, precision);	/* strip off leading zeros */
    if (precision == 0) {
	putstr(" 0\n");
	return;
    }
    make_msbptr(r, precision);
    i = 0;
    while (precision--) {
	if (!(i++ % (16 / BYTES_PER_UNIT))) {
	    if (i > 1) {
		putchar('\n');
		j = string_length(s);
		while (j--)
		    putchar(' ');
	    }
	}
	puthexunit(*r);
	putchar(' ');
	post_lowerunit(r);
    }
    putchar('\n');
}				/* mp_display */

/* Returns checksum of buffer. */
word16 checksum(register byteptr buf, register word16 count)
{
    word16 cs;
    cs = 0;
    while (count--)
	cs += *buf++;
    return (cs);
}				/* checksum */

/*
 * Performs the XOR necessary for RSA Cipher Block Chaining.
 * The dst buffer ought to have 1 less byte of significance than 
 * the src buffer.  Only the least significant part of the src 
 * buffer is used.  bytecount is the size of a plaintext block.
 */
void cbc_xor(register unitptr dst, register unitptr src, word16 bytecount)
{
    short nunits;		/* units of precision */
    nunits = bytes2units(bytecount) - 1;
    make_lsbptr(dst, global_precision);
    while (nunits--) {
	*dst ^= *post_higherunit(src);
	post_higherunit(dst);
	bytecount -= units2bytes(1);
    }
    /* on the last unit, don't xor the excess top byte... */
    *dst ^= (*src & (power_of_2(bytecount << 3) - 1));
}				/* cbc_xor */

/* Reverses the order of bytes in an array of bytes. */
void hiloswap(byteptr r1, short numbytes)
{
    byteptr r2;
    byte b;
    r2 = &(r1[numbytes - 1]);
    while (r1 < r2) {
	b = *r1;
	*r1++ = *r2;
	*r2-- = b;
    }
}				/* hiloswap */

#define byteglue(lo,hi) ((((word16) hi) << 8) + (word16) lo)

/****	The following functions must be changed if the external byteorder
	changes for integers in PGP packet data.
****/

/*      Fetches a 16-bit word from where byte pointer is pointing.
   buf points to external-format byteorder array.
 */
word16 fetch_word16(byte * buf)
{
    word16 w0, w1;
/* Assume MSB external byte ordering */
    w1 = *buf++;
    w0 = *buf++;
    return (w0 + (w1 << 8));
}				/* fetch_word16 */

/*
 * Puts a 16-bit word to where byte pointer is pointing, and 
 * returns updated byte pointer.
 * buf points to external-format byteorder array.
 */
byte *put_word16(word16 w, byte * buf)
{
/* Assume MSB external byte ordering */
    buf[1] = w & 0xff;
    w = w >> 8;
    buf[0] = w & 0xff;
    return (buf + 2);
}				/* put_word16 */

/*      Fetches a 32-bit word from where byte pointer is pointing.
   buf points to external-format byteorder array.
 */
word32 fetch_word32(byte * buf)
{
    word32 w0, w1, w2, w3;
/* Assume MSB external byte ordering */
    w3 = *buf++;
    w2 = *buf++;
    w1 = *buf++;
    w0 = *buf++;
    return (w0 + (w1 << 8) + (w2 << 16) + (w3 << 24));
}				/* fetch_word32 */

/*      Puts a 32-bit word to where byte pointer is pointing, and 
   returns updated byte pointer.
   buf points to external-format byteorder array.
 */
byte *put_word32(word32 w, byte * buf)
{
/* Assume MSB external byte ordering */
    buf[3] = w & 0xff;
    w = w >> 8;
    buf[2] = w & 0xff;
    w = w >> 8;
    buf[1] = w & 0xff;
    w = w >> 8;
    buf[0] = w & 0xff;
    return (buf + 4);
}				/* put_word32 */

/***	End of functions that must be changed if the external byteorder
	changes for integer fields in PGP packets.
***/

/*
 * Converts a multiprecision integer from the externally-represented 
 * form of a byte array with a 16-bit bitcount in a leading length 
 * word to the internally-used representation as a unit array.
 * Converts to INTERNAL byte order.
 * The same buffer address may be used for both r and buf.
 * Returns number of units in result, or returns -1 on error.
 */
short mpi2reg(register unitptr r, register byteptr buf)
{
    byte buf2[MAX_BYTE_PRECISION];
    word16 bitcount, bytecount, unitcount, zero_bytes, i;

    /* First, extract 16-bit bitcount prefix from first 2 bytes... */
    bitcount = fetch_word16(buf);
    buf += 2;

    /* Convert bitcount to bytecount and unitcount... */
    bytecount = bits2bytes(bitcount);
    unitcount = bytes2units(bytecount);
    if (unitcount > global_precision) {
	/* precision overflow during conversion. */
	return (-1);		/* precision overflow -- error return */
    }
    zero_bytes = units2bytes(global_precision) - bytecount;
/* Assume MSB external byte ordering */
    fill0(buf2, zero_bytes);	/* fill leading zero bytes */
    i = zero_bytes;		/* assumes MSB first */
    while (bytecount--)
	buf2[i++] = *buf++;

    mp_convert_order(buf2);	/* convert to INTERNAL byte order */
    mp_move(r, (unitptr) buf2);
    mp_burn((unitptr) buf2);	/* burn the evidence on the stack */
    return (unitcount);		/* returns unitcount of reg */
}				/* mpi2reg */

/*
 * Converts the multiprecision integer r from the internal form of 
 * a unit array to the normalized externally-represented form of a
 * byte array with a leading 16-bit bitcount word in buf[0] and buf[1].
 * This bitcount length prefix is exact count, not rounded up.
 * Converts to EXTERNAL byte order.
 * The same buffer address may be used for both r and buf.
 * Returns the number of bytes of the result, not counting length prefix.
 */
short reg2mpi(register byteptr buf, register unitptr r)
{
    byte buf1[MAX_BYTE_PRECISION];
    byteptr buf2;
    short bytecount, bc;
    word16 bitcount;
    bitcount = countbits(r);
#ifdef DEBUG
    if (bitcount > MAX_BIT_PRECISION) {
	fprintf(stderr, "reg2mpi: bitcount out of range (%d)\n", bitcount);
	return 0;
    }
#endif
    bytecount = bits2bytes(bitcount);
    bc = bytecount;		/* save bytecount for return */
    buf2 = buf1;
    mp_move((unitptr) buf2, r);
    mp_convert_order(buf2);	/* convert to EXTERNAL byteorder */
/* Assume MSB external byte ordering */
    buf2 += units2bytes(global_precision) - bytecount;
    buf = put_word16(bitcount, buf);	/* store bitcount in external
					   byteorder */

    while (bytecount--)
	*buf++ = *buf2++;

    mp_burn((unitptr) buf1);	/* burn the evidence on the stack */
    return (bc);		/* returns bytecount of mpi, not counting
				   prefix */
}				/* reg2mpi */


#ifdef DEBUG

/* Dump buffer in hex, with string label prefix. */
void dumpbuf(string s, byteptr buf, int bytecount)
{
    putstr(s);
    while (bytecount--) {
	puthexbyte(*buf++);
	putchar(' ');
	if ((bytecount & 0x0f) == 0)
	    putchar('\n');
    }
}				/* dumpbuf */

/*
 * Dump unit array r as a C array initializer, with string label prefix. 
 * Array is dumped in native unit order.
 */
void dump_unit_array(string s, unitptr r)
{
    int unitcount;
    unitcount = global_precision;
    putstr(s);
    putstr("\n{ ");
    while (unitcount--) {
	putstr("0x");
	puthexunit(*r++);
	putchar(',');
	if (unitcount && ((unitcount & 0x07) == 0))
	    putstr("\n  ");
    }
    putstr(" 0};\n");
}				/* dump_unit_array */

#endif				/* ifdef DEBUG */

/************ end of multiprecision integer I/O library *****************/


/* ==== mpilib.c ==== */
/* C source code for multiprecision arithmetic library routines.
   Implemented Nov 86 by Philip Zimmermann
   Last revised 27 Nov 91 by PRZ

   Boulder Software Engineering
   3021 Eleventh Street
   Boulder, CO 80304
   (303) 541-0140

   (c) Copyright 1986-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.  The use of this
   cryptographic software for developing weapon systems is expressly
   forbidden.


   These routines implement all of the multiprecision arithmetic
   necessary for number-theoretic cryptographic algorithms such as
   ElGamal, Diffie-Hellman, Rabin, or factoring studies for large
   composite numbers, as well as Rivest-Shamir-Adleman (RSA) public
   key cryptography.

   Although originally developed in Microsoft C for the IBM PC, this code
   contains few machine dependencies.  It assumes 2's complement
   arithmetic.  It can be adapted to 8-bit, 16-bit, or 32-bit machines,
   lowbyte-highbyte order or highbyte-lowbyte order.  This version
   has been converted to ANSI C.


   The internal representation for these extended precision integer
   "registers" is an array of "units".  A unit is a machine word, which
   is either an 8-bit byte, a 16-bit unsigned integer, or a 32-bit
   unsigned integer, depending on the machine's word size.  For example,
   an IBM PC or AT uses a unit size of 16 bits.  To perform arithmetic
   on these huge precision integers, we pass pointers to these unit
   arrays to various subroutines.  A pointer to an array of units is of
   type unitptr.  This is a pointer to a huge integer "register".

   When calling a subroutine, we always pass a pointer to the BEGINNING
   of the array of units, regardless of the byte order of the machine.
   On a lowbyte-first machine, such as the Intel 80x86, this unitptr
   points to the LEAST significant unit, and the array of units increases
   significance to the right.  On a highbyte-first machine, such as the
   Motorola 680x0, this unitptr points to the MOST significant unit, and
   the array of units decreases significance to the right.

   Modified 8 Apr 92 - HAJK
   Implement new VAX/VMS primitive support.

   Modified 30 Sep 92 -Castor Fu <castor@drizzle.stanford.edu>
   Upgraded PORTABLE support to allow sizeof(unit) == sizeof(long)

   Modified 28 Nov 92 - Thad Smith
   Added Smith modmult, generalized non-portable support.
 */

/* #define COUNTMULTS *//* count modmults for performance studies */

#ifdef DEBUG
#if defined(MSDOS) || defined(WIN32)
#ifdef __GO32__			/* DJGPP */
#include <pc.h>
#else
#include <conio.h>
#endif				/* __GO32__ */
#define poll_for_break() {while (kbhit()) getch();}
#endif				/* MSDOS || WIN32 */
#endif				/* DEBUG */

#ifndef poll_for_break
#define poll_for_break()	/* stub */
#endif

#include "mpilib.h"

#ifdef MACTC5
#include <stdio.h>
#include "Macutil3.h"
void upton_burn(void);
char *copyright_notice(void);
void merritt_burn(void);
#ifdef CODEWARRIOR
#define mp_modexp xxxx_mp_modexp
int mp_modexp(register unitptr expout, register unitptr expin,
	      register unitptr exponent, register unitptr modulus);
#endif
#endif

#ifdef mp_smula
#ifdef mp_smul
Error:Both mp_smula and mp_smul cannot be defined.
#else
#define mp_smul	mp_smula
#endif
#endif

/* set macros for MULTUNIT */
#ifdef MUNIT8
#define MULTUNITSIZE   8
typedef unsigned char MULTUNIT;
#ifdef UNIT8
#define MULTUNIT_SIZE_SAME
#endif
#else				/* not MUNIT8 */
#ifdef MUNIT32
#define MULTUNITSIZE   32
typedef unsigned long MULTUNIT;
#ifdef UNIT32
#define MULTUNIT_SIZE_SAME
#else
/* #error is not portable, this has the same effect */
#include "UNITSIZE cannot be smaller than MULTUNITSIZE"
#endif
#else				/* assume MUNIT16 */
#define MULTUNITSIZE   16
typedef unsigned short MULTUNIT;
#ifdef UNIT16
#define MULTUNIT_SIZE_SAME
#endif				/* UNIT16 */
#ifdef UNIT8
#include "UNITSIZE cannot be smaller than MULTUNITSIZE"
#endif				/* UNIT8 */
#endif				/* MUNIT32 */
#endif				/* MUNIT8 */

#define MULTUNIT_msb    ((MULTUNIT)1 << (MULTUNITSIZE-1))	/* msb of
								   MULTUNIT */
#define DMULTUNIT_msb   (1L << (2*MULTUNITSIZE-1))
#define MULTUNIT_mask   ((MULTUNIT)((1L << MULTUNITSIZE)-1))
#define MULTUNITs_perunit   (UNITSIZE/MULTUNITSIZE)


void mp_smul(MULTUNIT * prod, MULTUNIT * multiplicand, MULTUNIT multiplier);
void mp_dmul(unitptr prod, unitptr multiplicand, unitptr multiplier);

#if defined(WIN32)
/* For Win32 we want this to be 32-bit, for compatibility with the assembler code */
unsigned int global_precision = 0;     /* units of precision for all routines */
#else
short global_precision = 0;	/* units of precision for all routines */
#endif
/*      global_precision is the unit precision last set by set_precision.
   Initially, set_precision() should be called to define global_precision
   before using any of these other multiprecision library routines.
   i.e.:   set_precision(MAX_UNIT_PRECISION);
 */

/*************** multiprecision library primitives ****************/
/*      The following portable C primitives should be recoded in assembly.
   The entry point name should be defined, in "mpilib.h" to the external
   entry point name.  If undefined, the C version will be used.
 */

typedef unsigned long int ulint;

#ifndef mp_addc
/* 
   multiprecision add with carry r2 to r1, result in r1
   carry is incoming carry flag-- value should be 0 or 1
*/
boolean mp_addc
 (register unitptr r1, register unitptr r2, register boolean carry)
{
    register unit x;
    short precision;		/* number of units to add */
    precision = global_precision;
    make_lsbptr(r1, precision);
    make_lsbptr(r2, precision);
    while (precision--) {
	if (carry) {
	    x = *r1 + *r2 + 1;
	    carry = (*r2 >= (unit) (~*r1));
	} else {
	    x = *r1 + *r2;
	    carry = (x < *r1);
	}
	post_higherunit(r2);
	*post_higherunit(r1) = x;
    }
    return carry;		/* return the final carry flag bit */
}				/* mp_addc */
#endif				/* mp_addc */

#ifndef mp_subb

/* 
   multiprecision subtract with borrow, r2 from r1, result in r1
   borrow is incoming borrow flag-- value should be 0 or 1
 */
boolean mp_subb
 (register unitptr r1, register unitptr r2, register boolean borrow)
{
    register unit x;
    short precision;		/* number of units to subtract */
    precision = global_precision;
    make_lsbptr(r1, precision);
    make_lsbptr(r2, precision);
    while (precision--) {
	if (borrow) {
	    x = *r1 - *r2 - 1;
	    borrow = (*r1 <= *r2);
	} else {
	    x = *r1 - *r2;
	    borrow = (*r1 < *r2);
	}
	post_higherunit(r2);
	*post_higherunit(r1) = x;
    }
    return borrow;		/* return the final carry/borrow flag bit */
}				/* mp_subb */
#endif				/* mp_subb */

#ifndef mp_rotate_left

/*
   multiprecision rotate left 1 bit with carry, result in r1.
   carry is incoming carry flag-- value should be 0 or 1
*/
boolean mp_rotate_left(register unitptr r1, register boolean carry)
{
    register int precision;	/* number of units to rotate */
    unsigned int mcarry = carry, nextcarry;	/* int is supposed to be
						 * the efficient size for ops*/
    precision = global_precision;
    make_lsbptr(r1, precision);
    while (precision--) {
	nextcarry = (((signedunit) * r1) < 0);
	*r1 = (*r1 << 1) | mcarry;
	mcarry = nextcarry;
	pre_higherunit(r1);
    }
    return nextcarry;		/* return the final carry flag bit */
}				/* mp_rotate_left */
#endif				/* mp_rotate_left */

/************** end of primitives that should be in assembly *************/

/* The mp_shift_right_bits function is not called in any time-critical
   situations in public-key cryptographic functions, so it doesn't
   need to be coded in assembly language.
 */

/*
   multiprecision shift right bits, result in r1.
   bits is how many bits to shift, must be <= UNITSIZE.
*/
void mp_shift_right_bits(register unitptr r1, register short bits)
{
    register short precision;	/* number of units to shift */
    register unit carry, nextcarry, bitmask;
    register short unbits;
    if (bits == 0)
	return;			/* shift zero bits is a no-op */
    carry = 0;
    bitmask = power_of_2(bits) - 1;
    unbits = UNITSIZE - bits;	/* shift bits must be <= UNITSIZE */
    precision = global_precision;
    make_msbptr(r1, precision);
    if (bits == UNITSIZE) {
	while (precision--) {
	    nextcarry = *r1;
	    *r1 = carry;
	    carry = nextcarry;
	    pre_lowerunit(r1);
	}
    } else {
	while (precision--) {
	    nextcarry = *r1 & bitmask;
	    *r1 >>= bits;
	    *r1 |= carry << unbits;
	    carry = nextcarry;
	    pre_lowerunit(r1);
	}
    }
}				/* mp_shift_right_bits */

#ifndef mp_compare

/*
 * Compares multiprecision integers *r1, *r2, and returns:
 * -1 iff *r1 < *r2
 *  0 iff *r1 == *r2
 * +1 iff *r1 > *r2
 */
short mp_compare(register unitptr r1, register unitptr r2)
{
    register short precision;	/* number of units to compare */

    precision = global_precision;
    make_msbptr(r1, precision);
    make_msbptr(r2, precision);
    do {
	if (*r1 < *r2)
	    return -1;
	if (*post_lowerunit(r1) > *post_lowerunit(r2))
	    return 1;
    } while (--precision);
    return 0;			/*  *r1 == *r2  */
}				/* mp_compare */
#endif				/* mp_compare */

/* Increment multiprecision integer r. */
boolean mp_inc(register unitptr r)
{
    register short precision;
    precision = global_precision;
    make_lsbptr(r, precision);
    do {
	if (++(*r))
	    return 0;		/* no carry */
	post_higherunit(r);
    } while (--precision);
    return 1;			/* return carry set */
}				/* mp_inc */

/* Decrement multiprecision integer r. */
boolean mp_dec(register unitptr r)
{
    register short precision;
    precision = global_precision;
    make_lsbptr(r, precision);
    do {
	if ((signedunit) (--(*r)) != (signedunit) - 1)
	    return 0;		/* no borrow */
	post_higherunit(r);
    } while (--precision);
    return 1;			/* return borrow set */
}				/* mp_dec */

/* Compute 2's complement, the arithmetic negative, of r */
void mp_neg(register unitptr r)
{
    register short precision;	/* number of units to negate */
    precision = global_precision;
    mp_dec(r);			/* 2's complement is 1's complement plus 1 */
    do {			/* now do 1's complement */
	*r = ~(*r);
	r++;
    } while (--precision);
}				/* mp_neg */

#ifndef mp_move

void mp_move(register unitptr dst, register unitptr src)
{
    register short precision;	/* number of units to move */
    precision = global_precision;
    do {
	*dst++ = *src++;
    } while (--precision);
}				/* mp_move */
#endif				/* mp_move */

/* Init multiprecision register r with short value. */
void mp_init(register unitptr r, word16 value)
{	/* Note that mp_init doesn't extend sign bit for >32767 */

    unitfill0(r, global_precision);
    make_lsbptr(r, global_precision);
    *post_higherunit(r) = value;
#ifdef UNIT8
    *post_higherunit(r) = value >> UNITSIZE;
#endif
}				/* mp_init */

/* Returns number of significant units in r */
short significance(register unitptr r)
{
    register short precision;
    precision = global_precision;
    make_msbptr(r, precision);
    do {
	if (*post_lowerunit(r))
	    return precision;
    } while (--precision);
    return precision;
}				/* significance */


#ifndef unitfill0

/* Zero-fill the unit buffer r. */
void unitfill0(unitptr r, word16 unitcount)
{
    while (unitcount--)
	*r++ = 0;
}				/* unitfill0 */
#endif				/* unitfill0 */

/* Unsigned divide, treats both operands as positive. */
int mp_udiv(register unitptr remainder, register unitptr quotient,
	    register unitptr dividend, register unitptr divisor)
{
    int bits;
    short dprec;
    register unit bitmask;

    if (testeq(divisor, 0))
	return -1;		/* zero divisor means divide error */
    mp_init0(remainder);
    mp_init0(quotient);
    /* normalize and compute number of bits in dividend first */
    init_bitsniffer(dividend, bitmask, dprec, bits);
    /* rescale quotient to same precision (dprec) as dividend */
    rescale(quotient, global_precision, dprec);
    make_msbptr(quotient, dprec);

    while (bits--) {
	mp_rotate_left(remainder,
		       (boolean) (sniff_bit(dividend, bitmask) != 0));
	if (mp_compare(remainder, divisor) >= 0) {
	    mp_sub(remainder, divisor);
	    stuff_bit(quotient, bitmask);
	}
	bump_2bitsniffers(dividend, quotient, bitmask);
    }
    return 0;
}				/* mp_udiv */

#ifdef UPTON_OR_SMITH

#define RECIPMARGIN 0		/* extra margin bits used by mp_recip() */

/* Compute reciprocal (quotient) as 1/divisor.  Used by faster modmult. */
int mp_recip(register unitptr quotient, register unitptr divisor)
{
    int bits;
    short qprec;
    register unit bitmask;
    unit remainder[MAX_UNIT_PRECISION];
    if (testeq(divisor, 0))
	return -1;		/* zero divisor means divide error */
    mp_init0(remainder);
    mp_init0(quotient);

    /* normalize and compute number of bits in quotient first */
    bits = countbits(divisor) + RECIPMARGIN;
    bitmask = bitmsk(bits);	/* bitmask within a single unit */
    qprec = bits2units(bits + 1);
    mp_setbit(remainder, (bits - RECIPMARGIN) - 1);
    /* rescale quotient to precision of divisor + RECIPMARGIN bits */
    rescale(quotient, global_precision, qprec);
    make_msbptr(quotient, qprec);

    while (bits--) {
	mp_shift_left(remainder);
	if (mp_compare(remainder, divisor) >= 0) {
	    mp_sub(remainder, divisor);
	    stuff_bit(quotient, bitmask);
	}
	bump_bitsniffer(quotient, bitmask);
    }
    mp_init0(remainder);	/* burn sensitive data left on stack */
    return 0;
}				/* mp_recip */
#endif				/* UPTON_OR_SMITH */

/* Signed divide, either or both operands may be negative. */
int mp_div(register unitptr remainder, register unitptr quotient,
	   register unitptr dividend, register unitptr divisor)
{
    boolean dvdsign, dsign;
    int status;
    dvdsign = (boolean) (mp_tstminus(dividend) != 0);
    dsign = (boolean) (mp_tstminus(divisor) != 0);
    if (dvdsign)
	mp_neg(dividend);
    if (dsign)
	mp_neg(divisor);
    status = mp_udiv(remainder, quotient, dividend, divisor);
    if (dvdsign)
	mp_neg(dividend);	/* repair caller's dividend */
    if (dsign)
	mp_neg(divisor);	/* repair caller's divisor */
    if (status < 0)
	return status;		/* divide error? */
    if (dvdsign)
	mp_neg(remainder);
    if (dvdsign ^ dsign)
	mp_neg(quotient);
    return status;
}				/* mp_div */

/*
 * This function does a fast divide and mod on a multiprecision dividend
 * using a short integer divisor returning a short integer remainder.
 * This is an unsigned divide.  It treats both operands as positive.
 * It is used mainly for faster printing of large numbers in base 10.
 */
word16 mp_shortdiv(register unitptr quotient,
		   register unitptr dividend, register word16 divisor)
{
    int bits;
    short dprec;
    register unit bitmask;
    register word16 remainder;
    if (!divisor)		/* if divisor == 0 */
	return -1;		/* zero divisor means divide error */
    remainder = 0;
    mp_init0(quotient);
    /* normalize and compute number of bits in dividend first */
    init_bitsniffer(dividend, bitmask, dprec, bits);
    /* rescale quotient to same precision (dprec) as dividend */
    rescale(quotient, global_precision, dprec);
    make_msbptr(quotient, dprec);

    while (bits--) {
	remainder <<= 1;
	if (sniff_bit(dividend, bitmask))
	    remainder++;
	if (remainder >= divisor) {
	    remainder -= divisor;
	    stuff_bit(quotient, bitmask);
	}
	bump_2bitsniffers(dividend, quotient, bitmask);
    }
    return remainder;
}				/* mp_shortdiv */

/* Unsigned divide, treats both operands as positive. */
int mp_mod(register unitptr remainder,
	   register unitptr dividend, register unitptr divisor)
{
    int bits;
    short dprec;
    register unit bitmask;
    if (testeq(divisor, 0))
	return -1;		/* zero divisor means divide error */
    mp_init0(remainder);
    /* normalize and compute number of bits in dividend first */
    init_bitsniffer(dividend, bitmask, dprec, bits);

    while (bits--) {
	mp_rotate_left(remainder,
		       (boolean) (sniff_bit(dividend, bitmask) != 0));
	msub(remainder, divisor);
	bump_bitsniffer(dividend, bitmask);
    }
    return 0;
}				/* mp_mod */

/*
 * This function does a fast mod operation on a multiprecision dividend
 * using a short integer modulus returning a short integer remainder.
 * This is an unsigned divide.  It treats both operands as positive.
 * It is used mainly for fast sieve searches for large primes.
 */
word16 mp_shortmod(register unitptr dividend, register word16 divisor)
{
    int bits;
    short dprec;
    register unit bitmask;
    register word16 remainder;
    if (!divisor)		/* if divisor == 0 */
	return -1;		/* zero divisor means divide error */
    remainder = 0;
    /* normalize and compute number of bits in dividend first */
    init_bitsniffer(dividend, bitmask, dprec, bits);

    while (bits--) {
	remainder <<= 1;
	if (sniff_bit(dividend, bitmask))
	    remainder++;
	if (remainder >= divisor)
	    remainder -= divisor;
	bump_bitsniffer(dividend, bitmask);
    }
    return remainder;
}				/* mp_shortmod */



#ifdef COMB_MULT		/* use faster "comb" multiply algorithm */
/* We are skipping this code because it has a bug... */

/*
 * Computes multiprecision prod = multiplicand * multiplier
 *
 * Uses interleaved comb multiply algorithm.
 * This improved multiply more than twice as fast as a Russian
 * peasant multiply, because it does a lot fewer shifts.
 * Must have global_precision set to the size of the multiplicand
 * plus UNITSIZE-1 SLOP_BITS.  Produces a product that is the sum
 * of the lengths of the multiplier and multiplicand.
 *
 * BUG ALERT:  Unfortunately, this code has a bug.  It fails for
 * some numbers.  One such example:
 * x=   59DE 60CE 2345 8091 A02B 2A1C DBC3 8BE5
 * x*x= 59DE 60CE 2345 26B3 993B 67A5 2499 0B7D
 *      52C8 CDC7 AFB3 61C8 243C 741B
 * --which is obviously wrong.  The answer should be:
 * x*x= 1F8C 607B 5EA6 C061 2714 04A9 A0C6 A17A
 *      C9AB 6095 C62F 3756 3843 E4D0 3950 7AD9
 * We'll have to fix this some day.  In the meantime, we'll
 * just have the compiler skip over this code.
 *
 * BUG NOTE: Possibly fixed.  Needs testing.
 */
int mp_mult(register unitptr prod,
	    register unitptr multiplicand, register unitptr multiplier)
{
    int bits;
    register unit bitmask;
    unitptr product, mplier, temp;
    short mprec, mprec2;
    unit mplicand[MAX_UNIT_PRECISION];

    /* better clear full width--double precision */
    mp_init(prod + tohigher(global_precision), 0);

    if (testeq(multiplicand, 0))
	return 0;		/* zero multiplicand means zero product */

    mp_move(mplicand, multiplicand);	/* save it from damage */

    normalize(multiplier, mprec);
    if (!mprec)
	return 0;

    make_lsbptr(multiplier, mprec);
    bitmask = 1;		/* start scan at LSB of multiplier */

    do {			/* UNITSIZE times */
	/* do for bits 0-15 */
	product = prod;
	mplier = multiplier;
	mprec2 = mprec;
	while (mprec2--) {	/* do for each word in multiplier */

	    if (sniff_bit(mplier, bitmask)) {
		if (mp_addc(product, multiplicand, 0)) {  /* ripple carry */
		  /* After 1st time thru, this is rarely encountered. */
		    temp = msbptr(product, global_precision);
		    pre_higherunit(temp);
		    /* temp now points to LSU of carry region. */
		    unmake_lsbptr(temp, global_precision);
		    mp_inc(temp);
		}		/* ripple carry */
	    }
	    pre_higherunit(mplier);
	    pre_higherunit(product);
	}
	if (!(bitmask <<= 1))
	    break;
	mp_shift_left(multiplicand);

    } while (TRUE);

    mp_move(multiplicand, mplicand);	/* recover */

    return 0;			/* normal return */
}				/* mp_mult */

#endif				/* COMB_MULT */


/* Because the "comb" multiply has a bug, we will use the slower
   Russian peasant multiply instead.  Fortunately, the mp_mult
   function is not called from any time-critical code.
 */

/*
 * Computes multiprecision prod = multiplicand * multiplier
 * Uses "Russian peasant" multiply algorithm.
 */
int mp_mult(register unitptr prod,
	    register unitptr multiplicand, register unitptr multiplier)
{
    int bits;
    register unit bitmask;
    short mprec;
    mp_init(prod, 0);
    if (testeq(multiplicand, 0))
	return 0;		/* zero multiplicand means zero product */
    /* normalize and compute number of bits in multiplier first */
    init_bitsniffer(multiplier, bitmask, mprec, bits);

    while (bits--) {
	mp_shift_left(prod);
	if (sniff_bit(multiplier, bitmask))
	    mp_add(prod, multiplicand);
	bump_bitsniffer(multiplier, bitmask);
    }
    return 0;
}				/* mp_mult */

/*
 * mp_modmult computes a multiprecision multiply combined with a
 * modulo operation.  This is the most time-critical function in
 * this multiprecision arithmetic library for performing modulo
 * exponentiation.  We experimented with different versions of modmult,
 * depending on the machine architecture and performance requirements.
 * We will either use a Russian Peasant modmult (peasant_modmult), 
 * Charlie Merritt's modmult (merritt_modmult), Jimmy Upton's
 * modmult (upton_modmult), or Thad Smith's modmult (smith_modmult).
 * On machines with a hardware atomic multiply instruction,
 * Smith's modmult is fastest.  It can utilize assembly subroutines to
 * speed up the hardware multiply logic and trial quotient calculation.
 * If the machine lacks a fast hardware multiply, Merritt's modmult
 * is preferred, which doesn't call any assembly multiply routine.
 * We use the alias names mp_modmult, stage_modulus, and modmult_burn
 * for the corresponding true names, which depend on what flavor of
 * modmult we are using.
 * 
 * Before making the first call to mp_modmult, you must set up the
 * modulus-dependant precomputated tables by calling stage_modulus.
 * After making all the calls to mp_modmult, you call modmult_burn to
 * erase the tables created by stage_modulus that were left in memory.
 */

#ifdef COUNTMULTS
/* "number of modmults" counters, used for performance studies. */
static unsigned int tally_modmults = 0;
static unsigned int tally_modsquares = 0;
#endif				/* COUNTMULTS */


#ifdef PEASANT
/* Conventional Russian peasant multiply with modulo algorithm. */

static unitptr pmodulus = 0;	/* used only by mp_modmult */

/*
 * Must pass modulus to stage_modulus before calling modmult.
 * Assumes that global_precision has already been adjusted to the
 * size of the modulus, plus SLOP_BITS.
 */
int stage_peasant_modulus(unitptr n)
{	/* For this simple version of modmult, just copy unit pointer. */
    pmodulus = n;
    return 0;			/* normal return */
}				/* stage_peasant_modulus */

/* "Russian peasant" multiply algorithm, combined with a modulo
 * operation.  This is a simple naive replacement for Merritt's
 * faster modmult algorithm.  References global unitptr "modulus".
 * Computes:  prod = (multiplicand*multiplier) mod modulus
 * WARNING: All the arguments must be less than the modulus!
 */
int peasant_modmult(register unitptr prod,
		    unitptr multiplicand, register unitptr multiplier)
{
    int bits;
    register unit bitmask;
    short mprec;
    mp_init(prod, 0);
/*      if (testeq(multiplicand,0))
       return 0; *//* zero multiplicand means zero product */
    /* normalize and compute number of bits in multiplier first */
    init_bitsniffer(multiplier, bitmask, mprec, bits);

    while (bits--) {
	mp_shift_left(prod);
	msub(prod, pmodulus);	/* turns mult into modmult */
	if (sniff_bit(multiplier, bitmask)) {
	    mp_add(prod, multiplicand);
	    msub(prod, pmodulus);	/* turns mult into modmult */
	}
	bump_bitsniffer(multiplier, bitmask);
    }
    return 0;
}				/* peasant_modmult */

/*
 * If we are using a version of mp_modmult that uses internal tables
 * in memory, we have to call modmult_burn() at the end of mp_modexp.
 * This is so that no sensitive data is left in memory after the program
 * exits.  The Russian peasant method doesn't use any such tables.
 */

/*
 * Alias for modmult_burn, called only from mp_modexp().  Destroys
 * internal modmult tables.  This version does nothing because no
 * tables are used by the Russian peasant modmult.
 */
void peasant_burn(void)
{
}				/* peasant_burn */

#endif				/* PEASANT */


#ifdef MERRITT
/*=========================================================================*/
/*
   This is Charlie Merritt's MODMULT algorithm, implemented in C by PRZ.
   Also refined by Zhahai Stewart to reduce the number of subtracts.
   Modified by Raymond Brand to reduce the number of SLOP_BITS by 1.
   It performs a multiply combined with a modulo operation, without
   going into "double precision".  It is faster than the Russian peasant
   method, and still works well on machines that lack a fast hardware
   multiply instruction.
 */

/* The following support functions, macros, and data structures
   are used only by Merritt's modmult algorithm... */

/*      Shift r1 1 whole unit to the left.  Used only by modmult function. */
static void mp_lshift_unit(register unitptr r1)
{
    register short precision;
    register unitptr r2;
    precision = global_precision;
    make_msbptr(r1, precision);
    r2 = r1;
    while (--precision)
	*post_lowerunit(r1) = *pre_lowerunit(r2);
    *r1 = 0;
}				/* mp_lshift_unit */

/* moduli_buf contains shifted images of the modulus, set by stage_modulus */
static unit moduli_buf[UNITSIZE - 1][MAX_UNIT_PRECISION] =
{0};
static unitptr moduli[UNITSIZE] =	/* contains pointers into moduli_buf */
{0, moduli_buf[0], moduli_buf[1], moduli_buf[2],
 moduli_buf[3], moduli_buf[4], moduli_buf[5], moduli_buf[6]
#ifndef UNIT8
 ,moduli_buf[7], moduli_buf[8], moduli_buf[9], moduli_buf[10],
 moduli_buf[11], moduli_buf[12], moduli_buf[13], moduli_buf[14]
#ifndef UNIT16			/* and not UNIT8 */
 ,moduli_buf[15], moduli_buf[16], moduli_buf[17], moduli_buf[18],
 moduli_buf[19], moduli_buf[20], moduli_buf[21], moduli_buf[22],
 moduli_buf[23], moduli_buf[24], moduli_buf[25], moduli_buf[26],
 moduli_buf[27], moduli_buf[28], moduli_buf[29], moduli_buf[30]
#endif				/* UNIT16 and UNIT8 not defined */
#endif				/* UNIT8 not defined */
};

/*
 * To optimize msubs, need following 2 unit arrays, each filled
 * with the most significant unit and next-to-most significant unit
 * of the preshifted images of the modulus.
 */
static unit msu_moduli[UNITSIZE] =
{0};				/* most signif. unit */
static unit nmsu_moduli[UNITSIZE] =
{0};				/* next-most signif. unit */

/*
 * mpdbuf contains preshifted images of the multiplicand, mod n.
 * It is used only by mp_modmult.  It could be staticly declared
 * inside of mp_modmult, but we put it outside mp_modmult so that
 * it can be wiped clean by modmult_burn(), which is called at the
 * end of mp_modexp.  This is so that no sensitive data is left in
 * memory after the program exits.
 */
static unit mpdbuf[UNITSIZE - 1][MAX_UNIT_PRECISION] =
{0};

/*
 * Computes UNITSIZE images of r, each shifted left 1 more bit.
 * Used only by modmult function.
 */
static void stage_mp_images(unitptr images[UNITSIZE], unitptr r)
{
    short int i;

    images[0] = r;	/* no need to move the first image, just copy ptr */
    for (i = 1; i < UNITSIZE; i++) {
	mp_move(images[i], images[i - 1]);
	mp_shift_left(images[i]);
	msub(images[i], moduli[0]);
    }
}				/* stage_mp_images */

/*
 * Computes UNITSIZE images of modulus, each shifted left 1 more bit.
 * Before calling mp_modmult, you must first stage the moduli images by
 * calling stage_modulus.  n is the pointer to the modulus.
 * Assumes that global_precision has already been adjusted to the
 * size of the modulus, plus SLOP_BITS.
 */
int stage_merritt_modulus(unitptr n)
{
    short int i;
    unitptr msu;	/* ptr to most significant unit, for faster msubs */

    moduli[0] = n;	/* no need to move the first image, just copy ptr */

    /* used by optimized msubs macro... */
    msu = msbptr(n, global_precision);	/* needed by msubs */
    msu_moduli[0] = *post_lowerunit(msu);
    nmsu_moduli[0] = *msu;

    for (i = 1; i < UNITSIZE; i++) {
	mp_move(moduli[i], moduli[i - 1]);
	mp_shift_left(moduli[i]);

	/* used by optimized msubs macro... */
	msu = msbptr(moduli[i], global_precision);	/* needed by msubs */
	msu_moduli[i] = *post_lowerunit(msu);	/* for faster msubs */
	nmsu_moduli[i] = *msu;
    }
    return 0;			/* normal return */
}				/* stage_merritt_modulus */

/* The following macros, sniffadd and msubs, are used by modmult... */
#define sniffadd(i) if (*multiplier & power_of_2(i))  mp_add(prod,mpd[i])

/* Unoptimized msubs macro (msubs0) follows... */
/* #define msubs0(i) msub(prod,moduli[i])
 */

/*
 * To optimize msubs, compare the most significant units of the
 * product and the shifted modulus before deciding to call the full
 * mp_compare routine.  Better still, compare the upper two units
 * before deciding to call mp_compare.
 * Optimization of msubs relies heavily on standard C left-to-right
 * parsimonious evaluation of logical expressions.
 */

/* Partially-optimized msubs macro (msubs1) follows... */
/* #define msubs1(i) if ( \
   ((p_m = (*msu_prod-msu_moduli[i])) >= 0) && \
   (p_m || (mp_compare(prod,moduli[i]) >= 0) ) \
   ) mp_sub(prod,moduli[i])
 */

/* Fully-optimized msubs macro (msubs2) follows... */
#define msubs(i) if (((p_m = *msu_prod-msu_moduli[i])>0) || ( \
 (p_m==0) && ( (*nmsu_prod>nmsu_moduli[i]) || ( \
 (*nmsu_prod==nmsu_moduli[i]) && ((mp_compare(prod,moduli[i]) >= 0)) ))) ) \
 mp_sub(prod,moduli[i])

/*
 * Performs combined multiply/modulo operation.
 * Computes:  prod = (multiplicand*multiplier) mod modulus
 * WARNING: All the arguments must be less than the modulus!
 * Assumes the modulus has been predefined by first calling
 * stage_modulus.
 */
int merritt_modmult(register unitptr prod,
		    unitptr multiplicand, register unitptr multiplier)
{
    /* p_m, msu_prod, and nmsu_prod are used by the optimized msubs macro... */
    register signedunit p_m;
    register unitptr msu_prod;	/* ptr to most significant unit of product */
    register unitptr nmsu_prod;	/* next-most signif. unit of product */
    short mprec;		/* precision of multiplier, in units */
    /*      Array mpd contains a list of pointers to preshifted images of
       the multiplicand: */
    static unitptr mpd[UNITSIZE] =
    {
	0, mpdbuf[0], mpdbuf[1], mpdbuf[2],
	mpdbuf[3], mpdbuf[4], mpdbuf[5], mpdbuf[6]
#ifndef UNIT8
	,mpdbuf[7], mpdbuf[8], mpdbuf[9], mpdbuf[10],
	mpdbuf[11], mpdbuf[12], mpdbuf[13], mpdbuf[14]
#ifndef UNIT16			/* and not UNIT8 */
	,mpdbuf[15], mpdbuf[16], mpdbuf[17], mpdbuf[18],
	mpdbuf[19], mpdbuf[20], mpdbuf[21], mpdbuf[22],
	mpdbuf[23], mpdbuf[24], mpdbuf[25], mpdbuf[26],
	mpdbuf[27], mpdbuf[28], mpdbuf[29], mpdbuf[30]
#endif				/* UNIT16 and UNIT8 not defined */
#endif				/* UNIT8 not defined */
    };

    /* Compute preshifted images of multiplicand, mod n: */
    stage_mp_images(mpd, multiplicand);

    /* To optimize msubs, set up msu_prod and nmsu_prod: */
    msu_prod = msbptr(prod, global_precision);	/* Get ptr to MSU of prod */
    nmsu_prod = msu_prod;
    post_lowerunit(nmsu_prod);	/* Get next-MSU of prod */

    /*
     * To understand this algorithm, it would be helpful to first
     * study the conventional Russian peasant modmult algorithm.
     * This one does about the same thing as Russian peasant, but
     * is organized differently to save some steps.  It loops
     * through the multiplier a word (unit) at a time, instead of
     * a bit at a time.  It word-shifts the product instead of
     * bit-shifting it, so it should be faster.  It also does about
     * half as many subtracts as Russian peasant.
     */

    mp_init(prod, 0);		/* Initialize product to 0. */

    /*
     * The way mp_modmult is actually used in cryptographic
     * applications, there will NEVER be a zero multiplier or
     * multiplicand.  So there is no need to optimize for that
     * condition.
     */
/*      if (testeq(multiplicand,0))
       return 0; *//* zero multiplicand means zero product */
    /* Normalize and compute number of units in multiplier first: */
    normalize(multiplier, mprec);
    if (mprec == 0)		/* if precision of multiplier is 0 */
	return 0;		/* zero multiplier means zero product */
    make_msbptr(multiplier, mprec);	/* start at MSU of multiplier */

    while (mprec--) { /* Loop for the number of units in the multiplier */
	/*
	 * Shift the product one whole unit to the left.
	 * This is part of the multiply phase of modmult.
	 */

	mp_lshift_unit(prod);

	/* 
	 * The product may have grown by as many as UNITSIZE
	 * bits.  That's why we have global_precision set to the
	 * size of the modulus plus UNITSIZE slop bits.
	 * Now reduce the product back down by conditionally
	 * subtracting the preshifted images of the modulus.
	 * This is part of the modulo reduction phase of modmult. 
	 * The following loop is unrolled for speed, using macros...

	 for (i=UNITSIZE-1; i>=LOG_UNITSIZE; i--)
	 if (mp_compare(prod,moduli[i]) >= 0)
	 mp_sub(prod,moduli[i]);
	 */

#ifndef UNIT8
#ifndef UNIT16			/* and not UNIT8 */
	msubs(31);
	msubs(30);
	msubs(29);
	msubs(28);
	msubs(27);
	msubs(26);
	msubs(25);
	msubs(24);
	msubs(23);
	msubs(22);
	msubs(21);
	msubs(20);
	msubs(19);
	msubs(18);
	msubs(17);
	msubs(16);
#endif				/* not UNIT16 and not UNIT8 */
	msubs(15);
	msubs(14);
	msubs(13);
	msubs(12);
	msubs(11);
	msubs(10);
	msubs(9);
	msubs(8);
#endif				/* not UNIT8 */
	msubs(7);
	msubs(6);
	msubs(5);
#ifndef UNIT32
	msubs(4);
#ifndef UNIT16
	msubs(3);
#endif
#endif

	/*
	 * Sniff each bit in the current unit of the multiplier, 
	 * and conditionally add the corresponding preshifted 
	 * image of the multiplicand to the product.
	 * This is also part of the multiply phase of modmult.
	 *
	 * The following loop is unrolled for speed, using macros...

	 for (i=UNITSIZE-1; i>=0; i--)
	 if (*multiplier & power_of_2(i))
	 mp_add(prod,mpd[i]);
	 */
#ifndef UNIT8
#ifndef UNIT16			/* and not UNIT8 */
	sniffadd(31);
	sniffadd(30);
	sniffadd(29);
	sniffadd(28);
	sniffadd(27);
	sniffadd(26);
	sniffadd(25);
	sniffadd(24);
	sniffadd(23);
	sniffadd(22);
	sniffadd(21);
	sniffadd(20);
	sniffadd(19);
	sniffadd(18);
	sniffadd(17);
	sniffadd(16);
#endif				/* not UNIT16 and not UNIT8 */
	sniffadd(15);
	sniffadd(14);
	sniffadd(13);
	sniffadd(12);
	sniffadd(11);
	sniffadd(10);
	sniffadd(9);
	sniffadd(8);
#endif				/* not UNIT8 */
	sniffadd(7);
	sniffadd(6);
	sniffadd(5);
	sniffadd(4);
	sniffadd(3);
	sniffadd(2);
	sniffadd(1);
	sniffadd(0);

	/*
	 * The product may have grown by as many as LOG_UNITSIZE+1
	 * bits.
	 * Now reduce the product back down by conditionally 
	 * subtracting LOG_UNITSIZE+1 preshifted images of the 
	 * modulus.  This is the modulo reduction phase of modmult.
	 *
	 * The following loop is unrolled for speed, using macros...

	 for (i=LOG_UNITSIZE; i>=0; i--)
	 if (mp_compare(prod,moduli[i]) >= 0) 
	 mp_sub(prod,moduli[i]); 
	 */

#ifndef UNIT8
#ifndef UNIT16
	msubs(5);
#endif
	msubs(4);
#endif
	msubs(3);
	msubs(2);
	msubs(1);
	msubs(0);

	/* Bump pointer to next lower unit of multiplier: */
	post_lowerunit(multiplier);

    }				/* Loop for the number of units in the
				   multiplier */

    return 0;			/* normal return */

}				/* merritt_modmult */

#undef msubs
#undef sniffadd

/*
 * Merritt's mp_modmult function leaves some internal tables in memory,
 * so we have to call modmult_burn() at the end of mp_modexp.
 * This is so that no cryptographically sensitive data is left in memory
 * after the program exits.
 */

/* Alias for modmult_burn, merritt_burn() is called only by mp_modexp. */
void merritt_burn(void)
{
    unitfill0(&(mpdbuf[0][0]), (UNITSIZE - 1) * MAX_UNIT_PRECISION);
    unitfill0(&(moduli_buf[0][0]), (UNITSIZE - 1) * MAX_UNIT_PRECISION);
    unitfill0(msu_moduli, UNITSIZE);
    unitfill0(nmsu_moduli, UNITSIZE);
}				/* merritt_burn() */

/******* end of Merritt's MODMULT stuff. *******/
/*=========================================================================*/
#endif				/* MERRITT */


#ifdef UPTON_OR_SMITH		/* Used by Upton's and Smith's
				   modmult algorithms */

/*
 * Jimmy Upton's multiprecision modmult algorithm in C.
 * Performs a multiply combined with a modulo operation.
 *
 * The following support functions and data structures
 * are used only by Upton's modmult algorithm...
 */

short munit_prec;		/* global_precision expressed in MULTUNITs */

/*
 * Note that since the SPARC CPU has no hardware integer multiply
 * instruction, there is not that much advantage in having an
 * assembly version of mp_smul on that machine.  It might be faster
 * to use Merritt's modmult instead of Upton's modmult on the SPARC.
 */

/*
 * Multiply the single-word multiplier times the multiprecision integer
 * in multiplicand, accumulating result in prod.  The resulting
 * multiprecision prod will be 1 word longer than the multiplicand.
 * multiplicand is munit_prec words long.  We add into prod, so caller
 * should zero it out first.  For best results, this time-critical
 * function should be implemented in assembly.
 * NOTE:  Unlike other functions in the multiprecision arithmetic
 * library, both multiplicand and prod are pointing at the LSB,
 * regardless of byte order of the machine.  On an 80x86, this makes
 * no difference.  But if this assembly function is implemented
 * on a 680x0, it becomes important.
 */

/*
 * Note that this has been modified from the previous version to allow
 * better support for Smith's modmult:
 *      The final carry bit is added to the existing product
 *      array, rather than simply stored.
 */

#ifndef mp_smul
void mp_smul(MULTUNIT * prod, MULTUNIT * multiplicand, MULTUNIT multiplier)
{
    short i;
    unsigned long p, carry;

    carry = 0;
    for (i = 0; i < munit_prec; ++i) {
	p = (unsigned long) multiplier **post_higherunit(multiplicand);
	p += *prod + carry;
	*post_higherunit(prod) = (MULTUNIT) p;
	carry = p >> MULTUNITSIZE;
    }
    /* Add carry to the next higher word of product / dividend */
    *prod += (MULTUNIT) carry;
}				/* mp_smul */

#endif

/*
 * mp_dmul is a double-precision multiply multiplicand times multiplier,
 * result into prod.  prod must be pointing at a "double precision"
 * buffer.  E.g. If global_precision is 10 words, prod must be
 * pointing at a 20-word buffer.
 */
#ifndef mp_dmul
void mp_dmul(unitptr prod, unitptr multiplicand, unitptr multiplier)
{
    register int i;
    register MULTUNIT *p_multiplicand, *p_multiplier;
    register MULTUNIT *prodp;


    unitfill0(prod, global_precision * 2);	/* Pre-zero prod */
    /* Calculate precision in units of MULTUNIT */
    munit_prec = global_precision * UNITSIZE / MULTUNITSIZE;
    p_multiplicand = (MULTUNIT *) multiplicand;
    p_multiplier = (MULTUNIT *) multiplier;
    prodp = (MULTUNIT *) prod;
    make_lsbptr(p_multiplicand, munit_prec);
    make_lsbptr(p_multiplier, munit_prec);
    make_lsbptr(prodp, munit_prec * 2);
    /* Multiply multiplicand by each word in multiplier, accumulating prod: */
    for (i = 0; i < munit_prec; ++i)
	mp_smul(post_higherunit(prodp),
		p_multiplicand, *post_higherunit(p_multiplier));
}				/* mp_dmul */
#endif				/* mp_dmul */

static unit ALIGN modulus[MAX_UNIT_PRECISION];
static short nbits;		/* number of modulus significant bits */
#endif				/* UPTON_OR_SMITH */

#ifdef UPTON

/*
 * These scratchpad arrays are used only by upton_modmult (mp_modmult).
 * Some of them could be staticly declared inside of mp_modmult, but we
 * put them outside mp_modmult so that they can be wiped clean by
 * modmult_burn(), which is called at the end of mp_modexp.  This is
 * so that no sensitive data is left in memory after the program exits.
 */

static unit ALIGN reciprocal[MAX_UNIT_PRECISION];
static unit ALIGN dhi[MAX_UNIT_PRECISION];
static unit ALIGN d_data[MAX_UNIT_PRECISION * 2];	/* double-wide
							   register */
static unit ALIGN e_data[MAX_UNIT_PRECISION * 2];	/* double-wide
							   register */
static unit ALIGN f_data[MAX_UNIT_PRECISION * 2];	/* double-wide
							   register */

static short nbitsDivUNITSIZE;
static short nbitsModUNITSIZE;

/*
 * stage_upton_modulus() is aliased to stage_modulus().
 * Prepare for an Upton modmult.  Calculate the reciprocal of modulus,
 * and save both.  Note that reciprocal will have one more bit than
 * modulus.
 * Assumes that global_precision has already been adjusted to the
 * size of the modulus, plus SLOP_BITS.
 */
int stage_upton_modulus(unitptr n)
{
    mp_move(modulus, n);
    mp_recip(reciprocal, modulus);
    nbits = countbits(modulus);
    nbitsDivUNITSIZE = nbits / UNITSIZE;
    nbitsModUNITSIZE = nbits % UNITSIZE;
    return 0;			/* normal return */
}				/* stage_upton_modulus */


/*
 * Upton's algorithm performs a multiply combined with a modulo operation.
 * Computes:  prod = (multiplicand*multiplier) mod modulus
 * WARNING: All the arguments must be less than the modulus!
 * References global unitptr modulus and reciprocal.
 * The reciprocal of modulus is 1 bit longer than the modulus.
 * upton_modmult() is aliased to mp_modmult().
 */
int upton_modmult(unitptr prod, unitptr multiplicand, unitptr multiplier)
{
    unitptr d = d_data;
    unitptr d1 = d_data;
    unitptr e = e_data;
    unitptr f = f_data;
    short orig_precision;

    orig_precision = global_precision;
    mp_dmul(d, multiplicand, multiplier);

    /* Throw off low nbits of d */
#ifndef HIGHFIRST
    d1 = d + nbitsDivUNITSIZE;
#else
    d1 = d + orig_precision - nbitsDivUNITSIZE;
#endif
    mp_move(dhi, d1);		/* Don't screw up d, we need it later */
    mp_shift_right_bits(dhi, nbitsModUNITSIZE);

    mp_dmul(e, dhi, reciprocal); /* Note - reciprocal has nbits+1 bits */

#ifndef HIGHFIRST
    e += nbitsDivUNITSIZE;
#else
    e += orig_precision - nbitsDivUNITSIZE;
#endif
    mp_shift_right_bits(e, nbitsModUNITSIZE);

    mp_dmul(f, e, modulus);

    /* Now for the only double-precision call to mpilib: */
    set_precision(orig_precision * 2);
    mp_sub(d, f);

    /* d's precision should be <= orig_precision */
    rescale(d, orig_precision * 2, orig_precision);
    set_precision(orig_precision);

    /* Should never have to do this final subtract more than twice: */
    while (mp_compare(d, modulus) > 0)
	mp_sub(d, modulus);

    mp_move(prod, d);
    return 0;			/* normal return */
}				/* upton_modmult */

/* Upton's mp_modmult function leaves some internal arrays in memory,
   so we have to call modmult_burn() at the end of mp_modexp.
   This is so that no cryptographically sensitive data is left in memory
   after the program exits.
   upton_burn() is aliased to modmult_burn().
 */
void upton_burn(void)
{
    unitfill0(modulus, MAX_UNIT_PRECISION);
    unitfill0(reciprocal, MAX_UNIT_PRECISION);
    unitfill0(dhi, MAX_UNIT_PRECISION);
    unitfill0(d_data, MAX_UNIT_PRECISION * 2);
    unitfill0(e_data, MAX_UNIT_PRECISION * 2);
    unitfill0(f_data, MAX_UNIT_PRECISION * 2);
    nbits = nbitsDivUNITSIZE = nbitsModUNITSIZE = 0;
}				/* upton_burn */

/******* end of Upton's MODMULT stuff. *******/
/*=========================================================================*/
#endif				/* UPTON */

#ifdef SMITH			/* using Thad Smith's modmult algorithm */

/*
 * Thad Smith's implementation of multiprecision modmult algorithm in C.
 * Performs a multiply combined with a modulo operation.
 * The multiplication is done with mp_dmul, the same as for Upton's
 * modmult.  The modulus reduction is done by long division, in
 * which a trial quotient "digit" is determined, then the product of
 * that digit and the divisor are subtracted from the dividend.
 *
 * In this case, the digit is MULTUNIT in size and the subtraction
 * is done by adding the product to the one's complement of the
 * dividend, which allows use of the existing mp_smul routine.
 *
 * The following support functions and data structures
 * are used only by Smith's modmult algorithm...
 */

/*
 * These scratchpad arrays are used only by smith_modmult (mp_modmult).
 * Some of them could be statically declared inside of mp_modmult, but we
 * put them outside mp_modmult so that they can be wiped clean by
 * modmult_burn(), which is called at the end of mp_modexp.  This is
 * so that no sensitive data is left in memory after the program exits.
 */

static unit ALIGN ds_data[MAX_UNIT_PRECISION * 2 + 2];

static unit mod_quotient[4];
static unit mod_divisor[4];	/* 2 most signif. MULTUNITs of modulus */

static MULTUNIT *modmpl;	/* ptr to modulus least significant
				   ** MULTUNIT                      */
#if defined(WIN32) && defined(USE_WIN32_ASSEMBLER)
int  mshift;                          /* number of bits for
                                      ** recip scaling          */
MULTUNIT reciph;                      /* MSunit of scaled recip */
MULTUNIT recipl;                      /* LSunit of scaled recip */
#else
static int  mshift;                   /* number of bits for
                                      ** recip scaling          */
static MULTUNIT reciph;               /* MSunit of scaled recip */
static MULTUNIT recipl;               /* LSunit of scaled recip */
#endif

static short modlenMULTUNITS;	/* length of modulus in MULTUNITs */
static MULTUNIT mutemp;		/* temporary */

/*
 * The routines mp_smul and mp_dmul are the same as for UPTON and
 * should be coded in assembly.  Note, however, that the previous
 * Upton's mp_smul version has been modified to compatible with
 * Smith's modmult.  The new version also still works for Upton's
 * modmult.
 */

#ifndef mp_set_recip
#define mp_set_recip(rh,rl,m)	/* null */
#else
/* setup routine for external mp_quo_digit */
void mp_set_recip(MULTUNIT rh, MULTUNIT rl, int m);
#endif
MULTUNIT mp_quo_digit(MULTUNIT * dividend);

#ifdef	MULTUNIT_SIZE_SAME
#define mp_musubb mp_subb	/* use existing routine */
#else				/* ! MULTUNIT_SIZE_SAME */

/*
 * This performs the same function as mp_subb, but with MULTUNITs.
 * Note: Processors without alignment requirements may be able to use
 * mp_subb, even though MULTUNITs are smaller than units.  In that case,
 * use mp_subb, since it would be faster if coded in assembly.  Note that
 * this implementation won't work for MULTUNITs which are long -- use
 * mp_subb in that case.
 */
#ifndef mp_musubb

/*
 * multiprecision subtract of MULTUNITs with borrow, r2 from r1, result in r1
 * borrow is incoming borrow flag-- value should be 0 or 1
 */
boolean mp_musubb
 (register MULTUNIT * r1, register MULTUNIT * r2, register boolean borrow)
{
    register ulint x;		/* won't work if sizeof(MULTUNIT)==
				   sizeof(long)     */
    short precision;		/* number of MULTUNITs to subtract */
    precision = global_precision * MULTUNITs_perunit;
    make_lsbptr(r1, precision);
    make_lsbptr(r2, precision);
    while (precision--) {
	x = (ulint) * r1 - (ulint) * post_higherunit(r2) - (ulint) borrow;
	*post_higherunit(r1) = x;
	borrow = (((1L << MULTUNITSIZE) & x) != 0L);
    }
    return (borrow);
}				/* mp_musubb */
#endif				/* mp_musubb */
#endif				/* MULTUNIT_SIZE_SAME */

/*
 * The function mp_quo_digit is the heart of Smith's modulo reduction,
 * which uses a form of long division.  It computes a trial quotient
 * "digit" (MULTUNIT-sized digit) by multiplying the three most
 * significant MULTUNITs of the dividend by the two most significant
 * MULTUNITs of the reciprocal of the modulus.  Note that this function
 * requires that MULTUNITSIZE * 2 <= sizeof(unsigned long).
 *
 * An important part of this technique is that the quotient never be
 * too small, although it may occasionally be too large.  This was
 * done to eliminate the need to check and correct for a remainder
 * exceeding the divisor.       It is easier to check for a negative
 * remainder.  The following technique rarely needs correction for
 * MULTUNITs of at least 16 bits.
 *
 * The following routine has two implementations:
 *
 * ASM_PROTOTYPE defined: written to be an executable prototype for
 * an efficient assembly language implementation.  Note that several
 * of the following masks and shifts can be done by directly
 * manipulating single precision registers on some architectures.
 *
 * ASM_PROTOTYPE undefined: a slightly more efficient implementation
 * in C.  Although this version returns a result larger than the
 * optimum (which is corrected in smith_modmult) more often than the
 * prototype version, the faster execution in C more than makes up
 * for the difference.
 * 
 * Parameter: dividend - points to the most significant MULTUNIT
 *      of the dividend.  Note that dividend actually contains the 
 *      one's complement of the actual dividend value (see comments for 
 *      smith_modmult).
 * 
 *  Return: the trial quotient digit resulting from dividing the first
 *      three MULTUNITs at dividend by the upper two MULTUNITs of the 
 *      modulus.
 */

/* #define ASM_PROTOTYPE *//* undefined: use C-optimized version */

#ifndef mp_quo_digit
MULTUNIT mp_quo_digit(MULTUNIT * dividend)
{
    unsigned long q, q0, q1, q2;
    unsigned short lsb_factor;

/*
 * Compute the least significant product group.
 * The last terms of q1 and q2 perform upward rounding, which is
 * needed to guarantee that the result not be too small.
 */
    q1 = (dividend[tohigher(-2)] ^ MULTUNIT_mask) * (unsigned long) reciph
	+ reciph;
    q2 = (dividend[tohigher(-1)] ^ MULTUNIT_mask) * (unsigned long) recipl
	+ (1L << MULTUNITSIZE);
#ifdef ASM_PROTOTYPE
    lsb_factor = 1 & (q1 >> MULTUNITSIZE) & (q2 >> MULTUNITSIZE);
    q = q1 + q2;

    /* The following statement is equivalent to shifting the sum right
       one bit while shifting in the carry bit.
     */
    q0 = (q1 > ~q2 ? DMULTUNIT_msb : 0) | (q >> 1);
#else				/* optimized C version */
    q0 = (q1 >> 1) + (q2 >> 1) + 1;
#endif

/*      Compute the middle significant product group.   */

    q1 = (dividend[tohigher(-1)] ^ MULTUNIT_mask) * (unsigned long) reciph;
    q2 = (dividend[0] ^ MULTUNIT_mask) * (unsigned long) recipl;
#ifdef ASM_PROTOTYPE
    q = q1 + q2;
    q = (q1 > ~q2 ? DMULTUNIT_msb : 0) | (q >> 1);

/*      Add in the most significant word of the first group.
   The last term takes care of the carry from adding the lsb's
   that were shifted out prior to addition.
 */
    q = (q0 >> MULTUNITSIZE) + q + (lsb_factor & (q1 ^ q2));
#else				/* optimized C version */
    q = (q0 >> MULTUNITSIZE) + (q1 >> 1) + (q2 >> 1) + 1;
#endif

/*      Compute the most significant term and add in the others */

    q = (q >> (MULTUNITSIZE - 2)) +
	(((dividend[0] ^ MULTUNIT_mask) * (unsigned long) reciph) << 1);
    q >>= mshift;

/*      Prevent overflow and then wipe out the intermediate results. */

    mutemp = (MULTUNIT) min(q, (1L << MULTUNITSIZE) - 1);
    q = q0 = q1 = q2 = 0;
    lsb_factor = 0;
    (void) lsb_factor;
    return mutemp;
}
#endif				/* mp_quo_digit */

/*
 * stage_smith_modulus() - Prepare for a Smith modmult.
 * 
 * Calculate the reciprocal of modulus with a precision of two MULTUNITs.
 * Assumes that global_precision has already been adjusted to the
 * size of the modulus, plus SLOP_BITS.
 * 
 * Note: This routine was designed to work with large values and
 * doesn't have the necessary testing or handling to work with a
 * modulus having less than three significant units.  For such cases,
 * the separate multiply and modulus routines can be used.
 * 
 * stage_smith_modulus() is aliased to stage_modulus().
 */
int stage_smith_modulus(unitptr n_modulus)
{
    int original_precision;
    int sigmod;			/* significant units in modulus */
    unitptr mp;			/* modulus most significant pointer */
    MULTUNIT *mpm;		/* reciprocal pointer */
    int prec;			/* precision of reciprocal calc in units */

    mp_move(modulus, n_modulus);
    modmpl = (MULTUNIT *) modulus;
    modmpl = lsbptr(modmpl, global_precision * MULTUNITs_perunit);
    nbits = countbits(modulus);
    modlenMULTUNITS = (nbits + MULTUNITSIZE - 1) / MULTUNITSIZE;

    original_precision = global_precision;

    /* The following code copies the three most significant units of
     * modulus to mod_divisor.
     */
    mp = modulus;
    sigmod = significance(modulus);
    rescale(mp, original_precision, sigmod);
/* prec is the unit precision required for 3 MULTUNITs */
    prec = (3 + (MULTUNITs_perunit - 1)) / MULTUNITs_perunit;
    set_precision(prec);

    /* set mp = ptr to most significant units of modulus, then move
     * the most significant units to mp_divisor 
     */
    mp = msbptr(mp, sigmod) - tohigher(prec - 1);
    unmake_lsbptr(mp, prec);
    mp_move(mod_divisor, mp);

    /* Keep 2*MULTUNITSIZE bits in mod_divisor.
     * This will (normally) result in a reciprocal of 2*MULTUNITSIZE+1 bits.
     */
    mshift = countbits(mod_divisor) - 2 * MULTUNITSIZE;
    mp_shift_right_bits(mod_divisor, mshift);
    mp_recip(mod_quotient, mod_divisor);
    mp_shift_right_bits(mod_quotient, 1);

    /* Reduce to:   0 < mshift <= MULTUNITSIZE */
    mshift = ((mshift + (MULTUNITSIZE - 1)) % MULTUNITSIZE) + 1;
    /* round up, rescaling if necessary */
    mp_inc(mod_quotient);
    if (countbits(mod_quotient) > 2 * MULTUNITSIZE) {
	mp_shift_right_bits(mod_quotient, 1);
	mshift--;		/* now  0 <= mshift <= MULTUNITSIZE */
    }
    mpm = lsbptr((MULTUNIT *) mod_quotient, prec * MULTUNITs_perunit);
    recipl = *post_higherunit(mpm);
    reciph = *mpm;
    mp_set_recip(reciph, recipl, mshift);
    set_precision(original_precision);
    return 0;			/* normal return */
}				/* stage_smith_modulus */

/* Smith's algorithm performs a multiply combined with a modulo operation.
   Computes:  prod = (multiplicand*multiplier) mod modulus
   The modulus must first be set by stage_smith_modulus().
   smith_modmult() is aliased to mp_modmult().
 */
int smith_modmult(unitptr prod, unitptr multiplicand, unitptr multiplier)
{
    unitptr d;			/* ptr to product */
    MULTUNIT *dmph, *dmpl, *dmp;	/* ptrs to dividend (high, low, first)
					 * aligned for subtraction         */
/* Note that dmph points one MULTUNIT higher than indicated by
   global precision.  This allows us to zero out a word one higher than
   the normal precision.
 */
    short orig_precision;
    short nqd;			/* number of quotient digits remaining to
				 * be generated                            */
    short dmi;			/* number of significant MULTUNITs in
				   product */

    d = ds_data + 1;		/* room for leading MSB if HIGHFIRST */
    orig_precision = global_precision;
    mp_dmul(d, multiplicand, multiplier);

    rescale(d, orig_precision * 2, orig_precision * 2 + 1);
    set_precision(orig_precision * 2 + 1);
    *msbptr(d, global_precision) = 0;	/* leading 0 unit */

/*      We now start working with MULTUNITs.
   Determine the most significant MULTUNIT of the product so we don't
   have to process leading zeros in our divide loop.
 */
    dmi = significance(d) * MULTUNITs_perunit;
    if (dmi >= modlenMULTUNITS) {
	/* Make dividend negative.  This allows the use of mp_smul to
	 * "subtract" the product of the modulus and the trial divisor
	 * by actually adding to a negative dividend.
	 * The one's complement of the dividend is used, since it causes
	 * a zero value to be represented as all ones.  This facilitates
	 * testing the result for possible overflow, since a sign bit
	 * indicates that no adjustment is necessary, and we should not
	 * attempt to adjust if the result of the addition is zero.
	 */
	mp_inc(d);
	mp_neg(d);
	set_precision(orig_precision);
	munit_prec = global_precision * UNITSIZE / MULTUNITSIZE;

	/* Set msb, lsb, and normal ptrs of dividend */
	dmph = lsbptr((MULTUNIT *) d, (orig_precision * 2 + 1) *
		      MULTUNITs_perunit) + tohigher(dmi);
	nqd = dmi + 1 - modlenMULTUNITS;
	dmpl = dmph - tohigher(modlenMULTUNITS);

/*      
 * Divide loop.
 * Each iteration computes the next quotient MULTUNIT digit, then
 * multiplies the divisor (modulus) by the quotient digit and adds
 * it to the one's complement of the dividend (equivalent to
 * subtracting).  If the product was greater than the remaining dividend,
 * we get a non-negative result, in which case we subtract off the
 * modulus to get the proper negative remainder.
 */
	for (; nqd; nqd--) {
	    MULTUNIT q;		/* quotient trial digit */

	    q = mp_quo_digit(dmph);
	    if (q > 0) {
		mp_smul(dmpl, modmpl, q);

		/* Perform correction if q too large.
		   *  This rarely occurs.
		 */
		if (!(*dmph & MULTUNIT_msb)) {
		    dmp = dmpl;
		    unmake_lsbptr(dmp, orig_precision *
				  MULTUNITs_perunit);
		    if (mp_musubb(dmp,
				  (MULTUNIT *) modulus, 0))
			(*dmph)--;
		}
	    }
	    pre_lowerunit(dmph);
	    pre_lowerunit(dmpl);
	}
	/* d contains the one's complement of the remainder. */
	rescale(d, orig_precision * 2 + 1, orig_precision);
	set_precision(orig_precision);
	mp_neg(d);
	mp_dec(d);
    } else {
	/* Product was less than modulus.  Return it. */
	rescale(d, orig_precision * 2 + 1, orig_precision);
	set_precision(orig_precision);
    }
    mp_move(prod, d);
    return (0);			/* normal return */
}				/* smith_modmult */

/* Smith's mp_modmult function leaves some internal arrays in memory,
   so we have to call modmult_burn() at the end of mp_modexp.
   This is so that no cryptographically sensitive data is left in memory
   after the program exits.
   smith_burn() is aliased to modmult_burn().
 */
void smith_burn(void)
{
    empty_array(modulus);
    empty_array(ds_data);
    empty_array(mod_quotient);
    empty_array(mod_divisor);
    modmpl = 0;
    mshift = nbits = 0;
    reciph = recipl = 0;
    modlenMULTUNITS = mutemp = 0;
    mp_set_recip(0, 0, 0);
}				/* smith_burn */

/*      End of Thad Smith's implementation of modmult. */

#endif				/* SMITH */

/* Returns number of significant bits in r */
int countbits(unitptr r)
{
    int bits;
    short prec;
    register unit bitmask;
    init_bitsniffer(r, bitmask, prec, bits);
    return bits;
}				/* countbits */

char *copyright_notice(void)
/* force linker to include copyright notice in the executable object image. */
{
    return ("(c)1986 Philip Zimmermann");
}				/* copyright_notice */

/*
 * Russian peasant combined exponentiation/modulo algorithm.
 * Calls modmult instead of mult.
 * Computes:  expout = (expin**exponent) mod modulus
 * WARNING: All the arguments must be less than the modulus!
 */
int mp_modexp(register unitptr expout, register unitptr expin,
	      register unitptr exponent, register unitptr modulus)
{
    int bits;
    short oldprecision;
    register unit bitmask;
    unit product[MAX_UNIT_PRECISION];
    short eprec;

#ifdef COUNTMULTS
    tally_modmults = 0;		/* clear "number of modmults" counter */
    tally_modsquares = 0;	/* clear "number of modsquares" counter */
#endif				/* COUNTMULTS */
    mp_init(expout, 1);
    if (testeq(exponent, 0)) {
	if (testeq(expin, 0))
	    return -1;		/* 0 to the 0th power means return error */
	return 0;		/* otherwise, zero exponent means expout
				   is 1 */
    }
    if (testeq(modulus, 0))
	return -2;		/* zero modulus means error */
#if SLOP_BITS > 0		/* if there's room for sign bits */
    if (mp_tstminus(modulus))
	return -2;		/* negative modulus means error */
#endif				/* SLOP_BITS > 0 */
    if (mp_compare(expin, modulus) >= 0)
	return -3;		/* if expin >= modulus, return error */
    if (mp_compare(exponent, modulus) >= 0)
	return -4;		/* if exponent >= modulus, return error */

    oldprecision = global_precision;	/* save global_precision */
    /* set smallest optimum precision for this modulus */
    set_precision(bits2units(countbits(modulus) + SLOP_BITS));
    /* rescale all these registers to global_precision we just defined */
    rescale(modulus, oldprecision, global_precision);
    rescale(expin, oldprecision, global_precision);
    rescale(exponent, oldprecision, global_precision);
    rescale(expout, oldprecision, global_precision);

    if (stage_modulus(modulus)) {
	set_precision(oldprecision);	/* restore original precision */
	return -5;		/* unstageable modulus (STEWART algorithm) */
    }
    /* normalize and compute number of bits in exponent first */
    init_bitsniffer(exponent, bitmask, eprec, bits);

    /* We can "optimize out" the first modsquare and modmult: */
    bits--;			/* We know for sure at this point that
				   bits>0 */
    mp_move(expout, expin);	/*  expout = (1*1)*expin; */
    bump_bitsniffer(exponent, bitmask);

    while (bits--) {
#ifdef MACTC5	    
	    mac_poll_for_break();
#endif
	poll_for_break();	/* polls keyboard, allows ctrl-C to
				   abort program */
#ifdef COUNTMULTS
	tally_modsquares++;	/* bump "number of modsquares" counter */
#endif				/* COUNTMULTS */
	mp_modsquare(product, expout);
	if (sniff_bit(exponent, bitmask)) {
	    mp_modmult(expout, product, expin);
#ifdef COUNTMULTS
	    tally_modmults++;	/* bump "number of modmults" counter */
#endif				/* COUNTMULTS */
	} else {
	    mp_move(expout, product);
	}
	bump_bitsniffer(exponent, bitmask);
    }				/* while bits-- */
    mp_burn(product);		/* burn the evidence on the stack */
    modmult_burn();		/* ask mp_modmult to also burn its
				   own evidence */

#ifdef COUNTMULTS		/* diagnostic analysis */
    {
	long atomic_mults;
	unsigned int unitcount, totalmults;
	unitcount = bits2units(countbits(modulus));
	/* calculation assumes modsquare takes as long as a modmult: */
	atomic_mults = (long) tally_modmults *(unitcount * unitcount);
	atomic_mults += (long) tally_modsquares *(unitcount * unitcount);
	printf("%ld atomic mults for ", atomic_mults);
	printf("%d+%d = %d modsqr+modmlt, at %d bits, %d words.\n",
	       tally_modsquares, tally_modmults,
	       tally_modsquares + tally_modmults,
	       countbits(modulus), unitcount);
    }
#endif				/* COUNTMULTS */

    set_precision(oldprecision);	/* restore original precision */

    /* Do an explicit reference to the copyright notice so that the linker
       will be forced to include it in the executable object image... */
    copyright_notice();		/* has no real effect at run time */

    return 0;			/* normal return */
}				/* mp_modexp */

/*
 * This is a faster modexp for moduli with a known factorisation into two
 * relatively prime factors p and q, and an input relatively prime to the
 * modulus, the Chinese Remainder Theorem to do the computation mod p and
 * mod q, and then combine the results.  This relies on a number of
 * precomputed values, but does not actually require the modulus n or the
 * exponent e.
 * 
 * expout = expin ^ e mod (p*q).
 * We form this by evaluating
 * p2 = (expin ^ e) mod p and
 * q2 = (expin ^ e) mod q
 * and then combining the two by the CRT.
 * 
 * Two optimisations of this are possible.  First, we can reduce expin
 * modulo p and q before starting.
 * 
 * Second, since we know the factorisation of p and q (trivially derived
 * from the factorisation of n = p*q), and expin is relatively prime to
 * both p and q, we can use Euler's theorem, expin^phi(m) = 1 (mod m),
 * to throw away multiples of phi(p) or phi(q) in e.
 * Letting ep = e mod phi(p) and
 *         eq = e mod phi(q)
 * then combining these two speedups, we only need to evaluate
 * p2 = ((expin mod p) ^ ep) mod p and
 * q2 = ((expin mod q) ^ eq) mod q.
 * 
 * Now we need to apply the CRT.  Starting with
 * expout = p2 (mod p) and
 * expout = q2 (mod q)
 * we can say that expout = p2 + p * k, and if we assume that 0 <= p2 < p,
 * then 0 <= expout < p*q for some 0 <= k < q.  Since we want expout = q2
 * (mod q), then p*k = q2-p2 (mod q).  Since p and q are relatively prime,
 * p has a multiplicative inverse u mod q.  In other words, u = 1/p (mod q).
 *
 * Multiplying by u on both sides gives k = u*(q2-p2) (mod q).
 * Since we want 0 <= k < q, we can thus find k as
 * k = (u * (q2-p2)) mod q.
 * 
 * Once we have k, evaluating p2 + p * k is easy, and
 * that gives us the result.
 * 
 * In the detailed implementation, there is a temporary, temp, used to
 * hold intermediate results, p2 is held in expout, and q2 is used as a
 * temporary in the final step when it is no longer needed.  With that,
 * you should be able to understand the code below.
 */
int mp_modexp_crt(unitptr expout, unitptr expin,
		  unitptr p, unitptr q, unitptr ep, unitptr eq, unitptr u)
{
    unit q2[MAX_UNIT_PRECISION];
    unit temp[MAX_UNIT_PRECISION];
    int status;

/*      First, compiute p2 (physically held in M) */

/*      p2 = [ (expin mod p) ^ ep ] mod p               */
    mp_mod(temp, expin, p);	/* temp = expin mod p  */
    status = mp_modexp(expout, temp, ep, p);
    if (status < 0) {		/* mp_modexp returned an error. */
	mp_init(expout, 1);
	return status;		/* error return */
    }
/*      And the same thing for q2 */

/*      q2 = [ (expin mod q) ^ eq ] mod q               */
    mp_mod(temp, expin, q);	/* temp = expin mod q  */
    status = mp_modexp(q2, temp, eq, q);
    if (status < 0) {		/* mp_modexp returned an error. */
	mp_init(expout, 1);
	return status;		/* error return */
    }
/*      Now use the multiplicative inverse u to glue together the
   two halves.
 */
#if 0
/* This optimisation is useful if you commonly get small results,
   but for random results and large q, the odds of (1/q) of it
   being useful do not warrant the test.
 */
    if (mp_compare(expout, q2) != 0) {
#endif
	/*      Find q2-p2 mod q */
	if (mp_sub(q2, expout))	/* if the result went negative */
	    mp_add(q2, q);	/* add q to q2 */

	/*      expout = p2 + ( p * [(q2*u) mod q] )    */
	mp_mult(temp, q2, u);	/* q2*u  */
	mp_mod(q2, temp, q);	/* (q2*u) mod q */
	mp_mult(temp, p, q2);	/* p * [(q2*u) mod q] */
	mp_add(expout, temp);	/* expout = p2 + p * [...] */
#if 0
    }
#endif

    mp_burn(q2);		/* burn the evidence on the stack... */
    mp_burn(temp);
    return 0;			/* normal return */
}				/* mp_modexp_crt */


/****************** end of MPI library ****************************/


/* ==== mpw32asm.c ==== */

#if defined(USE_WIN32_ASSEMBLER)

#include "mpilib.h"

#if defined(SMITH)

extern unit reciph,recipl;
extern int  mshift;

#if !defined(_MSC_VER)
#error "This code needs a Microsoft compiler"
#endif

#if defined(_M_IX86)

#pragma warning(disable:4035)


unit P_QUO_DIGIT (unitptr dividend)
{
	__asm{
		mov	esi,DWORD PTR [dividend]
		push	ebp
		mov	eax, DWORD PTR [esi-8]
		not	eax
		mul	DWORD PTR [reciph]
		add	eax,DWORD PTR [reciph]
		adc	edx,0
		mov	ebx,eax
		mov	edi,edx
		mov	eax,DWORD PTR [esi-4]
		not	eax
		mul	DWORD PTR [recipl]
		inc	edx
		mov	ebp,edx
		and	ebp,edi
		and	ebp,1

		add	eax,ebx
		adc	edi,edx
		rcr	edi,1
		mov	eax,DWORD PTR [esi-4]
		not	eax
		mul	DWORD PTR [reciph]
		mov	ebx,eax
		mov	ecx,edx
		mov	eax, DWORD PTR [esi]
		not eax
		mul	DWORD PTR [recipl]
		xor	eax,ebx
		and	ebp,eax
		xor	eax,ebx
		add	eax,ebx
		adc	edx,ecx
		rcr	edx,1
		rcr	eax,1
		add	eax,edi
		adc	edx,0
		add	eax,ebp
		adc	edx,0
		shl	eax,1
		rcl	edx,1
		rcl eax,1
		rcl	edx,1
		rcl	eax,1
		and	eax,3
		mov	ecx,eax
		mov	ebx,edx
		mov	eax,DWORD PTR [esi]
		not eax
		mul	DWORD PTR [reciph]
		shl	eax,1
		rcl	edx,1
		add	eax,ebx
		adc	edx,ecx
		mov	ecx, DWORD PTR [mshift]
		cmp	DWORD PTR [mshift],32
		je	L2
		shrd	eax,edx,cl
		shr		edx,cl
		or	edx,edx
		je	L1
		mov	eax,-1
		jmp	L1
	L2:
		xchg	eax,edx
	L1:
		pop	ebp
	}
}

#pragma warning(default:4035)

#endif /* X86 */

#if defined(_M_PPC)
#error "We've not written the PowerPC Code Yet!"
#endif /* _M_PPC */

#if defined(_M_ALPHA)
#error "We've not written the Alpha Code Yet!"
#endif /* _M_ALPHA */

#if defined(_M_MRX000)
#error "We've not written the MIPS Code Yet!"
#endif /* _M_MRX000 */

#endif /*#defined(SMITH) */

#endif /* USE_WIN32_ASSEMBLER */


/* ==== noise.c ==== */
/*
 * Get environmental noise.
 *
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Note that while most PGP source modules bear Philip Zimmermann's
 * copyright notice, many of them have been revised or entirely written
 * by contributors who frequently failed to put their names in their
 * code.  Code that has been incorporated into PGP from other authors
 * was either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 *
 * Written by Colin Plumb.
 */


#ifdef UNIX
#include <sys/types.h>
#include <sys/time.h>		/* For gettimeofday() */
#include <sys/times.h>		/* for times() */
#include <stdlib.h>		/* For qsort() */
#endif /* UNIX */

#include <time.h>
#include "usuals.h"
#include "randpool.h"
#include "noise.h"
#ifdef MACTC5
#include "TimeManager.h"
#endif

#ifdef AMIGA  /* RKNOP 940613 */
#include <devices/timer.h>
#include <hardware/custom.h>
#include <exec/execbase.h>
extern __far struct Custom custom;   /* Custom chips */
#include <proto/exec.h>
#include <proto/timer.h>

/* Stuff used in noise.c, defined in random.c -- RKNOP 940613*/
extern struct timerequest *TimerIO;
extern union { struct timeval t;
               struct EClockVal e;
             } time0,time1;
extern unsigned short use_eclock;
#endif  /* AMIGA */

/* Some machines just don't have clock_t */
#if defined(sun) && defined(i386)
typedef long clock_t;
#endif

#if defined(MSDOS) || defined(__MSDOS__)

/* Use IBM PC hardware timer (1.19 MHz) */

#ifdef __GO32__
#include <pc.h>
#else
#include <conio.h>		/* for inp() and outp() */
#include <dos.h>                /* same for Turbo C EWS */
#endif

/* timer0 on 8253-5 on IBM PC or AT tics every .84 usec. */
#define timer0		0x40	/* 8253 timer 0 port */
#define timercntl	0x43	/* 8253 control register */

/*
 * On an IBM PC, timer 0 ticks every .84 usec.  It counts down
 * from 65536 by twos, toggling its output line after each
 * step.  On an original IBM PC, we can thus only get 15 bits
 * of the timer.  On a PC-AT or later, with an 8284 timer chip,
 * we can get all 16 bits by reading the status, which has the
 * state of the output bit in bit 7, and is effectively the
 * high bit of the counter.
 *
 * But latching the status is a command which the 8283 does not
 * recognize; the subsequent load is interpreted as one of
 * a pair to read the counter instead of the status.  (We get a
 * garbage bit instead of the one we expected, but that's no worse
 * than constant 0.)  But the 8283 doesn't like single reads.
 * (The 8284 is more forgiving.)
 *
 * So, to resolve all this, the following sequence is used:
 *
 * - Dummy read from counter 0 (low byte)
 * - Latch status and count (ignored by 8283)
 * - Read status (high byte on 8283)
 * - Latch count (ignored by 8284, as count is already latched)
 * - Read count (low)
 * - Read count (high)
 *
 * It would be better (a project for the future) to capture the counter
 * in a keyboard ISR, put it in a global variable, and have noise() read
 * the global.  This gets the most accurate possible time, and avoids
 * possible harmonic relationships with a keyboard polling loop.
 * (Which MS-DOS, silly thing that it is, almost certainly uses
 * internally.)
 */
static unsigned pctimer0(void)
{
    unsigned count;

#ifdef __GO32__
    inportb(timer0);
    outportb(timercntl, 0xC2);        /* Latch status and count for timer 0 */
    count = (inportb(timer0) & 0x80) << 8;
    outportb(timercntl, 0x00);        /* Latch count of timer 0 */
    count |= (inportb(timer0) & 0xFF) >> 1;
    count |= (inportb(timer0) & 0xFF) << 7;
#else
    inp(timer0);
    outp(timercntl, 0xC2);	/* Latch status and count for timer 0 */
    count = (inp(timer0) & 0x80) << 8;
    outp(timercntl, 0x00);	/* Latch count of timer 0 */
    count |= (inp(timer0) & 0xFF) >> 1;
    count |= (inp(timer0) & 0xFF) << 7;
#endif

    return count;
}

#endif				/* MSDOS || __MSDOS__ */


#ifdef UNIX

#define NOISEDEBUG
#ifdef NOISEDEBUG
#include "pgp.h"		/* for verbose and pgpout */
#include <stdio.h>
#endif

/* Function needed for qsort() */
static int noiseCompare(void const *p1, void const *p2)
{
    return *(int const *) p1 - *(int const *) p2;
}


#define DELTAS 15		/* Number of deltas to try */

/*
 * Find the resolution of the gettimeofday() clock by sampling
 * successive values until a tick boundary, at which point
 * the delta is entered into a table.  The median of the table is
 * returned as the system tick size.
 *
 * Some trickery is needed to defeat the habit systems have of
 * always incrementing the microseconds field so that no two calls
 * return the same value.  Thus, a "tick boundary" is assumed
 * when successive calls return a difference of >2 us.
 * (This catches cases where we make successive calls and one
 * other task sneaks in between.  More tasks in between are
 * sufficiently unlikely that they'll get cut off by the median
 * filter.
 *
 * When a tick boundary is found, the *first* time read during
 * the previous tick (tv_base) is subtracted from the new time
 * to get the microseconds per tick.
 *
 * The median of the ticks is taken to eliminate outliers due to
 * descheduling (extra large) or tv_base not being the "zero" time
 * in a given tick (slightly small).
 *
 * Note that Suns have a 1 us timer, and in SunOS 4.1, they return
 * that timer, but there is ~50 us of system-call overhead to get
 * it, so this overestimates the tick size consdierably.  On
 * SunOS 5.x/Solaris, the overhead has been cut to about 2.5 us,
 * so the inter-call time alternates between 2 and 3 us.  Some
 * better algorithms are required to cope with potentially faster
 * machines that really do return 1 us granularity.
 *
 * Current best idea (unimplemented): Sample a large number, and
 * track small (< 100 us) deltas in an array of counters, and
 * large ones in an array of deltas.  There should be three
 * bumps: 1 us auto-increment, the tick size (which may blend into
 * the previous bump), and time-slicing.  We want to throw out
 * the latter, then compute the average delta as the average cost
 * of making a call, then throw out the small values if they
 * are suspisciously smaller than this value.  Then some average
 * of the remainder should provide a good value for the cost of
 * making a call.
 *
 * The alternative to all this is to actually model the keystroke
 * latencies and compute the entropy directly.  A model considering
 * the previous interval only should be adequate.
 */
static unsigned noiseTickSize(void)
{
    int i;
    int j;
    unsigned deltas[DELTAS];
    unsigned t;
    struct timeval tv_base, tv_old, tv_new;

    i = j = 0;
    gettimeofday(&tv_base, 0);
    tv_old = tv_base;
    do {
	gettimeofday(&tv_new, 0);
	if (tv_new.tv_usec > tv_old.tv_usec + 2) {
	    deltas[i++] = tv_new.tv_usec - tv_base.tv_usec +
		1000000 * (tv_new.tv_sec - tv_base.tv_sec);
	    tv_base = tv_new;
	    j = 0;
	}
	tv_old = tv_new;

	/*
	 * If we are forever getting <= 2 us, then just assume
	 * it's 2 us.
	 */
	if (j++ > 10000)
	    return 2;
    } while (i < DELTAS);

    qsort(deltas, DELTAS, sizeof(deltas[0]), noiseCompare);

    t = deltas[DELTAS / 2];	/* Median */

#ifdef NOISEDEBUG
    if (verbose)
	fprintf(pgpout, "t = %u, clock frequency is %u Hz\n",
		t, (2000000 + t) / (2 * t));
#endif

    return t;
}

#endif				/* UNIX */


/* (Written by Guy Geens 95/12/07)
This routine gets the 200Hz counter from the system area.
This part of memory is only accessible in supervisor mode.
To add to randomness, also store 50/60/70Hz VBL (Vertical BLank)
counter. (There are two flavours of this one: One counter is
stopped while floppy disk access takes place, the other one keeps
running. Which one to use? Both? No: the elapsed time would be
connected (does this harm randomness? I don't know) and, when
using a hard disk drive, the same!
(I've just picked one.)
*/

#ifdef ATARI
#ifdef __PUREC__
#include <tos.h>
#else
#include <osbind.h>
#endif
static word32 counter,vblcount;

long getcount(void) {
	counter= *((long*)0x4baL); /* _hz_200 */
	vblcount= *((long*)0x466L); /* _frlock */
	return counter;
}

#endif


/*
 * Add as much environmentally-derived random noise as possible
 * to the randPool.  Typically, this involves reading the most
 * accurate system clocks available.
 *
 * Returns the number of ticks that has passed since the last call,
 * for entropy estimation purposes.
 */
word32
noise(void)
{
    static word32 lastcounter;
    word32 delta;
    time_t tnow;
    clock_t cnow;

    cnow = clock();
    randPoolAddBytes((byte *) & cnow, sizeof(cnow));

    tnow = time((time_t *) 0);
    randPoolAddBytes((byte *) & tnow, sizeof(tnow));

#if defined(MSDOS) || defined(__MSDOS__)
    {
	unsigned t;

	t = pctimer0();
	randPoolAddBytes((byte *) & t, sizeof(t));
	delta = t - (unsigned) lastcounter;
	lastcounter = t;
    }
#endif

#ifdef MACTC5
	{
		unsigned long t;
		
		delta = TMTicks();
		t=lastcounter+=delta;
		randPoolAddBytes((byte *)&t, sizeof(t));
	}
#endif

#ifdef WIN32
/* Win32 provides QueryPerformanceCounter(), which does precisely what we need here */
    {
        /* What am I doing here ? : We can't #include <windows.h> to get the prototype
           for QueryPerformanceCounter() because there are many namespace clashes
           between PGP and windows.h. So, we hack in the prototype inline. When we get
           a compiler which does namespaces, or someone removes all the clashes in PGP,
           this will go.
        */
#if defined(_MSC_VER)  /* only valid if we're using the Microsoft compiler */
	__declspec(dllimport) long __stdcall
            QueryPerformanceCounter(__int64 *lpPerformanceCount);
	unsigned t;
	__int64 perf_count;

	QueryPerformanceCounter(&perf_count);
	/* it doesn't matter if the return value is zero */
	t = (unsigned) perf_count;
	randPoolAddBytes((byte *) & t, sizeof(t));
	delta = t - (unsigned) lastcounter;
	lastcounter = t;
#else  /* Not Microsoft compiler */
#include "This compiler is not supported, modify the code above accordingly"
#endif /* _MSC_VER */
    }
#endif /* WIN32 */

#ifdef VMS
    {
	word32 t;
	/* VMS Hardware Clock */
	extern unsigned long vms_clock_bits[2];
	/* Clock update int. */
	extern const long vms_ticks_per_update;

	/* Capture fast system timer: */
	SYS$GETTIM(vms_clock_bits);
	randPoolAddBytes((byte *) & vms_clock_bits, sizeof(vms_clock_bits));
	t = vms_clock_bits[0] / vms_ticks_per_update;
	delta = t - lastcounter;
	lastcounter = t;
    }
#endif				/* VMS */

#ifdef UNIX
    /* Get noise from gettimeofday() */
    {
	struct timeval tv;
	word32 t;
	static unsigned ticksize = 0;

	if (!ticksize)
	    ticksize = noiseTickSize();

	gettimeofday(&tv, NULL);
	randPoolAddBytes((byte *) & tv, sizeof(tv));

	/* This may wrap, but it's unsigned, so that's okay */
	t = tv.tv_sec * 1000000 + tv.tv_usec;
	delta = t - lastcounter;
	lastcounter = t;

	delta /= ticksize;
    }
    /* Get noise from times() */
    {
	clock_t t;
	struct tms tms;

	t = times(&tms);
	randPoolAddBytes((byte *) & tms, sizeof(tms));
	randPoolAddBytes((byte *) & t, sizeof(t));
    }
#endif				/* UNIX */

#ifdef AMIGA     /* Whole next section added RKNOP 940613 */
#define AMIGA_DELTAS 15

   {
      static unsigned long ticksize = 0;
      int i=0,j;
      unsigned long deltas[AMIGA_DELTAS],swap;


      /* NOTE -- this next section (reading the Eclock or the
         microHz clock) will only happen within the do loop in
         trueRandAccum()!! */

      if (TimerIO && TimerBase)
      {  if (!ticksize)   /* Get tick size, similar to Unix */
         {  Forbid();    /* Turn off multitasking to get ticksize */
            if (use_eclock)
              ReadEClock(&time0.e);
            else
              am_GetSysTime(&time0.t);
            do
            {   if (use_eclock)
          {  ReadEClock(&time1.e);
             if (time1.e.ev_lo>time0.e.ev_lo)
               deltas[i++]=time1.e.ev_lo-time0.e.ev_lo;
             time0.e.ev_lo=time1.e.ev_lo;
             time0.e.ev_hi=time1.e.ev_hi;
          }
          else
          {  am_GetSysTime(&time1.t);
             if (CmpTime(&time0.t,&time1.t))
                deltas[i++]=1000000*(time1.t.tv_secs
                      -time0.t.tv_secs)
                  +(time1.t.tv_micro-time0.t.tv_micro);
             time0.t.tv_secs=time1.t.tv_secs;
             time0.t.tv_micro=time1.t.tv_micro;
          }
            } while (i<AMIGA_DELTAS);
            for (i=0;i<AMIGA_DELTAS-1;i++)
         for (j=i+1;j<AMIGA_DELTAS;j++)
            if (deltas[j]<deltas[i])
            {  swap=deltas[j];
               deltas[j]=deltas[i];
               deltas[i]=swap;
            }
            if ((ticksize=deltas[AMIGA_DELTAS/2])==0) ticksize=1;
            Permit();
         }

         if (use_eclock)
         {  ReadEClock(&time1.e);
            randPoolAddBytes((byte *)&time1.e.ev_lo,4);
            delta=time1.e.ev_lo-time0.e.ev_lo;  /* wrap ok?, unsigned */
            time0.e.ev_hi=time1.e.ev_hi;
            time0.e.ev_lo=time1.e.ev_lo;
         }
         else
         {  am_GetSysTime(&time1.t);
            randPoolAddBytes((byte *)&time1.t,sizeof(time1.t));
            delta=1000000*(time1.t.tv_secs - time0.t.tv_secs) +
             time1.t.tv_micro - time0.t.tv_micro;
            time0.t.tv_secs=time1.t.tv_secs;
         time0.t.tv_micro=time1.t.tv_micro;
         }
         delta/=ticksize;
      }

      /* Get some additional noise from the video beam poisition */
      randPoolAddBytes((byte *)&custom.vhposr,2);

      /* Pull the ExecBase dispatch count */
      randPoolAddBytes((byte *)
             &((*(struct ExecBase **)4)->DispCount),4);
   }
#endif /* AMIGA (RKNOP 940613) */

#ifdef ATARI	/* Section written by Guy Geens <guy.geens@elis.rug.ac.be> 951207 */
	Supexec(getcount);	/* Xbios 38 */
	delta=counter-lastcounter;
	lastcounter=counter;

#ifndef __PUREC__
	randPoolAddBytes((byte*)&counter,4);
#endif
	/* Under Pure C, counter is the same as cnow (returned by clock()),
	so it doesn't add to randomness.
	I don't have any other C compiler, so I can't check whether they
	also use the same counter. Please mail me further details */

 	randPoolAddBytes((byte*)&vblcount,4);
	/* Other compilers might require to comment this out and activate
	the previous line. */
#endif		/* End of section written by Guy Geens */

    return delta;
}


/* ==== passwd.c ==== */
/*	passwd.c - Password reading/hashing routines
	Implemented in Microsoft C.
	Routines for getting a pass phrase from the user's console.

	(c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
	The author assumes no liability for damages resulting from the use
	of this software, even if the damage results from defects in this
	software.  No warranty is expressed or implied.

	Note that while most PGP source modules bear Philip Zimmermann's
	copyright notice, many of them have been revised or entirely written
	by contributors who frequently failed to put their names in their
	code.  Code that has been incorporated into PGP from other authors
	was either originally published in the public domain or is used with
	permission from the various authors.

	PGP is available for free to the public under certain restrictions.
	See the PGP User's Guide (included in the release package) for
	important information about licensing, patent restrictions on
	certain algorithms, trademarks, copyrights, and export controls.
*/

#include	<stdio.h>	/* for fprintf() */
#include	<ctype.h>	/* for isdigit(), toupper(), etc. */
#include	<string.h>	/* for strlen() */

#include	"random.h"	/* for getstring() */
#include	"md5.h"
#include	"language.h"
#include	"pgp.h"
#include        "charset.h"

#ifdef AMIGA
#       include "system.h"
#endif

#define MAXKEYLEN 254	/* max byte length of pass phrase */

boolean showpass = FALSE;

/*
**	hashpass - Hash pass phrase down to 128 bits (16 bytes).
**  keylen must be less than 1024.
**	Use the MD5 algorithm.
*/
void hashpass (char *keystring, int keylen, byte *hash)
{
	struct MD5Context mdContext;

	/* Calculate the hash */
	MD5Init(&mdContext);
	MD5Update(&mdContext, (unsigned char *) keystring, keylen);
	MD5Final(hash, &mdContext);
} /* hashpass */


/*
**	GetHashedPassPhrase - get pass phrase from user,
		 hashes it to an IDEA key.
	Parameters:
		returns char *keystring as the pass phrase itself
		return char *hash as the 16-byte hash of the pass phrase
				using MD5.
		byte noecho:  
			0=ask once, echo. 
			1=ask once, no echo. 
			2=ask twice, no echo.
	Return 0 if no characters are input, else return 1.
	If we return 0, the hashed key will not be useful.
*/
int GetHashedPassPhrase(char *hash, boolean noecho)
{	char keystr1[MAXKEYLEN+2], keystr2[MAXKEYLEN+2];
	int len;

	if (showpass)
		noecho = 0;
	for (;;) {
		fprintf(pgpout,LANG("\nEnter pass phrase: "));
                fflush(pgpout);
#ifdef AMIGA
                requesterdesc=LANG("\nEnter pass phrase: ");
#endif

		getstring(keystr1,MAXKEYLEN-1,!noecho);
		if (noecho<2)	/* no need to ask again if user can see it */
			break;
		fprintf(pgpout,LANG("\nEnter same pass phrase again: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter same pass phrase again: ");
#endif
		getstring(keystr2,MAXKEYLEN-1,!noecho);
		if (strcmp(keystr1,keystr2)==0)
			break;
		fprintf(pgpout,
LANG("\n\007Error: Pass phrases were different.  Try again."));
		memset(keystr2, 0, sizeof(keystr2));
	}
	if (noecho && (filter_mode || quietmode))
		putc('\n', pgpout);

	len = strlen(keystr1);
	if (len == 0)
		return 0;
	CONVERT_TO_CANONICAL_CHARSET(keystr1);
	hashpass (keystr1, strlen(keystr1), (byte *) hash);
	memset(keystr1, 0, sizeof(keystr1));
	memset(keystr2, 0, sizeof(keystr2));
	return 1;
} /* GetHashedPassPhrase */



/* ==== pgp.c ==== */
/* #define TEMP_VERSION /* if defined, temporary experimental
   		            version of PGP */
/* pgp.c -- main module for PGP.
   PGP: Pretty Good(tm) Privacy - public key cryptography for the masses.

   Synopsis:  PGP uses public-key encryption to protect E-mail. 
   Communicate securely with people you've never met, with no secure
   channels needed for prior exchange of keys.  PGP is well featured and
   fast, with sophisticated key management, digital signatures, data
   compression, and good ergonomic design.

   The original PGP version 1.0 was written by Philip Zimmermann, of
   Phil's Pretty Good(tm) Software.  Many parts of later versions of 
   PGP were developed by an international collaborative effort, 
   involving a number of contributors, including major efforts by:
   Branko Lankester <branko@hacktic.nl>
   Hal Finney <74076.1041@compuserve.com>
   Peter Gutmann <pgut1@cs.aukuni.ac.nz>
   Other contributors who ported or translated or otherwise helped include:
   Jean-loup Gailly in France
   Hugh Kennedy in Germany
   Lutz Frank in Germany
   Cor Bosman in The Netherlands
   Felipe Rodriquez Svensson in The Netherlands
   Armando Ramos in Spain
   Miguel Angel Gallardo Ortiz in Spain
   Harry Bush and Maris Gabalins in Latvia
   Zygimantas Cepaitis in Lithuania
   Alexander Smishlajev
   Peter Suchkow and Andrew Chernov in Russia
   David Vincenzetti in Italy
   ...and others.


   (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
   The author assumes no liability for damages resulting from the use
   of this software, even if the damage results from defects in this
   software.  No warranty is expressed or implied.

   Note that while most PGP source modules bear Philip Zimmermann's
   copyright notice, many of them have been revised or entirely written
   by contributors who frequently failed to put their names in their
   code.  Code that has been incorporated into PGP from other authors
   was either originally published in the public domain or is used with
   permission from the various authors.

   PGP is available for free to the public under certain restrictions.
   See the PGP User's Guide (included in the release package) for
   important information about licensing, patent restrictions on
   certain algorithms, trademarks, copyrights, and export controls.


   Philip Zimmermann may be reached at:
   Boulder Software Engineering
   3021 Eleventh Street
   Boulder, Colorado 80304  USA
   (303) 541-0140  (voice or FAX)
   email:  prz@acm.org


   PGP will run on MSDOS, Sun Unix, VAX/VMS, Ultrix, Atari ST, 
   Commodore Amiga, and OS/2.  Note:  Don't try to do anything with 
   this source code without looking at the PGP User's Guide.

   PGP combines the convenience of the Rivest-Shamir-Adleman (RSA)
   public key cryptosystem with the speed of fast conventional
   cryptographic algorithms, fast message digest algorithms, data
   compression, and sophisticated key management.  And PGP performs 
   the RSA functions faster than most other software implementations.  
   PGP is RSA public key cryptography for the masses.

   Uses RSA Data Security, Inc. MD5 Message Digest Algorithm
   as a hash for signatures.  Uses the ZIP algorithm for compression.
   Uses the ETH IPES/IDEA algorithm for conventional encryption.

   PGP generally zeroes its used stack and memory areas before exiting.
   This avoids leaving sensitive information in RAM where other users
   could find it later.  The RSA library and keygen routines also
   sanitize their own stack areas.  This stack sanitizing has not been
   checked out under all the error exit conditions, when routines exit
   abnormally.  Also, we must find a way to clear the C I/O library
   file buffers, the disk buffers, and cache buffers.

   Revisions:
   Version 1.0 -  5 Jun 91
   Version 1.4 - 19 Jan 92
   Version 1.5 - 12 Feb 92
   Version 1.6 - 24 Feb 92
   Version 1.7 - 29 Mar 92
   Version 1.8 - 23 May 92
   Version 2.0 -  2 Sep 92
   Version 2.1 -  6 Dec 92
   Version 2.2 -  6 Mar 93
   Version 2.3 - 13 Jun 93
   Version 2.3a-  1 Jul 93
   Version 2.4 -  6 Nov 93
   Version 2.5 -  5 May 94
   Version 2.6 - 22 May 94
   Version 2.6.1 - 29 Aug 94
   Version 2.6.2 - 11 Oct 94
   Version 2.6.2i - 7 May 95
   Version 2.6.3(i) - 18 Jan 96
   Version 2.6.3(i)a - 4 Mar 96

 */


#include <ctype.h>
#ifndef AMIGA
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "system.h"
#include "mpilib.h"
#include "random.h"
#include "crypto.h"
#include "fileio.h"
#include "keymgmt.h"
#include "language.h"
#include "pgp.h"
#include "exitpgp.h"
#include "charset.h"
#include "getopt.h"
#include "config.h"
#include "keymaint.h"
#include "keyadd.h"
#include "rsaglue.h"
#include "noise.h"

#ifdef MACTC5
#include "Macutil.h"
#include "Macutil2.h"
#include "Macutil3.h"
#include "Macutil4.h"
#include "Macbinary.h"
#include "Binhex.h"
#include "MacPGP.h"
#include "mystr.h"
void AddOutputFiles(char *filepath);
void ReInitKeyMaint(void);
extern char appPathName[];
void 		ReInitGlobals(void);
extern int level,method;
extern Boolean explicit_plainfile;
extern long infile_line;
extern int eofonce;
extern boolean savedwashed;
extern char *special;
char *Outputfile = NULL;
void check_expiration_date(void);
#define BEST -1
#define exit Exit
void Exit(int x);
#endif

#ifdef  M_XENIX
char *strstr();
long time();
#endif

#ifdef MSDOS
#ifdef __ZTC__			/* Extend stack for Zortech C */
unsigned _stack_ = 24 * 1024;
#endif
#ifdef __TURBOC__
unsigned _stklen = 24 * 1024;
#endif
#endif
#define	STACK_WIPE	4096

#ifdef AMIGA
#ifdef __SASC_60
/* Let the compiler allocate us an appropriate stack. */
extern long __stack = 32768L;
#endif

 /* Add the appropriate AmigaOS version string, depending on the
  * compiler flags.
  */
#ifdef USA
static const char __DOSVer[] = "$VER: PGP 2.6.3a (04.03.96)"
#  ifdef _M68020
        " Amiga 68020 version by Rob Knop <rknop@mop.caltech.edu>";
#  else
        " Amiga 68000 version by Rob Knop <rknop@mop.caltech.edu>";
#  endif
#else
static const char __DOSVer[] = "$VER: PGP 2.6.3ia (04.03.96)"
#  ifdef _M68020
        " Amiga 68020 version by Peter Simons <simons@peti.rhein.de>";
#  else
        " Amiga 68000 version by Peter Simons <simons@peti.rhein.de>";
#  endif
#endif /* USA */
#endif /* AMIGA */

/* Global filenames and system-wide file extensions... */
#ifdef USA
char rel_version[] = _LANG("2.6.3a");	/* release version */
#else
char rel_version[] = _LANG("2.6.3ia");	/* release version */
#endif
char rel_date[] = "1996-03-04";		/* release date */
char PGP_EXTENSION[] = ".pgp";
char ASC_EXTENSION[] = ".asc";
char SIG_EXTENSION[] = ".sig";
char BAK_EXTENSION[] = ".bak";
static char HLP_EXTENSION[] = ".hlp";
char CONSOLE_FILENAME[] = "_CONSOLE";
#ifdef MACTC5
char HELP_FILENAME[256] = "pgp.hlp";
#else
static char HELP_FILENAME[] = "pgp.hlp";
#endif

/* These files use the environmental variable PGPPATH as a default path: */
char globalPubringName[MAX_PATH];
char globalSecringName[MAX_PATH];
char globalRandseedName[MAX_PATH];
char globalCommentString[128];

/* Flags which are global across the driver code files */
boolean verbose = FALSE;	/* -l option: display maximum information */
FILE *pgpout;			/* Place for routine messages */

static void usage(void);
static void key_usage(void);
static void arg_error(void);
static void initsigs(void);
static int do_keyopt(char);
static int do_decrypt(char *);
static void do_armorfile(char *);
char ** ParseRecipients(char **);
void hashpass (char *keystring, int keylen, byte *hash);

/* Various compression signatures: PKZIP, Zoo, GIF, Arj, and HPACK.
   Lha(rc) is handled specially in the code; it is missing from the
   compressSig structure intentionally.  If more formats are added,
   put them before lharc to keep the code consistent.

   27-Jun-95 simons@peti.rhein.de (Peter Simons)
   Added support for lh5 archive as generated by Lha. Unfortunately,
   lh5 requires special treatment also. I inserted the check right
   _before_ lharc, because lh5/lha is a special type of an lharc
   archive.
 */
static char *compressSig[] =
{"PK\03\04", "ZOO ", "GIF8", "\352\140", "Rar!",
 "HPAK", "\037\213", "\037\235", "\032\013", "\032HP%"
	/* lharc is special, must be last */ };
static char *compressName[] =
{"PKZIP", "Zoo", "GIF", "Arj", "RAR",
 "Hpack", "gzip", "compressed", "PAK", "Hyper",
 "Lha", "Lharc"};
static char *compressExt[] =
{".zip", ".zoo", ".gif", ".arj", ".rar",
 ".hpk", ".gz", ".Z", ".pak", ".hyp",
 ".lha", ".lzh"};

/* "\032\0??", "ARC", ".arc" */

#ifdef XOPENME
#include <xopenme.h>
#endif

/* Returns file signature type from a number of popular compression formats
   or -1 if no match */
int compressSignature(byte * header)
{
    int i;

    for (i = 0; i < sizeof(compressSig) / sizeof(*compressSig); i++)
	if (!strncmp((char *) header, compressSig[i], strlen(compressSig[i])))
	    return i;

    /* Special check for lha files */
    if (!strncmp((char *)header+2, "-lh5-", 5))
      return i;

    /* Special check for lharc files */
    if (header[2] == '-' && header[3] == 'l' &&
	(header[4] == 'z' || header[4] == 'h') &&
	header[6] == '-')
	return i+1;
    return -1;
}				/* compressSignature */

/* returns TRUE if file is likely to be compressible */
static boolean file_compressible(char *filename)
{
    byte header[8];
    get_header_info_from_file(filename, header, 8);
    if (compressSignature(header) >= 0)
	return FALSE;		/* probably not compressible */
    return TRUE;		/* possibly compressible */
}				/* compressible */


/* Possible error exit codes - not all of these are used.  Note that we
   don't use the ANSI EXIT_SUCCESS and EXIT_FAILURE.  To make things
   easier for compilers which don't support enum we use #defines */

#define EXIT_OK				0
#define INVALID_FILE_ERROR		1
#define FILE_NOT_FOUND_ERROR		2
#define UNKNOWN_FILE_ERROR		3
#define NO_BATCH			4
#define BAD_ARG_ERROR			5
#define INTERRUPT			6
#define OUT_OF_MEM			7

/* Keyring errors: Base value = 10 */
#define KEYGEN_ERROR			10
#define NONEXIST_KEY_ERROR		11
#define KEYRING_ADD_ERROR		12
#define KEYRING_EXTRACT_ERROR		13
#define KEYRING_EDIT_ERROR		14
#define KEYRING_VIEW_ERROR		15
#define KEYRING_REMOVE_ERROR		16
#define KEYRING_CHECK_ERROR		17
#define KEY_SIGNATURE_ERROR		18
#define KEYSIG_REMOVE_ERROR		19

/* Encode errors: Base value = 20 */
#define SIGNATURE_ERROR			20
#define RSA_ENCR_ERROR			21
#define ENCR_ERROR			22
#define COMPRESS_ERROR			23

/* Decode errors: Base value = 30 */
#define SIGNATURE_CHECK_ERROR		30
#define RSA_DECR_ERROR			31
#define DECR_ERROR			32
#define DECOMPRESS_ERROR		33


#ifdef SIGINT

/* This function is called if a BREAK signal is sent to the program.  In this
   case we zap the temporary files.
 */
void breakHandler(int sig)
{
#ifdef UNIX
    if (sig == SIGPIPE) {
	signal(SIGPIPE, SIG_IGN);
	exitPGP(INTERRUPT);
    }
    if (sig != SIGINT)
	fprintf(stderr, "\nreceived signal %d\n", sig);
    else
#endif
	fprintf(pgpout, LANG("\nStopped at user request\n"));
    exitPGP(INTERRUPT);
}
#endif

/* Clears screen and homes the cursor. */
static void clearscreen(void)
{
    fprintf(pgpout, "\n\033[0;0H\033[J\r           \r");  /* ANSI sequence. */
    fflush(pgpout);
}

/* We had to process the config file first to possibly select the 
   foreign language to translate the sign-on line that follows... */
static void signon_msg(void)
{    
    word32 tstamp;
    /* display message only once to allow calling multiple times */
    static boolean printed = FALSE;

    if (quietmode || printed)
	return;
    printed = TRUE;
    fprintf(stderr,
LANG("Pretty Good Privacy(tm) %s - Public-key encryption for the masses.\n"),
	    rel_version);
#ifdef TEMP_VERSION
    fputs(
"Internal development version only - not for general release.\n", stderr);
#endif
    fputs(LANG("(c) 1990-96 Philip Zimmermann, Phil's Pretty Good Software."),
    stderr);
    fprintf(stderr, " %s\n",LANG(rel_date));
#ifdef USA
    fputs(LANG(signon_legalese), stderr);
#endif
    fputs(
#ifdef USA
LANG("Export of this software may be restricted by the U.S. government.\n"),
#else
LANG("International version - not for use in the USA. Does not use RSAREF.\n"),
#endif
	  stderr);

    get_timestamp((byte *) & tstamp);	/* timestamp points to tstamp */
    fprintf(pgpout, LANG("Current time: %s\n"), ctdate(&tstamp));
}


#ifdef TEMP_VERSION		/* temporary experimental version of PGP */
#include <time.h>
#define CREATION_DATE 0x30FE3640ul
				/* CREATION_DATE is
				   Thu Jan 18, 1996 1200 hours UTC */
#define LIFESPAN	((unsigned long) 60L * (unsigned long) 86400L)
				/* LIFESPAN is 60 days */

/* If this is an experimental version of PGP, cut its life short */
void check_expiration_date(void)
{
    if (get_timestamp(NULL) > (CREATION_DATE + LIFESPAN)) {
	fprintf(stderr,
		"\n\007This experimental version of PGP has expired.\n");
	exit(-1);		/* error exit */
    }
}				/* check_expiration_date */
#else				/* no expiration date */
#define check_expiration_date()	/* null statement */
#endif				/* TEMP_VERSION */

/* -f means act as a unix-style filter */
/* -i means internalize extended file attribute information, only supported
 *          between like (or very compatible) operating systems. */
/* -l means show longer more descriptive diagnostic messages */
/* -m means display plaintext output on screen, like unix "more" */
/* -d means decrypt only, leaving inner signature wrapping intact */
/* -t means treat as pure text and convert to canonical text format */

/* Used by getopt function... */
#define OPTIONS "abcdefghiklmo:prstu:vwxz:ABCDEFGHIKLMO:PRSTU:VWX?"
extern int optind;
extern char *optarg;

#define INCLUDE_MARK "-@"
#define INCLUDE_MARK_LEN sizeof(INCLUDE_MARK)-1	/* skip the \0 */

boolean emit_radix_64 = FALSE;	/* set by config file */
static boolean sign_flag = FALSE;
boolean moreflag = FALSE;
boolean filter_mode = FALSE;
static boolean preserve_filename = FALSE;
static boolean decrypt_only_flag = FALSE;
static boolean de_armor_only = FALSE;
static boolean strip_sig_flag = FALSE;
boolean clear_signatures = TRUE;
boolean strip_spaces;
static boolean c_flag = FALSE;
static boolean u_flag = FALSE;		/* Did I get my_name from -u? */
boolean encrypt_to_self = FALSE; /* should I encrypt messages to myself? */
boolean sign_new_userids = TRUE;
boolean batchmode = FALSE;	/* if TRUE: don't ask questions */
boolean quietmode = TRUE; /* FGG changed */
boolean force_flag = TRUE;	/* overwrite existing file without asking */
#ifdef VMS			/* kludge for those stupid VMS variable-length
				   text records */
char literal_mode = MODE_TEXT;	/* MODE_TEXT or MODE_BINARY for literal
				   packet */
#else				/* not VMS */
char literal_mode = MODE_BINARY; /* MODE_TEXT or MODE_BINARY for literal
				    packet */
#endif				/* not VMS */
/* my_name is substring of default userid for secret key to make signatures */
char my_name[256] = "\0";	/* null my_name means take first userid
				   in ring */
boolean keepctx = FALSE;	/* TRUE means keep .ctx file on decrypt */
/* Ask for each key separately if it should be added to the keyring */
boolean interactive_add = FALSE;
boolean compress_enabled = TRUE; /* attempt compression before encryption */
long timeshift = 0L;		/* seconds from GMT timezone */
int version_byte = VERSION_BYTE_NEW;
boolean nomanual = 0;
/* If non-zero, initialize file to this many random bytes */
int makerandom = 0;


static char *outputfile = NULL;
#ifndef MACTC5
static int errorLvl = EXIT_OK;
#else
int errorLvl = EXIT_OK;
#endif
static char mcguffin[256];	/* userid search tag */
boolean signature_checked = FALSE;
int checksig_pass = 0;
boolean use_charset_header;
char charset_header[16] = "";
char plainfile[MAX_PATH];
int myArgc = 2;
char **myArgv;
struct hashedpw *passwds = 0, *keypasswds = 0;
static struct hashedpw **passwdstail = &passwds;

#ifdef MACTC5
extern unsigned long PGPStart, WNECalls;

void ReInitGlobals()
{
	int i;
    char scratch[64];
    WNECalls = 0;
    if (verbose)
    	PGPStart = TickCount();
    else
    	PGPStart = 0;
    Abort = FALSE;
	BreakCntl = 0;
	pgpout = stderr;
    optind = 1;
	errorLvl = EXIT_OK;
    myArgc = 2;
    myArgv = nil;
    emit_radix_64 = FALSE;		/* set by config file */
    sign_flag = FALSE;
    moreflag = FALSE;
    filter_mode = FALSE;
    preserve_filename = FALSE;
    decrypt_only_flag = FALSE;
    de_armor_only = FALSE;
    strip_sig_flag = FALSE;
    u_flag = FALSE;
	c_flag = FALSE;
    signature_checked = FALSE;
	literal_mode = MODE_BINARY;	/* MODE_TEXT or MODE_BINARY for literal packet */
    errorLvl = EXIT_OK;
    outputfile = Outputfile;
    method = BEST; 	/* one of BEST, DEFLATE (only), or STORE (only) */
    level = 9;		/* 0=fastest compression, 9=best compression */
    special = NULL;	/* List of special suffixes */
    infile_line = 0;
	eofonce = 0;
	savedwashed = FALSE;
	ReInitKeyMaint();
	settmpdir(nil);
	setoutdir(nil);
	makerandom = 0;
	if (xcli_opt[0]) {
    	if (argv[argc] == nil)
    		argv[argc] = malloc((size_t) 80);
		if (argv[argc] == nil) {
			BailoutAlert(LANG("Out of memory"));
			ExitToShell();
		}
		strcpy(argv[argc], xcli_opt);
		argc++;
		fprintf(pgpout, "         %s\n", xcli_opt);
	}
	for (i = 0; i <= 63; i++)
		scratch[i] = to_upper(xcli_opt[i]);
	if (strcmp(xcli_opt, "+NOMANUAL=ON")==0) nomanual = true;
	else nomanual = false;
 }

int init_pgp()
{
	int err=0;
	pgpout=stderr;
	/* Process the config file first.  Any command-line arguments will
	   override the config file settings */
	buildfilename( mcguffin, "config.txt");
	if ( processConfigFile( mcguffin ) < 0 )
		err=BAD_ARG_ERROR;
	init_charset();
	signon_msg();
	g_armor_flag=emit_radix_64;
	g_text_mode=(literal_mode == MODE_TEXT);
	g_clear_signatures=clear_signatures;
	PGPSetFinfo(globalRandseedName,'RSed','MPGP');
	set_precision(MAX_UNIT_PRECISION);
	return err;
}


void Exit(int x) {

	errorLvl = x;
	if (myArgv)
		free(myArgv);
	if (mcguffins)
		free(mcguffins);
	mac_cleanup_tmpf();
	longjmp(jmp_env,5);
}


int pgp_dispatch(int argc, char *argv[])
{
	int status, opt;
	char *inputfile = NULL;
	char **recipient = NULL;
/*	char **mcguffins;   zigf made global so we can free */
	boolean macbin_flag = FALSE;
#else

int main(int argc, char *argv[])
{
    long ct_repeat=0;
    long ct_repeat_max=1;
    int ct_return=0;

    int status, opt;
    char *inputfile = NULL;
    char **recipient = NULL;
    char **mcguffins;
#endif /* MACTC5 */
    char *workfile, *tempf;
    boolean nestflag = FALSE;
    boolean decrypt_mode = FALSE;
    boolean wipeflag = FALSE;
    boolean armor_flag = FALSE;	/* -a option */
    boolean separate_signature = FALSE;
    boolean keyflag = FALSE;
    boolean encrypt_flag = FALSE;
    boolean conventional_flag = FALSE;
    boolean attempt_compression; /* attempt compression before encryption */
    boolean output_stdout;	/* Output goes to stdout */
    char *clearfile = NULL;
    char *literal_file = NULL;
    char literal_file_name[MAX_PATH];
    char cipherfile[MAX_PATH];
    char keychar = '\0';
    char *p;
    byte ctb;
    struct hashedpw *hpw;

    /* Initial messages to stderr */
    pgpout = stderr;

#ifdef XOPENME
  xopenme_init(1,0);
#endif

    if (getenv("CT_REPEAT_MAIN")!=NULL) ct_repeat_max=atol(getenv("CT_REPEAT_MAIN"));

#ifdef XOPENME
  xopenme_clock_start(0);
#endif


#ifdef MACTC5
	ReInitGlobals();
#endif
#ifdef	DEBUG1
    verbose = TRUE;
#endif
    /* The various places one can get passwords from.
     * We accumulate them all into two lists.  One is
     * to try on keys only, and is stored in no particular
     * order, while the other is of unknown purpose so
     * far (they may be used for conventional encryption
     * or decryption as well), and are kept in a specific
     * order.  If any password in the general list is found
     * to decode a key, it is moved to the key list.
     * The general list is not grown after initialization,
     * so the tail pointer is not used after this.
     */

#ifndef MACTC5
    if ((p = getenv("PGPPASS")) != NULL) {
	hpw = xmalloc(sizeof(struct hashedpw));
	hashpass(p, strlen(p), hpw->hash);
	/* Add to linked list of key passwords */
	hpw->next = keypasswds;
	keypasswds = hpw;
    }
    /* The -z "password" option should be used instead of PGPPASS if
     * the environment can be displayed with the ps command (eg. BSD).
     * If the system does not allow overwriting of the command line
     * argument list but if it has a "hidden" environment, PGPPASS
     * should be used.
     */
    for (opt = 1; opt < argc; ++opt) {
	p = argv[opt];
	if (p[0] != '-' || p[1] != 'z')
	    continue;
	/* Accept either "-zpassword" or "-z password" */
	p += 2;
	if (!*p)
	    p = argv[++opt];
	/* p now points to password */
	if (!p)
	    break;		/* End of arg list - ignore */
	hpw = xmalloc(sizeof(struct hashedpw));
	hashpass(p, strlen(p), hpw->hash);
	/* Wipe password */
	while (*p)
	    *p++ = ' ';
	/* Add to tail of linked list of passwords */
	hpw->next = 0;
	*passwdstail = hpw;
	passwdstail = &hpw->next;
    }
    /*
     * If PGPPASSFD is set in the environment try to read the password
     * from this file descriptor.  If you set PGPPASSFD to 0 pgp will
     * use the first line read from stdin as password.
     */
    if ((p = getenv("PGPPASSFD")) != NULL) {
	int passfd;
	if (*p && (passfd = atoi(p)) >= 0) {
	    char pwbuf[256];
	    p = pwbuf;
	    while (read(passfd, p, 1) == 1 && *p != '\n')
		++p;
	    hpw = xmalloc(sizeof(struct hashedpw));
	    hashpass(pwbuf, p - pwbuf, hpw->hash);
	    memset(pwbuf, 0, p - pwbuf);
	    /* Add to tail of linked list of passwords */
	    hpw->next = 0;
	    *passwdstail = hpw;
	    passwdstail = &hpw->next;
	}
    }
    /* Process the config file.  The following override each other:
       - Hard-coded defaults
       - The system config file
       - Hard-coded defaults for security-critical things
       - The user's config file
       - Environment variables
       - Command-line options.
     */
    opt = 0;			/* Number of config files read */
#ifdef PGP_SYSTEM_DIR
#ifdef UNIX
    buildsysfilename(mcguffin, ".pgprc");
    if (access(mcguffin, 0) != 0)
#endif
    buildsysfilename(mcguffin, "config.txt");
    if (access(mcguffin, 0) == 0) {
	opt++;
	/*
	 * Note: errors here are NOT fatal, so that people
	 * can use PGP with a corrputed system file.
	 */
	processConfigFile(mcguffin);
    }
#endif

    /*
     * These must be personal; the system config file may not
     * influence them.
     */
    buildfilename(globalPubringName, "pubring.pgp");
    buildfilename(globalSecringName, "secring.pgp");
    buildfilename(globalRandseedName, "randseed.bin");
    my_name[0] = '\0';

    /* Process the config file first.  Any command-line arguments will
       override the config file settings */
#if defined(UNIX) || defined(MSDOS) || defined(OS2) || defined (WIN32)
    /* Try "pgp.ini" on MS-DOS or ".pgprc" on Unix */
#ifdef UNIX
    buildfilename(mcguffin, ".pgprc");
#else
    buildfilename(mcguffin, "pgp.ini");
#endif
    if (access(mcguffin, 0) != 0)
	buildfilename(mcguffin, "config.txt");
#else
    buildfilename(mcguffin, "config.txt");
#endif
    if (access(mcguffin, 0) == 0) {
	opt++;
	if (processConfigFile(mcguffin) < 0)
	    exit(BAD_ARG_ERROR);
    }
/* gfursin    if (!opt)
	fprintf(pgpout, LANG("\007No configuration file found.\n")); */

    init_charset();
#endif /* MACTC5 */

#ifdef MSDOS			/* only on MSDOS systems */
    if ((p = getenv("TZ")) == NULL || *p == '\0') {
	fprintf(pgpout,LANG("\007WARNING: Environmental variable TZ is not \
defined, so GMT timestamps\n\
may be wrong.  See the PGP User's Guide to properly define TZ\n\
in AUTOEXEC.BAT file.\n"));
    }
#endif				/* MSDOS */

#ifdef VMS
#define TEMP "SYS$SCRATCH"
#else
#define TEMP "TMP"
#endif				/* VMS */
    if ((p = getenv(TEMP)) != NULL && *p != '\0')
	settmpdir(p);

    if ((myArgv = (char **) malloc((argc + 2) * sizeof(char **))) == NULL) {
	fprintf(stderr, LANG("\n\007Out of memory.\n"));
	exitPGP(7);
    }
    myArgv[0] = NULL;
    myArgv[1] = NULL;

    /* Process all the command-line option switches: */
    while (optind < argc) {
	/*
	 * Allow random order of options and arguments (like GNU getopt)
	 * NOTE: this does not work with GNU getopt, use getopt.c from
	 * the PGP distribution.
	 */
	if ((!strncmp(argv[optind], INCLUDE_MARK, INCLUDE_MARK_LEN)) ||
           ((opt = pgp_getopt(argc, argv, OPTIONS)) == EOF)) {
	    if (optind == argc)	/* -- at end */
		break;
	    myArgv[myArgc++] = argv[optind++];
	    continue;
	}
	opt = to_lower(opt);
	if (keyflag && (keychar == '\0' || (keychar == 'v' && opt == 'v'))) {
	    if (keychar == 'v')
		keychar = 'V';
	    else
		keychar = opt;
	    continue;
	}
	switch (opt) {
	case 'a':
	    armor_flag = TRUE;
	    emit_radix_64 = 1;
	    break;
	case 'b':
	    separate_signature = strip_sig_flag = TRUE;
	    break;
	case 'c':
	    encrypt_flag = conventional_flag = TRUE;
	    c_flag = TRUE;
	    break;
	case 'd':
	    decrypt_only_flag = TRUE;
	    break;
	case 'e':
	    encrypt_flag = TRUE;
	    break;
#ifdef MACTC5
	case 'f':
		if (macbin_flag == FALSE) filter_mode = TRUE;
		break;
#else
	case 'f':
	    filter_mode = TRUE;
	    break;
#endif
	case '?':
	case 'h':
	    usage();
	    break;
#ifdef VMS
	case 'i':
	    literal_mode = MODE_LOCAL;
	    break;
#else
#ifdef MACTC5
	case 'i':
		macbin_flag = TRUE;
		moreflag = FALSE;
		literal_mode = MODE_BINARY;
		filter_mode = FALSE;
		break;
#endif /* MACTC5 */
#endif				/* VMS */
	case 'k':
	    keyflag = TRUE;
	    break;
	case 'l':
	    verbose = TRUE;
	    break;
#ifdef MACTC5
	case 'm':
		if( macbin_flag == FALSE )
			moreflag = TRUE;
		break;
#else
	case 'm':
	    moreflag = TRUE;
	    break;
#endif
	case 'p':
	    preserve_filename = TRUE;
	    break;
	case 'o':
	    outputfile = optarg;
	    break;
	case 's':
	    sign_flag = TRUE;
	    break;
#ifdef MACTC5
	case 't':
		if( macbin_flag == FALSE )
			literal_mode = MODE_TEXT;
		break;
#else
	case 't':
	    literal_mode = MODE_TEXT;
	    break;
#endif
	case 'u':
	    strncpy(my_name, optarg, sizeof(my_name) - 1);
	    CONVERT_TO_CANONICAL_CHARSET(my_name);
	    u_flag = TRUE;
	    break;
	case 'w':
	    wipeflag = TRUE;
	    break;
	case 'z':
	    break;
	    /* '+' special option: does not require - */
	case '+':
	    if (processConfigLine(optarg) == 0) {
                if (!strncmp(optarg,"CH",2)) /* CHARSET */
                    init_charset();
		break;
	    }
	    fprintf(stderr, "\n");
	    /* fallthrough */
	default:
	    arg_error();
	}
    }
    myArgv[myArgc] = NULL;	/* Just to make it NULL terminated */

    if (keyflag && keychar == '\0')
	key_usage();

    signon_msg();
    check_expiration_date();	/* hobble any experimental version */

    /*
     * Write to stdout if explicitly asked to, or in filter mode and
     * no explicit file name was given.
     */
    output_stdout = outputfile ? strcmp(outputfile, "-")  == 0 : filter_mode;

#if 1
    /* At request of Peter Simons, use stderr always. Sounds reasonable. */
    /* JIS: Put this code back in... removing it broke too many things */
    if (!output_stdout)
	pgpout = stdout;
#endif


#if defined(UNIX) || defined(VMS)
    umask(077);			/* Make files default to private */
#endif

    initsigs();			/* Catch signals */
    noise();			/* Start random number generation */

    if (keyflag) {
	status = do_keyopt(keychar);
	if (status < 0)
	    user_error();
	exitPGP(status);
    }
    /* -db means break off signature certificate into separate file */
    if (decrypt_only_flag && strip_sig_flag)
	decrypt_only_flag = FALSE;

    if (decrypt_only_flag && armor_flag)
	decrypt_mode = de_armor_only = TRUE;

    if (outputfile != NULL)
	preserve_filename = FALSE;

    if (!sign_flag && !encrypt_flag && !conventional_flag && !armor_flag) {
	if (wipeflag) {		/* wipe only */
	    if (myArgc != 3)
		arg_error();	/* need one argument */
	    if (wipefile(myArgv[2]) == 0 && remove(myArgv[2]) == 0) {
		fprintf(pgpout,
			LANG("\nFile %s wiped and deleted. "), myArgv[2]);
		fprintf(pgpout, "\n");
		exitPGP(EXIT_OK);
	    } else if (file_exists(myArgv[2]))
	        fprintf(pgpout,
LANG("\n\007Error: Can't wipe out file '%s' - read only, maybe?\n"),
                        myArgv[2]);
            else {
	        fprintf(pgpout,
		        LANG("\n\007File '%s' does not exist.\n"), myArgv[2]);
	    }
	    exitPGP(UNKNOWN_FILE_ERROR);
	}
	/* decrypt if none of the -s -e -c -a -w options are specified */
	decrypt_mode = TRUE;
    }
    if (myArgc == 2) {		/* no arguments */
#ifdef UNIX
	if (!filter_mode && !isatty(fileno(stdin))) {
	    /* piping to pgp without arguments and no -f:
	     * switch to filter mode but don't write output to stdout
	     * if it's a tty, use the preserved filename */
	    if (!moreflag)
		pgpout = stderr;
	    filter_mode = TRUE;
	    if (isatty(fileno(stdout)) && !moreflag)
		preserve_filename = TRUE;
	}
#endif
	if (!filter_mode) {
	    if (quietmode) {
		quietmode = FALSE;
		signon_msg();
	    }
	    fprintf(pgpout,
LANG("\nFor details on licensing and distribution, see the PGP User's Guide.\
\nFor other cryptography products and custom development services, contact:\
\nPhilip Zimmermann, 3021 11th St, Boulder CO 80304 USA, \
phone +1 303 541-0140\n"));
	    if (strcmp((p = LANG("@translator@")), "@translator@"))
		fprintf(pgpout, p);
	    fprintf(pgpout, LANG("\nFor a usage summary, type:  pgp -h\n"));
#ifdef MACTC5
		exitPGP(BAD_ARG_ERROR);
#else
	    exit(BAD_ARG_ERROR);	/* error exit */
#endif
	}
    } else {
	if (filter_mode) {
	    recipient = &myArgv[2];
	} else {
	    inputfile = myArgv[2];
	    recipient = &myArgv[3];
	}
	recipient = ParseRecipients(recipient);
    }


    if (filter_mode) {
	inputfile = "stdin";
    } else if (makerandom > 0) {	/* Create the input file */
	/*
	 * +makerandom=<bytes>: Create an input file consisting of <bytes>
	 * cryptographically strong random bytes, before applying the
	 * encryption options of PGP.  This is an advanced option, so
	 * assume the user knows what he's doing and don't bother about
	 * overwriting questions.  E.g.
	 * pgp +makerandom=24 foofile
	 *	Create "foofile" with 24 random bytes in it.
	 * pgp +makerandom=24 -ea foofile recipient
	 *	The same, but also encrypt it to "recipient", creating
	 *	foofile.asc as well.
	 * This feature was created to allow PGP to create and send keys
	 * around for other applications to use.
	 */
	status = cryptRandWriteFile(inputfile, (struct IdeaCfbContext *)0,
	                       (unsigned)makerandom);
	if (status < 0) {
		fprintf(stderr,"Error writing file \"%s\"\n",inputfile);
		exitPGP(INVALID_FILE_ERROR);
	}
	fprintf(pgpout, LANG("File %s created containing %d random bytes.\n"),
		inputfile, makerandom);
	/* If we aren't encrypting, don't bother trying to decrypt this! */
	if (decrypt_mode)
		exitPGP(EXIT_OK);

	/* This is obviously NOT a text file */
	literal_mode = MODE_BINARY;
    } else {
	if (decrypt_mode && no_extension(inputfile)) {
	    strcpy(cipherfile, inputfile);
	    force_extension(cipherfile, ASC_EXTENSION);
	    if (file_exists(cipherfile)) {
		inputfile = cipherfile;
	    } else {
		force_extension(cipherfile, PGP_EXTENSION);
		if (file_exists(cipherfile)) {
		    inputfile = cipherfile;
		} else {
		    force_extension(cipherfile, SIG_EXTENSION);
		    if (file_exists(cipherfile))
			inputfile = cipherfile;
		}
	    }
	}
	if (!file_exists(inputfile)) {
	    fprintf(pgpout,
		    LANG("\n\007File '%s' does not exist.\n"), inputfile);
	    errorLvl = FILE_NOT_FOUND_ERROR;
	    user_error();
	}
    }

    if (strlen(inputfile) >= (unsigned) MAX_PATH - 4) {
	fprintf(pgpout, 
		LANG("\007Invalid filename: '%s' too long\n"), inputfile);
	errorLvl = INVALID_FILE_ERROR;
	user_error();
    }
    strcpy(plainfile, inputfile);

    if (filter_mode) {
	setoutdir(NULL);	/* NULL means use tmpdir */
    } else {
	if (outputfile)
	    setoutdir(outputfile);
	else
	    setoutdir(inputfile);
    }

    if (filter_mode) {
	workfile = tempfile(TMP_WIPE | TMP_TMPDIR);
	readPhantomInput(workfile);
    } else {
	workfile = inputfile;
    }

    get_header_info_from_file(workfile, &ctb, 1);
    if (decrypt_mode) {
	strip_spaces = FALSE;
	if (!is_ctb(ctb) && is_armor_file(workfile, 0L))
	    do_armorfile(workfile);
	else
	{
          for (ct_repeat=0; ct_repeat<ct_repeat_max; ct_repeat++)
          {
            do_decrypt(workfile);
          }

          
/*	  if (do_decrypt(workfile) < 0)
	    user_error(); */
        }

#ifdef MACTC5
	if (verbose) fprintf(stderr, "Final file = %s.\n", plainfile);
	/* Allow for overide of auto-unmacbin : 205b */
	if( (macbin_flag == FALSE) && is_macbin(plainfile) ) 
		bin2mac(plainfile,TRUE);
	else {
		AddOutputFiles(plainfile);
		PGPSetFinfo(plainfile,FType,FCreator);
	}
	if (use_clipboard) File2Scrap(plainfile);
#endif
	if (batchmode && !signature_checked)
	    exitPGP(1);		/* alternate success, file did not have sig. */
	else {
		xopenme_clock_end(0);
		xopenme_dump_state();
		xopenme_finish();
		exitPGP(EXIT_OK);
	}
    }
    /*
     * See if plaintext input file was actually created by PGP earlier--
     * If it was, maybe we should NOT encapsulate it in a literal packet.
     * (nestflag = TRUE).  Otherwise, always encapsulate it (default).
     * (Why test for filter_mode???)
     */
    if (!batchmode && !filter_mode && legal_ctb(ctb)) {
	/*      Special case--may be a PGP-created packet, so
	   do we inhibit encapsulation in literal packet? */
	fprintf(pgpout,
LANG("\n\007Input file '%s' looks like it may have been created by PGP. "),
		inputfile);
	fprintf(pgpout,
LANG("\nIs it safe to assume that it was created by PGP (y/N)? "));
	nestflag = getyesno('n');
    } else if (force_flag && makerandom == 0 && legal_ctb(ctb)) {
	nestflag = TRUE;
    }

    if (moreflag && makerandom == 0) {
	/* special name to cause printout on decrypt */
	strcpy(literal_file_name, CONSOLE_FILENAME);
	literal_mode = MODE_TEXT;	/* will check for text file later */
    } else {
	strcpy(literal_file_name, file_tail(inputfile));
#ifdef MSDOS
	strlwr(literal_file_name);
#endif
    }
    literal_file = literal_file_name;

    /*      Make sure non-text files are not accidentally converted 
       to canonical text.  This precaution should only be followed 
       for US ASCII text files, since European text files may have 
       8-bit character codes and still be legitimate text files 
       suitable for conversion to canonical (CR/LF-terminated) 
       text format. */
    if (literal_mode == MODE_TEXT && !is_text_file(workfile)) {
	fprintf(pgpout,
LANG("\nNote: '%s' is not a pure text file.\n\
File will be treated as binary data.\n"),
		workfile);
	literal_mode = MODE_BINARY;	/* now expect straight binary */
    }
    if (moreflag && literal_mode == MODE_BINARY) {
	/* For eyes only?  Can't display binary file. */
	fprintf(pgpout,
LANG("\n\007Error: Only text files may be sent as display-only.\n"));
	errorLvl = INVALID_FILE_ERROR;
	user_error();
    }

    /*  
     * See if plainfile looks like it might be incompressible, 
     * by examining its contents for compression headers for 
     * commonly-used compressed file formats like PKZIP, etc.
     * Remember this information for later, when we are deciding
     * whether to attempt compression before encryption.
     *
     * Naturally, don't bother if we are making a separate signature or
     * clear-signed message.  Also, don't bother trying to compress a
     * PGP message, as it's probably already compressed.
     */
    attempt_compression = compress_enabled && !separate_signature &&
                          !nestflag && !clearfile && makerandom == 0 &&
                          file_compressible(plainfile);

#ifdef MACTC5
	if(( macbin_flag == TRUE ) && (nestflag==FALSE)) {
		char *saveworkfile;
		nestflag = false;
		saveworkfile = workfile;
		workfile = tempfile(TMP_WIPE|TMP_TMPDIR);
		if (mac2bin(saveworkfile, workfile)!=0) {
			fprintf(pgpout, LANG("\n\007Error: MacBinary failed!\n"));
			errorLvl = INVALID_FILE_ERROR;
			rmtemp(workfile);
			exitPGP(errorLvl);
		}
	}
#endif
    if (sign_flag) {
	if (!filter_mode && !quietmode)
	    fprintf(pgpout,
LANG("\nA secret key is required to make a signature. "));
	if (!quietmode && my_name[0] == '\0') {
	    fprintf(pgpout,
LANG("\nYou specified no user ID to select your secret key,\n\
so the default user ID and key will be the most recently\n\
added key on your secret keyring.\n"));
	}
	strip_spaces = FALSE;
	clearfile = NULL;
	if (literal_mode == MODE_TEXT) {
	    /* Text mode requires becoming canonical */
	    tempf = tempfile(TMP_WIPE | TMP_TMPDIR);
	    /* +clear means output file with signature in the clear,
	       only in combination with -t and -a, not with -e or -b */
	    if (!encrypt_flag && !separate_signature &&
		emit_radix_64 && clear_signatures) {
		clearfile = workfile;
		strip_spaces = TRUE;
	    }
	    make_canonical(workfile, tempf);
	    if (!clearfile)
		rmtemp(workfile);
	    workfile = tempf;
	}
	if (attempt_compression || encrypt_flag || emit_radix_64 ||
	    output_stdout)
	    tempf = tempfile(TMP_WIPE | TMP_TMPDIR);
	else
	    tempf = tempfile(TMP_WIPE);
	/* for clear signatures we create a separate signature */

  for (ct_repeat=0; ct_repeat<ct_repeat_max; ct_repeat++)
  {
	status = signfile(nestflag, separate_signature || (clearfile != NULL),
		   my_name, workfile, tempf, literal_mode, literal_file);
  }
	rmtemp(workfile);
	workfile = tempf;

	if (status < 0) {	/* signfile failed */
	    fprintf(pgpout, LANG("\007Signature error\n"));
	    errorLvl = SIGNATURE_ERROR;
	    user_error();
	}
    } else if (!nestflag) {	/* !sign_file */
	/*      Prepend CTB_LITERAL byte to plaintext file.
	   --sure wish this pass could be optimized away. */
	if (attempt_compression || encrypt_flag || emit_radix_64 ||
	    output_stdout)
	    tempf = tempfile(TMP_WIPE | TMP_TMPDIR);
	else
	    tempf = tempfile(TMP_WIPE);
	/* for clear signatures we create a separate signature */
	status = make_literal(workfile, tempf, literal_mode, literal_file);
	rmtemp(workfile);
	workfile = tempf;
    }

    if (encrypt_flag) {
        if (emit_radix_64 || output_stdout)
	    tempf = tempfile(TMP_WIPE | TMP_TMPDIR);
	else
	    tempf = tempfile(TMP_WIPE);
	if (!conventional_flag) {
	    if (!filter_mode && !quietmode)
		fprintf(pgpout,
LANG("\n\nRecipients' public key(s) will be used to encrypt. "));
	    if (recipient == NULL || *recipient == NULL ||
		**recipient == '\0') {
		/* no recipient specified on command line */
		fprintf(pgpout,
LANG("\nA user ID is required to select the recipient's public key. "));
		fprintf(pgpout, LANG("\nEnter the recipient's user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter the recipient's user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
		if ((mcguffins = (char **) malloc(2 * sizeof(char *))) == NULL)
		{
		    fprintf(stderr, LANG("\n\007Out of memory.\n"));
		    exitPGP(7);
		}
		mcguffins[0] = mcguffin;
		mcguffins[1] = "";
	    } else {
		/* recipient specified on command line */
		mcguffins = recipient;
	    }
	    for (recipient = mcguffins; *recipient != NULL &&
		 **recipient != '\0'; recipient++) {
		CONVERT_TO_CANONICAL_CHARSET(*recipient);
	    }
	    status = encryptfile(mcguffins, workfile, tempf,
				 attempt_compression);
	} else {
	    status = idea_encryptfile(workfile, tempf, attempt_compression);
	}

	rmtemp(workfile);
	workfile = tempf;

	if (status < 0) {
	    fprintf(pgpout, LANG("\007Encryption error\n"));
	    errorLvl = (conventional_flag ? ENCR_ERROR : RSA_ENCR_ERROR);
	    user_error();
	} 
    } else if (attempt_compression && !separate_signature && !clearfile) {
	/*
	 * PGP used to be parsimonious about compression; originally, it only
	 * did it for files that were being encrypted (to reduce the
	 * redundancy in the plaintext), but it should really do it for
	 * anything where it's not a bad idea.
	 */
        if (emit_radix_64 || output_stdout)
	    tempf = tempfile(TMP_WIPE | TMP_TMPDIR);
	else
	    tempf = tempfile(TMP_WIPE);
	squish_file(workfile, tempf);
	rmtemp(workfile);
	workfile = tempf;
    }

    /*
     * Write to stdout if explicitly asked to, or in filter mode and
     * no explicit file name was given.
     */
    if (output_stdout) {
	if (emit_radix_64) {
	    /* NULL for outputfile means write to stdout */
	    if (armor_file(workfile, NULL, inputfile, clearfile, FALSE) != 0) {
		errorLvl = UNKNOWN_FILE_ERROR;
		user_error();
	    }
	    if (clearfile)
		rmtemp(clearfile);
	} else {
	    if (writePhantomOutput(workfile) < 0) {
		errorLvl = UNKNOWN_FILE_ERROR;
		user_error();
	    }
	}
	rmtemp(workfile);
    } else {
	char name[MAX_PATH];
        char *t;
	if (outputfile) {
	    strcpy(name, outputfile);
	} else {
	    strcpy(name, inputfile);
	    drop_extension(name);
	}
        do {
	    if (!outputfile && no_extension(name)) {
	        if (emit_radix_64)
		    force_extension(name, ASC_EXTENSION);
	        else if (sign_flag && separate_signature)
		    force_extension(name, SIG_EXTENSION);
	        else
		    force_extension(name, PGP_EXTENSION);
#ifdef MACTC5
			if (addresfork) {
				drop_extension(name);
				force_extension(name, ".sdf");
			}
#endif
	    }
            if (!file_exists(name)) break;
            t=ck_dup_output(name, TRUE, !clearfile);
            if (t==NULL) user_error();
            if (clearfile && !strcmp(t,name)) break;
            strcpy(name,t);
        } while (TRUE);
	if (emit_radix_64) {
	    if (armor_file(workfile, name, inputfile, clearfile, FALSE) != 0) {
		errorLvl = UNKNOWN_FILE_ERROR;
		user_error();
	    }
	    if (clearfile)
		rmtemp(clearfile);
	} else {
	    if ((outputfile = savetemp(workfile, name)) == NULL) {
		errorLvl = UNKNOWN_FILE_ERROR;
		user_error();
	    }
	    if (!quietmode) {
		if (encrypt_flag)
		    fprintf(pgpout,
			    LANG("\nCiphertext file: %s\n"), outputfile);
		else if (sign_flag)
		    fprintf(pgpout,
			    LANG("\nSignature file: %s\n"), outputfile);
	    }
	}
#ifdef MACTC5
		AddOutputFiles(name);
		if (addresfork) {
			if(!AddResourceFork(name)) {
				short frefnum,len,i;
				char *p,*q;
				Handle h;
				c2pstr(name);
				q=file_tail(argv[2]);
				len=strlen(q);
				frefnum=OpenResFile((uchar *)name);
				h=NewHandle(len+1);
				HLock(h);
				p=*h;
				*p++=len;
				for (i=0; i<len; i++) *p++=*q++;
				AddResource(h,'STR ',500,(uchar *)"");
				ChangedResource(h);
				WriteResource(h);
				UpdateResFile(frefnum);
				CloseResFile(frefnum);
				p2cstr((uchar *)name);
			} else {
				BailoutAlert("AddResFork failed!");
				exitPGP(-1);
			}
		} 	
		if (binhex_flag) {
			if (binhex(name)) {
				BailoutAlert("BinHex failed!");
				exitPGP(-1);
			}
			remove(name);
		}
		if (use_clipboard) File2Scrap(name);
#endif /* MACTC5 */
    }

    if (wipeflag) {
	/* destroy every trace of plaintext */
	if (wipefile(inputfile) == 0) {
	    remove(inputfile);
	    fprintf(pgpout, LANG("\nFile %s wiped and deleted. "), inputfile);
	    fprintf(pgpout, "\n");
	}
    }

#ifdef MACTC5
	if(!addresfork && !use_clipboard)
		if (!emit_radix_64) PGPSetFinfo(outputfile,'Cryp','MPGP');
#endif

#ifdef XOPENME
  xopenme_clock_end(0);

  xopenme_dump_state();
  xopenme_finish();
#endif

    exitPGP(EXIT_OK);
    return 0;			/* to shut up lint and some compilers */
#ifdef MACTC5
}				/* pgp_dispatch */
#else
}				/* main */
#endif

#ifdef MSDOS
#include <dos.h>
static char *dos_errlst[] =
{
    "Write protect error",	/* LANG ("Write protect error") */
    "Unknown unit",
    "Drive not ready",		/* LANG ("Drive not ready") */
    "3", "4", "5", "6", "7", "8", "9",
    "Write error",		/* LANG ("Write error") */
    "Read error",		/* LANG ("Read error") */
    "General failure",
};

/* handler for msdos 'harderrors' */
#ifndef OS2
#ifdef __TURBOC__		/* Turbo C 2.0 */
static int dostrap(int errval)
#else
static void dostrap(unsigned deverr, unsigned errval)
#endif
{
    char errbuf[64];
    int i;
    sprintf(errbuf, "\r\nDOS error: %s\r\n", dos_errlst[errval]);
    i = 0;
    do
	bdos(2, (unsigned int) errbuf[i], 0);
    while (errbuf[++i]);
#ifdef __TURBOC__
    return 0;			/* ignore (fopen will return NULL) */
#else
    return;
#endif
}
#endif				/* MSDOS */
#endif

static void initsigs()
{
#ifdef MSDOS
#ifndef OS2
#ifdef __TURBOC__
    harderr(dostrap);
#else				/* MSC */
#ifndef __GNUC__		/* DJGPP's not MSC */
    _harderr(dostrap);
#endif
#endif
#endif
#endif				/* MSDOS */
#ifdef SIGINT
    if (signal(SIGINT, SIG_IGN) != SIG_IGN)
	signal(SIGINT, breakHandler);
#if defined(UNIX) || defined(VMS) || defined(ATARI)
#ifndef __PUREC__ /* PureC doesn't recognise all signals */
    if (signal(SIGHUP, SIG_IGN) != SIG_IGN)
	signal(SIGHUP, breakHandler);
    if (signal(SIGQUIT, SIG_IGN) != SIG_IGN)
	signal(SIGQUIT, breakHandler);
#endif
#ifdef UNIX
    signal(SIGPIPE, breakHandler);
#endif
    signal(SIGTERM, breakHandler);
#ifdef MACTC5
	signal(SIGABRT,breakHandler);
	signal(SIGTERM,breakHandler);
#ifndef DEBUG
    signal(SIGTRAP, breakHandler);
    signal(SIGSEGV, breakHandler);
    signal(SIGILL, breakHandler);
#ifdef SIGBUS
    signal(SIGBUS, breakHandler);
#endif
#endif				/* DEBUG */
#endif				/* MACTC5 */
#endif				/* UNIX */
#endif				/* SIGINT */
}				/* initsigs */


static void do_armorfile(char *armorfile)
{
    char *tempf;
    char cipherfile[MAX_PATH];
    long lastpos, linepos = 0;
    int status;
    int success = 0;

    for (;;) {
	/* Handle transport armor stripping */
	tempf = tempfile(0);
	strip_spaces = FALSE;	/* de_armor_file() sets
				   this for clear signature files */
        use_charset_header = TRUE;
        lastpos = linepos;
	status = de_armor_file(armorfile, tempf, &linepos);
	if (status) {
	    fprintf(pgpout,
LANG("\n\007Error: Transport armor stripping failed for file %s\n"),
		    armorfile);
	    errorLvl = INVALID_FILE_ERROR;
	    user_error();	/* Bad file */
	}
	if (keepctx || de_armor_only) {
	    if (outputfile && de_armor_only) {
		if (strcmp(outputfile, "-") == 0) {
		    writePhantomOutput(tempf);
		    rmtemp(tempf);
		    return;
		}
		strcpy(cipherfile, outputfile);
	    } else {
		strcpy(cipherfile, file_tail(armorfile));
		force_extension(cipherfile, PGP_EXTENSION);
	    }
	    if ((tempf = savetemp(tempf, cipherfile)) == NULL) {
		errorLvl = UNKNOWN_FILE_ERROR;
		user_error();
	    }
	    if (!quietmode)
		fprintf(pgpout,
LANG("Stripped transport armor from '%s', producing '%s'.\n"),
			armorfile, tempf);
	    /* -da flag: don't decrypt */

	    if (de_armor_only || do_decrypt(tempf) >= 0)
		++success;
	} else {
	    if (charset_header[0])        /* Check signature with charset from Charset: header */
		checksig_pass = 1;
	    if (do_decrypt(tempf) >= 0)
		++success;
	    rmtemp(tempf);
	    if (charset_header[0]) {
		if (checksig_pass == 2) { /* Sigcheck failed: try again with local charset */
		    tempf = tempfile(0);
		    use_charset_header = FALSE;
		    linepos = lastpos;
		    de_armor_file(armorfile, tempf, &linepos);

		    if (do_decrypt(tempf) >= 0)
		        ++success;
		    rmtemp(tempf);
		}
		checksig_pass = 0;
	    }
	}

	if (!is_armor_file(armorfile, linepos)) {
	    if (!success)	/* print error msg if we didn't
				   decrypt anything */
		user_error();
	    return;
	}
	fprintf(pgpout,
		LANG("\nLooking for next packet in '%s'...\n"), armorfile);
    }
}				/* do_armorfile */

static int do_decrypt(char *cipherfile)
{
    char *outfile = NULL;
    int status, i;
    boolean nested_info = FALSE;
    char ringfile[MAX_PATH];
    byte ctb;
    byte header[8];		/* used to classify file type at the end. */
    char preserved_name[MAX_PATH];
    char *newname;

    /* will be set to the original file name after processing a
       literal packet */
    preserved_name[0] = '\0';

    do {			/* while nested parsable info present */
 	if (nested_info) {

	    rmtemp(cipherfile); 	/* never executed on first pass */
	    cipherfile = outfile;
	}
	if (get_header_info_from_file(cipherfile, &ctb, 1) < 0) {
	    fprintf(pgpout,
LANG("\n\007Can't open ciphertext file '%s'\n"), cipherfile);
	    errorLvl = FILE_NOT_FOUND_ERROR;
	    return -1;
	}
	if (!is_ctb(ctb))	/* not a real CTB -- complain */
	    break;

	if (moreflag)
	    outfile = tempfile(TMP_WIPE | TMP_TMPDIR);
	else
	    outfile = tempfile(TMP_WIPE);

	/* PKE is Public Key Encryption */
	if (is_ctb_type(ctb, CTB_PKE_TYPE)) {

	    if (!quietmode)
		fprintf(pgpout,
LANG("\nFile is encrypted.  Secret key is required to read it. "));

	    /* Decrypt to scratch file since we may have a LITERAL2 */
	    status = decryptfile(cipherfile, outfile);

	    if (status < 0) {	/* error return */
		errorLvl = RSA_DECR_ERROR;
		return -1;
	    }
	    nested_info = (status > 0);

	} else if (is_ctb_type(ctb, CTB_SKE_TYPE)) {

	    if (decrypt_only_flag) {
		/* swap file names instead of just copying the file */
		rmtemp(outfile);
		outfile = cipherfile;
		cipherfile = NULL;
		if (!quietmode)
		    fprintf(pgpout,
LANG("\nThis file has a signature, which will be left in place.\n"));
		nested_info = FALSE;
		break;		/* Do no more */
	    }
	    if (!quietmode && checksig_pass<=1)
		fprintf(pgpout,
LANG("\nFile has signature.  Public key is required to check signature.\n"));

	    status = check_signaturefile(cipherfile, outfile,
					 strip_sig_flag, preserved_name);

	    if (status < 0) {	/* error return */
		errorLvl = SIGNATURE_CHECK_ERROR;
		return -1;
	    }
	    nested_info = (status > 0);

	    if (strcmp(preserved_name, "/dev/null") == 0) {
                rmtemp(outfile);
		fprintf(pgpout, "\n");
		return 0;
	    }
	} else if (is_ctb_type(ctb, CTB_CKE_TYPE)) {

	    /* Conventional Key Encrypted ciphertext. */
	    /* Tell user it's encrypted here, and prompt for
	       password in subroutine. */
	    if (!quietmode)
		fprintf(pgpout, LANG("\nFile is conventionally encrypted.  "));
	    /* Decrypt to scratch file since it may be a LITERAL2 */
	    status = idea_decryptfile(cipherfile, outfile);
	    if (status < 0) {	/* error return */
		errorLvl = DECR_ERROR;
		return -1;	/* error exit status */
	    }
	    nested_info = (status > 0);

	} else if (is_ctb_type(ctb, CTB_COMPRESSED_TYPE)) {

	    /* Compressed text. */
	    status = decompress_file(cipherfile, outfile);
	    if (status < 0) {	/* error return */
		errorLvl = DECOMPRESS_ERROR;
		return -1;
	    }
	    /* Always assume nested information... */
	    nested_info = TRUE;

	} else if (is_ctb_type(ctb, CTB_LITERAL_TYPE) ||
		   is_ctb_type(ctb, CTB_LITERAL2_TYPE)) { /* Raw plaintext.
							     Just copy it.
							     No more nesting.
							   */

	    /* Strip off CTB_LITERAL prefix byte from file: */
	    /* strip_literal may alter plainfile; will set mode */
	    status = strip_literal(cipherfile, outfile,
				   preserved_name, &literal_mode);
	    if (status < 0) {	/* error return */
		errorLvl = UNKNOWN_FILE_ERROR;
		return -1;
	    }
	    nested_info = FALSE;
	} else if (ctb == CTB_CERT_SECKEY || ctb == CTB_CERT_PUBKEY) {

            rmtemp(outfile); 
	    if (decrypt_only_flag) {
		/* swap file names instead of just copying the file */
		outfile = cipherfile;
		cipherfile = NULL;
		nested_info = FALSE;	/* No error */
		break;		/* no further processing */
	    }
	    /* Key ring.  View it. */
	    fprintf(pgpout,
LANG("\nFile contains key(s).  Contents follow..."));
	    if (view_keyring(NULL, cipherfile, TRUE, FALSE) < 0) {
		errorLvl = KEYRING_VIEW_ERROR;
		return -1;
	    }
	    /* filter mode explicit requested with -f */
	    if (filter_mode && !preserve_filename)
		return 0;	/*    No output file */
	    if (batchmode)
		return 0;
	    if (ctb == CTB_CERT_SECKEY)
		strcpy(ringfile, globalSecringName);
	    else
		strcpy(ringfile, globalPubringName);
	    /*      Ask if it should be put on key ring */
	    fprintf(pgpout,
LANG("\nDo you want to add this keyfile to keyring '%s' (y/N)? "), ringfile);
	    if (!getyesno('n'))
		return 0;
	    status = addto_keyring(cipherfile, ringfile);
	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring add error. "));
		errorLvl = KEYRING_ADD_ERROR;
		return -1;
	    }
	    return 0;		/*    No output file */

	} else {		/* Unrecognized CTB */
	    break;
	}

    } while (nested_info);
    /* No more nested parsable information */

    /* Stopped early due to error */
    if (nested_info) {
	fprintf(pgpout,
"\7\nERROR: Nested data has unexpected format.  CTB=0x%02X\n", ctb);
	if (outfile)
	    rmtemp(outfile); 
	if (cipherfile)
	    rmtemp(cipherfile); 
	errorLvl = UNKNOWN_FILE_ERROR;
	return -1;
    }
    if (outfile == NULL) {	/* file was not encrypted */
	if (!filter_mode && !moreflag) {
	    fprintf(pgpout,
LANG("\007\nError: '%s' is not a ciphertext, signature, or key file.\n"),
		    cipherfile);
	    errorLvl = UNKNOWN_FILE_ERROR;
	    return -1;
	}
	outfile = cipherfile;
    } else {
 if (cipherfile) 
	    rmtemp(cipherfile);
    }

    if (moreflag || (strcmp(preserved_name, CONSOLE_FILENAME) == 0)) {
	/* blort to screen */
	if (strcmp(preserved_name, CONSOLE_FILENAME) == 0) {
	    fprintf(pgpout,
LANG("\n\nThis message is marked \"For your eyes only\".  Display now \
(Y/n)? "));
	    if (batchmode
#ifdef UNIX
            || !isatty(fileno(stdout))	/* stdout is redirected! */
#endif
            || filter_mode || !getyesno('y')) {
		/* no -- abort display, and clean up */
                fprintf(pgpout, "\n");
		rmtemp(outfile);
		return 0;
	    }
	}
	if (!quietmode)
	    fprintf(pgpout, LANG("\n\nPlaintext message follows...\n"));
	else
	    putc('\n', pgpout);
	fprintf(pgpout, "------------------------------\n");
	more_file(outfile, strcmp(preserved_name, CONSOLE_FILENAME) == 0);
	/* Disallow saving to disk if outfile is console-only: */
	if (strcmp(preserved_name, CONSOLE_FILENAME) == 0) {
	    clearscreen();	/* remove all evidence */
	} else if (!quietmode && !batchmode) {
	    fprintf(pgpout, LANG("Save this file permanently (y/N)? "));
	    if (getyesno('n')) {
		char moreFilename[256];
		fprintf(pgpout, LANG("Enter filename to save file as: "));
#ifdef AMIGA
                requesterdesc=LANG("Enter filename to save file as: ");
#endif
                if (preserved_name[0]) {
		    fprintf(pgpout, "[%s]: ", file_tail(preserved_name));
#ifdef AMIGA
                    strcat(requesterdesc, "[");
                    strcat(requesterdesc, file_tail(preserved_name));
                    strcat(requesterdesc, "]:");
#endif
                }
#ifdef MACTC5
		if(!GetFilePath(LANG("Enter filename to save file as:"),moreFilename,PUTFILE))
			strcpy(moreFilename,"");
		else
			fprintf(pgpout, "%s\n",moreFilename);
#else
		getstring(moreFilename, 255, TRUE);
#endif
		if (*moreFilename == '\0') {
		    if (*preserved_name != '\0')
			savetemp(outfile, file_tail(preserved_name));
		    else
			rmtemp(outfile);
		} else
		    savetemp(outfile, moreFilename);
		return 0;
	    }
	}
        rmtemp(outfile);
	return 0;
    }				/* blort to screen */
    if (outputfile) {
	if (!strcmp(outputfile, "/dev/null")) {
            rmtemp(outfile);
	    return 0;
	}
	filter_mode = (strcmp(outputfile, "-") == 0);
	strcpy(plainfile, outputfile);
    } else {
#ifdef VMS
	/* VMS null extension has to be ".", not "" */
	force_extension(plainfile, ".");
#else				/* not VMS */
	drop_extension(plainfile);
#endif				/* not VMS */
    }

    if (!preserve_filename && filter_mode) {
	if (writePhantomOutput(outfile) < 0) {
	    errorLvl = UNKNOWN_FILE_ERROR;
	    return -1;
	}
        rmtemp(outfile);
	return 0;
    }
    if (preserve_filename && preserved_name[0] != '\0')
	strcpy(plainfile, file_tail(preserved_name));

    if (quietmode) {
	if (savetemp(outfile, plainfile) == NULL) {
	    errorLvl = UNKNOWN_FILE_ERROR;
	    return -1;
	}
	return 0;
    }
    if (!verbose)	       /* if other filename messages were suppressed */
	fprintf(pgpout, LANG("\nPlaintext filename: %s"), plainfile);


/*---------------------------------------------------------*/

    /*      One last thing-- let's attempt to classify some of the more
       frequently occurring cases of plaintext output files, as an
       aid to the user.

       For example, if output file is a public key, it should have
       the right extension on the filename.

       Also, it will likely be common to encrypt files created by
       various archivers, so they should be renamed with the archiver
       extension.
     */
    get_header_info_from_file(outfile, header, 8);

    newname = NULL;
#ifdef MACTC5
	if (header[0] == CTB_CERT_SECKEY)
		PGPSetFinfo(plainfile,'SKey','MPGP');
#endif
    if (header[0] == CTB_CERT_PUBKEY) {
	/* Special case--may be public key, worth renaming */
#ifdef MACTC5
		PGPSetFinfo(plainfile,'PKey','MPGP');
#endif
	fprintf(pgpout,
LANG("\nPlaintext file '%s' looks like it contains a public key."),
		plainfile);
	newname = maybe_force_extension(plainfile, PGP_EXTENSION);
    }
    /* Possible public key output file */ 
    else if ((i = compressSignature(header)) >= 0) {
	/*      Special case--may be an archived/compressed file,
		worth renaming
	*/
	fprintf(pgpout, LANG("\nPlaintext file '%s' looks like a %s file."),
		plainfile, compressName[i]);
	newname = maybe_force_extension(plainfile, compressExt[i]);
    } else if (is_ctb(header[0]) &&
	       (is_ctb_type(header[0], CTB_PKE_TYPE)
		|| is_ctb_type(header[0], CTB_SKE_TYPE)
		|| is_ctb_type(header[0], CTB_CKE_TYPE))) {
	/* Special case--may be another ciphertext file, worth renaming */
	fprintf(pgpout,
LANG("\n\007Output file '%s' may contain more ciphertext or signature."),
		plainfile);
	newname = maybe_force_extension(plainfile, PGP_EXTENSION);
    }				/* Possible ciphertext output file */
#ifdef MACTC5
	if( (newname = savetemp(outfile, (newname ? newname : plainfile))) == NULL) {
#else
    if (savetemp(outfile, (newname ? newname : plainfile)) == NULL) {
#endif
	errorLvl = UNKNOWN_FILE_ERROR;
	return -1;
    }
#ifdef MACTC5
	else if( strcmp(newname, plainfile) != 0 )	/* 203a */
		strcpy(plainfile, newname);
#endif
    fprintf(pgpout, "\n");
    return 0;
}				/* do_decrypt */

static int do_keyopt(char keychar)
{
    char keyfile[MAX_PATH];
    char ringfile[MAX_PATH];
    char *workfile;
    int status;

    if ((filter_mode || batchmode)
	&& (keychar == 'g' || keychar == 'e' || keychar == 'd'
	    || (keychar == 'r' && sign_flag))) {
	errorLvl = NO_BATCH;
	arg_error();		/* interactive process, no go in batch mode */
    }
    /*
     * If we're not doing anything that uses stdout, produce output there,
     * in case user wants to redirect it.
     */
    if (!filter_mode)
	pgpout = stdout;

    switch (keychar) {

/*-------------------------------------------------------*/
    case 'g':
	{		/*      Key generation
			   Arguments: bitcount, bitcount
			 */
	    char keybits[6], ebits[6], *username = NULL;

	    /* 
	     * Why all this code?
	     * 
	     * Some people may object to PGP insisting on finding the
	     * manual somewhere in the neighborhood to generate a key.
	     * They bristle against this seemingly authoritarian
	     * attitude.  Some people have even modified PGP to defeat
	     * this feature, and redistributed their hotwired version to
	     * others.  That creates problems for me (PRZ).
	     * 
	     * Here is the problem.  Before I added this feature, there
	     * were maimed versions of the PGP distribution package
	     * floating around that lacked the manual.  One of them was
	     * uploaded to Compuserve, and was distributed to countless
	     * users who called me on the phone to ask me why such a
	     * complicated program had no manual.  It spread out to BBS
	     * systems around the country.  And a freeware distributor got
	     * hold of the package from Compuserve and enshrined it on
	     * CD-ROM, distributing thousands of copies without the
	     * manual.  What a mess.
	     * 
	     * Please don't make my life harder by modifying PGP to
	     * disable this feature so that others may redistribute PGP
	     * without the manual.  If you run PGP on a palmtop with no
	     * memory for the manual, is it too much to ask that you type
	     * one little extra word on the command line to do a key
	     * generation, a command that is seldom used by people who
	     * already know how to use PGP?  If you can't stand even this
	     * trivial inconvenience, can you suggest a better method of
	     * reducing PGP's distribution without the manual?
	     * 
	     * PLEASE DO NOT DISABLE THIS CHECK IN THE SOURCE CODE
	     * WITHOUT AT LEAST CALLING PHILIP ZIMMERMANN 
	     * (+1 303 541-0140, or prz@acm.org) TO DISCUSS IT. 
	     */
	    if (!nomanual && manuals_missing()) {
		char const *const *dir;
		fputs(LANG("\a\nError: PGP User's Guide not found.\n\
PGP looked for it in the following directories:\n"), pgpout);
#ifdef MACTC5
		fprintf(pgpout, "\t\"%s\"\n", appPathName);
#else
		for (dir = manual_dirs; *dir; dir++)
		    fprintf(pgpout, "\t\"%s\"\n", *dir);
#endif	/* MACTC5 */
		fputs(
LANG("and the doc subdirectory of each of the above.  Please put a copy of\n\
both volumes of the User's Guide in one of these directories.\n\
\n\
Under NO CIRCUMSTANCES should PGP ever be distributed without the PGP\n\
User's Guide, which is included in the standard distribution package.\n\
If you got a copy of PGP without the manual, please inform whomever you\n\
got it from that this is an incomplete package that should not be\n\
distributed further.\n\
\n\
PGP will not generate a key without finding the User's Guide.\n\
There is a simple way to override this restriction.  See the\n\
PGP User's Guide for details on how to do it.\n\
\n"), pgpout);
		return KEYGEN_ERROR;
	    }
	    if (myArgc > 2)
		strncpy(keybits, myArgv[2], sizeof(keybits) - 1);
	    else
		keybits[0] = '\0';

	    if (myArgc > 3)
		strncpy(ebits, myArgv[3], sizeof(ebits) - 1);
	    else
		ebits[0] = '\0';

	    /* If the -u option is given, use that username */
	    if (u_flag && my_name != NULL && *my_name != '\0')
		username = my_name;

	    /* dokeygen writes the keys out to the key rings... */
	    status = dokeygen(keybits, ebits, username);

	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keygen error. "));
		errorLvl = KEYGEN_ERROR;
	    }
#ifdef MACTC5
		else  {
			strcpy(ringfile, globalPubringName );
			PGPSetFinfo(ringfile,'PKey','MPGP');
			strcpy(ringfile, globalSecringName  );
			PGPSetFinfo(ringfile,'SKey','MPGP');                
		}
#endif              
	    return status;
	}			/* Key generation */

/*-------------------------------------------------------*/
    case 'c':
	{			/*      Key checking
				   Arguments: userid, ringfile
				 */

	    if (myArgc < 3) {	/* Default to all user ID's */
		mcguffin[0] = '\0';
	    } else {
		strcpy(mcguffin, myArgv[2]);
		if (strcmp(mcguffin, "*") == 0)
		    mcguffin[0] = '\0';
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

	    if (myArgc < 4)	/* default key ring filename */
		strcpy(ringfile, globalPubringName);
	    else
		strncpy(ringfile, myArgv[3], sizeof(ringfile) - 1);

	    if ((myArgc < 4 && myArgc > 2)     /* Allow just key file as arg */
		&&has_extension(myArgv[2], PGP_EXTENSION)) {
		strcpy(ringfile, myArgv[2]);
		mcguffin[0] = '\0';
	    }
	    status = dokeycheck(mcguffin, ringfile, CHECK_ALL);

	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring check error.\n"));
		errorLvl = KEYRING_CHECK_ERROR;
	    }
	    if (status >= 0 && mcguffin[0] != '\0')
		return status;	/* just checking a single user,
				   dont do maintenance */

	    if ((status = maint_check(ringfile, 0)) < 0 && status != -7) {
		fprintf(pgpout, LANG("\007Maintenance pass error. "));
		errorLvl = KEYRING_CHECK_ERROR;
	    }
#ifdef MACTC5
		{   
		byte ctb;
		get_header_info_from_file(ringfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
		}
#endif
	    return status == -7 ? 0 : status;
	}			/* Key check */

/*-------------------------------------------------------*/
    case 'm':
	{			/*      Maintenance pass
				   Arguments: ringfile
				 */

	    if (myArgc < 3)	/* default key ring filename */
		strcpy(ringfile, globalPubringName);
	    else
		strcpy(ringfile, myArgv[2]);

#ifdef MSDOS
	    strlwr(ringfile);
#endif
	    if (!file_exists(ringfile))
		default_extension(ringfile, PGP_EXTENSION);

	    if ((status = maint_check(ringfile,
		      MAINT_VERBOSE | (c_flag ? MAINT_CHECK : 0))) < 0) {
		if (status == -7)
		    fprintf(pgpout,
			    LANG("File '%s' is not a public keyring\n"),
			    ringfile);
		fprintf(pgpout, LANG("\007Maintenance pass error. "));
		errorLvl = KEYRING_CHECK_ERROR;
	    }
#ifdef MACTC5
		PGPSetFinfo(ringfile,'PKey','MPGP');
#endif
	    return status;
	}			/* Maintenance pass */

/*-------------------------------------------------------*/
    case 's':
	{			/*      Key signing
				   Arguments: her_id, keyfile
				 */

	    if (myArgc >= 4)
		strncpy(keyfile, myArgv[3], sizeof(keyfile) - 1);
	    else
		strcpy(keyfile, globalPubringName);

	    if (myArgc >= 3) {
		strcpy(mcguffin, myArgv[2]);	/* Userid to sign */
	    } else {
		fprintf(pgpout,
LANG("\nA user ID is required to select the public key you want to sign. "));
		if (batchmode)	/* not interactive, userid
				   must be on command line */
		    return -1;
		fprintf(pgpout, LANG("\nEnter the public key's user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter the public key's user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

	    if (my_name[0] == '\0') {
		fprintf(pgpout,
LANG("\nA secret key is required to make a signature. "));
		fprintf(pgpout,
LANG("\nYou specified no user ID to select your secret key,\n\
so the default user ID and key will be the most recently\n\
added key on your secret keyring.\n"));
	    }
	    status = signkey(mcguffin, my_name, keyfile);

	    if (status >= 0) {
		status = maint_update(keyfile, 0);
		if (status == -7) { /* ringfile is a keyfile or
				       secret keyring */
		    fprintf(pgpout,
			    "Warning: '%s' is not a public keyring\n",
			    keyfile);
		    return 0;
		}
		if (status < 0)
		    fprintf(pgpout, LANG("\007Maintenance pass error. "));
	    }
	    if (status < 0) {
		fprintf(pgpout, LANG("\007Key signature error. "));
		errorLvl = KEY_SIGNATURE_ERROR;
	    }
#ifdef MACTC5
		PGPSetFinfo(keyfile,'PKey','MPGP');
#endif
	    return status;
	}			/* Key signing */


/*-------------------------------------------------------*/
    case 'd':
	{			/*      disable/revoke key
				   Arguments: userid, keyfile
				 */

	    if (myArgc >= 4)
		strncpy(keyfile, myArgv[3], sizeof(keyfile) - 1);
	    else
		strcpy(keyfile, globalPubringName);

	    if (myArgc >= 3) {
		strcpy(mcguffin, myArgv[2]);	/* Userid to sign */
	    } else {
		fprintf(pgpout,
LANG("\nA user ID is required to select the key you want to revoke or \
disable. "));
		fprintf(pgpout, LANG("\nEnter user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

	    status = disable_key(mcguffin, keyfile);

	    if (status >= 0) {
		status = maint_update(keyfile, 0);
		if (status == -7) { /* ringfile is a keyfile or
				       secret keyring */
		    fprintf(pgpout, "Warning: '%s' is not a public keyring\n",
			    keyfile);
		    return 0;
		}
		if (status < 0)
		    fprintf(pgpout, LANG("\007Maintenance pass error. "));
	    }
	    if (status < 0)
		errorLvl = KEY_SIGNATURE_ERROR;
#ifdef MACTC5
		PGPSetFinfo(keyfile,'PKey','MPGP');
#endif
	    return status;
	}			/* Key compromise */

/*-------------------------------------------------------*/
    case 'e':
	{			/*      Key editing
				   Arguments: userid, ringfile
				 */

	    if (myArgc >= 4)
		strncpy(ringfile, myArgv[3], sizeof(ringfile) - 1);
	    else		/* default key ring filename */
		strcpy(ringfile, globalPubringName);

	    if (myArgc >= 3) {
		strcpy(mcguffin, myArgv[2]);	/* Userid to edit */
	    } else {
		fprintf(pgpout,
LANG("\nA user ID is required to select the key you want to edit. "));
		fprintf(pgpout, LANG("\nEnter the key's user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter the key's user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

	    status = dokeyedit(mcguffin, ringfile);

	    if (status >= 0) {
		status = maint_update(ringfile, 0);
		if (status == -7)
		    status = 0;	/* ignore "not a public keyring" error */
		if (status < 0)
		    fprintf(pgpout, LANG("\007Maintenance pass error. "));
	    }
	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring edit error. "));
		errorLvl = KEYRING_EDIT_ERROR;
	    }
#ifdef MACTC5
		{   
		byte ctb;
		get_header_info_from_file(ringfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
		}
#endif
	    return status;
	}			/* Key edit */

/*-------------------------------------------------------*/
    case 'a':
	{			/*      Add key to key ring
				   Arguments: keyfile, ringfile
				 */

	    if (myArgc < 3 && !filter_mode)
		arg_error();

	    if (!filter_mode) {	/* Get the keyfile from args */
		strncpy(keyfile, myArgv[2], sizeof(keyfile) - 1);

#ifdef MSDOS
		strlwr(keyfile);
#endif
		if (!file_exists(keyfile))
		    default_extension(keyfile, PGP_EXTENSION);

		if (!file_exists(keyfile)) {
		    fprintf(pgpout,
			    LANG("\n\007Key file '%s' does not exist.\n"),
			    keyfile);
		    errorLvl = NONEXIST_KEY_ERROR;
		    return -1;
		}
		workfile = keyfile;

	    } else {
		workfile = tempfile(TMP_WIPE | TMP_TMPDIR);
		readPhantomInput(workfile);
	    }

	    if (myArgc < (filter_mode ? 3 : 4)) { /* default key ring
						     filename */
		byte ctb;
		get_header_info_from_file(workfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
		    strcpy(ringfile, globalSecringName);
		else
		    strcpy(ringfile, globalPubringName);
	    } else {
		strncpy(ringfile, myArgv[(filter_mode ? 2 : 3)],
			sizeof(ringfile) - 1);
		default_extension(ringfile, PGP_EXTENSION);
	    }
#ifdef MSDOS
	    strlwr(ringfile);
#endif

	    status = addto_keyring(workfile, ringfile);

	    if (filter_mode)
		rmtemp(workfile);

	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring add error. "));
		errorLvl = KEYRING_ADD_ERROR;
	    }
#ifdef MACTC5
		{   
		byte ctb;
		get_header_info_from_file(ringfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
		}
#endif
	    return status;
	}			/* Add key to key ring */

/*-------------------------------------------------------*/
    case 'x':
	{			/*      Extract key from key ring
				   Arguments: mcguffin, keyfile, ringfile
				 */

	    if (myArgc >= (filter_mode ? 4 : 5)) /* default key ring
						    filename */
		strncpy(ringfile, myArgv[(filter_mode ? 3 : 4)],
			sizeof(ringfile) - 1);
	    else
		strcpy(ringfile, globalPubringName);

	    if (myArgc >= (filter_mode ? 2 : 3)) {
		if (myArgv[2])
		    /* Userid to extract */
		    strcpy(mcguffin, myArgv[2]);
		else
		    strcpy(mcguffin, "");
	    } else {
		fprintf(pgpout,
LANG("\nA user ID is required to select the key you want to extract. "));
		if (batchmode)	/* not interactive, userid
				   must be on command line */
		    return -1;
		fprintf(pgpout, LANG("\nEnter the key's user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter the key's user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

	    if (!filter_mode) {
		if (myArgc >= 4)
		    strncpy(keyfile, myArgv[3], sizeof(keyfile) - 1);
		else
		    keyfile[0] = '\0';

		workfile = keyfile;
	    } else {
		workfile = tempfile(TMP_WIPE | TMP_TMPDIR);
	    }

#ifdef MSDOS
	    strlwr(workfile);
	    strlwr(ringfile);
#endif

	    default_extension(ringfile, PGP_EXTENSION);

	    status = extract_from_keyring(mcguffin, workfile,
					  ringfile, (filter_mode ? FALSE :
						     emit_radix_64));

	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring extract error. "));
		errorLvl = KEYRING_EXTRACT_ERROR;
		if (filter_mode)
		    rmtemp(workfile);
		return status;
	    }
	    if (filter_mode && !status) {
		if (emit_radix_64) {
		    /* NULL for outputfile means write to stdout */
		    if (armor_file(workfile, NULL, NULL, NULL, FALSE) != 0) {
			errorLvl = UNKNOWN_FILE_ERROR;
			return -1;
		    }
		} else {
		    if (writePhantomOutput(workfile) < 0) {
			errorLvl = UNKNOWN_FILE_ERROR;
			return -1;
		    }
		}
		rmtemp(workfile);
	    }
#ifdef MACTC5
		if (status)
			return KEYRING_EXTRACT_ERROR;
		if ((!emit_radix_64)&&(strlen(keyfile)>0)) {
		byte ctb;
		get_header_info_from_file(keyfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
			PGPSetFinfo(ringfile,'PKey','MPGP');
		}
#endif
	    return 0;
	}			/* Extract key from key ring */

/*-------------------------------------------------------*/
    case 'r':
	{	/*      Remove keys or selected key signatures from userid keys
			Arguments: userid, ringfile
		 */

	    if (myArgc >= 4)
		strcpy(ringfile, myArgv[3]);
	    else		/* default key ring filename */
		strcpy(ringfile, globalPubringName);

	    if (myArgc >= 3) {
		strcpy(mcguffin, myArgv[2]);	/* Userid to work on */
	    } else {
		if (sign_flag) {
		    fprintf(pgpout,
LANG("\nA user ID is required to select the public key you want to\n\
remove certifying signatures from. "));
		} else {
		    fprintf(pgpout,
LANG("\nA user ID is required to select the key you want to remove. "));
		}
		if (batchmode)	/* not interactive, userid must be on
				   command line */
		    return -1;
		fprintf(pgpout, LANG("\nEnter the key's user ID: "));
#ifdef AMIGA
                requesterdesc=LANG("\nEnter the key's user ID: ");
#endif
		getstring(mcguffin, 255, TRUE);		/* echo keyboard */
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

#ifdef MSDOS
	    strlwr(ringfile);
#endif
	    if (!file_exists(ringfile))
		default_extension(ringfile, PGP_EXTENSION);

	    if (sign_flag) {	/* Remove signatures */
		if (remove_sigs(mcguffin, ringfile) < 0) {
		    fprintf(pgpout, LANG("\007Key signature remove error. "));
		    errorLvl = KEYSIG_REMOVE_ERROR;
		    return -1;
		}
	    } else {		/* Remove keyring */
#ifdef MACTC5
			if (remove_from_keyring( NULL, mcguffin, ringfile,
					(boolean)!strcmp(ringfile, globalPubringName))) {
#else
		if (remove_from_keyring(NULL, mcguffin, ringfile,
					(boolean) (myArgc < 4)) < 0) {
#endif
		    fprintf(pgpout, LANG("\007Keyring remove error. "));
		    errorLvl = KEYRING_REMOVE_ERROR;
		    return -1;
		}
	    }
#ifdef MACTC5
		{   
		byte ctb;
		get_header_info_from_file(ringfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
		PGPSetFinfo(globalPubringName,'PKey','MPGP');
		}
#endif
	    return 0;
	}			/* remove key signatures from userid */

/*-------------------------------------------------------*/
    case 'v':
    case 'V':			/* -kvv */
	{			/* View or remove key ring entries,
				   with userid match
				   Arguments: userid, ringfile
				 */

	    if (myArgc < 4)	/* default key ring filename */
		strcpy(ringfile, globalPubringName);
	    else
		strcpy(ringfile, myArgv[3]);

	    if (myArgc > 2) {
		strcpy(mcguffin, myArgv[2]);
		if (strcmp(mcguffin, "*") == 0)
		    mcguffin[0] = '\0';
	    } else {
		*mcguffin = '\0';
	    }

	    if ((myArgc == 3) && has_extension(myArgv[2], PGP_EXTENSION)) {
		strcpy(ringfile, myArgv[2]);
		mcguffin[0] = '\0';
	    }
	    CONVERT_TO_CANONICAL_CHARSET(mcguffin);

#ifdef MSDOS
	    strlwr(ringfile);
#endif
	    if (!file_exists(ringfile))
		default_extension(ringfile, PGP_EXTENSION);

	    /* If a second 'v' (keychar = V), show signatures too */
	    status = view_keyring(mcguffin, ringfile,
				  (boolean) (keychar == 'V'), c_flag);
	    if (status < 0) {
		fprintf(pgpout, LANG("\007Keyring view error. "));
		errorLvl = KEYRING_VIEW_ERROR;
	    }
#ifdef MACTC5
		{   
		byte ctb;
		get_header_info_from_file(ringfile, &ctb, 1);
		if (ctb == CTB_CERT_SECKEY)
			PGPSetFinfo(ringfile,'SKey','MPGP');
		else if (ctb == CTB_CERT_PUBKEY)
		PGPSetFinfo(ringfile,'PKey','MPGP');
		}
#endif
	    return status;
	}			/* view key ring entries, with userid match */

    default:
	arg_error();
    }
    return 0;
}				/* do_keyopt */

/* comes here if user made a boo-boo. */
void user_error()
{
    fprintf(pgpout, LANG("\nFor a usage summary, type:  pgp -h\n"));
    fprintf(pgpout,
	    LANG("For more detailed help, consult the PGP User's Guide.\n"));
    exitPGP(errorLvl ? errorLvl : 127);		/* error exit */
}

#if defined(DEBUG) && defined(linux)
#include <malloc.h>
#endif

/*
 * exitPGP: wipes and removes temporary files, also tries to wipe
 * the stack.
 */
void exitPGP(int returnval)
{
    char buf[STACK_WIPE];
    struct hashedpw *hpw;

    if (verbose)
	fprintf(pgpout, "exitPGP: exitcode = %d\n", returnval);
    for (hpw = passwds; hpw; hpw = hpw->next)
	memset(hpw->hash, 0, sizeof(hpw->hash));
    for (hpw = keypasswds; hpw; hpw = hpw->next)
	memset(hpw->hash, 0, sizeof(hpw->hash));
#ifdef MACTC5
	mac_cleanup_tmpf();
#else
    cleanup_tmpf();
#endif
    /* Merge any entropy we collected into the randseed.bin file */
    if (cryptRandOpen((struct IdeaCfbContext *)0) >= 0)
	    cryptRandSave((struct IdeaCfbContext *)0);
#if defined(DEBUG) && defined(linux)
    if (verbose) {
	struct mstats mstat;
	mstat = mstats();
	printf("%d chunks used (%d bytes)  %d bytes total\n",
	       mstat.chunks_used, mstat.bytes_used, mstat.bytes_total);
    }
#endif
    memset(buf, 0, sizeof(buf));	/* wipe stack */
#ifdef VMS
/*
 * Fake VMS style error returns with severity in bottom 3 bits
 */
    if (returnval)
	returnval = (returnval << 3) | 0x10000002;
    else
	returnval = 0x10000001;
#endif				/* VMS */
    exit(returnval);
}

static void arg_error()
{
    signon_msg();
    fprintf(pgpout, LANG("\nInvalid arguments.\n"));
    errorLvl = BAD_ARG_ERROR;
    user_error();
}

/*
 * Check for language specific help files in PGPPATH, then the system
 * directory.  If that fails, check for the default pgp.hlp, again
 * first a private copy, then the system-wide one.
 *
 * System-wide copies currently only exist on Unix.
 */
static void build_helpfile(char *helpfile, char const *extra)
{
    if (strcmp(language, "en")) {
	buildfilename(helpfile, language);
	strcat(helpfile, extra);
	force_extension(helpfile, HLP_EXTENSION);
	if (file_exists(helpfile))
	    return;
#ifdef PGP_SYSTEM_DIR
	strcpy(helpfile, PGP_SYSTEM_DIR);
	strcat(helpfile, language);
	strcat(helpfile, extra);
	force_extension(helpfile, HLP_EXTENSION);
	if (file_exists(helpfile))
	    return;
#endif
    }
    buildfilename(helpfile, "pgp");
    strcat(helpfile, extra);
    force_extension(helpfile, HLP_EXTENSION);
#ifdef PGP_SYSTEM_DIR
    if (file_exists(helpfile))
	return;
    strcpy(helpfile, PGP_SYSTEM_DIR);
    strcat(helpfile, "pgp");
    strcat(helpfile, extra);
    force_extension(helpfile, HLP_EXTENSION);
#endif
}

static void usage()
{
    char helpfile[MAX_PATH];
    char *tmphelp = helpfile;
    extern unsigned char *ext_c_ptr;

    signon_msg();
    build_helpfile(helpfile, "");

    if (ext_c_ptr) {
	/* conversion to external format necessary */
	tmphelp = tempfile(TMP_TMPDIR);
	CONVERSION = EXT_CONV;
	if (copyfiles_by_name(helpfile, tmphelp) < 0) {
	    rmtemp(tmphelp);
	    tmphelp = helpfile;
	}
	CONVERSION = NO_CONV;
    }
    /* built-in help if pgp.hlp is not available */
    if (more_file(tmphelp, FALSE) < 0)
	fprintf(pgpout, LANG("\nUsage summary:\
\nTo encrypt a plaintext file with recipent's public key, type:\
\n   pgp -e textfile her_userid [other userids] (produces textfile.pgp)\
\nTo sign a plaintext file with your secret key:\
\n   pgp -s textfile [-u your_userid]           (produces textfile.pgp)\
\nTo sign a plaintext file with your secret key, and then encrypt it\
\n   with recipent's public key, producing a .pgp file:\
\n   pgp -es textfile her_userid [other userids] [-u your_userid]\
\nTo encrypt with conventional encryption only:\
\n   pgp -c textfile\
\nTo decrypt or check a signature for a ciphertext (.pgp) file:\
\n   pgp ciphertextfile [-o plaintextfile]\
\nTo produce output in ASCII for email, add the -a option to other options.\
\nTo generate your own unique public/secret key pair:  pgp -kg\
\nFor help on other key management functions, type:   pgp -k\n"));
    if (ext_c_ptr)
	rmtemp(tmphelp);
    exit(BAD_ARG_ERROR);	/* error exit */
}

static void key_usage()
{
    char helpfile[MAX_PATH];
    char *tmphelp = helpfile;
    extern unsigned char *ext_c_ptr;

    signon_msg();
    build_helpfile(helpfile, "key");

    if (ext_c_ptr) {
	/* conversion to external format necessary */
	tmphelp = tempfile(TMP_TMPDIR);
	CONVERSION = EXT_CONV;
	if (copyfiles_by_name(helpfile, tmphelp) < 0) {
	    rmtemp(tmphelp);
	    tmphelp = helpfile;
	}
	CONVERSION = NO_CONV;
    }
    /* built-in help if key.hlp is not available */
    if (more_file(tmphelp, FALSE) < 0)
	/* only use built-in help if there is no helpfile */
	fprintf(pgpout, LANG("\nKey management functions:\
\nTo generate your own unique public/secret key pair:\
\n   pgp -kg\
\nTo add a key file's contents to your public or secret key ring:\
\n   pgp -ka keyfile [keyring]\
\nTo remove a key or a user ID from your public or secret key ring:\
\n   pgp -kr userid [keyring]\
\nTo edit your user ID or pass phrase:\
\n   pgp -ke your_userid [keyring]\
\nTo extract (copy) a key from your public or secret key ring:\
\n   pgp -kx userid keyfile [keyring]\
\nTo view the contents of your public key ring:\
\n   pgp -kv[v] [userid] [keyring]\
\nTo check signatures on your public key ring:\
\n   pgp -kc [userid] [keyring]\
\nTo sign someone else's public key on your public key ring:\
\n   pgp -ks her_userid [-u your_userid] [keyring]\
\nTo remove selected signatures from a userid on a keyring:\
\n   pgp -krs userid [keyring]\
\n"));
    if (ext_c_ptr)
	rmtemp(tmphelp);
    exit(BAD_ARG_ERROR);	/* error exit */
}

char **ParseRecipients(char **recipients)
{
	/*
	 * ParseRecipients() expects an array of pointers to
	 * characters, usually the array returned by the C startup
	 * code. Then it will look for entries beginning with the
  	 * string "-@" followed by a filename, which may be appended
 	 * directly or seperated by a blank.
 	 *
 	 * If the file exists and is readable, the routine will load
 	 * the contents and insert it into the command line as if the
 	 * names had been specified there.
 	 *
 	 * Each entry in the file consists of one line. The file line
 	 * will be treated as one argument, no matter whether it
 	 * contains spaces or not. Lines beginning with "#" will be
 	 * ignored and treated as comments. Empty lines will be ignored
 	 * also. Trailing white spaces will be removed.
 	 *
 	 * Currently, ParseRecipients() uses one fixed buffer, meaning,
 	 * that one single line must not be longer than 255 characters.
 	 * The number of included lines is unlimited.
 	 *
 	 * When any kind of problem occurs, PGP will terminate and do
 	 * nothing. No need to test for an error, the result is always
 	 * correct.
 	 *
  	 *             21-Sep-95, Peter Simons <simons@peti.rhein.de>
 	 */

	char **backup = recipients, **new;
	int entrynum;
 	int MAX_RECIPIENTS = 128;   /* The name is somewhat wrong. of
 				     * course the memory handling is
 				     * dynamic.
 				     */

 	/* Check whether we need to do something or not. */
 	while(*recipients) {
 		if (!strncmp(*recipients, INCLUDE_MARK, INCLUDE_MARK_LEN))
 		    break;
 		recipients++;
 	}
 	if (!*recipients)
 	  return backup;	/* nothin' happened */

 	recipients=backup;
 	if (!(new = malloc(MAX_RECIPIENTS * sizeof(char *))))
 	  exitPGP(OUT_OF_MEM);
 	entrynum = 0;

 	while(*recipients) {
 		if (strncmp(*recipients, INCLUDE_MARK, INCLUDE_MARK_LEN))
                {
 			new[entrynum++] = *recipients++;
 			if (entrynum == MAX_RECIPIENTS) {
 				/* Current buffer is too small.
 				 * Use realloc() to largen itt.
 				 */
 				MAX_RECIPIENTS += 128;
 				if (!(new = realloc(new,
                                 MAX_RECIPIENTS * sizeof(char *))))
 				  exitPGP(OUT_OF_MEM);
 			}
 		}
 		else {
 			/* We got a hit! Load the file and parse it. */
 			FILE *fh;
 			char *filename, tempbuf[256];

 			if (strlen(*recipients) == INCLUDE_MARK_LEN)
 			  filename = *++recipients;
 			else
 			  filename = *recipients+INCLUDE_MARK_LEN;
 			fprintf(pgpout, LANG("\nIncluding \"%s\"...\n"), filename);
 			if (!(fh = fopen(filename, "r"))) {
 				perror("PGP");
 				exitPGP(UNKNOWN_FILE_ERROR);
 			}
 			while(fgets(tempbuf, sizeof(tempbuf)-1, fh)) {
 				int i = strlen(tempbuf);

 				/* Test for comments or empty lines. */
 				if (!i || *tempbuf == '#')
 				  continue;

 				/* Remove trailing blanks. */
 				while (isspace(tempbuf[i-1]))
 				  i--;
 				tempbuf[i] = '\0';

 				/* Copy new entry to new */
 				if (!(new[entrynum++] = store_str(tempbuf)))
 				  exitPGP(OUT_OF_MEM);
 				if (entrynum == MAX_RECIPIENTS) {
 					/* Current buffer is too small.
 					 * Use realloc() to largen itt.
 					 */
 					MAX_RECIPIENTS += 128;
 					if (!(new = realloc(new,
                                         MAX_RECIPIENTS * sizeof(char *))))
 					  exitPGP(OUT_OF_MEM);
 				}
 			}
 			if (ferror(fh)) {
 				perror("PGP");
 				exitPGP(UNKNOWN_FILE_ERROR);
 			}
 			fclose(fh);
 			recipients++;
 		}
 	}

 	/*
 	 * We have to write one trailing NULL pointer.
 	 * Check array size first.
 	 */
 	if (entrynum == MAX_RECIPIENTS) {
 		if (!(new = realloc(new, (MAX_RECIPIENTS+1) * sizeof(char *))))
 		  exitPGP(OUT_OF_MEM);
 	}
 	new[entrynum] = NULL;
 	return new;
}


/* ==== random.c ==== */
/*
 * True and cryptographic random number generation.
 *
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Note that while most PGP source modules bear Philip Zimmermann's
 * copyright notice, many of them have been revised or entirely written
 * by contributors who frequently failed to put their names in their
 * code.  Code that has been incorporated into PGP from other authors
 * was either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 *
 * Written by Colin Plumb.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <signal.h>	/* For SIGINT */
#include <time.h>

#ifdef AMIGA      /* Includes for timer -- RKNOP */
#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/exec.h>
#endif /* AMIGA */

#ifdef __PUREC__
#include <ext.h>
#endif

#include "system.h"	/* For ttycbreak, getch, etc. */
#include "idea.h"
#include "md5.h"
#include "noise.h"
#include "language.h"
#include "random.h"
#include "fileio.h"	/* For FOPRBIN */
#include "pgp.h"	/* For globalRandseedName */
#include "randpool.h"
#ifdef MACTC5
#include "Macutil2.h"
#include "Macutil3.h"
#include "TimeManager.h"
#include <unix.h>
#endif

/*
 * As of PGP 2.6.2, the randseed.bin file has been expanded.  An explanation
 * of how the whole thing works in in order, as people are always suspiscious
 * of the random number generator.  (After the xorbytes bug in 2.6, perhaps
 * you should be.)  There are two random number generators in PGP.  One
 * is the "cryptRand" family, which is based on X9.17, but uses IDEA instead
 * of 2-key EDE triple-DES.  This is the generator with a lot of peer review.
 * The implementation is in idea.c.
 * The second is the "trueRand" family, which attempt to accurately measure
 * the entropy available from keyboard I/O.  It keeps a lot more state.
 * The implementation of this is in randpool.c.
 * Originally, the trueRand generator was only used for generating primes,
 * and the cryptRand for generating IDEA session keys.  But things have
 * become a bit more complex.  In particular, the X9.17 specification
 * wants a source of high-resolution time-of-day information, as a source
 * of some "true" randomness to throw in.  So we use the trueRand pool
 * for that.
 * The cryptRand functions keep a state file around, usually named
 * randseed.bin, for a seed, as the X9.17 generator requires 24 bytes of
 * known initial information.
 * This data in this file is carefully "washed" before and after use to
 * help ensure that if the file is captured or altered, the keys will
 * not be too vulnerable.  A washing consists of an encryption in PGP's
 * usual CFB mode of the material coming from or being written to the
 * randseed.bin file on disk.  Assuming the cipher is strong, the effects
 * of the wash are as difficult to predict as the key that is used is
 * difficult to guess.
 * Beforehand, we use the MD5 of the file being encrypted as an additional
 * source of randomness (on the theory that an attacker trying to break
 * a session key probably doesn't have the plaintext, or he wouldn't need
 * to bother), and use that as an IDEA key (with a fixed IV of zero)
 * to encrypt the randseed.bin data.
 * After generating an IDEA key and IV, some more random bytes are generated
 * to reinitialize randseed.bin, and these are encrypted in the same manner
 * as the PGP message before being written to disk, on the assumption that
 * if an attacker can decrypt that, they can decrypt the message directly
 * and not bother attacking the randseed.bin file.
 * The previous code only saved the 24 bytes needed by the X9.17 algorithm.
 * But in 2.6.2, we decided to make the randseed.bin file substantially
 * larger to hold more information that a would-be attacker must guess.
 * There are two reasons for this:
 * - Every time you run PGP, especially when responding to one of PGP's
 *   prompts, PGP samples the keystrokes for use as random numbers.
 *   It is a shame to throw this entropy (randomness) away just because
 *   there is no need for it in the current invocation of PGP.
 * - A feature was added to 2.6.2 to generate files full of random bytes
 *   for other programs to use as key material.  In this case, we haven't
 *   got a message we're encrypting to take some entropy from, and we may
 *   be asked to generate more than 24 random bytes, so there should be
 *   more than 24 bytes of seed material to work from.
 * The implementation is added on to the previous one, to offer assurance
 * that it is no weaker.
 * When the cryptRand generator is opened, the file is washed (if possible)
 * and the first 24 bytes are fed to the cryptographic RNG, while the
 * remainder is added to the trueRand random number pool.
 * When saving, the randseed.bin file is refilled with newly generated
 * bytes, again washed if possible.  It turns out (if you study the
 * X9.17 RNG) that each of the 2^64 possible timestamp information
 * values used in generating each 8 bytes of output generates a output
 * value, so the entropy in the trueRand pool is put to good use; this
 * is not just generating more data from 24 bytes of seed.
 * The random pool is opened and saved with a washing key when
 * generating a session key (see make_random_ideakey in crypto.c),
 * but it is also opened (harmless if alreasy open) and saved
 * (harmless if already saved) without a washing key in the exitPGP routine,
 * to mix in any entropy collected in this invocation of PGP even if
 * a session key was not generated.
 */

/*
 * The new randseed size, big enough to hold the full context of the cryptRand
 * and trueRand generators.  With the current RANDPOOLBITS of 3072 (384 bytes),
 * that's 408 bytes.  It's useless to make it any larger, although if
 * RANDPOOLBITS is increased, it might be an idea to keep this smaller than
 * one disk block on all systems (512 bytes is a good figure to use)
 * so we don't change the space requirements for randseed.bin.
 */
#define RANDSEED_BYTES	(RANDPOOLBITS/8 + 24)
/* Have we read in the randseed.bin file? */
static boolean randSeedOpen = 0;
static struct IdeaRandContext randContext;

/*
 * Load the RNG state from the randseed.bin file on disk.
 * Returns 0 on success, <0 on error.
 *
 * If cfb is non-zero, prewashes the data by encrypting with it.
 */
int
cryptRandOpen(struct IdeaCfbContext *cfb)
{
	byte buf[256];
	int len;
	FILE *f;

	if (randSeedOpen)
		return 0;	/* Already open */

	f = fopen(globalRandseedName, FOPRBIN);
	if (!f)
		return -1;

	/* First get the bare minimum 24 bytes we need for the IDEA RNG */
	len = fread((char *)buf, 1, 24, f);
	if (cfb)
		ideaCfbEncrypt(cfb, buf, buf, 24);
	ideaRandInit(&randContext, buf, buf+16);
	randSeedOpen = TRUE;
	if (len != 24) { /* Error */
		fclose(f);
		return -1;
	}

	/* Read any extra into the random pool */
	for (;;) {
		len = fread((char *)buf, 1, sizeof(buf), f);
		if (len <= 0)
			break;
		if (cfb)
			ideaCfbEncrypt(cfb, buf, buf, len);
		randPoolAddBytes(buf, len);
	}

	fclose(f);
	burn(buf);
#ifdef MACTC5
	PGPSetFinfo(globalRandseedName,'RSed','MPGP');
#endif
	return 0;
}

/* Create a new state from the output of trueRandByte */
void
cryptRandInit(struct IdeaCfbContext *cfb)
{
	byte buf[24];
	int i;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = trueRandByte();
	if (cfb)
		ideaCfbEncrypt(cfb, buf, buf, sizeof(buf));

	ideaRandInit(&randContext, buf, buf+16);
	randSeedOpen = TRUE;
	burn(buf);
}

byte
cryptRandByte(void)
{
	if (!randSeedOpen)
		cryptRandOpen((struct IdeaCfbContext *)0);
	return ideaRandByte(&randContext);
}

/*
 * Write out a file of random bytes.  If cfb is defined, wash it with the
 * cipher.
 */
int
cryptRandWriteFile(char const *name, struct IdeaCfbContext *cfb, unsigned bytes)
{
	byte buf[256];
	FILE *f;
	int i, len;

	f = fopen(name, FOPWBIN);
	if (!f)
		return -1;

	while (bytes) {
		len = (bytes < sizeof(buf)) ? bytes : sizeof(buf);
		for (i = 0; i < len; i++)
			buf[i] = ideaRandByte(&randContext);
		if (cfb)
			ideaCfbEncrypt(cfb, buf, buf, len);
		i = fwrite(buf, 1, len, f);
		if (i < len)
			break;
		bytes -= len;
	}

#ifdef MACTC5
	PGPSetFinfo((char *)name,'RSed','MPGP');
#endif

	return (fclose(f) != 0 || bytes != 0) ? -1 : 0;
}

/*
 * Create a new RNG state, encrypt it with the supplied key, and save it
 * out to disk.
 *
 * When we encrypt a file, the saved data is "postwashed" using the
 * same key and initial vector (including the repeated check bytes and
 * everything) that is used to encrypt the user's message.
 * The hope is that this "postwash" renders it is at least as hard to
 * derive old session keys from randseed.bin as it is to crack the the
 * message directly.
 *
 * The purpose of using EXACTLY the same encryption is to make sure that
 * there isn't related, but different data floating around that can be
 * used for cryptanalysis.
 *
 * This function is always called by exitPGP, without a washing encryption,
 * so this function ignores that call if it has previously been called
 * to save washed bytes.
 */
#ifdef MACTC5
	boolean savedwashed = FALSE;
#endif

void
cryptRandSave(struct IdeaCfbContext *cfb)
{
#ifndef MACTC5
	static boolean savedwashed = FALSE;
#endif

	if (!randSeedOpen)
		return;	/* Do nothing */

	if (cfb)
		savedwashed = TRUE;
	else if (savedwashed)
		return;	/* Don't re-save if it's already been saved washed. */

	(void)cryptRandWriteFile(globalRandseedName, cfb, RANDSEED_BYTES);
#ifdef MACTC5
	PGPSetFinfo(globalRandseedName,'RSed','MPGP');
#endif
}

/*
 * True random bit handling
 */

/*
 * Because these truly random bytes are so unwieldy to accumulate,
 * they can be regarded as a precious resource.  Unfortunately,
 * cryptographic key generation algorithms may require a great many
 * random bytes while searching about for large random prime numbers.
 * Fortunately, they need not all be truly random.  We only need as
 * many truly random bits as there are bits in the large prime we
 * are searching for.  These random bytes can be recycled and modified
 * via pseudorandom numbers until the key is generated, without losing
 * any of the integrity of randomness of the final key.
 *
 * The technique used is a pool of random numbers, which bytes are
 * taken from successively and, when the end is reached, the pool
 * is stirred using an irreversible hash function.  Some (64 bytes)
 * of the pool is not returned to ensure the sequence is not predictible
 * from the values retriefed from trueRandByte().  To be precise,
 * MD5Transform is used as a block cipher in CBC mode, and then the
 * "key" (i.e. what is usually the material to be hashed) is overwritten
 * with some of the just-generated random bytes.
 *
 * This is implemented in randpool.c; see that file for details.
 *
 * An estimate of the number of bits of true (Shannon) entropy in the
 * pool is kept in trueRandBits.  This is incremented when timed
 * keystrokes are available, and decremented when bits are explicitly
 * consumed for some purpose (such as prime generation) or another.
 *
 * trueRandFlush is called to obliterate traces of old random bits after
 * prime generation is completed.  (Primes are the most carefully-guarded
 * values in PGP.)
 */

static unsigned trueRandBits = 0;	/* Bits of entropy in pool */

#ifdef MACTC5
#define CBITS 5
#define TRByt 3
#define TREvt 1

static void perturb(rbits)
int rbits;
{	
	spinner();
	randPoolAddBytes((byte *) seedBuffer, 8);
	trueRandBits +=rbits;
	if (trueRandBits > RANDPOOLBITS)
		trueRandBits = RANDPOOLBITS;
	return;
}
#endif

/* trueRandPending is bits to add to next accumulation request */
static unsigned trueRandPending = 0;

/*
 * Destroys already-used random numbers.  Ensures no sensitive data
 * remains in memory that can be recovered later.  This is called
 * after RSA key generation, so speed is not critical, but security is.
 * RSA key generation takes long enough that interrupts and other
 * tasks are likely to have used a measurable and difficult-to-predict
 * amount of real time, so there is some virtue in sampling the clocks
 * with noise().
 */
void
trueRandFlush(void)
{
	noise();
	randPoolStir();	/* Destroy evidence of what primes were generated */
	randPoolStir();
	randPoolStir();
	randPoolStir();	/* Make *really* certain */
}

/*
 * "Consumes" count bits worth of entropy from the true random pool for some
 * purpose, such as prime generation.
 *
 * Note that something like prime generation can end up calling trueRandByte
 * more often than is implied by the count passed to trueRandClaim; this
 * may happen if the random bit consumer is not perfectly efficient in its
 * use of random bits.  For example, if a search for a suitable prime fails,
 * the easiest thing to do is to get another load of random bits and try
 * again.  It is perfectly acceptable if these bits are correlated with the
 * bits used in the failed attempt, since they are discarded.
 */
void
trueRandConsume(unsigned count)
{
#ifdef MACTC5
	if( trueRandBits >= count )
		trueRandBits -= count;
	else
		trueRandBits = 0;
#else
	assert (trueRandBits >= count);
	trueRandBits -= count;
#endif
}

/*
 * Returns a truly random byte if any are available.  It degenerates to
 * a pseudorandom value if there are none.  It is not an error to call
 * this if none are available.  For example, it is called when generating
 * session keys to add to other sources of cryptographic random numbers.
 *
 * This forces an accumulation if any extra random bytes are pending.
 */
int
trueRandByte(void)
{
	if (trueRandPending)
		trueRandAccum(0);
#ifdef MACTC5
	while( trueRandBits < 8 ) {
		perturb(TRByt);
		spinner();
		}
#endif

	return randPoolGetByte();
}

/*
 * Given an event (typically a keystroke) coded by "event"
 * at a random time, add all randomness to the random pool,
 * compute a (conservative) estimate of the amount, add it
 * to the pool, and return the amount of randomness.
 * (The return value is just for informational purposes.)
 *
 * Double events are okay, but three in a row is considered
 * suspiscous and the randomness is counted as 0.
 */
unsigned
trueRandEvent(int event)
{
	static int event1 = 0, event2 = 0;
	word32 delta;
	unsigned cbits;

	delta = noise();
	randPoolAddBytes((byte *)&event, sizeof(event));

#ifdef MACTC5
	perturb(TRByt);
#endif

	if (event == event1 && event == event2) {
		cbits = 0;
	} else {
		event2 = event1;
		event1 = event;

		for (cbits = 0; delta; cbits++)
			delta >>= 1;

		/* Excessive paranoia? */
#ifdef MACTC5
		if (cbits > CBITS)
			cbits = CBITS;
#else
		if (cbits > 8)
			cbits = 8;
#endif
	}

	trueRandBits += cbits;
	if (trueRandBits > RANDPOOLBITS)
		trueRandBits = RANDPOOLBITS;

	return cbits;
}


/*
 * Since getting random bits from the keyboard requires user attention,
 * we buffer up requests for them until we can do one big request.
 */
void
trueRandAccumLater(unsigned bitcount)
{
	trueRandPending += bitcount;	/* Wow, that was easy! :-) */
#ifdef MACTC5
	spinner();
#endif
}

static void flush_input(void);

#ifdef AMIGA  /* Globals used for timing here and in noise.c - RKNOP 940613 */
struct Library *TimerBase=NULL;
struct timerequest *TimerIO=NULL;
union { struct timeval t;
        struct EClockVal e;
      } time0,time1;
unsigned short use_eclock=0;
#endif /* AMIGA */

/*
 * Performs an accumulation of random bits.  As long as there are fewer bits
 * in the buffer than are needed (the number passed, plus pending bits),
 * prompt for more.
 */
void
trueRandAccum(unsigned count)	/* Get this many random bits ready */
{
	int c;
#if defined(MSDOS) || defined(__MSDOS__)
	time_t timer;
#endif

	count += trueRandPending;	/* Do deferred accumulation now */
	trueRandPending = 0;

	if (count > RANDPOOLBITS)
		count = RANDPOOLBITS;

	if (trueRandBits >= count)
		return;

	fprintf(stderr,
LANG("\nWe need to generate %u random bits.  This is done by measuring the\
\ntime intervals between your keystrokes.  Please enter some random text\
\non your keyboard until you hear the beep:\n"), count-trueRandBits);

	ttycbreak();

#ifdef AMIGA  /* RKNOP 940613 */
        TimerIO=(struct timerequest *)AllocMem(sizeof(struct timerequest),
                                               MEMF_PUBLIC|MEMF_CLEAR);
        if (TimerIO)
        {  if (OpenDevice(TIMERNAME,UNIT_MICROHZ,(struct IORequest *)TimerIO,0))
              TimerBase=NULL;
           else
           {  TimerBase=(struct Library *)TimerIO->tr_node.io_Device;
              if (TimerBase->lib_Version>=36) /* Use E-clock instead */
              {  use_eclock=1;
                 CloseDevice((struct IORequest *)TimerIO);
                 if (!OpenDevice(TIMERNAME,UNIT_ECLOCK,
                                 (struct IORequest *)TimerIO,0))
                    TimerBase=(struct Library *)TimerIO->tr_node.io_Device;
                 else
                    TimerBase=NULL;
              }
              else use_eclock=0;
           }
        }
        else TimerBase=NULL;
#endif /* AMIGA */

	do {
		/* display counter to show progress */
		fprintf(stderr,"\r%4d ", count-trueRandBits);
		fflush(stderr);	/* assure screen update */

		flush_input();	/* If there's no delay, we can't use it */
#ifdef MACTC5
		StartTMCounter();
#endif
		c = getch();	/* always wait for input */
#ifdef MSDOS
		if (c == 3)
			breakHandler(SIGINT);
		if (c == 0)
			c = getch() + 256;
#endif
		/* Print flag indicating acceptance (or not) */
                /* putc a macro, not safe to have function as an arg!! */
                fputc(trueRandEvent(c) ? '.' : '?' , stderr);
#ifdef MACTC5
		StopTMCounter();
#endif
	} while (trueRandBits < count);

	fputs("\r   0 *", stderr);
	fputs(LANG("\007 -Enough, thank you.\n"), stderr);

#if defined(MSDOS) || defined(__MSDOS__)
	/* Wait until one full second has passed without keyboard input */
	do {
		flush_input();
		sleep(1);
	} while (kbhit());
#else
#ifdef AMIGA       /* Added RKNOP 940608 */
        Delay(50*1);    /* dos.library function, wait 1 second */
        if (TimerBase) CloseDevice((struct IORequest *)TimerIO);
        TimerBase=NULL;
        if (TimerIO) FreeMem(TimerIO,sizeof(struct timerequest));
        TimerIO=NULL;
#else
	sleep(1);
	flush_input();
#endif
#endif

	ttynorm();
}

#ifndef EBCDIC                   /* already defined in usuals.h */
#define BS 8
#define LF 10
#define CR 13
#define DEL 127
#endif

#ifdef VMS
int putch(int);
#else
#define putch(c) putc(c, stderr)
#endif

int
getstring(char *strbuf, unsigned maxlen, int echo)
/* Gets string from user, with no control characters allowed.
 * Also accumulates random numbers.
 * maxlen is max length allowed for string.
 * echo is TRUE iff we should echo keyboard to screen.
 * Returns null-terminated string in strbuf.
 */
{
	unsigned i;
	char c;

	ttycbreak();

#ifdef AMIGA     /* In case of -f (use stdio for plaintext input),
                    use ReqTools for input from the user */
        if (!IsInteractive(Input()))
           return AmigaRequestString(strbuf,maxlen,echo);
#endif

	fflush(stdout);
	i=0;
	for (;;) {
#ifndef VMS
		fflush(stderr);
#endif /* VMS */
		c = getch();
		trueRandEvent(c);
#ifdef VMS
		if (c == 25) {  /*  Control-Y detected */
		    ttynorm();
		    breakHandler(SIGINT);
		}
#endif /* VMS */
#if defined(MSDOS) || defined (__MSDOS__)
		if (c == 3)
			breakHandler(SIGINT);
#endif
		if (c==BS || c==DEL) {
			if (i) {
				if (echo) {
					putch(BS);
					putch(' ');
					putch(BS);
				}
				i--;
			} else {
				putch('\007');
			}
			continue;
		}
		if (c < ' ' && c != LF && c != CR) {
			putch('\007');
#if defined(MSDOS) || defined (__MSDOS__)
			if (c == 3)
				breakHandler(SIGINT);	
			if (c == 0)
				getch(); /* Skip extended key codes */
#endif
			continue;
		}
		if (echo)
			putch(c);
		if (c==CR) {
			if (echo)
				putch(LF);
			break;
		}
		if (c==LF)
			break;
		if (c=='\n')
			break;
		strbuf[i++] = c;
		if (i >= maxlen) {
			fputs("\007*\n", stderr);	/* -Enough! */
#if 0
			while (kbhit())
				getch();	/* clean up any typeahead */
#endif
			break;
		}
	}
	strbuf[i] = '\0';	/* null termination of string */

	ttynorm();

	return(i);		/* returns string length */
} /* getstring */


static void
flush_input(void)
{	/* on unix ttycbreak() will flush the input queue */
#if defined(MSDOS) || defined (__MSDOS__) || defined(MACTC5)
	while (kbhit())	/* flush typahead buffer */
		getch();
#endif
}


/* ==== randpool.c ==== */
/*
 * True random number computation and storage
 *
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Note that while most PGP source modules bear Philip Zimmermann's
 * copyright notice, many of them have been revised or entirely written
 * by contributors who frequently failed to put their names in their
 * code.  Code that has been incorporated into PGP from other authors
 * was either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 *
 * Written by Colin Plumb.
 */
#include <stdlib.h>
#include <string.h>

#include "randpool.h"
#include "usuals.h"
#include "md5.h"
#ifdef MACTC5
#include "TimeManager.h"
#endif

/* The pool must be a multiple of the 16-byte (128-bit) MD5 block size */
#define RANDPOOLWORDS ((RANDPOOLBITS+127 & ~127) >> 5)
#if RANDPOOLWORDS <= 16
/* #error is not portable, this has the same effect */
#include "Random pool too small - please increase RANDPOOLBITS in randpool.h"
#endif

/* Must be word-aligned, so make it words.  Cast to bytes as needed. */
static word32 randPool[RANDPOOLWORDS];	/* Random pool */
static unsigned randPoolGetPos = sizeof(randPool); /* Position to get from */
static unsigned randPoolAddPos = 0;	/* Position to add to */

static void
xorbytes(byte *dest, byte const *src, unsigned len)
{
	while (len--)
		*dest++ ^= *src++;
}

/*
 * Destroys already-used random numbers.  Ensures no sensitive data
 * remains in memory that can be recovered later.  This is also
 * called to "stir in" newly acquired environmental noise bits before
 * removing any random bytes.
 *
 * The transformation is carried out by "encrypting" the data in CFB
 * mode with MD5 as the block cipher.  Then, to make certain the stirring
 * operation is strictly one-way, we destroy the key, getting 64 bytes
 * from the beginning of the pool and using them to reinitialize the
 * key.  These bytes are not returned by randPoolGetBytes().
 *
 * The stirring operation is done twice, to ensure that each bit in the
 * pool depends on each bit of entropy XORed in after each call to
 * randPoolStir().
 *
 * To make this useful for pseudo-random (that is, repeatable) operations,
 * the MD5 transformation is always done with a consistent byte order.
 * MD5Transform itself works with 32-bit words, not bytes, so the pool,
 * usually an array of bytes, is transformed into an array of 32-bit words,
 * taking each group of 4 bytes in big-endian order.  At the end of the
 * stirring, the transformation is reversed.
 */
void
randPoolStir(void)
{
	int i;
	byte *p;
	word32 t;
	word32 iv[4];
	static word32 randPoolKey[16] = {0};

	/* Convert to word32s for stirring operation */
	p = (byte *)randPool;
	for (i = 0; i < RANDPOOLWORDS; i++) {
		t = (word32)((unsigned)p[3]<<8 | p[2]) << 16 |
		             (unsigned)p[1]<<8 | p[0];
		randPool[i] = t;
		p += 4;
	}

	/* Start IV from last block of randPool */
	memcpy(iv, randPool+RANDPOOLWORDS-4, sizeof(iv));

	/* First CFB pass */
	for (i = 0; i < RANDPOOLWORDS; i += 4) {
		MD5Transform(iv, randPoolKey);
		iv[0] = randPool[i  ] ^= iv[0];
		iv[1] = randPool[i+1] ^= iv[1];
		iv[2] = randPool[i+2] ^= iv[2];
		iv[3] = randPool[i+3] ^= iv[3];
	}

	/* Get new key */
	memcpy(randPoolKey, randPool, sizeof(randPoolKey));

	/* Second CFB pass */
	for (i = 0; i < RANDPOOLWORDS; i += 4) {
		MD5Transform(iv, randPoolKey);
		iv[0] = randPool[i  ] ^= iv[0];
		iv[1] = randPool[i+1] ^= iv[1];
		iv[2] = randPool[i+2] ^= iv[2];
		iv[3] = randPool[i+3] ^= iv[3];
	}

	/* Get new key */
	memcpy(randPoolKey, randPool, sizeof(randPoolKey));

	/* Wipe iv from memory */
	memset(iv, 0, sizeof(iv));

	/* Convert randPool back to bytes for further use */
	p = (byte *)randPool;
	for (i = 0; i < RANDPOOLWORDS; i++) {
		t = randPool[i];
		p[0] = t>>24;
		p[1] = t>>16;
		p[2] = t>>8;
		p[3] = t;
		p += 4;
	}

	/* Set up pointers for future addition or removal of random bytes */
	randPoolAddPos = 0;
	randPoolGetPos = sizeof(randPoolKey);
#ifdef MACTC5
	spinner();
#endif
}

/*
 * Make a deposit of information (entropy) into the pool.  The bits
 * deposited need not have any particular distribution; the stirring
 * operation transformes them to uniformly-distributed bits.
 */
void
randPoolAddBytes(byte const *buf, unsigned len)
{
	unsigned t;

	while (len > (t = sizeof(randPool) - randPoolAddPos)) {
		xorbytes((byte *)randPool+randPoolAddPos, buf, t);
		buf += t;
		len -= t;
		randPoolStir();
	}

	if (len) {
		xorbytes((byte *)randPool+randPoolAddPos, buf, len);
		randPoolAddPos += len;
		randPoolGetPos = sizeof(randPool); /* Force stir on get */
	}
}

/*
 * Withdraw some bits from the pool.  Regardless of the distribution of the
 * input bits, the bits returned are uniformly distributed, although they
 * cannot, of course, contain more Shannon entropy than the input bits.
 */
void
randPoolGetBytes(byte *buf, unsigned len)
{
	unsigned t;

	while (len > (t = sizeof(randPool) - randPoolGetPos)) {
		memcpy(buf, (byte *)randPool+randPoolGetPos, t);
		buf += t;
		len -= t;
		randPoolStir();
	}

#ifdef MACTC5
	spinner();
#endif

	if (len) {
		memcpy(buf, (byte *)randPool+randPoolGetPos, len);
		randPoolGetPos += len;
	}
}

byte
randPoolGetByte(void)
{
	if (randPoolGetPos == sizeof(randPool))
		randPoolStir();

#ifdef MACTC5
	spinner();
#endif

	return (((byte *)randPool)[randPoolGetPos++]);
}


/* ==== rsagen.c ==== */
/*	rsagen.c - C source code for RSA public-key key generation routines.
	First version 17 Mar 87

        (c) Copyright 1987 by Philip Zimmermann.  All rights reserved.
        The author assumes no liability for damages resulting from the use
        of this software, even if the damage results from defects in this
        software.  No warranty is expressed or implied.

	RSA-specific routines follow.  These are the only functions that 
	are specific to the RSA public key cryptosystem.  The other
	multiprecision integer math functions may be used for non-RSA
	applications.  Without these functions that follow, the rest of 
	the software cannot perform the RSA public key algorithm.  

	The RSA public key cryptosystem is patented by the Massachusetts
	Institute of Technology (U.S. patent #4,405,829).  This patent does
	not apply outside the USA.  Public Key Partners (PKP) holds the
	exclusive commercial license to sell and sub-license the RSA public
	key cryptosystem.  The author of this software implementation of the
	RSA algorithm is providing this software for educational use only. 
	Licensing this algorithm from PKP is the responsibility of you, the
	user, not Philip Zimmermann, the author of this software.  The author
	assumes no liability for any breach of patent law resulting from the
	unlicensed use of this software by the user.
*/

#include "mpilib.h"
#include "genprime.h"
#include "rsagen.h"
#include "random.h"
#include "rsaglue.h"

#ifdef MACTC5
extern DialogPtr ProgressDialog;
#endif

static void derive_rsakeys(unitptr n,unitptr e,unitptr d,
	unitptr p,unitptr q,unitptr u,short ebits);
	/* Given primes p and q, derive RSA key components n, e, d, and u. */


/* Define some error status returns for RSA keygen... */
#define KEYFAILED -15		/* key failed final test */


#define swap(p,q)  { unitptr t; t = p;  p = q;  q = t; }


static void derive_rsakeys(unitptr n, unitptr e, unitptr d,
	unitptr p, unitptr q, unitptr u, short ebits)
/*
 * Given primes p and q, derive RSA key components n, e, d, and u. 
 * The global_precision must have already been set large enough for n.
 * Note that p must be < q.
 * Primes p and q must have been previously generated elsewhere.
 * The bit precision of e will be >= ebits.  The search for a usable
 * exponent e will begin with an ebits-sized number.  The recommended 
 * value for ebits is 5, for efficiency's sake.  This could yield 
 * an e as small as 17.
 */
{
	unit F[MAX_UNIT_PRECISION];
	unitptr ptemp, qtemp, phi, G; 	/* scratchpads */

	/*	For strong prime generation only, latitude is the amount 
		which the modulus may differ from the desired bit precision.  
		It must be big enough to allow primes to be generated by 
		goodprime reasonably fast. 
	*/
#define latitude(bits) (max(min(bits/16,12),4))	/* between 4 and 12 bits */
	
	ptemp = d;	/* use d for temporary scratchpad array */
	qtemp = u;	/* use u for temporary scratchpad array */
	phi = n;	/* use n for temporary scratchpad array */
	G = F;		/* use F for both G and F */
	
	if (mp_compare(p,q) >= 0)	/* ensure that p<q for computing u */
		swap(p,q);		/* swap the pointers p and q */

	/*	phi(n) is the Euler totient function of n, or the number of
		positive integers less than n that are relatively prime to n.
		G is the number of "spare key sets" for a given modulus n. 
		The smaller G is, the better.  The smallest G can get is 2. 
	*/
	mp_move(ptemp,p); mp_move(qtemp,q);
	mp_dec(ptemp); mp_dec(qtemp);
	mp_mult(phi,ptemp,qtemp);	/*  phi(n) = (p-1)*(q-1)  */
	mp_gcd(G,ptemp,qtemp);		/*  G(n) = gcd(p-1,q-1)  */
#ifdef DEBUG
	if (countbits(G) > 12)		/* G shouldn't get really big. */
		mp_display("\007G = ",G);	/* Worry the user. */
#endif /* DEBUG */
	mp_udiv(ptemp,qtemp,phi,G);	/* F(n) = phi(n)/G(n)  */
	mp_move(F,qtemp);

	/*
	 * We now have phi and F.  Next, compute e...
	 * Strictly speaking, we might get slightly faster results by
	 * testing all small prime e's greater than 2 until we hit a 
	 * good e.  But we can do just about as well by testing all 
	 * odd e's greater than 2.
	 * We could begin searching for a candidate e anywhere, perhaps
	 * using a random 16-bit starting point value for e, or even
	 * larger values.  But the most efficient value for e would be 3, 
	 * if it satisfied the gcd test with phi.
	 * Parameter ebits specifies the number of significant bits e
	 * should have to begin search for a workable e.
	 * Make e at least 2 bits long, and no longer than one bit 
	 * shorter than the length of phi.
	 */
	ebits = min(ebits,countbits(phi)-1);
	if (ebits==0) ebits=5;	/* default is 5 bits long */
	ebits = max(ebits,2);
	mp_init(e,0);
	mp_setbit(e,ebits-1);
	lsunit(e) |= 1;		/* set e candidate's lsb - make it odd */
	mp_dec(e);  mp_dec(e); /* precompensate for preincrements of e */
	do {
		mp_inc(e); mp_inc(e);	/* try odd e's until we get it. */
		mp_gcd(ptemp,e,phi);    /* look for e such that
					   gcd(e,phi(n)) = 1 */
	} while (testne(ptemp,1));

	/*	Now we have e.  Next, compute d, then u, then n.
		d is the multiplicative inverse of e, mod F(n).
		u is the multiplicative inverse of p, mod q, if p<q.
		n is the public modulus p*q.
	*/
	mp_inv(d,e,F);		/* compute d such that (e*d) mod F(n) = 1 */
	mp_inv(u,p,q);			/* (p*u) mod q = 1, assuming p<q */
	mp_mult(n,p,q);	/*  n = p*q  */
	mp_burn(F);		/* burn the evidence on the stack */
}	/* derive_rsakeys */


int rsa_keygen(unitptr n, unitptr e, unitptr d,
	unitptr p, unitptr q, unitptr u, short keybits, short ebits)
/*
 * Generate RSA key components p, q, n, e, d, and u. 
 * This routine sets the global_precision appropriate for n,
 * where keybits is desired precision of modulus n.
 * The precision of exponent e will be >= ebits.
 * It will generate a p that is < q.
 * Returns 0 for succcessful keygen, negative status otherwise.
 */
{
	short pbits, qbits;
	boolean too_close_together; /* TRUE iff p and q are too close */
	int status;
	int slop;

	/*
	 * Don't let keybits get any smaller than 2 units, because	
	 * some parts of the math package require at least 2 units 
	 * for global_precision.
	 * Nor any smaller than the 32 bits of preblocking overhead.
	 * Nor any bigger than MAX_BIT_PRECISION - SLOP_BITS.
	 * Also, if generating "strong" primes, don't let keybits get
	 * any smaller than 64 bits, because of the search latitude.
	 */
	slop = max(SLOP_BITS,1); /* allow at least 1 slop bit for sign bit */
	keybits = min(keybits,(MAX_BIT_PRECISION-slop));
	keybits = max(keybits,UNITSIZE*2);
	keybits = max(keybits,32); /* minimum preblocking overhead */
#ifdef STRONGPRIMES
	keybits = max(keybits,64); /* for strong prime search latitude */
#endif	/* STRONGPRIMES */

	set_precision(bits2units(keybits + slop));

	/*	We will need a series of truly random bits to generate the 
		primes.  We need enough random bits for keybits, plus two 
		random units for combined discarded bit losses in randombits. 
		Since we now know how many random bits we will need,
		this is the place to prefill the pool of random bits. 
	*/
	trueRandAccum(keybits+2*UNITSIZE);

#if 0
	/*
	 * If you want primes of different lengths ("separation" bits apart),
	 * do the following:
	 */
	pbits = (keybits-separation)/2;
	qbits = keybits - pbits;
#else
	pbits = keybits/2;
	qbits = keybits - pbits;
#endif

	trueRandConsume(pbits); /* "use up" this many bits */

#ifdef MACTC5
	ShowWindow(ProgressDialog);
	DrawDialog(ProgressDialog);
#endif

#ifdef STRONGPRIMES	/* make a good strong prime for the key */
	status = goodprime(p,pbits,pbits-latitude(pbits));
#else	/* just any random prime will suffice for the key */
	status = randomprime(p,pbits);
#endif	/* else not STRONGPRIMES */
	if (status < 0) 
		return(status);	/* failed to find a suitable prime */

	/* We now have prime p.  Now generate q such that q>p... */

	qbits = keybits - countbits(p);

	trueRandConsume(qbits); /* "use up" this many bits */
	/*	This load of random bits will be stirred and recycled until 
		a good q is generated. */

	do {	/* Generate a q until we get one that isn't too close to p. */
#ifdef STRONGPRIMES	/* make a good strong prime for the key */
		status = goodprime(q,qbits,qbits-latitude(qbits));
#else	/* just any random prime will suffice for the key */
		status = randomprime(q,qbits);
#endif	/* else not STRONGPRIMES */
		if (status < 0) 
			return(status);	/* failed to find a suitable prime */

		/* Note that at this point we can't be sure that q>p. */
		if (mp_compare(p,q) >= 0) { /* ensure that p<q
					       for computing u */
			mp_move(u,p);
			mp_move(p,q);
			mp_move(q,u);
		}
		/* See if p and q are far enough apart.  Is q-p big enough? */
		mp_move(u,q);	/* use u as scratchpad */
		mp_sub(u,p);	/* compute q-p */
		too_close_together = (countbits(u) < (countbits(q)-7));

		/* Keep trying q's until we get one far enough from p... */
	} while (too_close_together);

	derive_rsakeys(n,e,d,p,q,u,ebits);
	trueRandFlush();	/* ensure recycled random pool is destroyed */

	/* Now test key just to make sure --this had better work! */
	{
		unit C[MAX_UNIT_PRECISION];
		int i;

/* Create a dummy signature */
		for (i = 0; i < 16; i++)
			((byte *)C)[i] = 3*i+7;
/* Encrypt it */
		status = rsa_private_encrypt(C,(byte *)C,16,e,d,p,q,u,n);
		if (status < 0)	/* modexp error? */
			return status;	/* return error status */
/* Extract the signature */
		status = rsa_public_decrypt((byte *)C,C,e,n);
		if (status < 0)	/* modexp error? */
			return status;	/* return error status */
/* Verify that we got the same thing back. */
		if (status != 16)
			return KEYFAILED;
		for (i = 0; i < 16; i++)
			if (((byte *)C)[i] != 3*i+7)
				return KEYFAILED;
	}
	return 0;	/* normal return */
}	/* rsa_keygen */

/****************** End of RSA-specific routines  *******************/




/* ==== rsaglue1.c ==== */
/*
 * rsaglue1.c - These functions wrap and unwrap message digests (MDs) and
 * data encryption keys (DEKs) in padding and RSA-encrypt them into
 * multi-precision integers.  This layer of abstraction was introduced
 * to allow the transparent use of either the RSAREF Cryptographic
 * Toolkit from RSA Data Security Inc for the RSA calculations (where
 * the RSA patent applies), or, Philip Zimmermann's mpi library for the
 * RSA calculations.  The rsaglue.c module from PGP version 2.3a performs
 * the same functions as this module, but can be compiled to select the
 * use of mpilib functions instead of RSAREF as the underlying math engine.
 * That version of rsaglue.c would be suitable where the RSA patent does
 * not apply, such as Canada.
 *
 * This file uses MPILIB to perform the actual encryption and decryption.
 * It uses the same PKCS format as RSAREF, although it also accepts an older
 * format used in PGP 2.1.
 *
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Note that while most PGP source modules bear Philip Zimmermann's
 * copyright notice, many of them have been revised or entirely written
 * by contributors who frequently failed to put their names in their
 * code.  Code that has been incorporated into PGP from other authors
 * was either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 */

#include <string.h> 	/* for mem*() */
#include "mpilib.h"
#include "mpiio.h"
#include "pgp.h"
#include "rsaglue.h"
#include "random.h"	/* for cryptRandByte() */

/* No RSADSI credit for MPI version */
char signon_legalese[] = "";

/* These functions hide all the internal details of RSA-encrypted
 * keys and digests.  They owe a lot of their heritage to
 * the preblock() and postunblock() routines in mpiio.c.
 */

/* Abstract Syntax Notation One (ASN.1) Distinguished Encoding Rules (DER)
   encoding for RSA/MD5, used in PKCS-format signatures. */
static byte asn_array[] = {	/* PKCS 01 block type 01 data */
	0x30,0x20,0x30,0x0c,0x06,0x08,0x2a,0x86,0x48,0x86,0xf7,0x0d,
	0x02,0x05,0x05,0x00,0x04,0x10 };
/* This many bytes from the end, there's a zero byte */
#define ASN_ZERO_END 3

int
rsa_public_encrypt(unitptr outbuf, byteptr inbuf, short bytes,
	 unitptr E, unitptr N)
/* Encrypt a DEK with a public key.  Returns 0 on success.
 * <0 means there was an error.
 * -1: Generic error
 * -3: Key too big
 * -4: Key too small
 */
{
	unit temp[MAX_UNIT_PRECISION];
	unsigned int blocksize;
	int i;	/* Temporary, and holds error codes */
	byte *p = (byte *)temp;

	/*
	 * We are building the mpi in place, except for a possible
	 * byte-order swap to little-endian at the end.  Thus, we
	 * need to fill the buffer with leading 0's in the unused
	 * most significant byte positions.
	 */
	blocksize = countbytes(N) - 1;	/* Bytes available for user data */
	for (i = units2bytes(global_precision) - blocksize; i > 0; --i)
		*p++ = 0;
	/*
	 * Both the PKCS and PGP 2.0 key formats add a type byte, and a
	 * a framing byte of 0 to the user data.  The remaining space
	 * is filled with random padding.  (PKCS requires that there be
	 * at least 1 byte of padding.)
	 */
	i = blocksize - 2 - bytes;

	if (i < 1)		/* Less than minimum padding? */
		return -4;
	*p++ = CK_ENCRYPTED_BYTE;	/* Type byte */
	while (i)			/* Non-zero random padding */
		if ((*p = cryptRandByte()))
			++p, --i;
	*p++ = 0;			/* Framing byte */
	memcpy(p, inbuf, bytes);	/* User data */

	mp_convert_order((byte *)temp);		/* Convert buffer to MPI */
	i = mp_modexp(outbuf, temp, E, N);	/* Do the encryption */
	if (i < 0)
		i = -1;

Cleanup:
	mp_burn(temp);
	return i < 0 ? i : 0;
} /* rsa_public_encrypt */

int
rsa_private_encrypt(unitptr outbuf, byteptr inbuf, short bytes,
	 unitptr E, unitptr D, unitptr P, unitptr Q, unitptr U, unitptr N)
/* Encrypt a message digest with a private key.
 * Returns <0 on error:
 * -1: generic error
 * -4: Key too big
 * -5: Key too small
 */
{
	unit temp[MAX_UNIT_PRECISION];
	unit DP[MAX_UNIT_PRECISION], DQ[MAX_UNIT_PRECISION];
	byte *p;
	int i;
	unsigned int blocksize;

	/* PGP doesn't store these coefficents, so we need to compute them. */
	mp_move(temp,P);
	mp_dec(temp);
	mp_mod(DP,D,temp);
	mp_move(temp,Q);
	mp_dec(temp);
	mp_mod(DQ,D,temp);

	p = (byte *)temp;


	/* We are building the mpi in place, except for a possible
	 * byte-order swap to little-endian at the end.  Thus, we
	 * need to fill the buffer with leading 0's in the unused
	 * most significant byte positions.
	 */
	blocksize = countbytes(N) - 1;	/* Space available for data */
	for (i = units2bytes(global_precision) - blocksize; i > 0; --i)
		*p++ = 0;

	i = blocksize - 2 - bytes;		/* Padding needed */
	i -= sizeof(asn_array);		/* Space for type encoding */
	if (i < 0) {
		i = -4;			/* Error code */
		goto Cleanup;
	}
	*p++ = MD_ENCRYPTED_BYTE;	/* Type byte */
	memset(p, ~0, i);		/* All 1's padding */
	p += i;
	*p++ = 0;			/* Zero framing byte */
	memcpy(p, asn_array, sizeof(asn_array)); /* ASN data */
	p += sizeof(asn_array);
	memcpy(p, inbuf, bytes);	/* User data */

	mp_convert_order((byte *)temp);
	i = mp_modexp_crt(outbuf, temp, P, Q, DP, DQ, U);	/* Encrypt */
	if (i < 0)
		i = -1;

Cleanup:
	burn(temp);

	return i;
} /* rsa_private_encrypt */

/* Remove a signature packet from an MPI */
/* Thus, we expect constant padding and the MIC ASN sequence */
int
rsa_public_decrypt(byteptr outbuf, unitptr inbuf,
	unitptr E, unitptr N)
/* Decrypt a message digest using a public key.  Returns the number of bytes
 * extracted, or <0 on error.
 * -1: Corrupted packet.
 * -3: Key too big
 * -4: Key too small
 * -5: Maybe malformed RSA packet
 * -7: Unknown conventional algorithm
 * -9: Malformed RSA packet
 */
{
	unit temp[MAX_UNIT_PRECISION];
	unsigned int blocksize;
	int i;
	byte *front, *back;

	i = mp_modexp(temp, inbuf, E, N);
	if (i < 0) {
		mp_burn(temp);
		return -1;
	}
	mp_convert_order((byte *)temp);
	blocksize = countbytes(N) - 1;
	front = (byte *)temp;			/* Points to start of block */
	i = units2bytes(global_precision);
	back = front + i;			/* Points to end of block */
	i -= countbytes(N) - 1;			/* Expected leading 0's */

	/*
	 * Strip off the padding.  This handles both PKCS and PGP 2.0
	 * formats.  If we're using RSAREF2, we use the padding-removal
	 * code in RSAPublicDecrypt, which accepts only PKCS style.
	 * Oh, well.
	 */

	if (i < 0)				/* This shouldn't happen */
		goto ErrorReturn;
	while (i--)				/* Extra bytes should be 0 */
		if (*front++)
			goto ErrorReturn;

	/* How to distinguish old PGP from PKCS formats.
	 * The old PGP format ends in a trailing type byte, with
	 * all 1's padding before that.  The PKCS format ends in
	 * 16 bytes of message digest, preceded by an ASN string
	 * which is not all 1's.
	 */
	if (back[-1] == MD_ENCRYPTED_BYTE &&
	    back[-17] == 0xff && back[-18] == 0xff) {
		/* Old PGP format: Padding is at the end */
		if (*--back != MD_ENCRYPTED_BYTE)
			goto ErrorReturn;
		if (*front++ != MD5_ALGORITHM_BYTE) {
			mp_burn(temp);
			return -7;
		}
		while (*--back == 0xff)	/* Skip constant padding */
			;
		if (*back)		/* It should end with a zero */
			goto ErrorReturn;
	} else {
		/* PKCS format: padding at the beginning */
		if (*front++ != MD_ENCRYPTED_BYTE)
			goto ErrorReturn;
		while (*front++ == 0xff) /* Skip constant padding */
			;
		if (front[-1])	/* First non-FF byte should be 0 */
			goto ErrorReturn;
		/* Then comes the ASN header */
		if (memcmp(front, asn_array, sizeof(asn_array))) {
			mp_burn(temp);
			return -7;
		}
		front += sizeof(asn_array);
	}

/* We're done - copy user data to outbuf */
	if (back < front)
		goto ErrorReturn;
	blocksize = back-front;
	memcpy(outbuf, front, blocksize);
	mp_burn(temp);
	return blocksize;
ErrorReturn:
	mp_burn(temp);
	return -9;
} /* rsa_public_decrypt */

/* We expect to find random padding and an encryption key */
int
rsa_private_decrypt(byteptr outbuf, unitptr inbuf,
	 unitptr E, unitptr D, unitptr P, unitptr Q, unitptr U, unitptr N)
/* Decrypt an encryption key using a private key.  Returns the number of bytes
 * extracted, or <0 on error.
 * -1: Generic error
 * -3: Key too big
 * -4: Key too small
 * -5: Maybe malformed RSA
 * -7: Unknown conventional algorithm
 * -9: Malformed RSA packet
 */
{
	byte *back;
	byte *front;
	unsigned int blocksize;
	unit temp[MAX_UNIT_PRECISION];
	unit DP[MAX_UNIT_PRECISION], DQ[MAX_UNIT_PRECISION];
	int i;

	/* PGP doesn't store (d mod p-1) and (d mod q-1), so compute 'em */
	mp_move(temp,P);
	mp_dec(temp);
	mp_mod(DP,D,temp);
	mp_move(temp,Q);
	mp_dec(temp);
	mp_mod(DQ,D,temp);

	i = mp_modexp_crt(temp, inbuf, P, Q, DP, DQ, U);
	mp_burn(DP);
	mp_burn(DQ);
	if (i < 0) {
		mp_burn(temp);
		return -1;
	}
	mp_convert_order((byte *)temp);
	front = (byte *)temp;			/* Start of block */
	i = units2bytes(global_precision);
	back = (byte *)front + i;		/* End of block */
	blocksize = countbytes(N) - 1;
	i -= blocksize;				/* Expected # of leading 0's */

	if (i < 0)				/* This shouldn't happen */
		goto Corrupted;
	while (i--)				/* Extra bytes should be 0 */
		if (*front++)
			goto Corrupted;

	/* How to distinguish old PGP from PKCS formats.
	 * PGP packets have a trailing type byte (CK_ENCRYPTED_BYTE),
	 * while PKCS formats have it leading.
	 */
	if (front[0] != CK_ENCRYPTED_BYTE && back[-1] == CK_ENCRYPTED_BYTE) {
		/* PGP 2.0 format  - padding at the end */
		if (back[-1] != CK_ENCRYPTED_BYTE)
			goto Corrupted;
		while (*--back)	/* Skip non-zero random padding */
			;
	} else {
		/* PKCS format - padding at the beginning */
		if (*front++ != CK_ENCRYPTED_BYTE)
			goto Corrupted;
		while (*front++)	/* Skip non-zero random padding */
			;
	}
	if (back <= front)
		goto Corrupted;
	blocksize = back-front;

	memcpy(outbuf, front, blocksize);
	mp_burn(temp);
	return blocksize;

Corrupted:
	mp_burn(temp);
	return -9;
} /* rsa_private_decrypt */


/* ==== sleep.c ==== */
/*************
* sleep.c -- provide unix style sleep function
*
*************/
#include <time.h>

int sleep(unsigned secs){
	long	start;
	long	check;
	long	finish;

	time(&start);
	finish = start + (long) secs;
#ifdef DEBUG
	printf ("sleep for %d secs, stop sleeping at %ld\n", secs, finish);
	time(&check);
	printf ("it is now %ld\n", check );
#endif
	for (;;) {
		time(&check);
		if (check > finish)
			break;
	}
	return (0);
}


/* ==== system.c ==== */
/*
 * system.c
 *
 * Routines specific for non-MSDOS implementations of pgp.
 * 
 * (c) Copyright 1990-1996 by Philip Zimmermann.  All rights reserved.
 * The author assumes no liability for damages resulting from the use
 * of this software, even if the damage results from defects in this
 * software.  No warranty is expressed or implied.
 *
 * Note that while most PGP source modules bear Philip Zimmermann's
 * copyright notice, many of them have been revised or entirely written
 * by contributors who frequently failed to put their names in their
 * code.  Code that has been incorporated into PGP from other authors
 * was either originally published in the public domain or is used with
 * permission from the various authors.
 *
 * PGP is available for free to the public under certain restrictions.
 * See the PGP User's Guide (included in the release package) for
 * important information about licensing, patent restrictions on
 * certain algorithms, trademarks, copyrights, and export controls.
 *
 *	Modified 24-Jun-92 HAJK
 *	Adapt for VAX/VMS.
 *
 *	Modified: 11-Nov-92 HAJK
 *	Add FDL Support Routines. 
 *
 *	Modified: 31-Jan-93 HAJK
 *	Misc. updates for terminal handling.
 *	Add VMS command stuff.
 *	Add fileparse routine.
 */
#include <stdio.h>
#include "exitpgp.h"
#include "system.h"
#include "usuals.h"

/*===========================================================================*/
/*
 * UNIX
 */

#ifdef UNIX
/*
 * Define USE_SELECT to use the select() system call to check if
 * keyboard input is available. Define USE_NBIO to use non-blocking
 * read(). If you don't define anything the FIONREAD ioctl() command
 * will be used.
 *
 * Define NOTERMIO if you don't have the termios stuff
 */
#include <sys/types.h>
#include <fcntl.h>

#ifndef	NOTERMIO
#ifndef SVR2
#include <termios.h>
#else
#include <termio.h>
#endif /* not SVR2 */
#else
#include <sgtty.h>
#endif

#ifdef	USE_SELECT
#include <sys/time.h>
#ifdef _IBMR2
#include <sys/select.h>
#endif /* _IBMR2 */
#else
#ifndef USE_NBIO
#ifndef sun
#include <sys/ioctl.h>		/* for FIONREAD */
#else /* including both ioctl.h and termios.h gives a lot of warnings on sun */
#include <sys/filio.h>
#endif /* sun */
#ifndef FIONREAD
#define	FIONREAD	TIOCINQ
#endif
#endif
#endif
#include <signal.h>

static void setsigs(void);
static void rmsigs(void);
static void sig1(int);
static void sig2(int);
void breakHandler(int);
static int ttyfd= -1;
#ifndef SVR2
static void (*savesig)(int);
#else
static int (*savesig)(int);
#endif

void ttycbreak(void);
void ttynorm(void);

#ifndef NEED_KBHIT
#undef USE_NBIO
#endif

#ifndef NOTERMIO
#ifndef SVR2
static struct termios itio, tio;
#else
static struct termio itio, tio;
#endif /* not SVR2 */
#else
static struct sgttyb isg, sg;
#endif

#ifdef USE_NBIO
static int kbuf= -1;	/* buffer to store char read by kbhit() */
static int fflags;
#endif

static int gottio = 0;

void ttycbreak(void)
{
	if (ttyfd == -1) {
		if ((ttyfd = open("/dev/tty", O_RDWR)) < 0) {
		    fprintf(stderr, "cannot open tty, using stdin\n");
			ttyfd = 0;
		}
	}
#ifndef NOTERMIO
#ifndef SVR2
	if (tcgetattr(ttyfd, &tio) < 0)
#else
	if (ioctl(ttyfd, TCGETA, &tio) < 0)
#endif  /* not SVR2 */
	{
		fprintf (stderr, "\nUnable to get terminal characteristics: ");
		perror("ioctl");
		exitPGP(1);
	}
	itio = tio;
	setsigs();
	gottio = 1;
#ifdef USE_NBIO
	tio.c_cc[VMIN] = 0;
#else
	tio.c_cc[VMIN] = 1;
#endif
	tio.c_cc[VTIME] = 0;
	tio.c_lflag &= ~(ECHO|ICANON);
#ifndef SVR2
#ifdef ultrix
	/* Ultrix is broken and flushes the output as well! */
	tcsetattr (ttyfd, TCSANOW, &tio);
#else
	tcsetattr (ttyfd, TCSAFLUSH, &tio);
#endif
#else
	ioctl(ttyfd, TCSETAF, &tio);
#endif /* not SVR2 */
#else
	if (ioctl(ttyfd, TIOCGETP, &sg) < 0) {
		fprintf (stderr, "\nUnable to get terminal characteristics: ");
		perror("ioctl");
		exitPGP(1);
	}
	isg = sg;
	setsigs();
	gottio = 1;
#ifdef CBREAK
    sg.sg_flags |= CBREAK;
#else
    sg.sg_flags |= RAW;
#endif
	sg.sg_flags &= ~ECHO;
    ioctl(ttyfd, TIOCSETP, &sg);
#endif	/* !NOTERMIO */
#ifdef USE_NBIO
#ifndef O_NDELAY
#define	O_NDELAY	O_NONBLOCK
#endif
	if ((fflags = fcntl(ttyfd, F_GETFL, 0)) != -1)
		fcntl(ttyfd, F_SETFL, fflags|O_NDELAY);
#endif
}


void ttynorm(void)
{	gottio = 0;
#ifdef USE_NBIO
	if (fcntl(ttyfd, F_SETFL, fflags) == -1)
		perror("fcntl");
#endif
#ifndef NOTERMIO
#ifndef SVR2
#ifdef ultrix
	/* Ultrix is broken and flushes the output as well! */
	tcsetattr (ttyfd, TCSANOW, &itio);
#else
	tcsetattr (ttyfd, TCSAFLUSH, &itio);
#endif
#else
	ioctl(ttyfd, TCSETAF, &itio);
#endif /* not SVR2 */
#else
    ioctl(ttyfd, TIOCSETP, &isg);
#endif
	rmsigs();
}

static void sig1 (int sig)
{
#ifndef NOTERMIO
#ifndef SVR2
	tcsetattr (ttyfd, TCSANOW, &itio);
#else
	ioctl(ttyfd, TCSETAW, &itio);
#endif /* not SVR2 */
#else
    ioctl(ttyfd, TIOCSETP, &isg);
#endif
	signal (sig, SIG_DFL);
	if (sig == SIGINT)
		breakHandler(SIGINT);
	kill (getpid(), sig);
}

static void sig2 (int sig)
{
	if (gottio)
		ttycbreak();
	else
		setsigs();
}

static void setsigs(void)
{
	savesig = signal (SIGINT, sig1);
#ifdef	SIGTSTP
	signal (SIGCONT, sig2);
	signal (SIGTSTP, sig1);
#endif
}

static void rmsigs(void)
{	signal (SIGINT, savesig);
#ifdef	SIGTSTP
	signal (SIGCONT, SIG_DFL);
	signal (SIGTSTP, SIG_DFL);
#endif
}

#ifdef NEED_KBHIT
#ifndef CRUDE
int kbhit(void)
/* Return TRUE if there is a key to be read */
{
#ifdef USE_SELECT		/* use select() system call */
	struct timeval t;
	fd_set n;
	int r;

	timerclear(&t);
	FD_ZERO(&n);
	FD_SET(ttyfd, &n);
	r = select(32, &n, NULL, NULL, &t);
	if (r == -1) {
		perror("select");
		exitPGP(1);
	}
	return r > 0;
#else
#ifdef	USE_NBIO		/* use non-blocking read() */
	unsigned char ch;
	if (kbuf >= 0) 
		return(1);
	if (read(ttyfd, &ch, 1) == 1) {
		kbuf = ch;
		return(1);
	}
	return(0);
#else
	long lf;
	if (ioctl(ttyfd, FIONREAD, &lf) == -1) {
		perror("ioctl: FIONREAD");
		exitPGP(1);
	}
	return(lf);
#endif
#endif
}
#endif	/* !CRUDE */
#endif

int getch(void)
{
	char c;
#ifdef USE_NBIO
	while (!kbhit());	/* kbhit() does the reading */
	c = kbuf;
	kbuf = -1;
#else
 	c = 0;
	read(ttyfd, &c, 1);
#endif
 	return(c & 0xFF);
}

#if defined(_BSD) && !defined(__STDC__)

VOID *memset(s, c, n)
VOID *s;
register int c, n;
{
	register char *p = s;
	++n;
	while (--n)
		*p++ = c;
	return(s);
}
int memcmp(s1, s2, n)
register unsigned char *s1, *s2;
register int n;
{
	if (!n)
		return(0);
	while (--n && *s1 == *s2) {
		++s1;
		++s2;
	}
	return(*s1 - *s2);
}
VOID *memcpy(s1, s2, n)
register char *s1, *s2;
register int n;
{
	char *p = s1;
	++n;
	while (--n)
		*s1++ = *s2++;
	return(p);
}
#endif /* _BSD */

#if (defined(MACH) || defined(SVR2) || defined(_BSD)) && !defined(NEXT) \
&& !defined(AUX) && !defined(__MACHTEN__) || (defined(sun) && defined(i386))
int remove(name)
char *name;
{
	return unlink(name);
}
#endif

#if defined(SVR2) && !defined(AUX)
int rename(old, new)
register char *old, *new;
{
	unlink(new);
	if (link(old, new) < 0)
		return -1;
	if (unlink(old) < 0) {
		unlink(new);
		return -1;
	}
	return 0;
}
#endif /* SVR2 */

/* not all unices have clock() */
long
Clock()	/* not a replacement for clock(), just for random number generation */
{
#if defined(_BSD) || (defined(sun) && !defined(SOLARIS)) || \
defined(MACH) || defined(linux)
#include <sys/time.h>
#include <sys/resource.h>
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec +
		ru.ru_stime.tv_sec + ru.ru_stime.tv_usec +
		ru.ru_minflt + ru.ru_majflt +
		ru.ru_inblock + ru.ru_oublock +
		ru.ru_maxrss + ru.ru_nvcsw + ru.ru_nivcsw;

#else	/* no getrusage() */
#include <sys/times.h>
	struct tms tms;

	times(&tms);
	return(tms.tms_utime + tms.tms_stime);
#endif
}
#endif /* UNIX */


/*===========================================================================*/
/*
 * VMS
 */

#ifdef VMS			/* kbhit()/getch() equivalent */

/*
 * This code defines an equivalent version of kbhit() and getch() for
 * use under VAX/VMS, together with an exit handler to reset terminal
 * characteristics.
 *
 * This code assumes that kbhit() has been invoked to test that there
 * are characters in the typeahead buffer before getch() is invoked to
 * get the answer.
 */

#include <signal.h>
#include <string.h>
#include <file.h>
#include <ctype.h>
#include "pgp.h"
#include "mpilib.h"
#include "mpiio.h"
#include "fileio.h"
extern byte textbuf[DISKBUFSIZE];   /*	Defined in FILEIO.C */

/*	  
**  VMS Private Macros
*/	  
#include <descrip.h>
#include <devdef>
#include <iodef.h>
#include <ttdef.h>
#include <tt2def.h>
#include <dcdef.h>
#include <climsgdef.h>
#include <rms.h>
#include <hlpdef.h>

#define MAX_CMDSIZ	256  /*  Maximum command size */
#define MAX_FILENM	255 /* Mamimum file name size */

#define FDL$M_FDL_STRING    2		/* Use string for fdl text */
#define FDLSIZE		    4096	/* Maximum possible file size */

#ifdef _USEDCL_

/*
 * Declare some external procedure prototypes (saves me confusion!)
 */
extern int lib$get_input(
	    struct dsc$descriptor *resultant,
	    struct dsc$descriptor *prompt, 
	    unsigned short *resultant_length);
extern int lib$put_output(
	    struct dsc$descriptor *output);
extern int lib$sig_to_ret();
/*	  
**  The CLI routines are documented in the system routines manual.
*/	  
extern int cli$dcl_parse(
	    struct dsc$descriptor *command,
	    char cmd_table[],
	    int (*get_command)(
		struct dsc$descriptor *resultant,
		struct dsc$descriptor *prompt, 
		unsigned short *resultant_length),
	    int (*get_parameter)(
		struct dsc$descriptor *resultant,
		struct dsc$descriptor *prompt, 
		unsigned short *resultant_length),
	    struct dsc$descriptor *prompt);
extern int cli$present( struct dsc$descriptor *object);
extern int cli$_get_value(
	    struct dsc$descriptor *object,
	    struct dsc$decsriptor *value,
	    unsigned short *value_len);
/*
 * Static Data
 */
static $DESCRIPTOR (cmdprmt_d, "DROPSAFE> ");  /*  Prompt string */

#endif /* _USEDCL_ */

static volatile short	_kbhitChan_ = 0;

static volatile struct IOSB {
	unsigned short sts;
	unsigned short byteCount;
	unsigned short terminator;
	unsigned short terminatorSize;
	} iosb;

static $DESCRIPTOR (kbdev_desc, "SYS$COMMAND:");

static volatile struct {
	char Class;
	char Type;
	unsigned short BufferSize;
	unsigned int Mode;
	int ExtChar;
  } CharBuf, OldCharBuf;

static $DESCRIPTOR (out_file_descr, "SYS$DISK:[]"); /* Default Output
						       File Descr */

static int flags = FDL$M_FDL_STRING;

/*
 * **-kbhit_handler-This exit handler restores the terminal characteristics
 *
 * Description:
 *
 * This procedure is invoked to return the the terminal to normality (depends
 * on what you think is normal!). Anyway, it gets called to restore
 * characteristics either through ttynorm or via an exit handler.
 */
static void kbhit_handler(int *sts)
{
  ttynorm();
  (void) sys$dassgn (
	  _kbhitChan_);
  _kbhitChan_ = 0;
}

/*
 * Data Structures For Linking Up Exit Handler 
 */
unsigned int exsts;

static struct {
	int link;
	VOID *rtn;
	int argcnt;
	int *stsaddr;
   } exhblk = { 0, &(kbhit_handler), 1, &(exsts)};
/*
 * **-kbhit_Getchn-Get Channel
 *
 * Functional Description:
 *
 * Private routine to get a terminal channel and save the terminal
 * characteristics.
 *
 * Arguments:
 *
 *  None.
 *
 * Returns:
 *
 *  If 0, channel already assigned. If odd, then assign was successful
 * otherwise returns VMS error status.
 *
 * Implicit Inputs:
 *
 * _kbhitChan_	Channel assigned to the terminal (if any).
 *
 * Implicit Outputs:
 *
 *  OldCharBuf	Initial terminal characteristics.
 *  _kbhitChan_	Channel assigned to the terminal.
 *
 * Side Effects:
 *
 *  Establishes an exit handler to restore characteristics and deassign
 * terminal channel.
 */
static int kbhit_Getchn()
{
    int sts = 0;

    if (_kbhitChan_ == 0) {
	if ((sts = sys$assign (
			   &kbdev_desc,
			   &_kbhitChan_,
			   0,
			   0)) & 1) {
	    if ((sts = sys$qiow (
			       0,
			       _kbhitChan_,
			       IO$_SENSEMODE,
			       &iosb,
			       0,
			       0,
			       &OldCharBuf,
			       12,
			       0,
			       0,
			       0,
			       0)) & 01) sts = iosb.sts;
	    if (sts & 01) {
	      if (!(OldCharBuf.Class & DC$_TERM)) {
		fprintf(stderr,"\nNot running on a terminal");
		exitPGP(1);
	      }
	      (void) sys$dclexh (&exhblk);
	    }
	}
    }
    return(sts);
}
/*	  
 * **-ttynorm-Restore initial terminal characteristics
 *
 * Functional Description:
 *
 * This procedure is invoked to restore the initial terminal characteristics.
 */
void ttynorm()
/*
 * Arguments:
 *
 *  None.
 *
 * Implicit Inputs:
 *
 *  OldCharBuf	Initial terminal characteristics.
 *  _kbhitChan_	Channel assigned to the terminal.
 *
 * Implicit Outputs:
 *
 *  None.
 */	  
{
  int sts;

  if (_kbhitChan_ != 0) {
      CharBuf.Mode = OldCharBuf.Mode;
      CharBuf.ExtChar = OldCharBuf.ExtChar;
    /*
      CharBuf.Mode &= ~TT$M_NOECHO;
      CharBuf.ExtChar &= ~TT2$M_PASTHRU;
    */
      if ((sts = sys$qiow (
			       0,
			       _kbhitChan_,
			       IO$_SETMODE,
			       &iosb,
			       0,
			       0,
			       &OldCharBuf,
			       12,
			       0,
			       0,
			       0,
			       0)) & 01) sts = iosb.sts;
      if (!(sts & 01)) {
	    fprintf(stderr,"\nFailed to reset terminal characteristics!");
	    (void) lib$signal(sts);
      }
   }
   return;
}
/*
 * **-kbhit-Find out if a key has been pressed
 *
 * Description:
 *
 * Make the terminal noecho and sense the characters coming in by looking at
 * the typeahead count. Note that the character remains in the typeahead buffer
 * untill either read, or that the user types a Control-X when not in 'passall'
 * mode.
 */
int kbhit()
/*
 * Arguments:
 *
 *  None.
 *
 * Returns:
 *
 *  TRUE  if there is a character in the typeahead buffer.
 *  FALSE if there is no character in the typeahead buffer.
 */


{
  int sts;

  struct {
	unsigned short TypAhdCnt;
	char FirstChar;
	char Reserved[5];
  } TypCharBuf;

  /*
  **  Get typeahead count
  */
  if ((sts = sys$qiow (
			   0,
			   _kbhitChan_,
			   IO$_SENSEMODE | IO$M_TYPEAHDCNT,
			   &iosb,
			   0,
			   0,
			   &TypCharBuf,
			   8,
			   0,
			   0,
			   0,
			   0)) & 01) sts = iosb.sts;
  if (sts & 01) return(TypCharBuf.TypAhdCnt>0);
  (void) lib$signal(sts);
  exitPGP(1);
}

static int NoTerm[2] = { 0, 0};  /*  TT Terminator Mask (Nothing) */

/*
 * **-getch-Get a character and return it
 *
 * Description:
 *
 * Get a character from the keyboard and return it. Unlike Unix, the character
 * will be explicitly echoed unless ttycbreak() has been called first. If the
 * character is in the typeahead, that will be read first.
 */
int getch()
/*
 * Arguments:
 *
 *  None.
 *
 * Returns:
 *
 *  Character Read.
 */
{
  unsigned int sts;
  volatile char CharBuf;

  if (((sts = kbhit_Getchn()) & 01) || sts == 0) {
      if ((sts = sys$qiow (
			      0,
			      _kbhitChan_,
			      IO$_READVBLK,
			      &iosb,
			      0,
			      0,
			      &CharBuf,
			      1,
			      0,
			      &NoTerm,
			      0,
			      0)) & 01) sts = iosb.sts;
  }
  if (sts & 01) return ((int) CharBuf);
  fprintf(stderr,"\nFailed to get character");
  (void) lib$signal(sts);
}
/*
 * **-putch-Put Character To 'Console' Device
 *
 * This procedure is a companion to getch, outputing a character to the
 * terminal with a minimum of fuss (no VAXCRTLK, no RMS!). This routine
 * simply gets a channel (if there isn't one already and uses QIO to
 * output.
 *
 */
int putch(int chr)
/*
 * Arguments:
 *  chr		Character to output.
 *
 * Returns:
 *
 *  Status return from Getchn and qio.
 *
 * Side Effects
 *
 * May assign a channel to the terminal.
 */
{
  unsigned int sts;

  if (((sts = kbhit_Getchn()) & 01) || sts == 0) {
      if ((sts = sys$qiow (
			      0,
			      _kbhitChan_,
			      IO$_WRITEVBLK,
			      &iosb,
			      0,
			      0,
			      &chr,
			      1,
			      0,
			      0,
			      0,
			      0)) & 01) sts = iosb.sts;
  }
  if (sts & 01) return (sts);
  fprintf(stderr,"\nFailed to put character");
  (void) lib$signal(sts);
}
/*
 * **-ttycbreak-Set Unix-like Cbreak mode
 *
 * Functional Description:
 *
 * This code must be invoked to produce the Unix-like cbreak operation which
 * disables echo, allows control character input.
 */
void ttycbreak ()
/*
 * Arguments:
 *
 *  None.
 *
 * Returns:
 *
 *  None.
 *
 * Side Effects
 *
 * May assign a channel to the terminal.
 */
{
    struct {
	unsigned short TypAhdCnt;
	char FirstChar;
	char Reserved[5];
    } TypCharBuf;
    char buf[80];
    int sts;

    if (((sts = kbhit_Getchn()) & 01) || sts == 0) {
/*
 * Flush any typeahead before we change characteristics
 */
	if ((sts = sys$qiow (
			       0,
			       _kbhitChan_,
			       IO$_SENSEMODE | IO$M_TYPEAHDCNT,
			       &iosb,
			       0,
			       0,
			       &TypCharBuf,
			       8,
			       0,
			       0,
			       0,
			       0)) & 01) sts = iosb.sts;
	if (sts) {
	    if (TypCharBuf.TypAhdCnt>0) {
		if ((sts = sys$qiow (
			    0,
			   _kbhitChan_,
			   IO$_READVBLK | IO$M_NOECHO | IO$M_TIMED,
			   &iosb,
			   0,
			   0,
			   &buf,
			   (TypCharBuf.TypAhdCnt >= 80 ? 80 :
			    TypCharBuf.TypAhdCnt),
			   1,
			   &NoTerm,
			   0,
			   0)) & 01) sts = iosb.sts;
			   
		if (sts)
		    TypCharBuf.TypAhdCnt -= iosb.byteCount;
	    }
	}
	if (!(sts & 01)) TypCharBuf.TypAhdCnt = 0;
/*
 * Modify characteristics
 */
	CharBuf = OldCharBuf;
	CharBuf.Mode = (OldCharBuf.Mode | TT$M_NOECHO) & ~TT$M_NOTYPEAHD;
	CharBuf.ExtChar = OldCharBuf.ExtChar | TT2$M_PASTHRU;
	if ((sts = sys$qiow (
		       0,
		       _kbhitChan_,
		       IO$_SETMODE,
		       &iosb,
		       0,
		       0,
		       &CharBuf,
		       12,
	    	       0,
		       0,
		       0,
		       0)) & 01) sts = iosb.sts;
	if (!(sts & 01)) {
	  fprintf(stderr,
		  "\nttybreak()- Failed to set terminal characteristics!");
	  (void) lib$signal(sts);
	  exitPGP(1);
	}
    }
}


#ifdef _USEDCL_

/*
 * **-vms_getcmd-Get VMS Style Foreign Command
 *
 * Functional Description:
 *
 *  Get command from VAX/VMS foreign command line interface and parse
 * according to DCL rules. If the command line is ok, it can then be
 * parsed according to the rules in the DCL command language table.
 *
 */
int vms_GetCmd( char *cmdtbl)
/*
 * Arguments:
 *
 *  cmdtbl	Pointer to command table to parse.
 *
 * Returns:
 *
 *  ...TBS...
 *
 * Implicit Inputs:
 *
 *  Command language table defined in DROPDCL.CLD
 */
{
    int sts;
    char cmdbuf[MAX_CMDSIZ];
    unsigned short cmdsiz;
    struct dsc$descriptor cmdbuf_d = {0,0,0,0};
    struct dsc$descriptor infile_d = {0,0,0,0};
    char filenm[MAX_FILENM];
    unsigned short filenmsiz;
    unsigned short verb_size;

    /*	  
    **  DCL Parse Expects A Command Verb Prefixing The Argumnents
    **	fake it!
    */	  
    verb_size = cmdprmt_d.dsc$w_length - 2;  /*  Loose '> ' characters */
    cmdbuf_d.dsc$w_length = MAX_CMDSIZ-verb_size-1;
    cmdbuf_d.dsc$a_pointer = strncpy(cmdbuf,cmdprmt_d.dsc$a_pointer,verb_size)
      +	verb_size+1;
    cmdbuf[verb_size++]=' ';
    if ((sts = lib$get_foreign (  /*  Recover command line from DCL */
	           &cmdbuf_d, 
	           0, 
	           &cmdsiz, 
	           0)) & 01) {
	cmdbuf_d.dsc$a_pointer = cmdbuf;
	cmdbuf_d.dsc$w_length = cmdsiz + verb_size;
	VAXC$ESTABLISH(lib$sig_to_ret);   /*  Force unhandled exceptions
					      to return */
        sts = cli$dcl_parse(  /*  Parse Command Line */
		    &cmdbuf_d,
		    cmdtbl,			
		    lib$get_input,
		    lib$get_input,
		    &cmdprmt_d);
    }
    return(sts);
}
/*
 * **-vms_TstOpt-Test for command qualifier present
 *
 * Functional Description:
 *
 * This procedure is invoked to test whether an option is present. It is
 * really just a jacket routine for the system routine CLI$PRESENT
 * converting the argument and result into 'C' speak.
 *
 */
vms_TstOpt(char opt)
/*
 * Arguments:
 *
 *  opt	    Character label of qualifier to test for.
 *
 * Returns:
 *
 *  +1	Option present.
 *  0	Option absent.
 *  -1	Option negated.
 *
 * Implicit Inputs:
 *
 * Uses DCL command line context established by vms_GetOpt.
 */
{
    int sts;
    char buf;
    struct dsc$descriptor option_d = { 1, 0, 0, &buf};

    buf = _toupper(opt);
    VAXC$ESTABLISH(lib$sig_to_ret);   /*  Force unhandled exceptions
					  to return */
    switch (sts=cli$present(&option_d))
    {

	case CLI$_PRESENT :
	    return(1);
	case CLI$_ABSENT:
	    return(0);
	case CLI$_NEGATED:
	    return(-1);
    	default:
	    return(0);
    }    
}
/*
 * **-vms_GetVal-Get Qualifier Value.
 *
 * Functional Description:
 *
 * This procedure is invoked to return the value associated with a
 * qualifier that exists (See TstOpt).
 */
vms_GetVal( char opt, char *resval, unsigned short maxsiz)
/*
 * Arguments:
 *
 *  opt	    Character label of qualifier to test for.
 *  resval  Pointer to resulting value string.
 *  maxsiz  Maximum size of string.
 *
 * Returns:
 *
 *  ...TBS...
 *
 * Implicit Inputs:
 *
 * Uses DCL command line context established by vms_GetOpt.
 */
{
    int sts;
    char buf;
    struct dsc$descriptor option_d = { 1, 0, 0, &buf};
    struct dsc$descriptor value_d = {maxsiz-1, 0, 0, resval };
    unsigned short valsiz;

    VAXC$ESTABLISH(lib$sig_to_ret);   /*  Force unhandled exceptions
					  to return */
    buf = _toupper(opt);
    if ((sts = cli$get_value( 
	    &option_d,
	    &value_d,
	    &valsiz)) & 01) resval[valsiz] = '\0';
    return(sts);
}
/*
 * **-vms_GetArg-Get Argument Value.
 *
 * Functional Description:
 *
 * This procedure is invoked to return the value associated with an
 * argument.
 */
vms_GetArg( unsigned short arg, char *resval, unsigned short maxsiz)
/*
 * Arguments:
 *
 *  arg	    Argument Number (1-9)
 *  resval  Pointer to resulting value string.
 *  maxsiz  Maximum size of string.
 *
 * Returns:
 *
 *  ...TBS...
 *
 * Implicit Inputs:
 *
 * Uses DCL command line context established by vms_GetOpt.
 */
{
    int sts;
    char buf[2] = "P";
    struct dsc$descriptor option_d = { 2, 0, 0, buf};
    struct dsc$descriptor value_d = {maxsiz-1, 0, 0, resval };
    unsigned short valsiz;

    VAXC$ESTABLISH(lib$sig_to_ret);   /*  Force unhandled exceptions
					  to return */
    buf[1] = arg + '0';
    if ((sts = cli$present(&option_d)) & 01) {
	if ((sts = cli$get_value( 
	    &option_d,
	    &value_d,
	    &valsiz)) & 01) resval[valsiz] = '\0';
    } else return(0);
    return(sts);
}



/*
 * **-do_help-Invoke VMS Help Processor
 *
 * Functional Description:
 *
 * This procedure is invoked to display a suitable help message to the caller
 * using the standard VMS help library.
 *
 */
do_help(char *helptext, char *helplib)
/*
 * Arguments:
 *
 *  helptext	Text of help request.
 *  helplib	Help library.
 *
 * Returns:
 *
 * As for kbhit_Getchn and lbr$output_help.
 *
 * Side Effects:
 *
 * A channel may be opened to the terminal. A library is opened.
 */
{
    int sts;
    int helpflags;
    struct dsc$descriptor helptext_d = { strlen(helptext), 0, 0, helptext};
    struct dsc$descriptor helplib_d = { strlen(helplib), 0, 0, helplib};

    VAXC$ESTABLISH(lib$sig_to_ret);   /*  Force unhandled
					  exceptions to return */
    if (((sts = kbhit_Getchn()) & 01) || sts == 0) {
	helpflags = HLP$M_PROMPT|HLP$M_SYSTEM|HLP$M_GROUP|HLP$M_PROCESS;    
	sts = lbr$output_help(
		    lib$put_output,
		    &OldCharBuf.BufferSize,
		    &helptext_d,
		    &helplib_d,
		    &helpflags,
		    lib$get_input);
    }
    return(sts);
}
#endif /* _USEDCL_ */
unsigned long	vms_clock_bits[2];	/* VMS Hardware Clock */
const long	vms_ticks_per_update = 100000L; /* Clock update int. */

/*
 * FDL Stuff For Getting & Setting File Characteristics
 * This code was derived (loosely!) from the module LZVIO.C in the public 
 * domain LZW compress routine as found on the DECUS VAX SIG tapes (no author
 * given, so no credits!) 
 */

/*
 * **-fdl_generate-Generate An FDL
 *
 * Description:
 *
 * This procedure takes the name of an existing file as input and creates
 * an fdl. The FDL is retuned by pointer and length. The FDL space should be
 * released after use with a call to free();
 */
int fdl_generate(char *in_file, char **fdl, short *len)
/*
 * Arguments:
 *
 *	in_file	    char*   Filename of file to examine (Zero terminated).
 *
 *	fdl	    char*   Pointer to FDL that was created.
 *
 *	len	    short   Length of FDL created.
 *
 * Status Returns:
 *
 * VMS style. lower bit set means success.
 */
{

    struct dsc$descriptor fdl_descr = { 0,
				DSC$K_DTYPE_T,
				DSC$K_CLASS_D,
				0};
    struct FAB fab, *fab_addr;
    struct RAB rab, *rab_addr;
    struct NAM nam;
    struct XABFHC xab;
    int sts;
    int badblk;

/*
 * Build FDL Descriptor
 */
    if (!(sts = str$get1_dx(&FDLSIZE,&fdl_descr)) & 01) return(0);
/*
 * Build RMS Data Structures
 */
    fab = cc$rms_fab;
    fab_addr = &fab;
    nam = cc$rms_nam;
    rab = cc$rms_rab;
    rab_addr = &rab;
    xab = cc$rms_xabfhc;
    fab.fab$l_nam = &nam;
    fab.fab$l_xab = &xab;
    fab.fab$l_fna = in_file;
    fab.fab$b_fns = strlen(in_file);
    rab.rab$l_fab = &fab;
    fab.fab$b_fac = FAB$M_GET | FAB$M_BIO; /* This open block mode only */
/*
 * Attempt to Open File
 */
    if (!((sts = sys$open(&fab)) & 01)) {
	if (verbose) {
	    fprintf(stderr,"\n(SYSTEM) Failed to $OPEN %s\n",in_file);
	    (void) lib$signal(fab.fab$l_sts,fab.fab$l_stv);
	}
	return(sts);
    }
    if (fab.fab$l_dev & DEV$M_REC) {
	fprintf(stderr,"\n(SYSTEM) Attempt to read from output only device\n");
	sts = 0;
    } else {
	rab.rab$l_rop = RAB$M_BIO;
	if (!((sts = sys$connect(&rab)) & 01)) {
	    if (verbose) {
		fprintf(stderr,"\n(SYSTEM) Failed to $CONNECT %s\n",in_file);
		(void) lib$signal(fab.fab$l_sts,fab.fab$l_stv);
	    }
	} else {
	    if (!((sts = fdl$generate(
			&flags,
			&fab_addr,
			&rab_addr,
			NULL,NULL,
			&fdl_descr,
			&badblk,
			len)) & 01)) {
		if (verbose)
		  fprintf(stderr,"\n(SYSTEM) Failed to generate FDL\n",
			  in_file);
		free(fdl);
	    } else {
		if (!(*fdl = malloc(*len))) return(0);
		memcpy(*fdl,fdl_descr.dsc$a_pointer,*len);
	    }
	    (void) str$free1_dx(&fdl_descr);
	}
        sys$close(&fab);
    }
    return(sts);	    
}

/*	  
 * **-fdl_close-Closes files created by fdl_generate
 *  
 * Description:
 *
 * This procedure is invoked to close the file and release the data structures
 * allocated by fdl$parse.
 */
void fdl_close(void* rab)
/*
 * Arguments:
 *
 *	rab	VOID *	Pointer to RAB (voided to avoid problems for caller).
 *
 * Returns:
 *
 *	None.
 */
{
    struct FAB *fab;

    fab = ((struct RAB *) rab)->rab$l_fab;
    if (fab) {  /*  Close file if not already closed */
	if (fab->fab$w_ifi) sys$close(fab);
    }
    fdl$release( NULL, &rab);	  
}

/*
 * **-fdl_create-Create A File Using the recorded FDL (hope we get it right!)
 *
 * Description:
 *
 * This procedure accepts an FDL and uses it create a file. Unfortunately
 * there is no way we can easily patch into the back of the VAX C I/O
 * subsystem.
 */
VOID * fdl_create( char *fdl, short len, char *outfile, char *preserved_name)
/*
 * Arguments:
 *
 *	fdl	char*	FDL string descriptor.
 *
 *	len	short	Returned string length.
 *
 *	outfile	char*	Output filename.
 *
 *	preserved_name char*	Name from FDL.
 *
 * Returns:
 *
 *     0 in case of error, or otherwise the RAB pointer.
 */
{
    VOID *sts;
    int sts2;
    struct FAB *fab;
    struct RAB *rab;
    struct NAM nam;
    int badblk;
    char *resnam;

    struct dsc$descriptor fdl_descr = {
			    len,
			    DSC$K_DTYPE_T,
			    DSC$K_CLASS_S,
			    fdl
			    };

    sts = NULL;
/*
 * Initialize RMS NAM Block
 */
    nam = cc$rms_nam;
    nam.nam$b_rss = NAM$C_MAXRSSLCL;
    nam.nam$b_ess = NAM$C_MAXRSSLCL;
    if (!(resnam = nam.nam$l_esa = malloc(NAM$C_MAXRSSLCL+1))) {
	fprintf(stderr,"\n(FDL_CREATE) Out of memory!\n");
	return(NULL);
    }
/*
 * Parse FDL
 */
    if (!((sts2 = fdl$parse( &fdl_descr,
				&fab,
				&rab,
				&flags)) & 01)) {
	fprintf(stderr,"\nCreating (fdl$parse)\n");
	(void) lib$signal(sts2);
    } else {
/*
 * Extract & Return Name of FDL Supplied Filename
 */
	memcpy (preserved_name,fab->fab$l_fna,fab->fab$b_fns);
	preserved_name[fab->fab$b_fns] = '\0';
/*
 * Set Name Of Temporary File
 */
	fab->fab$l_fna = outfile;
	fab->fab$b_fns = strlen(outfile);
/*
 * Connect NAM Block
 */
	fab->fab$l_nam = &nam;
	fab->fab$l_fop |= FAB$M_NAM | FAB$M_CIF;
	fab->fab$b_fac |= FAB$M_BIO | FAB$M_PUT;
/*
 * Create File
 */
	if (!(sys$create(fab) & 01)) {
	    fprintf(stderr,"\nCreating (RMS)\n");
	    (void) lib$signal(fab->fab$l_sts,fab->fab$l_stv);
	    fdl_close(rab);
	} else {
	    if (verbose) {
		resnam[nam.nam$b_esl+1] = '\0';
		fprintf(stderr,"\nCreated %s successfully\n",resnam);
	    }
	    rab->rab$l_rop = RAB$M_BIO;
	    if (!(sys$connect(rab) & 01)) {
		fprintf(stderr,"\nConnecting (RMS)\n");
		(void) lib$signal(rab->rab$l_sts,rab->rab$l_stv);
		fdl_close(rab);
	    } else sts = rab;
	}
	fab->fab$l_nam = 0; /* I allocated NAM block,
			       so I must deallocate it! */
    }
    free(resnam);
    return(sts);		
}

/*
 * **-fdl_copyfile2bin-Copies the input file to a 'binary' output file
 *
 * Description:
 *
 * This procedure is invoked to copy from an opened file f to a file opened
 * directly through RMS. This allows us to make a block copy into one of the
 * many esoteric RMS file types thus preserving characteristics without blowing
 * up the C RTL. This code is based directly on copyfile from FILEIO.C.
 *
 * Calling Sequence:
 */
int fdl_copyfile2bin( FILE *f, VOID *rab, word32 longcount)
/*
 * Arguments:
 *
 *	f	    FILE*	Pointer to input file
 *
 *	rab	    RAB*	Pointer to output file RAB
 * 
 *	longcount   word32	Size of file
 *
 * Returns:
 *
 *	0   If we were successful.
 *	-1  We had an error on the input file (VAXCRTL).
 *	+1  We had an error on the output file (direct RMS).
 */
{
    int status = 0;
    word32 count;
    ((struct RAB *) rab)->rab$l_rbf = &textbuf;
    ((struct RAB *) rab)->rab$l_bkt = 0;
    do { /*  Read and write longcount bytes */
	if (longcount < (word32) DISKBUFSIZE)
	    count = longcount;
	else
	    count = DISKBUFSIZE;
	count = fread(textbuf,1,count,f);
	if (count > 0) {
/*	  
 *  No byte order conversion required, source and target system are both
 *  VMS so have the same byte ordering.
 */	  
	    ((struct RAB *) rab)->rab$w_rsz = (unsigned short) count;
	    if (!(sys$write (
		       rab, 
		       NULL, 
		       NULL) & 01)) {
		  lib$signal(((struct RAB *) rab)->rab$l_sts,
			     ((struct RAB *) rab)->rab$l_stv);
		  status = 1;
		  break;
	    }
	    longcount -= count;
	}
    } while (count==DISKBUFSIZE);
    burn(textbuf);
    return(status);
}
/*
 * **-vms_fileparse-Parse A VMS File Specification
 *
 * Functional Description:
 *
 * This procedure is invoked to parse a VMS file specification using default 
 * and related specifications to fill in any missing components. This works a 
 * little like DCL's F$PARSE function with the syntax check only specified
 * (that is we don't check the device or the directory). The related file
 * spec is really for when we want to use the name of an input file (w/o the
 * directory) to supply the name of an output file.
 *
 * Note that we correctly handle the situation where the output buffer overlays
 * the input filespec by testing for the case and then handling it by copying
 * the primary input specification to a temporary buffer before parsing.
 */
int vms_fileparse( char *outbuf, char *filespec, char *defspec, char *relspec)
/*
 * Arguments:
 *
 *  outbuf	Returned file specification.
 *  filespec	Primary file specification (optional).
 *  defspec	Default file specification (optional).
 *  relspec	Related file specification (optional).
 *
 * Returns:
 *
 *  As for SYS$PARSE.
 *
 * Implicit Inputs:
 *
 *  None.
 *
 * Implicit Outputs:
 *
 *  None.
 *
 * Side Effects:
 *
 *  ...TBS...
 */
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    struct NAM rlnam = cc$rms_nam;
    int sts = 1;
    int len;
    char tmpbuf[NAM$C_MAXRSSLCL];
    char expfnam2[NAM$C_MAXRSSLCL];

    if (outbuf != NULL) {
	outbuf[0] = '\0';
	fab.fab$l_fop != FAB$M_NAM;  /*  Enable RMS NAM block processing */
	nam.nam$b_nop |= NAM$M_PWD | NAM$M_SYNCHK;
	/*	  
	**  Handle Related Spec (If reqd).
	*/	  
	if (relspec != NULL) {
	    if ((len = strlen(relspec)) > 0) {
		fab.fab$l_nam = &rlnam;
		fab.fab$b_fns = len;
		fab.fab$l_fna = relspec;
		rlnam.nam$b_ess = NAM$C_MAXRSSLCL;
		rlnam.nam$l_esa = expfnam2;
		rlnam.nam$b_nop |= NAM$M_PWD | NAM$M_SYNCHK;
		if ((sts = sys$parse (
			    &fab, 
			    0, 
			    0)) & 01) {
		    rlnam.nam$l_rsa = rlnam.nam$l_esa;
		    rlnam.nam$b_rsl = rlnam.nam$b_esl;
		    nam.nam$l_rlf = &rlnam;
		    fab.fab$l_fop |= FAB$M_OFP;
		}
	    }
	}
	if (sts) {
	    fab.fab$l_nam = &nam;
	    nam.nam$l_esa = outbuf;
	    nam.nam$b_ess = NAM$C_MAXRSSLCL;
	    /*	  
	    **  Process Default Specification:
	    */	  
	    if (defspec != NULL) {
		if ((len = strlen(defspec)) > 0) {
		    fab.fab$l_dna = defspec;
		    fab.fab$b_dns = len;
		}
	    }
	    /*	  
	    **  Process Main File Specification:
	    */	  
	    fab.fab$l_fna = NULL;
	    fab.fab$b_fns = 0;
	    if (filespec != NULL) {
		if ((len = strlen(filespec)) > 0) {
		    fab.fab$b_fns = len;
		    if (filespec == outbuf)
			fab.fab$l_fna = memcpy(tmpbuf,filespec,len);
		    else
			fab.fab$l_fna = filespec;
		}
	    }
	    if ((sts = sys$parse(
		       &fab, 
		       0, 
		       0)) && 01) outbuf[nam.nam$b_esl] = '\0';
	}
    }
    return (sts);
}
#endif /* VMS */


/*
 * ------------------------- Amiga specific routines -------------------------
 */

#ifdef AMIGA

#include <time.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <libraries/dosextens.h>
#include <libraries/reqtools.h>
#include <proto/dos.h> 
#include <proto/exec.h>
#include <proto/reqtools.h>
#include "pgp.h"

/*
 * This getenv will use the WB2.0 calls if you have the 2.0
 * rom. If not, it resorts to looking in the ENV: directory.
 */

/*
 * I am sorry to report that SAS/C is buggy. :-(
 * It doesn't recognize replacement routines if they are linked
 * to the main code and not included in the file itself. I hate
 * stuff like that. :-(
 *                                            -peter
 */

char *amiga_getenv(const char *name)
{
        FILE *fp;
        char *ptr;
        static char value[256];
        static char buf[256];

        /*
         * 2.0 style?
         */
        if (DOSBase->dl_lib.lib_Version >= 36) {
                if (GetVar((char *) name, value, 256, 0L) == -1)
                        return NULL;
        }
        else {
                if (strlen(name) > 252)
                        return NULL;
                strcpy(buf, "ENV:");
                strcpy(&buf[4], name);
                if (!(fp = fopen(buf, "r")))
                        return NULL;
                for (ptr = value; (*ptr = getc(fp)) != EOF
                     && *ptr != '\n'
                     && ++ptr < &value[256];) ;
                fclose(fp);
                *ptr = 0;
        }
        return value;
}


extern FILE *pgpout;
char *requesterdesc;

/*
 * AmigaRequestString() is a trick to make PGP more usable from scripts.
 * The problem is, that most scripts don't allow user interaction over
 * the standard input. The same problem occurs when working in filter mode.
 *
 * This routine will be called by PGP's getstring() whenever user input
 * is requested but the standard input is not interactive. Because the
 * routine can't know what string to ask for, I added the Amiga-specific
 * variable requesterdesc, which holds the last string printed to pgpout
 * before getstring was called.
 *
 * This solution is not pretty, but it works.
 *                                                      Peter Simons
 */

int AmigaRequestString(char *buffer, int maxlen, int echo)
{
        struct ReqToolsBase *ReqToolsBase;
        struct TagItem ti[] = {
                {RTGS_Invisible, FALSE},
                {RTGS_TextFmt, 0L},
                {RTGS_Flags, GSREQF_CENTERTEXT},
                {TAG_DONE, 0L}
        };
        int len = 0;
        char name[64];

        if (!maxlen)
                return 0;
        if (!echo)
                ti[0].ti_Data = TRUE;
        ti[1].ti_Data = (ULONG) (requesterdesc) ? ((*requesterdesc == '\n') ? requesterdesc+1 : requesterdesc) : "Please enter required string";
                                /* This one is tricky, too. Because of the format of the
                                 * LANG() module we have a prefacing return before most
                                 * strings, which will make our beautiful requester look
                                 * a bit stupid. This way, we get rid of it. :-)
                                 */
        sprintf(name, "PGPAmiga %s", rel_version);

        if (ReqToolsBase = (struct ReqToolsBase *) OpenLibrary(REQTOOLSNAME, 38L))
        {
                *buffer = '\0';
                if (rtGetStringA(buffer, maxlen, name, NULL, ti))
                        len = strlen(buffer);
                CloseLibrary((struct Library *) ReqToolsBase);
        }
        else
        {  fprintf(stderr,"Could not open ReqTools.library!  Try using PGP "
                   "without -f.\n");
           exitPGP(7);   /* Error exit */
        }
        requesterdesc=NULL;   /* Program will re-set it before next getstring() call */
        return len;
}

sendpacket(struct MsgPort *rec,LONG action,LONG arg1) 
{
  struct StandardPacket *pkt;
  struct MsgPort *rp;
  LONG res1 = 0L;

  if (rp = (struct MsgPort *)CreatePort(NULL,0L)) {
    if (pkt = (struct StandardPacket *)\
	 AllocMem(sizeof(struct StandardPacket),MEMF_PUBLIC|MEMF_CLEAR)) {
	   pkt->sp_Msg.mn_Node.ln_Name = (BYTE *)&pkt->sp_Pkt;
	   pkt->sp_Pkt.dp_Link = &pkt->sp_Msg;
	   pkt->sp_Pkt.dp_Port = rp;
	   pkt->sp_Pkt.dp_Type = action;
	   pkt->sp_Pkt.dp_Arg1 = arg1;
	   PutMsg(rec,&pkt->sp_Msg);
	   WaitPort(rp);
	   GetMsg(rp);
	   res1 = pkt->sp_Pkt.dp_Res1;
	   FreeMem((UBYTE*)pkt,sizeof(struct StandardPacket));
	 }
	 DeletePort(rp);
	}
	return(res1);

}

void ttycbreak(void)
{
  struct MsgPort *ch;

  ch = ((struct FileHandle *)BADDR(Input()))->fh_Type;
  sendpacket(ch,ACTION_SCREEN_MODE,-1L);
}

void ttynorm(void)
{
  struct MsgPort *ch;

  ch = ((struct FileHandle *)BADDR(Input()))->fh_Type;
  sendpacket(ch,ACTION_SCREEN_MODE,0L);
}

int getch(void)
{
  char buf;

  Read(Input(),&buf,1);
  return((int)buf);
}

int kbhit(void)
{
  if(WaitForChar(Input(), 1)) return 1;
  return 0;
}

/* GetSysTime problem with WB 1.3 fixed by A. Hartley (february@genie.com) */

extern struct timerequest *TimerIO;    /* Defined in random.c */

void am_GetSysTime(struct timeval *tv)
{
   TimerIO->tr_node.io_Command=TR_GETSYSTIME;
   DoIO((struct IORequest *) TimerIO);
   *tv=TimerIO->tr_time;
}

#ifdef __SASC

/*
 * SAS/C CTRL-C handler
 */

void __regargs _CXBRK(void)
{
  struct MsgPort *ch;

  /* it might happen we catch a ^C while in cbreak mode.
   * so always set the screen to the normal mode.
  */

  ch = ((struct FileHandle *)BADDR(Input()))->fh_Type;
  sendpacket(ch, ACTION_SCREEN_MODE, 0L);


  fprintf(pgpout, "\n*** Program Aborted.\n");
  exitPGP(6); /* INTERRUPT */
}
#endif    /* __SASC */

#endif /* AMIGA */


/*===========================================================================*/
/*
 * other stuff for non-MSDOS systems
 */

#ifdef ATARI
#ifdef __PUREC__
#include <tos.h>
#else
#include <osbind.h>		/* use GEMDOS functions for I/O */
#endif

int kbhit(void)
{
	return Cconis();	/* ret == 0 : no char available */
}

int getch(void)
{
	return (Cnecin() & 0x000000FF);	/* ASCII-Code in Bits 0..7   */
}					/* Scan-Codes in Bits 16..23 */
#endif /* ATARI */

#if !defined(MSDOS) && !defined(ATARI)
#include <ctype.h>
#include "charset.h"
char *strlwr(char *s)
{	/*
	**        Turns string s into lower case.
	*/
	int c;
	char *p = s;
	while (c = *p)
		*p++ = to_lower(c);
	return(s);
}
#endif /* !MSDOS && !ATARI */


#ifdef strstr
#undef strstr
/* Not implemented on some systems - return first instance of s2 in s1 */
char *mystrstr (char *s1, char *s2)
{	int i;
	char *strchr();

	if (!s2 || !*s2)
		return s1;
	for ( ; ; )
	{	if (!(s1 = strchr (s1, *s2)))
			return s1;
		for (i=1; s2[i] && (s1[i]==s2[i]); ++i)
			;
		if (!s2[i])
			return s1;
		++s1;
	}
}
#endif /* strstr */


#ifdef fopen
#undef fopen

#ifdef ATARI
#define F_BUF_SIZE 8192  /* seems to be a good value ... */

FILE *myfopen(const char *filename, const char *mode)
/* Open streams with larger buffer to increase disk I/O speed. */
/* Adjust F_BUF_SIZE to change buffer size.                    */
{
    FILE *f;

    if ( (f = fopen(filename, mode)) != NULL )
        if (setvbuf(f, NULL, _IOFBF, F_BUF_SIZE)) /* no memory? */
        {
            fclose(f);                 /* then close it again */
            f = fopen(filename, mode); /* and try again in normal mode */
        }
    return(f);                         /* return either handle or NULL */
}
	
#else /* ATARI */

/* Remove "b" from 2nd arg */
FILE *myfopen(char *filename, char *type)
{	char buf[10];

	buf[0] = *type++;
	if (*type=='b')
		++type;
	strcpy(buf+1,type);
	return fopen(filename, buf);
}
#endif /* not ATARI */
#endif /* fopen */


#ifndef MSDOS
#ifdef OS2

static int chr = -1;

int kbhit(void)
{
	if (chr == -1)
	  	chr = _read_kbd(0, 0, 0);
	return (chr != -1);
}

int getch(void)
{
	int c;

	if (chr >= 0) {
		c = chr;
		chr = -1;
	} else
	  	c = _read_kbd(0, 1, 0);

	return c;
}

#endif /* OS2 */
#endif /* MSDOS */

#ifdef MACTC5	/* 203a */

#include "My_console.h"

int getch(void) {
	while( !kbhit() );
	return( getc(stdin) );
}

int kbhit(void) {
	int kbuf;
	
	csetmode(C_RAW, stdin);
	kbuf = getc(stdin);
	if( kbuf != EOF ) ungetc((kbuf & 0xff), stdin);
	csetmode(C_ECHO, stdin);
	return( (kbuf == EOF) ? 0 : 1 );
}

#endif

/*EWS Fix -f lockup on passphrase prompts for TURBO C++ */
#if defined(MSDOS) && !defined(__GO32__) && defined(__TURBOC__)
#include <bios.h>
#include <signal.h>

#if !defined(_KEYBRD_READY)
#define _KEYBRD_READY 1    /* To support old versions of Turbo C */
#endif
#if !defined(_KEYBRD_READ)
#define _KEYBRD_READ 0     /* To support old versions of Turbo C */
#endif

int kbhit(void)
{
  int c;
  c=bioskey(_KEYBRD_READY);
  if (c != 0) c=1;
  return c;
} /*kbhit*/

int getch(void)
{
   int c;
   c=bioskey(_KEYBRD_READ);
   if (c==11779) raise(SIGINT);   /* Ctrl-C */
   return c & 0xff;
} /*getch*/
#endif

/*EWS Fix -f lockup on passphrase prompts for MSC */
#if defined(MSDOS) && !defined(__GO32__) && defined(_MSC_VER)
#include <bios.h>
#include <signal.h>
#include <dos.h>

int getcbrk(void)
{
    union REGS r;

    r.x.ax=0x3300;
    intdos(&r, &r);
    return(r.h.dl);
}

int setcbrk(int xx)
{
    union REGS r;

    r.x.ax=0x3301;
    r.h.dl=xx;
    intdos(&r, &r);
    return(r.h.dl);
}

int kbhit(void)
{
    int c;
    c=_bios_keybrd(_KEYBRD_READY);
    if (c != 0) c=1;
    return c;
} /*kbhit*/

int getch(void)
{
    int c;
    c=_bios_keybrd(_KEYBRD_READ);
    if (c==11779) raise(SIGINT);   /* Ctrl-C */
    return c & 0xff;
} /*getch*/
#endif
 
#ifdef EBCDIC
static int kbuf = -1;

int kbhit(void)
{
   int ch;
   if (kbuf >= 0)
      return 1;
   if (ch = getchar()) {
      kbuf = ch;
      return 1;
   }
   return 0;
}

int getch(void)
{
   int ch;
   while (!kbhit());
   ch = kbuf;
   kbuf = -1;
   return ch;
}

int c370_rename(char *from, char *to)
{
   return rename(from,to) == 0 ? 0 : -1;
}
#endif /* EBCDIC */


/* ==== zbits.c ==== */
/*

 Copyright (C) 1990,1991 Mark Adler, Richard B. Wales, and Jean-loup Gailly.
 Permission is granted to any individual or institution to use, copy, or
 redistribute this software so long as all of the original files are included
 unmodified, that it is not sold for profit, and that this copyright notice
 is retained.

*/

/*
 *  bits.c by Jean-loup Gailly.
 *
 *  This is a new version of im_bits.c originally written by Richard B. Wales
 *
 *  PURPOSE
 *
 *      Output variable-length bit strings.
 *
 *  DISCUSSION
 *
 *      The PKZIP "deflate" file format interprets compressed file data
 *      as a sequence of bits.  Multi-bit strings in the file may cross
 *      byte boundaries without restriction.
 *
 *      The first bit of each byte is the low-order bit.
 *
 *      The routines in this file allow a variable-length bit value to
 *      be output right-to-left (useful for literal values). For
 *      left-to-right output (useful for code strings from the tree routines),
 *      the bits must have been reversed first with bi_reverse().
 *
 *  INTERFACE
 *
 *      void bi_init (FILE *zipfile)
 *          Initialize the bit string routines.
 *
 *      void send_bits (int value, int length)
 *          Write out a bit string, taking the source bits right to
 *          left.
 *
 *      int bi_reverse (int value, int length)
 *          Reverse the bits of a bit string, taking the source bits left to
 *          right and emitting them right to left.
 *
 *      void bi_windup (void)
 *          Write out any remaining bits in an incomplete byte.
 *
 *      void copy_block(char far *buf, unsigned len, int header)
 *          Copy a stored block to the zip file, storing first the length and
 *          its one's complement if requested.
 *
 */

#include "zip.h"

/* ===========================================================================
 * Local data used by the "bit string" routines.
 */

local FILE *zfile; /* output zip file */

local unsigned short bi_buf;
/* Output buffer. bits are inserted starting at the bottom (least significant
 * bits).
 */

#define Buf_size (8 * 2*sizeof(char))
/* Number of bits used within bi_buf. (bi_buf might be implemented on
 * more than 16 bits on some systems.)
 */

local int bi_valid;                  /* number of valid bits in bi_buf */
/* All bits above the last valid bit are always zero.
 */

#ifdef DEBUG
ulg bits_sent;   /* bit length of the compressed data */
#endif

/* Output a 16 bit value to the bit stream, lower (oldest) byte first */
#define PUTSHORT(w) \
{  (void) zputc ((char)((w) & 0xff), zfile); \
   (void) zputc ((char)((ush)(w) >> 8), zfile); \
}

/* Output an 8 bit value to the bit stream, bits right to left */
#define PUTBYTE(w) \
{  (void) zputc ((char)((w) & 0xff), zfile); \
}

/* ===========================================================================
 * Initialize the bit string routines.
 */
void bi_init (zipfile)

    FILE *zipfile;  /* output zip file */
{
    zfile  = zipfile;
    bi_buf = 0;
    bi_valid = 0;
#ifdef DEBUG
    bits_sent = 0L;
#endif
}

/* ===========================================================================
 * Send a value on a given number of bits.
 * IN assertion: length <= 16 and value fits in length bits.
 */
void send_bits(value, length)
    int value;  /* value to send */
    int length; /* number of bits */
{
#ifdef DEBUG
    Tracevv((stderr," l %2d v %4x ", length, value));
    Assert(length > 0 && length <= 15, "invalid length");
    bits_sent += (ulg)length;
#endif
    /* If not enough room in bi_buf, use (valid) bits from bi_buf and
     * (16 - bi_valid) bits from value, leaving (width - (16-bi_valid))
     * unused bits in value.
     */
    if (bi_valid > (int)Buf_size - length) {
        bi_buf |= (value << bi_valid);
        PUTSHORT(bi_buf);
        bi_buf = (ush)value >> (Buf_size - bi_valid);
        bi_valid += length - Buf_size;
    } else {
        bi_buf |= value << bi_valid;
        bi_valid += length;
    }
}

/* ===========================================================================
 * Reverse the first len bits of a code, using straightforward code (a faster
 * method would use a table)
 * IN assertion: 1 <= len <= 15
 */
unsigned bi_reverse(code, len)
    unsigned code; /* the value to invert */
    int len;       /* its bit length */
{
    register unsigned res = 0;
    do {
        res |= code & 1;
        code >>= 1, res <<= 1;
    } while (--len > 0);
    return res >> 1;
}

/* ===========================================================================
 * Write out any remaining bits in an incomplete byte.
 */
void bi_windup()
{
    if (bi_valid > 8) {
        PUTSHORT(bi_buf);
    } else if (bi_valid > 0) {
        PUTBYTE(bi_buf);
    }
    bi_buf = 0;
    bi_valid = 0;
    if (ferror (zfile)) error ("write error on zip file");
#ifdef DEBUG
    bits_sent = (bits_sent+7) & ~7;
#endif
}

/* ===========================================================================
 * Copy a stored block to the zip file, storing first the length and its
 * one's complement if requested.
 */
void copy_block(buf, len, header)
    char far *buf; /* the input data */
    unsigned len;  /* its length */
    int header;    /* true if block header must be written */
{
    bi_windup();              /* align on byte boundary */

    if (header) {
	PUTSHORT((ush)len);   
	PUTSHORT((ush)~len);
#ifdef DEBUG
        bits_sent += 2*16;
#endif
    }
    zfwrite(buf, 1, len, zfile); /* ??? far */
#ifdef DEBUG
    bits_sent += (ulg)len<<3;
#endif
	fflush(zfile);
    if (ferror(zfile)) error ("write error on zip file");
}


/* ==== zdeflate.c ==== */
/*

 Copyright (C) 1990-1992 Mark Adler, Richard B. Wales, Jean-loup Gailly,
 Kai Uwe Rommel and Igor Mandrichenko.
 Permission is granted to any individual or institution to use, copy, or
 redistribute this software so long as all of the original files are included
 unmodified, that it is not sold for profit, and that this copyright notice
 is retained.

*/

/*
 *  deflate.c by Jean-loup Gailly.
 *
 *  PURPOSE
 *
 *      Identify new text as repetitions of old text within a fixed-
 *      length sliding window trailing behind the new text.
 *
 *  DISCUSSION
 *
 *      The "deflation" process depends on being able to identify portions
 *      of the input text which are identical to earlier input (within a
 *      sliding window trailing behind the input currently being processed).
 *
 *      The most straightforward technique turns out to be the fastest for
 *      most input files: try all possible matches and select the longest.
 *      The key feature is of this algorithm is that insertion and deletions
 *      from the string dictionary are very simple and thus fast. Insertions
 *      and deletions are performed at each input character, whereas string
 *      matches are performed only when the previous match ends. So it is
 *      preferable to spend more time in matches to allow very fast string
 *      insertions and deletions. The matching algorithm for small strings
 *      is inspired from that of Rabin & Karp. A brute force approach is
 *      used to find longer strings when a small match has been found.
 *      A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 *      (by Leonid Broukhis).
 *         A previous version of this file used a more sophisticated algorithm
 *      (by Fiala and Greene) which is guaranteed to run in linear amortized
 *      time, but has a larger average cost and uses more memory. However
 *      the F&G algorithm may be faster for some highly redundant files if
 *      the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 *      The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 *      I found it in 'freeze' written by Leonid Broukhis.
 *      Thanks to many info-zippers for bug reports and testing.
 *
 *  REFERENCES
 *
 *      APPNOTE.TXT documentation file in PKZIP 2.0 distribution.
 *
 *      A description of the Rabin and Karp algorithm is given in the book
 *         "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 *      Fiala,E.R., and Greene,D.H.
 *         Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 *  INTERFACE
 *
 *      void lm_init (int pack_level, ush *flags)
 *          Initialize the "longest match" routines for a new file
 *
 *      ulg deflate (void)
 *          Processes a new input file and return its compressed length. Sets
 *          the compressed length, crc, deflate flags and internal file
 *          attributes.
 */

#include "zunzip.h"
#include "zip.h"
#ifdef MACTC5
#include "Macutil3.h"
#endif

/* ===========================================================================
 * Configuration parameters
 */

/* Compile with MEDIUM_MEM to reduce the memory requirements or
 * with SMALL_MEM to use as little memory as possible.
 * Warning: defining these symbols affects MATCH_BUFSIZE and HASH_BITS
 * (see below) and thus affects the compression ratio. The compressed output
 * is still correct, and might even be smaller in some cases.
 */

#ifdef SMALL_MEM
#   define HASH_BITS  13  /* Number of bits used to hash strings */
#else
#ifdef MEDIUM_MEM
#   define HASH_BITS  14
#else
#   define HASH_BITS  15
   /* For portability to 16 bit machines, do not use values above 15. */
#endif
#endif

#define HASH_SIZE (unsigned)(1<<HASH_BITS)
#define HASH_MASK (HASH_SIZE-1)
#define WMASK     (WSIZE-1)
/* HASH_SIZE and WSIZE must be powers of two */

#define NIL 0
/* Tail of hash chains */

#define FAST 4
#define SLOW 2
/* speed options for the general purpose bit flag */

#ifndef TOO_FAR
#  define TOO_FAR 4096
#endif
/* Matches of length 3 are discarded if their distance exceeds TOO_FAR */

/* ===========================================================================
 * Local data used by the "longest match" routines.
 */

typedef ush Pos;
typedef unsigned IPos;
/* A Pos is an index in the character window. We use short instead of int to
 * save space in the various tables. IPos is used only for parameter passing.
 */

#ifndef DYN_ALLOC
  uch    far slide[2L*WSIZE];
  /* Sliding window. Input bytes are read into the second half of the window,
   * and move to the first half later to keep a dictionary of at least WSIZE
   * bytes. With this organization, matches are limited to a distance of
   * WSIZE-MAX_MATCH bytes, but this ensures that IO is always
   * performed with a length multiple of the block size. Also, it limits
   * the window size to 64K, which is quite useful on MSDOS.
   * To do: limit the window size to WSIZE+BSZ if SMALL_MEM (the code would
   * be less efficient since the data would have to be copied WSIZE/BSZ times)
   */
  Pos    far prev[WSIZE];
  /* Link to older string with same hash index. To limit the size of this
   * array to 64K, this link is maintained only for the last 32K strings.
   * An index in this array is thus a window index modulo 32K.
   */
  Pos    far head[HASH_SIZE];
  /* Heads of the hash chains or NIL */
#else
  uch    far * near slide = NULL;
  Pos    far * near prev   = NULL;
  static void far *__slide, *__prev;
  static Pos    far * near head;
#endif

long block_start;
/* window position at the beginning of the current output block. Gets
 * negative when the window is moved backwards.
 */

local unsigned near ins_h;  /* hash index of string to be inserted */

#define H_SHIFT  ((HASH_BITS+MIN_MATCH-1)/MIN_MATCH)
/* Number of bits by which ins_h and del_h must be shifted at each
 * input step. It must be such that after MIN_MATCH steps, the oldest
 * byte no longer takes part in the hash key, that is:
 *   H_SHIFT * MIN_MATCH >= HASH_BITS
 */

unsigned int near prev_length;
/* Length of the best match at previous step. Matches not greater than this
 * are discarded. This is used in the lazy match evaluation.
 */

      unsigned near strstart;      /* start of string to insert */
unsigned near match_start;         /* start of matching string */
local int      near eofile;        /* flag set at end of input file */
local unsigned near lookahead;     /* number of valid bytes ahead in window */

unsigned near max_chain_length;
/* To speed up deflation, hash chains are never searched beyond this length.
 * A higher limit improves compression ratio but degrades the speed.
 */

local unsigned int max_lazy_match;
/* Attempt to find a better match only when the current match is strictly
 * smaller than this value.
 */

int near good_match;
/* Use a faster search when the previous match is longer than this */


/* Values for max_lazy_match, good_match and max_chain_length, depending on
 * the desired pack level (0..9). The values given below have been tuned to
 * exclude worst case performance for pathological files. Better values may be
 * found for specific files.
 */
typedef struct config {
   int good_length;
   int max_lazy;
   unsigned max_chain;
   uch flag;
} config;

local config configuration_table[10] = {
/*      good lazy chain flag */
/* 0 */ {0,    0,    0,  0},     /* store only */
/* 1 */ {4,    4,   16,  FAST},  /* maximum speed  */
/* 2 */ {6,    8,   16,  0},
/* 3 */ {8,   16,   32,  0},
/* 4 */ {8,   32,   64,  0},
/* 5 */ {8,   64,  128,  0},
/* 6 */ {8,  128,  256,  0},
/* 7 */ {8,  128,  512,  0},
/* 8 */ {32, 258, 1024,  0},
/* 9 */ {32, 258, 4096,  SLOW}}; /* maximum compression */

/* Note: the current code requires max_lazy >= MIN_MATCH and max_chain >= 4
 * but these restrictions can easily be removed at a small cost.
 */

#define EQUAL 0
/* result of memcmp for equal strings */

/* ===========================================================================
 *  Prototypes for local functions. Use asm version by default for
 *  MSDOS but not Unix. However the asm version version is recommended
 *  for 386 Unix.
 */
#ifdef ATARI_ST
#  undef MSDOS /* avoid the processor specific parts */
#endif
#if defined(MSDOS) && !defined(NO_ASM) && !defined(ASM)
#  define ASM
#endif

local void fill_window   OF((void));
      int  longest_match OF((IPos cur_match));
#ifdef ASM
      void match_init OF((void)); /* asm code initialization */
#endif

#ifdef DEBUG
local  void check_match OF((IPos start, IPos match, int length));
#endif

#undef MIN
#define MIN(a,b) ((a) <= (b) ? (a) : (b))
/* The arguments must not have side effects. */

/* ===========================================================================
 * Update a hash value with the given input byte
 * IN  assertion: all calls to to UPDATE_HASH are made with consecutive
 *    input characters, so that a running hash key can be computed from the
 *    previous key instead of complete recalculation each time.
 */
#define UPDATE_HASH(h,c) (h = (((h)<<H_SHIFT) ^ (c)) & HASH_MASK)

/* ===========================================================================
 * Insert string s in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * IN  assertion: all calls to to INSERT_STRING are made with consecutive
 *    input characters and the first MIN_MATCH bytes of s are valid
 *    (except for the last MIN_MATCH-1 bytes of the input file).
 */
#define INSERT_STRING(s, match_head) \
   (UPDATE_HASH(ins_h, slide[(s) + MIN_MATCH-1]), \
    prev[(s) & WMASK] = match_head = head[ins_h], \
    head[ins_h] = (s))

/* ===========================================================================
 * Initialize the "longest match" routines for a new file
 */
void lm_init (pack_level, flags)
    int pack_level; /* 0: store, 1: best speed, 9: best compression */
    ush *flags;     /* general purpose bit flag */
{
    register unsigned j;

    if (pack_level < 1 || pack_level > 9) error("bad pack level");

    /* Use dynamic allocation if compiler does not like big static arrays: */
#ifdef DYN_ALLOC
	__slide = slide = (uch far*) fcalloc(WSIZE*2*sizeof(uch)+16, 1);
	__prev = prev = (Pos far*) fcalloc(WSIZE*sizeof(Pos)+16, 1);
	head   = (Pos far*) fcalloc(HASH_SIZE, sizeof(Pos));

	if (slide == NULL || prev == NULL || head == NULL) {
		err(ZE_MEM, "window allocation");
	}
#endif /* DYN_ALLOC */
#ifdef ASM
    match_init(); /* initialize the asm code */
#endif
    /* Initialize the hash table. */
    for (j = 0;  j < HASH_SIZE; j++) head[j] = NIL;
    /* prev will be initialized on the fly */

    /* Set the default configuration parameters:
     */
    max_lazy_match   = configuration_table[pack_level].max_lazy;
    good_match       = configuration_table[pack_level].good_length;
    max_chain_length = configuration_table[pack_level].max_chain;
    *flags          |= configuration_table[pack_level].flag;
    /* ??? reduce max_chain_length for binary files */

    strstart = 0;
    block_start = 0L;

#if defined(MSDOS) && !defined(__32BIT__) && !defined(__GO32__)
    /* Can't read a 64K block under MSDOS */
    lookahead = read_buf((char*)slide, (unsigned)WSIZE);
#else
    lookahead = read_buf((char*)slide, 2*WSIZE);
#endif
    if (lookahead == 0 || lookahead == (unsigned)EOF) {
       eofile = 1, lookahead = 0;
       return;
    }
    eofile = 0;
    /* Make sure that we always have enough lookahead. This is important
     * if input comes from a device such as a tty.
     */
    while (lookahead < MIN_LOOKAHEAD && !eofile) fill_window();

    ins_h = 0;
    for (j=0; j<MIN_MATCH-1; j++) UPDATE_HASH(ins_h, slide[j]);
    /* If lookahead < MIN_MATCH, ins_h is garbage, but this is
     * not important since only literal bytes will be emitted.
     */
}

void lm_free()
{
#ifdef DYN_ALLOC
#ifndef __TURBOC__          /*EWS*/
	fcfree(__slide);
	fcfree(__prev);
	fcfree(head);
#else
        free(__slide);
        free(__prev);
        free(head);
#endif
	slide = NULL;
	prev = head = NULL;
#endif
}

/* ===========================================================================
 * Set match_start to the longest match starting at the given string and
 * return its length. Matches shorter or equal to prev_length are discarded,
 * in which case the result is equal to prev_length and match_start is
 * garbage.
 * IN assertions: cur_match is the head of the hash chain for the current
 *   string (strstart) and its distance is <= MAX_DIST, and prev_length >= 1
 */
#ifndef ASM
/* For MSDOS, OS/2 and 386 Unix, an optimized version is in match.asm. The code
 * is functionally equivalent, so you can use the C version if desired.
 */
int longest_match(cur_match)
    IPos cur_match;                             /* current match */
{
    unsigned chain_length = max_chain_length;   /* max hash chain length */
    register uch far *scan = slide + strstart;  /* current string */
    register uch far *match = scan;             /* matched string */
    register int len;                           /* length of current match */
    int best_len = prev_length;                 /* best match length so far */
    IPos limit = strstart > (IPos)MAX_DIST ? strstart - (IPos)MAX_DIST : NIL;
    /* Stop when cur_match becomes <= limit. To simplify the code,
     * we prevent matches with the string of slide index 0.
     */
#ifdef UNALIGNED_OK
    register ush scan_start = *(ush*)scan;
    register ush scan_end   = *(ush*)(scan+best_len-1);
#else
    register uch scan_start = *scan;
    register uch scan_end1  = scan[best_len-1];
    register uch scan_end   = scan[best_len];
#endif

    /* Do not waste too much time if we already have a good match: */
    if (prev_length >= good_match) {
        chain_length >>= 2;
    }

    do {
        Assert(cur_match < strstart, "no future");
        match = slide + cur_match;

        /* Skip to next match if the match length cannot increase
         * or if the match length is less than 2:
         */
#if (defined(UNALIGNED_OK) && HASH_BITS >= 8)
        /* This code assumes sizeof(unsigned short) == 2 and
         * sizeof(unsigned long) == 4. Do not use UNALIGNED_OK if your
         * compiler uses different sizes.
         */
        if (*(ush*)(match+best_len-1) != scan_end ||
            *(ush*)match != scan_start) continue;

        len = MIN_MATCH - 4;
        /* It is not necessary to compare scan[2] and match[2] since they are
         * always equal when the other bytes match, given that the hash keys
         * are equal and that HASH_BITS >= 8.
         */
        do {} while ((len+=4) < MAX_MATCH-3 &&
                     *(ulg*)(scan+len) == *(ulg*)(match+len));
        /* The funny do {} generates better code for most compilers */

        if (*(ush*)(scan+len) == *(ush*)(match+len)) len += 2;
        if (scan[len] == match[len]) len++;

#else /* UNALIGNED_OK */
        if (match[best_len] != scan_end ||
            match[best_len-1] != scan_end1 || *match != scan_start)
           continue;
        /* It is not necessary to compare scan[1] and match[1] since they
         * are always equal when the other bytes match, given that
         * the hash keys are equal and that h_shift+8 <= HASH_BITS,
         * that is, when the last byte is entirely included in the hash key.
         * The condition is equivalent to
         *       (HASH_BITS+2)/3 + 8 <= HASH_BITS
         * or: HASH_BITS >= 13
         * Also, we check for a match at best_len-1 to get rid quickly of
         * the match with the suffix of the match made at the previous step,
         * which is known to fail.
         */
#if HASH_BITS >= 13
        len = 1;
#else
        len = 0;
#endif
        do {} while (++len < MAX_MATCH && scan[len] == match[len]);

#endif /* UNALIGNED_OK */

        if (len > best_len) {
            match_start = cur_match;
            best_len = len;
            if (len == MAX_MATCH) break;
#ifdef UNALIGNED_OK
            scan_end = *(ush*)(scan+best_len-1);
#else
            scan_end1  = scan[best_len-1];
            scan_end   = scan[best_len];
#endif
        }
    } while (--chain_length != 0 &&
             (cur_match = prev[cur_match & WMASK]) > limit);

    return best_len;
}
#endif /* NO_ASM */

#ifdef DEBUG
/* ===========================================================================
 * Check that the match at match_start is indeed a match.
 */
local void check_match(start, match, length)
    IPos start, match;
    int length;
{
    /* check that the match is indeed a match */
    if (memcmp((char*)slide + match,
                (char*)slide + start, length) != EQUAL) {
        fprintf(stderr,
            " start %d, match %d, length %d\n",
            start, match, length);
        error("invalid match");
    }
    if (verbose) {
        fprintf(stderr,"\\[%d,%d]", start-match, length);
        /* putc a macro, not safe to modify args!! */
        do { putc(slide[start], stderr); start++; } while (--length!=0);
    }
}
#else
#  define check_match(start, match, length)
#endif

/* ===========================================================================
 * Fill the window when the lookahead becomes insufficient.
 * Updates strstart and lookahead, and sets eofile if end of input file.
 * IN assertion: lookahead < MIN_LOOKAHEAD && strstart + lookahead > 0
 * OUT assertions: at least one byte has been read, or eofile is set;
 *    file reads are performed for at least two bytes (required for the
 *    translate_eol option).
 */
local void fill_window()
{
    register unsigned n, m;
    unsigned more = (unsigned)((ulg)2*WSIZE - (ulg)lookahead - (ulg)strstart);
    /* Amount of free space at the end of the window. */

    /* If the window is full, move the upper half to the lower one to make
     * room in the upper half.
     */
    if (more == (unsigned)EOF) {
        /* Very unlikely, but possible on 16 bit machine if strstart == 0
         * and lookahead == 1 (input done one byte at time)
         */
        more--;
    } else if (more <= 1) {
        /* By the IN assertion, the window is not empty so we can't confuse
         * more == 0 with more == 64K on a 16 bit machine.
         */
        memcpy((char*)slide, (char*)slide+WSIZE, (unsigned)WSIZE);
        match_start -= WSIZE;
        strstart    -= WSIZE;
        /* strstart - WSIZE >= WSIZE - 1 - lookahead >= WSIZE - MIN_LOOKAHEAD
         * so we now have strstart >= MAX_DIST:
         */
        Assert (strstart >= MAX_DIST, "window slide too early");
        block_start -= (long) WSIZE;

        for (n = 0; n < HASH_SIZE; n++) {
            m = head[n];
            head[n] = (Pos)(m >= WSIZE ? m-WSIZE : NIL);
        }
        for (n = 0; n < WSIZE; n++) {
            m = prev[n];
            prev[n] = (Pos)(m >= WSIZE ? m-WSIZE : NIL);
            /* If n is not on any hash chain, prev[n] is garbage but
             * its value will never be used.
             */
        }
        more += WSIZE;
#ifdef ZIP
        if (verbose) putc('.', stderr);
#endif
    }
    /* At this point, more >= 2 */
    n = read_buf((char*)slide+strstart+lookahead, more);
    if (n == 0 || n == (unsigned)EOF) {
        eofile = 1;
    } else {
        lookahead += n;
    }
}

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK(eof) \
   flush_block(block_start >= 0L ? (char*)&slide[block_start] : (char*)NULL,\
               (long)strstart - block_start, (eof))

/* ===========================================================================
 * Processes a new input file and return its compressed length.
 */
#ifdef NO_LAZY
ulg deflate()
{
    IPos hash_head; /* head of the hash chain */
    int flush;      /* set if current block must be flushed */
    unsigned match_length = 0;  /* length of best match */

    prev_length = MIN_MATCH-1;
    while (lookahead != 0) {
        /* Insert the string slide[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
#ifdef MACTC5
		mac_poll_for_break();
#endif
        INSERT_STRING(strstart, hash_head);

        /* Find the longest match, discarding those <= prev_length.
         * At this point we have always match_length < MIN_MATCH
         */
        if (hash_head != NIL && strstart - hash_head <= MAX_DIST) {
            /* To simplify the code, we prevent matches with the string
             * of slide index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
            match_length = longest_match (hash_head);
            /* longest_match() sets match_start */
            if (match_length > lookahead) match_length = lookahead;
        }
        if (match_length >= MIN_MATCH) {
            check_match(strstart, match_start, match_length);

            flush = ct_tally(strstart-match_start, match_length - MIN_MATCH);

            lookahead -= match_length;
            match_length--; /* string at strstart already in hash table */
            do {
                strstart++;
                INSERT_STRING(strstart, hash_head);
                /* strstart never exceeds WSIZE-MAX_MATCH, so there are
                 * always MIN_MATCH bytes ahead. If lookahead < MIN_MATCH
                 * these bytes are garbage, but it does not matter since the
                 * next lookahead bytes will always be emitted as literals.
                 */
            } while (--match_length != 0);
        } else {
            /* No match, output a literal byte */
            flush = ct_tally (0, slide[strstart]);
            lookahead--;
        }
        strstart++; 
        if (flush) FLUSH_BLOCK(0), block_start = strstart;

        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        while (lookahead < MIN_LOOKAHEAD && !eofile) fill_window();

    }
    return FLUSH_BLOCK(1); /* eof */
}
#else /* LAZY */
/* ===========================================================================
 * Same as above, but achieves better compression. We use a lazy
 * evaluation for matches: a match is finally adopted only if there is
 * no better match at the next window position.
 */
ulg deflate()
{
    IPos hash_head;          /* head of hash chain */
    IPos prev_match;         /* previous match */
    int flush;               /* set if current block must be flushed */
    int match_available = 0; /* set if previous match exists */
    register unsigned match_length = MIN_MATCH-1; /* length of best match */
#ifdef DEBUG
    extern ulg isize;        /* byte length of input file, for debug only */
#endif

    /* Process the input block. */
    while (lookahead != 0) {
        /* Insert the string slide[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
#ifdef MACTC5
		mac_poll_for_break();
#endif
        INSERT_STRING(strstart, hash_head);

        /* Find the longest match, discarding those <= prev_length.
         */
        prev_length = match_length, prev_match = match_start;
        match_length = MIN_MATCH-1;

        if (hash_head != NIL && prev_length < max_lazy_match &&
            strstart - hash_head <= MAX_DIST) {
            /* To simplify the code, we prevent matches with the string
             * of slide index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
            match_length = longest_match (hash_head);
            /* longest_match() sets match_start */
            if (match_length > lookahead) match_length = lookahead;
            /* Ignore a length 3 match if it is too distant: */
            if (match_length == MIN_MATCH && strstart-match_start > TOO_FAR){
                /* If prev_match is also MIN_MATCH, match_start is garbage
                 * but we will ignore the current match anyway.
                 */
                match_length--;
            }
        }
        /* If there was a match at the previous step and the current
         * match is not better, output the previous match:
         */
        if (prev_length >= MIN_MATCH && match_length <= prev_length) {

            check_match(strstart-1, prev_match, prev_length);

            flush = ct_tally(strstart-1-prev_match, prev_length - MIN_MATCH);

            /* Insert in hash table all strings up to the end of the match.
             * strstart-1 and strstart are already inserted.
             */
            lookahead -= prev_length-1;
            prev_length -= 2;
            do {
                strstart++;
                INSERT_STRING(strstart, hash_head);
                /* strstart never exceeds WSIZE-MAX_MATCH, so there are
                 * always MIN_MATCH bytes ahead. If lookahead < MIN_MATCH
                 * these bytes are garbage, but it does not matter since the
                 * next lookahead bytes will always be emitted as literals.
                 */
            } while (--prev_length != 0);
            match_available = 0;
            match_length = MIN_MATCH-1;
            strstart++;
            if (flush) FLUSH_BLOCK(0), block_start = strstart;

        } else if (match_available) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             */
            Tracevv((stderr,"%c",slide[strstart-1]));
            if (ct_tally (0, slide[strstart-1])) {
                FLUSH_BLOCK(0), block_start = strstart;
            }
            strstart++;
            lookahead--;
        } else {
            /* There is no previous match to compare with, wait for
             * the next step to decide.
             */
            match_available = 1;
            strstart++;
            lookahead--;
        }
#if 0	/* for pgp: disabled to allow compiling with -DDEBUG */
        Assert (strstart <= isize && lookahead <= isize, "a bit too far");
#endif

        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        while (lookahead < MIN_LOOKAHEAD && !eofile) fill_window();
    }
    if (match_available) ct_tally (0, slide[strstart-1]);

    return FLUSH_BLOCK(1); /* eof */
}
#endif /* LAZY */


/* ==== zfile_io.c ==== */
/*---------------------------------------------------------------------------

  file_io.c

  This file contains routines for doing direct input/output, file-related
  sorts of things.  Most of the system-specific code for unzip is contained
  here, including the non-echoing password code for decryption (bottom).

  Modified: 24 Jun 92 - HAJK
  Fix VMS support
  ---------------------------------------------------------------------------*/


#include "zunzip.h"

#if 0
/****************************/
/* Function FillBitBuffer() */
/****************************/

int FillBitBuffer()
{
    /*
     * Fill bitbuf, which is 32 bits.  This function is only used by the
     * READBIT and PEEKBIT macros (which are used by all of the uncompression
     * routines).
     */
    UWORD temp;

    zipeof = 1;
    while (bits_left < 25 && ReadByte(&temp) == 8)
    {
      bitbuf |= (ULONG)temp << bits_left;
      bits_left += 8;
      zipeof = 0;
    }
    return 0;
}

/***********************/
/* Function ReadByte() */
/***********************/

int ReadByte(x)
UWORD *x;
{
    /*
     * read a byte; return 8 if byte available, 0 if not
     */


    if (csize-- <= 0)
	return 0;

    if (incnt == 0) {
	if ((incnt = read(zipfd, (char *) inbuf, INBUFSIZ)) <= 0)
	    return 0;
	/* buffer ALWAYS starts on a block boundary:  */
	inptr = inbuf;
    }
    *x = *inptr++;
    --incnt;
    return 8;
}

#else
/*
 * This function is used only by the NEEDBYTE macro in inflate.c.
 * It fills the buffer and resets the count and pointers for the
 * macro to resume processing.  The count is set to the number of bytes
 * read in minus one, while the pointer is set to the beginning of the
 * buffer.  This is all to make the macro more efficient.
 *
 * In exceptional circumstances, zinflate can read a byte or two past the
 * end of input, so we allow this for a little bit before returning
 * an error.  zinflate doesn't actually use the bytes, so the value
 * is irrelevant.
 *
 * Returns 0 on success, non-zero on error.
 */
#ifdef MACTC5
int eofonce = 0;
#endif

int FillInBuf()
{
#ifndef MACTC5
	static int eofonce = 0;
#endif

	incnt = read(zipfd, (char *)inbuf, INBUFSIZ);

	if (incnt > 0) {	/* Read went okay */
		inptr = inbuf;
		--incnt;
		return 0;
	} else if (incnt == 0 && !eofonce) {	/* Special fudge case */
		eofonce++;
		incnt = 2;
		inptr = inbuf;
		return 0;
	} else {		/* Error */
		return 1;
	}
}
#endif

/**************************/
/* Function FlushOutput() */
/**************************/

int FlushOutput()
{
    /*
     * flush contents of output buffer; return PK-type error code
     */
    int len;

    if (outcnt) {
			len = outcnt;
	    if (write(outfd, (char *) outout, len) != len)
#ifdef MINIX
                if (errno == EFBIG)
                    if (write(fd, outout, len/2) != len/2  ||
                        write(fd, outout+len/2, len/2) != len/2)
#endif /* MINIX */
                {
                    return 50;    /* 50:  disk full */
                }
        outpos += outcnt;
        outcnt = 0;
        outptr = outbuf;
    }
    return (0);                 /* 0:  no error */
}


/* ==== zglobals.c ==== */
/*

 Copyright (C) 1990,1991 Mark Adler, Richard B. Wales, and Jean-loup Gailly.
 Permission is granted to any individual or institution to use, copy, or
 redistribute this software so long as all of the original files are included
 unmodified, that it is not sold for profit, and that this copyright notice
 is retained.

*/

/*
 *  globals.c by Mark Adler.
 */

#define GLOBALS         /* include definition of errors[] in zip.h */
#include "zip.h"

/* Argument processing globals */
int method = BEST;      /* one of BEST, DEFLATE (only), or STORE (only) */
#ifdef MACTC5
int level = 9;          /* 0=fastest compression, 9=best compression */
#else
int level = 5;          /* 0=fastest compression, 9=best compression */
#endif
char *special = (char *)NULL;   /* List of special suffixes */


/* ==== zinflate.c ==== */
/* inflate.c -- Not copyrighted 1992 by Mark Adler
   version c4, 15 April 1992 */


/* You can do whatever you like with this source file, though I would
   prefer that if you modify it and redistribute it that you include
   comments to that effect with your name and the date.  Thank you.

   History:
   vers    date          who           what
   ----  ---------  --------------  ------------------------------------
    a    ~~ Feb 92  M. Adler        used full (large, one-step) lookup table
    b1   21 Mar 92  M. Adler        first version with partial lookup tables
    b2   21 Mar 92  M. Adler        fixed bug in fixed-code blocks
    b3   22 Mar 92  M. Adler        sped up match copies, cleaned up some
    b4   25 Mar 92  M. Adler        added prototypes; removed window[] (now
                                    is the responsibility of unzip.h--also
                                    changed name to slide[]), so needs diffs
                                    for unzip.c and unzip.h (this allows
                                    compiling in the small model on MSDOS);
                                    fixed cast of q in huft_build();
    b5   26 Mar 92  M. Adler        got rid of unintended macro recursion.
    b6   27 Mar 92  M. Adler        got rid of nextbyte() routine.  fixed
                                    bug in inflate_fixed().
    c1   30 Mar 92  M. Adler        removed lbits, dbits environment variables.
                                    changed BMAX to 16 for explode.  Removed
                                    OUTB usage, and replaced it with flush()--
                                    this was a 20% speed improvement!  Added
                                    an explode.c (to replace unimplode.c) that
                                    uses the huft routines here.  Removed
                                    register union.
    c2    4 Apr 92  M. Adler        fixed bug for file sizes a multiple of 32k.
    c3   10 Apr 92  M. Adler        reduced memory of code tables made by
                                    huft_build significantly (factor of two to
                                    three).
    c4   15 Apr 92  M. Adler        added NOMEMCPY do kill use of memcpy().
                                    worked around a Turbo C optimization bug.
    c5   21 Apr 92  M. Adler        added the WSIZE #define to allow reducing
                                    the 32K window size for specialized
                                    applications.
    c6   27 May 92  J.loup Gailly   Adapted for pgp
 */


/*
   Inflate deflated (PKZIP's method 8 compressed) data.  The compression
   method searches for as much of the current string of bytes (up to a
   length of 258) in the previous 32K bytes.  If it doesn't find any
   matches (of at least length 3), it codes the next byte.  Otherwise, it
   codes the length of the matched string and its distance backwards from
   the current position.  There is a single Huffman code that codes both
   single bytes (called "literals") and match lengths.  A second Huffman
   code codes the distance information, which follows a length code.  Each
   length or distance code actually represents a base value and a number
   of "extra" (sometimes zero) bits to get to add to the base value.  At
   the end of each deflated block is a special end-of-block (EOB) literal/
   length code.  The decoding process is basically: get a literal/length
   code; if EOB then done; if a literal, emit the decoded byte; if a
   length then get the distance and emit the referred-to bytes from the
   sliding window of previously emitted data.

   There are (currently) three kinds of inflate blocks: stored, fixed, and
   dynamic.  The compressor deals with some chunk of data at a time, and
   decides which method to use on a chunk-by-chunk basis.  A chunk might
   typically be 32K or 64K.  If the chunk is uncompressible, then the
   "stored" method is used.  In this case, the bytes are simply stored as
   is, eight bits per byte, with none of the above coding.  The bytes are
   preceded by a count, since there is no longer an EOB code.

   If the data is compressible, then either the fixed or dynamic methods
   are used.  In the dynamic method, the compressed data is preceded by
   an encoding of the literal/length and distance Huffman codes that are
   to be used to decode this block.  The representation is itself Huffman
   coded, and so is preceded by a description of that code.  These code
   descriptions take up a little space, and so for small blocks, there is
   a predefined set of codes, called the fixed codes.  The fixed method is
   used if the block codes up smaller that way (usually for quite small
   chunks), otherwise the dynamic method is used.  In the latter case, the
   codes are customized to the probabilities in the current block, and so
   can code it much better than the pre-determined fixed codes.
 
   The Huffman codes themselves are decoded using a mutli-level table
   lookup, in order to maximize the speed of decoding plus the speed of
   building the decoding tables.  See the comments below that precede the
   lbits and dbits tuning parameters.
 */


/*
   Notes beyond the 1.93a appnote.txt:

   1. Distance pointers never point before the beginning of the output
      stream.
   2. Distance pointers can point back across blocks, up to 32k away.
   3. There is an implied maximum of 7 bits for the bit length table and
      15 bits for the actual data.
   4. If only one code exists, then it is encoded using one bit.  (Zero
      would be more efficient, but perhaps a little confusing.)  If two
      codes exist, they are coded using one bit each (0 and 1).
   5. There is no way of sending zero distance codes--a dummy must be
      sent if there are none.  (History: a pre 2.0 version of PKZIP would
      store blocks with no distance codes, but this was discovered to be
      too harsh a criterion.)
   6. There are up to 286 literal/length codes.  Code 256 represents the
      end-of-block.  Note however that the static length tree defines
      288 codes just to fill out the Huffman codes.  Codes 286 and 287
      cannot be used though, since there is no length base or extra bits
      defined for them.  Similarily, there are up to 30 distance codes.
      However, static trees define 32 codes (all 5 bits) to fill out the
      Huffman codes, but the last two had better not show up in the data.
   7. Unzip can check dynamic huffman blocks for complete code sets.
      The exception is that a single code would not be complete (see #4).
   8. The five bits following the block type is really the number of
      literal codes sent minus 257.
   9. Length codes 8,16,16 are interpreted as 13 length codes of 8 bits
      (1+6+6).  Therefore, to output three times the length, you output
      three codes (1+1+1), whereas to output four times the same length,
      you only need two codes (1+3).  Hmm.
  10. In the tree reconstruction algorithm, Code = Code + Increment
      only if BitLength(i) is not zero.  (Pretty obvious.)
  11. Correction: 4 Bits: # of Bit Length codes - 4     (4 - 19)
  12. Note: length code 284 can represent 227-258, but length code 285
      really is 258.  The last length deserves its own, short code
      since it gets used a lot in very redundant files.  The length
      258 is special since 258 - 3 (the min match length) is 255.
  13. The literal/length and distance code bit lengths are read as a
      single stream of lengths.  It is possible (and advantageous) for
      a repeat code (16, 17, or 18) to go across the boundary between
      the two sets of lengths.
 */

#include "zunzip.h"
#include "exitpgp.h"
#define OF __

#ifdef MACTC5
#include "Macutil3.h"
void err(int c, char *msg);
#endif

#ifndef WSIZE
#  define WSIZE 8192
/* window size--must be a power of two, <= 32K, and equal to that of zip.
 * On 16 bit machines (MSDOS), WSIZE must be <= 16K (32K is possible
 * with a few hacks, see the zip archiver.
 */
#endif

#ifdef DYN_ALLOC
  extern char *slide;
#else
  extern char slide[];
#endif

/* Huffman code lookup table entry--this entry is four bytes for machines
   that have 16-bit pointers (e.g. PC's in the small or medium model).
   Valid extra bits are 0..13.  e == 15 is EOB (end of block), e == 16
   means that v is a literal, 16 < e < 32 means that v is a pointer to
   the next table, which codes e - 16 bits, and lastly e == 99 indicates
   an unused code.  If a code with e == 99 is looked up, this implies an
   error in the data. */
struct huft {
  byte e;               /* number of extra bits or operation */
  byte b;               /* number of bits in this code or subcode */
  union {
    UWORD n;            /* literal, length base, or distance base */
    struct huft *t;     /* pointer to next level of table */
  } v;
};


/* Function prototypes */
int huft_build OF((unsigned *, unsigned, unsigned, UWORD *, UWORD *,
		   struct huft **, int *));
int huft_free OF((struct huft *));
void flush OF((unsigned));
int inflate_codes OF((struct huft *, struct huft *, int, int));
int inflate_stored OF((void));
int inflate_fixed OF((void));
int inflate_dynamic OF((void));
int inflate_block OF((int *));
int inflate_entry OF((void));
int inflate OF((void));


/* The inflate algorithm uses a sliding 32K byte window on the uncompressed
   stream to find repeated byte strings.  This is implemented here as a
   circular buffer.  The index is updated simply by incrementing and then
   and'ing with 0x7fff (32K-1). */
/* It is left to other modules to supply the 32K area.  It is assumed
   to be usable as if it were declared "byte slide[32768];" or as just
   "byte *slide;" and then malloc'ed in the latter case.  The definition
   must be in unzip.h, included above. */
unsigned wp;            /* current position in slide */


/* Tables for deflate from PKZIP's appnote.txt. */
static unsigned border[] = {    /* Order of the bit length code lengths */
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static UWORD cplens[] = {       /* Copy lengths for literal codes 257..285 */
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
        /* note: see note #13 above about the 258 in this list. */
static UWORD cplext[] = {       /* Extra bits for literal codes 257..285 */
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 99, 99}; /* 99==invalid */
static UWORD cpdist[] = {       /* Copy offsets for distance codes 0..29 */
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577};
static UWORD cpdext[] = {       /* Extra bits for distance codes */
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
        12, 12, 13, 13};



/* Macros for inflate() bit peeking and grabbing.
   The usage is:
   
        NEEDBITS(j)
        x = b & mask_bits[j];
        DUMPBITS(j)

   where NEEDBITS makes sure that b has at least j bits in it, and
   DUMPBITS removes the bits from b.  The macros use the variable k
   for the number of bits in b.  Normally, b and k are register
   variables for speed, and are initialized at the begining of a
   routine that uses these macros from a global bit buffer and count.

   If we assume that EOB will be the longest code, then we will never
   ask for bits with NEEDBITS that are beyond the end of the stream.
   So, NEEDBITS should not read any more bytes than are needed to
   meet the request.  Then no bytes need to be "returned" to the buffer
   at the end of the last block.

   However, this assumption is not true for fixed blocks--the EOB code
   is 7 bits, but the other literal/length codes can be 8 or 9 bits.
   (Why PK made the EOB code, which can only occur once in a block,
   the *shortest* code in the set, I'll never know.)  However, by
   making the first table have a lookup of seven bits, the EOB code
   will be found in that first lookup, and so will not require that too
   many bits be pulled from the stream.
 */

ULONG bb;                       /* bit buffer */
unsigned bk;                    /* bits in bit buffer */

UWORD bytebuf;
#if 0
#define NEXTBYTE    (ReadByte(&bytebuf), bytebuf)
#define NEEDBITS(n) {while(k<(n)){b|=((ULONG)NEXTBYTE)<<k;k+=8;}}
#else
#define NEEDBITS(n) \
	{ while (k < (n)) { \
		if (--incnt < 0 && FillInBuf()) return 1; \
		b |= (ULONG)*inptr++ << k; \
		k += 8; \
	} }
#endif

#define DUMPBITS(n) {b>>=(n);k-=(n);}


/*
   Huffman code decoding is performed using a multi-level table lookup.
   The fastest way to decode is to simply build a lookup table whose
   size is determined by the longest code.  However, the time it takes
   to build this table can also be a factor if the data being decoded
   is not very long.  The most common codes are necessarily the
   shortest codes, so those codes dominate the decoding time, and hence
   the speed.  The idea is you can have a shorter table that decodes the
   shorter, more probable codes, and then point to subsidiary tables for
   the longer codes.  The time it costs to decode the longer codes is
   then traded against the time it takes to make longer tables.

   This results of this trade are in the variables lbits and dbits
   below.  lbits is the number of bits the first level table for literal/
   length codes can decode in one step, and dbits is the same thing for
   the distance codes.  Subsequent tables are also less than or equal to
   those sizes.  These values may be adjusted either when all of the
   codes are shorter than that, in which case the longest code length in
   bits is used, or when the shortest code is *longer* than the requested
   table size, in which case the length of the shortest code in bits is
   used.

   There are two different values for the two tables, since they code a
   different number of possibilities each.  The literal/length table
   codes 286 possible values, or in a flat code, a little over eight
   bits.  The distance table codes 30 possible values, or a little less
   than five bits, flat.  The optimum values for speed end up being
   about one bit more than those, so lbits is 8+1 and dbits is 5+1.
   The optimum values may differ though from machine to machine, and
   possibly even between compilers.  Your mileage may vary.
 */


int lbits = 9;          /* bits in base literal/length lookup table */
int dbits = 6;          /* bits in base distance lookup table */


/* If BMAX needs to be larger than 16, then h and x[] should be ULONG. */
#define BMAX 16         /* maximum bit length of any code (16 for explode) */
#define NMAX 288        /* maximum number of codes in any set */


unsigned hufts;         /* track memory usage */


int huft_build(b, n, s, d, e, t, m)
unsigned *b;            /* code lengths in bits (all assumed <= BMAX) */
unsigned n;             /* number of codes (assumed <= NMAX) */
unsigned s;             /* number of simple-valued codes (0..s-1) */
UWORD *d;               /* list of base values for non-simple codes */
UWORD *e;               /* list of extra bits for non-simple codes */
struct huft **t;        /* result: starting table */
int *m;                 /* maximum lookup bits, returns actual */
/* Given a list of code lengths and a maximum table size, make a set of
   tables to decode that set of codes.  Return zero on success, one if
   the given code set is incomplete (the tables are still built in this
   case), two if the input is invalid (all zero length codes or an
   oversubscribed set of lengths), and three if not enough memory. */
{
  unsigned a;                   /* counter for codes of length k */
  unsigned c[BMAX+1];           /* bit length count table */
  unsigned f;                   /* i repeats in table every f entries */
  int g;                        /* maximum code length */
  int h;                        /* table level */
  register unsigned i;          /* counter, current code */
  register unsigned j;          /* counter */
  register int k;               /* number of bits in current code */
  int l;                        /* bits per table (returned in m) */
  register unsigned *p;         /* pointer into c[], b[], or v[] */
  register struct huft *q;      /* points to current table */
  struct huft r;                /* table entry for structure assignment */
  struct huft *u[BMAX];         /* table stack */
  unsigned v[NMAX];             /* values in order of bit length */
  register int w;               /* bits before this table == (l * h) */
  unsigned x[BMAX+1];           /* bit offsets, then code stack */
  unsigned *xp;                 /* pointer into x */
  int y;                        /* number of dummy codes added */
  unsigned z;                   /* number of entries in current table */


  /* Generate counts for each bit length */
  memset(c, 0, sizeof(c));
  p = b;  i = n;
  do {
    c[*p++]++;                  /* assume all entries <= BMAX */
  } while (--i);
  if (c[0] == n)
    return 2;                   /* bad input--all zero length codes */


  /* Find minimum and maximum length, bound *m by those */
  l = *m;
  for (j = 1; j <= BMAX; j++)
    if (c[j])
      break;
  k = j;                        /* minimum code length */
  if (l < j)
    l = j;
  for (i = BMAX; i; i--)
    if (c[i])
      break;
  g = i;                        /* maximum code length */
  if (l > i)
    l = i;
  *m = l;


  /* Adjust last length count to fill out codes, if needed */
  for (y = 1 << j; j < i; j++, y <<= 1)
    if ((y -= c[j]) < 0)
      return 2;                 /* bad input: more codes than bits */
  if ((y -= c[i]) < 0)
    return 2;
  c[i] += y;


  /* Generate starting offsets into the value table for each length */
  x[1] = j = 0;
  p = c + 1;  xp = x + 2;
  while (--i) {                 /* note that i == g from above */
    *xp++ = (j += *p++);
  }


  /* Make a table of values in order of bit lengths */
  p = b;  i = 0;
  do {
    if ((j = *p++) != 0)
      v[x[j]++] = i;
  } while (++i < n);


  /* Generate the Huffman codes and for each, make the table entries */
  x[0] = i = 0;                 /* first Huffman code is zero */
  p = v;                        /* grab values in bit order */
  h = -1;                       /* no tables yet--level -1 */
  w = -l;                       /* bits decoded == (l * h) */
  u[0] = NULL;  q = NULL;  z = 0;       /* just to keep compilers happy */

  /* go through the bit lengths (k already is bits in shortest code) */
  for (; k <= g; k++)
  {
    a = c[k];
    while (a--)
    {
      /* here i is the Huffman code of length k bits for value *p */
      /* make tables up to required level */
      while (k > w + l)
      {
        h++;
        w += l;                 /* previous table always l bits */

        /* compute minimum size table less than or equal to l bits */
        z = (z = g - w) > l ? l : z;    /* upper limit on table size */
        if ((f = 1 << (j = k - w)) > a + 1)     /* try a k-w bit table */
        {                       /* too few codes for k-w bit table */
	  f -= a + 1;           /* deduct codes from patterns left */
          xp = c + k;
          while (++j < z)       /* try smaller tables up to z bits */
          {
            if ((f <<= 1) <= *++xp)
              break;            /* enough codes to use up j bits */
            f -= *xp;           /* else deduct codes from patterns */
          }
        }
        z = 1 << j;             /* table entries for j-bit table */

        /* allocate and link in new table */
        if ((q = (struct huft *)malloc((z + 1)*sizeof(struct huft))) == NULL)
        {
          if (h)
            huft_free(u[0]);
          fprintf(stderr, "\n*** inflate out of memory *** ");
          return 3;             /* not enough memory */
        }
        hufts += z + 1;         /* track memory usage */
        *t = q + 1;             /* link to list for huft_free() */
        *(t = &(q->v.t)) = NULL;
        u[h] = ++q;             /* table starts after link */

        /* connect to last table, if there is one */
        if (h)
        {
          x[h] = i;             /* save pattern for backing up */
          r.b = l;              /* bits to dump before this table */
          r.e = 16 + j;         /* bits in this table */
          r.v.t = q;            /* pointer to this table */
          j = i >> (w - l);     /* (get around Turbo C bug) */
          u[h-1][j] = r;        /* connect to last table */
        }
      }

      /* set up table entry in r */
      r.b = k - w;
      if (p >= v + n)
        r.e = 99;               /* out of values--invalid code */
      else if (*p < s)
      {
        r.e = *p < 256 ? 16 : 15;       /* 256 is end-of-block code */
        r.v.n = *p++;           /* simple code is just the value */
      }
      else
      {
        r.e = e[*p - s];        /* non-simple--look up in lists */
        r.v.n = d[*p++ - s];
      }

      /* fill code-like entries with r */
      f = 1 << (k - w);
      for (j = i >> w; j < z; j += f)
        q[j] = r;

      /* backwards increment the k-bit code i */
      for (j = 1 << (k - 1); i & j; j >>= 1)
        i ^= j;
      i ^= j;

      /* backup over finished tables */
      while ((i & ((1 << w) - 1)) != x[h])
      {
        h--;                    /* don't need to update q */
        w -= l;
      }
    }
  }


  /* Return true (1) if we were given an incomplete table */
  return y != 0 && n != 1;
}



int huft_free(t)
struct huft *t;         /* table to free */
/* Free the malloc'ed tables built by huft_build(), which makes a linked
   list of the tables it made, with the links in a dummy first entry of
   each table. */
{
  register struct huft *p, *q;


  /* Go through linked list, freeing from the malloced (t[-1]) address. */
  p = t;
  while (p != NULL)
  {
    q = (--p)->v.t;
    free(p);
    p = q;
  } 
  return 0;
}



void flush(w)
unsigned w;             /* number of bytes to flush */
/* Do the equivalent of OUTB for the bytes slide[0..w-1]. */
{
  unsigned n;
  byte *p;

  p = (byte*)slide;
  while (w)
  {
    n = (n = OUTBUFSIZ - outcnt) < w ? n : w;
    memcpy(outptr, p, n);       /* try to fill up buffer */
    outptr += n;
    if ((outcnt += n) == OUTBUFSIZ)
      if (FlushOutput())            /* if full, empty */
	  {
		fprintf(stderr, "\nWrite error.\n");
		exitPGP(1);
	  }
    p += n;
    w -= n;
  }
}



int inflate_codes(tl, td, bl, bd)
struct huft *tl, *td;   /* literal/length and distance decoder tables */
int bl, bd;             /* number of bits decoded by tl[] and td[] */
/* inflate (decompress) the codes in a deflated (compressed) block.
   Return an error code or zero if it all goes ok. */
{
  register unsigned e;  /* table entry flag/number of extra bits */
  unsigned n, d;        /* length and index for copy */
  unsigned w;           /* current window position */
  struct huft *t;       /* pointer to table entry */
  ULONG ml, md;         /* masks for bl and bd bits */
  register ULONG b;     /* bit buffer */
  register unsigned k;  /* number of bits in bit buffer */


  /* make local copies of globals */
  b = bb;                       /* initialize bit buffer */
  k = bk;
  w = wp;                       /* initialize window position */


  /* inflate the coded data */
  ml = mask_bits[bl];           /* precompute masks for speed */
  md = mask_bits[bd];
  while (1)                     /* do until end of block */
  {
    NEEDBITS(bl)
    if ((e = (t = tl + (b & ml))->e) > 16)
      do {
        if (e == 99)
          return 1;
        DUMPBITS(t->b)
        e -= 16;
        NEEDBITS(e)
      } while ((e = (t = t->v.t + (b & mask_bits[e]))->e) > 16);
    DUMPBITS(t->b)
    if (e == 16)                /* then it's a literal */
    {
      slide[w++] = t->v.n;
      if (w == WSIZE)
      {
        flush(w);
        w = 0;
      }
    }
    else                        /* it's an EOB or a length */
    {
      /* exit if end of block */
      if (e == 15)
        break;

      /* get length of block to copy */
      NEEDBITS(e)
      n = t->v.n + (b & mask_bits[e]);
      DUMPBITS(e);

      /* decode distance of block to copy */
      NEEDBITS(bd)
      if ((e = (t = td + (b & md))->e) > 16)
        do {
          if (e == 99)
            return 1;
          DUMPBITS(t->b)
          e -= 16;
          NEEDBITS(e)
        } while ((e = (t = t->v.t + (b & mask_bits[e]))->e) > 16);
      DUMPBITS(t->b)
      NEEDBITS(e)
      d = w - t->v.n - (b & mask_bits[e]);
      DUMPBITS(e)

      /* do the copy */
      do {
        n -= (e = (e = WSIZE - ((d &= WSIZE-1) > w ? d : w)) > n ? n : e);
#ifndef NOMEMCPY
        if (w - d >= e)         /* (this test assumes unsigned comparison) */
        {
          memcpy(slide + w, slide + d, e);
          w += e;
	  d += e;
        }
        else                      /* do it slow to avoid memcpy() overlap */
#endif /* !NOMEMCPY */
          do {
            slide[w++] = slide[d++];
          } while (--e);
        if (w == WSIZE)
        {
          flush(w);
          w = 0;
        }
      } while (n);
    }
  }


  /* restore the globals from the locals */
  wp = w;                       /* restore global window pointer */
  bb = b;                       /* restore global bit buffer */
  bk = k;


  /* done */
  return 0;
}



int inflate_stored()
/* "decompress" an inflated type 0 (stored) block. */
{
  unsigned n;           /* number of bytes in block */
  unsigned w;           /* current window position */
  register ULONG b;     /* bit buffer */
  register unsigned k;  /* number of bits in bit buffer */


  /* make local copies of globals */
  b = bb;                       /* initialize bit buffer */
  k = bk;
  w = wp;                       /* initialize window position */


  /* go to byte boundary */
  n = k & 7;
  DUMPBITS(n);


  /* get the length and its complement */
  NEEDBITS(16)
  n = b & 0xffff;
  DUMPBITS(16)
  NEEDBITS(16)
  if (n != ((~b) & 0xffff))
    return 1;                   /* error in compressed data */
  DUMPBITS(16)


  /* read and output the compressed data */
  while (n--)
  {
    NEEDBITS(8)
    slide[w++] = b;
    if (w == WSIZE)
    {
      flush(w);
      w = 0;
    }
    DUMPBITS(8)
  }


  /* restore the globals from the locals */
  wp = w;                       /* restore global window pointer */
  bb = b;                       /* restore global bit buffer */
  bk = k;
  return 0;
}



int inflate_fixed()
/* decompress an inflated type 1 (fixed Huffman codes) block.  We should
   either replace this with a custom decoder, or at least precompute the
   Huffman tables. */
{
  int i;                /* temporary variable */
  struct huft *tl;      /* literal/length code table */
  struct huft *td;      /* distance code table */
  int bl;               /* lookup bits for tl */
  int bd;               /* lookup bits for td */
  unsigned l[288];      /* length list for huft_build */


  /* set up literal table */
  for (i = 0; i < 144; i++)
    l[i] = 8;
  for (; i < 256; i++)
    l[i] = 9;
  for (; i < 280; i++)
    l[i] = 7;
  for (; i < 288; i++)          /* make a complete, but wrong code set */
    l[i] = 8;
  bl = 7;
  if ((i = huft_build(l, 288, 257, cplens, cplext, &tl, &bl)) != 0)
    return i;


  /* set up distance table */
  for (i = 0; i < 30; i++)      /* make an incomplete code set */
    l[i] = 5;
  bd = 5;
  if ((i = huft_build(l, 30, 0, cpdist, cpdext, &td, &bd)) > 1)
  {
    huft_free(tl);
    return i;
  }


  /* decompress until an end-of-block code */
  if (inflate_codes(tl, td, bl, bd))
    return 1;


  /* free the decoding tables, return */
  huft_free(tl);
  huft_free(td);
  return 0;
}



int inflate_dynamic()
/* decompress an inflated type 2 (dynamic Huffman codes) block. */
{
  int i;                /* temporary variables */
  unsigned j;
  unsigned l;           /* last length */
  unsigned m;           /* mask for bit lengths table */
  unsigned n;           /* number of lengths to get */
  struct huft *tl;      /* literal/length code table */
  struct huft *td;      /* distance code table */
  int bl;               /* lookup bits for tl */
  int bd;               /* lookup bits for td */
  unsigned nb;          /* number of bit length codes */
  unsigned nl;          /* number of literal/length codes */
  unsigned nd;          /* number of distance codes */
  unsigned ll[286+30];  /* literal/length and distance code lengths */
  register ULONG b;     /* bit buffer */
  register unsigned k;  /* number of bits in bit buffer */


  /* make local bit buffer */
  b = bb;
  k = bk;


  /* read in table lengths */
  NEEDBITS(5)
  nl = 257 + (b & 0x1f);        /* number of literal/length codes */
  DUMPBITS(5)
  NEEDBITS(5)
  nd = 1 + (b & 0x1f);          /* number of distance codes */
  DUMPBITS(5)
  NEEDBITS(4)
  nb = 4 + (b & 0xf);           /* number of bit length codes */
  DUMPBITS(4)
  if (nl > 286 || nd > 30)
    return 1;                   /* bad lengths */


  /* read in bit-length-code lengths */
  for (i = 0; i < nb; i++)
  {
    NEEDBITS(3)
    ll[border[i]] = b & 7;
    DUMPBITS(3)
  }
  for (; i < 19; i++)
    ll[border[i]] = 0;


  /* build decoding table for trees--single level, 7 bit lookup */
  bl = 7;
  if ((i = huft_build(ll, 19, 19, NULL, NULL, &tl, &bl)) != 0)
  {
    if (i == 1)
      huft_free(tl);
    return i;                   /* incomplete code set */
  }


  /* read in literal and distance code lengths */
  n = nl + nd;
  m = mask_bits[bl];
  i = l = 0;
  while (i < n)
  {
    NEEDBITS(bl)
    j = (td = tl + (b & m))->b;
    DUMPBITS(j)
    j = td->v.n;
    if (j < 16)                 /* length of code in bits (0..15) */
      ll[i++] = l = j;          /* save last length in l */
    else if (j == 16)           /* repeat last length 3 to 6 times */
    {
      NEEDBITS(2)
      j = 3 + (b & 3);
      DUMPBITS(2)
      if (i + j > n)
        return 1;
      while (j--)
        ll[i++] = l;
    }
    else if (j == 17)           /* 3 to 10 zero length codes */
    {
      NEEDBITS(3)
      j = 3 + (b & 7);
      DUMPBITS(3)
      if (i + j > n)
        return 1;
      while (j--)
        ll[i++] = 0;
      l = 0;
    }
    else                        /* j == 18: 11 to 138 zero length codes */
    {
      NEEDBITS(7)
      j = 11 + (b & 0x7f);
      DUMPBITS(7)
      if (i + j > n)
        return 1;
      while (j--)
        ll[i++] = 0;
      l = 0;
    }
  }


  /* free decoding table for trees */
  huft_free(tl);


  /* restore the global bit buffer */
  bb = b;
  bk = k;


  /* build the decoding tables for literal/length and distance codes */
  bl = lbits;
  if ((i = huft_build(ll, nl, 257, cplens, cplext, &tl, &bl)) != 0)
  {
    if (i == 1)
      huft_free(tl);
    return i;                   /* incomplete code set */
  }
  bd = dbits;
  if ((i = huft_build(ll + nl, nd, 0, cpdist, cpdext, &td, &bd)) != 0)
  {
    if (i == 1)
      huft_free(td);
    huft_free(tl);
    return i;                   /* incomplete code set */
  }


  /* decompress until an end-of-block code */
  if (inflate_codes(tl, td, bl, bd))
    return 1;


  /* free the decoding tables, return */
  huft_free(tl);
  huft_free(td);
  return 0;
}



int inflate_block(e)
int *e;                 /* last block flag */
/* decompress an inflated block */
{
  unsigned t;           /* block type */
  register ULONG b;     /* bit buffer */
  register unsigned k;  /* number of bits in bit buffer */


  /* make local bit buffer */
  b = bb;
  k = bk;


  /* read in last block bit */
  NEEDBITS(1)
  *e = b & 1;
  DUMPBITS(1)


  /* read in block type */
  NEEDBITS(2)
  t = b & 3;
  DUMPBITS(2)


  /* restore the global bit buffer */
  bb = b;
  bk = k;


  /* inflate that block type */
  if (t == 2)
    return inflate_dynamic();
  if (t == 0)
    return inflate_stored();
  if (t == 1)
    return inflate_fixed();


  /* bad block type */
  return 2;
}



int inflate_entry()
/* decompress an inflated entry */
{
  int e;                /* last block flag */
  int r;                /* result code */
  unsigned h;           /* maximum struct huft's malloc'ed */


  /* initialize window, bit buffer */
  wp = 0;
  bk = 0;
  bb = 0;


  /* decompress until the last block */
  h = 0;
  do {
    hufts = 0;
    if ((r = inflate_block(&e)) != 0)
      return r;
    if (hufts > h)
      h = hufts;
#ifdef MACTC5
	mac_poll_for_break();
#endif
  } while (!e);


  /* flush out slide */
  flush(wp);


  /* return success */
#ifdef DEBUG
  fprintf(stderr, "<%u> ", h);
#endif /* DEBUG */
  return 0;
}


int inflate()
/* ignore the return code for now ... */
{
  int status;

#ifdef DYN_ALLOC
  slide = (char*)  calloc((unsigned)WSIZE, 2*sizeof(char));
    /* Note that inflate only needs WSIZE bytes, but the slide
     * array is shared with deflate, which needs 2*WISZE bytes.
     */
  if (slide==NULL) err(4, "");
#endif

  status = inflate_entry();

#ifdef DYN_ALLOC
  free(slide);
  slide = NULL;
#endif

  return status;
}


/* ==== zip.c ==== */
/* Support code for the zip/unzip code - just handles error messages.  To
   get exact errors, define ZIPDEBUG */

#include <stdio.h>
#include <stdlib.h>
#include "usuals.h"
#include "fileio.h"
#include "language.h"
#include "pgp.h"
#include "exitpgp.h"
#include "zip.h"

#include "ziperr.h" /* for ZE_MEM (and errors[] if ZIPDEBUG defined) */

/* Clean error exit: c is a ZE_-class error, *msg is an error message.
   Issue a message for the error, clean up files and memory, and exit */

void err(int c, char *msg)
{

#ifdef ZIPDEBUG
	if (PERR(c))
		perror("zip error");
	fprintf(stderr, "zip error: %s (%s)\n", errors[c-1], msg);
#endif /* ZIPDEBUG */

	/* Complain and return and out of memory error code */
	if(c==ZE_MEM) {
		fprintf( stderr, LANG("\nOut of memory\n") );
		exitPGP( 7 );
	} else {
		fprintf(stderr,LANG("\nCompression/decompression error\n") );
		/* Yuck */
		exitPGP( 23 );
	}
}

/* Internal error, should never happen */

void error(char *msg)
{
	err(-1, msg);
}


/* ==== zipup.c ==== */
/*

 Copyright (C) 1990,1991 Mark Adler, Richard B. Wales, and Jean-loup Gailly.
 Permission is granted to any individual or institution to use, copy, or
 redistribute this software so long as all of the original files are included
 unmodified, that it is not sold for profit, and that this copyright notice
 is retained.

*/

/*
 *  zipup.c by Mark Adler. Includes modifications by Jean-loup Gailly.
 */

#define NOCPYRT         /* this is not a main module */
#include <ctype.h>
#include "zip.h"
#include "zrevisio.h"
#include "system.h"

/* Use the raw functions for MSDOS and Unix to save on buffer space.
   They're not used for VMS since it doesn't work (raw is weird on VMS).
   (This sort of stuff belongs in fileio.c, but oh well.) */
#if defined(VMS) || defined(C370)
   typedef FILE *ftype;
#  define fhow FOPR
#  define fbad NULL
#  define zopen(n,p) fopen(n,p)
#  define zread(f,b,n) fread(b,1,n,f)
#  define zclose(f) fclose(f)
#  define zerr(f) ferror(f)
#  define zrew(f) rewind(f)
#  define zstdin stdin
#else /* !VMS */
#  if defined(MSDOS) || defined(WIN32)
#    include <io.h>
#    include <fcntl.h>
#    define fhow (O_RDONLY|O_BINARY)
#  elif defined(MACTC5) /* Macintosh */
#    define fhow 0
#  else /* !MSDOS && !Macintosh */
#    ifndef HAVE_UNISTD_H
       size_t lseek(int handle, size_t offset, int whence);
#    endif /* !HAVE_UNISTD_H */
#    define fhow 0
#  endif /* ?MSDOS */
   typedef int ftype;
#  define fbad (-1)
#  define zopen(n,p) open(n,p)
#  define zread(f,b,n) read(f,b,n)
#  define zclose(f) close(f)
#  define zerr(f) (k==(extent)(-1L))
#  define zrew(f) lseek(f,0L,0)
#  define zstdin 0
#endif /* ?VMS */

/* Local data */

local ftype ifile;		/* file to compress */

void lm_free();
void ct_free();

/* Compress the file fileName and write it to the file *y. Return an error
   code in the ZE_ class.  Also, update tempzn by the number of bytes written.
*/
int zipup(FILE *inFile, FILE *y)
/* ??? Does not yet handle non-seekable y */
{
  int m;				/* method for this entry */
  long q = -1L;			/* size returned by filetime */
  ush att;			/* internal file attributes (dummy only) */
  ush flg;				/* gp compresion flags (dummy only) */

	/* Set input file and find its size */
#if defined(VMS) || defined(C370)
	ifile = inFile;
	fseek(ifile, 0L, SEEK_END);
	q = ftell(ifile);
	fseek(ifile, 0L, SEEK_SET);
#else
	ifile = fileno( inFile );
	q = lseek(ifile, 0L, SEEK_END);
	lseek(ifile, 0L, SEEK_SET);
#endif /* VMS */

	m = (q == 0) ? STORE : DEFLATE;

  if (m == DEFLATE) {
	 bi_init(y);
	 att = UNKNOWN;
	 ct_init(&att, &m);
	 lm_init(level, &flg);
	 /* s = */ deflate();
  }
  lm_free();
  ct_free();

  return(0);
}

int read_buf(buf, size)
  char far *buf;
  unsigned size;
/* Read a new buffer from the current input file, and update the crc and
 * input file size.
 * IN assertion: size >= 2 (for end-of-line translation) */
{
  unsigned len;

  len = zread(ifile, buf, size);
  if (len == (unsigned)EOF || len == 0) return len;
  return len;
}


/* ==== ztrees.c ==== */
/*

 Copyright (C) 1990-1992 Mark Adler, Richard B. Wales, Jean-loup Gailly,
 Kai Uwe Rommel and Igor Mandrichenko.
 Permission is granted to any individual or institution to use, copy, or
 redistribute this software so long as all of the original files are included
 unmodified, that it is not sold for profit, and that this copyright notice
 is retained.

*/

/*
 *  trees.c by Jean-loup Gailly
 *
 *  This is a new version of im_ctree.c originally written by Richard B. Wales
 *  for the defunct implosion method.
 *
 *  PURPOSE
 *
 *      Encode various sets of source values using variable-length
 *      binary code trees.
 *
 *  DISCUSSION
 *
 *      The PKZIP "deflation" process uses several Huffman trees. The more
 *      common source values are represented by shorter bit sequences.
 *
 *      Each code tree is stored in the ZIP file in a compressed form
 *      which is itself a Huffman encoding of the lengths of
 *      all the code strings (in ascending order by source values).
 *      The actual code strings are reconstructed from the lengths in
 *      the UNZIP process, as described in the "application note"
 *      (APPNOTE.TXT) distributed as part of PKWARE's PKZIP program.
 *
 *  REFERENCES
 *
 *      Lynch, Thomas J.
 *          Data Compression:  Techniques and Applications, pp. 53-55.
 *          Lifetime Learning Publications, 1985.  ISBN 0-534-03418-7.
 *
 *      Storer, James A.
 *          Data Compression:  Methods and Theory, pp. 49-50.
 *          Computer Science Press, 1988.  ISBN 0-7167-8156-5.
 *
 *      Sedgewick, R.
 *          Algorithms, p290.
 *          Addison-Wesley, 1983. ISBN 0-201-06672-6.
 *
 *  INTERFACE
 *
 *      void ct_init (ush *attr, int *method)
 *          Allocate the match buffer, initialize the various tables and save
 *          the location of the internal file attribute (ascii/binary) and
 *          method (DEFLATE/STORE)
 *
 *      void ct_tally (int dist, int lc);
 *          Save the match info and tally the frequency counts.
 *
 *      long flush_block (char *buf, ulg stored_len, int eof)
 *          Determine the best encoding for the current block: dynamic trees,
 *          static trees or store, and output the encoded block to the zip
 *          file. Returns the total compressed length for the file so far.
 *
 */

#include <ctype.h>
#include "zip.h"

/* ===========================================================================
 * Constants
 */

#define MAX_BITS 15
/* All codes must not exceed MAX_BITS bits */

#define MAX_BL_BITS 7
/* Bit length codes must not exceed MAX_BL_BITS bits */

#define LENGTH_CODES 29
/* number of length codes, not counting the special END_BLOCK code */

#define LITERALS  256
/* number of literal bytes 0..255 */

#define END_BLOCK 256
/* end of block literal code */

#define L_CODES (LITERALS+1+LENGTH_CODES)
/* number of Literal or Length codes, including the END_BLOCK code */

#define D_CODES   30
/* number of distance codes */

#define BL_CODES  19
/* number of codes used to transfer the bit lengths */


local int near extra_lbits[LENGTH_CODES] /* extra bits for each length code */
   = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};

local int near extra_dbits[D_CODES] /* extra bits for each distance code */
   = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

local int near extra_blbits[BL_CODES]/* extra bits for each bit length code */
   = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7};

#define STORED_BLOCK 0
#define STATIC_TREES 1
#define DYN_TREES    2
/* The three kinds of block type */

#ifndef LIT_BUFSIZE
#  ifdef SMALL_MEM
#    define LIT_BUFSIZE  0x2000
#  else
#  ifdef MEDIUM_MEM
#    define LIT_BUFSIZE  0x4000
#  else
#    define LIT_BUFSIZE  0x8000
#  endif
#  endif
#endif
#define DIST_BUFSIZE  LIT_BUFSIZE
/* Sizes of match buffers for literals/lengths and distances.  There are
 * 4 reasons for limiting LIT_BUFSIZE to 64K:
 *   - frequencies can be kept in 16 bit counters
 *   - if compression is not successful for the first block, all input data is
 *     still in the window so we can still emit a stored block even when input
 *     comes from standard input.  (This can also be done for all blocks if
 *     LIT_BUFSIZE is not greater than 32K.)
 *   - if compression is not successful for a file smaller than 64K, we can
 *     even emit a stored file instead of a stored block (saving 5 bytes).
 *   - creating new Huffman trees less frequently may not provide fast
 *     adaptation to changes in the input data statistics. (Take for
 *     example a binary file with poorly compressible code followed by
 *     a highly compressible string table.) Smaller buffer sizes give
 *     fast adaptation but have of course the overhead of transmitting trees
 *     more frequently.
 *   - I can't count above 4
 * The current code is general and allows DIST_BUFSIZE < LIT_BUFSIZE (to save
 * memory at the expense of compression). Some optimizations would be possible
 * if we rely on DIST_BUFSIZE == LIT_BUFSIZE.
 */

#define REP_3_6      16
/* repeat previous bit length 3-6 times (2 bits of repeat count) */

#define REPZ_3_10    17
/* repeat a zero length 3-10 times  (3 bits of repeat count) */

#define REPZ_11_138  18
/* repeat a zero length 11-138 times  (7 bits of repeat count) */

/* ===========================================================================
 * Local data
 */

/* Data structure describing a single value and its code string. */
typedef struct ct_data {
    union {
        ush  freq;       /* frequency count */
        ush  code;       /* bit string */
    } fc;
    union {
        ush  dad;        /* father node in Huffman tree */
        ush  len;        /* length of bit string */
    } dl;
} ct_data;

#define Freq fc.freq
#define Code fc.code
#define Dad  dl.dad
#define Len  dl.len

#define HEAP_SIZE (2*L_CODES+1)
/* maximum heap size */

local ct_data near dyn_ltree[HEAP_SIZE];   /* literal and length tree */
local ct_data near dyn_dtree[2*D_CODES+1]; /* distance tree */

local ct_data near static_ltree[L_CODES+2];
/* The static literal tree. Since the bit lengths are imposed, there is no
 * need for the L_CODES extra codes used during heap construction. However
 * The codes 286 and 287 are needed to build a canonical tree (see ct_init
 * below).
 */

local ct_data near static_dtree[D_CODES];
/* The static distance tree. (Actually a trivial tree since all codes use
 * 5 bits.)
 */

local ct_data near bl_tree[2*BL_CODES+1];
/* Huffman tree for the bit lengths */

typedef struct tree_desc {
    ct_data near *dyn_tree;      /* the dynamic tree */
    ct_data near *static_tree;   /* corresponding static tree or NULL */
    int     near *extra_bits;    /* extra bits for each code or NULL */
    int     extra_base;          /* base index for extra_bits */
    int     elems;               /* max number of elements in the tree */
    int     max_length;          /* max bit length for the codes */
    int     max_code;            /* largest code with non zero frequency */
} tree_desc;

local tree_desc near l_desc =
{dyn_ltree, static_ltree, extra_lbits, LITERALS+1, L_CODES, MAX_BITS, 0};

local tree_desc near d_desc =
{dyn_dtree, static_dtree, extra_dbits, 0,          D_CODES, MAX_BITS, 0};

local tree_desc near bl_desc =
{bl_tree, NULL,       extra_blbits, 0,         BL_CODES, MAX_BL_BITS, 0};


local ush near bl_count[MAX_BITS+1];
/* number of codes at each bit length for an optimal tree */

local uch near bl_order[BL_CODES]
   = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
/* The lengths of the bit length codes are sent in order of decreasing
 * probability, to avoid transmitting the lengths for unused bit length codes.
 */

local int near heap[2*L_CODES+1]; /* heap used to build the Huffman trees */
local int heap_len;               /* number of elements in the heap */
local int heap_max;               /* element of largest frequency */
/* The sons of heap[n] are heap[2*n] and heap[2*n+1]. heap[0] is not used.
 * The same heap array is used to build all trees.
 */

local uch near depth[2*L_CODES+1];
/* Depth of each subtree used as tie breaker for trees of equal frequency */

local uch length_code[MAX_MATCH-MIN_MATCH+1];
/* length code for each normalized match length (0 == MIN_MATCH) */

local uch dist_code[512];
/* distance codes. The first 256 values correspond to the distances
 * 3 .. 258, the last 256 values correspond to the top 8 bits of
 * the 15 bit distances.
 */

local int near base_length[LENGTH_CODES];
/* First normalized length for each code (0 = MIN_MATCH) */

local int near base_dist[D_CODES];
/* First normalized distance for each code (0 = distance of 1) */

#ifndef DYN_ALLOC
  local uch far l_buf[LIT_BUFSIZE];  /* buffer for literals/lengths */
  local ush far d_buf[DIST_BUFSIZE]; /* buffer for distances */
#else
  local uch far *l_buf;
  local ush far *d_buf;
#endif

local uch near flag_buf[(LIT_BUFSIZE/8)];
/* flag_buf is a bit array distinguishing literals from lengths in
 * l_buf, and thus indicating the presence or absence of a distance.
 */

local unsigned last_lit;    /* running index in l_buf */
local unsigned last_dist;   /* running index in d_buf */
local unsigned last_flags;  /* running index in flag_buf */
local uch flags;            /* current flags not yet saved in flag_buf */
local uch flag_bit;         /* current bit used in flags */
/* bits are filled in flags starting at bit 0 (least significant).
 * Note: these flags are overkill in the current code since we don't
 * take advantage of DIST_BUFSIZE == LIT_BUFSIZE.
 */

local ulg opt_len;        /* bit length of current block with optimal trees */
local ulg static_len;     /* bit length of current block with static trees */

local ulg compressed_len; /* total bit length of compressed file */

local ulg input_len;      /* total byte length of input file */
/* input_len is for debugging only since we can get it by other means. */

static ush *file_type;        /* pointer to UNKNOWN, BINARY or ASCII */
static int *file_method;      /* pointer to DEFLATE or STORE */

#ifdef DEBUG
extern ulg bits_sent;  /* bit length of the compressed data */
extern ulg isize;      /* byte length of input file */
#endif

extern long block_start;       /* window offset of current block */
extern unsigned near strstart; /* window offset of current string */

/* ===========================================================================
 * Local (static) routines in this file.
 */

local void init_block     OF((void));
local void pqdownheap     OF((ct_data near *tree, int k));
local void gen_bitlen     OF((tree_desc near *desc));
local void gen_codes      OF((ct_data near *tree, int max_code));
local void build_tree     OF((tree_desc near *desc));
local void scan_tree      OF((ct_data near *tree, int max_code));
local void send_tree      OF((ct_data near *tree, int max_code));
local int  build_bl_tree  OF((void));
local void send_all_trees OF((int lcodes, int dcodes, int blcodes));
local void compress_block OF((ct_data near *ltree, ct_data near *dtree));
local void set_file_type  OF((void));


#ifndef DEBUG
#  define send_code(c, tree) send_bits(tree[c].Code, tree[c].Len)
   /* Send a code of the given tree. c and tree must not have side effects */

#else /* DEBUG */
#  define send_code(c, tree) \
     { if (verbose) fprintf(stderr,"\ncd %3d ",(c)); \
       send_bits(tree[c].Code, tree[c].Len); }
#endif

#define d_code(dist) \
   ((dist) < 256 ? dist_code[dist] : dist_code[256+((dist)>>7)])
/* Mapping from a distance to a distance code. dist is the distance - 1 and
 * must not have side effects. dist_code[256] and dist_code[257] are never
 * used.
 */

#define MAX(a,b) (a >= b ? a : b)
/* the arguments must not have side effects */

/* ===========================================================================
 * Allocate the match buffer, initialize the various tables and save the
 * location of the internal file attribute (ascii/binary) and method
 * (DEFLATE/STORE).
 */
void ct_init(attr, Method)
    ush  *attr;   /* pointer to internal file attribute */
    int  *Method; /* pointer to compression method */
{
    int n;        /* iterates over tree elements */
    int bits;     /* bit counter */
    int length;   /* length value */
    int code;     /* code value */
    int dist;     /* distance index */

    file_type = attr;
    file_method = Method;
    compressed_len = input_len = 0L;
        
#ifdef DYN_ALLOC
    d_buf = (ush far*) fcalloc(DIST_BUFSIZE, sizeof(ush));
    l_buf = (uch far*) fcalloc(LIT_BUFSIZE/2, 2);
    /* Avoid using the value 64K on 16 bit machines */
    if (l_buf == NULL || d_buf == NULL) error("ct_init: out of memory");
#endif

    if (static_dtree[0].Len != 0) return; /* ct_init already called */

    /* Initialize the mapping length (0..255) -> length code (0..28) */
    length = 0;
    for (code = 0; code < LENGTH_CODES-1; code++) {
        base_length[code] = length;
        for (n = 0; n < (1<<extra_lbits[code]); n++) {
            length_code[length++] = (uch)code;
        }
    }
    Assert (length == 256, "ct_init: length != 256");
    /* Note that the length 255 (match length 258) can be represented
     * in two different ways: code 284 + 5 bits or code 285, so we
     * overwrite length_code[255] to use the best encoding:
     */
    length_code[length-1] = (uch)code;

    /* Initialize the mapping dist (0..32K) -> dist code (0..29) */
    dist = 0;
    for (code = 0 ; code < 16; code++) {
        base_dist[code] = dist;
        for (n = 0; n < (1<<extra_dbits[code]); n++) {
            dist_code[dist++] = (uch)code;
        }
    }
    Assert (dist == 256, "ct_init: dist != 256");
    dist >>= 7; /* from now on, all distances are divided by 128 */
    for ( ; code < D_CODES; code++) {
        base_dist[code] = dist << 7;
        for (n = 0; n < (1<<(extra_dbits[code]-7)); n++) {
            dist_code[256 + dist++] = (uch)code;
        }
    }
    Assert (dist == 256, "ct_init: 256+dist != 512");

    /* Construct the codes of the static literal tree */
    for (bits = 0; bits <= MAX_BITS; bits++) bl_count[bits] = 0;
    n = 0;
    while (n <= 143) static_ltree[n++].Len = 8, bl_count[8]++;
    while (n <= 255) static_ltree[n++].Len = 9, bl_count[9]++;
    while (n <= 279) static_ltree[n++].Len = 7, bl_count[7]++;
    while (n <= 287) static_ltree[n++].Len = 8, bl_count[8]++;
    /* Codes 286 and 287 do not exist, but we must include them in the
     * tree construction to get a canonical Huffman tree (longest code
     * all ones)
     */
    gen_codes(static_ltree, L_CODES+1);

    /* The static distance tree is trivial: */
    for (n = 0; n < D_CODES; n++) {
        static_dtree[n].Len = 5;
        static_dtree[n].Code = bi_reverse(n, 5);
    }

    /* Initialize the first block of the first file: */
    init_block();
}

void ct_free()
{
#ifdef DYN_ALLOC
#ifndef __TURBOC__          /*EWS*/
	fcfree(d_buf);
	fcfree(l_buf);
#else
        free(d_buf);
        free(l_buf);
#endif
	d_buf = NULL;
	l_buf = NULL;
#endif
}

/* ===========================================================================
 * Initialize a new block.
 */
local void init_block()
{
    int n; /* iterates over tree elements */

    /* Initialize the trees. */
    for (n = 0; n < L_CODES;  n++) dyn_ltree[n].Freq = 0;
    for (n = 0; n < D_CODES;  n++) dyn_dtree[n].Freq = 0;
    for (n = 0; n < BL_CODES; n++) bl_tree[n].Freq = 0;

    dyn_ltree[END_BLOCK].Freq = 1;
    opt_len = static_len = 0L;
    last_lit = last_dist = last_flags = 0;
    flags = 0; flag_bit = 1;
}

#define SMALLEST 1
/* Index within the heap array of least frequent node in the Huffman tree */


/* ===========================================================================
 * Remove the smallest element from the heap and recreate the heap with
 * one less element. Updates heap and heap_len.
 */
#define pqremove(tree, top) \
{\
    top = heap[SMALLEST]; \
    heap[SMALLEST] = heap[heap_len--]; \
    pqdownheap(tree, SMALLEST); \
}

/* ===========================================================================
 * Compares to subtrees, using the tree depth as tie breaker when
 * the subtrees have equal frequency. This minimizes the worst case length.
 */
#define smaller(tree, n, m) \
   (tree[n].Freq < tree[m].Freq || \
   (tree[n].Freq == tree[m].Freq && depth[n] <= depth[m]))

/* ===========================================================================
 * Restore the heap property by moving down the tree starting at node k,
 * exchanging a node with the smallest of its two sons if necessary, stopping
 * when the heap property is re-established (each father smaller than its
 * two sons).
 */
local void pqdownheap(tree, k)
    ct_data near *tree;  /* the tree to restore */
    int k;               /* node to move down */
{
    int v = heap[k];
    int j = k << 1;  /* left son of k */
    while (j <= heap_len) {
        /* Set j to the smallest of the two sons: */
        if (j < heap_len && smaller(tree, heap[j+1], heap[j])) j++;

        /* Exit if v is smaller than both sons */
        if (smaller(tree, v, heap[j])) break;

        /* Exchange v with the smallest son */
        heap[k] = heap[j],  k = j;

        /* And continue down the tree, setting j to the left son of k */
        j <<= 1;
    }
    heap[k] = v;
}

/* ===========================================================================
 * Compute the optimal bit lengths for a tree and update the total bit length
 * for the current block.
 * IN assertion: the fields freq and dad are set, heap[heap_max] and
 *    above are the tree nodes sorted by increasing frequency.
 * OUT assertions: the field len is set to the optimal bit length, the
 *     array bl_count contains the frequencies for each bit length.
 *     The length opt_len is updated; static_len is also updated if stree is
 *     not null.
 */
local void gen_bitlen(desc)
    tree_desc near *desc; /* the tree descriptor */
{
    ct_data near *tree  = desc->dyn_tree;
    int near *extra     = desc->extra_bits;
    int base            = desc->extra_base;
    int max_code        = desc->max_code;
    int max_length      = desc->max_length;
    ct_data near *stree = desc->static_tree;
    int h;              /* heap index */
    int n, m;           /* iterate over the tree elements */
    int bits;           /* bit length */
    int xbits;          /* extra bits */
    ush f;              /* frequency */
    int overflow = 0;   /* number of elements with bit length too large */

    for (bits = 0; bits <= MAX_BITS; bits++) bl_count[bits] = 0;

    /* In a first pass, compute the optimal bit lengths (which may
     * overflow in the case of the bit length tree).
     */
    tree[heap[heap_max]].Len = 0; /* root of the heap */

    for (h = heap_max+1; h < HEAP_SIZE; h++) {
        n = heap[h];
        bits = tree[tree[n].Dad].Len + 1;
        if (bits > max_length) bits = max_length, overflow++;
        tree[n].Len = bits;
        /* We overwrite tree[n].Dad which is no longer needed */

        if (n > max_code) continue; /* not a leaf node */

        bl_count[bits]++;
        xbits = 0;
        if (n >= base) xbits = extra[n-base];
        f = tree[n].Freq;
        opt_len += (ulg)f * (bits + xbits);
        if (stree) static_len += (ulg)f * (stree[n].Len + xbits);
    }
    if (overflow == 0) return;

    Trace((stderr,"\nbit length overflow\n"));
    /* This happens for example on obj2 and pic of the Calgary corpus */

    /* Find the first bit length which could increase: */
    do {
        bits = max_length-1;
        while (bl_count[bits] == 0) bits--;
        bl_count[bits]--;      /* move one leaf down the tree */
        bl_count[bits+1] += 2; /* move one overflow item as its brother */
        bl_count[max_length]--;
        /* The brother of the overflow item also moves one step up,
         * but this does not affect bl_count[max_length]
         */
        overflow -= 2;
    } while (overflow > 0);

    /* Now recompute all bit lengths, scanning in increasing frequency.
     * h is still equal to HEAP_SIZE. (It is simpler to reconstruct all
     * lengths instead of fixing only the wrong ones. This idea is taken
     * from 'ar' written by Haruhiko Okumura.)
     */
    for (bits = max_length; bits != 0; bits--) {
        n = bl_count[bits];
        while (n != 0) {
            m = heap[--h];
            if (m > max_code) continue;
            if (tree[m].Len != (unsigned) bits) {
                Trace((stderr,"code %d bits %d->%d\n", m, tree[m].Len, bits));
                opt_len += ((long)bits-(long)tree[m].Len)*(long)tree[m].Freq;
                tree[m].Len = bits;
            }
            n--;
        }
    }
}

/* ===========================================================================
 * Generate the codes for a given tree and bit counts (which need not be
 * optimal).
 * IN assertion: the array bl_count contains the bit length statistics for
 * the given tree and the field len is set for all tree elements.
 * OUT assertion: the field code is set for all tree elements of non
 *     zero code length.
 */
local void gen_codes (tree, max_code)
    ct_data near *tree;        /* the tree to decorate */
    int max_code;              /* largest code with non zero frequency */
{
    ush next_code[MAX_BITS+1]; /* next code value for each bit length */
    ush code = 0;              /* running code value */
    int bits;                  /* bit index */
    int n;                     /* code index */

    /* The distribution counts are first used to generate the code values
     * without bit reversal.
     */
    for (bits = 1; bits <= MAX_BITS; bits++) {
        next_code[bits] = code = (code + bl_count[bits-1]) << 1;
    }
    /* Check that the bit counts in bl_count are consistent. The last code
     * must be all ones.
     */
    Assert (code + bl_count[MAX_BITS]-1 == (1<<MAX_BITS)-1,
            "inconsistent bit counts");
    Tracev((stderr,"\ngen_codes: max_code %d ", max_code));

    for (n = 0;  n <= max_code; n++) {
        int len = tree[n].Len;
        if (len == 0) continue;
        /* Now reverse the bits */
        tree[n].Code = bi_reverse(next_code[len]++, len);

        Tracec(tree != static_ltree, (stderr,"\nn %3d %c l %2d c %4x (%x) ",
             n, (isgraph(n) ? n : ' '), len, tree[n].Code, next_code[len]-1));
    }
}

/* ===========================================================================
 * Construct one Huffman tree and assigns the code bit strings and lengths.
 * Update the total bit length for the current block.
 * IN assertion: the field freq is set for all tree elements.
 * OUT assertions: the fields len and code are set to the optimal bit length
 *     and corresponding code. The length opt_len is updated; static_len is
 *     also updated if stree is not null. The field max_code is set.
 */
local void build_tree(desc)
    tree_desc near *desc; /* the tree descriptor */
{
    ct_data near *tree   = desc->dyn_tree;
    ct_data near *stree  = desc->static_tree;
    int elems            = desc->elems;
    int n, m;          /* iterate over heap elements */
    int max_code = -1; /* largest code with non zero frequency */
    int node = elems;  /* next internal node of the tree */

    /* Construct the initial heap, with least frequent element in
     * heap[SMALLEST]. The sons of heap[n] are heap[2*n] and heap[2*n+1].
     * heap[0] is not used.
     */
    heap_len = 0, heap_max = HEAP_SIZE;

    for (n = 0; n < elems; n++) {
        if (tree[n].Freq != 0) {
            heap[++heap_len] = max_code = n;
            depth[n] = 0;
        } else {
            tree[n].Len = 0;
        }
    }

    /* The pkzip format requires that at least one distance code exists,
     * and that at least one bit should be sent even if there is only one
     * possible code. So to avoid special checks later on we force at least
     * two codes of non zero frequency.
     */
    while (heap_len < 2) {
        int new = heap[++heap_len] = (max_code < 2 ? ++max_code : 0);
        tree[new].Freq = 1;
        depth[new] = 0;
        opt_len--; if (stree) static_len -= stree[new].Len;
        /* new is 0 or 1 so it does not have extra bits */
    }
    desc->max_code = max_code;

    /* The elements heap[heap_len/2+1 .. heap_len] are leaves of the tree,
     * establish sub-heaps of increasing lengths:
     */
    for (n = heap_len/2; n >= 1; n--) pqdownheap(tree, n);

    /* Construct the Huffman tree by repeatedly combining the least two
     * frequent nodes.
     */
    do {
        pqremove(tree, n);   /* n = node of least frequency */
        m = heap[SMALLEST];  /* m = node of next least frequency */

        heap[--heap_max] = n; /* keep the nodes sorted by frequency */
        heap[--heap_max] = m;

        /* Create a new node father of n and m */
        tree[node].Freq = tree[n].Freq + tree[m].Freq;
        depth[node] = (uch) (MAX(depth[n], depth[m]) + 1);
        tree[n].Dad = tree[m].Dad = node;
#ifdef DUMP_BL_TREE
        if (tree == bl_tree) {
            fprintf(stderr,"\nnode %d(%d), sons %d(%d) %d(%d)",
                    node, tree[node].Freq, n, tree[n].Freq, m, tree[m].Freq);
        }
#endif
        /* and insert the new node in the heap */
        heap[SMALLEST] = node++;
        pqdownheap(tree, SMALLEST);

    } while (heap_len >= 2);

    heap[--heap_max] = heap[SMALLEST];

    /* At this point, the fields freq and dad are set. We can now
     * generate the bit lengths.
     */
    gen_bitlen(desc);

    /* The field len is now set, we can generate the bit codes */
    gen_codes (tree, max_code);
}

/* ===========================================================================
 * Scan a literal or distance tree to determine the frequencies of the codes
 * in the bit length tree. Updates opt_len to take into account the repeat
 * counts. (The contribution of the bit length codes will be added later
 * during the construction of bl_tree.)
 */
local void scan_tree (tree, max_code)
    ct_data near *tree; /* the tree to be scanned */
    int max_code;       /* and its largest code of non zero frequency */
{
    int n;                     /* iterates over all tree elements */
    int prevlen = -1;          /* last emitted length */
    int curlen;                /* length of current code */
    int nextlen = tree[0].Len; /* length of next code */
    int count = 0;             /* repeat count of the current code */
    int max_count = 7;         /* max repeat count */
    int min_count = 4;         /* min repeat count */

    if (nextlen == 0) max_count = 138, min_count = 3;
    tree[max_code+1].Len = (ush)-1; /* guard */

    for (n = 0; n <= max_code; n++) {
        curlen = nextlen; nextlen = tree[n+1].Len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            bl_tree[curlen].Freq += count;
        } else if (curlen != 0) {
            if (curlen != prevlen) bl_tree[curlen].Freq++;
            bl_tree[REP_3_6].Freq++;
        } else if (count <= 10) {
            bl_tree[REPZ_3_10].Freq++;
        } else {
            bl_tree[REPZ_11_138].Freq++;
        }
        count = 0; prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138, min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6, min_count = 3;
        } else {
            max_count = 7, min_count = 4;
        }
    }
}

/* ===========================================================================
 * Send a literal or distance tree in compressed form, using the codes in
 * bl_tree.
 */
local void send_tree (tree, max_code)
    ct_data near *tree; /* the tree to be scanned */
    int max_code;       /* and its largest code of non zero frequency */
{
    int n;                     /* iterates over all tree elements */
    int prevlen = -1;          /* last emitted length */
    int curlen;                /* length of current code */
    int nextlen = tree[0].Len; /* length of next code */
    int count = 0;             /* repeat count of the current code */
    int max_count = 7;         /* max repeat count */
    int min_count = 4;         /* min repeat count */

    /* tree[max_code+1].Len = -1; */  /* guard already set */
    if (nextlen == 0) max_count = 138, min_count = 3;

    for (n = 0; n <= max_code; n++) {
        curlen = nextlen; nextlen = tree[n+1].Len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            do { send_code(curlen, bl_tree); } while (--count != 0);

        } else if (curlen != 0) {
            if (curlen != prevlen) {
                send_code(curlen, bl_tree); count--;
            }
            Assert(count >= 3 && count <= 6, " 3_6?");
            send_code(REP_3_6, bl_tree); send_bits(count-3, 2);

        } else if (count <= 10) {
            send_code(REPZ_3_10, bl_tree); send_bits(count-3, 3);

        } else {
            send_code(REPZ_11_138, bl_tree); send_bits(count-11, 7);
        }
        count = 0; prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138, min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6, min_count = 3;
        } else {
            max_count = 7, min_count = 4;
        }
    }
}

/* ===========================================================================
 * Construct the Huffman tree for the bit lengths and return the index in
 * bl_order of the last bit length code to send.
 */
local int build_bl_tree()
{
    int max_blindex;  /* index of last bit length code of non zero freq */

    /* Determine the bit length frequencies for literal and distance trees */
    scan_tree(dyn_ltree, l_desc.max_code);
    scan_tree(dyn_dtree, d_desc.max_code);

    /* Build the bit length tree: */
    build_tree(&bl_desc);
    /* opt_len now includes the length of the tree representations, except
     * the lengths of the bit lengths codes and the 5+5+4 bits for the counts.
     */

    /* Determine the number of bit length codes to send. The pkzip format
     * requires that at least 4 bit length codes be sent. (appnote.txt says
     * 3 but the actual value used is 4.)
     */
    for (max_blindex = BL_CODES-1; max_blindex >= 3; max_blindex--) {
        if (bl_tree[bl_order[max_blindex]].Len != 0) break;
    }
    /* Update opt_len to include the bit length tree and counts */
    opt_len += 3*(max_blindex+1) + 5+5+4;
    Tracev((stderr, "\ndyn trees: dyn %ld, stat %ld", opt_len, static_len));

    return max_blindex;
}

/* ===========================================================================
 * Send the header for a block using dynamic Huffman trees: the counts, the
 * lengths of the bit length codes, the literal tree and the distance tree.
 * IN assertion: lcodes >= 257, dcodes >= 1, blcodes >= 4.
 */
local void send_all_trees(lcodes, dcodes, blcodes)
    int lcodes, dcodes, blcodes; /* number of codes for each tree */
{
    int rank;                    /* index in bl_order */

    Assert (lcodes >= 257 && dcodes >= 1 && blcodes >= 4, "not enough codes");
    Assert (lcodes <= L_CODES && dcodes <= D_CODES && blcodes <= BL_CODES,
            "too many codes");
    Tracev((stderr, "\nbl counts: "));
    send_bits(lcodes-257, 5); /* not -255 as stated in appnote.txt */
    send_bits(dcodes-1,   5);
    send_bits(blcodes-4,  4); /* not -3 as stated in appnote.txt */
    for (rank = 0; rank < blcodes; rank++) {
        Tracev((stderr, "\nbl code %2d ", bl_order[rank]));
        send_bits(bl_tree[bl_order[rank]].Len, 3);
    }
    Tracev((stderr, "\nbl tree: sent %ld", bits_sent));

    send_tree(dyn_ltree, lcodes-1); /* send the literal tree */
    Tracev((stderr, "\nlit tree: sent %ld", bits_sent));

    send_tree(dyn_dtree, dcodes-1); /* send the distance tree */
    Tracev((stderr, "\ndist tree: sent %ld", bits_sent));
}

/* ===========================================================================
 * Determine the best encoding for the current block: dynamic trees, static
 * trees or store, and output the encoded block to the zip file. This function
 * returns the total compressed length for the file so far.
 */
ulg flush_block(buf, stored_len, eof)
    char *buf;        /* input block, or NULL if too old */
    ulg stored_len;   /* length of input block */
    int eof;          /* true if this is the last block for a file */
{
    ulg opt_lenb, static_lenb; /* opt_len and static_len in bytes */
    int max_blindex;  /* index of last bit length code of non zero freq */

    flag_buf[last_flags] = flags; /* Save the flags for the last 8 items */

     /* Check if the file is ascii or binary */
    if (*file_type == (ush)UNKNOWN) set_file_type();

    /* Construct the literal and distance trees */
    build_tree(&l_desc);
    Tracev((stderr, "\nlit data: dyn %ld, stat %ld", opt_len, static_len));

    build_tree(&d_desc);
    Tracev((stderr, "\ndist data: dyn %ld, stat %ld", opt_len, static_len));
    /* At this point, opt_len and static_len are the total bit lengths of
     * the compressed block data, excluding the tree representations.
     */

    /* Build the bit length tree for the above two trees, and get the index
     * in bl_order of the last bit length code to send.
     */
    max_blindex = build_bl_tree();

    /* Determine the best encoding. Compute first the block length in bytes */
    opt_lenb = (opt_len+3+7)>>3;
    static_lenb = (static_len+3+7)>>3;
    input_len += stored_len; /* for debugging only */

    Trace((stderr, "\nopt %lu(%lu) stat %lu(%lu) stored %lu lit %u dist %u ",
            opt_lenb, opt_len, static_lenb, static_len, stored_len,
            last_lit, last_dist));

    if (static_lenb <= opt_lenb) opt_lenb = static_lenb;

#ifdef ZIP /* not ok for PGP */
    /* If compression failed and this is the first and last block,
     * and if the zip file can be seeked (to rewrite the local header),
     * the whole file is transformed into a stored file:
     */
#ifdef FORCE_METHOD
    if (level == 1 && eof && compressed_len == 0L) { /* force stored file */
#else
    if (stored_len <= opt_lenb && eof && compressed_len == 0L && seekable()) {
#endif
        /* Since LIT_BUFSIZE <= 2*WSIZE, the input data must be there: */
        if (buf == NULL) error ("block vanished");

        copy_block(buf, (unsigned)stored_len, 0); /* without header */
        compressed_len = stored_len << 3;
        *file_method = STORE;
    } else
#endif /* ZIP */

#ifdef FORCE_METHOD
    if (level == 2 && buf != NULL) { /* force stored block */
#else
    if ((stored_len+4 <= opt_lenb) && (buf != (char *)NULL)) {
                       /* 4: two words for the lengths */
#endif
        /* The test buf != NULL is only necessary if LIT_BUFSIZE > WSIZE.
         * Otherwise we can't have processed more than WSIZE input bytes since
         * the last block flush, because compression would have been
         * successful. If LIT_BUFSIZE <= WSIZE, it is never too late to
         * transform a block into a stored block.
         */
        send_bits((STORED_BLOCK<<1)+eof, 3);  /* send block type */
        compressed_len = (compressed_len + 3 + 7) & ~7L;
        compressed_len += (stored_len + 4) << 3;

        copy_block(buf, (unsigned)stored_len, 1); /* with header */

#ifdef FORCE_METHOD
    } else if (level == 3) { /* force static trees */
#else
    } else if (static_lenb == opt_lenb) {
#endif
        send_bits((STATIC_TREES<<1)+eof, 3);
        compress_block(static_ltree, static_dtree);
        compressed_len += 3 + static_len;
    } else {
        send_bits((DYN_TREES<<1)+eof, 3);
        send_all_trees(l_desc.max_code+1, d_desc.max_code+1, max_blindex+1);
        compress_block(dyn_ltree, dyn_dtree);
        compressed_len += 3 + opt_len;
    }
    Assert (compressed_len == bits_sent, "bad compressed size");
    init_block();

    if (eof) {
#ifndef ZIP
        /* Wipe out sensitive data for pgp */
# ifdef DYN_ALLOC
        extern uch *slide;
# else
        extern uch slide[];
# endif
        memset(slide, 0, (unsigned)(2*WSIZE-1)); /* -1 needed if WSIZE=32K */
#endif /* ZIP */

#if 0
        Assert (input_len == isize, "bad input size");
#endif
        bi_windup();
        compressed_len += 7;  /* align on byte boundary */
    }
    Tracev((stderr,"\ncomprlen %lu(%lu) ", compressed_len>>3,
           compressed_len-7*eof));

    return compressed_len >> 3;
}

/* ===========================================================================
 * Save the match info and tally the frequency counts. Return true if
 * the current block must be flushed.
 */
int ct_tally (dist, lc)
    int dist;  /* distance of matched string */
    int lc;    /* match length-MIN_MATCH or unmatched char (if dist==0) */
{
    l_buf[last_lit++] = (uch)lc;
    if (dist == 0) {
        /* lc is the unmatched char */
        dyn_ltree[lc].Freq++;
    } else {
        /* Here, lc is the match length - MIN_MATCH */
        dist--;             /* dist = match distance - 1 */
        Assert((ush)dist < (ush)MAX_DIST &&
               (ush)lc <= (ush)(MAX_MATCH-MIN_MATCH) &&
               (ush)d_code(dist) < (ush)D_CODES,  "ct_tally: bad match");

        dyn_ltree[length_code[lc]+LITERALS+1].Freq++;
        dyn_dtree[d_code(dist)].Freq++;

        d_buf[last_dist++] = dist;
        flags |= flag_bit;
    }
    flag_bit <<= 1;

    /* Output the flags if they fill a byte: */
    if ((last_lit & 7) == 0) {
        flag_buf[last_flags++] = flags;
        flags = 0, flag_bit = 1;
    }
    /* Try to guess if it is profitable to stop the current block here */
    if (level > 2 && (last_lit & 0xfff) == 0) {
        /* Compute an upper bound for the compressed length */
        ulg out_length = (ulg)last_lit*8L;
        ulg in_length = (ulg)strstart-block_start;
        int dcode;
        for (dcode = 0; dcode < D_CODES; dcode++) {
            out_length += (ulg)dyn_dtree[dcode].Freq*(5L+extra_dbits[dcode]);
        }
        out_length >>= 3;
        Trace((stderr,"\nlast_lit %u, last_dist %u, in %ld, out ~%ld(%ld%%) ",
               last_lit, last_dist, in_length, out_length,
               100L - out_length*100L/in_length));
        if (last_dist < last_lit/2 && out_length < in_length/2) return 1;
    }
    return (last_lit == LIT_BUFSIZE-1 || last_dist == DIST_BUFSIZE);
    /* We avoid equality with LIT_BUFSIZE because of wraparound at 64K
     * on 16 bit machines and because stored blocks are restricted to
     * 64K-1 bytes.
     */
}

/* ===========================================================================
 * Send the block data compressed using the given Huffman trees
 */
local void compress_block(ltree, dtree)
    ct_data near *ltree; /* literal tree */
    ct_data near *dtree; /* distance tree */
{
    unsigned dist;      /* distance of matched string */
    int lc;             /* match length or unmatched char (if dist == 0) */
    unsigned lx = 0;    /* running index in l_buf */
    unsigned dx = 0;    /* running index in d_buf */
    unsigned fx = 0;    /* running index in flag_buf */
    uch flag = 0;       /* current flags */
    unsigned code;      /* the code to send */
    int extra;          /* number of extra bits to send */

    if (last_lit != 0) do {
        if ((lx & 7) == 0) flag = flag_buf[fx++];
        lc = l_buf[lx++];
        if ((flag & 1) == 0) {
            send_code(lc, ltree); /* send a literal byte */
            Tracecv(isgraph(lc), (stderr," '%c' ", lc));
        } else {
            /* Here, lc is the match length - MIN_MATCH */
            code = length_code[lc];
            send_code(code+LITERALS+1, ltree); /* send the length code */
            extra = extra_lbits[code];
            if (extra != 0) {
                lc -= base_length[code];
                send_bits(lc, extra);        /* send the extra length bits */
            }
            dist = d_buf[dx++];
            /* Here, dist is the match distance - 1 */
            code = d_code(dist);
            Assert (code < D_CODES, "bad d_code");

            send_code(code, dtree);       /* send the distance code */
            extra = extra_dbits[code];
            if (extra != 0) {
                dist -= base_dist[code];
                send_bits(dist, extra);   /* send the extra distance bits */
            }
        } /* literal or match pair ? */
        flag >>= 1;
    } while (lx < last_lit);

    send_code(END_BLOCK, ltree);
}

/* ===========================================================================
 * Set the file type to ASCII or BINARY, using a crude approximation:
 * binary if more than 20% of the bytes are <= 6 or >= 128, ascii otherwise.
 * IN assertion: the fields freq of dyn_ltree are set and the total of all
 * frequencies does not exceed 64K (to fit in an int on 16 bit machines).
 */
local void set_file_type()
{
    int n = 0;
    unsigned ascii_freq = 0;
    unsigned bin_freq = 0;
    while (n < 7)        bin_freq += dyn_ltree[n++].Freq;
    while (n < 128)    ascii_freq += dyn_ltree[n++].Freq;
    while (n < LITERALS) bin_freq += dyn_ltree[n++].Freq;
    *file_type = bin_freq > (ascii_freq >> 2) ? BINARY : ASCII;
#ifdef ZIP
    if (*file_type == BINARY && translate_eol) {
        warn("-l used on binary file", "");
    }
#endif
}


/* ==== zunzip.c ==== */
/*---------------------------------------------------------------------------

  unzip.c

  Highly butchered minimum unzip code for inflate.c

  ---------------------------------------------------------------------------*/

#include "zunzip.h"              /* includes, defines, and macros */
#include "language.h"            /* for LANG() */

#define VERSION  "v4.20p BETA of 2-18-92"

/**********************/
/*  Global Variables  */
/**********************/

#if 0
longint csize;        /* used by list_files(), ReadByte(): must be signed */
static longint ucsize;       /* used by list_files(), unReduce(),
				unImplode() */
#endif

ULONG mask_bits[] =
{0x00000000L,
 0x00000001L, 0x00000003L, 0x00000007L, 0x0000000fL,
 0x0000001fL, 0x0000003fL, 0x0000007fL, 0x000000ffL,
 0x000001ffL, 0x000003ffL, 0x000007ffL, 0x00000fffL,
 0x00001fffL, 0x00003fffL, 0x00007fffL, 0x0000ffffL,
 0x0001ffffL, 0x0003ffffL, 0x0007ffffL, 0x000fffffL,
 0x001fffffL, 0x003fffffL, 0x007fffffL, 0x00ffffffL,
 0x01ffffffL, 0x03ffffffL, 0x07ffffffL, 0x0fffffffL,
 0x1fffffffL, 0x3fffffffL, 0x7fffffffL, 0xffffffffL};

/*---------------------------------------------------------------------------
    Input file variables:
  ---------------------------------------------------------------------------*/

byte *inbuf = NULL, *inptr;     /* input buffer (any size is legal)
				   and pointer */
int incnt;

ULONG bitbuf;
int bits_left;
boolean zipeof;

int zipfd;               /* zipfile file handle */

/*---------------------------------------------------------------------------
    Output stream variables:
  ---------------------------------------------------------------------------*/

byte *outbuf;                   /* buffer for rle look-back */
byte *outptr;
byte *outout;                   /* scratch pad for ASCII-native trans */
longint outpos;                 /* absolute position in outfile */
int outcnt;                     /* current position in outbuf */
int outfd;

/*---------------------------------------------------------------------------
    unzip.c static global variables (visible only within this file):
  ---------------------------------------------------------------------------*/

static byte *hold;

/*******************/
/* Main unzip code */
/*******************/

int unzip( FILE *inFile, FILE *outFile )        /* return PK-type error code
						   (except under VMS) */
{
	int status = 0;
	outfd = fileno( outFile );
	zipfd = fileno( inFile );

	inbuf = (byte *) (malloc(INBUFSIZ + 4));    /* 4 extra for hold[]
						       (below) */
	outbuf = (byte *) (malloc(OUTBUFSIZ + 1));  /* 1 extra for string
						       termination */
	outout = outbuf;        /*  else just point to outbuf */

	if ((inbuf == NULL) || (outbuf == NULL) || (outout == NULL)) {
		fprintf(stderr, "error:  can't allocate unzip buffers\n");
		RETURN(4);              /* 4-8:  insufficient memory */
	}
	hold = &inbuf[INBUFSIZ];    /* to check for boundary-spanning
				       signatures */

	bits_left = 0;
	bitbuf = 0;
	outpos = 0L;
	outcnt = 0;
	outptr = outbuf;
	zipeof = 0;

	/* Set output buffer to initial value */
	memset(outbuf, 0, OUTBUFSIZ);

	/* Go from high- to low-level I/O */
	lseek( zipfd, ftell(inFile), SEEK_SET );

	if ((incnt = read(zipfd, (char *) inbuf,INBUFSIZ)) <= 0) {
		fprintf(stderr, LANG("\nERROR: unexpected end of compressed data input.\n"));
		status = -1;               /*  can still do next file   */
	}
	inptr = inbuf;

#if 0
	/* Read in implode information */
	csize = 1000L;			/* Dummy size just to get input bits */

	/* Get compressed, uncompressed file sizes */
	csize = ucsize = 1000000000L;	/* Make sure we can read in anything */
#endif
	if (status == 0)
		status = inflate();	/* Ftoomschk! */

	/* Flush output buffer before returning */
	if (status == 0 && FlushOutput())
		status = -1;
	free(inbuf);
	free(outbuf);
	inbuf = outbuf = outout = NULL;
	return(status);
}
