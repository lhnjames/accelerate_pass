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


/* ==== tif_aux.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_aux.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1991-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Auxiliary Support Routines.
 */
#include "tiffiop.h"

#ifdef COLORIMETRY_SUPPORT
#include <math.h>

static void
TIFFDefaultTransferFunction(TIFFDirectory* td)
{
	uint16 **tf = td->td_transferfunction;
	long i, n = 1<<td->td_bitspersample;

	tf[0] = (uint16 *)_TIFFmalloc(n * sizeof (uint16));
	tf[0][0] = 0;
	for (i = 1; i < n; i++) {
		double t = (double)i/((double) n-1.);
		tf[0][i] = (uint16)floor(65535.*pow(t, 2.2) + .5);
	}
	if (td->td_samplesperpixel - td->td_extrasamples > 1) {
		tf[1] = (uint16 *)_TIFFmalloc(n * sizeof (uint16));
		_TIFFmemcpy(tf[1], tf[0], n * sizeof (uint16));
		tf[2] = (uint16 *)_TIFFmalloc(n * sizeof (uint16));
		_TIFFmemcpy(tf[2], tf[0], n * sizeof (uint16));
	}
}

static void
TIFFDefaultRefBlackWhite(TIFFDirectory* td)
{
	int i;

	td->td_refblackwhite = (float *)_TIFFmalloc(6*sizeof (float));
	for (i = 0; i < 3; i++) {
	    td->td_refblackwhite[2*i+0] = 0;
	    td->td_refblackwhite[2*i+1] = (float)((1L<<td->td_bitspersample)-1L);
	}
}
#endif

/*
 * Like TIFFGetField, but return any default
 * value if the tag is not present in the directory.
 *
 * NB:	We use the value in the directory, rather than
 *	explcit values so that defaults exist only one
 *	place in the library -- in TIFFDefaultDirectory.
 */
int
TIFFVGetFieldDefaulted(TIFF* tif, ttag_t tag, va_list ap)
{
	TIFFDirectory *td = &tif->tif_dir;

	if (TIFFVGetField(tif, tag, ap))
		return (1);
	switch (tag) {
	case TIFFTAG_SUBFILETYPE:
		*va_arg(ap, uint32 *) = td->td_subfiletype;
		return (1);
	case TIFFTAG_BITSPERSAMPLE:
		*va_arg(ap, uint16 *) = td->td_bitspersample;
		return (1);
	case TIFFTAG_THRESHHOLDING:
		*va_arg(ap, uint16 *) = td->td_threshholding;
		return (1);
	case TIFFTAG_FILLORDER:
		*va_arg(ap, uint16 *) = td->td_fillorder;
		return (1);
	case TIFFTAG_ORIENTATION:
		*va_arg(ap, uint16 *) = td->td_orientation;
		return (1);
	case TIFFTAG_SAMPLESPERPIXEL:
		*va_arg(ap, uint16 *) = td->td_samplesperpixel;
		return (1);
	case TIFFTAG_ROWSPERSTRIP:
		*va_arg(ap, uint32 *) = td->td_rowsperstrip;
		return (1);
	case TIFFTAG_MINSAMPLEVALUE:
		*va_arg(ap, uint16 *) = td->td_minsamplevalue;
		return (1);
	case TIFFTAG_MAXSAMPLEVALUE:
		*va_arg(ap, uint16 *) = td->td_maxsamplevalue;
		return (1);
	case TIFFTAG_PLANARCONFIG:
		*va_arg(ap, uint16 *) = td->td_planarconfig;
		return (1);
	case TIFFTAG_RESOLUTIONUNIT:
		*va_arg(ap, uint16 *) = td->td_resolutionunit;
		return (1);
#ifdef CMYK_SUPPORT
	case TIFFTAG_DOTRANGE:
		*va_arg(ap, uint16 *) = 0;
		*va_arg(ap, uint16 *) = (1<<td->td_bitspersample)-1;
		return (1);
	case TIFFTAG_INKSET:
		*va_arg(ap, uint16 *) = td->td_inkset;
		return (1);
	case TIFFTAG_NUMBEROFINKS:
		*va_arg(ap, uint16 *) = td->td_ninks;
		return (1);
#endif
	case TIFFTAG_EXTRASAMPLES:
		*va_arg(ap, uint16 *) = td->td_extrasamples;
		*va_arg(ap, uint16 **) = td->td_sampleinfo;
		return (1);
	case TIFFTAG_MATTEING:
		*va_arg(ap, uint16 *) =
		    (td->td_extrasamples == 1 &&
		     td->td_sampleinfo[0] == EXTRASAMPLE_ASSOCALPHA);
		return (1);
	case TIFFTAG_TILEDEPTH:
		*va_arg(ap, uint32 *) = td->td_tiledepth;
		return (1);
	case TIFFTAG_DATATYPE:
		*va_arg(ap, uint16 *) = td->td_sampleformat-1;
		return (1);
	case TIFFTAG_IMAGEDEPTH:
		*va_arg(ap, uint32 *) = td->td_imagedepth;
		return (1);
#ifdef YCBCR_SUPPORT
	case TIFFTAG_YCBCRCOEFFICIENTS:
		if (!td->td_ycbcrcoeffs) {
			td->td_ycbcrcoeffs = (float *)
			    _TIFFmalloc(3*sizeof (float));
			/* defaults are from CCIR Recommendation 601-1 */
			td->td_ycbcrcoeffs[0] = 0.299f;
			td->td_ycbcrcoeffs[1] = 0.587f;
			td->td_ycbcrcoeffs[2] = 0.114f;
		}
		*va_arg(ap, float **) = td->td_ycbcrcoeffs;
		return (1);
	case TIFFTAG_YCBCRSUBSAMPLING:
		*va_arg(ap, uint16 *) = td->td_ycbcrsubsampling[0];
		*va_arg(ap, uint16 *) = td->td_ycbcrsubsampling[1];
		return (1);
	case TIFFTAG_YCBCRPOSITIONING:
		*va_arg(ap, uint16 *) = td->td_ycbcrpositioning;
		return (1);
#endif
#ifdef COLORIMETRY_SUPPORT
	case TIFFTAG_TRANSFERFUNCTION:
		if (!td->td_transferfunction[0])
			TIFFDefaultTransferFunction(td);
		*va_arg(ap, uint16 **) = td->td_transferfunction[0];
		if (td->td_samplesperpixel - td->td_extrasamples > 1) {
			*va_arg(ap, uint16 **) = td->td_transferfunction[1];
			*va_arg(ap, uint16 **) = td->td_transferfunction[2];
		}
		return (1);
	case TIFFTAG_REFERENCEBLACKWHITE:
		if (!td->td_refblackwhite)
			TIFFDefaultRefBlackWhite(td);
		*va_arg(ap, float **) = td->td_refblackwhite;
		return (1);
#endif
	}
	return (0);
}

/*
 * Like TIFFGetField, but return any default
 * value if the tag is not present in the directory.
 */
int
TIFFGetFieldDefaulted(TIFF* tif, ttag_t tag, ...)
{
	int ok;
	va_list ap;

	va_start(ap, tag);
	ok =  TIFFVGetFieldDefaulted(tif, tag, ap);
	va_end(ap);
	return (ok);
}


/* ==== tif_close.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_close.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 */
#include "tiffiop.h"

void
TIFFClose(TIFF* tif)
{
	if (tif->tif_mode != O_RDONLY)
		/*
		 * Flush buffered data and directory (if dirty).
		 */
		TIFFFlush(tif);
	(*tif->tif_cleanup)(tif);
	TIFFFreeDirectory(tif);
	if (tif->tif_rawdata && (tif->tif_flags&TIFF_MYBUFFER))
		_TIFFfree(tif->tif_rawdata);
	if (isMapped(tif))
		TIFFUnmapFileContents(tif, tif->tif_base, tif->tif_size);
	(void) TIFFCloseFile(tif);
	if (tif->tif_fieldinfo)
		_TIFFfree(tif->tif_fieldinfo);
	_TIFFfree(tif);
}


/* ==== tif_codec.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_codec.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library
 *
 * Builtin Compression Scheme Configuration Support.
 */
#include "tiffiop.h"

static	int NotConfigured(TIFF*, int);

#ifndef	LZW_SUPPORT
#define	TIFFInitLZW		NotConfigured
#endif
#ifndef	PACKBITS_SUPPORT
#define	TIFFInitPackbits	NotConfigured
#endif
#ifndef	THUNDER_SUPPORT
#define	TIFFInitThunderScan	NotConfigured
#endif
#ifndef	NEXT_SUPPORT
#define	TIFFInitNeXT		NotConfigured
#endif
#ifndef	JPEG_SUPPORT
#define	TIFFInitJPEG		NotConfigured
#endif
#ifndef	OJPEG_SUPPORT
#define	TIFFInitOJPEG		NotConfigured
#endif
#ifndef	CCITT_SUPPORT
#define	TIFFInitCCITTRLE	NotConfigured
#define	TIFFInitCCITTRLEW	NotConfigured
#define	TIFFInitCCITTFax3	NotConfigured
#define	TIFFInitCCITTFax4	NotConfigured
#endif
#ifndef JBIG_SUPPORT
#define	TIFFInitJBIG		NotConfigured
#endif
#ifndef	ZIP_SUPPORT
#define	TIFFInitZIP		NotConfigured
#endif
#ifndef	PIXARLOG_SUPPORT
#define	TIFFInitPixarLog	NotConfigured
#endif
#ifndef LOGLUV_SUPPORT
#define TIFFInitSGILog		NotConfigured
#endif

/*
 * Compression schemes statically built into the library.
 */
#ifdef VMS
const TIFFCodec _TIFFBuiltinCODECS[] = {
#else
TIFFCodec _TIFFBuiltinCODECS[] = {
#endif
    { "None",		COMPRESSION_NONE,	TIFFInitDumpMode },
    { "LZW",		COMPRESSION_LZW,	TIFFInitLZW },
    { "PackBits",	COMPRESSION_PACKBITS,	TIFFInitPackBits },
    { "ThunderScan",	COMPRESSION_THUNDERSCAN,TIFFInitThunderScan },
    { "NeXT",		COMPRESSION_NEXT,	TIFFInitNeXT },
    { "JPEG",		COMPRESSION_JPEG,	TIFFInitJPEG },
    { "Old-style JPEG",	COMPRESSION_OJPEG,	TIFFInitOJPEG },
    { "CCITT RLE",	COMPRESSION_CCITTRLE,	TIFFInitCCITTRLE },
    { "CCITT RLE/W",	COMPRESSION_CCITTRLEW,	TIFFInitCCITTRLEW },
    { "CCITT Group 3",	COMPRESSION_CCITTFAX3,	TIFFInitCCITTFax3 },
    { "CCITT Group 4",	COMPRESSION_CCITTFAX4,	TIFFInitCCITTFax4 },
    { "ISO JBIG",	COMPRESSION_JBIG,	TIFFInitJBIG },
    { "Deflate",	COMPRESSION_DEFLATE,	TIFFInitZIP },
    { "AdobeDeflate",   COMPRESSION_ADOBE_DEFLATE , TIFFInitZIP }, 
    { "PixarLog",	COMPRESSION_PIXARLOG,	TIFFInitPixarLog },
    { "SGILog",		COMPRESSION_SGILOG,	TIFFInitSGILog },
    { "SGILog24",	COMPRESSION_SGILOG24,	TIFFInitSGILog },
    { NULL }
};

static int
_notConfigured(TIFF* tif)
{
	const TIFFCodec* c = TIFFFindCODEC(tif->tif_dir.td_compression);

	TIFFError(tif->tif_name,
	    "%s compression support is not configured", c->name);
	return (0);
}

static int
NotConfigured(TIFF* tif, int scheme)
{
	tif->tif_setupdecode = _notConfigured;
	tif->tif_setupencode = _notConfigured;
	return (1);
}


/* ==== tif_compress.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_compress.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library
 *
 * Compression Scheme Configuration Support.
 */
#include "tiffiop.h"

static int
TIFFNoEncode(TIFF* tif, char* method)
{
	const TIFFCodec* c = TIFFFindCODEC(tif->tif_dir.td_compression);

	if (c) { 
	  if (! strncmp(c->name, "LZW", 3) ){ 
	    TIFFError(tif->tif_name, 
		      "%s %s encoding is no longer implemented due to Unisys patent enforcement", 
		      c->name, method); 
	  } else { 
	    TIFFError(tif->tif_name, "%s %s encoding is not implemented",
		      c->name, method);
	  }
	}
	else { 
		TIFFError(tif->tif_name,
			  "Compression scheme %u %s encoding is not implemented",
		    tif->tif_dir.td_compression, method);
	}
	return (-1);
}

int
_TIFFNoRowEncode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoEncode(tif, "scanline"));
}

int
_TIFFNoStripEncode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoEncode(tif, "strip"));
}

int
_TIFFNoTileEncode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoEncode(tif, "tile"));
}

static int
TIFFNoDecode(TIFF* tif, char* method)
{
	const TIFFCodec* c = TIFFFindCODEC(tif->tif_dir.td_compression);

	if (c)
		TIFFError(tif->tif_name, "%s %s decoding is not implemented",
		    c->name, method);
	else
		TIFFError(tif->tif_name,
		    "Compression scheme %u %s decoding is not implemented",
		    tif->tif_dir.td_compression, method);
	return (-1);
}

int
_TIFFNoRowDecode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoDecode(tif, "scanline"));
}

int
_TIFFNoStripDecode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoDecode(tif, "strip"));
}

int
_TIFFNoTileDecode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) pp; (void) cc; (void) s;
	return (TIFFNoDecode(tif, "tile"));
}

int
_TIFFNoSeek(TIFF* tif, uint32 off)
{
	(void) off;
	TIFFError(tif->tif_name,
	    "Compression algorithm does not support random access");
	return (0);
}

int
_TIFFNoPreCode(TIFF* tif, tsample_t s)
{
	(void) tif; (void) s;
	return (1);
}

static int _TIFFtrue(TIFF* tif) { (void) tif; return (1); }
static void _TIFFvoid(TIFF* tif) { (void) tif; }

void
_TIFFSetDefaultCompressionState(TIFF* tif)
{
	tif->tif_setupdecode = _TIFFtrue;
	tif->tif_predecode = _TIFFNoPreCode;
	tif->tif_decoderow = _TIFFNoRowDecode;
	tif->tif_decodestrip = _TIFFNoStripDecode;
	tif->tif_decodetile = _TIFFNoTileDecode;
	tif->tif_setupencode = _TIFFtrue;
	tif->tif_preencode = _TIFFNoPreCode;
	tif->tif_postencode = _TIFFtrue;
	tif->tif_encoderow = _TIFFNoRowEncode;
	tif->tif_encodestrip = _TIFFNoStripEncode;
	tif->tif_encodetile = _TIFFNoTileEncode;
	tif->tif_close = _TIFFvoid;
	tif->tif_seek = _TIFFNoSeek;
	tif->tif_cleanup = _TIFFvoid;
	tif->tif_defstripsize = _TIFFDefaultStripSize;
	tif->tif_deftilesize = _TIFFDefaultTileSize;
	tif->tif_flags &= ~TIFF_NOBITREV;
}

int
TIFFSetCompressionScheme(TIFF* tif, int scheme)
{
	const TIFFCodec *c = TIFFFindCODEC((uint16) scheme);

	_TIFFSetDefaultCompressionState(tif);
	/*
	 * Don't treat an unknown compression scheme as an error.
	 * This permits applications to open files with data that
	 * the library does not have builtin support for, but which
	 * may still be meaningful.
	 */
	return (c ? (*c->init)(tif, scheme) : 1);
}

/*
 * Other compression schemes may be registered.  Registered
 * schemes can also override the builtin versions provided
 * by this library.
 */
typedef struct _codec {
	struct _codec*	next;
	TIFFCodec*	info;
} codec_t;
static	codec_t* registeredCODECS = NULL;

const TIFFCodec*
TIFFFindCODEC(uint16 scheme)
{
	const TIFFCodec* c;
	codec_t* cd;

	for (cd = registeredCODECS; cd; cd = cd->next)
		if (cd->info->scheme == scheme)
			return ((const TIFFCodec*) cd->info);
	for (c = _TIFFBuiltinCODECS; c->name; c++)
		if (c->scheme == scheme)
			return (c);
	return ((const TIFFCodec*) 0);
}

TIFFCodec*
TIFFRegisterCODEC(uint16 scheme, const char* name, TIFFInitMethod init)
{
	codec_t* cd = (codec_t*)
	    _TIFFmalloc(sizeof (codec_t) + sizeof (TIFFCodec) + strlen(name)+1);

	if (cd != NULL) {
		cd->info = (TIFFCodec*) ((tidata_t) cd + sizeof (codec_t));
		cd->info->name = (char*)
		    ((tidata_t) cd->info + sizeof (TIFFCodec));
		strcpy(cd->info->name, name);
		cd->info->scheme = scheme;
		cd->info->init = init;
		cd->next = registeredCODECS;
		registeredCODECS = cd;
	} else
		TIFFError("TIFFRegisterCODEC",
		    "No space to register compression scheme %s", name);
	return (cd->info);
}

void
TIFFUnRegisterCODEC(TIFFCodec* c)
{
	codec_t* cd;
	codec_t** pcd;

	for (pcd = &registeredCODECS; (cd = *pcd); pcd = &cd->next)
		if (cd->info == c) {
			*pcd = cd->next;
			_TIFFfree(cd);
			return;
		}
	TIFFError("TIFFUnRegisterCODEC",
	    "Cannot remove compression scheme %s; not registered", c->name);
}


/* ==== tif_dir.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_dir.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Directory Tag Get & Set Routines.
 * (and also some miscellaneous stuff)
 */
#include "tiffiop.h"

/*
 * These are used in the backwards compatibility code...
 */
#define DATATYPE_VOID		0       /* !untyped data */
#define DATATYPE_INT		1       /* !signed integer data */
#define DATATYPE_UINT		2       /* !unsigned integer data */
#define DATATYPE_IEEEFP		3       /* !IEEE floating point data */

void
_TIFFsetByteArray(void** vpp, void* vp, long n)
{
	if (*vpp)
		_TIFFfree(*vpp), *vpp = 0;
	if (vp && (*vpp = (void*) _TIFFmalloc(n)))
		_TIFFmemcpy(*vpp, vp, n);
}
void _TIFFsetString(char** cpp, char* cp)
    { _TIFFsetByteArray((void**) cpp, (void*) cp, (long) (strlen(cp)+1)); }
void _TIFFsetNString(char** cpp, char* cp, long n)
    { _TIFFsetByteArray((void**) cpp, (void*) cp, n); }
void _TIFFsetShortArray(uint16** wpp, uint16* wp, long n)
    { _TIFFsetByteArray((void**) wpp, (void*) wp, n*sizeof (uint16)); }
void _TIFFsetLongArray(uint32** lpp, uint32* lp, long n)
    { _TIFFsetByteArray((void**) lpp, (void*) lp, n*sizeof (uint32)); }
void _TIFFsetFloatArray(float** fpp, float* fp, long n)
    { _TIFFsetByteArray((void**) fpp, (void*) fp, n*sizeof (float)); }
void _TIFFsetDoubleArray(double** dpp, double* dp, long n)
    { _TIFFsetByteArray((void**) dpp, (void*) dp, n*sizeof (double)); }

/*
 * Install extra samples information.
 */
static int
setExtraSamples(TIFFDirectory* td, va_list ap, int* v)
{
	uint16* va;
	int i;

	*v = va_arg(ap, int);
	if ((uint16) *v > td->td_samplesperpixel)
		return (0);
	va = va_arg(ap, uint16*);
	if (*v > 0 && va == NULL)		/* typically missing param */
		return (0);
	for (i = 0; i < *v; i++)
		if (va[i] > EXTRASAMPLE_UNASSALPHA)
			return (0);
	td->td_extrasamples = (uint16) *v;
	_TIFFsetShortArray(&td->td_sampleinfo, va, td->td_extrasamples);
	return (1);
}

#ifdef CMYK_SUPPORT
static int
checkInkNamesString(TIFF* tif, int slen, const char* s)
{
	TIFFDirectory* td = &tif->tif_dir;
	int i = td->td_samplesperpixel;

	if (slen > 0) {
		const char* ep = s+slen;
		const char* cp = s;
		for (; i > 0; i--) {
			for (; *cp != '\0'; cp++)
				if (cp >= ep)
					goto bad;
			cp++;				/* skip \0 */
		}
		return (cp-s);
	}
bad:
	TIFFError("TIFFSetField",
	    "%s: Invalid InkNames value; expecting %d names, found %d",
	    tif->tif_name,
	    td->td_samplesperpixel,
	    td->td_samplesperpixel-i);
	return (0);
}
#endif

static int
_TIFFVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	TIFFDirectory* td = &tif->tif_dir;
	int status = 1;
	uint32 v32;
	int i, v;
	double d;
	char* s;

	switch (tag) {
	case TIFFTAG_SUBFILETYPE:
		td->td_subfiletype = va_arg(ap, uint32);
		break;
	case TIFFTAG_IMAGEWIDTH:
		td->td_imagewidth = va_arg(ap, uint32);
		break;
	case TIFFTAG_IMAGELENGTH:
		td->td_imagelength = va_arg(ap, uint32);
		break;
	case TIFFTAG_BITSPERSAMPLE:
		td->td_bitspersample = (uint16) va_arg(ap, int);
		/*
		 * If the data require post-decoding processing
		 * to byte-swap samples, set it up here.  Note
		 * that since tags are required to be ordered,
		 * compression code can override this behaviour
		 * in the setup method if it wants to roll the
		 * post decoding work in with its normal work.
		 */
		if (tif->tif_flags & TIFF_SWAB) {
			if (td->td_bitspersample == 16)
				tif->tif_postdecode = _TIFFSwab16BitData;
			else if (td->td_bitspersample == 32)
				tif->tif_postdecode = _TIFFSwab32BitData;
			else if (td->td_bitspersample == 64)
				tif->tif_postdecode = _TIFFSwab64BitData;
		}
		break;
	case TIFFTAG_COMPRESSION:
		v = va_arg(ap, int) & 0xffff;
		/*
		 * If we're changing the compression scheme,
		 * the notify the previous module so that it
		 * can cleanup any state it's setup.
		 */
		if (TIFFFieldSet(tif, FIELD_COMPRESSION)) {
			if (td->td_compression == v)
				break;
			(*tif->tif_cleanup)(tif);
			tif->tif_flags &= ~TIFF_CODERSETUP;
		}
		/*
		 * Setup new compression routine state.
		 */
		if ( ! tif->tif_mode == O_RDONLY ) { 
		  /* Handle removal of LZW compression */ 
		  if ( v == COMPRESSION_LZW ) { 
		    TIFFError(tif->tif_name, 
			      "LZW compression no longer supported due to Unisys patent enforcement"); 
		    v=COMPRESSION_NONE;
		  }
		}
		if( (status = TIFFSetCompressionScheme(tif, v)) != 0 )
		  td->td_compression = v;
		break;
	case TIFFTAG_PHOTOMETRIC:
		td->td_photometric = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_THRESHHOLDING:
		td->td_threshholding = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_FILLORDER:
		v = va_arg(ap, int);
		if (v != FILLORDER_LSB2MSB && v != FILLORDER_MSB2LSB)
			goto badvalue;
		td->td_fillorder = (uint16) v;
		break;
	case TIFFTAG_DOCUMENTNAME:
		_TIFFsetString(&td->td_documentname, va_arg(ap, char*));
		break;
	case TIFFTAG_ARTIST:
		_TIFFsetString(&td->td_artist, va_arg(ap, char*));
		break;
	case TIFFTAG_DATETIME:
		_TIFFsetString(&td->td_datetime, va_arg(ap, char*));
		break;
	case TIFFTAG_HOSTCOMPUTER:
		_TIFFsetString(&td->td_hostcomputer, va_arg(ap, char*));
		break;
	case TIFFTAG_IMAGEDESCRIPTION:
		_TIFFsetString(&td->td_imagedescription, va_arg(ap, char*));
		break;
	case TIFFTAG_MAKE:
		_TIFFsetString(&td->td_make, va_arg(ap, char*));
		break;
	case TIFFTAG_MODEL:
		_TIFFsetString(&td->td_model, va_arg(ap, char*));
		break;
	case TIFFTAG_SOFTWARE:
		_TIFFsetString(&td->td_software, va_arg(ap, char*));
		break;
	case TIFFTAG_ORIENTATION:
		v = va_arg(ap, int);
		if (v < ORIENTATION_TOPLEFT || ORIENTATION_LEFTBOT < v) {
			TIFFWarning(tif->tif_name,
			    "Bad value %ld for \"%s\" tag ignored",
			    v, _TIFFFieldWithTag(tif, tag)->field_name);
		} else
			td->td_orientation = (uint16) v;
		break;
	case TIFFTAG_SAMPLESPERPIXEL:
		/* XXX should cross check -- e.g. if pallette, then 1 */
		v = va_arg(ap, int);
		if (v == 0)
			goto badvalue;
		td->td_samplesperpixel = (uint16) v;
		break;
	case TIFFTAG_ROWSPERSTRIP:
		v32 = va_arg(ap, uint32);
		if (v32 == 0)
			goto badvalue32;
		td->td_rowsperstrip = v32;
		if (!TIFFFieldSet(tif, FIELD_TILEDIMENSIONS)) {
			td->td_tilelength = v32;
			td->td_tilewidth = td->td_imagewidth;
		}
		break;
	case TIFFTAG_MINSAMPLEVALUE:
		td->td_minsamplevalue = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_MAXSAMPLEVALUE:
		td->td_maxsamplevalue = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_SMINSAMPLEVALUE:
		td->td_sminsamplevalue = (double) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_SMAXSAMPLEVALUE:
		td->td_smaxsamplevalue = (double) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_XRESOLUTION:
		td->td_xresolution = (float) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_YRESOLUTION:
		td->td_yresolution = (float) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_PLANARCONFIG:
		v = va_arg(ap, int);
		if (v != PLANARCONFIG_CONTIG && v != PLANARCONFIG_SEPARATE)
			goto badvalue;
		td->td_planarconfig = (uint16) v;
		break;
	case TIFFTAG_PAGENAME:
		_TIFFsetString(&td->td_pagename, va_arg(ap, char*));
		break;
	case TIFFTAG_XPOSITION:
		td->td_xposition = (float) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_YPOSITION:
		td->td_yposition = (float) va_arg(ap, dblparam_t);
		break;
	case TIFFTAG_RESOLUTIONUNIT:
		v = va_arg(ap, int);
		if (v < RESUNIT_NONE || RESUNIT_CENTIMETER < v)
			goto badvalue;
		td->td_resolutionunit = (uint16) v;
		break;
	case TIFFTAG_PAGENUMBER:
		td->td_pagenumber[0] = (uint16) va_arg(ap, int);
		td->td_pagenumber[1] = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_HALFTONEHINTS:
		td->td_halftonehints[0] = (uint16) va_arg(ap, int);
		td->td_halftonehints[1] = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_COLORMAP:
		v32 = (uint32)(1L<<td->td_bitspersample);
		_TIFFsetShortArray(&td->td_colormap[0], va_arg(ap, uint16*), v32);
		_TIFFsetShortArray(&td->td_colormap[1], va_arg(ap, uint16*), v32);
		_TIFFsetShortArray(&td->td_colormap[2], va_arg(ap, uint16*), v32);
		break;
	case TIFFTAG_EXTRASAMPLES:
		if (!setExtraSamples(td, ap, &v))
			goto badvalue;
		break;
	case TIFFTAG_MATTEING:
		td->td_extrasamples = (uint16) (va_arg(ap, int) != 0);
		if (td->td_extrasamples) {
			uint16 sv = EXTRASAMPLE_ASSOCALPHA;
			_TIFFsetShortArray(&td->td_sampleinfo, &sv, 1);
		}
		break;
	case TIFFTAG_TILEWIDTH:
		v32 = va_arg(ap, uint32);
		if (v32 % 16) {
			if (tif->tif_mode != O_RDONLY)
				goto badvalue32;
			TIFFWarning(tif->tif_name,
			    "Nonstandard tile width %d, convert file", v32);
		}
		td->td_tilewidth = v32;
		tif->tif_flags |= TIFF_ISTILED;
		break;
	case TIFFTAG_TILELENGTH:
		v32 = va_arg(ap, uint32);
		if (v32 % 16) {
			if (tif->tif_mode != O_RDONLY)
				goto badvalue32;
			TIFFWarning(tif->tif_name,
			    "Nonstandard tile length %d, convert file", v32);
		}
		td->td_tilelength = v32;
		tif->tif_flags |= TIFF_ISTILED;
		break;
	case TIFFTAG_TILEDEPTH:
		v32 = va_arg(ap, uint32);
		if (v32 == 0)
			goto badvalue32;
		td->td_tiledepth = v32;
		break;
	case TIFFTAG_DATATYPE:
		v = va_arg(ap, int);
		switch (v) {
		case DATATYPE_VOID:	v = SAMPLEFORMAT_VOID;	break;
		case DATATYPE_INT:	v = SAMPLEFORMAT_INT;	break;
		case DATATYPE_UINT:	v = SAMPLEFORMAT_UINT;	break;
		case DATATYPE_IEEEFP:	v = SAMPLEFORMAT_IEEEFP;break;
		default:		goto badvalue;
		}
		td->td_sampleformat = (uint16) v;
		break;
	case TIFFTAG_SAMPLEFORMAT:
		v = va_arg(ap, int);
		if (v < SAMPLEFORMAT_UINT || SAMPLEFORMAT_VOID < v)
			goto badvalue;
		td->td_sampleformat = (uint16) v;
		break;
	case TIFFTAG_IMAGEDEPTH:
		td->td_imagedepth = va_arg(ap, uint32);
		break;
	case TIFFTAG_STONITS:
		d = va_arg(ap, dblparam_t);
		if (d <= 0.)
			goto badvaluedbl;
		td->td_stonits = d;
		break;

	/* Begin Pixar Tags */
 	case TIFFTAG_PIXAR_IMAGEFULLWIDTH:
 		td->td_imagefullwidth = va_arg(ap, uint32);
 		break;
 	case TIFFTAG_PIXAR_IMAGEFULLLENGTH:
 		td->td_imagefulllength = va_arg(ap, uint32);
 		break;
 	case TIFFTAG_PIXAR_TEXTUREFORMAT:
 		_TIFFsetString(&td->td_textureformat, va_arg(ap, char*));
 		break;
 	case TIFFTAG_PIXAR_WRAPMODES:
 		_TIFFsetString(&td->td_wrapmodes, va_arg(ap, char*));
 		break;
 	case TIFFTAG_PIXAR_FOVCOT:
 		td->td_fovcot = (float) va_arg(ap, dblparam_t);
 		break;
 	case TIFFTAG_PIXAR_MATRIX_WORLDTOSCREEN:
 		_TIFFsetFloatArray(&td->td_matrixWorldToScreen,
 			va_arg(ap, float*), 16);
 		break;
 	case TIFFTAG_PIXAR_MATRIX_WORLDTOCAMERA:
 		_TIFFsetFloatArray(&td->td_matrixWorldToCamera,
 			va_arg(ap, float*), 16);
 		break;
 	/* End Pixar Tags */	       

#if SUBIFD_SUPPORT
	case TIFFTAG_SUBIFD:
		if ((tif->tif_flags & TIFF_INSUBIFD) == 0) {
			td->td_nsubifd = (uint16) va_arg(ap, int);
			_TIFFsetLongArray(&td->td_subifd, va_arg(ap, uint32*),
			    (long) td->td_nsubifd);
		} else {
			TIFFError(tif->tif_name, "Sorry, cannot nest SubIFDs");
			status = 0;
		}
		break;
#endif
#ifdef YCBCR_SUPPORT
	case TIFFTAG_YCBCRCOEFFICIENTS:
		_TIFFsetFloatArray(&td->td_ycbcrcoeffs, va_arg(ap, float*), 3);
		break;
	case TIFFTAG_YCBCRPOSITIONING:
		td->td_ycbcrpositioning = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_YCBCRSUBSAMPLING:
		td->td_ycbcrsubsampling[0] = (uint16) va_arg(ap, int);
		td->td_ycbcrsubsampling[1] = (uint16) va_arg(ap, int);
		break;
#endif
#ifdef COLORIMETRY_SUPPORT
	case TIFFTAG_WHITEPOINT:
		_TIFFsetFloatArray(&td->td_whitepoint, va_arg(ap, float*), 2);
		break;
	case TIFFTAG_PRIMARYCHROMATICITIES:
		_TIFFsetFloatArray(&td->td_primarychromas, va_arg(ap, float*), 6);
		break;
	case TIFFTAG_TRANSFERFUNCTION:
		v = (td->td_samplesperpixel - td->td_extrasamples) > 1 ? 3 : 1;
		for (i = 0; i < v; i++)
			_TIFFsetShortArray(&td->td_transferfunction[i],
			    va_arg(ap, uint16*), 1L<<td->td_bitspersample);
		break;
	case TIFFTAG_REFERENCEBLACKWHITE:
		/* XXX should check for null range */
		_TIFFsetFloatArray(&td->td_refblackwhite, va_arg(ap, float*), 6);
		break;
#endif
#ifdef CMYK_SUPPORT
	case TIFFTAG_INKSET:
		td->td_inkset = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_DOTRANGE:
		/* XXX should check for null range */
		td->td_dotrange[0] = (uint16) va_arg(ap, int);
		td->td_dotrange[1] = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_INKNAMES:
		i = va_arg(ap, int);
		s = va_arg(ap, char*);
		i = checkInkNamesString(tif, i, s);
                status = i > 0;
		if( i > 0 ) {
			_TIFFsetNString(&td->td_inknames, s, i);
			td->td_inknameslen = i;
		}
		break;
	case TIFFTAG_NUMBEROFINKS:
		td->td_ninks = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_TARGETPRINTER:
		_TIFFsetString(&td->td_targetprinter, va_arg(ap, char*));
		break;
#endif
#ifdef ICC_SUPPORT
	case TIFFTAG_ICCPROFILE:
		td->td_profileLength = (uint32) va_arg(ap, uint32);
		_TIFFsetByteArray(&td->td_profileData, va_arg(ap, void*),
		    td->td_profileLength);
		break;
#endif
#ifdef PHOTOSHOP_SUPPORT
 	case TIFFTAG_PHOTOSHOP:
  		td->td_photoshopLength = (uint32) va_arg(ap, uint32);
  		_TIFFsetByteArray (&td->td_photoshopData, va_arg(ap, void*),
 			td->td_photoshopLength);
 		break;
#endif
#ifdef IPTC_SUPPORT
    case TIFFTAG_RICHTIFFIPTC: 
  		td->td_richtiffiptcLength = (uint32) va_arg(ap, uint32);
#ifdef PHOTOSHOP_SUPPORT
  		_TIFFsetLongArray ((uint32**)&td->td_richtiffiptcData, va_arg(ap, uint32*),
 			td->td_richtiffiptcLength);
#else
  		_TIFFsetByteArray (&td->td_photoshopData, va_arg(ap, void*),
 			td->td_photoshopLength);
#endif
 		break;
#endif
	default:
		/*
		 * This can happen if multiple images are open with
		 * different codecs which have private tags.  The
		 * global tag information table may then have tags
		 * that are valid for one file but not the other. 
		 * If the client tries to set a tag that is not valid
		 * for the image's codec then we'll arrive here.  This
		 * happens, for example, when tiffcp is used to convert
		 * between compression schemes and codec-specific tags
		 * are blindly copied.
		 */
		TIFFError("TIFFSetField",
		    "%s: Invalid %stag \"%s\" (not supported by codec)",
		    tif->tif_name, isPseudoTag(tag) ? "pseduo-" : "",
		    _TIFFFieldWithTag(tif, tag)->field_name);
		status = 0;
		break;
	}
	if (status) {
		TIFFSetFieldBit(tif, _TIFFFieldWithTag(tif, tag)->field_bit);
		tif->tif_flags |= TIFF_DIRTYDIRECT;
	}
	va_end(ap);
	return (status);
badvalue:
	TIFFError(tif->tif_name, "%d: Bad value for \"%s\"", v,
	    _TIFFFieldWithTag(tif, tag)->field_name);
	va_end(ap);
	return (0);
badvalue32:
	TIFFError(tif->tif_name, "%ld: Bad value for \"%s\"", v32,
	    _TIFFFieldWithTag(tif, tag)->field_name);
	va_end(ap);
	return (0);
badvaluedbl:
	TIFFError(tif->tif_name, "%f: Bad value for \"%s\"", d,
	    _TIFFFieldWithTag(tif, tag)->field_name);
	va_end(ap);
	return (0);
}

/*
 * Return 1/0 according to whether or not
 * it is permissible to set the tag's value.
 * Note that we allow ImageLength to be changed
 * so that we can append and extend to images.
 * Any other tag may not be altered once writing
 * has commenced, unless its value has no effect
 * on the format of the data that is written.
 */
static int
OkToChangeTag(TIFF* tif, ttag_t tag)
{
	const TIFFFieldInfo* fip = _TIFFFindFieldInfo(tif, tag, TIFF_ANY);
	if (!fip) {			/* unknown tag */
		TIFFError("TIFFSetField", "%s: Unknown %stag %u",
		    tif->tif_name, isPseudoTag(tag) ? "pseudo-" : "", tag);
		return (0);
	}
	if (tag != TIFFTAG_IMAGELENGTH && (tif->tif_flags & TIFF_BEENWRITING) &&
	    !fip->field_oktochange) {
		/*
		 * Consult info table to see if tag can be changed
		 * after we've started writing.  We only allow changes
		 * to those tags that don't/shouldn't affect the
		 * compression and/or format of the data.
		 */
		TIFFError("TIFFSetField",
		    "%s: Cannot modify tag \"%s\" while writing",
		    tif->tif_name, fip->field_name);
		return (0);
	}
	return (1);
}

/*
 * Record the value of a field in the
 * internal directory structure.  The
 * field will be written to the file
 * when/if the directory structure is
 * updated.
 */
int
TIFFSetField(TIFF* tif, ttag_t tag, ...)
{
	va_list ap;
	int status;

	va_start(ap, tag);
	status = TIFFVSetField(tif, tag, ap);
	va_end(ap);
	return (status);
}

/*
 * Like TIFFSetField, but taking a varargs
 * parameter list.  This routine is useful
 * for building higher-level interfaces on
 * top of the library.
 */
int
TIFFVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	return OkToChangeTag(tif, tag) ?
	    (*tif->tif_vsetfield)(tif, tag, ap) : 0;
}

static int
_TIFFVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	TIFFDirectory* td = &tif->tif_dir;

	switch (tag) {
	case TIFFTAG_SUBFILETYPE:
		*va_arg(ap, uint32*) = td->td_subfiletype;
		break;
	case TIFFTAG_IMAGEWIDTH:
		*va_arg(ap, uint32*) = td->td_imagewidth;
		break;
	case TIFFTAG_IMAGELENGTH:
		*va_arg(ap, uint32*) = td->td_imagelength;
		break;
	case TIFFTAG_BITSPERSAMPLE:
		*va_arg(ap, uint16*) = td->td_bitspersample;
		break;
	case TIFFTAG_COMPRESSION:
		*va_arg(ap, uint16*) = td->td_compression;
		break;
	case TIFFTAG_PHOTOMETRIC:
		*va_arg(ap, uint16*) = td->td_photometric;
		break;
	case TIFFTAG_THRESHHOLDING:
		*va_arg(ap, uint16*) = td->td_threshholding;
		break;
	case TIFFTAG_FILLORDER:
		*va_arg(ap, uint16*) = td->td_fillorder;
		break;
	case TIFFTAG_DOCUMENTNAME:
		*va_arg(ap, char**) = td->td_documentname;
		break;
	case TIFFTAG_ARTIST:
		*va_arg(ap, char**) = td->td_artist;
		break;
	case TIFFTAG_DATETIME:
		*va_arg(ap, char**) = td->td_datetime;
		break;
	case TIFFTAG_HOSTCOMPUTER:
		*va_arg(ap, char**) = td->td_hostcomputer;
		break;
	case TIFFTAG_IMAGEDESCRIPTION:
		*va_arg(ap, char**) = td->td_imagedescription;
		break;
	case TIFFTAG_MAKE:
		*va_arg(ap, char**) = td->td_make;
		break;
	case TIFFTAG_MODEL:
		*va_arg(ap, char**) = td->td_model;
		break;
	case TIFFTAG_SOFTWARE:
		*va_arg(ap, char**) = td->td_software;
		break;
	case TIFFTAG_ORIENTATION:
		*va_arg(ap, uint16*) = td->td_orientation;
		break;
	case TIFFTAG_SAMPLESPERPIXEL:
		*va_arg(ap, uint16*) = td->td_samplesperpixel;
		break;
	case TIFFTAG_ROWSPERSTRIP:
		*va_arg(ap, uint32*) = td->td_rowsperstrip;
		break;
	case TIFFTAG_MINSAMPLEVALUE:
		*va_arg(ap, uint16*) = td->td_minsamplevalue;
		break;
	case TIFFTAG_MAXSAMPLEVALUE:
		*va_arg(ap, uint16*) = td->td_maxsamplevalue;
		break;
	case TIFFTAG_SMINSAMPLEVALUE:
		*va_arg(ap, double*) = td->td_sminsamplevalue;
		break;
	case TIFFTAG_SMAXSAMPLEVALUE:
		*va_arg(ap, double*) = td->td_smaxsamplevalue;
		break;
	case TIFFTAG_XRESOLUTION:
		*va_arg(ap, float*) = td->td_xresolution;
		break;
	case TIFFTAG_YRESOLUTION:
		*va_arg(ap, float*) = td->td_yresolution;
		break;
	case TIFFTAG_PLANARCONFIG:
		*va_arg(ap, uint16*) = td->td_planarconfig;
		break;
	case TIFFTAG_XPOSITION:
		*va_arg(ap, float*) = td->td_xposition;
		break;
	case TIFFTAG_YPOSITION:
		*va_arg(ap, float*) = td->td_yposition;
		break;
	case TIFFTAG_PAGENAME:
		*va_arg(ap, char**) = td->td_pagename;
		break;
	case TIFFTAG_RESOLUTIONUNIT:
		*va_arg(ap, uint16*) = td->td_resolutionunit;
		break;
	case TIFFTAG_PAGENUMBER:
		*va_arg(ap, uint16*) = td->td_pagenumber[0];
		*va_arg(ap, uint16*) = td->td_pagenumber[1];
		break;
	case TIFFTAG_HALFTONEHINTS:
		*va_arg(ap, uint16*) = td->td_halftonehints[0];
		*va_arg(ap, uint16*) = td->td_halftonehints[1];
		break;
	case TIFFTAG_COLORMAP:
		*va_arg(ap, uint16**) = td->td_colormap[0];
		*va_arg(ap, uint16**) = td->td_colormap[1];
		*va_arg(ap, uint16**) = td->td_colormap[2];
		break;
	case TIFFTAG_STRIPOFFSETS:
	case TIFFTAG_TILEOFFSETS:
		*va_arg(ap, uint32**) = td->td_stripoffset;
		break;
	case TIFFTAG_STRIPBYTECOUNTS:
	case TIFFTAG_TILEBYTECOUNTS:
		*va_arg(ap, uint32**) = td->td_stripbytecount;
		break;
	case TIFFTAG_MATTEING:
		*va_arg(ap, uint16*) =
		    (td->td_extrasamples == 1 &&
		     td->td_sampleinfo[0] == EXTRASAMPLE_ASSOCALPHA);
		break;
	case TIFFTAG_EXTRASAMPLES:
		*va_arg(ap, uint16*) = td->td_extrasamples;
		*va_arg(ap, uint16**) = td->td_sampleinfo;
		break;
	case TIFFTAG_TILEWIDTH:
		*va_arg(ap, uint32*) = td->td_tilewidth;
		break;
	case TIFFTAG_TILELENGTH:
		*va_arg(ap, uint32*) = td->td_tilelength;
		break;
	case TIFFTAG_TILEDEPTH:
		*va_arg(ap, uint32*) = td->td_tiledepth;
		break;
	case TIFFTAG_DATATYPE:
		switch (td->td_sampleformat) {
		case SAMPLEFORMAT_UINT:
			*va_arg(ap, uint16*) = DATATYPE_UINT;
			break;
		case SAMPLEFORMAT_INT:
			*va_arg(ap, uint16*) = DATATYPE_INT;
			break;
		case SAMPLEFORMAT_IEEEFP:
			*va_arg(ap, uint16*) = DATATYPE_IEEEFP;
			break;
		case SAMPLEFORMAT_VOID:
			*va_arg(ap, uint16*) = DATATYPE_VOID;
			break;
		}
		break;
	case TIFFTAG_SAMPLEFORMAT:
		*va_arg(ap, uint16*) = td->td_sampleformat;
		break;
	case TIFFTAG_IMAGEDEPTH:
		*va_arg(ap, uint32*) = td->td_imagedepth;
		break;
	case TIFFTAG_STONITS:
		*va_arg(ap, double*) = td->td_stonits;
		break;
#if SUBIFD_SUPPORT
	case TIFFTAG_SUBIFD:
		*va_arg(ap, uint16*) = td->td_nsubifd;
		*va_arg(ap, uint32**) = td->td_subifd;
		break;
#endif
#ifdef YCBCR_SUPPORT
	case TIFFTAG_YCBCRCOEFFICIENTS:
		*va_arg(ap, float**) = td->td_ycbcrcoeffs;
		break;
	case TIFFTAG_YCBCRPOSITIONING:
		*va_arg(ap, uint16*) = td->td_ycbcrpositioning;
		break;
	case TIFFTAG_YCBCRSUBSAMPLING:
		*va_arg(ap, uint16*) = td->td_ycbcrsubsampling[0];
		*va_arg(ap, uint16*) = td->td_ycbcrsubsampling[1];
		break;
#endif
#ifdef COLORIMETRY_SUPPORT
	case TIFFTAG_WHITEPOINT:
		*va_arg(ap, float**) = td->td_whitepoint;
		break;
	case TIFFTAG_PRIMARYCHROMATICITIES:
		*va_arg(ap, float**) = td->td_primarychromas;
		break;
	case TIFFTAG_TRANSFERFUNCTION:
		*va_arg(ap, uint16**) = td->td_transferfunction[0];
		if (td->td_samplesperpixel - td->td_extrasamples > 1) {
			*va_arg(ap, uint16**) = td->td_transferfunction[1];
			*va_arg(ap, uint16**) = td->td_transferfunction[2];
		}
		break;
	case TIFFTAG_REFERENCEBLACKWHITE:
		*va_arg(ap, float**) = td->td_refblackwhite;
		break;
#endif
#ifdef CMYK_SUPPORT
	case TIFFTAG_INKSET:
		*va_arg(ap, uint16*) = td->td_inkset;
		break;
	case TIFFTAG_DOTRANGE:
		*va_arg(ap, uint16*) = td->td_dotrange[0];
		*va_arg(ap, uint16*) = td->td_dotrange[1];
		break;
	case TIFFTAG_INKNAMES:
		*va_arg(ap, char**) = td->td_inknames;
		break;
	case TIFFTAG_NUMBEROFINKS:
		*va_arg(ap, uint16*) = td->td_ninks;
		break;
	case TIFFTAG_TARGETPRINTER:
		*va_arg(ap, char**) = td->td_targetprinter;
		break;
#endif
#ifdef ICC_SUPPORT
	case TIFFTAG_ICCPROFILE:
		*va_arg(ap, uint32*) = td->td_profileLength;
		*va_arg(ap, void**) = td->td_profileData;
		break;
#endif
#ifdef PHOTOSHOP_SUPPORT
 	case TIFFTAG_PHOTOSHOP:
 		*va_arg(ap, uint32*) = td->td_photoshopLength;
 		*va_arg(ap, void**) = td->td_photoshopData;
 		break;
#endif
#ifdef IPTC_SUPPORT
 	case TIFFTAG_RICHTIFFIPTC:
 		*va_arg(ap, uint32*) = td->td_richtiffiptcLength;
 		*va_arg(ap, void**) = td->td_richtiffiptcData;
 		break;
#endif
 	/* Begin Pixar Tags */
 	case TIFFTAG_PIXAR_IMAGEFULLWIDTH:
 		*va_arg(ap, uint32*) = td->td_imagefullwidth;
 		break;
 	case TIFFTAG_PIXAR_IMAGEFULLLENGTH:
 		*va_arg(ap, uint32*) = td->td_imagefulllength;
 		break;
 	case TIFFTAG_PIXAR_TEXTUREFORMAT:
 		*va_arg(ap, char**) = td->td_textureformat;
 		break;
 	case TIFFTAG_PIXAR_WRAPMODES:
 		*va_arg(ap, char**) = td->td_wrapmodes;
 		break;
 	case TIFFTAG_PIXAR_FOVCOT:
 		*va_arg(ap, float*) = td->td_fovcot;
 		break;
 	case TIFFTAG_PIXAR_MATRIX_WORLDTOSCREEN:
 		*va_arg(ap, float**) = td->td_matrixWorldToScreen;
 		break;
 	case TIFFTAG_PIXAR_MATRIX_WORLDTOCAMERA:
 		*va_arg(ap, float**) = td->td_matrixWorldToCamera;
 		break;
 	/* End Pixar Tags */

	default:
		/*
		 * This can happen if multiple images are open with
		 * different codecs which have private tags.  The
		 * global tag information table may then have tags
		 * that are valid for one file but not the other. 
		 * If the client tries to get a tag that is not valid
		 * for the image's codec then we'll arrive here.
		 */
		TIFFError("TIFFGetField",
		    "%s: Invalid %stag \"%s\" (not supported by codec)",
		    tif->tif_name, isPseudoTag(tag) ? "pseudo-" : "",
		    _TIFFFieldWithTag(tif, tag)->field_name);
		break;
	}
	return (1);
}

/*
 * Return the value of a field in the
 * internal directory structure.
 */
int
TIFFGetField(TIFF* tif, ttag_t tag, ...)
{
	int status;
	va_list ap;

	va_start(ap, tag);
	status = TIFFVGetField(tif, tag, ap);
	va_end(ap);
	return (status);
}

/*
 * Like TIFFGetField, but taking a varargs
 * parameter list.  This routine is useful
 * for building higher-level interfaces on
 * top of the library.
 */
int
TIFFVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	const TIFFFieldInfo* fip = _TIFFFindFieldInfo(tif, tag, TIFF_ANY);
	return (fip && (isPseudoTag(tag) || TIFFFieldSet(tif, fip->field_bit)) ?
	    (*tif->tif_vgetfield)(tif, tag, ap) : 0);
}

#define	CleanupField(member) {		\
    if (td->member) {			\
	_TIFFfree(td->member);		\
	td->member = 0;			\
    }					\
}

/*
 * Release storage associated with a directory.
 */
void
TIFFFreeDirectory(TIFF* tif)
{
	register TIFFDirectory *td = &tif->tif_dir;

	CleanupField(td_colormap[0]);
	CleanupField(td_colormap[1]);
	CleanupField(td_colormap[2]);
	CleanupField(td_documentname);
	CleanupField(td_artist);
	CleanupField(td_datetime);
	CleanupField(td_hostcomputer);
	CleanupField(td_imagedescription);
	CleanupField(td_make);
	CleanupField(td_model);
	CleanupField(td_software);
	CleanupField(td_pagename);
	CleanupField(td_sampleinfo);
#if SUBIFD_SUPPORT
	CleanupField(td_subifd);
#endif
#ifdef YCBCR_SUPPORT
	CleanupField(td_ycbcrcoeffs);
#endif
#ifdef CMYK_SUPPORT
	CleanupField(td_inknames);
	CleanupField(td_targetprinter);
#endif
#ifdef COLORIMETRY_SUPPORT
	CleanupField(td_whitepoint);
	CleanupField(td_primarychromas);
	CleanupField(td_refblackwhite);
	CleanupField(td_transferfunction[0]);
	CleanupField(td_transferfunction[1]);
	CleanupField(td_transferfunction[2]);
#endif
#ifdef ICC_SUPPORT
	CleanupField(td_profileData);
#endif
#ifdef PHOTOSHOP_SUPPORT
	CleanupField(td_photoshopData);
#endif
#ifdef IPTC_SUPPORT
	CleanupField(td_richtiffiptcData);
#endif
	CleanupField(td_stripoffset);
	CleanupField(td_stripbytecount);
 	/* Begin Pixar Tags */
 	CleanupField(td_textureformat);
 	CleanupField(td_wrapmodes);
 	CleanupField(td_matrixWorldToScreen);
 	CleanupField(td_matrixWorldToCamera);
 	/* End Pixar Tags */
}
#undef CleanupField

/*
 * Client Tag extension support (from Niles Ritter).
 */
static TIFFExtendProc _TIFFextender = (TIFFExtendProc) NULL;

TIFFExtendProc
TIFFSetTagExtender(TIFFExtendProc extender)
{
	TIFFExtendProc prev = _TIFFextender;
	_TIFFextender = extender;
	return (prev);
}

/*
 * Setup a default directory structure.
 */
int
TIFFDefaultDirectory(TIFF* tif)
{
	register TIFFDirectory* td = &tif->tif_dir;

	_TIFFSetupFieldInfo(tif);
	_TIFFmemset(td, 0, sizeof (*td));
	td->td_fillorder = FILLORDER_MSB2LSB;
	td->td_bitspersample = 1;
	td->td_threshholding = THRESHHOLD_BILEVEL;
	td->td_orientation = ORIENTATION_TOPLEFT;
	td->td_samplesperpixel = 1;
	td->td_rowsperstrip = (uint32) -1;
	td->td_tilewidth = (uint32) -1;
	td->td_tilelength = (uint32) -1;
	td->td_tiledepth = 1;
	td->td_resolutionunit = RESUNIT_INCH;
	td->td_sampleformat = SAMPLEFORMAT_VOID;
	td->td_imagedepth = 1;
#ifdef YCBCR_SUPPORT
	td->td_ycbcrsubsampling[0] = 2;
	td->td_ycbcrsubsampling[1] = 2;
	td->td_ycbcrpositioning = YCBCRPOSITION_CENTERED;
#endif
#ifdef CMYK_SUPPORT
	td->td_inkset = INKSET_CMYK;
	td->td_ninks = 4;
#endif
	tif->tif_postdecode = _TIFFNoPostDecode;
	tif->tif_vsetfield = _TIFFVSetField;
	tif->tif_vgetfield = _TIFFVGetField;
	tif->tif_printdir = NULL;
	/*
	 *  Give client code a chance to install their own
	 *  tag extensions & methods, prior to compression overloads.
	 */
	if (_TIFFextender)
		(*_TIFFextender)(tif);
	(void) TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	/*
	 * NB: The directory is marked dirty as a result of setting
	 * up the default compression scheme.  However, this really
	 * isn't correct -- we want TIFF_DIRTYDIRECT to be set only
	 * if the user does something.  We could just do the setup
	 * by hand, but it seems better to use the normal mechanism
	 * (i.e. TIFFSetField).
	 */
	tif->tif_flags &= ~TIFF_DIRTYDIRECT;
	return (1);
}

static int
TIFFAdvanceDirectory(TIFF* tif, uint32* nextdir, toff_t* off)
{
	static const char module[] = "TIFFAdvanceDirectory";
	uint16 dircount;
	if (isMapped(tif))
	  {
	    tsize_t poff=*nextdir;
	    if (((tsize_t) (poff+sizeof(uint16))) > tif->tif_size)
	      {
		TIFFError(module, "%s: Error fetching directory count",
			  tif->tif_name);
		return (0);
	      }
	    _TIFFmemcpy(&dircount, tif->tif_base+poff, sizeof (uint16));
	    if (tif->tif_flags & TIFF_SWAB)
	      TIFFSwabShort(&dircount);
	    poff+=sizeof (uint16)+dircount*sizeof (TIFFDirEntry);
	    if (off != NULL)
	      *off = poff;
	    if (((tsize_t) (poff+sizeof (uint32))) > tif->tif_size)
	      {
		TIFFError(module, "%s: Error fetching directory link",
			  tif->tif_name);
		return (0);
	      }
	    _TIFFmemcpy(nextdir, tif->tif_base+poff, sizeof (uint32));
	    if (tif->tif_flags & TIFF_SWAB)
	      TIFFSwabLong(nextdir);
	    return (1);
	  }
	else
	  {
	    if (!SeekOK(tif, *nextdir) ||
		!ReadOK(tif, &dircount, sizeof (uint16))) {
	      TIFFError(module, "%s: Error fetching directory count",
			tif->tif_name);
	      return (0);
	    }
	    if (tif->tif_flags & TIFF_SWAB)
	      TIFFSwabShort(&dircount);
	    if (off != NULL)
	      *off = TIFFSeekFile(tif,
				  dircount*sizeof (TIFFDirEntry), SEEK_CUR);
	    else
	      (void) TIFFSeekFile(tif,
				  dircount*sizeof (TIFFDirEntry), SEEK_CUR);
	    if (!ReadOK(tif, nextdir, sizeof (uint32))) {
	      TIFFError(module, "%s: Error fetching directory link",
			tif->tif_name);
	      return (0);
	    }
	    if (tif->tif_flags & TIFF_SWAB)
	      TIFFSwabLong(nextdir);
	    return (1);
	  }
}

/*
 * Count the number of directories in a file.
 */
tdir_t
TIFFNumberOfDirectories(TIFF* tif)
{
	uint32 nextdir = tif->tif_header.tiff_diroff;
	tdir_t n = 0;

	while (nextdir != 0 && TIFFAdvanceDirectory(tif, &nextdir, NULL))
		n++;
	return (n);
}

/*
 * Set the n-th directory as the current directory.
 * NB: Directories are numbered starting at 0.
 */
int
TIFFSetDirectory(TIFF* tif, tdir_t dirn)
{
	uint32 nextdir;
	tdir_t n;

	nextdir = tif->tif_header.tiff_diroff;
	for (n = dirn; n > 0 && nextdir != 0; n--)
		if (!TIFFAdvanceDirectory(tif, &nextdir, NULL))
			return (0);
	tif->tif_nextdiroff = nextdir;
	/*
	 * Set curdir to the actual directory index.  The
	 * -1 is because TIFFReadDirectory will increment
	 * tif_curdir after successfully reading the directory.
	 */
	tif->tif_curdir = (dirn - n) - 1;
	return (TIFFReadDirectory(tif));
}

/*
 * Set the current directory to be the directory
 * located at the specified file offset.  This interface
 * is used mainly to access directories linked with
 * the SubIFD tag (e.g. thumbnail images).
 */
int
TIFFSetSubDirectory(TIFF* tif, uint32 diroff)
{
	tif->tif_nextdiroff = diroff;
	return (TIFFReadDirectory(tif));
}

/*
 * Return file offset of the current directory.
 */
uint32
TIFFCurrentDirOffset(TIFF* tif)
{
	return (tif->tif_diroff);
}

/*
 * Return an indication of whether or not we are
 * at the last directory in the file.
 */
int
TIFFLastDirectory(TIFF* tif)
{
	return (tif->tif_nextdiroff == 0);
}

/*
 * Unlink the specified directory from the directory chain.
 */
int
TIFFUnlinkDirectory(TIFF* tif, tdir_t dirn)
{
	static const char module[] = "TIFFUnlinkDirectory";
	uint32 nextdir;
	toff_t off;
	tdir_t n;

	if (tif->tif_mode == O_RDONLY) {
		TIFFError(module, "Can not unlink directory in read-only file");
		return (0);
	}
	/*
	 * Go to the directory before the one we want
	 * to unlink and nab the offset of the link
	 * field we'll need to patch.
	 */
	nextdir = tif->tif_header.tiff_diroff;
	off = sizeof (uint16) + sizeof (uint16);
	for (n = dirn-1; n > 0; n--) {
		if (nextdir == 0) {
			TIFFError(module, "Directory %d does not exist", dirn);
			return (0);
		}
		if (!TIFFAdvanceDirectory(tif, &nextdir, &off))
			return (0);
	}
	/*
	 * Advance to the directory to be unlinked and fetch
	 * the offset of the directory that follows.
	 */
	if (!TIFFAdvanceDirectory(tif, &nextdir, NULL))
		return (0);
	/*
	 * Go back and patch the link field of the preceding
	 * directory to point to the offset of the directory
	 * that follows.
	 */
	(void) TIFFSeekFile(tif, off, SEEK_SET);
	if (tif->tif_flags & TIFF_SWAB)
		TIFFSwabLong(&nextdir);
	if (!WriteOK(tif, &nextdir, sizeof (uint32))) {
		TIFFError(module, "Error writing directory link");
		return (0);
	}
	/*
	 * Leave directory state setup safely.  We don't have
	 * facilities for doing inserting and removing directories,
	 * so it's safest to just invalidate everything.  This
	 * means that the caller can only append to the directory
	 * chain.
	 */
	(*tif->tif_cleanup)(tif);
	if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata) {
		_TIFFfree(tif->tif_rawdata);
		tif->tif_rawdata = NULL;
		tif->tif_rawcc = 0;
	}
	tif->tif_flags &= ~(TIFF_BEENWRITING|TIFF_BUFFERSETUP|TIFF_POSTENCODE);
	TIFFFreeDirectory(tif);
	TIFFDefaultDirectory(tif);
	tif->tif_diroff = 0;			/* force link on next write */
	tif->tif_nextdiroff = 0;		/* next write must be at end */
	tif->tif_curoff = 0;
	tif->tif_row = (uint32) -1;
	tif->tif_curstrip = (tstrip_t) -1;
	return (1);
}

/*			[BFC]
 *
 * Author: Bruce Cameron <cameron@petris.com>
 *
 * Set a table of tags that are to be replaced during directory process by the
 * 'IGNORE' state - or return TRUE/FALSE for the requested tag such that
 * 'ReadDirectory' can use the stored information.
 */
int
TIFFReassignTagToIgnore (enum TIFFIgnoreSense task, int TIFFtagID)
{
    static int TIFFignoretags [FIELD_LAST];
    static int tagcount = 0 ;
    int		i;					/* Loop index */
    int		j;					/* Loop index */

    switch (task)
    {
      case TIS_STORE:
        if ( tagcount < (FIELD_LAST - 1) )
        {
            for ( j = 0 ; j < tagcount ; ++j )
            {					/* Do not add duplicate tag */
                if ( TIFFignoretags [j] == TIFFtagID )
                    return (TRUE) ;
            }
            TIFFignoretags [tagcount++] = TIFFtagID ;
            return (TRUE) ;
        }
        break ;
        
      case TIS_EXTRACT:
        for ( i = 0 ; i < tagcount ; ++i )
        {
            if ( TIFFignoretags [i] == TIFFtagID )
                return (TRUE) ;
        }
        break;
        
      case TIS_EMPTY:
        tagcount = 0 ;			/* Clear the list */
        return (TRUE) ;
        break;
        
      default:
        break;
    }
    
    return (FALSE);
}


/* ==== tif_dirinfo.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_dirinfo.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Core Directory Tag Support.
 */
#include "tiffiop.h"
#include <stdlib.h>

/*
 * NB: NB: THIS ARRAY IS ASSUMED TO BE SORTED BY TAG.
 *     If a tag can have both LONG and SHORT types
 *     then the LONG must be placed before the SHORT for
 *     writing to work properly.
 */
static const TIFFFieldInfo tiffFieldInfo[] = {
    { TIFFTAG_SUBFILETYPE,	 1, 1, TIFF_LONG,	FIELD_SUBFILETYPE,
      TRUE,	FALSE,	"SubfileType" },
/* XXX SHORT for compatibility w/ old versions of the library */
    { TIFFTAG_SUBFILETYPE,	 1, 1, TIFF_SHORT,	FIELD_SUBFILETYPE,
      TRUE,	FALSE,	"SubfileType" },
    { TIFFTAG_OSUBFILETYPE,	 1, 1, TIFF_SHORT,	FIELD_SUBFILETYPE,
      TRUE,	FALSE,	"OldSubfileType" },
    { TIFFTAG_IMAGEWIDTH,	 1, 1, TIFF_LONG,	FIELD_IMAGEDIMENSIONS,
      FALSE,	FALSE,	"ImageWidth" },
    { TIFFTAG_IMAGEWIDTH,	 1, 1, TIFF_SHORT,	FIELD_IMAGEDIMENSIONS,
      FALSE,	FALSE,	"ImageWidth" },
    { TIFFTAG_IMAGELENGTH,	 1, 1, TIFF_LONG,	FIELD_IMAGEDIMENSIONS,
      TRUE,	FALSE,	"ImageLength" },
    { TIFFTAG_IMAGELENGTH,	 1, 1, TIFF_SHORT,	FIELD_IMAGEDIMENSIONS,
      TRUE,	FALSE,	"ImageLength" },
    { TIFFTAG_BITSPERSAMPLE,	-1,-1, TIFF_SHORT,	FIELD_BITSPERSAMPLE,
      FALSE,	FALSE,	"BitsPerSample" },
    { TIFFTAG_COMPRESSION,	-1, 1, TIFF_SHORT,	FIELD_COMPRESSION,
      FALSE,	FALSE,	"Compression" },
    { TIFFTAG_PHOTOMETRIC,	 1, 1, TIFF_SHORT,	FIELD_PHOTOMETRIC,
      FALSE,	FALSE,	"PhotometricInterpretation" },
    { TIFFTAG_THRESHHOLDING,	 1, 1, TIFF_SHORT,	FIELD_THRESHHOLDING,
      TRUE,	FALSE,	"Threshholding" },
    { TIFFTAG_CELLWIDTH,	 1, 1, TIFF_SHORT,	FIELD_IGNORE,
      TRUE,	FALSE,	"CellWidth" },
    { TIFFTAG_CELLLENGTH,	 1, 1, TIFF_SHORT,	FIELD_IGNORE,
      TRUE,	FALSE,	"CellLength" },
    { TIFFTAG_FILLORDER,	 1, 1, TIFF_SHORT,	FIELD_FILLORDER,
      FALSE,	FALSE,	"FillOrder" },
    { TIFFTAG_DOCUMENTNAME,	-1,-1, TIFF_ASCII,	FIELD_DOCUMENTNAME,
      TRUE,	FALSE,	"DocumentName" },
    { TIFFTAG_IMAGEDESCRIPTION,	-1,-1, TIFF_ASCII,	FIELD_IMAGEDESCRIPTION,
      TRUE,	FALSE,	"ImageDescription" },
    { TIFFTAG_MAKE,		-1,-1, TIFF_ASCII,	FIELD_MAKE,
      TRUE,	FALSE,	"Make" },
    { TIFFTAG_MODEL,		-1,-1, TIFF_ASCII,	FIELD_MODEL,
      TRUE,	FALSE,	"Model" },
    { TIFFTAG_STRIPOFFSETS,	-1,-1, TIFF_LONG,	FIELD_STRIPOFFSETS,
      FALSE,	FALSE,	"StripOffsets" },
    { TIFFTAG_STRIPOFFSETS,	-1,-1, TIFF_SHORT,	FIELD_STRIPOFFSETS,
      FALSE,	FALSE,	"StripOffsets" },
    { TIFFTAG_ORIENTATION,	 1, 1, TIFF_SHORT,	FIELD_ORIENTATION,
      FALSE,	FALSE,	"Orientation" },
    { TIFFTAG_SAMPLESPERPIXEL,	 1, 1, TIFF_SHORT,	FIELD_SAMPLESPERPIXEL,
      FALSE,	FALSE,	"SamplesPerPixel" },
    { TIFFTAG_ROWSPERSTRIP,	 1, 1, TIFF_LONG,	FIELD_ROWSPERSTRIP,
      FALSE,	FALSE,	"RowsPerStrip" },
    { TIFFTAG_ROWSPERSTRIP,	 1, 1, TIFF_SHORT,	FIELD_ROWSPERSTRIP,
      FALSE,	FALSE,	"RowsPerStrip" },
    { TIFFTAG_STRIPBYTECOUNTS,	-1,-1, TIFF_LONG,	FIELD_STRIPBYTECOUNTS,
      FALSE,	FALSE,	"StripByteCounts" },
    { TIFFTAG_STRIPBYTECOUNTS,	-1,-1, TIFF_SHORT,	FIELD_STRIPBYTECOUNTS,
      FALSE,	FALSE,	"StripByteCounts" },
    { TIFFTAG_MINSAMPLEVALUE,	-2,-1, TIFF_SHORT,	FIELD_MINSAMPLEVALUE,
      TRUE,	FALSE,	"MinSampleValue" },
    { TIFFTAG_MAXSAMPLEVALUE,	-2,-1, TIFF_SHORT,	FIELD_MAXSAMPLEVALUE,
      TRUE,	FALSE,	"MaxSampleValue" },
    { TIFFTAG_XRESOLUTION,	 1, 1, TIFF_RATIONAL,	FIELD_RESOLUTION,
      FALSE,	FALSE,	"XResolution" },
    { TIFFTAG_YRESOLUTION,	 1, 1, TIFF_RATIONAL,	FIELD_RESOLUTION,
      FALSE,	FALSE,	"YResolution" },
    { TIFFTAG_PLANARCONFIG,	 1, 1, TIFF_SHORT,	FIELD_PLANARCONFIG,
      FALSE,	FALSE,	"PlanarConfiguration" },
    { TIFFTAG_PAGENAME,		-1,-1, TIFF_ASCII,	FIELD_PAGENAME,
      TRUE,	FALSE,	"PageName" },
    { TIFFTAG_XPOSITION,	 1, 1, TIFF_RATIONAL,	FIELD_POSITION,
      TRUE,	FALSE,	"XPosition" },
    { TIFFTAG_YPOSITION,	 1, 1, TIFF_RATIONAL,	FIELD_POSITION,
      TRUE,	FALSE,	"YPosition" },
    { TIFFTAG_FREEOFFSETS,	-1,-1, TIFF_LONG,	FIELD_IGNORE,
      FALSE,	FALSE,	"FreeOffsets" },
    { TIFFTAG_FREEBYTECOUNTS,	-1,-1, TIFF_LONG,	FIELD_IGNORE,
      FALSE,	FALSE,	"FreeByteCounts" },
    { TIFFTAG_GRAYRESPONSEUNIT,	 1, 1, TIFF_SHORT,	FIELD_IGNORE,
      TRUE,	FALSE,	"GrayResponseUnit" },
    { TIFFTAG_GRAYRESPONSECURVE,-1,-1, TIFF_SHORT,	FIELD_IGNORE,
      TRUE,	FALSE,	"GrayResponseCurve" },
    { TIFFTAG_RESOLUTIONUNIT,	 1, 1, TIFF_SHORT,	FIELD_RESOLUTIONUNIT,
      FALSE,	FALSE,	"ResolutionUnit" },
    { TIFFTAG_PAGENUMBER,	 2, 2, TIFF_SHORT,	FIELD_PAGENUMBER,
      TRUE,	FALSE,	"PageNumber" },
    { TIFFTAG_COLORRESPONSEUNIT, 1, 1, TIFF_SHORT,	FIELD_IGNORE,
      TRUE,	FALSE,	"ColorResponseUnit" },
#ifdef COLORIMETRY_SUPPORT
    { TIFFTAG_TRANSFERFUNCTION,	-1,-1, TIFF_SHORT,	FIELD_TRANSFERFUNCTION,
      TRUE,	FALSE,	"TransferFunction" },
#endif
    { TIFFTAG_SOFTWARE,		-1,-1, TIFF_ASCII,	FIELD_SOFTWARE,
      TRUE,	FALSE,	"Software" },
    { TIFFTAG_DATETIME,		20,20, TIFF_ASCII,	FIELD_DATETIME,
      TRUE,	FALSE,	"DateTime" },
    { TIFFTAG_ARTIST,		-1,-1, TIFF_ASCII,	FIELD_ARTIST,
      TRUE,	FALSE,	"Artist" },
    { TIFFTAG_HOSTCOMPUTER,	-1,-1, TIFF_ASCII,	FIELD_HOSTCOMPUTER,
      TRUE,	FALSE,	"HostComputer" },
#ifdef COLORIMETRY_SUPPORT
    { TIFFTAG_WHITEPOINT,	 2, 2, TIFF_RATIONAL,FIELD_WHITEPOINT,
      TRUE,	FALSE,	"WhitePoint" },
    { TIFFTAG_PRIMARYCHROMATICITIES,6,6,TIFF_RATIONAL,FIELD_PRIMARYCHROMAS,
      TRUE,	FALSE,	"PrimaryChromaticities" },
#endif
    { TIFFTAG_COLORMAP,		-1,-1, TIFF_SHORT,	FIELD_COLORMAP,
      TRUE,	FALSE,	"ColorMap" },
    { TIFFTAG_HALFTONEHINTS,	 2, 2, TIFF_SHORT,	FIELD_HALFTONEHINTS,
      TRUE,	FALSE,	"HalftoneHints" },
    { TIFFTAG_TILEWIDTH,	 1, 1, TIFF_LONG,	FIELD_TILEDIMENSIONS,
      FALSE,	FALSE,	"TileWidth" },
    { TIFFTAG_TILEWIDTH,	 1, 1, TIFF_SHORT,	FIELD_TILEDIMENSIONS,
      FALSE,	FALSE,	"TileWidth" },
    { TIFFTAG_TILELENGTH,	 1, 1, TIFF_LONG,	FIELD_TILEDIMENSIONS,
      FALSE,	FALSE,	"TileLength" },
    { TIFFTAG_TILELENGTH,	 1, 1, TIFF_SHORT,	FIELD_TILEDIMENSIONS,
      FALSE,	FALSE,	"TileLength" },
    { TIFFTAG_TILEOFFSETS,	-1, 1, TIFF_LONG,	FIELD_STRIPOFFSETS,
      FALSE,	FALSE,	"TileOffsets" },
    { TIFFTAG_TILEBYTECOUNTS,	-1, 1, TIFF_LONG,	FIELD_STRIPBYTECOUNTS,
      FALSE,	FALSE,	"TileByteCounts" },
    { TIFFTAG_TILEBYTECOUNTS,	-1, 1, TIFF_SHORT,	FIELD_STRIPBYTECOUNTS,
      FALSE,	FALSE,	"TileByteCounts" },
#ifdef TIFFTAG_SUBIFD
    { TIFFTAG_SUBIFD,		-1,-1, TIFF_LONG,	FIELD_SUBIFD,
      TRUE,	TRUE,	"SubIFD" },
#endif
#ifdef CMYK_SUPPORT		/* 6.0 CMYK tags */
    { TIFFTAG_INKSET,		 1, 1, TIFF_SHORT,	FIELD_INKSET,
      FALSE,	FALSE,	"InkSet" },
    { TIFFTAG_INKNAMES,		-1,-1, TIFF_ASCII,	FIELD_INKNAMES,
      TRUE,	TRUE,	"InkNames" },
    { TIFFTAG_NUMBEROFINKS,	 1, 1, TIFF_SHORT,	FIELD_NUMBEROFINKS,
      TRUE,	FALSE,	"NumberOfInks" },
    { TIFFTAG_DOTRANGE,		 2, 2, TIFF_SHORT,	FIELD_DOTRANGE,
      FALSE,	FALSE,	"DotRange" },
    { TIFFTAG_DOTRANGE,		 2, 2, TIFF_BYTE,	FIELD_DOTRANGE,
      FALSE,	FALSE,	"DotRange" },
    { TIFFTAG_TARGETPRINTER,	-1,-1, TIFF_ASCII,	FIELD_TARGETPRINTER,
      TRUE,	FALSE,	"TargetPrinter" },
#endif
    { TIFFTAG_EXTRASAMPLES,	-1,-1, TIFF_SHORT,	FIELD_EXTRASAMPLES,
      FALSE,	FALSE,	"ExtraSamples" },
/* XXX for bogus Adobe Photoshop v2.5 files */
    { TIFFTAG_EXTRASAMPLES,	-1,-1, TIFF_BYTE,	FIELD_EXTRASAMPLES,
      FALSE,	FALSE,	"ExtraSamples" },
    { TIFFTAG_SAMPLEFORMAT,	-1,-1, TIFF_SHORT,	FIELD_SAMPLEFORMAT,
      FALSE,	FALSE,	"SampleFormat" },
    { TIFFTAG_SMINSAMPLEVALUE,	-2,-1, TIFF_ANY,	FIELD_SMINSAMPLEVALUE,
      TRUE,	FALSE,	"SMinSampleValue" },
    { TIFFTAG_SMAXSAMPLEVALUE,	-2,-1, TIFF_ANY,	FIELD_SMAXSAMPLEVALUE,
      TRUE,	FALSE,	"SMaxSampleValue" },
#ifdef YCBCR_SUPPORT		/* 6.0 YCbCr tags */
    { TIFFTAG_YCBCRCOEFFICIENTS, 3, 3, TIFF_RATIONAL,	FIELD_YCBCRCOEFFICIENTS,
      FALSE,	FALSE,	"YCbCrCoefficients" },
    { TIFFTAG_YCBCRSUBSAMPLING,	 2, 2, TIFF_SHORT,	FIELD_YCBCRSUBSAMPLING,
      FALSE,	FALSE,	"YCbCrSubsampling" },
    { TIFFTAG_YCBCRPOSITIONING,	 1, 1, TIFF_SHORT,	FIELD_YCBCRPOSITIONING,
      FALSE,	FALSE,	"YCbCrPositioning" },
#endif
#ifdef COLORIMETRY_SUPPORT
    { TIFFTAG_REFERENCEBLACKWHITE,6,6,TIFF_RATIONAL,	FIELD_REFBLACKWHITE,
      TRUE,	FALSE,	"ReferenceBlackWhite" },
/* XXX temporarily accept LONG for backwards compatibility */
    { TIFFTAG_REFERENCEBLACKWHITE,6,6,TIFF_LONG,	FIELD_REFBLACKWHITE,
      TRUE,	FALSE,	"ReferenceBlackWhite" },
#endif
/* begin SGI tags */
    { TIFFTAG_MATTEING,		 1, 1, TIFF_SHORT,	FIELD_EXTRASAMPLES,
      FALSE,	FALSE,	"Matteing" },
    { TIFFTAG_DATATYPE,		-2,-1, TIFF_SHORT,	FIELD_SAMPLEFORMAT,
      FALSE,	FALSE,	"DataType" },
    { TIFFTAG_IMAGEDEPTH,	 1, 1, TIFF_LONG,	FIELD_IMAGEDEPTH,
      FALSE,	FALSE,	"ImageDepth" },
    { TIFFTAG_IMAGEDEPTH,	 1, 1, TIFF_SHORT,	FIELD_IMAGEDEPTH,
      FALSE,	FALSE,	"ImageDepth" },
    { TIFFTAG_TILEDEPTH,	 1, 1, TIFF_LONG,	FIELD_TILEDEPTH,
      FALSE,	FALSE,	"TileDepth" },
    { TIFFTAG_TILEDEPTH,	 1, 1, TIFF_SHORT,	FIELD_TILEDEPTH,
      FALSE,	FALSE,	"TileDepth" },
/* end SGI tags */
#ifdef IPTC_SUPPORT
#ifdef PHOTOSHOP_SUPPORT
    { TIFFTAG_RICHTIFFIPTC, -1,-1, TIFF_LONG,   FIELD_RICHTIFFIPTC, 
      FALSE,    TRUE,   "RichTIFFIPTC" },
#else
    { TIFFTAG_RICHTIFFIPTC, -1,-3, TIFF_UNDEFINED, FIELD_RICHTIFFIPTC, 
      FALSE,    TRUE,   "RichTIFFIPTC" },
#endif
#endif
#ifdef PHOTOSHOP_SUPPORT
    { TIFFTAG_PHOTOSHOP,    -1,-3, TIFF_UNDEFINED, FIELD_PHOTOSHOP, 
      FALSE,    TRUE,   "Photoshop" },
    { TIFFTAG_PHOTOSHOP,    -1,-1, TIFF_BYTE,   FIELD_PHOTOSHOP, 
      FALSE,    TRUE,   "Photoshop" },
#endif
#ifdef ICC_SUPPORT
    { TIFFTAG_ICCPROFILE,	-1,-3, TIFF_UNDEFINED,	FIELD_ICCPROFILE,
      FALSE,	TRUE,	"ICC Profile" },
#endif
    { TIFFTAG_STONITS,		 1, 1, TIFF_DOUBLE,	FIELD_STONITS,
      FALSE,	FALSE,	"StoNits" },
/* begin Pixar tags */
    { TIFFTAG_PIXAR_IMAGEFULLWIDTH,  1, 1, TIFF_LONG,	FIELD_IMAGEFULLWIDTH,
      TRUE,	FALSE,	"ImageFullWidth" },
    { TIFFTAG_PIXAR_IMAGEFULLLENGTH, 1, 1, TIFF_LONG,	FIELD_IMAGEFULLLENGTH,
      TRUE,	FALSE,	"ImageFullLength" },
    { TIFFTAG_PIXAR_TEXTUREFORMAT,  -1,-1, TIFF_ASCII,	FIELD_TEXTUREFORMAT,
      TRUE,	FALSE,	"TextureFormat" },
    { TIFFTAG_PIXAR_WRAPMODES,	    -1,-1, TIFF_ASCII,	FIELD_WRAPMODES,
      TRUE,	FALSE,	"TextureWrapModes" },
    { TIFFTAG_PIXAR_FOVCOT,	     1, 1, TIFF_FLOAT,	FIELD_FOVCOT,
      TRUE,	FALSE,	"FieldOfViewCotan" },
    { TIFFTAG_PIXAR_MATRIX_WORLDTOSCREEN,	16,16,	TIFF_FLOAT,
      FIELD_MATRIX_WORLDTOSCREEN,	TRUE,	FALSE,	"MatrixWorldToScreen" },
    { TIFFTAG_PIXAR_MATRIX_WORLDTOCAMERA,	16,16,	TIFF_FLOAT,
       FIELD_MATRIX_WORLDTOCAMERA,	TRUE,	FALSE,	"MatrixWorldToCamera" },
/* end Pixar tags */
};
#define	N(a)	(sizeof (a) / sizeof (a[0]))

void
_TIFFSetupFieldInfo(TIFF* tif)
{
	if (tif->tif_fieldinfo) {
		_TIFFfree(tif->tif_fieldinfo);
		tif->tif_nfields = 0;
	}
	_TIFFMergeFieldInfo(tif, tiffFieldInfo, N(tiffFieldInfo));
}

static int
tagCompare(const void* a, const void* b)
{
	const TIFFFieldInfo* ta = *(const TIFFFieldInfo**) a;
	const TIFFFieldInfo* tb = *(const TIFFFieldInfo**) b;
	/* NB: be careful of return values for 16-bit platforms */
	if (ta->field_tag != tb->field_tag)
		return (ta->field_tag < tb->field_tag ? -1 : 1);
	else
		return (tb->field_type < ta->field_type ? -1 : 1);
}

void
_TIFFMergeFieldInfo(TIFF* tif, const TIFFFieldInfo info[], int n)
{
	TIFFFieldInfo** tp;
	int i;

	if (tif->tif_nfields > 0) {
		tif->tif_fieldinfo = (TIFFFieldInfo**)
		    _TIFFrealloc(tif->tif_fieldinfo,
			(tif->tif_nfields+n) * sizeof (TIFFFieldInfo*));
	} else {
		tif->tif_fieldinfo = (TIFFFieldInfo**)
		    _TIFFmalloc(n * sizeof (TIFFFieldInfo*));
	}
	tp = &tif->tif_fieldinfo[tif->tif_nfields];
	for (i = 0; i < n; i++)
		tp[i] = (TIFFFieldInfo*) &info[i];	/* XXX */
	/*
	 * NB: the core tags are presumed sorted correctly.
	 */
	if (tif->tif_nfields > 0)
		qsort(tif->tif_fieldinfo, (size_t) (tif->tif_nfields += n),
		    sizeof (TIFFFieldInfo*), tagCompare);
	else
		tif->tif_nfields += n;
}

void
_TIFFPrintFieldInfo(TIFF* tif, FILE* fd)
{
	int i;

	fprintf(fd, "%s: \n", tif->tif_name);
	for (i = 0; i < tif->tif_nfields; i++) {
		const TIFFFieldInfo* fip = tif->tif_fieldinfo[i];
		fprintf(fd, "field[%2d] %5lu, %2d, %2d, %d, %2d, %5s, %5s, %s\n"
			, i
			, (unsigned long) fip->field_tag
			, fip->field_readcount, fip->field_writecount
			, fip->field_type
			, fip->field_bit
			, fip->field_oktochange ? "TRUE" : "FALSE"
			, fip->field_passcount ? "TRUE" : "FALSE"
			, fip->field_name
		);
	}
}

const int tiffDataWidth[] = {
    1,	/* nothing */
    1,	/* TIFF_BYTE */
    1,	/* TIFF_ASCII */
    2,	/* TIFF_SHORT */
    4,	/* TIFF_LONG */
    8,	/* TIFF_RATIONAL */
    1,	/* TIFF_SBYTE */
    1,	/* TIFF_UNDEFINED */
    2,	/* TIFF_SSHORT */
    4,	/* TIFF_SLONG */
    8,	/* TIFF_SRATIONAL */
    4,	/* TIFF_FLOAT */
    8,	/* TIFF_DOUBLE */
};

/*
 * Return nearest TIFFDataType to the sample type of an image.
 */
TIFFDataType
_TIFFSampleToTagType(TIFF* tif)
{
	int bps = (int) TIFFhowmany(tif->tif_dir.td_bitspersample, 8);

	switch (tif->tif_dir.td_sampleformat) {
	case SAMPLEFORMAT_IEEEFP:
		return (bps == 4 ? TIFF_FLOAT : TIFF_DOUBLE);
	case SAMPLEFORMAT_INT:
		return (bps <= 1 ? TIFF_SBYTE :
		    bps <= 2 ? TIFF_SSHORT : TIFF_SLONG);
	case SAMPLEFORMAT_UINT:
		return (bps <= 1 ? TIFF_BYTE :
		    bps <= 2 ? TIFF_SHORT : TIFF_LONG);
	case SAMPLEFORMAT_VOID:
		return (TIFF_UNDEFINED);
	}
	/*NOTREACHED*/
	return (TIFF_UNDEFINED);
}

const TIFFFieldInfo*
_TIFFFindFieldInfo(TIFF* tif, ttag_t tag, TIFFDataType dt)
{
	static const TIFFFieldInfo *last = NULL;
	int i, n;

	if (last && last->field_tag == tag &&
	    (dt == TIFF_ANY || dt == last->field_type))
		return (last);
	/* NB: if table gets big, use sorted search (e.g. binary search) */
	for (i = 0, n = tif->tif_nfields; i < n; i++) {
		const TIFFFieldInfo* fip = tif->tif_fieldinfo[i];
		if (fip->field_tag == tag &&
		    (dt == TIFF_ANY || fip->field_type == dt))
			return (last = fip);
	}
	return ((const TIFFFieldInfo *)0);
}

#include <assert.h>
#include <stdio.h>

const TIFFFieldInfo*
_TIFFFieldWithTag(TIFF* tif, ttag_t tag)
{
	const TIFFFieldInfo* fip = _TIFFFindFieldInfo(tif, tag, TIFF_ANY);
	if (!fip) {
		TIFFError("TIFFFieldWithTag",
		    "Internal error, unknown tag 0x%x", (u_int) tag);
		assert(fip != NULL);
		/*NOTREACHED*/
	}
	return (fip);
}


/* ==== tif_dirread.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_dirread.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Directory Read Support Routines.
 */
#include "tiffiop.h"

#define	IGNORE	0		/* tag placeholder used below */

#if HAVE_IEEEFP
#define	TIFFCvtIEEEFloatToNative(tif, n, fp)
#define	TIFFCvtIEEEDoubleToNative(tif, n, dp)
#else
extern	void TIFFCvtIEEEFloatToNative(TIFF*, uint32, float*);
extern	void TIFFCvtIEEEDoubleToNative(TIFF*, uint32, double*);
#endif

static	void EstimateStripByteCounts(TIFF*, TIFFDirEntry*, uint16);
static	void MissingRequired(TIFF*, const char*);
static	int CheckDirCount(TIFF*, TIFFDirEntry*, uint32);
static	tsize_t TIFFFetchData(TIFF*, TIFFDirEntry*, char*);
static	tsize_t TIFFFetchString(TIFF*, TIFFDirEntry*, char*);
static	float TIFFFetchRational(TIFF*, TIFFDirEntry*);
static	int TIFFFetchNormalTag(TIFF*, TIFFDirEntry*);
static	int TIFFFetchPerSampleShorts(TIFF*, TIFFDirEntry*, int*);
static	int TIFFFetchPerSampleAnys(TIFF*, TIFFDirEntry*, double*);
static	int TIFFFetchShortArray(TIFF*, TIFFDirEntry*, uint16*);
static	int TIFFFetchStripThing(TIFF*, TIFFDirEntry*, long, uint32**);
static	int TIFFFetchExtraSamples(TIFF*, TIFFDirEntry*);
static	int TIFFFetchRefBlackWhite(TIFF*, TIFFDirEntry*);
static	float TIFFFetchFloat(TIFF*, TIFFDirEntry*);
static	int TIFFFetchFloatArray(TIFF*, TIFFDirEntry*, float*);
static	int TIFFFetchDoubleArray(TIFF*, TIFFDirEntry*, double*);
static	int TIFFFetchAnyArray(TIFF*, TIFFDirEntry*, double*);
static	int TIFFFetchShortPair(TIFF*, TIFFDirEntry*);
static	void ChopUpSingleUncompressedStrip(TIFF*);

static char *
CheckMalloc(TIFF* tif, tsize_t n, const char* what)
{
	char *cp = (char*)_TIFFmalloc(n);
	if (cp == NULL)
		TIFFError(tif->tif_name, "No space %s", what);
	return (cp);
}

/*
 * Read the next TIFF directory from a file
 * and convert it to the internal format.
 * We read directories sequentially.
 */
int
TIFFReadDirectory(TIFF* tif)
{
	register TIFFDirEntry* dp;
	register int n;
	register TIFFDirectory* td;
	TIFFDirEntry* dir;
	int iv;
	long v;
	double dv;
	const TIFFFieldInfo* fip;
	int fix;
	uint16 dircount;
	uint32 nextdiroff;
	char* cp;
	int diroutoforderwarning = 0;

	tif->tif_diroff = tif->tif_nextdiroff;
	if (tif->tif_diroff == 0)		/* no more directories */
		return (0);
	/*
	 * Cleanup any previous compression state.
	 */
	(*tif->tif_cleanup)(tif);
	tif->tif_curdir++;
	nextdiroff = 0;
	if (!isMapped(tif)) {
		if (!SeekOK(tif, tif->tif_diroff)) {
			TIFFError(tif->tif_name,
			    "Seek error accessing TIFF directory");
			return (0);
		}
		if (!ReadOK(tif, &dircount, sizeof (uint16))) {
			TIFFError(tif->tif_name,
			    "Can not read TIFF directory count");
			return (0);
		}
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabShort(&dircount);
		dir = (TIFFDirEntry *)CheckMalloc(tif,
		    dircount * sizeof (TIFFDirEntry), "to read TIFF directory");
		if (dir == NULL)
			return (0);
		if (!ReadOK(tif, dir, dircount*sizeof (TIFFDirEntry))) {
			TIFFError(tif->tif_name, "Can not read TIFF directory");
			goto bad;
		}
		/*
		 * Read offset to next directory for sequential scans.
		 */
		(void) ReadOK(tif, &nextdiroff, sizeof (uint32));
	} else {
		toff_t off = tif->tif_diroff;

		if ((tsize_t) (off + sizeof (uint16)) > tif->tif_size) {
			TIFFError(tif->tif_name,
			    "Can not read TIFF directory count");
			return (0);
		} else
			_TIFFmemcpy(&dircount, tif->tif_base + off, sizeof (uint16));
		off += sizeof (uint16);
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabShort(&dircount);
		dir = (TIFFDirEntry *)CheckMalloc(tif,
		    dircount * sizeof (TIFFDirEntry), "to read TIFF directory");
		if (dir == NULL)
			return (0);
		if (((tsize_t) (off + dircount*sizeof (TIFFDirEntry)))
                                                   > tif->tif_size) {
			TIFFError(tif->tif_name, "Can not read TIFF directory");
			goto bad;
		} else
			_TIFFmemcpy(dir, tif->tif_base + off,
			    dircount*sizeof (TIFFDirEntry));
		off += dircount* sizeof (TIFFDirEntry);
		if (((tsize_t)(off + sizeof (uint32))) <= tif->tif_size)
			_TIFFmemcpy(&nextdiroff, tif->tif_base+off, sizeof (uint32));
	}
	if (tif->tif_flags & TIFF_SWAB)
		TIFFSwabLong(&nextdiroff);
	tif->tif_nextdiroff = nextdiroff;

	tif->tif_flags &= ~TIFF_BEENWRITING;	/* reset before new dir */
	/*
	 * Setup default value and then make a pass over
	 * the fields to check type and tag information,
	 * and to extract info required to size data
	 * structures.  A second pass is made afterwards
	 * to read in everthing not taken in the first pass.
	 */
	td = &tif->tif_dir;
	/* free any old stuff and reinit */
	TIFFFreeDirectory(tif);
	TIFFDefaultDirectory(tif);
	/*
	 * Electronic Arts writes gray-scale TIFF files
	 * without a PlanarConfiguration directory entry.
	 * Thus we setup a default value here, even though
	 * the TIFF spec says there is no default value.
	 */
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

	/*
	 * Sigh, we must make a separate pass through the
	 * directory for the following reason:
	 *
	 * We must process the Compression tag in the first pass
	 * in order to merge in codec-private tag definitions (otherwise
	 * we may get complaints about unknown tags).  However, the
	 * Compression tag may be dependent on the SamplesPerPixel
	 * tag value because older TIFF specs permited Compression
	 * to be written as a SamplesPerPixel-count tag entry.
	 * Thus if we don't first figure out the correct SamplesPerPixel
	 * tag value then we may end up ignoring the Compression tag
	 * value because it has an incorrect count value (if the
	 * true value of SamplesPerPixel is not 1).
	 *
	 * It sure would have been nice if Aldus had really thought
	 * this stuff through carefully.
	 */ 
	for (dp = dir, n = dircount; n > 0; n--, dp++) {
		if (tif->tif_flags & TIFF_SWAB) {
			TIFFSwabArrayOfShort(&dp->tdir_tag, 2);
			TIFFSwabArrayOfLong(&dp->tdir_count, 2);
		}
		if (dp->tdir_tag == TIFFTAG_SAMPLESPERPIXEL) {
			if (!TIFFFetchNormalTag(tif, dp))
				goto bad;
			dp->tdir_tag = IGNORE;
		}
	}
	/*
	 * First real pass over the directory.
	 */
	fix = 0;
	for (dp = dir, n = dircount; n > 0; n--, dp++) {

                /*
                 * Find the field information entry for this tag.
		 * Added check for tags to ignore ... [BFC]
                 */
		if( TIFFReassignTagToIgnore(TIS_EXTRACT, dp->tdir_tag) )
                    dp->tdir_tag = IGNORE;

		if (dp->tdir_tag == IGNORE)
                    continue;
                
		/*
		 * Silicon Beach (at least) writes unordered
		 * directory tags (violating the spec).  Handle
		 * it here, but be obnoxious (maybe they'll fix it?).
		 */
		if (dp->tdir_tag < tif->tif_fieldinfo[fix]->field_tag) {
			if (!diroutoforderwarning) {
				TIFFWarning(tif->tif_name,
	"invalid TIFF directory; tags are not sorted in ascending order");
				diroutoforderwarning = 1;
			}
			fix = 0;			/* O(n^2) */
		}
		while (fix < tif->tif_nfields &&
		    tif->tif_fieldinfo[fix]->field_tag < dp->tdir_tag)
			fix++;
		if (fix == tif->tif_nfields ||
		    tif->tif_fieldinfo[fix]->field_tag != dp->tdir_tag) {
			TIFFWarning(tif->tif_name,
			    "unknown field with tag %d (0x%x) ignored",
			    dp->tdir_tag,  dp->tdir_tag);
			dp->tdir_tag = IGNORE;
			fix = 0;			/* restart search */
			continue;
		}
		/*
		 * Null out old tags that we ignore.
		 */
		if (tif->tif_fieldinfo[fix]->field_bit == FIELD_IGNORE) {
	ignore:
			dp->tdir_tag = IGNORE;
			continue;
		}
		/*
		 * Check data type.
		 */
		fip = tif->tif_fieldinfo[fix];
		while (dp->tdir_type != (u_short) fip->field_type) {
			if (fip->field_type == TIFF_ANY)	/* wildcard */
				break;
			fip++, fix++;
			if (fix == tif->tif_nfields ||
			    fip->field_tag != dp->tdir_tag) {
				TIFFWarning(tif->tif_name,
				   "wrong data type %d for \"%s\"; tag ignored",
				    dp->tdir_type, fip[-1].field_name);
				goto ignore;
			}
		}
		/*
		 * Check count if known in advance.
		 */
		if (fip->field_readcount != TIFF_VARIABLE) {
			uint32 expected = (fip->field_readcount == TIFF_SPP) ?
			    (uint32) td->td_samplesperpixel :
			    (uint32) fip->field_readcount;
			if (!CheckDirCount(tif, dp, expected))
				goto ignore;
		}

		switch (dp->tdir_tag) {
		case TIFFTAG_COMPRESSION:
			/*
			 * The 5.0 spec says the Compression tag has
			 * one value, while earlier specs say it has
			 * one value per sample.  Because of this, we
			 * accept the tag if one value is supplied.
			 */
			if (dp->tdir_count == 1) {
				v = TIFFExtractData(tif,
				    dp->tdir_type, dp->tdir_offset);
				if (!TIFFSetField(tif, dp->tdir_tag, (int)v))
					goto bad;
				break;
			}
			if (!TIFFFetchPerSampleShorts(tif, dp, &iv) ||
			    !TIFFSetField(tif, dp->tdir_tag, iv))
				goto bad;
			dp->tdir_tag = IGNORE;
			break;
		case TIFFTAG_STRIPOFFSETS:
		case TIFFTAG_STRIPBYTECOUNTS:
		case TIFFTAG_TILEOFFSETS:
		case TIFFTAG_TILEBYTECOUNTS:
			TIFFSetFieldBit(tif, fip->field_bit);
			break;
		case TIFFTAG_IMAGEWIDTH:
		case TIFFTAG_IMAGELENGTH:
		case TIFFTAG_IMAGEDEPTH:
		case TIFFTAG_TILELENGTH:
		case TIFFTAG_TILEWIDTH:
		case TIFFTAG_TILEDEPTH:
		case TIFFTAG_PLANARCONFIG:
		case TIFFTAG_ROWSPERSTRIP:
			if (!TIFFFetchNormalTag(tif, dp))
				goto bad;
			dp->tdir_tag = IGNORE;
			break;
		case TIFFTAG_EXTRASAMPLES:
			(void) TIFFFetchExtraSamples(tif, dp);
			dp->tdir_tag = IGNORE;
			break;
		}
	}

	/*
	 * Allocate directory structure and setup defaults.
	 */
	if (!TIFFFieldSet(tif, FIELD_IMAGEDIMENSIONS)) {
		MissingRequired(tif, "ImageLength");
		goto bad;
	}
	if (!TIFFFieldSet(tif, FIELD_PLANARCONFIG)) {
		MissingRequired(tif, "PlanarConfiguration");
		goto bad;
	}
	/* 
 	 * Setup appropriate structures (by strip or by tile)
	 */
	if (!TIFFFieldSet(tif, FIELD_TILEDIMENSIONS)) {
		td->td_nstrips = TIFFNumberOfStrips(tif);
		td->td_tilewidth = td->td_imagewidth;
		td->td_tilelength = td->td_rowsperstrip;
		td->td_tiledepth = td->td_imagedepth;
		tif->tif_flags &= ~TIFF_ISTILED;
	} else {
		td->td_nstrips = TIFFNumberOfTiles(tif);
		tif->tif_flags |= TIFF_ISTILED;
	}
	td->td_stripsperimage = td->td_nstrips;
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
		td->td_stripsperimage /= td->td_samplesperpixel;
	if (!TIFFFieldSet(tif, FIELD_STRIPOFFSETS)) {
		MissingRequired(tif,
		    isTiled(tif) ? "TileOffsets" : "StripOffsets");
		goto bad;
	}

	/*
	 * Second pass: extract other information.
	 */
	for (dp = dir, n = dircount; n > 0; n--, dp++) {
		if (dp->tdir_tag == IGNORE)
			continue;
		switch (dp->tdir_tag) {
		case TIFFTAG_MINSAMPLEVALUE:
		case TIFFTAG_MAXSAMPLEVALUE:
		case TIFFTAG_BITSPERSAMPLE:
			/*
			 * The 5.0 spec says the Compression tag has
			 * one value, while earlier specs say it has
			 * one value per sample.  Because of this, we
			 * accept the tag if one value is supplied.
			 *
			 * The MinSampleValue, MaxSampleValue and
			 * BitsPerSample tags are supposed to be written
			 * as one value/sample, but some vendors incorrectly
			 * write one value only -- so we accept that
			 * as well (yech).
			 */
			if (dp->tdir_count == 1) {
				v = TIFFExtractData(tif,
				    dp->tdir_type, dp->tdir_offset);
				if (!TIFFSetField(tif, dp->tdir_tag, (int)v))
					goto bad;
				break;
			}
			/* fall thru... */
		case TIFFTAG_DATATYPE:
		case TIFFTAG_SAMPLEFORMAT:
			if (!TIFFFetchPerSampleShorts(tif, dp, &iv) ||
			    !TIFFSetField(tif, dp->tdir_tag, iv))
				goto bad;
			break;
		case TIFFTAG_SMINSAMPLEVALUE:
		case TIFFTAG_SMAXSAMPLEVALUE:
			if (!TIFFFetchPerSampleAnys(tif, dp, &dv) ||
			    !TIFFSetField(tif, dp->tdir_tag, dv))
				goto bad;
			break;
		case TIFFTAG_STRIPOFFSETS:
		case TIFFTAG_TILEOFFSETS:
			if (!TIFFFetchStripThing(tif, dp,
			    td->td_nstrips, &td->td_stripoffset))
				goto bad;
			break;
		case TIFFTAG_STRIPBYTECOUNTS:
		case TIFFTAG_TILEBYTECOUNTS:
			if (!TIFFFetchStripThing(tif, dp,
			    td->td_nstrips, &td->td_stripbytecount))
				goto bad;
			break;
		case TIFFTAG_COLORMAP:
		case TIFFTAG_TRANSFERFUNCTION:
			/*
			 * TransferFunction can have either 1x or 3x data
			 * values; Colormap can have only 3x items.
			 */
			v = 1L<<td->td_bitspersample;
			if (dp->tdir_tag == TIFFTAG_COLORMAP ||
			    dp->tdir_count != (uint32) v) {
				if (!CheckDirCount(tif, dp, (uint32)(3*v)))
					break;
			}
			v *= sizeof (uint16);
			cp = CheckMalloc(tif, dp->tdir_count * sizeof (uint16),
			    "to read \"TransferFunction\" tag");
			if (cp != NULL) {
				if (TIFFFetchData(tif, dp, cp)) {
					/*
					 * This deals with there being only
					 * one array to apply to all samples.
					 */
					uint32 c =
					    (uint32)1 << td->td_bitspersample;
					if (dp->tdir_count == c)
						v = 0;
					TIFFSetField(tif, dp->tdir_tag,
					    cp, cp+v, cp+2*v);
				}
				_TIFFfree(cp);
			}
			break;
		case TIFFTAG_PAGENUMBER:
		case TIFFTAG_HALFTONEHINTS:
		case TIFFTAG_YCBCRSUBSAMPLING:
		case TIFFTAG_DOTRANGE:
			(void) TIFFFetchShortPair(tif, dp);
			break;
#ifdef COLORIMETRY_SUPPORT
		case TIFFTAG_REFERENCEBLACKWHITE:
			(void) TIFFFetchRefBlackWhite(tif, dp);
			break;
#endif
/* BEGIN REV 4.0 COMPATIBILITY */
		case TIFFTAG_OSUBFILETYPE:
			v = 0;
			switch (TIFFExtractData(tif, dp->tdir_type,
			    dp->tdir_offset)) {
			case OFILETYPE_REDUCEDIMAGE:
				v = FILETYPE_REDUCEDIMAGE;
				break;
			case OFILETYPE_PAGE:
				v = FILETYPE_PAGE;
				break;
			}
			if (v)
				(void) TIFFSetField(tif,
				    TIFFTAG_SUBFILETYPE, (int)v);
			break;
/* END REV 4.0 COMPATIBILITY */
		default:
			(void) TIFFFetchNormalTag(tif, dp);
			break;
		}
	}
	/*
	 * Verify Palette image has a Colormap.
	 */
	if (td->td_photometric == PHOTOMETRIC_PALETTE &&
	    !TIFFFieldSet(tif, FIELD_COLORMAP)) {
		MissingRequired(tif, "Colormap");
		goto bad;
	}
	/*
	 * Attempt to deal with a missing StripByteCounts tag.
	 */
	if (!TIFFFieldSet(tif, FIELD_STRIPBYTECOUNTS)) {
		/*
		 * Some manufacturers violate the spec by not giving
		 * the size of the strips.  In this case, assume there
		 * is one uncompressed strip of data.
		 */
		if ((td->td_planarconfig == PLANARCONFIG_CONTIG &&
		    td->td_nstrips > 1) ||
		    (td->td_planarconfig == PLANARCONFIG_SEPARATE &&
		     td->td_nstrips != td->td_samplesperpixel)) {
		    MissingRequired(tif, "StripByteCounts");
		    goto bad;
		}
		TIFFWarning(tif->tif_name,
"TIFF directory is missing required \"%s\" field, calculating from imagelength",
		    _TIFFFieldWithTag(tif,TIFFTAG_STRIPBYTECOUNTS)->field_name);
		EstimateStripByteCounts(tif, dir, dircount);
#define	BYTECOUNTLOOKSBAD \
    (td->td_stripbytecount[0] == 0 || \
    (td->td_compression == COMPRESSION_NONE && \
     td->td_stripbytecount[0] > TIFFGetFileSize(tif) - td->td_stripoffset[0]))
	} else if (td->td_nstrips == 1 && BYTECOUNTLOOKSBAD) {
		/*
		 * Plexus (and others) sometimes give a value
		 * of zero for a tag when they don't know what
		 * the correct value is!  Try and handle the
		 * simple case of estimating the size of a one
		 * strip image.
		 */
		TIFFWarning(tif->tif_name,
	    "Bogus \"%s\" field, ignoring and calculating from imagelength",
		    _TIFFFieldWithTag(tif,TIFFTAG_STRIPBYTECOUNTS)->field_name);
		EstimateStripByteCounts(tif, dir, dircount);
	}
	if (dir)
		_TIFFfree((char *)dir);
	if (!TIFFFieldSet(tif, FIELD_MAXSAMPLEVALUE))
		td->td_maxsamplevalue = (uint16)((1L<<td->td_bitspersample)-1);
	/*
	 * Setup default compression scheme.
	 */
	if (!TIFFFieldSet(tif, FIELD_COMPRESSION))
		TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        /*
         * Some manufacturers make life difficult by writing
	 * large amounts of uncompressed data as a single strip.
	 * This is contrary to the recommendations of the spec.
         * The following makes an attempt at breaking such images
	 * into strips closer to the recommended 8k bytes.  A
	 * side effect, however, is that the RowsPerStrip tag
	 * value may be changed.
         */
	if (td->td_nstrips == 1 && td->td_compression == COMPRESSION_NONE &&
	    (tif->tif_flags & (TIFF_STRIPCHOP|TIFF_ISTILED)) == TIFF_STRIPCHOP)
		ChopUpSingleUncompressedStrip(tif);
	/*
	 * Reinitialize i/o since we are starting on a new directory.
	 */
	tif->tif_row = (uint32) -1;
	tif->tif_curstrip = (tstrip_t) -1;
	tif->tif_col = (uint32) -1;
	tif->tif_curtile = (ttile_t) -1;
	tif->tif_tilesize = TIFFTileSize(tif);
	tif->tif_scanlinesize = TIFFScanlineSize(tif);
	return (1);
bad:
	if (dir)
		_TIFFfree(dir);
	return (0);
}

static void
EstimateStripByteCounts(TIFF* tif, TIFFDirEntry* dir, uint16 dircount)
{
	register TIFFDirEntry *dp;
	register TIFFDirectory *td = &tif->tif_dir;
	uint16 i;

	if (td->td_stripbytecount)
		_TIFFfree(td->td_stripbytecount);
	td->td_stripbytecount = (uint32*)
	    CheckMalloc(tif, td->td_nstrips * sizeof (uint32),
		"for \"StripByteCounts\" array");
	if (td->td_compression != COMPRESSION_NONE) {
		uint32 space = (uint32)(sizeof (TIFFHeader)
		    + sizeof (uint16)
		    + (dircount * sizeof (TIFFDirEntry))
		    + sizeof (uint32));
		toff_t filesize = TIFFGetFileSize(tif);
		uint16 n;

		/* calculate amount of space used by indirect values */
		for (dp = dir, n = dircount; n > 0; n--, dp++) {
			uint32 cc = dp->tdir_count*tiffDataWidth[dp->tdir_type];
			if (cc > sizeof (uint32))
				space += cc;
		}
		space = filesize - space;
		if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
			space /= td->td_samplesperpixel;
		for (i = 0; i < td->td_nstrips; i++)
			td->td_stripbytecount[i] = space;
		/*
		 * This gross hack handles the case were the offset to
		 * the last strip is past the place where we think the strip
		 * should begin.  Since a strip of data must be contiguous,
		 * it's safe to assume that we've overestimated the amount
		 * of data in the strip and trim this number back accordingly.
		 */ 
		i--;
		if (((toff_t)(td->td_stripoffset[i]+td->td_stripbytecount[i]))
                                                               > filesize)
			td->td_stripbytecount[i] =
			    filesize - td->td_stripoffset[i];
	} else {
		uint32 rowbytes = TIFFScanlineSize(tif);
		uint32 rowsperstrip = td->td_imagelength/td->td_stripsperimage;
		for (i = 0; i < td->td_nstrips; i++)
			td->td_stripbytecount[i] = rowbytes*rowsperstrip;
	}
	TIFFSetFieldBit(tif, FIELD_STRIPBYTECOUNTS);
	if (!TIFFFieldSet(tif, FIELD_ROWSPERSTRIP))
		td->td_rowsperstrip = td->td_imagelength;
}

static void
MissingRequired(TIFF* tif, const char* tagname)
{
	TIFFError(tif->tif_name,
	    "TIFF directory is missing required \"%s\" field", tagname);
}

/*
 * Check the count field of a directory
 * entry against a known value.  The caller
 * is expected to skip/ignore the tag if
 * there is a mismatch.
 */
static int
CheckDirCount(TIFF* tif, TIFFDirEntry* dir, uint32 count)
{
	if (count != dir->tdir_count) {
		TIFFWarning(tif->tif_name,
	"incorrect count for field \"%s\" (%lu, expecting %lu); tag ignored",
		    _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name,
		    dir->tdir_count, count);
		return (0);
	}
	return (1);
}

/*
 * Fetch a contiguous directory item.
 */
static tsize_t
TIFFFetchData(TIFF* tif, TIFFDirEntry* dir, char* cp)
{
	int w = tiffDataWidth[dir->tdir_type];
	tsize_t cc = dir->tdir_count * w;

	if (!isMapped(tif)) {
		if (!SeekOK(tif, dir->tdir_offset))
			goto bad;
		if (!ReadOK(tif, cp, cc))
			goto bad;
	} else {
		if (((tsize_t) (dir->tdir_offset + cc)) > tif->tif_size)
			goto bad;
		_TIFFmemcpy(cp, tif->tif_base + dir->tdir_offset, cc);
	}
	if (tif->tif_flags & TIFF_SWAB) {
		switch (dir->tdir_type) {
		case TIFF_SHORT:
		case TIFF_SSHORT:
			TIFFSwabArrayOfShort((uint16*) cp, dir->tdir_count);
			break;
		case TIFF_LONG:
		case TIFF_SLONG:
		case TIFF_FLOAT:
			TIFFSwabArrayOfLong((uint32*) cp, dir->tdir_count);
			break;
		case TIFF_RATIONAL:
		case TIFF_SRATIONAL:
			TIFFSwabArrayOfLong((uint32*) cp, 2*dir->tdir_count);
			break;
		case TIFF_DOUBLE:
			TIFFSwabArrayOfDouble((double*) cp, dir->tdir_count);
			break;
		}
	}
	return (cc);
bad:
	TIFFError(tif->tif_name, "Error fetching data for field \"%s\"",
	    _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name);
	return ((tsize_t) 0);
}

/*
 * Fetch an ASCII item from the file.
 */
static tsize_t
TIFFFetchString(TIFF* tif, TIFFDirEntry* dir, char* cp)
{
	if (dir->tdir_count <= 4) {
		uint32 l = dir->tdir_offset;
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabLong(&l);
		_TIFFmemcpy(cp, &l, dir->tdir_count);
		return (1);
	}
	return (TIFFFetchData(tif, dir, cp));
}

/*
 * Convert numerator+denominator to float.
 */
static int
cvtRational(TIFF* tif, TIFFDirEntry* dir, uint32 num, uint32 denom, float* rv)
{
	if (denom == 0) {
		TIFFError(tif->tif_name,
		    "%s: Rational with zero denominator (num = %lu)",
		    _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name, num);
		return (0);
	} else {
		if (dir->tdir_type == TIFF_RATIONAL)
			*rv = ((float)num / (float)denom);
		else
			*rv = ((float)(int32)num / (float)(int32)denom);
		return (1);
	}
}

/*
 * Fetch a rational item from the file
 * at offset off and return the value
 * as a floating point number.
 */
static float
TIFFFetchRational(TIFF* tif, TIFFDirEntry* dir)
{
	uint32 l[2];
	float v;

	return (!TIFFFetchData(tif, dir, (char *)l) ||
	    !cvtRational(tif, dir, l[0], l[1], &v) ? 1.0f : v);
}

/*
 * Fetch a single floating point value
 * from the offset field and return it
 * as a native float.
 */
static float
TIFFFetchFloat(TIFF* tif, TIFFDirEntry* dir)
{
	long l = TIFFExtractData(tif, dir->tdir_type, dir->tdir_offset);
	float v = *(float*) &l;
	TIFFCvtIEEEFloatToNative(tif, 1, &v);
	return (v);
}

/*
 * Fetch an array of BYTE or SBYTE values.
 */
static int
TIFFFetchByteArray(TIFF* tif, TIFFDirEntry* dir, uint16* v)
{
    if (dir->tdir_count <= 4) {
        /*
         * Extract data from offset field.
         */
        if (tif->tif_header.tiff_magic == TIFF_BIGENDIAN) {
            switch (dir->tdir_count) {
                case 4: v[3] = (uint16)(dir->tdir_offset & 0xff);
                case 3: v[2] = (uint16)((dir->tdir_offset >> 8) & 0xff);
                case 2: v[1] = (uint16)((dir->tdir_offset >> 16) & 0xff);
                case 1: v[0] = (uint16)(dir->tdir_offset >> 24);
            }
        } else {
            switch (dir->tdir_count) {
                case 4: v[3] = (uint16)(dir->tdir_offset >> 24);
                case 3: v[2] = (uint16)((dir->tdir_offset >> 16) & 0xff);
                case 2: v[1] = (uint16)((dir->tdir_offset >> 8) & 0xff);
                case 1: v[0] = (uint16)(dir->tdir_offset & 0xff);
            }
        }
        return (1);
    } else
        return (TIFFFetchData(tif, dir, (char*) v) != 0);	/* XXX */
}

/*
 * Fetch an array of SHORT or SSHORT values.
 */
static int
TIFFFetchShortArray(TIFF* tif, TIFFDirEntry* dir, uint16* v)
{
	if (dir->tdir_count <= 2) {
		if (tif->tif_header.tiff_magic == TIFF_BIGENDIAN) {
			switch (dir->tdir_count) {
			case 2: v[1] = (uint16) (dir->tdir_offset & 0xffff);
			case 1: v[0] = (uint16) (dir->tdir_offset >> 16);
			}
		} else {
			switch (dir->tdir_count) {
			case 2: v[1] = (uint16) (dir->tdir_offset >> 16);
			case 1: v[0] = (uint16) (dir->tdir_offset & 0xffff);
			}
		}
		return (1);
	} else
		return (TIFFFetchData(tif, dir, (char *)v) != 0);
}

/*
 * Fetch a pair of SHORT or BYTE values.
 */
static int
TIFFFetchShortPair(TIFF* tif, TIFFDirEntry* dir)
{
	uint16 v[2];
	int ok = 0;

	switch (dir->tdir_type) {
	case TIFF_SHORT:
	case TIFF_SSHORT:
		ok = TIFFFetchShortArray(tif, dir, v);
		break;
	case TIFF_BYTE:
	case TIFF_SBYTE:
		ok  = TIFFFetchByteArray(tif, dir, v);
		break;
	}
	if (ok)
		TIFFSetField(tif, dir->tdir_tag, v[0], v[1]);
	return (ok);
}

/*
 * Fetch an array of LONG or SLONG values.
 */
static int
TIFFFetchLongArray(TIFF* tif, TIFFDirEntry* dir, uint32* v)
{
	if (dir->tdir_count == 1) {
		v[0] = dir->tdir_offset;
		return (1);
	} else
		return (TIFFFetchData(tif, dir, (char*) v) != 0);
}

/*
 * Fetch an array of RATIONAL or SRATIONAL values.
 */
static int
TIFFFetchRationalArray(TIFF* tif, TIFFDirEntry* dir, float* v)
{
	int ok = 0;
	uint32* l;

	l = (uint32*)CheckMalloc(tif,
	    dir->tdir_count*tiffDataWidth[dir->tdir_type],
	    "to fetch array of rationals");
	if (l) {
		if (TIFFFetchData(tif, dir, (char *)l)) {
			uint32 i;
			for (i = 0; i < dir->tdir_count; i++) {
				ok = cvtRational(tif, dir,
				    l[2*i+0], l[2*i+1], &v[i]);
				if (!ok)
					break;
			}
		}
		_TIFFfree((char *)l);
	}
	return (ok);
}

/*
 * Fetch an array of FLOAT values.
 */
static int
TIFFFetchFloatArray(TIFF* tif, TIFFDirEntry* dir, float* v)
{

	if (dir->tdir_count == 1) {
		v[0] = *(float*) &dir->tdir_offset;
		TIFFCvtIEEEFloatToNative(tif, dir->tdir_count, v);
		return (1);
	} else	if (TIFFFetchData(tif, dir, (char*) v)) {
		TIFFCvtIEEEFloatToNative(tif, dir->tdir_count, v);
		return (1);
	} else
		return (0);
}

/*
 * Fetch an array of DOUBLE values.
 */
static int
TIFFFetchDoubleArray(TIFF* tif, TIFFDirEntry* dir, double* v)
{
	if (TIFFFetchData(tif, dir, (char*) v)) {
		TIFFCvtIEEEDoubleToNative(tif, dir->tdir_count, v);
		return (1);
	} else
		return (0);
}

/*
 * Fetch an array of ANY values.  The actual values are
 * returned as doubles which should be able hold all the
 * types.  Yes, there really should be an tany_t to avoid
 * this potential non-portability ...  Note in particular
 * that we assume that the double return value vector is
 * large enough to read in any fundamental type.  We use
 * that vector as a buffer to read in the base type vector
 * and then convert it in place to double (from end
 * to front of course).
 */
static int
TIFFFetchAnyArray(TIFF* tif, TIFFDirEntry* dir, double* v)
{
	int i;

	switch (dir->tdir_type) {
	case TIFF_BYTE:
	case TIFF_SBYTE:
		if (!TIFFFetchByteArray(tif, dir, (uint16*) v))
			return (0);
		if (dir->tdir_type == TIFF_BYTE) {
			uint16* vp = (uint16*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		} else {
			int16* vp = (int16*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		}
		break;
	case TIFF_SHORT:
	case TIFF_SSHORT:
		if (!TIFFFetchShortArray(tif, dir, (uint16*) v))
			return (0);
		if (dir->tdir_type == TIFF_SHORT) {
			uint16* vp = (uint16*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		} else {
			int16* vp = (int16*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		}
		break;
	case TIFF_LONG:
	case TIFF_SLONG:
		if (!TIFFFetchLongArray(tif, dir, (uint32*) v))
			return (0);
		if (dir->tdir_type == TIFF_LONG) {
			uint32* vp = (uint32*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		} else {
			int32* vp = (int32*) v;
			for (i = dir->tdir_count-1; i >= 0; i--)
				v[i] = vp[i];
		}
		break;
	case TIFF_RATIONAL:
	case TIFF_SRATIONAL:
		if (!TIFFFetchRationalArray(tif, dir, (float*) v))
			return (0);
		{ float* vp = (float*) v;
		  for (i = dir->tdir_count-1; i >= 0; i--)
			v[i] = vp[i];
		}
		break;
	case TIFF_FLOAT:
		if (!TIFFFetchFloatArray(tif, dir, (float*) v))
			return (0);
		{ float* vp = (float*) v;
		  for (i = dir->tdir_count-1; i >= 0; i--)
			v[i] = vp[i];
		}
		break;
	case TIFF_DOUBLE:
		return (TIFFFetchDoubleArray(tif, dir, (double*) v));
	default:
		/* TIFF_NOTYPE */
		/* TIFF_ASCII */
		/* TIFF_UNDEFINED */
		TIFFError(tif->tif_name,
		    "Cannot read TIFF_ANY type %d for field \"%s\"",
		    _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name);
		return (0);
	}
	return (1);
}

/*
 * Fetch a tag that is not handled by special case code.
 */
static int
TIFFFetchNormalTag(TIFF* tif, TIFFDirEntry* dp)
{
	static const char mesg[] = "to fetch tag value";
	int ok = 0;
	const TIFFFieldInfo* fip = _TIFFFieldWithTag(tif, dp->tdir_tag);

	if (dp->tdir_count > 1) {		/* array of values */
		char* cp = NULL;

		switch (dp->tdir_type) {
		case TIFF_BYTE:
		case TIFF_SBYTE:
			/* NB: always expand BYTE values to shorts */
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (uint16), mesg);
			ok = cp && TIFFFetchByteArray(tif, dp, (uint16*) cp);
			break;
		case TIFF_SHORT:
		case TIFF_SSHORT:
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (uint16), mesg);
			ok = cp && TIFFFetchShortArray(tif, dp, (uint16*) cp);
			break;
		case TIFF_LONG:
		case TIFF_SLONG:
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (uint32), mesg);
			ok = cp && TIFFFetchLongArray(tif, dp, (uint32*) cp);
			break;
		case TIFF_RATIONAL:
		case TIFF_SRATIONAL:
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (float), mesg);
			ok = cp && TIFFFetchRationalArray(tif, dp, (float*) cp);
			break;
		case TIFF_FLOAT:
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (float), mesg);
			ok = cp && TIFFFetchFloatArray(tif, dp, (float*) cp);
			break;
		case TIFF_DOUBLE:
			cp = CheckMalloc(tif,
			    dp->tdir_count * sizeof (double), mesg);
			ok = cp && TIFFFetchDoubleArray(tif, dp, (double*) cp);
			break;
		case TIFF_ASCII:
		case TIFF_UNDEFINED:		/* bit of a cheat... */
			/*
			 * Some vendors write strings w/o the trailing
			 * NULL byte, so always append one just in case.
			 */
			cp = CheckMalloc(tif, dp->tdir_count+1, mesg);
			if( (ok = (cp && TIFFFetchString(tif, dp, cp))) != 0 )
				cp[dp->tdir_count] = '\0';	/* XXX */
			break;
		}
		if (ok) {
			ok = (fip->field_passcount ?
			    TIFFSetField(tif, dp->tdir_tag, dp->tdir_count, cp)
			  : TIFFSetField(tif, dp->tdir_tag, cp));
		}
		if (cp != NULL)
			_TIFFfree(cp);
	} else if (CheckDirCount(tif, dp, 1)) {	/* singleton value */
		switch (dp->tdir_type) {
		case TIFF_BYTE:
		case TIFF_SBYTE:
		case TIFF_SHORT:
		case TIFF_SSHORT:
			/*
			 * If the tag is also acceptable as a LONG or SLONG
			 * then TIFFSetField will expect an uint32 parameter
			 * passed to it (through varargs).  Thus, for machines
			 * where sizeof (int) != sizeof (uint32) we must do
			 * a careful check here.  It's hard to say if this
			 * is worth optimizing.
			 *
			 * NB: We use TIFFFieldWithTag here knowing that
			 *     it returns us the first entry in the table
			 *     for the tag and that that entry is for the
			 *     widest potential data type the tag may have.
			 */
			{ TIFFDataType type = fip->field_type;
			  if (type != TIFF_LONG && type != TIFF_SLONG) {
				uint16 v = (uint16)
			   TIFFExtractData(tif, dp->tdir_type, dp->tdir_offset);
				ok = (fip->field_passcount ?
				    TIFFSetField(tif, dp->tdir_tag, 1, &v)
				  : TIFFSetField(tif, dp->tdir_tag, v));
				break;
			  }
			}
			/* fall thru... */
		case TIFF_LONG:
		case TIFF_SLONG:
			{ uint32 v32 =
		    TIFFExtractData(tif, dp->tdir_type, dp->tdir_offset);
			  ok = (fip->field_passcount ? 
			      TIFFSetField(tif, dp->tdir_tag, 1, &v32)
			    : TIFFSetField(tif, dp->tdir_tag, v32));
			}
			break;
		case TIFF_RATIONAL:
		case TIFF_SRATIONAL:
		case TIFF_FLOAT:
			{ float v = (dp->tdir_type == TIFF_FLOAT ? 
			      TIFFFetchFloat(tif, dp)
			    : TIFFFetchRational(tif, dp));
			  ok = (fip->field_passcount ?
			      TIFFSetField(tif, dp->tdir_tag, 1, &v)
			    : TIFFSetField(tif, dp->tdir_tag, v));
			}
			break;
		case TIFF_DOUBLE:
			{ double v;
			  ok = (TIFFFetchDoubleArray(tif, dp, &v) &&
			    (fip->field_passcount ?
			      TIFFSetField(tif, dp->tdir_tag, 1, &v)
			    : TIFFSetField(tif, dp->tdir_tag, v))
			  );
			}
			break;
		case TIFF_ASCII:
		case TIFF_UNDEFINED:		/* bit of a cheat... */
			{ char c[2];
			  if( (ok = (TIFFFetchString(tif, dp, c) != 0)) != 0 ){
				c[1] = '\0';		/* XXX paranoid */
				ok = TIFFSetField(tif, dp->tdir_tag, c);
			  }
			}
			break;
		}
	}
	return (ok);
}

#define	NITEMS(x)	(sizeof (x) / sizeof (x[0]))
/*
 * Fetch samples/pixel short values for 
 * the specified tag and verify that
 * all values are the same.
 */
static int
TIFFFetchPerSampleShorts(TIFF* tif, TIFFDirEntry* dir, int* pl)
{
	int samples = tif->tif_dir.td_samplesperpixel;
	int status = 0;

	if (CheckDirCount(tif, dir, (uint32) samples)) {
		uint16 buf[10];
		uint16* v = buf;

		if (samples > NITEMS(buf))
			v = (uint16*) _TIFFmalloc(samples * sizeof (uint16));
		if (TIFFFetchShortArray(tif, dir, v)) {
			int i;
			for (i = 1; i < samples; i++)
				if (v[i] != v[0]) {
					TIFFError(tif->tif_name,
		"Cannot handle different per-sample values for field \"%s\"",
			   _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name);
					goto bad;
				}
			*pl = v[0];
			status = 1;
		}
	bad:
		if (v != buf)
			_TIFFfree((char*) v);
	}
	return (status);
}

/*
 * Fetch samples/pixel ANY values for 
 * the specified tag and verify that
 * all values are the same.
 */
static int
TIFFFetchPerSampleAnys(TIFF* tif, TIFFDirEntry* dir, double* pl)
{
	int samples = (int) tif->tif_dir.td_samplesperpixel;
	int status = 0;

	if (CheckDirCount(tif, dir, (uint32) samples)) {
		double buf[10];
		double* v = buf;

		if (samples > NITEMS(buf))
			v = (double*) _TIFFmalloc(samples * sizeof (double));
		if (TIFFFetchAnyArray(tif, dir, v)) {
			int i;
			for (i = 1; i < samples; i++)
				if (v[i] != v[0]) {
					TIFFError(tif->tif_name,
		"Cannot handle different per-sample values for field \"%s\"",
			   _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name);
					goto bad;
				}
			*pl = v[0];
			status = 1;
		}
	bad:
		if (v != buf)
			_TIFFfree(v);
	}
	return (status);
}
#undef NITEMS

/*
 * Fetch a set of offsets or lengths.
 * While this routine says "strips",
 * in fact it's also used for tiles.
 */
static int
TIFFFetchStripThing(TIFF* tif, TIFFDirEntry* dir, long nstrips, uint32** lpp)
{
	register uint32* lp;
	int status;

	if (!CheckDirCount(tif, dir, (uint32) nstrips))
		return (0);
	/*
	 * Allocate space for strip information.
	 */
	if (*lpp == NULL &&
	    (*lpp = (uint32 *)CheckMalloc(tif,
	      nstrips * sizeof (uint32), "for strip array")) == NULL)
		return (0);
	lp = *lpp;
	if (dir->tdir_type == (int)TIFF_SHORT) {
		/*
		 * Handle uint16->uint32 expansion.
		 */
		uint16* dp = (uint16*) CheckMalloc(tif,
		    dir->tdir_count* sizeof (uint16), "to fetch strip tag");
		if (dp == NULL)
			return (0);
		if( (status = TIFFFetchShortArray(tif, dir, dp)) != 0 ) {
			register uint16* wp = dp;
			while (nstrips-- > 0)
				*lp++ = *wp++;
		}
		_TIFFfree((char*) dp);
	} else
		status = TIFFFetchLongArray(tif, dir, lp);
	return (status);
}

#define	NITEMS(x)	(sizeof (x) / sizeof (x[0]))
/*
 * Fetch and set the ExtraSamples tag.
 */
static int
TIFFFetchExtraSamples(TIFF* tif, TIFFDirEntry* dir)
{
	uint16 buf[10];
	uint16* v = buf;
	int status;

	if (dir->tdir_count > NITEMS(buf))
		v = (uint16*) _TIFFmalloc(dir->tdir_count * sizeof (uint16));
	if (dir->tdir_type == TIFF_BYTE)
		status = TIFFFetchByteArray(tif, dir, v);
	else
		status = TIFFFetchShortArray(tif, dir, v);
	if (status)
		status = TIFFSetField(tif, dir->tdir_tag, dir->tdir_count, v);
	if (v != buf)
		_TIFFfree((char*) v);
	return (status);
}
#undef NITEMS

#ifdef COLORIMETRY_SUPPORT
/*
 * Fetch and set the RefBlackWhite tag.
 */
static int
TIFFFetchRefBlackWhite(TIFF* tif, TIFFDirEntry* dir)
{
	static const char mesg[] = "for \"ReferenceBlackWhite\" array";
	char* cp;
	int ok;

	if (dir->tdir_type == TIFF_RATIONAL)
		return (TIFFFetchNormalTag(tif, dir));
	/*
	 * Handle LONG's for backward compatibility.
	 */
	cp = CheckMalloc(tif, dir->tdir_count * sizeof (uint32), mesg);
	if( (ok = (cp && TIFFFetchLongArray(tif, dir, (uint32*) cp))) != 0) {
		float* fp = (float*)
		    CheckMalloc(tif, dir->tdir_count * sizeof (float), mesg);
		if( (ok = (fp != NULL)) != 0 ) {
			uint32 i;
			for (i = 0; i < dir->tdir_count; i++)
				fp[i] = (float)((uint32*) cp)[i];
			ok = TIFFSetField(tif, dir->tdir_tag, fp);
			_TIFFfree((char*) fp);
		}
	}
	if (cp)
		_TIFFfree(cp);
	return (ok);
}
#endif

/*
 * Replace a single strip (tile) of uncompressed data by
 * multiple strips (tiles), each approximately 8Kbytes.
 * This is useful for dealing with large images or
 * for dealing with machines with a limited amount
 * memory.
 */
static void
ChopUpSingleUncompressedStrip(TIFF* tif)
{
	register TIFFDirectory *td = &tif->tif_dir;
	uint32 bytecount = td->td_stripbytecount[0];
	uint32 offset = td->td_stripoffset[0];
	tsize_t rowbytes = TIFFVTileSize(tif, 1), stripbytes;
	tstrip_t strip, nstrips, rowsperstrip;
	uint32* newcounts;
	uint32* newoffsets;

	/*
	 * Make the rows hold at least one
	 * scanline, but fill 8k if possible.
	 */
	if (rowbytes > 8192) {
		stripbytes = rowbytes;
		rowsperstrip = 1;
	} else {
		rowsperstrip = 8192 / rowbytes;
		stripbytes = rowbytes * rowsperstrip;
	}
	/* never increase the number of strips in an image */
	if (rowsperstrip >= td->td_rowsperstrip)
		return;
	nstrips = (tstrip_t) TIFFhowmany(bytecount, stripbytes);
	newcounts = (uint32*) CheckMalloc(tif, nstrips * sizeof (uint32),
				"for chopped \"StripByteCounts\" array");
	newoffsets = (uint32*) CheckMalloc(tif, nstrips * sizeof (uint32),
				"for chopped \"StripOffsets\" array");
	if (newcounts == NULL || newoffsets == NULL) {
	        /*
		 * Unable to allocate new strip information, give
		 * up and use the original one strip information.
		 */
		if (newcounts != NULL)
			_TIFFfree(newcounts);
		if (newoffsets != NULL)
			_TIFFfree(newoffsets);
		return;
	}
	/*
	 * Fill the strip information arrays with
	 * new bytecounts and offsets that reflect
	 * the broken-up format.
	 */
	for (strip = 0; strip < nstrips; strip++) {
		if (stripbytes > (tsize_t) bytecount)
			stripbytes = bytecount;
		newcounts[strip] = stripbytes;
		newoffsets[strip] = offset;
		offset += stripbytes;
		bytecount -= stripbytes;
	}
	/*
	 * Replace old single strip info with multi-strip info.
	 */
	td->td_stripsperimage = td->td_nstrips = nstrips;
	TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rowsperstrip);

	_TIFFfree(td->td_stripbytecount);
	_TIFFfree(td->td_stripoffset);
	td->td_stripbytecount = newcounts;
	td->td_stripoffset = newoffsets;
}


/* ==== tif_dirwrite.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_dirwrite.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Directory Write Support Routines.
 */
#include "tiffiop.h"

#if HAVE_IEEEFP
#define	TIFFCvtNativeToIEEEFloat(tif, n, fp)
#define	TIFFCvtNativeToIEEEDouble(tif, n, dp)
#else
extern	void TIFFCvtNativeToIEEEFloat(TIFF*, uint32, float*);
extern	void TIFFCvtNativeToIEEEDouble(TIFF*, uint32, double*);
#endif

static	int TIFFWriteNormalTag(TIFF*, TIFFDirEntry*, const TIFFFieldInfo*);
static	void TIFFSetupShortLong(TIFF*, ttag_t, TIFFDirEntry*, uint32);
static	int TIFFSetupShortPair(TIFF*, ttag_t, TIFFDirEntry*);
static	int TIFFWritePerSampleShorts(TIFF*, ttag_t, TIFFDirEntry*);
static	int TIFFWritePerSampleAnys(TIFF*, TIFFDataType, ttag_t, TIFFDirEntry*);
static	int TIFFWriteShortTable(TIFF*, ttag_t, TIFFDirEntry*, uint32, uint16**);
static	int TIFFWriteShortArray(TIFF*,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, uint16*);
static	int TIFFWriteLongArray(TIFF *,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, uint32*);
static	int TIFFWriteRationalArray(TIFF *,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, float*);
static	int TIFFWriteFloatArray(TIFF *,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, float*);
static	int TIFFWriteDoubleArray(TIFF *,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, double*);
static	int TIFFWriteByteArray(TIFF*, TIFFDirEntry*, char*);
static	int TIFFWriteAnyArray(TIFF*,
	    TIFFDataType, ttag_t, TIFFDirEntry*, uint32, double*);
#ifdef COLORIMETRY_SUPPORT
static	int TIFFWriteTransferFunction(TIFF*, TIFFDirEntry*);
#endif
#ifdef CMYK_SUPPORT
static	int TIFFWriteInkNames(TIFF*, TIFFDirEntry*);
#endif
static	int TIFFWriteData(TIFF*, TIFFDirEntry*, char*);
static	int TIFFLinkDirectory(TIFF*);

#define	WriteRationalPair(type, tag1, v1, tag2, v2) {		\
	if (!TIFFWriteRational(tif, type, tag1, dir, v1))	\
		goto bad;					\
	if (!TIFFWriteRational(tif, type, tag2, dir+1, v2))	\
		goto bad;					\
	dir++;							\
}
#define	TIFFWriteRational(tif, type, tag, dir, v) \
	TIFFWriteRationalArray((tif), (type), (tag), (dir), 1, &(v))
#ifndef TIFFWriteRational
static	int TIFFWriteRational(TIFF*,
	    TIFFDataType, ttag_t, TIFFDirEntry*, float);
#endif

/*
 * Write the contents of the current directory
 * to the specified file.  This routine doesn't
 * handle overwriting a directory with auxiliary
 * storage that's been changed.
 */
int
TIFFWriteDirectory(TIFF* tif)
{
	uint16 dircount;
	uint32 diroff;
	ttag_t tag;
	uint32 nfields;
	tsize_t dirsize;
	char* data;
	TIFFDirEntry* dir;
	TIFFDirectory* td;
	u_long b, fields[FIELD_SETLONGS];
	int fi, nfi;

	if (tif->tif_mode == O_RDONLY)
		return (1);
	/*
	 * Clear write state so that subsequent images with
	 * different characteristics get the right buffers
	 * setup for them.
	 */
	if (tif->tif_flags & TIFF_POSTENCODE) {
		tif->tif_flags &= ~TIFF_POSTENCODE;
		if (!(*tif->tif_postencode)(tif)) {
			TIFFError(tif->tif_name,
			    "Error post-encoding before directory write");
			return (0);
		}
	}
	(*tif->tif_close)(tif);			/* shutdown encoder */
	/*
	 * Flush any data that might have been written
	 * by the compression close+cleanup routines.
	 */
	if (tif->tif_rawcc > 0 && !TIFFFlushData1(tif)) {
		TIFFError(tif->tif_name,
		    "Error flushing data before directory write");
		return (0);
	}
	if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata) {
		_TIFFfree(tif->tif_rawdata);
		tif->tif_rawdata = NULL;
		tif->tif_rawcc = 0;
	}
	tif->tif_flags &= ~(TIFF_BEENWRITING|TIFF_BUFFERSETUP);

	td = &tif->tif_dir;
	/*
	 * Size the directory so that we can calculate
	 * offsets for the data items that aren't kept
	 * in-place in each field.
	 */
	nfields = 0;
	for (b = 0; b <= FIELD_LAST; b++)
		if (TIFFFieldSet(tif, b))
			nfields += (b < FIELD_SUBFILETYPE ? 2 : 1);
	dirsize = nfields * sizeof (TIFFDirEntry);
	data = (char*) _TIFFmalloc(dirsize);
	if (data == NULL) {
		TIFFError(tif->tif_name,
		    "Cannot write directory, out of space");
		return (0);
	}
	/*
	 * Directory hasn't been placed yet, put
	 * it at the end of the file and link it
	 * into the existing directory structure.
	 */
	if (tif->tif_diroff == 0 && !TIFFLinkDirectory(tif))
		goto bad;
	tif->tif_dataoff = (toff_t)(
	    tif->tif_diroff + sizeof (uint16) + dirsize + sizeof (toff_t));
	if (tif->tif_dataoff & 1)
		tif->tif_dataoff++;
	(void) TIFFSeekFile(tif, tif->tif_dataoff, SEEK_SET);
	tif->tif_curdir++;
	dir = (TIFFDirEntry*) data;
	/*
	 * Setup external form of directory
	 * entries and write data items.
	 */
	_TIFFmemcpy(fields, td->td_fieldsset, sizeof (fields));
	/*
	 * Write out ExtraSamples tag only if
	 * extra samples are present in the data.
	 */
	if (FieldSet(fields, FIELD_EXTRASAMPLES) && !td->td_extrasamples) {
		ResetFieldBit(fields, FIELD_EXTRASAMPLES);
		nfields--;
		dirsize -= sizeof (TIFFDirEntry);
	}								/*XXX*/
	for (fi = 0, nfi = tif->tif_nfields; nfi > 0; nfi--, fi++) {
		const TIFFFieldInfo* fip = tif->tif_fieldinfo[fi];
		if (!FieldSet(fields, fip->field_bit))
			continue;
		switch (fip->field_bit) {
		case FIELD_STRIPOFFSETS:
			/*
			 * We use one field bit for both strip and tile
			 * offsets, and so must be careful in selecting
			 * the appropriate field descriptor (so that tags
			 * are written in sorted order).
			 */
			tag = isTiled(tif) ?
			    TIFFTAG_TILEOFFSETS : TIFFTAG_STRIPOFFSETS;
			if (tag != fip->field_tag)
				continue;
			if (!TIFFWriteLongArray(tif, TIFF_LONG, tag, dir,
			    (uint32) td->td_nstrips, td->td_stripoffset))
				goto bad;
			break;
		case FIELD_STRIPBYTECOUNTS:
			/*
			 * We use one field bit for both strip and tile
			 * byte counts, and so must be careful in selecting
			 * the appropriate field descriptor (so that tags
			 * are written in sorted order).
			 */
			tag = isTiled(tif) ?
			    TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS;
			if (tag != fip->field_tag)
				continue;
			if (!TIFFWriteLongArray(tif, TIFF_LONG, tag, dir,
			    (uint32) td->td_nstrips, td->td_stripbytecount))
				goto bad;
			break;
		case FIELD_ROWSPERSTRIP:
			TIFFSetupShortLong(tif, TIFFTAG_ROWSPERSTRIP,
			    dir, td->td_rowsperstrip);
			break;
		case FIELD_COLORMAP:
			if (!TIFFWriteShortTable(tif, TIFFTAG_COLORMAP, dir,
			    3, td->td_colormap))
				goto bad;
			break;
		case FIELD_IMAGEDIMENSIONS:
			TIFFSetupShortLong(tif, TIFFTAG_IMAGEWIDTH,
			    dir++, td->td_imagewidth);
			TIFFSetupShortLong(tif, TIFFTAG_IMAGELENGTH,
			    dir, td->td_imagelength);
			break;
		case FIELD_TILEDIMENSIONS:
			TIFFSetupShortLong(tif, TIFFTAG_TILEWIDTH,
			    dir++, td->td_tilewidth);
			TIFFSetupShortLong(tif, TIFFTAG_TILELENGTH,
			    dir, td->td_tilelength);
			break;
		case FIELD_POSITION:
			WriteRationalPair(TIFF_RATIONAL,
			    TIFFTAG_XPOSITION, td->td_xposition,
			    TIFFTAG_YPOSITION, td->td_yposition);
			break;
		case FIELD_RESOLUTION:
			WriteRationalPair(TIFF_RATIONAL,
			    TIFFTAG_XRESOLUTION, td->td_xresolution,
			    TIFFTAG_YRESOLUTION, td->td_yresolution);
			break;
		case FIELD_BITSPERSAMPLE:
		case FIELD_MINSAMPLEVALUE:
		case FIELD_MAXSAMPLEVALUE:
		case FIELD_SAMPLEFORMAT:
			if (!TIFFWritePerSampleShorts(tif, fip->field_tag, dir))
				goto bad;
			break;
		case FIELD_SMINSAMPLEVALUE:
		case FIELD_SMAXSAMPLEVALUE:
			if (!TIFFWritePerSampleAnys(tif,
			    _TIFFSampleToTagType(tif), fip->field_tag, dir))
				goto bad;
			break;
		case FIELD_PAGENUMBER:
		case FIELD_HALFTONEHINTS:
#ifdef YCBCR_SUPPORT
		case FIELD_YCBCRSUBSAMPLING:
#endif
#ifdef CMYK_SUPPORT
		case FIELD_DOTRANGE:
#endif
			if (!TIFFSetupShortPair(tif, fip->field_tag, dir))
				goto bad;
			break;
#ifdef CMYK_SUPPORT
		case FIELD_INKNAMES:
			if (!TIFFWriteInkNames(tif, dir))
				goto bad;
			break;
#endif
#ifdef COLORIMETRY_SUPPORT
		case FIELD_TRANSFERFUNCTION:
			if (!TIFFWriteTransferFunction(tif, dir))
				goto bad;
			break;
#endif
#if SUBIFD_SUPPORT
		case FIELD_SUBIFD:
			if (!TIFFWriteNormalTag(tif, dir, fip))
				goto bad;
			/*
			 * Total hack: if this directory includes a SubIFD
			 * tag then force the next <n> directories to be
			 * written as ``sub directories'' of this one.  This
			 * is used to write things like thumbnails and
			 * image masks that one wants to keep out of the
			 * normal directory linkage access mechanism.
			 */
			if (dir->tdir_count > 0) {
				tif->tif_flags |= TIFF_INSUBIFD;
				tif->tif_nsubifd = (uint16) dir->tdir_count;
				if (dir->tdir_count > 1)
					tif->tif_subifdoff = dir->tdir_offset;
				else
					tif->tif_subifdoff = (uint32)(
					      tif->tif_diroff
					    + sizeof (uint16)
					    + ((char*)&dir->tdir_offset-data));
			}
			break;
#endif
		default:
			if (!TIFFWriteNormalTag(tif, dir, fip))
				goto bad;
			break;
		}
		dir++;
		ResetFieldBit(fields, fip->field_bit);
	}
	/*
	 * Write directory.
	 */
	dircount = (uint16) nfields;
	diroff = (uint32) tif->tif_nextdiroff;
	if (tif->tif_flags & TIFF_SWAB) {
		/*
		 * The file's byte order is opposite to the
		 * native machine architecture.  We overwrite
		 * the directory information with impunity
		 * because it'll be released below after we
		 * write it to the file.  Note that all the
		 * other tag construction routines assume that
		 * we do this byte-swapping; i.e. they only
		 * byte-swap indirect data.
		 */
		for (dir = (TIFFDirEntry*) data; dircount; dir++, dircount--) {
			TIFFSwabArrayOfShort(&dir->tdir_tag, 2);
			TIFFSwabArrayOfLong(&dir->tdir_count, 2);
		}
		dircount = (uint16) nfields;
		TIFFSwabShort(&dircount);
		TIFFSwabLong(&diroff);
	}
	(void) TIFFSeekFile(tif, tif->tif_diroff, SEEK_SET);
	if (!WriteOK(tif, &dircount, sizeof (dircount))) {
		TIFFError(tif->tif_name, "Error writing directory count");
		goto bad;
	}
	if (!WriteOK(tif, data, dirsize)) {
		TIFFError(tif->tif_name, "Error writing directory contents");
		goto bad;
	}
	if (!WriteOK(tif, &diroff, sizeof (diroff))) {
		TIFFError(tif->tif_name, "Error writing directory link");
		goto bad;
	}
	TIFFFreeDirectory(tif);
	_TIFFfree(data);
	tif->tif_flags &= ~TIFF_DIRTYDIRECT;
	(*tif->tif_cleanup)(tif);

	/*
	 * Reset directory-related state for subsequent
	 * directories.
	 */
	TIFFDefaultDirectory(tif);
	tif->tif_diroff = 0;
	tif->tif_curoff = 0;
	tif->tif_row = (uint32) -1;
	tif->tif_curstrip = (tstrip_t) -1;
	return (1);
bad:
	_TIFFfree(data);
	return (0);
}
#undef WriteRationalPair

/*
 * Process tags that are not special cased.
 */
static int
TIFFWriteNormalTag(TIFF* tif, TIFFDirEntry* dir, const TIFFFieldInfo* fip)
{
	u_short wc = (u_short) fip->field_writecount;
	uint32 wc2;

	dir->tdir_tag = (uint16) fip->field_tag;
	dir->tdir_type = (u_short) fip->field_type;
	dir->tdir_count = wc;
#define	WRITEF(x,y)	x(tif, fip->field_type, fip->field_tag, dir, wc, y)
	switch (fip->field_type) {
	case TIFF_SHORT:
	case TIFF_SSHORT:
		if (wc > 1) {
			uint16* wp;
			if (wc == (u_short) TIFF_VARIABLE)
				TIFFGetField(tif, fip->field_tag, &wc, &wp);
			else
				TIFFGetField(tif, fip->field_tag, &wp);
			if (!WRITEF(TIFFWriteShortArray, wp))
				return (0);
		} else {
			uint16 sv;
			TIFFGetField(tif, fip->field_tag, &sv);
			dir->tdir_offset =
			    TIFFInsertData(tif, dir->tdir_type, sv);
		}
		break;
	case TIFF_LONG:
	case TIFF_SLONG:
		if (wc > 1) {
			uint32* lp;
			if (wc == (u_short) TIFF_VARIABLE)
				TIFFGetField(tif, fip->field_tag, &wc, &lp);
			else
				TIFFGetField(tif, fip->field_tag, &lp);
			if (!WRITEF(TIFFWriteLongArray, lp))
				return (0);
		} else {
			/* XXX handle LONG->SHORT conversion */
			TIFFGetField(tif, fip->field_tag, &dir->tdir_offset);
		}
		break;
	case TIFF_RATIONAL:
	case TIFF_SRATIONAL:
		if (wc > 1) {
			float* fp;
			if (wc == (u_short) TIFF_VARIABLE)
				TIFFGetField(tif, fip->field_tag, &wc, &fp);
			else
				TIFFGetField(tif, fip->field_tag, &fp);
			if (!WRITEF(TIFFWriteRationalArray, fp))
				return (0);
		} else {
			float fv;
			TIFFGetField(tif, fip->field_tag, &fv);
			if (!WRITEF(TIFFWriteRationalArray, &fv))
				return (0);
		}
		break;
	case TIFF_FLOAT:
		if (wc > 1) {
			float* fp;
			if (wc == (u_short) TIFF_VARIABLE)
				TIFFGetField(tif, fip->field_tag, &wc, &fp);
			else
				TIFFGetField(tif, fip->field_tag, &fp);
			if (!WRITEF(TIFFWriteFloatArray, fp))
				return (0);
		} else {
			float fv;
			TIFFGetField(tif, fip->field_tag, &fv);
			if (!WRITEF(TIFFWriteFloatArray, &fv))
				return (0);
		}
		break;
	case TIFF_DOUBLE:
		if (wc > 1) {
			double* dp;
			if (wc == (u_short) TIFF_VARIABLE)
				TIFFGetField(tif, fip->field_tag, &wc, &dp);
			else
				TIFFGetField(tif, fip->field_tag, &dp);
			if (!WRITEF(TIFFWriteDoubleArray, dp))
				return (0);
		} else {
			double dv;
			TIFFGetField(tif, fip->field_tag, &dv);
			if (!WRITEF(TIFFWriteDoubleArray, &dv))
				return (0);
		}
		break;
	case TIFF_ASCII:
		{ char* cp;
		  TIFFGetField(tif, fip->field_tag, &cp);
		  dir->tdir_count = (uint32) (strlen(cp) + 1);
		  if (!TIFFWriteByteArray(tif, dir, cp))
			return (0);
		}
		break;

        /* added based on patch request from MARTIN.MCBRIDE.MM@agfa.co.uk,
           correctness not verified (FW, 99/08) */
        case TIFF_BYTE:
        case TIFF_SBYTE:          
                if (wc > 1) {
                    char* cp;
                    if (wc == (u_short) TIFF_VARIABLE) {
                        TIFFGetField(tif, fip->field_tag, &wc, &cp);
                        dir->tdir_count = wc;
                    } else
                        TIFFGetField(tif, fip->field_tag, &cp);
                    if (!TIFFWriteByteArray(tif, dir, cp))
                        return (0);
                } else {
                    char cv;
                    TIFFGetField(tif, fip->field_tag, &cv);
                    if (!TIFFWriteByteArray(tif, dir, &cv))
                        return (0);
                }
                break;

	case TIFF_UNDEFINED:
		{ char* cp;
		  if (wc == (u_short) TIFF_VARIABLE) {
			TIFFGetField(tif, fip->field_tag, &wc, &cp);
			dir->tdir_count = wc;
		  } else if (wc == (u_short) TIFF_VARIABLE2) {
			TIFFGetField(tif, fip->field_tag, &wc2, &cp);
			dir->tdir_count = wc2;
		  } else 
			TIFFGetField(tif, fip->field_tag, &cp);
		  if (!TIFFWriteByteArray(tif, dir, cp))
			return (0);
		}
		break;

        case TIFF_NOTYPE:
                break;
	}
	return (1);
}
#undef WRITEF

/*
 * Setup a directory entry with either a SHORT
 * or LONG type according to the value.
 */
static void
TIFFSetupShortLong(TIFF* tif, ttag_t tag, TIFFDirEntry* dir, uint32 v)
{
	dir->tdir_tag = (uint16) tag;
	dir->tdir_count = 1;
	if (v > 0xffffL) {
		dir->tdir_type = (short) TIFF_LONG;
		dir->tdir_offset = v;
	} else {
		dir->tdir_type = (short) TIFF_SHORT;
		dir->tdir_offset = TIFFInsertData(tif, (int) TIFF_SHORT, v);
	}
}
#undef MakeShortDirent

#ifndef TIFFWriteRational
/*
 * Setup a RATIONAL directory entry and
 * write the associated indirect value.
 */
static int
TIFFWriteRational(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, float v)
{
	return (TIFFWriteRationalArray(tif, type, tag, dir, 1, &v));
}
#endif

#define	NITEMS(x)	(sizeof (x) / sizeof (x[0]))
/*
 * Setup a directory entry that references a
 * samples/pixel array of SHORT values and
 * (potentially) write the associated indirect
 * values.
 */
static int
TIFFWritePerSampleShorts(TIFF* tif, ttag_t tag, TIFFDirEntry* dir)
{
	uint16 buf[10], v;
	uint16* w = buf;
	int i, status, samples = tif->tif_dir.td_samplesperpixel;

	if (samples > NITEMS(buf))
		w = (uint16*) _TIFFmalloc(samples * sizeof (uint16));
	TIFFGetField(tif, tag, &v);
	for (i = 0; i < samples; i++)
		w[i] = v;
	status = TIFFWriteShortArray(tif, TIFF_SHORT, tag, dir, samples, w);
	if (w != buf)
		_TIFFfree((char*) w);
	return (status);
}

/*
 * Setup a directory entry that references a samples/pixel array of ``type''
 * values and (potentially) write the associated indirect values.  The source
 * data from TIFFGetField() for the specified tag must be returned as double.
 */
static int
TIFFWritePerSampleAnys(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir)
{
	double buf[10], v;
	double* w = buf;
	int i, status;
	int samples = (int) tif->tif_dir.td_samplesperpixel;

	if (samples > NITEMS(buf))
		w = (double*) _TIFFmalloc(samples * sizeof (double));
	TIFFGetField(tif, tag, &v);
	for (i = 0; i < samples; i++)
		w[i] = v;
	status = TIFFWriteAnyArray(tif, type, tag, dir, samples, w);
	if (w != buf)
		_TIFFfree(w);
	return (status);
}
#undef NITEMS

/*
 * Setup a pair of shorts that are returned by
 * value, rather than as a reference to an array.
 */
static int
TIFFSetupShortPair(TIFF* tif, ttag_t tag, TIFFDirEntry* dir)
{
	uint16 v[2];

	TIFFGetField(tif, tag, &v[0], &v[1]);
	return (TIFFWriteShortArray(tif, TIFF_SHORT, tag, dir, 2, v));
}

/*
 * Setup a directory entry for an NxM table of shorts,
 * where M is known to be 2**bitspersample, and write
 * the associated indirect data.
 */
static int
TIFFWriteShortTable(TIFF* tif,
    ttag_t tag, TIFFDirEntry* dir, uint32 n, uint16** table)
{
	uint32 i, off;

	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) TIFF_SHORT;
	/* XXX -- yech, fool TIFFWriteData */
	dir->tdir_count = (uint32) (1L<<tif->tif_dir.td_bitspersample);
	off = tif->tif_dataoff;
	for (i = 0; i < n; i++)
		if (!TIFFWriteData(tif, dir, (char *)table[i]))
			return (0);
	dir->tdir_count *= n;
	dir->tdir_offset = off;
	return (1);
}

/*
 * Write/copy data associated with an ASCII or opaque tag value.
 */
static int
TIFFWriteByteArray(TIFF* tif, TIFFDirEntry* dir, char* cp)
{
	if (dir->tdir_count > 4) {
		if (!TIFFWriteData(tif, dir, cp))
			return (0);
	} else
		_TIFFmemcpy(&dir->tdir_offset, cp, dir->tdir_count);
	return (1);
}

/*
 * Setup a directory entry of an array of SHORT
 * or SSHORT and write the associated indirect values.
 */
static int
TIFFWriteShortArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, uint16* v)
{
	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) type;
	dir->tdir_count = n;
	if (n <= 2) {
		if (tif->tif_header.tiff_magic == TIFF_BIGENDIAN) {
			dir->tdir_offset = (uint32) ((long) v[0] << 16);
			if (n == 2)
				dir->tdir_offset |= v[1] & 0xffff;
		} else {
			dir->tdir_offset = v[0] & 0xffff;
			if (n == 2)
				dir->tdir_offset |= (long) v[1] << 16;
		}
		return (1);
	} else
		return (TIFFWriteData(tif, dir, (char*) v));
}

/*
 * Setup a directory entry of an array of LONG
 * or SLONG and write the associated indirect values.
 */
static int
TIFFWriteLongArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, uint32* v)
{
	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) type;
	dir->tdir_count = n;
	if (n == 1) {
		dir->tdir_offset = v[0];
		return (1);
	} else
		return (TIFFWriteData(tif, dir, (char*) v));
}

/*
 * Setup a directory entry of an array of RATIONAL
 * or SRATIONAL and write the associated indirect values.
 */
static int
TIFFWriteRationalArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, float* v)
{
	uint32 i;
	uint32* t;
	int status;

	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) type;
	dir->tdir_count = n;
	t = (uint32*) _TIFFmalloc(2*n * sizeof (uint32));
	for (i = 0; i < n; i++) {
		float fv = v[i];
		int sign = 1;
		uint32 den;

		if (fv < 0) {
			if (type == TIFF_RATIONAL) {
				TIFFWarning(tif->tif_name,
	"\"%s\": Information lost writing value (%g) as (unsigned) RATIONAL",
				_TIFFFieldWithTag(tif,tag)->field_name, fv);
				fv = 0;
			} else
				fv = -fv, sign = -1;
		}
		den = 1L;
		if (fv > 0) {
			while (fv < 1L<<(31-3) && den < 1L<<(31-3))
				fv *= 1<<3, den *= 1L<<3;
		}
		t[2*i+0] = (uint32) (sign * (fv + 0.5));
		t[2*i+1] = den;
	}
	status = TIFFWriteData(tif, dir, (char *)t);
	_TIFFfree((char*) t);
	return (status);
}

static int
TIFFWriteFloatArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, float* v)
{
	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) type;
	dir->tdir_count = n;
	TIFFCvtNativeToIEEEFloat(tif, n, v);
	if (n == 1) {
		dir->tdir_offset = *(uint32*) &v[0];
		return (1);
	} else
		return (TIFFWriteData(tif, dir, (char*) v));
}

static int
TIFFWriteDoubleArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, double* v)
{
	dir->tdir_tag = (uint16) tag;
	dir->tdir_type = (short) type;
	dir->tdir_count = n;
	TIFFCvtNativeToIEEEDouble(tif, n, v);
	return (TIFFWriteData(tif, dir, (char*) v));
}

/*
 * Write an array of ``type'' values for a specified tag (i.e. this is a tag
 * which is allowed to have different types, e.g. SMaxSampleType).
 * Internally the data values are represented as double since a double can
 * hold any of the TIFF tag types (yes, this should really be an abstract
 * type tany_t for portability).  The data is converted into the specified
 * type in a temporary buffer and then handed off to the appropriate array
 * writer.
 */
static int
TIFFWriteAnyArray(TIFF* tif,
    TIFFDataType type, ttag_t tag, TIFFDirEntry* dir, uint32 n, double* v)
{
	char buf[10 * sizeof(double)];
	char* w = buf;
	int i, status = 0;

	if (n * tiffDataWidth[type] > sizeof buf)
		w = (char*) _TIFFmalloc(n * tiffDataWidth[type]);
	switch (type) {
	case TIFF_BYTE:
		{ uint8* bp = (uint8*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (uint8) v[i];
		  dir->tdir_tag = (uint16) tag;
		  dir->tdir_type = (short) type;
		  dir->tdir_count = n;
		  if (!TIFFWriteByteArray(tif, dir, (char*) bp))
			goto out;
		}
		break;
	case TIFF_SBYTE:
		{ int8* bp = (int8*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (int8) v[i];
		  dir->tdir_tag = (uint16) tag;
		  dir->tdir_type = (short) type;
		  dir->tdir_count = n;
		  if (!TIFFWriteByteArray(tif, dir, (char*) bp))
			goto out;
		}
		break;
	case TIFF_SHORT:
		{ uint16* bp = (uint16*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (uint16) v[i];
		  if (!TIFFWriteShortArray(tif, type, tag, dir, n, (uint16*)bp))
				goto out;
		}
		break;
	case TIFF_SSHORT:
		{ int16* bp = (int16*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (int16) v[i];
		  if (!TIFFWriteShortArray(tif, type, tag, dir, n, (uint16*)bp))
			goto out;
		}
		break;
	case TIFF_LONG:
		{ uint32* bp = (uint32*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (uint32) v[i];
		  if (!TIFFWriteLongArray(tif, type, tag, dir, n, bp))
			goto out;
		}
		break;
	case TIFF_SLONG:
		{ int32* bp = (int32*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (int32) v[i];
		  if (!TIFFWriteLongArray(tif, type, tag, dir, n, (uint32*) bp))
			goto out;
		}
		break;
	case TIFF_FLOAT:
		{ float* bp = (float*) w;
		  for (i = 0; i < (int) n; i++)
			bp[i] = (float) v[i];
		  if (!TIFFWriteFloatArray(tif, type, tag, dir, n, bp))
			goto out;
		}
		break;
	case TIFF_DOUBLE:
		return (TIFFWriteDoubleArray(tif, type, tag, dir, n, v));
	default:
		/* TIFF_NOTYPE */
		/* TIFF_ASCII */
		/* TIFF_UNDEFINED */
		/* TIFF_RATIONAL */
		/* TIFF_SRATIONAL */
		goto out;
	}
	status = 1;
 out:
	if (w != buf)
		_TIFFfree(w);
	return (status);
}

#ifdef COLORIMETRY_SUPPORT
static int
TIFFWriteTransferFunction(TIFF* tif, TIFFDirEntry* dir)
{
	TIFFDirectory* td = &tif->tif_dir;
	tsize_t n = (1L<<td->td_bitspersample) * sizeof (uint16);
	uint16** tf = td->td_transferfunction;
	int ncols;

	/*
	 * Check if the table can be written as a single column,
	 * or if it must be written as 3 columns.  Note that we
	 * write a 3-column tag if there are 2 samples/pixel and
	 * a single column of data won't suffice--hmm.
	 */
	switch (td->td_samplesperpixel - td->td_extrasamples) {
	default:	if (_TIFFmemcmp(tf[0], tf[2], n)) { ncols = 3; break; }
	case 2:		if (_TIFFmemcmp(tf[0], tf[1], n)) { ncols = 3; break; }
	case 1: case 0:	ncols = 1;
	}
	return (TIFFWriteShortTable(tif,
	    TIFFTAG_TRANSFERFUNCTION, dir, ncols, tf));
}
#endif

#ifdef CMYK_SUPPORT
static int
TIFFWriteInkNames(TIFF* tif, TIFFDirEntry* dir)
{
	TIFFDirectory* td = &tif->tif_dir;

	dir->tdir_tag = TIFFTAG_INKNAMES;
	dir->tdir_type = (short) TIFF_ASCII;
	dir->tdir_count = td->td_inknameslen;
	return (TIFFWriteByteArray(tif, dir, td->td_inknames));
}
#endif

/*
 * Write a contiguous directory item.
 */
static int
TIFFWriteData(TIFF* tif, TIFFDirEntry* dir, char* cp)
{
	tsize_t cc;

	if (tif->tif_flags & TIFF_SWAB) {
		switch (dir->tdir_type) {
		case TIFF_SHORT:
		case TIFF_SSHORT:
			TIFFSwabArrayOfShort((uint16*) cp, dir->tdir_count);
			break;
		case TIFF_LONG:
		case TIFF_SLONG:
		case TIFF_FLOAT:
			TIFFSwabArrayOfLong((uint32*) cp, dir->tdir_count);
			break;
		case TIFF_RATIONAL:
		case TIFF_SRATIONAL:
			TIFFSwabArrayOfLong((uint32*) cp, 2*dir->tdir_count);
			break;
		case TIFF_DOUBLE:
			TIFFSwabArrayOfDouble((double*) cp, dir->tdir_count);
			break;
		}
	}
	dir->tdir_offset = tif->tif_dataoff;
	cc = dir->tdir_count * tiffDataWidth[dir->tdir_type];
	if (SeekOK(tif, dir->tdir_offset) &&
	    WriteOK(tif, cp, cc)) {
		tif->tif_dataoff += (cc + 1) & ~1;
		return (1);
	}
	TIFFError(tif->tif_name, "Error writing data for field \"%s\"",
	    _TIFFFieldWithTag(tif, dir->tdir_tag)->field_name);
	return (0);
}

/*
 * Link the current directory into the
 * directory chain for the file.
 */
static int
TIFFLinkDirectory(TIFF* tif)
{
	static const char module[] = "TIFFLinkDirectory";
	uint32 nextdir;
	uint32 diroff;

	tif->tif_diroff = (TIFFSeekFile(tif, (toff_t) 0, SEEK_END)+1) &~ 1;
	diroff = (uint32) tif->tif_diroff;
	if (tif->tif_flags & TIFF_SWAB)
		TIFFSwabLong(&diroff);
#if SUBIFD_SUPPORT
	if (tif->tif_flags & TIFF_INSUBIFD) {
		(void) TIFFSeekFile(tif, tif->tif_subifdoff, SEEK_SET);
		if (!WriteOK(tif, &diroff, sizeof (diroff))) {
			TIFFError(module,
			    "%s: Error writing SubIFD directory link",
			    tif->tif_name);
			return (0);
		}
		/*
		 * Advance to the next SubIFD or, if this is
		 * the last one configured, revert back to the
		 * normal directory linkage.
		 */
		if (--tif->tif_nsubifd)
			tif->tif_subifdoff += sizeof (diroff);
		else
			tif->tif_flags &= ~TIFF_INSUBIFD;
		return (1);
	}
#endif
	if (tif->tif_header.tiff_diroff == 0) {
		/*
		 * First directory, overwrite offset in header.
		 */
		tif->tif_header.tiff_diroff = (uint32) tif->tif_diroff;
#define	HDROFF(f)	((toff_t) &(((TIFFHeader*) 0)->f))
		(void) TIFFSeekFile(tif, HDROFF(tiff_diroff), SEEK_SET);
		if (!WriteOK(tif, &diroff, sizeof (diroff))) {
			TIFFError(tif->tif_name, "Error writing TIFF header");
			return (0);
		}
		return (1);
	}
	/*
	 * Not the first directory, search to the last and append.
	 */
	nextdir = tif->tif_header.tiff_diroff;
	do {
		uint16 dircount;

		if (!SeekOK(tif, nextdir) ||
		    !ReadOK(tif, &dircount, sizeof (dircount))) {
			TIFFError(module, "Error fetching directory count");
			return (0);
		}
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabShort(&dircount);
		(void) TIFFSeekFile(tif,
		    dircount * sizeof (TIFFDirEntry), SEEK_CUR);
		if (!ReadOK(tif, &nextdir, sizeof (nextdir))) {
			TIFFError(module, "Error fetching directory link");
			return (0);
		}
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabLong(&nextdir);
	} while (nextdir != 0);
	(void) TIFFSeekFile(tif, -(toff_t) sizeof (nextdir), SEEK_CUR);
	if (!WriteOK(tif, &diroff, sizeof (diroff))) {
		TIFFError(module, "Error writing directory link");
		return (0);
	}
	return (1);
}


/* ==== tif_dumpmode.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_dumpmode.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * "Null" Compression Algorithm Support.
 */
#include "tiffiop.h"

/*
 * Encode a hunk of pixels.
 */
static int
DumpModeEncode(TIFF* tif, tidata_t pp, tsize_t cc, tsample_t s)
{
	(void) s;
	while (cc > 0) {
		tsize_t n;

		n = cc;
		if (tif->tif_rawcc + n > tif->tif_rawdatasize)
			n = tif->tif_rawdatasize - tif->tif_rawcc;
		/*
		 * Avoid copy if client has setup raw
		 * data buffer to avoid extra copy.
		 */
		if (tif->tif_rawcp != pp)
			_TIFFmemcpy(tif->tif_rawcp, pp, n);
		tif->tif_rawcp += n;
		tif->tif_rawcc += n;
		pp += n;
		cc -= n;
		if (tif->tif_rawcc >= tif->tif_rawdatasize &&
		    !TIFFFlushData1(tif))
			return (-1);
	}
	return (1);
}

/*
 * Decode a hunk of pixels.
 */
static int
DumpModeDecode(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	(void) s;
	if (tif->tif_rawcc < cc) {
		TIFFError(tif->tif_name,
		    "DumpModeDecode: Not enough data for scanline %d",
		    tif->tif_row);
		return (0);
	}
	/*
	 * Avoid copy if client has setup raw
	 * data buffer to avoid extra copy.
	 */
	if (tif->tif_rawcp != buf)
		_TIFFmemcpy(buf, tif->tif_rawcp, cc);
	tif->tif_rawcp += cc;
	tif->tif_rawcc -= cc;
	return (1);
}

/*
 * Seek forwards nrows in the current strip.
 */
static int
DumpModeSeek(TIFF* tif, uint32 nrows)
{
	tif->tif_rawcp += nrows * tif->tif_scanlinesize;
	tif->tif_rawcc -= nrows * tif->tif_scanlinesize;
	return (1);
}

/*
 * Initialize dump mode.
 */
int
TIFFInitDumpMode(TIFF* tif, int scheme)
{
	(void) scheme;
	tif->tif_decoderow = DumpModeDecode;
	tif->tif_decodestrip = DumpModeDecode;
	tif->tif_decodetile = DumpModeDecode;
	tif->tif_encoderow = DumpModeEncode;
	tif->tif_encodestrip = DumpModeEncode;
	tif->tif_encodetile = DumpModeEncode;
	tif->tif_seek = DumpModeSeek;
	return (1);
}


/* ==== tif_error.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_error.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 */
#include "tiffiop.h"

TIFFErrorHandler
TIFFSetErrorHandler(TIFFErrorHandler handler)
{
	TIFFErrorHandler prev = _TIFFerrorHandler;
	_TIFFerrorHandler = handler;
	return (prev);
}

void
TIFFError(const char* module, const char* fmt, ...)
{
	if (_TIFFerrorHandler) {
		va_list ap;
		va_start(ap, fmt);
		(*_TIFFerrorHandler)(module, fmt, ap);
		va_end(ap);
	}
}


/* ==== tif_fax3.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_fax3.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1990-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef CCITT_SUPPORT
/*
 * TIFF Library.
 *
 * CCITT Group 3 (T.4) and Group 4 (T.6) Compression Support.
 *
 * This file contains support for decoding and encoding TIFF
 * compression algorithms 2, 3, 4, and 32771.
 *
 * Decoder support is derived, with permission, from the code
 * in Frank Cringle's viewfax program;
 *      Copyright (C) 1990, 1995  Frank D. Cringle.
 */
#include "tif_fax3.h"
#define	G3CODES
#include "t4.h"
#include <assert.h>
#include <stdio.h>

/*
 * NB: define PURIFY if you're using purify and you want
 * to avoid some harmless array bounds complaints that
 * can happen in the _TIFFFax3fillruns routine.
 */

/*
 * Compression+decompression state blocks are
 * derived from this ``base state'' block.
 */
typedef struct {
	int	mode;			/* operating mode */
	uint32	rowbytes;		/* bytes in a decoded scanline */
	uint32	rowpixels;		/* pixels in a scanline */

	uint16	cleanfaxdata;		/* CleanFaxData tag */
	uint32	badfaxrun;		/* BadFaxRun tag */
	uint32	badfaxlines;		/* BadFaxLines tag */
	uint32	groupoptions;		/* Group 3/4 options tag */
	uint32	recvparams;		/* encoded Class 2 session params */
	char*	subaddress;		/* subaddress string */
	uint32	recvtime;		/* time spent receiving (secs) */
	TIFFVGetMethod vgetparent;	/* super-class method */
	TIFFVSetMethod vsetparent;	/* super-class method */
} Fax3BaseState;
#define	Fax3State(tif)		((Fax3BaseState*) (tif)->tif_data)

typedef struct {
	Fax3BaseState b;
	const u_char* bitmap;		/* bit reversal table */
	uint32	data;			/* current i/o byte/word */
	int	bit;			/* current i/o bit in byte */
	int	EOLcnt;			/* count of EOL codes recognized */
	TIFFFaxFillFunc fill;		/* fill routine */
	uint32*	runs;			/* b&w runs for current/previous row */
	uint32*	refruns;		/* runs for reference line */
	uint32*	curruns;		/* runs for current line */
} Fax3DecodeState;
#define	DecoderState(tif)	((Fax3DecodeState*) Fax3State(tif))

typedef enum { G3_1D, G3_2D } Ttag;
typedef struct {
	Fax3BaseState b;
	int	data;			/* current i/o byte */
	int	bit;			/* current i/o bit in byte */
	Ttag    tag;	                /* encoding state */
	u_char*	refline;		/* reference line for 2d decoding */
	int	k;			/* #rows left that can be 2d encoded */
	int	maxk;			/* max #rows that can be 2d encoded */
} Fax3EncodeState;
#define	EncoderState(tif)	((Fax3EncodeState*) Fax3State(tif))

#define	is2DEncoding(sp) \
	(sp->b.groupoptions & GROUP3OPT_2DENCODING)
#define	isAligned(p,t)	((((u_long)(p)) & (sizeof (t)-1)) == 0)

/*
 * Group 3 and Group 4 Decoding.
 */

/*
 * These macros glue the TIFF library state to
 * the state expected by Frank's decoder.
 */
#define	DECLARE_STATE(tif, sp, mod)					\
    static const char module[] = mod;					\
    Fax3DecodeState* sp = DecoderState(tif);				\
    int a0;				/* reference element */		\
    int lastx = sp->b.rowpixels;	/* last element in row */	\
    uint32 BitAcc;			/* bit accumulator */		\
    int BitsAvail;			/* # valid bits in BitAcc */	\
    int RunLength;			/* length of current run */	\
    u_char* cp;				/* next byte of input data */	\
    u_char* ep;				/* end of input data */		\
    uint32* pa;				/* place to stuff next run */	\
    uint32* thisrun;			/* current row's run array */	\
    int EOLcnt;				/* # EOL codes recognized */	\
    const u_char* bitmap = sp->bitmap;	/* input data bit reverser */	\
    const TIFFFaxTabEnt* TabEnt
#define	DECLARE_STATE_2D(tif, sp, mod)					\
    DECLARE_STATE(tif, sp, mod);					\
    int b1;				/* next change on prev line */	\
    uint32* pb				/* next run in reference line */\
/*
 * Load any state that may be changed during decoding.
 */
#define	CACHE_STATE(tif, sp) do {					\
    BitAcc = sp->data;							\
    BitsAvail = sp->bit;						\
    EOLcnt = sp->EOLcnt;						\
    cp = (unsigned char*) tif->tif_rawcp;				\
    ep = cp + tif->tif_rawcc;						\
} while (0)
/*
 * Save state possibly changed during decoding.
 */
#define	UNCACHE_STATE(tif, sp) do {					\
    sp->bit = BitsAvail;						\
    sp->data = BitAcc;							\
    sp->EOLcnt = EOLcnt;						\
    tif->tif_rawcc -= (tidata_t) cp - tif->tif_rawcp;			\
    tif->tif_rawcp = (tidata_t) cp;					\
} while (0)

/*
 * Setup state for decoding a strip.
 */
static int
Fax3PreDecode(TIFF* tif, tsample_t s)
{
	Fax3DecodeState* sp = DecoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->bit = 0;			/* force initial read */
	sp->data = 0;
	sp->EOLcnt = 0;			/* force initial scan for EOL */
	/*
	 * Decoder assumes lsb-to-msb bit order.  Note that we select
	 * this here rather than in Fax3SetupState so that viewers can
	 * hold the image open, fiddle with the FillOrder tag value,
	 * and then re-decode the image.  Otherwise they'd need to close
	 * and open the image to get the state reset.
	 */
	sp->bitmap =
	    TIFFGetBitRevTable(tif->tif_dir.td_fillorder != FILLORDER_LSB2MSB);
	if (sp->refruns) {		/* init reference line to white */
		sp->refruns[0] = (uint16) sp->b.rowpixels;
		sp->refruns[1] = 0;
	}
	return (1);
}

/*
 * Routine for handling various errors/conditions.
 * Note how they are "glued into the decoder" by
 * overriding the definitions used by the decoder.
 */

static void
Fax3Unexpected(const char* module, TIFF* tif, uint32 a0)
{
	TIFFError(module, "%s: Bad code word at scanline %d (x %lu)",
	    tif->tif_name, tif->tif_row, (u_long) a0);
}
#define	unexpected(table, a0)	Fax3Unexpected(module, tif, a0)

static void
Fax3Extension(const char* module, TIFF* tif, uint32 a0)
{
	TIFFError(module,
	    "%s: Uncompressed data (not supported) at scanline %d (x %lu)",
	    tif->tif_name, tif->tif_row, (u_long) a0);
}
#define	extension(a0)	Fax3Extension(module, tif, a0)

static void
Fax3BadLength(const char* module, TIFF* tif, uint32 a0, uint32 lastx)
{
	TIFFWarning(module, "%s: %s at scanline %d (got %lu, expected %lu)",
	    tif->tif_name,
	    a0 < lastx ? "Premature EOL" : "Line length mismatch",
	    tif->tif_row, (u_long) a0, (u_long) lastx);
}
#define	badlength(a0,lastx)	Fax3BadLength(module, tif, a0, lastx)

static void
Fax3PrematureEOF(const char* module, TIFF* tif, uint32 a0)
{
	TIFFWarning(module, "%s: Premature EOF at scanline %d (x %lu)",
	    tif->tif_name, tif->tif_row, (u_long) a0);
}
#define	prematureEOF(a0)	Fax3PrematureEOF(module, tif, a0)

#define	Nop

/*
 * Decode the requested amount of G3 1D-encoded data.
 */
static int
Fax3Decode1D(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	DECLARE_STATE(tif, sp, "Fax3Decode1D");

	(void) s;
	CACHE_STATE(tif, sp);
	thisrun = sp->curruns;
	while ((long)occ > 0) {
		a0 = 0;
		RunLength = 0;
		pa = thisrun;
#ifdef FAX3_DEBUG
		printf("\nBitAcc=%08X, BitsAvail = %d\n", BitAcc, BitsAvail);
		printf("-------------------- %d\n", tif->tif_row);
		fflush(stdout);
#endif
		SYNC_EOL(EOF1D);
		EXPAND1D(EOF1Da);
		(*sp->fill)(buf, thisrun, pa, lastx);
		buf += sp->b.rowbytes;
		occ -= sp->b.rowbytes;
		if (occ != 0)
			tif->tif_row++;
		continue;
	EOF1D:				/* premature EOF */
		CLEANUP_RUNS();
	EOF1Da:				/* premature EOF */
		(*sp->fill)(buf, thisrun, pa, lastx);
		UNCACHE_STATE(tif, sp);
		return (-1);
	}
	UNCACHE_STATE(tif, sp);
	return (1);
}

#define	SWAP(t,a,b)	{ t x; x = (a); (a) = (b); (b) = x; }
/*
 * Decode the requested amount of G3 2D-encoded data.
 */
static int
Fax3Decode2D(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	DECLARE_STATE_2D(tif, sp, "Fax3Decode2D");
	int is1D;			/* current line is 1d/2d-encoded */

	(void) s;
	CACHE_STATE(tif, sp);
	while ((long)occ > 0) {
		a0 = 0;
		RunLength = 0;
		pa = thisrun = sp->curruns;
#ifdef FAX3_DEBUG
		printf("\nBitAcc=%08X, BitsAvail = %d EOLcnt = %d",
		    BitAcc, BitsAvail, EOLcnt);
#endif
		SYNC_EOL(EOF2D);
		NeedBits8(1, EOF2D);
		is1D = GetBits(1);	/* 1D/2D-encoding tag bit */
		ClrBits(1);
#ifdef FAX3_DEBUG
		printf(" %s\n-------------------- %d\n",
		    is1D ? "1D" : "2D", tif->tif_row);
		fflush(stdout);
#endif
		pb = sp->refruns;
		b1 = *pb++;
		if (is1D)
			EXPAND1D(EOF2Da);
		else
			EXPAND2D(EOF2Da);
		(*sp->fill)(buf, thisrun, pa, lastx);
		SETVAL(0);		/* imaginary change for reference */
		SWAP(uint32*, sp->curruns, sp->refruns);
		buf += sp->b.rowbytes;
		occ -= sp->b.rowbytes;
		if (occ != 0)
			tif->tif_row++;
		continue;
	EOF2D:				/* premature EOF */
		CLEANUP_RUNS();
	EOF2Da:				/* premature EOF */
		(*sp->fill)(buf, thisrun, pa, lastx);
		UNCACHE_STATE(tif, sp);
		return (-1);
	}
	UNCACHE_STATE(tif, sp);
	return (1);
}
#undef SWAP

/*
 * The ZERO & FILL macros must handle spans < 2*sizeof(long) bytes.
 * For machines with 64-bit longs this is <16 bytes; otherwise
 * this is <8 bytes.  We optimize the code here to reflect the
 * machine characteristics.
 */
#if defined(__alpha) || _MIPS_SZLONG == 64
#define FILL(n, cp)							    \
    switch (n) {							    \
    case 15:(cp)[14] = 0xff; case 14:(cp)[13] = 0xff; case 13: (cp)[12] = 0xff;\
    case 12:(cp)[11] = 0xff; case 11:(cp)[10] = 0xff; case 10: (cp)[9] = 0xff;\
    case  9: (cp)[8] = 0xff; case  8: (cp)[7] = 0xff; case  7: (cp)[6] = 0xff;\
    case  6: (cp)[5] = 0xff; case  5: (cp)[4] = 0xff; case  4: (cp)[3] = 0xff;\
    case  3: (cp)[2] = 0xff; case  2: (cp)[1] = 0xff;			      \
    case  1: (cp)[0] = 0xff; (cp) += (n); case 0:  ;			      \
    }
#define ZERO(n, cp)							\
    switch (n) {							\
    case 15:(cp)[14] = 0; case 14:(cp)[13] = 0; case 13: (cp)[12] = 0;	\
    case 12:(cp)[11] = 0; case 11:(cp)[10] = 0; case 10: (cp)[9] = 0;	\
    case  9: (cp)[8] = 0; case  8: (cp)[7] = 0; case  7: (cp)[6] = 0;	\
    case  6: (cp)[5] = 0; case  5: (cp)[4] = 0; case  4: (cp)[3] = 0;	\
    case  3: (cp)[2] = 0; case  2: (cp)[1] = 0;			      	\
    case  1: (cp)[0] = 0; (cp) += (n); case 0:  ;			\
    }
#else
#define FILL(n, cp)							    \
    switch (n) {							    \
    case 7: (cp)[6] = 0xff; case 6: (cp)[5] = 0xff; case 5: (cp)[4] = 0xff; \
    case 4: (cp)[3] = 0xff; case 3: (cp)[2] = 0xff; case 2: (cp)[1] = 0xff; \
    case 1: (cp)[0] = 0xff; (cp) += (n); case 0:  ;			    \
    }
#define ZERO(n, cp)							\
    switch (n) {							\
    case 7: (cp)[6] = 0; case 6: (cp)[5] = 0; case 5: (cp)[4] = 0;	\
    case 4: (cp)[3] = 0; case 3: (cp)[2] = 0; case 2: (cp)[1] = 0;	\
    case 1: (cp)[0] = 0; (cp) += (n); case 0:  ;			\
    }
#endif

/*
 * Bit-fill a row according to the white/black
 * runs generated during G3/G4 decoding.
 */
void
_TIFFFax3fillruns(u_char* buf, uint32* runs, uint32* erun, uint32 lastx)
{
	static const unsigned char _fillmasks[] =
	    { 0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff };
	u_char* cp;
	uint32 x, bx, run;
	int32 n, nw;
	long* lp;

	if ((erun-runs)&1)
	    *erun++ = 0;
	x = 0;
	for (; runs < erun; runs += 2) {
	    run = runs[0];
	    if (x+run > lastx)
		run = runs[0] = (uint16) (lastx - x);
	    if (run) {
		cp = buf + (x>>3);
		bx = x&7;
		if (run > 8-bx) {
		    if (bx) {			/* align to byte boundary */
			*cp++ &= 0xff << (8-bx);
			run -= 8-bx;
		    }
		    if( (n = run >> 3) != 0 ) {	/* multiple bytes to fill */
			if ((n/sizeof (long)) > 1) {
			    /*
			     * Align to longword boundary and fill.
			     */
			    for (; n && !isAligned(cp, long); n--)
				    *cp++ = 0x00;
			    lp = (long*) cp;
			    nw = (int32)(n / sizeof (long));
			    n -= nw * sizeof (long);
			    do {
				    *lp++ = 0L;
			    } while (--nw);
			    cp = (u_char*) lp;
			}
			ZERO(n, cp);
			run &= 7;
		    }
#ifdef PURIFY
		    if (run)
			cp[0] &= 0xff >> run;
#else
		    cp[0] &= 0xff >> run;
#endif
		} else
		    cp[0] &= ~(_fillmasks[run]>>bx);
		x += runs[0];
	    }
	    run = runs[1];
	    if (x+run > lastx)
		run = runs[1] = lastx - x;
	    if (run) {
		cp = buf + (x>>3);
		bx = x&7;
		if (run > 8-bx) {
		    if (bx) {			/* align to byte boundary */
			*cp++ |= 0xff >> bx;
			run -= 8-bx;
		    }
		    if( (n = run>>3) != 0 ) {	/* multiple bytes to fill */
			if ((n/sizeof (long)) > 1) {
			    /*
			     * Align to longword boundary and fill.
			     */
			    for (; n && !isAligned(cp, long); n--)
				*cp++ = 0xff;
			    lp = (long*) cp;
			    nw = (int32)(n / sizeof (long));
			    n -= nw * sizeof (long);
			    do {
				*lp++ = -1L;
			    } while (--nw);
			    cp = (u_char*) lp;
			}
			FILL(n, cp);
			run &= 7;
		    }
#ifdef PURIFY
		    if (run)
			cp[0] |= 0xff00 >> run;
#else
		    cp[0] |= 0xff00 >> run;
#endif
		} else
		    cp[0] |= _fillmasks[run]>>bx;
		x += runs[1];
	    }
	}
	assert(x == lastx);
}
#undef	ZERO
#undef	FILL

/*
 * Setup G3/G4-related compression/decompression state
 * before data is processed.  This routine is called once
 * per image -- it sets up different state based on whether
 * or not decoding or encoding is being done and whether
 * 1D- or 2D-encoded data is involved.
 */
static int
Fax3SetupState(TIFF* tif)
{
	TIFFDirectory* td = &tif->tif_dir;
	Fax3BaseState* sp = Fax3State(tif);
	long rowbytes, rowpixels;
	int needsRefLine;

	if (td->td_bitspersample != 1) {
		TIFFError(tif->tif_name,
		    "Bits/sample must be 1 for Group 3/4 encoding/decoding");
		return (0);
	}
	/*
	 * Calculate the scanline/tile widths.
	 */
	if (isTiled(tif)) {
		rowbytes = TIFFTileRowSize(tif);
		rowpixels = td->td_tilewidth;
	} else {
		rowbytes = TIFFScanlineSize(tif);
		rowpixels = td->td_imagewidth;
	}
	sp->rowbytes = (uint32) rowbytes;
	sp->rowpixels = (uint32) rowpixels;
	/*
	 * Allocate any additional space required for decoding/encoding.
	 */
	needsRefLine = (
	    (sp->groupoptions & GROUP3OPT_2DENCODING) ||
	    td->td_compression == COMPRESSION_CCITTFAX4
	);
	if (tif->tif_mode == O_RDONLY) {	/* 1d/2d decoding */
		Fax3DecodeState* dsp = DecoderState(tif);
		uint32 nruns = needsRefLine ?
		     2*TIFFroundup(rowpixels,32) : rowpixels;

		dsp->runs = (uint32*) _TIFFmalloc(nruns*sizeof (uint32));
		if (dsp->runs == NULL) {
			TIFFError("Fax3SetupState",
			    "%s: No space for Group 3/4 run arrays",
			    tif->tif_name);
			return (0);
		}
		dsp->curruns = dsp->runs;
		if (needsRefLine)
			dsp->refruns = dsp->runs + (nruns>>1);
		else
			dsp->refruns = NULL;
		if (is2DEncoding(dsp)) {	/* NB: default is 1D routine */
			tif->tif_decoderow = Fax3Decode2D;
			tif->tif_decodestrip = Fax3Decode2D;
			tif->tif_decodetile = Fax3Decode2D;
		}
	} else if (needsRefLine) {		/* 2d encoding */
		Fax3EncodeState* esp = EncoderState(tif);
		/*
		 * 2d encoding requires a scanline
		 * buffer for the ``reference line''; the
		 * scanline against which delta encoding
		 * is referenced.  The reference line must
		 * be initialized to be ``white'' (done elsewhere).
		 */
		esp->refline = (u_char*) _TIFFmalloc(rowbytes);
		if (esp->refline == NULL) {
			TIFFError("Fax3SetupState",
			    "%s: No space for Group 3/4 reference line",
			    tif->tif_name);
			return (0);
		}
	} else					/* 1d encoding */
		EncoderState(tif)->refline = NULL;
	return (1);
}

/*
 * CCITT Group 3 FAX Encoding.
 */

#define	Fax3FlushBits(tif, sp) {				\
	if ((tif)->tif_rawcc >= (tif)->tif_rawdatasize)		\
		(void) TIFFFlushData1(tif);			\
	*(tif)->tif_rawcp++ = (sp)->data;			\
	(tif)->tif_rawcc++;					\
	(sp)->data = 0, (sp)->bit = 8;				\
}
#define	_FlushBits(tif) {					\
	if ((tif)->tif_rawcc >= (tif)->tif_rawdatasize)		\
		(void) TIFFFlushData1(tif);			\
	*(tif)->tif_rawcp++ = data;				\
	(tif)->tif_rawcc++;					\
	data = 0, bit = 8;					\
}
static const int _msbmask[9] =
    { 0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff };
#define	_PutBits(tif, bits, length) {				\
	while (length > bit) {					\
		data |= bits >> (length - bit);			\
		length -= bit;					\
		_FlushBits(tif);				\
	}							\
	data |= (bits & _msbmask[length]) << (bit - length);	\
	bit -= length;						\
	if (bit == 0)						\
		_FlushBits(tif);				\
}
	
/*
 * Write a variable-length bit-value to
 * the output stream.  Values are
 * assumed to be at most 16 bits.
 */
static void
Fax3PutBits(TIFF* tif, u_int bits, u_int length)
{
	Fax3EncodeState* sp = EncoderState(tif);
	u_int bit = sp->bit;
	int data = sp->data;

	_PutBits(tif, bits, length);

	sp->data = data;
	sp->bit = bit;
}

/*
 * Write a code to the output stream.
 */
#define putcode(tif, te)	Fax3PutBits(tif, (te)->code, (te)->length)

#ifdef FAX3_DEBUG
#define	DEBUG_COLOR(w) (tab == TIFFFaxWhiteCodes ? w "W" : w "B")
#define	DEBUG_PRINT(what,len) {						\
    int t;								\
    printf("%08X/%-2d: %s%5d\t", data, bit, DEBUG_COLOR(what), len);	\
    for (t = length-1; t >= 0; t--)					\
	putchar(code & (1<<t) ? '1' : '0');				\
    putchar('\n');							\
}
#endif

/*
 * Write the sequence of codes that describes
 * the specified span of zero's or one's.  The
 * appropriate table that holds the make-up and
 * terminating codes is supplied.
 */
static void
putspan(TIFF* tif, int32 span, const tableentry* tab)
{
	Fax3EncodeState* sp = EncoderState(tif);
	u_int bit = sp->bit;
	int data = sp->data;
	u_int code, length;

	while (span >= 2624) {
		const tableentry* te = &tab[63 + (2560>>6)];
		code = te->code, length = te->length;
#ifdef FAX3_DEBUG
		DEBUG_PRINT("MakeUp", te->runlen);
#endif
		_PutBits(tif, code, length);
		span -= te->runlen;
	}
	if (span >= 64) {
		const tableentry* te = &tab[63 + (span>>6)];
		assert(te->runlen == 64*(span>>6));
		code = te->code, length = te->length;
#ifdef FAX3_DEBUG
		DEBUG_PRINT("MakeUp", te->runlen);
#endif
		_PutBits(tif, code, length);
		span -= te->runlen;
	}
	code = tab[span].code, length = tab[span].length;
#ifdef FAX3_DEBUG
	DEBUG_PRINT("  Term", tab[span].runlen);
#endif
	_PutBits(tif, code, length);

	sp->data = data;
	sp->bit = bit;
}

/*
 * Write an EOL code to the output stream.  The zero-fill
 * logic for byte-aligning encoded scanlines is handled
 * here.  We also handle writing the tag bit for the next
 * scanline when doing 2d encoding.
 */
static void
Fax3PutEOL(TIFF* tif)
{
	Fax3EncodeState* sp = EncoderState(tif);
	u_int bit = sp->bit;
	int data = sp->data;
	u_int code, length, tparm;

	if (sp->b.groupoptions & GROUP3OPT_FILLBITS) {
		/*
		 * Force bit alignment so EOL will terminate on
		 * a byte boundary.  That is, force the bit alignment
		 * to 16-12 = 4 before putting out the EOL code.
		 */
		int align = 8 - 4;
		if (align != sp->bit) {
			if (align > sp->bit)
				align = sp->bit + (8 - align);
			else
				align = sp->bit - align;
			code = 0;
			tparm=align; 
			_PutBits(tif, 0, tparm);
		}
	}
	code = EOL, length = 12;
	if (is2DEncoding(sp))
		code = (code<<1) | (sp->tag == G3_1D), length++;
	_PutBits(tif, code, length);

	sp->data = data;
	sp->bit = bit;
}

/*
 * Reset encoding state at the start of a strip.
 */
static int
Fax3PreEncode(TIFF* tif, tsample_t s)
{
	Fax3EncodeState* sp = EncoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->bit = 8;
	sp->data = 0;
	sp->tag = G3_1D;
	/*
	 * This is necessary for Group 4; otherwise it isn't
	 * needed because the first scanline of each strip ends
	 * up being copied into the refline.
	 */
	if (sp->refline)
		_TIFFmemset(sp->refline, 0x00, sp->b.rowbytes);
	if (is2DEncoding(sp)) {
		float res = tif->tif_dir.td_yresolution;
		/*
		 * The CCITT spec says that when doing 2d encoding, you
		 * should only do it on K consecutive scanlines, where K
		 * depends on the resolution of the image being encoded
		 * (2 for <= 200 lpi, 4 for > 200 lpi).  Since the directory
		 * code initializes td_yresolution to 0, this code will
		 * select a K of 2 unless the YResolution tag is set
		 * appropriately.  (Note also that we fudge a little here
		 * and use 150 lpi to avoid problems with units conversion.)
		 */
		if (tif->tif_dir.td_resolutionunit == RESUNIT_CENTIMETER)
			res *= 2.54f;		/* convert to inches */
		sp->maxk = (res > 150 ? 4 : 2);
		sp->k = sp->maxk-1;
	} else
		sp->k = sp->maxk = 0;
	return (1);
}

static const u_char zeroruns[256] = {
    8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,	/* 0x00 - 0x0f */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,	/* 0x10 - 0x1f */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,	/* 0x20 - 0x2f */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,	/* 0x30 - 0x3f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x40 - 0x4f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x50 - 0x5f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x60 - 0x6f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x70 - 0x7f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x80 - 0x8f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x90 - 0x9f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xa0 - 0xaf */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xb0 - 0xbf */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xc0 - 0xcf */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xd0 - 0xdf */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xe0 - 0xef */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0xf0 - 0xff */
};
static const u_char oneruns[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x00 - 0x0f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x10 - 0x1f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x20 - 0x2f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x30 - 0x3f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x40 - 0x4f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x50 - 0x5f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x60 - 0x6f */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 0x70 - 0x7f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x80 - 0x8f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0x90 - 0x9f */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0xa0 - 0xaf */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	/* 0xb0 - 0xbf */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,	/* 0xc0 - 0xcf */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,	/* 0xd0 - 0xdf */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,	/* 0xe0 - 0xef */
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8,	/* 0xf0 - 0xff */
};

/*
 * On certain systems it pays to inline
 * the routines that find pixel spans.
 */
#ifdef VAXC
static	int32 find0span(u_char*, int32, int32);
static	int32 find1span(u_char*, int32, int32);
#pragma inline(find0span,find1span)
#endif

/*
 * Find a span of ones or zeros using the supplied
 * table.  The ``base'' of the bit string is supplied
 * along with the start+end bit indices.
 */
INLINE static int32
find0span(u_char* bp, int32 bs, int32 be)
{
	int32 bits = be - bs;
	int32 n, span;

	bp += bs>>3;
	/*
	 * Check partial byte on lhs.
	 */
	if (bits > 0 && (n = (bs & 7))) {
		span = zeroruns[(*bp << n) & 0xff];
		if (span > 8-n)		/* table value too generous */
			span = 8-n;
		if (span > bits)	/* constrain span to bit range */
			span = bits;
		if (n+span < 8)		/* doesn't extend to edge of byte */
			return (span);
		bits -= span;
		bp++;
	} else
		span = 0;
	if (bits >= 2*8*sizeof (long)) {
		long* lp;
		/*
		 * Align to longword boundary and check longwords.
		 */
		while (!isAligned(bp, long)) {
			if (*bp != 0x00)
				return (span + zeroruns[*bp]);
			span += 8, bits -= 8;
			bp++;
		}
		lp = (long*) bp;
		while (bits >= 8*sizeof (long) && *lp == 0) {
			span += 8*sizeof (long), bits -= 8*sizeof (long);
			lp++;
		}
		bp = (u_char*) lp;
	}
	/*
	 * Scan full bytes for all 0's.
	 */
	while (bits >= 8) {
		if (*bp != 0x00)	/* end of run */
			return (span + zeroruns[*bp]);
		span += 8, bits -= 8;
		bp++;
	}
	/*
	 * Check partial byte on rhs.
	 */
	if (bits > 0) {
		n = zeroruns[*bp];
		span += (n > bits ? bits : n);
	}
	return (span);
}

INLINE static int32
find1span(u_char* bp, int32 bs, int32 be)
{
	int32 bits = be - bs;
	int32 n, span;

	bp += bs>>3;
	/*
	 * Check partial byte on lhs.
	 */
	if (bits > 0 && (n = (bs & 7))) {
		span = oneruns[(*bp << n) & 0xff];
		if (span > 8-n)		/* table value too generous */
			span = 8-n;
		if (span > bits)	/* constrain span to bit range */
			span = bits;
		if (n+span < 8)		/* doesn't extend to edge of byte */
			return (span);
		bits -= span;
		bp++;
	} else
		span = 0;
	if (bits >= 2*8*sizeof (long)) {
		long* lp;
		/*
		 * Align to longword boundary and check longwords.
		 */
		while (!isAligned(bp, long)) {
			if (*bp != 0xff)
				return (span + oneruns[*bp]);
			span += 8, bits -= 8;
			bp++;
		}
		lp = (long*) bp;
		while (bits >= 8*sizeof (long) && *lp == ~0) {
			span += 8*sizeof (long), bits -= 8*sizeof (long);
			lp++;
		}
		bp = (u_char*) lp;
	}
	/*
	 * Scan full bytes for all 1's.
	 */
	while (bits >= 8) {
		if (*bp != 0xff)	/* end of run */
			return (span + oneruns[*bp]);
		span += 8, bits -= 8;
		bp++;
	}
	/*
	 * Check partial byte on rhs.
	 */
	if (bits > 0) {
		n = oneruns[*bp];
		span += (n > bits ? bits : n);
	}
	return (span);
}

/*
 * Return the offset of the next bit in the range
 * [bs..be] that is different from the specified
 * color.  The end, be, is returned if no such bit
 * exists.
 */
#define	finddiff(_cp, _bs, _be, _color)	\
	(_bs + (_color ? find1span(_cp,_bs,_be) : find0span(_cp,_bs,_be)))
/*
 * Like finddiff, but also check the starting bit
 * against the end in case start > end.
 */
#define	finddiff2(_cp, _bs, _be, _color) \
	(_bs < _be ? finddiff(_cp,_bs,_be,_color) : _be)

/*
 * 1d-encode a row of pixels.  The encoding is
 * a sequence of all-white or all-black spans
 * of pixels encoded with Huffman codes.
 */
static int
Fax3Encode1DRow(TIFF* tif, u_char* bp, uint32 bits)
{
	Fax3EncodeState* sp = EncoderState(tif);
	int32 span;
        uint32 bs = 0;

	for (;;) {
		span = find0span(bp, bs, bits);		/* white span */
		putspan(tif, span, TIFFFaxWhiteCodes);
		bs += span;
		if (bs >= bits)
			break;
		span = find1span(bp, bs, bits);		/* black span */
		putspan(tif, span, TIFFFaxBlackCodes);
		bs += span;
		if (bs >= bits)
			break;
	}
	if (sp->b.mode & (FAXMODE_BYTEALIGN|FAXMODE_WORDALIGN)) {
		if (sp->bit != 8)			/* byte-align */
			Fax3FlushBits(tif, sp);
		if ((sp->b.mode&FAXMODE_WORDALIGN) &&
		    !isAligned(tif->tif_rawcp, uint16))
			Fax3FlushBits(tif, sp);
	}
	return (1);
}

static const tableentry horizcode =
    { 3, 0x1 };		/* 001 */
static const tableentry passcode =
    { 4, 0x1 };		/* 0001 */
static const tableentry vcodes[7] = {
    { 7, 0x03 },	/* 0000 011 */
    { 6, 0x03 },	/* 0000 11 */
    { 3, 0x03 },	/* 011 */
    { 1, 0x1 },		/* 1 */
    { 3, 0x2 },		/* 010 */
    { 6, 0x02 },	/* 0000 10 */
    { 7, 0x02 }		/* 0000 010 */
};

/*
 * 2d-encode a row of pixels.  Consult the CCITT
 * documentation for the algorithm.
 */
static int
Fax3Encode2DRow(TIFF* tif, u_char* bp, u_char* rp, uint32 bits)
{
#define	PIXEL(buf,ix)	((((buf)[(ix)>>3]) >> (7-((ix)&7))) & 1)
        uint32 a0 = 0;
	uint32 a1 = (PIXEL(bp, 0) != 0 ? 0 : finddiff(bp, 0, bits, 0));
	uint32 b1 = (PIXEL(rp, 0) != 0 ? 0 : finddiff(rp, 0, bits, 0));
	uint32 a2, b2;

	for (;;) {
		b2 = finddiff2(rp, b1, bits, PIXEL(rp,b1));
		if (b2 >= a1) {
			int32 d = b1 - a1;
			if (!(-3 <= d && d <= 3)) {	/* horizontal mode */
				a2 = finddiff2(bp, a1, bits, PIXEL(bp,a1));
				putcode(tif, &horizcode);
				if (a0+a1 == 0 || PIXEL(bp, a0) == 0) {
					putspan(tif, a1-a0, TIFFFaxWhiteCodes);
					putspan(tif, a2-a1, TIFFFaxBlackCodes);
				} else {
					putspan(tif, a1-a0, TIFFFaxBlackCodes);
					putspan(tif, a2-a1, TIFFFaxWhiteCodes);
				}
				a0 = a2;
			} else {			/* vertical mode */
				putcode(tif, &vcodes[d+3]);
				a0 = a1;
			}
		} else {				/* pass mode */
			putcode(tif, &passcode);
			a0 = b2;
		}
		if (a0 >= bits)
			break;
		a1 = finddiff(bp, a0, bits, PIXEL(bp,a0));
		b1 = finddiff(rp, a0, bits, !PIXEL(bp,a0));
		b1 = finddiff(rp, b1, bits, PIXEL(bp,a0));
	}
	return (1);
#undef PIXEL
}

/*
 * Encode a buffer of pixels.
 */
static int
Fax3Encode(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	Fax3EncodeState* sp = EncoderState(tif);

	(void) s;
	while ((long)cc > 0) {
		if ((sp->b.mode & FAXMODE_NOEOL) == 0)
			Fax3PutEOL(tif);
		if (is2DEncoding(sp)) {
			if (sp->tag == G3_1D) {
				if (!Fax3Encode1DRow(tif, bp, sp->b.rowpixels))
					return (0);
				sp->tag = G3_2D;
			} else {
				if (!Fax3Encode2DRow(tif, bp, sp->refline, sp->b.rowpixels))
					return (0);
				sp->k--;
			}
			if (sp->k == 0) {
				sp->tag = G3_1D;
				sp->k = sp->maxk-1;
			} else
				_TIFFmemcpy(sp->refline, bp, sp->b.rowbytes);
		} else {
			if (!Fax3Encode1DRow(tif, bp, sp->b.rowpixels))
				return (0);
		}
		bp += sp->b.rowbytes;
		cc -= sp->b.rowbytes;
		if (cc != 0)
			tif->tif_row++;
	}
	return (1);
}

static int
Fax3PostEncode(TIFF* tif)
{
	Fax3EncodeState* sp = EncoderState(tif);

	if (sp->bit != 8)
		Fax3FlushBits(tif, sp);
	return (1);
}

static void
Fax3Close(TIFF* tif)
{
	if ((Fax3State(tif)->mode & FAXMODE_NORTC) == 0) {
		Fax3EncodeState* sp = EncoderState(tif);
		u_int code = EOL;
		u_int length = 12;
		int i;

		if (is2DEncoding(sp))
			code = (code<<1) | (sp->tag == G3_1D), length++;
		for (i = 0; i < 6; i++)
			Fax3PutBits(tif, code, length);
		Fax3FlushBits(tif, sp);
	}
}

static void
Fax3Cleanup(TIFF* tif)
{
	if (tif->tif_data) {
		if (tif->tif_mode == O_RDONLY) {
			Fax3DecodeState* sp = DecoderState(tif);
			if (sp->runs)
				_TIFFfree(sp->runs);
		} else {
			Fax3EncodeState* sp = EncoderState(tif);
			if (sp->refline)
				_TIFFfree(sp->refline);
		}
		if (Fax3State(tif)->subaddress)
			_TIFFfree(Fax3State(tif)->subaddress);
		_TIFFfree(tif->tif_data);
		tif->tif_data = NULL;
	}
}

#define	FIELD_BADFAXLINES	(FIELD_CODEC+0)
#define	FIELD_CLEANFAXDATA	(FIELD_CODEC+1)
#define	FIELD_BADFAXRUN		(FIELD_CODEC+2)
#define	FIELD_RECVPARAMS	(FIELD_CODEC+3)
#define	FIELD_SUBADDRESS	(FIELD_CODEC+4)
#define	FIELD_RECVTIME		(FIELD_CODEC+5)

#define	FIELD_OPTIONS		(FIELD_CODEC+6)

static const TIFFFieldInfo faxFieldInfo[] = {
    { TIFFTAG_FAXMODE,		 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      FALSE,	FALSE,	"FaxMode" },
    { TIFFTAG_FAXFILLFUNC,	 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      FALSE,	FALSE,	"FaxFillFunc" },
    { TIFFTAG_BADFAXLINES,	 1, 1,	TIFF_LONG,	FIELD_BADFAXLINES,
      TRUE,	FALSE,	"BadFaxLines" },
    { TIFFTAG_BADFAXLINES,	 1, 1,	TIFF_SHORT,	FIELD_BADFAXLINES,
      TRUE,	FALSE,	"BadFaxLines" },
    { TIFFTAG_CLEANFAXDATA,	 1, 1,	TIFF_SHORT,	FIELD_CLEANFAXDATA,
      TRUE,	FALSE,	"CleanFaxData" },
    { TIFFTAG_CONSECUTIVEBADFAXLINES,1,1, TIFF_LONG,	FIELD_BADFAXRUN,
      TRUE,	FALSE,	"ConsecutiveBadFaxLines" },
    { TIFFTAG_CONSECUTIVEBADFAXLINES,1,1, TIFF_SHORT,	FIELD_BADFAXRUN,
      TRUE,	FALSE,	"ConsecutiveBadFaxLines" },
    { TIFFTAG_FAXRECVPARAMS,	 1, 1, TIFF_LONG,	FIELD_RECVPARAMS,
      TRUE,	FALSE,	"FaxRecvParams" },
    { TIFFTAG_FAXSUBADDRESS,	-1,-1, TIFF_ASCII,	FIELD_SUBADDRESS,
      TRUE,	FALSE,	"FaxSubAddress" },
    { TIFFTAG_FAXRECVTIME,	 1, 1, TIFF_LONG,	FIELD_RECVTIME,
      TRUE,	FALSE,	"FaxRecvTime" },
};
static const TIFFFieldInfo fax3FieldInfo[] = {
    { TIFFTAG_GROUP3OPTIONS,	 1, 1,	TIFF_LONG,	FIELD_OPTIONS,
      FALSE,	FALSE,	"Group3Options" },
};
static const TIFFFieldInfo fax4FieldInfo[] = {
    { TIFFTAG_GROUP4OPTIONS,	 1, 1,	TIFF_LONG,	FIELD_OPTIONS,
      FALSE,	FALSE,	"Group4Options" },
};
#define	N(a)	(sizeof (a) / sizeof (a[0]))

static int
Fax3VSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	Fax3BaseState* sp = Fax3State(tif);

	switch (tag) {
	case TIFFTAG_FAXMODE:
		sp->mode = va_arg(ap, int);
		return (1);			/* NB: pseudo tag */
	case TIFFTAG_FAXFILLFUNC:
		if (tif->tif_mode == O_RDONLY)
			DecoderState(tif)->fill = va_arg(ap, TIFFFaxFillFunc);
		return (1);			/* NB: pseudo tag */
	case TIFFTAG_GROUP3OPTIONS:
	case TIFFTAG_GROUP4OPTIONS:
		sp->groupoptions = va_arg(ap, uint32);
		break;
	case TIFFTAG_BADFAXLINES:
		sp->badfaxlines = va_arg(ap, uint32);
		break;
	case TIFFTAG_CLEANFAXDATA:
		sp->cleanfaxdata = (uint16) va_arg(ap, int);
		break;
	case TIFFTAG_CONSECUTIVEBADFAXLINES:
		sp->badfaxrun = va_arg(ap, uint32);
		break;
	case TIFFTAG_FAXRECVPARAMS:
		sp->recvparams = va_arg(ap, uint32);
		break;
	case TIFFTAG_FAXSUBADDRESS:
		_TIFFsetString(&sp->subaddress, va_arg(ap, char*));
		break;
	case TIFFTAG_FAXRECVTIME:
		sp->recvtime = va_arg(ap, uint32);
		break;
	default:
		return (*sp->vsetparent)(tif, tag, ap);
	}
	TIFFSetFieldBit(tif, _TIFFFieldWithTag(tif, tag)->field_bit);
	tif->tif_flags |= TIFF_DIRTYDIRECT;
	return (1);
}

static int
Fax3VGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	Fax3BaseState* sp = Fax3State(tif);

	switch (tag) {
	case TIFFTAG_FAXMODE:
		*va_arg(ap, int*) = sp->mode;
		break;
	case TIFFTAG_FAXFILLFUNC:
		if (tif->tif_mode == O_RDONLY)
			*va_arg(ap, TIFFFaxFillFunc*) = DecoderState(tif)->fill;
		break;
	case TIFFTAG_GROUP3OPTIONS:
	case TIFFTAG_GROUP4OPTIONS:
		*va_arg(ap, uint32*) = sp->groupoptions;
		break;
	case TIFFTAG_BADFAXLINES:
		*va_arg(ap, uint32*) = sp->badfaxlines;
		break;
	case TIFFTAG_CLEANFAXDATA:
		*va_arg(ap, uint16*) = sp->cleanfaxdata;
		break;
	case TIFFTAG_CONSECUTIVEBADFAXLINES:
		*va_arg(ap, uint32*) = sp->badfaxrun;
		break;
	case TIFFTAG_FAXRECVPARAMS:
		*va_arg(ap, uint32*) = sp->recvparams;
		break;
	case TIFFTAG_FAXSUBADDRESS:
		*va_arg(ap, char**) = sp->subaddress;
		break;
	case TIFFTAG_FAXRECVTIME:
		*va_arg(ap, uint32*) = sp->recvtime;
		break;
	default:
		return (*sp->vgetparent)(tif, tag, ap);
	}
	return (1);
}

static void
Fax3PrintDir(TIFF* tif, FILE* fd, long flags)
{
	Fax3BaseState* sp = Fax3State(tif);

	(void) flags;
	if (TIFFFieldSet(tif,FIELD_OPTIONS)) {
		const char* sep = " ";
		if (tif->tif_dir.td_compression == COMPRESSION_CCITTFAX4) {
			fprintf(fd, "  Group 4 Options:");
			if (sp->groupoptions & GROUP4OPT_UNCOMPRESSED)
				fprintf(fd, "%suncompressed data", sep);
		} else {

			fprintf(fd, "  Group 3 Options:");
			if (sp->groupoptions & GROUP3OPT_2DENCODING)
				fprintf(fd, "%s2-d encoding", sep), sep = "+";
			if (sp->groupoptions & GROUP3OPT_FILLBITS)
				fprintf(fd, "%sEOL padding", sep), sep = "+";
			if (sp->groupoptions & GROUP3OPT_UNCOMPRESSED)
				fprintf(fd, "%suncompressed data", sep);
		}
		fprintf(fd, " (%lu = 0x%lx)\n",
		    (u_long) sp->groupoptions, (u_long) sp->groupoptions);
	}
	if (TIFFFieldSet(tif,FIELD_CLEANFAXDATA)) {
		fprintf(fd, "  Fax Data:");
		switch (sp->cleanfaxdata) {
		case CLEANFAXDATA_CLEAN:
			fprintf(fd, " clean");
			break;
		case CLEANFAXDATA_REGENERATED:
			fprintf(fd, " receiver regenerated");
			break;
		case CLEANFAXDATA_UNCLEAN:
			fprintf(fd, " uncorrected errors");
			break;
		}
		fprintf(fd, " (%u = 0x%x)\n",
		    sp->cleanfaxdata, sp->cleanfaxdata);
	}
	if (TIFFFieldSet(tif,FIELD_BADFAXLINES))
		fprintf(fd, "  Bad Fax Lines: %lu\n", (u_long) sp->badfaxlines);
	if (TIFFFieldSet(tif,FIELD_BADFAXRUN))
		fprintf(fd, "  Consecutive Bad Fax Lines: %lu\n",
		    (u_long) sp->badfaxrun);
	if (TIFFFieldSet(tif,FIELD_RECVPARAMS))
		fprintf(fd, "  Fax Receive Parameters: %08lx\n",
		   (u_long) sp->recvparams);
	if (TIFFFieldSet(tif,FIELD_SUBADDRESS))
		fprintf(fd, "  Fax SubAddress: %s\n", sp->subaddress);
	if (TIFFFieldSet(tif,FIELD_RECVTIME))
		fprintf(fd, "  Fax Receive Time: %lu secs\n",
		    (u_long) sp->recvtime);
}

static int
InitCCITTFax3(TIFF* tif)
{
	Fax3BaseState* sp;

	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	if (tif->tif_mode == O_RDONLY)
		tif->tif_data = (tidata_t)
                    _TIFFmalloc(sizeof (Fax3DecodeState));
	else
		tif->tif_data = (tidata_t)
                    _TIFFmalloc(sizeof (Fax3EncodeState));
        
	if (tif->tif_data == NULL) {
		TIFFError("TIFFInitCCITTFax3",
		    "%s: No space for state block", tif->tif_name);
		return (0);
	}
	sp = Fax3State(tif);

	/*
	 * Merge codec-specific tag information and
	 * override parent get/set field methods.
	 */
	_TIFFMergeFieldInfo(tif, faxFieldInfo, N(faxFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = Fax3VGetField;	/* hook for codec tags */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = Fax3VSetField;	/* hook for codec tags */
	tif->tif_printdir = Fax3PrintDir;	/* hook for codec tags */
	sp->groupoptions = 0;	
	sp->recvparams = 0;
	sp->subaddress = NULL;

	if (tif->tif_mode == O_RDONLY) {
		tif->tif_flags |= TIFF_NOBITREV;/* decoder does bit reversal */
		DecoderState(tif)->runs = NULL;
		TIFFSetField(tif, TIFFTAG_FAXFILLFUNC, _TIFFFax3fillruns);
	} else
		EncoderState(tif)->refline = NULL;

	/*
	 * Install codec methods.
	 */
	tif->tif_setupdecode = Fax3SetupState;
	tif->tif_predecode = Fax3PreDecode;
	tif->tif_decoderow = Fax3Decode1D;
	tif->tif_decodestrip = Fax3Decode1D;
	tif->tif_decodetile = Fax3Decode1D;
	tif->tif_setupencode = Fax3SetupState;
	tif->tif_preencode = Fax3PreEncode;
	tif->tif_postencode = Fax3PostEncode;
	tif->tif_encoderow = Fax3Encode;
	tif->tif_encodestrip = Fax3Encode;
	tif->tif_encodetile = Fax3Encode;
	tif->tif_close = Fax3Close;
	tif->tif_cleanup = Fax3Cleanup;

	return (1);
}

int
TIFFInitCCITTFax3(TIFF* tif, int scheme)
{
	if (InitCCITTFax3(tif)) {
		_TIFFMergeFieldInfo(tif, fax3FieldInfo, N(fax3FieldInfo));

		/*
		 * The default format is Class/F-style w/o RTC.
		 */
		return TIFFSetField(tif, TIFFTAG_FAXMODE, FAXMODE_CLASSF);
	} else
		return (0);
}

/*
 * CCITT Group 4 (T.6) Facsimile-compatible
 * Compression Scheme Support.
 */

#define	SWAP(t,a,b)	{ t x; x = (a); (a) = (b); (b) = x; }
/*
 * Decode the requested amount of G4-encoded data.
 */
static int
Fax4Decode(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	DECLARE_STATE_2D(tif, sp, "Fax4Decode");

	(void) s;
	CACHE_STATE(tif, sp);
	while ((long)occ > 0) {
		a0 = 0;
		RunLength = 0;
		pa = thisrun = sp->curruns;
		pb = sp->refruns;
		b1 = *pb++;
#ifdef FAX3_DEBUG
		printf("\nBitAcc=%08X, BitsAvail = %d\n", BitAcc, BitsAvail);
		printf("-------------------- %d\n", tif->tif_row);
		fflush(stdout);
#endif
		EXPAND2D(EOFG4);
		(*sp->fill)(buf, thisrun, pa, lastx);
		SETVAL(0);		/* imaginary change for reference */
		SWAP(uint32*, sp->curruns, sp->refruns);
		buf += sp->b.rowbytes;
		occ -= sp->b.rowbytes;
		if (occ != 0)
			tif->tif_row++;
		continue;
	EOFG4:
		(*sp->fill)(buf, thisrun, pa, lastx);
		UNCACHE_STATE(tif, sp);
		return (-1);
	}
	UNCACHE_STATE(tif, sp);
	return (1);
}
#undef	SWAP

/*
 * Encode the requested amount of data.
 */
static int
Fax4Encode(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	Fax3EncodeState *sp = EncoderState(tif);

	(void) s;
	while ((long)cc > 0) {
		if (!Fax3Encode2DRow(tif, bp, sp->refline, sp->b.rowpixels))
			return (0);
		_TIFFmemcpy(sp->refline, bp, sp->b.rowbytes);
		bp += sp->b.rowbytes;
		cc -= sp->b.rowbytes;
		if (cc != 0)
			tif->tif_row++;
	}
	return (1);
}

static int
Fax4PostEncode(TIFF* tif)
{
	Fax3EncodeState *sp = EncoderState(tif);

	/* terminate strip w/ EOFB */
	Fax3PutBits(tif, EOL, 12);
	Fax3PutBits(tif, EOL, 12);
	if (sp->bit != 8)
		Fax3FlushBits(tif, sp);
	return (1);
}

int
TIFFInitCCITTFax4(TIFF* tif, int scheme)
{
	if (InitCCITTFax3(tif)) {		/* reuse G3 support */
		_TIFFMergeFieldInfo(tif, fax4FieldInfo, N(fax4FieldInfo));

		tif->tif_decoderow = Fax4Decode;
		tif->tif_decodestrip = Fax4Decode;
		tif->tif_decodetile = Fax4Decode;
		tif->tif_encoderow = Fax4Encode;
		tif->tif_encodestrip = Fax4Encode;
		tif->tif_encodetile = Fax4Encode;
		tif->tif_postencode = Fax4PostEncode;
		/*
		 * Suppress RTC at the end of each strip.
		 */
		return TIFFSetField(tif, TIFFTAG_FAXMODE, FAXMODE_NORTC);
	} else
		return (0);
}

/*
 * CCITT Group 3 1-D Modified Huffman RLE Compression Support.
 * (Compression algorithms 2 and 32771)
 */

/*
 * Decode the requested amount of RLE-encoded data.
 */
static int
Fax3DecodeRLE(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	DECLARE_STATE(tif, sp, "Fax3DecodeRLE");
	int mode = sp->b.mode;

	(void) s;
	CACHE_STATE(tif, sp);
	thisrun = sp->curruns;
	while ((long)occ > 0) {
		a0 = 0;
		RunLength = 0;
		pa = thisrun;
#ifdef FAX3_DEBUG
		printf("\nBitAcc=%08X, BitsAvail = %d\n", BitAcc, BitsAvail);
		printf("-------------------- %d\n", tif->tif_row);
		fflush(stdout);
#endif
		EXPAND1D(EOFRLE);
		(*sp->fill)(buf, thisrun, pa, lastx);
		/*
		 * Cleanup at the end of the row.
		 */
		if (mode & FAXMODE_BYTEALIGN) {
			int n = BitsAvail - (BitsAvail &~ 7);
			ClrBits(n);
		} else if (mode & FAXMODE_WORDALIGN) {
			int n = BitsAvail - (BitsAvail &~ 15);
			ClrBits(n);
			if (BitsAvail == 0 && !isAligned(cp, uint16))
			    cp++;
		}
		buf += sp->b.rowbytes;
		occ -= sp->b.rowbytes;
		if (occ != 0)
			tif->tif_row++;
		continue;
	EOFRLE:				/* premature EOF */
		(*sp->fill)(buf, thisrun, pa, lastx);
		UNCACHE_STATE(tif, sp);
		return (-1);
	}
	UNCACHE_STATE(tif, sp);
	return (1);
}

int
TIFFInitCCITTRLE(TIFF* tif, int scheme)
{
	if (InitCCITTFax3(tif)) {		/* reuse G3 support */
		tif->tif_decoderow = Fax3DecodeRLE;
		tif->tif_decodestrip = Fax3DecodeRLE;
		tif->tif_decodetile = Fax3DecodeRLE;
		/*
		 * Suppress RTC+EOLs when encoding and byte-align data.
		 */
		return TIFFSetField(tif, TIFFTAG_FAXMODE,
		    FAXMODE_NORTC|FAXMODE_NOEOL|FAXMODE_BYTEALIGN);
	} else
		return (0);
}

int
TIFFInitCCITTRLEW(TIFF* tif, int scheme)
{
	if (InitCCITTFax3(tif)) {		/* reuse G3 support */
		tif->tif_decoderow = Fax3DecodeRLE;
		tif->tif_decodestrip = Fax3DecodeRLE;
		tif->tif_decodetile = Fax3DecodeRLE;
		/*
		 * Suppress RTC+EOLs when encoding and word-align data.
		 */
		return TIFFSetField(tif, TIFFTAG_FAXMODE,
		    FAXMODE_NORTC|FAXMODE_NOEOL|FAXMODE_WORDALIGN);
	} else
		return (0);
}
#endif /* CCITT_SUPPORT */


/* ==== tif_fax3sm.c ==== */
/* WARNING, this file was automatically generated by the
    mkg3states program */
#include "tiff.h"
#include "tif_fax3.h"
 const TIFFFaxTabEnt TIFFFaxMainTable[128] = {
12,7,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,1,4,0,3,1,0,5,3,1,3,1,0,
2,3,0,3,1,0,4,3,1,3,1,0,5,6,2,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,
1,4,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,5,7,3,3,1,0,5,3,1,3,1,0,
2,3,0,3,1,0,4,3,1,3,1,0,1,4,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,
4,6,2,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,1,4,0,3,1,0,5,3,1,3,1,0,
2,3,0,3,1,0,4,3,1,3,1,0,6,7,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,
1,4,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,5,6,2,3,1,0,5,3,1,3,1,0,
2,3,0,3,1,0,4,3,1,3,1,0,1,4,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,
4,7,3,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,1,4,0,3,1,0,5,3,1,3,1,0,
2,3,0,3,1,0,4,3,1,3,1,0,4,6,2,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0,
1,4,0,3,1,0,5,3,1,3,1,0,2,3,0,3,1,0,4,3,1,3,1,0
};
 const TIFFFaxTabEnt TIFFFaxWhiteTable[4096] = {
12,11,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1472,7,4,5,7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,960,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,11,1792,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,
7,8,44,7,6,17,9,9,1344,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,
7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,
7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,
7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
11,11,1856,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,
7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,
7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1728,7,4,5,7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,
7,8,42,7,6,16,9,9,1152,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,
9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,
7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1472,7,4,5,
7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,
7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,960,7,4,6,
7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,11,12,2112,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,
7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,7,8,44,7,6,17,9,9,1344,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,
7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,
7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,12,2368,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1728,7,4,5,
7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1152,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,
7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1472,7,4,5,7,8,43,7,6,17,9,9,1216,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,
7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,960,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,704,7,4,6,
7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
11,12,1984,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,832,7,4,6,
7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,
7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1600,7,4,5,7,8,44,7,6,17,9,9,1344,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,
7,8,42,7,6,16,9,9,1088,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,
9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,
7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1536,7,4,5,
7,8,43,7,6,17,9,9,1280,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,
7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,1024,7,4,6,
7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,9,768,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,11,11,1920,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,40,7,6,16,9,9,896,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,
7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1728,7,4,5,7,8,44,7,6,17,9,9,1408,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,
7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1152,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,
7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1472,7,4,5,7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,960,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,12,2240,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,
7,8,44,7,6,17,9,9,1344,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,
7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,
7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,
7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
11,12,2496,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,
7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,
7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1728,7,4,5,7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,
7,8,42,7,6,16,9,9,1152,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,
9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,12,11,0,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,
7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1472,7,4,5,
7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,
7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,960,7,4,6,
7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,11,11,1792,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,
7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,7,8,44,7,6,17,9,9,1344,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,
7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,
7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,11,1856,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1728,7,4,5,
7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1152,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,
7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1472,7,4,5,7,8,43,7,6,17,9,9,1216,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,
7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,960,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,704,7,4,6,
7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
11,12,2176,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,832,7,4,6,
7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,
7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1600,7,4,5,7,8,44,7,6,17,9,9,1344,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,
7,8,42,7,6,16,9,9,1088,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,
9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,
7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1536,7,4,5,
7,8,43,7,6,17,9,9,1280,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,
7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,1024,7,4,6,
7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,9,768,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,11,12,2432,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,40,7,6,16,9,9,896,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,
7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1728,7,4,5,7,8,44,7,6,17,9,9,1408,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,
7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1152,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,
7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1472,7,4,5,7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,960,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,12,2048,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,
7,8,44,7,6,17,9,9,1344,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,
7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,
7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,
7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
11,11,1920,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,
7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,
7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1728,7,4,5,7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,
7,8,42,7,6,16,9,9,1152,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,
9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,0,0,0,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,7,7,19,7,5,8,7,8,55,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,7,8,53,7,5,9,9,8,448,7,4,6,
7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1472,7,4,5,
7,8,43,7,6,17,9,9,1216,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,33,9,5,128,7,8,49,7,6,14,
7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,7,8,41,7,6,16,9,9,960,7,4,6,
7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,9,704,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,9,8,320,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,11,12,2304,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,
7,8,40,7,6,16,9,9,832,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,7,8,36,9,5,128,7,8,52,7,6,15,
7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1600,7,4,5,7,8,44,7,6,17,9,9,1344,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,30,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,7,8,62,7,4,4,7,4,2,7,4,7,
7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1088,7,4,6,7,8,32,7,5,8,7,8,58,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,8,640,7,4,6,
7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,
7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
0,0,0,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,
7,7,28,7,4,4,7,4,2,7,4,7,7,7,23,7,4,3,7,7,27,7,4,5,7,8,39,7,6,16,9,8,576,7,4,6,
7,7,19,7,5,8,7,8,55,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,8,45,7,4,3,7,5,11,7,4,5,
7,8,53,7,5,9,9,8,448,7,4,6,7,8,35,9,5,128,7,8,51,7,6,15,7,8,63,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,9,9,1536,7,4,5,7,8,43,7,6,17,9,9,1280,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,29,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,
7,8,33,9,5,128,7,8,49,7,6,14,7,8,61,7,4,4,7,4,2,7,4,7,7,8,47,7,4,3,7,8,59,7,4,5,
7,8,41,7,6,16,9,9,1024,7,4,6,7,8,31,7,5,8,7,8,57,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,7,22,7,4,3,7,5,11,7,4,5,7,7,26,7,5,9,9,9,768,7,4,6,7,8,37,9,5,128,7,7,25,7,6,15,
9,8,320,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,
7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,11,12,2560,7,4,3,7,5,11,7,4,5,
7,6,12,7,5,9,9,6,1664,7,4,6,7,7,20,9,5,128,7,7,24,7,6,14,7,7,28,7,4,4,7,4,2,7,4,7,
7,7,23,7,4,3,7,7,27,7,4,5,7,8,40,7,6,16,9,9,896,7,4,6,7,7,19,7,5,8,7,8,56,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7,7,8,46,7,4,3,7,5,11,7,4,5,7,8,54,7,5,9,9,8,512,7,4,6,
7,8,36,9,5,128,7,8,52,7,6,15,7,8,0,7,4,4,7,4,2,7,4,7,7,6,13,7,4,3,9,9,1728,7,4,5,
7,8,44,7,6,17,9,9,1408,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,
7,8,30,7,4,3,7,5,11,7,4,5,7,6,12,7,5,9,9,6,1664,7,4,6,7,8,34,9,5,128,7,8,50,7,6,14,
7,8,62,7,4,4,7,4,2,7,4,7,7,8,48,7,4,3,7,8,60,7,4,5,7,8,42,7,6,16,9,9,1152,7,4,6,
7,8,32,7,5,8,7,8,58,9,5,64,7,5,10,7,4,4,7,4,2,7,4,7,7,7,22,7,4,3,7,5,11,7,4,5,
7,7,26,7,5,9,9,8,640,7,4,6,7,8,38,9,5,128,7,7,25,7,6,15,9,8,384,7,4,4,7,4,2,7,4,7,
7,6,13,7,4,3,7,7,18,7,4,5,7,7,21,7,6,17,9,7,256,7,4,6,7,6,1,7,5,8,9,6,192,9,5,64,
7,5,10,7,4,4,7,4,2,7,4,7
};
 const TIFFFaxTabEnt TIFFFaxBlackTable[8192] = {
12,11,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,11,1792,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,23,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,20,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,25,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,12,128,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,56,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,30,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,11,1856,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,57,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,21,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,54,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,52,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,48,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,2112,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,44,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,36,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,12,384,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,28,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,60,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,40,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2368,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,1984,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,50,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,34,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,1664,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,26,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,1408,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,32,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,11,1920,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,61,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,42,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,1024,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,768,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,62,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2240,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,46,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,38,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,512,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,19,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,24,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,22,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2496,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,12,11,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,18,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,17,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,11,1792,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,23,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,20,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,25,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,12,192,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,1280,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,31,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,11,1856,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,58,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,21,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,896,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,640,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,49,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2176,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,45,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,37,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,12,448,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,29,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,1536,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,41,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,2432,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,16,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,10,64,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2048,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,51,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,35,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,12,320,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,27,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,59,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,33,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,11,1920,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,12,256,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,43,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,1152,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,55,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,63,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,2304,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,47,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,39,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,53,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,19,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,24,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,22,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2560,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,12,11,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,11,1792,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,23,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,20,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,25,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,12,128,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,56,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,30,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,11,1856,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,57,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,21,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,54,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,52,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,48,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2112,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,44,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,36,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,12,384,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,28,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,60,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,40,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2368,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,18,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,17,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,1984,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,50,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,34,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,1728,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,26,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,1472,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,32,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,11,1920,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,61,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,42,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,1088,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,832,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,62,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2240,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,46,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,38,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,576,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,19,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,24,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,22,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,2496,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,16,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,10,64,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
12,11,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,11,1792,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,23,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,20,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,25,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,12,192,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,1344,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,31,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,11,1856,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,58,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,11,21,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,960,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,13,704,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,49,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,12,2176,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,45,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,37,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,12,448,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,29,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,13,1600,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,41,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2432,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,18,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,10,17,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2048,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,51,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,35,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,12,320,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,27,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,59,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,33,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,11,11,1920,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
10,12,256,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,43,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,10,13,1216,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,13,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,9,15,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,55,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,63,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,11,12,2304,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,12,47,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,12,39,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,12,53,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,8,14,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,0,0,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,13,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,19,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,11,24,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,7,11,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,11,22,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
11,12,2560,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,9,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,10,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,10,16,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,10,0,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,10,10,64,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,6,9,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,8,7,11,8,2,3,8,3,1,8,2,2,
8,4,6,8,2,3,8,3,4,8,2,2,8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2,
8,8,14,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,8,6,8,8,2,3,8,3,1,8,2,2,
8,4,5,8,2,3,8,3,4,8,2,2,8,7,12,8,2,3,8,3,1,8,2,2,8,4,6,8,2,3,8,3,4,8,2,2,
8,5,7,8,2,3,8,3,1,8,2,2,8,4,5,8,2,3,8,3,4,8,2,2
};


/* ==== tif_flush.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_flush.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 */
#include "tiffiop.h"

int
TIFFFlush(TIFF* tif)
{

	if (tif->tif_mode != O_RDONLY) {
		if (!TIFFFlushData(tif))
			return (0);
		if ((tif->tif_flags & TIFF_DIRTYDIRECT) &&
		    !TIFFWriteDirectory(tif))
			return (0);
	}
	return (1);
}

/*
 * Flush buffered data to the file.
 */
int
TIFFFlushData(TIFF* tif)
{
	if ((tif->tif_flags & TIFF_BEENWRITING) == 0)
		return (0);
	if (tif->tif_flags & TIFF_POSTENCODE) {
		tif->tif_flags &= ~TIFF_POSTENCODE;
		if (!(*tif->tif_postencode)(tif))
			return (0);
	}
	return (TIFFFlushData1(tif));
}


/* ==== tif_getimage.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_getimage.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1991-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library
 *
 * Read and return a packed RGBA image.
 */
#include "tiffiop.h"
#include <assert.h>
#include <stdio.h>

static	int gtTileContig(TIFFRGBAImage*, uint32*, uint32, uint32);
static	int gtTileSeparate(TIFFRGBAImage*, uint32*, uint32, uint32);
static	int gtStripContig(TIFFRGBAImage*, uint32*, uint32, uint32);
static	int gtStripSeparate(TIFFRGBAImage*, uint32*, uint32, uint32);
static	int pickTileContigCase(TIFFRGBAImage*);
static	int pickTileSeparateCase(TIFFRGBAImage*);

static	const char photoTag[] = "PhotometricInterpretation";

/*
 * Check the image to see if TIFFReadRGBAImage can deal with it.
 * 1/0 is returned according to whether or not the image can
 * be handled.  If 0 is returned, emsg contains the reason
 * why it is being rejected.
 */
int
TIFFRGBAImageOK(TIFF* tif, char emsg[1024])
{
    TIFFDirectory* td = &tif->tif_dir;
    uint16 photometric;
    int colorchannels;

    switch (td->td_bitspersample) {
    case 1: case 2: case 4:
    case 8: case 16:
	break;
    default:
	sprintf(emsg, "Sorry, can not handle images with %d-bit samples",
	    td->td_bitspersample);
	return (0);
    }
    colorchannels = td->td_samplesperpixel - td->td_extrasamples;
    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric)) {
	switch (colorchannels) {
	case 1:
	    photometric = PHOTOMETRIC_MINISBLACK;
	    break;
	case 3:
	    photometric = PHOTOMETRIC_RGB;
	    break;
	default:
	    sprintf(emsg, "Missing needed %s tag", photoTag);
	    return (0);
	}
    }
    switch (photometric) {
    case PHOTOMETRIC_MINISWHITE:
    case PHOTOMETRIC_MINISBLACK:
    case PHOTOMETRIC_PALETTE:
	if (td->td_planarconfig == PLANARCONFIG_CONTIG && td->td_samplesperpixel != 1) {
	    sprintf(emsg,
		"Sorry, can not handle contiguous data with %s=%d, and %s=%d",
		photoTag, photometric,
		"Samples/pixel", td->td_samplesperpixel);
	    return (0);
	}
	break;
    case PHOTOMETRIC_YCBCR:
	if (td->td_planarconfig != PLANARCONFIG_CONTIG) {
	    sprintf(emsg, "Sorry, can not handle YCbCr images with %s=%d",
		"Planarconfiguration", td->td_planarconfig);
	    return (0);
	}
	break;
    case PHOTOMETRIC_RGB: 
	if (colorchannels < 3) {
	    sprintf(emsg, "Sorry, can not handle RGB image with %s=%d",
		"Color channels", colorchannels);
	    return (0);
	}
	break;
#ifdef CMYK_SUPPORT
    case PHOTOMETRIC_SEPARATED:
	if (td->td_inkset != INKSET_CMYK) {
	    sprintf(emsg, "Sorry, can not handle separated image with %s=%d",
		"InkSet", td->td_inkset);
	    return (0);
	}
	if (td->td_samplesperpixel != 4) {
	    sprintf(emsg, "Sorry, can not handle separated image with %s=%d",
		"Samples/pixel", td->td_samplesperpixel);
	    return (0);
	}
	break;
#endif
    case PHOTOMETRIC_LOGL:
	if (td->td_compression != COMPRESSION_SGILOG) {
	    sprintf(emsg, "Sorry, LogL data must have %s=%d",
		"Compression", COMPRESSION_SGILOG);
	    return (0);
	}
	break;
    case PHOTOMETRIC_LOGLUV:
	if (td->td_compression != COMPRESSION_SGILOG &&
		td->td_compression != COMPRESSION_SGILOG24) {
	    sprintf(emsg, "Sorry, LogLuv data must have %s=%d or %d",
		"Compression", COMPRESSION_SGILOG, COMPRESSION_SGILOG24);
	    return (0);
	}
	if (td->td_planarconfig != PLANARCONFIG_CONTIG) {
	    sprintf(emsg, "Sorry, can not handle LogLuv images with %s=%d",
		"Planarconfiguration", td->td_planarconfig);
	    return (0);
	}
	break;
    default:
	sprintf(emsg, "Sorry, can not handle image with %s=%d",
	    photoTag, photometric);
	return (0);
    }
    return (1);
}

void
TIFFRGBAImageEnd(TIFFRGBAImage* img)
{
    if (img->Map)
	_TIFFfree(img->Map), img->Map = NULL;
    if (img->BWmap)
	_TIFFfree(img->BWmap), img->BWmap = NULL;
    if (img->PALmap)
	_TIFFfree(img->PALmap), img->PALmap = NULL;
    if (img->ycbcr)
	_TIFFfree(img->ycbcr), img->ycbcr = NULL;

    if( img->redcmap ) {
        _TIFFfree( img->redcmap );
        _TIFFfree( img->greencmap );
        _TIFFfree( img->bluecmap );
    }
}

static int
isCCITTCompression(TIFF* tif)
{
    uint16 compress;
    TIFFGetField(tif, TIFFTAG_COMPRESSION, &compress);
    return (compress == COMPRESSION_CCITTFAX3 ||
	    compress == COMPRESSION_CCITTFAX4 ||
	    compress == COMPRESSION_CCITTRLE ||
	    compress == COMPRESSION_CCITTRLEW);
}

int
TIFFRGBAImageBegin(TIFFRGBAImage* img, TIFF* tif, int stop, char emsg[1024])
{
    uint16* sampleinfo;
    uint16 extrasamples;
    uint16 planarconfig;
    uint16 compress;
    int colorchannels;
    uint16	*red_orig, *green_orig, *blue_orig;
    int		n_color;

    /* Initialize to normal values */
    img->row_offset = 0;
    img->col_offset = 0;
    img->redcmap = NULL;
    img->greencmap = NULL;
    img->bluecmap = NULL;
    
    img->tif = tif;
    img->stoponerr = stop;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &img->bitspersample);
    switch (img->bitspersample) {
    case 1: case 2: case 4:
    case 8: case 16:
	break;
    default:
	sprintf(emsg, "Sorry, can not image with %d-bit samples",
	    img->bitspersample);
	return (0);
    }
    img->alpha = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &img->samplesperpixel);
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES,
	&extrasamples, &sampleinfo);
    if (extrasamples == 1)
	switch (sampleinfo[0]) {
	case EXTRASAMPLE_ASSOCALPHA:	/* data is pre-multiplied */
	case EXTRASAMPLE_UNASSALPHA:	/* data is not pre-multiplied */
	    img->alpha = sampleinfo[0];
	    break;
	}
    colorchannels = img->samplesperpixel - extrasamples;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compress);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planarconfig);
    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &img->photometric)) {
	switch (colorchannels) {
	case 1:
	    if (isCCITTCompression(tif))
		img->photometric = PHOTOMETRIC_MINISWHITE;
	    else
		img->photometric = PHOTOMETRIC_MINISBLACK;
	    break;
	case 3:
	    img->photometric = PHOTOMETRIC_RGB;
	    break;
	default:
	    sprintf(emsg, "Missing needed %s tag", photoTag);
	    return (0);
	}
    }
    switch (img->photometric) {
    case PHOTOMETRIC_PALETTE:
	if (!TIFFGetField(tif, TIFFTAG_COLORMAP,
	    &red_orig, &green_orig, &blue_orig)) {
	    TIFFError(TIFFFileName(tif), "Missing required \"Colormap\" tag");
	    return (0);
	}

        /* copy the colormaps so we can modify them */
        n_color = (1L << img->bitspersample);
        img->redcmap = (uint16 *) _TIFFmalloc(sizeof(uint16)*n_color);
        img->greencmap = (uint16 *) _TIFFmalloc(sizeof(uint16)*n_color);
        img->bluecmap = (uint16 *) _TIFFmalloc(sizeof(uint16)*n_color);
        if( !img->redcmap || !img->greencmap || !img->bluecmap ) {
	    TIFFError(TIFFFileName(tif), "Out of memory for colormap copy");
	    return (0);
        }

        memcpy( img->redcmap, red_orig, n_color * 2 );
        memcpy( img->greencmap, green_orig, n_color * 2 );
        memcpy( img->bluecmap, blue_orig, n_color * 2 );
        
	/* fall thru... */
    case PHOTOMETRIC_MINISWHITE:
    case PHOTOMETRIC_MINISBLACK:
	if (planarconfig == PLANARCONFIG_CONTIG && img->samplesperpixel != 1) {
	    sprintf(emsg,
		"Sorry, can not handle contiguous data with %s=%d, and %s=%d",
		photoTag, img->photometric,
		"Samples/pixel", img->samplesperpixel);
	    return (0);
	}
	break;
    case PHOTOMETRIC_YCBCR:
	if (planarconfig != PLANARCONFIG_CONTIG) {
	    sprintf(emsg, "Sorry, can not handle YCbCr images with %s=%d",
		"Planarconfiguration", planarconfig);
	    return (0);
	}
	/* It would probably be nice to have a reality check here. */
	if (compress == COMPRESSION_JPEG && planarconfig == PLANARCONFIG_CONTIG) {
	    /* can rely on libjpeg to convert to RGB */
	    /* XXX should restore current state on exit */
	    TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
	    img->photometric = PHOTOMETRIC_RGB;
	}
	break;
    case PHOTOMETRIC_RGB: 
	if (colorchannels < 3) {
	    sprintf(emsg, "Sorry, can not handle RGB image with %s=%d",
		"Color channels", colorchannels);
	    return (0);
	}
	break;
    case PHOTOMETRIC_SEPARATED: {
	uint16 inkset;
	TIFFGetFieldDefaulted(tif, TIFFTAG_INKSET, &inkset);
	if (inkset != INKSET_CMYK) {
	    sprintf(emsg, "Sorry, can not handle separated image with %s=%d",
		"InkSet", inkset);
	    return (0);
	}
	if (img->samplesperpixel != 4) {
	    sprintf(emsg, "Sorry, can not handle separated image with %s=%d",
		"Samples/pixel", img->samplesperpixel);
	    return (0);
	}
	break;
    }
    case PHOTOMETRIC_LOGL:
	if (compress != COMPRESSION_SGILOG) {
	    sprintf(emsg, "Sorry, LogL data must have %s=%d",
		"Compression", COMPRESSION_SGILOG);
	    return (0);
	}
	TIFFSetField(tif, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT);
	img->photometric = PHOTOMETRIC_MINISBLACK;	/* little white lie */
	img->bitspersample = 8;
	break;
    case PHOTOMETRIC_LOGLUV:
	if (compress != COMPRESSION_SGILOG && compress != COMPRESSION_SGILOG24) {
	    sprintf(emsg, "Sorry, LogLuv data must have %s=%d or %d",
		"Compression", COMPRESSION_SGILOG, COMPRESSION_SGILOG24);
	    return (0);
	}
	if (planarconfig != PLANARCONFIG_CONTIG) {
	    sprintf(emsg, "Sorry, can not handle LogLuv images with %s=%d",
		"Planarconfiguration", planarconfig);
	    return (0);
	}
	TIFFSetField(tif, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT);
	img->photometric = PHOTOMETRIC_RGB;		/* little white lie */
	img->bitspersample = 8;
	break;
    default:
	sprintf(emsg, "Sorry, can not handle image with %s=%d",
	    photoTag, img->photometric);
	return (0);
    }
    img->Map = NULL;
    img->BWmap = NULL;
    img->PALmap = NULL;
    img->ycbcr = NULL;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &img->width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &img->height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &img->orientation);
    img->isContig =
	!(planarconfig == PLANARCONFIG_SEPARATE && colorchannels > 1);
    if (img->isContig) {
	img->get = TIFFIsTiled(tif) ? gtTileContig : gtStripContig;
	(void) pickTileContigCase(img);
    } else {
	img->get = TIFFIsTiled(tif) ? gtTileSeparate : gtStripSeparate;
	(void) pickTileSeparateCase(img);
    }
    return (1);
}

int
TIFFRGBAImageGet(TIFFRGBAImage* img, uint32* raster, uint32 w, uint32 h)
{
    if (img->get == NULL) {
	TIFFError(TIFFFileName(img->tif), "No \"get\" routine setup");
	return (0);
    }
    if (img->put.any == NULL) {
	TIFFError(TIFFFileName(img->tif),
	    "No \"put\" routine setupl; probably can not handle image format");
	return (0);
    }
    return (*img->get)(img, raster, w, h);
}

/*
 * Read the specified image into an ABGR-format raster.
 */
int
TIFFReadRGBAImage(TIFF* tif,
    uint32 rwidth, uint32 rheight, uint32* raster, int stop)
{
    char emsg[1024];
    TIFFRGBAImage img;
    int ok;

    if (TIFFRGBAImageBegin(&img, tif, stop, emsg)) {
	/* XXX verify rwidth and rheight against width and height */
	ok = TIFFRGBAImageGet(&img, raster+(rheight-img.height)*rwidth,
	    rwidth, img.height);
	TIFFRGBAImageEnd(&img);
    } else {
	TIFFError(TIFFFileName(tif), emsg);
	ok = 0;
    }
    return (ok);
}

static uint32
setorientation(TIFFRGBAImage* img, uint32 h)
{
    TIFF* tif = img->tif;
    uint32 y;

    switch (img->orientation) {
    case ORIENTATION_BOTRIGHT:
    case ORIENTATION_RIGHTBOT:	/* XXX */
    case ORIENTATION_LEFTBOT:	/* XXX */
	TIFFWarning(TIFFFileName(tif), "using bottom-left orientation");
	img->orientation = ORIENTATION_BOTLEFT;
	/* fall thru... */
    case ORIENTATION_BOTLEFT:
	y = 0;
	break;
    case ORIENTATION_TOPRIGHT:
    case ORIENTATION_RIGHTTOP:	/* XXX */
    case ORIENTATION_LEFTTOP:	/* XXX */
    default:
	TIFFWarning(TIFFFileName(tif), "using top-left orientation");
	img->orientation = ORIENTATION_TOPLEFT;
	/* fall thru... */
    case ORIENTATION_TOPLEFT:
	y = h-1;
	break;
    }
    return (y);
}

/*
 * Get an tile-organized image that has
 *	PlanarConfiguration contiguous if SamplesPerPixel > 1
 * or
 *	SamplesPerPixel == 1
 */	
static int
gtTileContig(TIFFRGBAImage* img, uint32* raster, uint32 w, uint32 h)
{
    TIFF* tif = img->tif;
    tileContigRoutine put = img->put.contig;
    uint16 orientation;
    uint32 col, row, y;
    uint32 tw, th;
    u_char* buf;
    int32 fromskew, toskew;
    uint32 nrow;

    buf = (u_char*) _TIFFmalloc(TIFFTileSize(tif));
    if (buf == 0) {
	TIFFError(TIFFFileName(tif), "No space for tile buffer");
	return (0);
    }
    TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
    y = setorientation(img, h);
    orientation = img->orientation;
    toskew = -(int32) (orientation == ORIENTATION_TOPLEFT ? tw+w : tw-w);
    for (row = 0; row < h; row += th) {
	nrow = (row + th > h ? h - row : th);
	for (col = 0; col < w; col += tw) {
	    if (TIFFReadTile(tif, buf, col+img->col_offset,
                             row+img->row_offset, 0, 0) < 0 && img->stoponerr)
		break;
	    if (col + tw > w) {
		/*
		 * Tile is clipped horizontally.  Calculate
		 * visible portion and skewing factors.
		 */
		uint32 npix = w - col;
		fromskew = tw - npix;
		(*put)(img, raster+y*w+col, col, y,
		    npix, nrow, fromskew, toskew + fromskew, buf);
	    } else {
		(*put)(img, raster+y*w+col, col, y, tw, nrow, 0, toskew, buf);
	    }
	}
	y += (orientation == ORIENTATION_TOPLEFT ?
	    -(int32) nrow : (int32) nrow);
    }
    _TIFFfree(buf);
    return (1);
}

/*
 * Get an tile-organized image that has
 *	 SamplesPerPixel > 1
 *	 PlanarConfiguration separated
 * We assume that all such images are RGB.
 */	
static int
gtTileSeparate(TIFFRGBAImage* img, uint32* raster, uint32 w, uint32 h)
{
    TIFF* tif = img->tif;
    tileSeparateRoutine put = img->put.separate;
    uint16 orientation;
    uint32 col, row, y;
    uint32 tw, th;
    u_char* buf;
    u_char* r;
    u_char* g;
    u_char* b;
    u_char* a;
    tsize_t tilesize;
    int32 fromskew, toskew;
    int alpha = img->alpha;
    uint32 nrow;

    tilesize = TIFFTileSize(tif);
    buf = (u_char*) _TIFFmalloc(4*tilesize);
    if (buf == 0) {
	TIFFError(TIFFFileName(tif), "No space for tile buffer");
	return (0);
    }
    r = buf;
    g = r + tilesize;
    b = g + tilesize;
    a = b + tilesize;
    if (!alpha)
	memset(a, 0xff, tilesize);
    TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
    y = setorientation(img, h);
    orientation = img->orientation;
    toskew = -(int32) (orientation == ORIENTATION_TOPLEFT ? tw+w : tw-w);
    for (row = 0; row < h; row += th) {
	nrow = (row + th > h ? h - row : th);
	for (col = 0; col < w; col += tw) {
	    if (TIFFReadTile(tif, r, col+img->col_offset,
                             row+img->row_offset,0,0) < 0 && img->stoponerr)
		break;
	    if (TIFFReadTile(tif, g, col+img->col_offset,
                             row+img->row_offset,0,1) < 0 && img->stoponerr)
		break;
	    if (TIFFReadTile(tif, b, col+img->col_offset,
                             row+img->row_offset,0,2) < 0 && img->stoponerr)
		break;
	    if (alpha && TIFFReadTile(tif,a,col+img->col_offset,
                               row+img->row_offset,0,3) < 0 && img->stoponerr)
		break;
	    if (col + tw > w) {
		/*
		 * Tile is clipped horizontally.  Calculate
		 * visible portion and skewing factors.
		 */
		uint32 npix = w - col;
		fromskew = tw - npix;
		(*put)(img, raster+y*w+col, col, y,
		    npix, nrow, fromskew, toskew + fromskew, r, g, b, a);
	    } else {
		(*put)(img, raster+y*w+col, col, y,
		    tw, nrow, 0, toskew, r, g, b, a);
	    }
	}
	y += (orientation == ORIENTATION_TOPLEFT ?
	    -(int32) nrow : (int32) nrow);
    }
    _TIFFfree(buf);
    return (1);
}

/*
 * Get a strip-organized image that has
 *	PlanarConfiguration contiguous if SamplesPerPixel > 1
 * or
 *	SamplesPerPixel == 1
 */	
static int
gtStripContig(TIFFRGBAImage* img, uint32* raster, uint32 w, uint32 h)
{
    TIFF* tif = img->tif;
    tileContigRoutine put = img->put.contig;
    uint16 orientation;
    uint32 row, y, nrow;
    u_char* buf;
    uint32 rowsperstrip;
    uint32 imagewidth = img->width;
    tsize_t scanline;
    int32 fromskew, toskew;

    buf = (u_char*) _TIFFmalloc(TIFFStripSize(tif));
    if (buf == 0) {
	TIFFError(TIFFFileName(tif), "No space for strip buffer");
	return (0);
    }
    y = setorientation(img, h);
    orientation = img->orientation;
    toskew = -(int32) (orientation == ORIENTATION_TOPLEFT ? w+w : w-w);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsperstrip);
    scanline = TIFFScanlineSize(tif);
    fromskew = (w < imagewidth ? imagewidth - w : 0);
    for (row = 0; row < h; row += rowsperstrip) {
	nrow = (row + rowsperstrip > h ? h - row : rowsperstrip);
	if (TIFFReadEncodedStrip(tif,
                                 TIFFComputeStrip(tif,row+img->row_offset, 0),
                                 buf, nrow*scanline) < 0
            && img->stoponerr)
		break;
	(*put)(img, raster+y*w, 0, y, w, nrow, fromskew, toskew, buf);
	y += (orientation == ORIENTATION_TOPLEFT ?
	    -(int32) nrow : (int32) nrow);
    }
    _TIFFfree(buf);
    return (1);
}

/*
 * Get a strip-organized image with
 *	 SamplesPerPixel > 1
 *	 PlanarConfiguration separated
 * We assume that all such images are RGB.
 */
static int
gtStripSeparate(TIFFRGBAImage* img, uint32* raster, uint32 w, uint32 h)
{
    TIFF* tif = img->tif;
    tileSeparateRoutine put = img->put.separate;
    uint16 orientation;
    u_char *buf;
    u_char *r, *g, *b, *a;
    uint32 row, y, nrow;
    tsize_t scanline;
    uint32 rowsperstrip, offset_row;
    uint32 imagewidth = img->width;
    tsize_t stripsize;
    int32 fromskew, toskew;
    int alpha = img->alpha;

    stripsize = TIFFStripSize(tif);
    r = buf = (u_char *)_TIFFmalloc(4*stripsize);
    if (buf == 0) {
	TIFFError(TIFFFileName(tif), "No space for tile buffer");
	return (0);
    }
    g = r + stripsize;
    b = g + stripsize;
    a = b + stripsize;
    if (!alpha)
	memset(a, 0xff, stripsize);
    y = setorientation(img, h);
    orientation = img->orientation;
    toskew = -(int32) (orientation == ORIENTATION_TOPLEFT ? w+w : w-w);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsperstrip);
    scanline = TIFFScanlineSize(tif);
    fromskew = (w < imagewidth ? imagewidth - w : 0);
    for (row = 0; row < h; row += rowsperstrip) {
	nrow = (row + rowsperstrip > h ? h - row : rowsperstrip);
        offset_row = row + img->row_offset;
	if (TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, offset_row, 0),
	    r, nrow*scanline) < 0 && img->stoponerr)
	    break;
	if (TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, offset_row, 1),
	    g, nrow*scanline) < 0 && img->stoponerr)
	    break;
	if (TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, offset_row, 2),
	    b, nrow*scanline) < 0 && img->stoponerr)
	    break;
	if (alpha &&
	    (TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, offset_row, 3),
	    a, nrow*scanline) < 0 && img->stoponerr))
	    break;
	(*put)(img, raster+y*w, 0, y, w, nrow, fromskew, toskew, r, g, b, a);
	y += (orientation == ORIENTATION_TOPLEFT ?
	    -(int32) nrow : (int32) nrow);
    }
    _TIFFfree(buf);
    return (1);
}

/*
 * The following routines move decoded data returned
 * from the TIFF library into rasters filled with packed
 * ABGR pixels (i.e. suitable for passing to lrecwrite.)
 *
 * The routines have been created according to the most
 * important cases and optimized.  pickTileContigCase and
 * pickTileSeparateCase analyze the parameters and select
 * the appropriate "put" routine to use.
 */
#define	REPEAT8(op)	REPEAT4(op); REPEAT4(op)
#define	REPEAT4(op)	REPEAT2(op); REPEAT2(op)
#define	REPEAT2(op)	op; op
#define	CASE8(x,op)			\
    switch (x) {			\
    case 7: op; case 6: op; case 5: op;	\
    case 4: op; case 3: op; case 2: op;	\
    case 1: op;				\
    }
#define	CASE4(x,op)	switch (x) { case 3: op; case 2: op; case 1: op; }
#define	NOP

#define	UNROLL8(w, op1, op2) {		\
    uint32 _x;				\
    for (_x = w; _x >= 8; _x -= 8) {	\
	op1;				\
	REPEAT8(op2);			\
    }					\
    if (_x > 0) {			\
	op1;				\
	CASE8(_x,op2);			\
    }					\
}
#define	UNROLL4(w, op1, op2) {		\
    uint32 _x;				\
    for (_x = w; _x >= 4; _x -= 4) {	\
	op1;				\
	REPEAT4(op2);			\
    }					\
    if (_x > 0) {			\
	op1;				\
	CASE4(_x,op2);			\
    }					\
}
#define	UNROLL2(w, op1, op2) {		\
    uint32 _x;				\
    for (_x = w; _x >= 2; _x -= 2) {	\
	op1;				\
	REPEAT2(op2);			\
    }					\
    if (_x) {				\
	op1;				\
	op2;				\
    }					\
}
    
#define	SKEW(r,g,b,skew)	{ r += skew; g += skew; b += skew; }
#define	SKEW4(r,g,b,a,skew)	{ r += skew; g += skew; b += skew; a+= skew; }

#define A1 ((uint32)(0xffL<<24))
#define	PACK(r,g,b)	\
	((uint32)(r)|((uint32)(g)<<8)|((uint32)(b)<<16)|A1)
#define	PACK4(r,g,b,a)	\
	((uint32)(r)|((uint32)(g)<<8)|((uint32)(b)<<16)|((uint32)(a)<<24))
#define W2B(v) (((v)>>8)&0xff)
#define	PACKW(r,g,b)	\
	((uint32)W2B(r)|((uint32)W2B(g)<<8)|((uint32)W2B(b)<<16)|A1)
#define	PACKW4(r,g,b,a)	\
	((uint32)W2B(r)|((uint32)W2B(g)<<8)|((uint32)W2B(b)<<16)|((uint32)W2B(a)<<24))

#define	DECLAREContigPutFunc(name) \
static void name(\
    TIFFRGBAImage* img, \
    uint32* cp, \
    uint32 x, uint32 y, \
    uint32 w, uint32 h, \
    int32 fromskew, int32 toskew, \
    u_char* pp \
)

/*
 * 8-bit palette => colormap/RGB
 */
DECLAREContigPutFunc(put8bitcmaptile)
{
    uint32** PALmap = img->PALmap;

    (void) x; (void) y;
    while (h-- > 0) {
	UNROLL8(w, NOP, *cp++ = PALmap[*pp++][0]);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 4-bit palette => colormap/RGB
 */
DECLAREContigPutFunc(put4bitcmaptile)
{
    uint32** PALmap = img->PALmap;

    (void) x; (void) y;
    fromskew /= 2;
    while (h-- > 0) {
	uint32* bw;
	UNROLL2(w, bw = PALmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 2-bit palette => colormap/RGB
 */
DECLAREContigPutFunc(put2bitcmaptile)
{
    uint32** PALmap = img->PALmap;

    (void) x; (void) y;
    fromskew /= 4;
    while (h-- > 0) {
	uint32* bw;
	UNROLL4(w, bw = PALmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 1-bit palette => colormap/RGB
 */
DECLAREContigPutFunc(put1bitcmaptile)
{
    uint32** PALmap = img->PALmap;

    (void) x; (void) y;
    fromskew /= 8;
    while (h-- > 0) {
	uint32* bw;
	UNROLL8(w, bw = PALmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 8-bit greyscale => colormap/RGB
 */
DECLAREContigPutFunc(putgreytile)
{
    uint32** BWmap = img->BWmap;

    (void) y;
    while (h-- > 0) {
	for (x = w; x-- > 0;)
	    *cp++ = BWmap[*pp++][0];
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 1-bit bilevel => colormap/RGB
 */
DECLAREContigPutFunc(put1bitbwtile)
{
    uint32** BWmap = img->BWmap;

    (void) x; (void) y;
    fromskew /= 8;
    while (h-- > 0) {
	uint32* bw;
	UNROLL8(w, bw = BWmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 2-bit greyscale => colormap/RGB
 */
DECLAREContigPutFunc(put2bitbwtile)
{
    uint32** BWmap = img->BWmap;

    (void) x; (void) y;
    fromskew /= 4;
    while (h-- > 0) {
	uint32* bw;
	UNROLL4(w, bw = BWmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 4-bit greyscale => colormap/RGB
 */
DECLAREContigPutFunc(put4bitbwtile)
{
    uint32** BWmap = img->BWmap;

    (void) x; (void) y;
    fromskew /= 2;
    while (h-- > 0) {
	uint32* bw;
	UNROLL2(w, bw = BWmap[*pp++], *cp++ = *bw++);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 8-bit packed samples, no Map => RGB
 */
DECLAREContigPutFunc(putRGBcontig8bittile)
{
    int samplesperpixel = img->samplesperpixel;

    (void) x; (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	UNROLL8(w, NOP,
	    *cp++ = PACK(pp[0], pp[1], pp[2]);
	    pp += samplesperpixel);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 8-bit packed samples, w/ Map => RGB
 */
DECLAREContigPutFunc(putRGBcontig8bitMaptile)
{
    TIFFRGBValue* Map = img->Map;
    int samplesperpixel = img->samplesperpixel;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	for (x = w; x-- > 0;) {
	    *cp++ = PACK(Map[pp[0]], Map[pp[1]], Map[pp[2]]);
	    pp += samplesperpixel;
	}
	pp += fromskew;
	cp += toskew;
    }
}

/*
 * 8-bit packed samples => RGBA w/ associated alpha
 * (known to have Map == NULL)
 */
DECLAREContigPutFunc(putRGBAAcontig8bittile)
{
    int samplesperpixel = img->samplesperpixel;

    (void) x; (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	UNROLL8(w, NOP,
	    *cp++ = PACK4(pp[0], pp[1], pp[2], pp[3]);
	    pp += samplesperpixel);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 8-bit packed samples => RGBA w/ unassociated alpha
 * (known to have Map == NULL)
 */
DECLAREContigPutFunc(putRGBUAcontig8bittile)
{
    int samplesperpixel = img->samplesperpixel;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	uint32 r, g, b, a;
	for (x = w; x-- > 0;) {
	    a = pp[3];
	    r = (pp[0] * a) / 255;
	    g = (pp[1] * a) / 255;
	    b = (pp[2] * a) / 255;
	    *cp++ = PACK4(r,g,b,a);
	    pp += samplesperpixel;
	}
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 16-bit packed samples => RGB
 */
DECLAREContigPutFunc(putRGBcontig16bittile)
{
    int samplesperpixel = img->samplesperpixel;
    uint16 *wp = (uint16 *)pp;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	for (x = w; x-- > 0;) {
	    *cp++ = PACKW(wp[0], wp[1], wp[2]);
	    wp += samplesperpixel;
	}
	cp += toskew;
	wp += fromskew;
    }
}

/*
 * 16-bit packed samples => RGBA w/ associated alpha
 * (known to have Map == NULL)
 */
DECLAREContigPutFunc(putRGBAAcontig16bittile)
{
    int samplesperpixel = img->samplesperpixel;
    uint16 *wp = (uint16 *)pp;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	for (x = w; x-- > 0;) {
	    *cp++ = PACKW4(wp[0], wp[1], wp[2], wp[3]);
	    wp += samplesperpixel;
	}
	cp += toskew;
	wp += fromskew;
    }
}

/*
 * 16-bit packed samples => RGBA w/ unassociated alpha
 * (known to have Map == NULL)
 */
DECLAREContigPutFunc(putRGBUAcontig16bittile)
{
    int samplesperpixel = img->samplesperpixel;
    uint16 *wp = (uint16 *)pp;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	uint32 r,g,b,a;
	/*
	 * We shift alpha down four bits just in case unsigned
	 * arithmetic doesn't handle the full range.
	 * We still have plenty of accuracy, since the output is 8 bits.
	 * So we have (r * 0xffff) * (a * 0xfff)) = r*a * (0xffff*0xfff)
	 * Since we want r*a * 0xff for eight bit output,
	 * we divide by (0xffff * 0xfff) / 0xff == 0x10eff.
	 */
	for (x = w; x-- > 0;) {
	    a = wp[3] >> 4; 
	    r = (wp[0] * a) / 0x10eff;
	    g = (wp[1] * a) / 0x10eff;
	    b = (wp[2] * a) / 0x10eff;
	    *cp++ = PACK4(r,g,b,a);
	    wp += samplesperpixel;
	}
	cp += toskew;
	wp += fromskew;
    }
}

/*
 * 8-bit packed CMYK samples w/o Map => RGB
 *
 * NB: The conversion of CMYK->RGB is *very* crude.
 */
DECLAREContigPutFunc(putRGBcontig8bitCMYKtile)
{
    int samplesperpixel = img->samplesperpixel;
    uint16 r, g, b, k;

    (void) x; (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	UNROLL8(w, NOP,
	    k = 255 - pp[3];
	    r = (k*(255-pp[0]))/255;
	    g = (k*(255-pp[1]))/255;
	    b = (k*(255-pp[2]))/255;
	    *cp++ = PACK(r, g, b);
	    pp += samplesperpixel);
	cp += toskew;
	pp += fromskew;
    }
}

/*
 * 8-bit packed CMYK samples w/Map => RGB
 *
 * NB: The conversion of CMYK->RGB is *very* crude.
 */
DECLAREContigPutFunc(putRGBcontig8bitCMYKMaptile)
{
    int samplesperpixel = img->samplesperpixel;
    TIFFRGBValue* Map = img->Map;
    uint16 r, g, b, k;

    (void) y;
    fromskew *= samplesperpixel;
    while (h-- > 0) {
	for (x = w; x-- > 0;) {
	    k = 255 - pp[3];
	    r = (k*(255-pp[0]))/255;
	    g = (k*(255-pp[1]))/255;
	    b = (k*(255-pp[2]))/255;
	    *cp++ = PACK(Map[r], Map[g], Map[b]);
	    pp += samplesperpixel;
	}
	pp += fromskew;
	cp += toskew;
    }
}

#define	DECLARESepPutFunc(name) \
static void name(\
    TIFFRGBAImage* img,\
    uint32* cp,\
    uint32 x, uint32 y, \
    uint32 w, uint32 h,\
    int32 fromskew, int32 toskew,\
    u_char* r, u_char* g, u_char* b, u_char* a\
)

/*
 * 8-bit unpacked samples => RGB
 */
DECLARESepPutFunc(putRGBseparate8bittile)
{
    (void) img; (void) x; (void) y; (void) a;
    while (h-- > 0) {
	UNROLL8(w, NOP, *cp++ = PACK(*r++, *g++, *b++));
	SKEW(r, g, b, fromskew);
	cp += toskew;
    }
}

/*
 * 8-bit unpacked samples => RGB
 */
DECLARESepPutFunc(putRGBseparate8bitMaptile)
{
    TIFFRGBValue* Map = img->Map;

    (void) y; (void) a;
    while (h-- > 0) {
	for (x = w; x > 0; x--)
	    *cp++ = PACK(Map[*r++], Map[*g++], Map[*b++]);
	SKEW(r, g, b, fromskew);
	cp += toskew;
    }
}

/*
 * 8-bit unpacked samples => RGBA w/ associated alpha
 */
DECLARESepPutFunc(putRGBAAseparate8bittile)
{
    (void) img; (void) x; (void) y;
    while (h-- > 0) {
	UNROLL8(w, NOP, *cp++ = PACK4(*r++, *g++, *b++, *a++));
	SKEW4(r, g, b, a, fromskew);
	cp += toskew;
    }
}

/*
 * 8-bit unpacked samples => RGBA w/ unassociated alpha
 */
DECLARESepPutFunc(putRGBUAseparate8bittile)
{
    (void) img; (void) y;
    while (h-- > 0) {
	uint32 rv, gv, bv, av;
	for (x = w; x-- > 0;) {
	    av = *a++;
	    rv = (*r++ * av) / 255;
	    gv = (*g++ * av) / 255;
	    bv = (*b++ * av) / 255;
	    *cp++ = PACK4(rv,gv,bv,av);
	}
	SKEW4(r, g, b, a, fromskew);
	cp += toskew;
    }
}

/*
 * 16-bit unpacked samples => RGB
 */
DECLARESepPutFunc(putRGBseparate16bittile)
{
    uint16 *wr = (uint16*) r;
    uint16 *wg = (uint16*) g;
    uint16 *wb = (uint16*) b;

    (void) img; (void) y; (void) a;
    while (h-- > 0) {
	for (x = 0; x < w; x++)
	    *cp++ = PACKW(*wr++, *wg++, *wb++);
	SKEW(wr, wg, wb, fromskew);
	cp += toskew;
    }
}

/*
 * 16-bit unpacked samples => RGBA w/ associated alpha
 */
DECLARESepPutFunc(putRGBAAseparate16bittile)
{
    uint16 *wr = (uint16*) r;
    uint16 *wg = (uint16*) g;
    uint16 *wb = (uint16*) b;
    uint16 *wa = (uint16*) a;

    (void) img; (void) y;
    while (h-- > 0) {
	for (x = 0; x < w; x++)
	    *cp++ = PACKW4(*wr++, *wg++, *wb++, *wa++);
	SKEW4(wr, wg, wb, wa, fromskew);
	cp += toskew;
    }
}

/*
 * 16-bit unpacked samples => RGBA w/ unassociated alpha
 */
DECLARESepPutFunc(putRGBUAseparate16bittile)
{
    uint16 *wr = (uint16*) r;
    uint16 *wg = (uint16*) g;
    uint16 *wb = (uint16*) b;
    uint16 *wa = (uint16*) a;

    (void) img; (void) y;
    while (h-- > 0) {
	uint32 r,g,b,a;
	/*
	 * We shift alpha down four bits just in case unsigned
	 * arithmetic doesn't handle the full range.
	 * We still have plenty of accuracy, since the output is 8 bits.
	 * So we have (r * 0xffff) * (a * 0xfff)) = r*a * (0xffff*0xfff)
	 * Since we want r*a * 0xff for eight bit output,
	 * we divide by (0xffff * 0xfff) / 0xff == 0x10eff.
	 */
	for (x = w; x-- > 0;) {
	    a = *wa++ >> 4; 
	    r = (*wr++ * a) / 0x10eff;
	    g = (*wg++ * a) / 0x10eff;
	    b = (*wb++ * a) / 0x10eff;
	    *cp++ = PACK4(r,g,b,a);
	}
	SKEW4(wr, wg, wb, wa, fromskew);
	cp += toskew;
    }
}

/*
 * YCbCr -> RGB conversion and packing routines.  The colorspace
 * conversion algorithm comes from the IJG v5a code; see below
 * for more information on how it works.
 */

#define	YCbCrtoRGB(dst, yc) {						\
    int Y = (yc);							\
    dst = PACK(								\
	clamptab[Y+Crrtab[Cr]],						\
	clamptab[Y + (int)((Cbgtab[Cb]+Crgtab[Cr])>>16)],		\
	clamptab[Y+Cbbtab[Cb]]);					\
}
#define	YCbCrSetup							\
    TIFFYCbCrToRGB* ycbcr = img->ycbcr;					\
    int* Crrtab = ycbcr->Cr_r_tab;					\
    int* Cbbtab = ycbcr->Cb_b_tab;					\
    int32* Crgtab = ycbcr->Cr_g_tab;					\
    int32* Cbgtab = ycbcr->Cb_g_tab;					\
    TIFFRGBValue* clamptab = ycbcr->clamptab

/*
 * 8-bit packed YCbCr samples w/ 4,4 subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr44tile)
{
    YCbCrSetup;
    uint32* cp1 = cp+w+toskew;
    uint32* cp2 = cp1+w+toskew;
    uint32* cp3 = cp2+w+toskew;
    int32 incr = 3*w+4*toskew;

    (void) y;
    /* XXX adjust fromskew */
    for (; h >= 4; h -= 4) {
	x = w>>2;
	do {
	    int Cb = pp[16];
	    int Cr = pp[17];

	    YCbCrtoRGB(cp [0], pp[ 0]);
	    YCbCrtoRGB(cp [1], pp[ 1]);
	    YCbCrtoRGB(cp [2], pp[ 2]);
	    YCbCrtoRGB(cp [3], pp[ 3]);
	    YCbCrtoRGB(cp1[0], pp[ 4]);
	    YCbCrtoRGB(cp1[1], pp[ 5]);
	    YCbCrtoRGB(cp1[2], pp[ 6]);
	    YCbCrtoRGB(cp1[3], pp[ 7]);
	    YCbCrtoRGB(cp2[0], pp[ 8]);
	    YCbCrtoRGB(cp2[1], pp[ 9]);
	    YCbCrtoRGB(cp2[2], pp[10]);
	    YCbCrtoRGB(cp2[3], pp[11]);
	    YCbCrtoRGB(cp3[0], pp[12]);
	    YCbCrtoRGB(cp3[1], pp[13]);
	    YCbCrtoRGB(cp3[2], pp[14]);
	    YCbCrtoRGB(cp3[3], pp[15]);

	    cp += 4, cp1 += 4, cp2 += 4, cp3 += 4;
	    pp += 18;
	} while (--x);
	cp += incr, cp1 += incr, cp2 += incr, cp3 += incr;
	pp += fromskew;
    }
}

/*
 * 8-bit packed YCbCr samples w/ 4,2 subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr42tile)
{
    YCbCrSetup;
    uint32* cp1 = cp+w+toskew;
    int32 incr = 2*toskew+w;

    (void) y;
    /* XXX adjust fromskew */
    for (; h >= 2; h -= 2) {
	x = w>>2;
	do {
	    int Cb = pp[8];
	    int Cr = pp[9];

	    YCbCrtoRGB(cp [0], pp[0]);
	    YCbCrtoRGB(cp [1], pp[1]);
	    YCbCrtoRGB(cp [2], pp[2]);
	    YCbCrtoRGB(cp [3], pp[3]);
	    YCbCrtoRGB(cp1[0], pp[4]);
	    YCbCrtoRGB(cp1[1], pp[5]);
	    YCbCrtoRGB(cp1[2], pp[6]);
	    YCbCrtoRGB(cp1[3], pp[7]);

	    cp += 4, cp1 += 4;
	    pp += 10;
	} while (--x);
	cp += incr, cp1 += incr;
	pp += fromskew;
    }
}

/*
 * 8-bit packed YCbCr samples w/ 4,1 subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr41tile)
{
    YCbCrSetup;

    (void) y;
    /* XXX adjust fromskew */
    do {
	x = w>>2;
	do {
	    int Cb = pp[4];
	    int Cr = pp[5];

	    YCbCrtoRGB(cp [0], pp[0]);
	    YCbCrtoRGB(cp [1], pp[1]);
	    YCbCrtoRGB(cp [2], pp[2]);
	    YCbCrtoRGB(cp [3], pp[3]);

	    cp += 4;
	    pp += 6;
	} while (--x);
	cp += toskew;
	pp += fromskew;
    } while (--h);
}

/*
 * 8-bit packed YCbCr samples w/ 2,2 subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr22tile)
{
    YCbCrSetup;
    uint32* cp1 = cp+w+toskew;
    int32 incr = 2*toskew+w;

    (void) y;
    /* XXX adjust fromskew */
    for (; h >= 2; h -= 2) {
	x = w>>1;
	do {
	    int Cb = pp[4];
	    int Cr = pp[5];

	    YCbCrtoRGB(cp [0], pp[0]);
	    YCbCrtoRGB(cp [1], pp[1]);
	    YCbCrtoRGB(cp1[0], pp[2]);
	    YCbCrtoRGB(cp1[1], pp[3]);

	    cp += 2, cp1 += 2;
	    pp += 6;
	} while (--x);
	cp += incr, cp1 += incr;
	pp += fromskew;
    }
}

/*
 * 8-bit packed YCbCr samples w/ 2,1 subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr21tile)
{
    YCbCrSetup;

    (void) y;
    /* XXX adjust fromskew */
    do {
	x = w>>1;
	do {
	    int Cb = pp[2];
	    int Cr = pp[3];

	    YCbCrtoRGB(cp[0], pp[0]);
	    YCbCrtoRGB(cp[1], pp[1]);

	    cp += 2;
	    pp += 4;
	} while (--x);
	cp += toskew;
	pp += fromskew;
    } while (--h);
}

/*
 * 8-bit packed YCbCr samples w/ no subsampling => RGB
 */
DECLAREContigPutFunc(putcontig8bitYCbCr11tile)
{
    YCbCrSetup;

    (void) y;
    /* XXX adjust fromskew */
    do {
	x = w>>1;
	do {
	    int Cb = pp[1];
	    int Cr = pp[2];

	    YCbCrtoRGB(*cp++, pp[0]);

	    pp += 3;
	} while (--x);
	cp += toskew;
	pp += fromskew;
    } while (--h);
}
#undef	YCbCrSetup
#undef	YCbCrtoRGB

#define	LumaRed			coeffs[0]
#define	LumaGreen		coeffs[1]
#define	LumaBlue		coeffs[2]
#define	SHIFT			16
#define	FIX(x)			((int32)((x) * (1L<<SHIFT) + 0.5))
#define	ONE_HALF		((int32)(1<<(SHIFT-1)))

/*
 * Initialize the YCbCr->RGB conversion tables.  The conversion
 * is done according to the 6.0 spec:
 *
 *    R = Y + Cr*(2 - 2*LumaRed)
 *    B = Y + Cb*(2 - 2*LumaBlue)
 *    G =   Y
 *        - LumaBlue*Cb*(2-2*LumaBlue)/LumaGreen
 *        - LumaRed*Cr*(2-2*LumaRed)/LumaGreen
 *
 * To avoid floating point arithmetic the fractional constants that
 * come out of the equations are represented as fixed point values
 * in the range 0...2^16.  We also eliminate multiplications by
 * pre-calculating possible values indexed by Cb and Cr (this code
 * assumes conversion is being done for 8-bit samples).
 */
static void
TIFFYCbCrToRGBInit(TIFFYCbCrToRGB* ycbcr, TIFF* tif)
{
    TIFFRGBValue* clamptab;
    float* coeffs;
    int i;

    clamptab = (TIFFRGBValue*)(
	(tidata_t) ycbcr+TIFFroundup(sizeof (TIFFYCbCrToRGB), sizeof (long)));
    _TIFFmemset(clamptab, 0, 256);		/* v < 0 => 0 */
    ycbcr->clamptab = (clamptab += 256);
    for (i = 0; i < 256; i++)
	clamptab[i] = i;
    _TIFFmemset(clamptab+256, 255, 2*256);	/* v > 255 => 255 */
    TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRCOEFFICIENTS, &coeffs);
    _TIFFmemcpy(ycbcr->coeffs, coeffs, 3*sizeof (float));
    { float f1 = 2-2*LumaRed;		int32 D1 = FIX(f1);
      float f2 = LumaRed*f1/LumaGreen;	int32 D2 = -FIX(f2);
      float f3 = 2-2*LumaBlue;		int32 D3 = FIX(f3);
      float f4 = LumaBlue*f3/LumaGreen;	int32 D4 = -FIX(f4);
      int x;

      ycbcr->Cr_r_tab = (int*) (clamptab + 3*256);
      ycbcr->Cb_b_tab = ycbcr->Cr_r_tab + 256;
      ycbcr->Cr_g_tab = (int32*) (ycbcr->Cb_b_tab + 256);
      ycbcr->Cb_g_tab = ycbcr->Cr_g_tab + 256;
      /*
       * i is the actual input pixel value in the range 0..255
       * Cb and Cr values are in the range -128..127 (actually
       * they are in a range defined by the ReferenceBlackWhite
       * tag) so there is some range shifting to do here when
       * constructing tables indexed by the raw pixel data.
       *
       * XXX handle ReferenceBlackWhite correctly to calculate
       *     Cb/Cr values to use in constructing the tables.
       */
      for (i = 0, x = -128; i < 256; i++, x++) {
	  ycbcr->Cr_r_tab[i] = (int)((D1*x + ONE_HALF)>>SHIFT);
	  ycbcr->Cb_b_tab[i] = (int)((D3*x + ONE_HALF)>>SHIFT);
	  ycbcr->Cr_g_tab[i] = D2*x;
	  ycbcr->Cb_g_tab[i] = D4*x + ONE_HALF;
      }
    }
}
#undef	SHIFT
#undef	ONE_HALF
#undef	FIX
#undef	LumaBlue
#undef	LumaGreen
#undef	LumaRed

static tileContigRoutine
initYCbCrConversion(TIFFRGBAImage* img)
{
    uint16 hs, vs;

    if (img->ycbcr == NULL) {
	img->ycbcr = (TIFFYCbCrToRGB*) _TIFFmalloc(
	      TIFFroundup(sizeof (TIFFYCbCrToRGB), sizeof (long))
	    + 4*256*sizeof (TIFFRGBValue)
	    + 2*256*sizeof (int)
	    + 2*256*sizeof (int32)
	);
	if (img->ycbcr == NULL) {
	    TIFFError(TIFFFileName(img->tif),
		"No space for YCbCr->RGB conversion state");
	    return (NULL);
	}
	TIFFYCbCrToRGBInit(img->ycbcr, img->tif);
    } else {
	float* coeffs;

	TIFFGetFieldDefaulted(img->tif, TIFFTAG_YCBCRCOEFFICIENTS, &coeffs);
	if (_TIFFmemcmp(coeffs, img->ycbcr->coeffs, 3*sizeof (float)) != 0)
	    TIFFYCbCrToRGBInit(img->ycbcr, img->tif);
    }
    /*
     * The 6.0 spec says that subsampling must be
     * one of 1, 2, or 4, and that vertical subsampling
     * must always be <= horizontal subsampling; so
     * there are only a few possibilities and we just
     * enumerate the cases.
     */
    TIFFGetFieldDefaulted(img->tif, TIFFTAG_YCBCRSUBSAMPLING, &hs, &vs);
    switch ((hs<<4)|vs) {
    case 0x44: return (putcontig8bitYCbCr44tile);
    case 0x42: return (putcontig8bitYCbCr42tile);
    case 0x41: return (putcontig8bitYCbCr41tile);
    case 0x22: return (putcontig8bitYCbCr22tile);
    case 0x21: return (putcontig8bitYCbCr21tile);
    case 0x11: return (putcontig8bitYCbCr11tile);
    }
    return (NULL);
}

/*
 * Greyscale images with less than 8 bits/sample are handled
 * with a table to avoid lots of shifts and masks.  The table
 * is setup so that put*bwtile (below) can retrieve 8/bitspersample
 * pixel values simply by indexing into the table with one
 * number.
 */
static int
makebwmap(TIFFRGBAImage* img)
{
    TIFFRGBValue* Map = img->Map;
    int bitspersample = img->bitspersample;
    int nsamples = 8 / bitspersample;
    int i;
    uint32* p;

    img->BWmap = (uint32**) _TIFFmalloc(
	256*sizeof (uint32 *)+(256*nsamples*sizeof(uint32)));
    if (img->BWmap == NULL) {
	TIFFError(TIFFFileName(img->tif), "No space for B&W mapping table");
	return (0);
    }
    p = (uint32*)(img->BWmap + 256);
    for (i = 0; i < 256; i++) {
	TIFFRGBValue c;
	img->BWmap[i] = p;
	switch (bitspersample) {
#define	GREY(x)	c = Map[x]; *p++ = PACK(c,c,c);
	case 1:
	    GREY(i>>7);
	    GREY((i>>6)&1);
	    GREY((i>>5)&1);
	    GREY((i>>4)&1);
	    GREY((i>>3)&1);
	    GREY((i>>2)&1);
	    GREY((i>>1)&1);
	    GREY(i&1);
	    break;
	case 2:
	    GREY(i>>6);
	    GREY((i>>4)&3);
	    GREY((i>>2)&3);
	    GREY(i&3);
	    break;
	case 4:
	    GREY(i>>4);
	    GREY(i&0xf);
	    break;
	case 8:
	    GREY(i);
	    break;
	}
#undef	GREY
    }
    return (1);
}

/*
 * Construct a mapping table to convert from the range
 * of the data samples to [0,255] --for display.  This
 * process also handles inverting B&W images when needed.
 */ 
static int
setupMap(TIFFRGBAImage* img)
{
    int32 x, range;

    range = (int32)((1L<<img->bitspersample)-1);
    img->Map = (TIFFRGBValue*) _TIFFmalloc((range+1) * sizeof (TIFFRGBValue));
    if (img->Map == NULL) {
	TIFFError(TIFFFileName(img->tif),
	    "No space for photometric conversion table");
	return (0);
    }
    if (img->photometric == PHOTOMETRIC_MINISWHITE) {
	for (x = 0; x <= range; x++)
	    img->Map[x] = (TIFFRGBValue) (((range - x) * 255) / range);
    } else {
	for (x = 0; x <= range; x++)
	    img->Map[x] = (TIFFRGBValue) ((x * 255) / range);
    }
    if (img->bitspersample <= 8 &&
	(img->photometric == PHOTOMETRIC_MINISBLACK ||
	 img->photometric == PHOTOMETRIC_MINISWHITE)) {
	/*
	 * Use photometric mapping table to construct
	 * unpacking tables for samples <= 8 bits.
	 */
	if (!makebwmap(img))
	    return (0);
	/* no longer need Map, free it */
	_TIFFfree(img->Map), img->Map = NULL;
    }
    return (1);
}

static int
checkcmap(TIFFRGBAImage* img)
{
    uint16* r = img->redcmap;
    uint16* g = img->greencmap;
    uint16* b = img->bluecmap;
    long n = 1L<<img->bitspersample;

    while (n-- > 0)
	if (*r++ >= 256 || *g++ >= 256 || *b++ >= 256)
	    return (16);
    return (8);
}

static void
cvtcmap(TIFFRGBAImage* img)
{
    uint16* r = img->redcmap;
    uint16* g = img->greencmap;
    uint16* b = img->bluecmap;
    long i;

    for (i = (1L<<img->bitspersample)-1; i >= 0; i--) {
#define	CVT(x)		((uint16)((x)>>8))
	r[i] = CVT(r[i]);
	g[i] = CVT(g[i]);
	b[i] = CVT(b[i]);
#undef	CVT
    }
}

/*
 * Palette images with <= 8 bits/sample are handled
 * with a table to avoid lots of shifts and masks.  The table
 * is setup so that put*cmaptile (below) can retrieve 8/bitspersample
 * pixel values simply by indexing into the table with one
 * number.
 */
static int
makecmap(TIFFRGBAImage* img)
{
    int bitspersample = img->bitspersample;
    int nsamples = 8 / bitspersample;
    uint16* r = img->redcmap;
    uint16* g = img->greencmap;
    uint16* b = img->bluecmap;
    uint32 *p;
    int i;

    img->PALmap = (uint32**) _TIFFmalloc(
	256*sizeof (uint32 *)+(256*nsamples*sizeof(uint32)));
    if (img->PALmap == NULL) {
	TIFFError(TIFFFileName(img->tif), "No space for Palette mapping table");
	return (0);
    }
    p = (uint32*)(img->PALmap + 256);
    for (i = 0; i < 256; i++) {
	TIFFRGBValue c;
	img->PALmap[i] = p;
#define	CMAP(x)	c = x; *p++ = PACK(r[c]&0xff, g[c]&0xff, b[c]&0xff);
	switch (bitspersample) {
	case 1:
	    CMAP(i>>7);
	    CMAP((i>>6)&1);
	    CMAP((i>>5)&1);
	    CMAP((i>>4)&1);
	    CMAP((i>>3)&1);
	    CMAP((i>>2)&1);
	    CMAP((i>>1)&1);
	    CMAP(i&1);
	    break;
	case 2:
	    CMAP(i>>6);
	    CMAP((i>>4)&3);
	    CMAP((i>>2)&3);
	    CMAP(i&3);
	    break;
	case 4:
	    CMAP(i>>4);
	    CMAP(i&0xf);
	    break;
	case 8:
	    CMAP(i);
	    break;
	}
#undef CMAP
    }
    return (1);
}

/* 
 * Construct any mapping table used
 * by the associated put routine.
 */
static int
buildMap(TIFFRGBAImage* img)
{
    switch (img->photometric) {
    case PHOTOMETRIC_RGB:
    case PHOTOMETRIC_YCBCR:
    case PHOTOMETRIC_SEPARATED:
	if (img->bitspersample == 8)
	    break;
	/* fall thru... */
    case PHOTOMETRIC_MINISBLACK:
    case PHOTOMETRIC_MINISWHITE:
	if (!setupMap(img))
	    return (0);
	break;
    case PHOTOMETRIC_PALETTE:
	/*
	 * Convert 16-bit colormap to 8-bit (unless it looks
	 * like an old-style 8-bit colormap).
	 */
	if (checkcmap(img) == 16)
	    cvtcmap(img);
	else
	    TIFFWarning(TIFFFileName(img->tif), "Assuming 8-bit colormap");
	/*
	 * Use mapping table and colormap to construct
	 * unpacking tables for samples < 8 bits.
	 */
	if (img->bitspersample <= 8 && !makecmap(img))
	    return (0);
	break;
    }
    return (1);
}

/*
 * Select the appropriate conversion routine for packed data.
 */
static int
pickTileContigCase(TIFFRGBAImage* img)
{
    tileContigRoutine put = 0;

    if (buildMap(img)) {
	switch (img->photometric) {
	case PHOTOMETRIC_RGB:
	    switch (img->bitspersample) {
	    case 8:
		if (!img->Map) {
		    if (img->alpha == EXTRASAMPLE_ASSOCALPHA)
			put = putRGBAAcontig8bittile;
		    else if (img->alpha == EXTRASAMPLE_UNASSALPHA)
			put = putRGBUAcontig8bittile;
		    else
			put = putRGBcontig8bittile;
		} else
		    put = putRGBcontig8bitMaptile;
		break;
	    case 16:
		put = putRGBcontig16bittile;
		if (!img->Map) {
		    if (img->alpha == EXTRASAMPLE_ASSOCALPHA)
			put = putRGBAAcontig16bittile;
		    else if (img->alpha == EXTRASAMPLE_UNASSALPHA)
			put = putRGBUAcontig16bittile;
		}
		break;
	    }
	    break;
	case PHOTOMETRIC_SEPARATED:
	    if (img->bitspersample == 8) {
		if (!img->Map)
		    put = putRGBcontig8bitCMYKtile;
		else
		    put = putRGBcontig8bitCMYKMaptile;
	    }
	    break;
	case PHOTOMETRIC_PALETTE:
	    switch (img->bitspersample) {
	    case 8:	put = put8bitcmaptile; break;
	    case 4: put = put4bitcmaptile; break;
	    case 2: put = put2bitcmaptile; break;
	    case 1: put = put1bitcmaptile; break;
	    }
	    break;
	case PHOTOMETRIC_MINISWHITE:
	case PHOTOMETRIC_MINISBLACK:
	    switch (img->bitspersample) {
	    case 8:	put = putgreytile; break;
	    case 4: put = put4bitbwtile; break;
	    case 2: put = put2bitbwtile; break;
	    case 1: put = put1bitbwtile; break;
	    }
	    break;
	case PHOTOMETRIC_YCBCR:
	    if (img->bitspersample == 8)
		put = initYCbCrConversion(img);
	    break;
	}
    }
    return ((img->put.contig = put) != 0);
}

/*
 * Select the appropriate conversion routine for unpacked data.
 *
 * NB: we assume that unpacked single channel data is directed
 *	 to the "packed routines.
 */
static int
pickTileSeparateCase(TIFFRGBAImage* img)
{
    tileSeparateRoutine put = 0;

    if (buildMap(img)) {
	switch (img->photometric) {
	case PHOTOMETRIC_RGB:
	    switch (img->bitspersample) {
	    case 8:
		if (!img->Map) {
		    if (img->alpha == EXTRASAMPLE_ASSOCALPHA)
			put = putRGBAAseparate8bittile;
		    else if (img->alpha == EXTRASAMPLE_UNASSALPHA)
			put = putRGBUAseparate8bittile;
		    else
			put = putRGBseparate8bittile;
		} else
		    put = putRGBseparate8bitMaptile;
		break;
	    case 16:
		put = putRGBseparate16bittile;
		if (!img->Map) {
		    if (img->alpha == EXTRASAMPLE_ASSOCALPHA)
			put = putRGBAAseparate16bittile;
		    else if (img->alpha == EXTRASAMPLE_UNASSALPHA)
			put = putRGBUAseparate16bittile;
		}
		break;
	    }
	    break;
	}
    }
    return ((img->put.separate = put) != 0);
}

/*
 * Read a whole strip off data from the file, and convert to RGBA form.
 * If this is the last strip, then it will only contain the portion of
 * the strip that is actually within the image space.  The result is
 * organized in bottom to top form.
 */


int
TIFFReadRGBAStrip(TIFF* tif, uint32 row, uint32 * raster )

{
    char 	emsg[1024];
    TIFFRGBAImage img;
    int 	ok;
    uint32	rowsperstrip, rows_to_read;

    if( TIFFIsTiled( tif ) )
    {
        TIFFError(TIFFFileName(tif),
                  "Can't use TIFFReadRGBAStrip() with tiled file.");
	return (0);
    }
    
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsperstrip);
    if( (row % rowsperstrip) != 0 )
    {
        TIFFError(TIFFFileName(tif),
                "Row passed to TIFFReadRGBAStrip() must be first in a strip.");
	return (0);
    }

    if (TIFFRGBAImageBegin(&img, tif, 0, emsg)) {

        img.row_offset = row;
        img.col_offset = 0;

        if( row + rowsperstrip > img.height )
            rows_to_read = img.height - row;
        else
            rows_to_read = rowsperstrip;
        
	ok = TIFFRGBAImageGet(&img, raster, img.width, rows_to_read );
        
	TIFFRGBAImageEnd(&img);
    } else {
	TIFFError(TIFFFileName(tif), emsg);
	ok = 0;
    }
    
    return (ok);
}

/*
 * Read a whole tile off data from the file, and convert to RGBA form.
 * The returned RGBA data is organized from bottom to top of tile,
 * and may include zeroed areas if the tile extends off the image.
 */

int
TIFFReadRGBATile(TIFF* tif, uint32 col, uint32 row, uint32 * raster)

{
    char 	emsg[1024];
    TIFFRGBAImage img;
    int 	ok;
    uint32	tile_xsize, tile_ysize;
    uint32	read_xsize, read_ysize;
    uint32	i_row;

    /*
     * Verify that our request is legal - on a tile file, and on a
     * tile boundary.
     */
    
    if( !TIFFIsTiled( tif ) )
    {
        TIFFError(TIFFFileName(tif),
                  "Can't use TIFFReadRGBATile() with stripped file.");
	return (0);
    }
    
    TIFFGetFieldDefaulted(tif, TIFFTAG_TILEWIDTH, &tile_xsize);
    TIFFGetFieldDefaulted(tif, TIFFTAG_TILELENGTH, &tile_ysize);
    if( (col % tile_xsize) != 0 || (row % tile_ysize) != 0 )
    {
        TIFFError(TIFFFileName(tif),
                  "Row/col passed to TIFFReadRGBATile() must be top"
                  "left corner of a tile.");
	return (0);
    }

    /*
     * Setup the RGBA reader.
     */
    
    if ( !TIFFRGBAImageBegin(&img, tif, 0, emsg)) {
	TIFFError(TIFFFileName(tif), emsg);
        return( 0 );
    }

    /*
     * The TIFFRGBAImageGet() function doesn't allow us to get off the
     * edge of the image, even to fill an otherwise valid tile.  So we
     * figure out how much we can read, and fix up the tile buffer to
     * a full tile configuration afterwards.
     */

    if( row + tile_ysize > img.height )
        read_ysize = img.height - row;
    else
        read_ysize = tile_ysize;
    
    if( col + tile_xsize > img.width )
        read_xsize = img.width - col;
    else
        read_xsize = tile_xsize;

    /*
     * Read the chunk of imagery.
     */
    
    img.row_offset = row;
    img.col_offset = col;

    ok = TIFFRGBAImageGet(&img, raster, read_xsize, read_ysize );
        
    TIFFRGBAImageEnd(&img);

    /*
     * If our read was incomplete we will need to fix up the tile by
     * shifting the data around as if a full tile of data is being returned.
     *
     * This is all the more complicated because the image is organized in
     * bottom to top format. 
     */

    if( read_xsize == tile_xsize && read_ysize == tile_ysize )
        return( ok );

    for( i_row = 0; i_row < read_ysize; i_row++ )
    {
        _TIFFmemcpy( raster + (tile_ysize - i_row - 1) * tile_xsize,
                     raster + (read_ysize - i_row - 1) * read_xsize,
                     read_xsize * sizeof(uint32) );
        _TIFFmemset( raster + (tile_ysize - i_row - 1) * tile_xsize+read_xsize,
                     0, sizeof(uint32) * (tile_xsize - read_xsize) );
    }

    for( i_row = read_ysize; i_row < tile_ysize; i_row++ )
    {
        _TIFFmemset( raster + (tile_ysize - i_row - 1) * tile_xsize,
                     0, sizeof(uint32) * tile_xsize );
    }
    
    return (ok);
}


/* ==== tif_jpeg.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_jpeg.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1994-1997 Sam Leffler
 * Copyright (c) 1994-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef JPEG_SUPPORT
/*
 * TIFF Library
 *
 * JPEG Compression support per TIFF Technical Note #2
 * (*not* per the original TIFF 6.0 spec).
 *
 * This file is simply an interface to the libjpeg library written by
 * the Independent JPEG Group.  You need release 5 or later of the IJG
 * code, which you can find on the Internet at ftp.uu.net:/graphics/jpeg/.
 *
 * Contributed by Tom Lane <tgl@sss.pgh.pa.us>.
 */
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>

/* We undefine FAR to avoid conflict with JPEG definition */

#ifdef FAR
#undef FAR
#endif

#include "jpeglib.h"
#include "jerror.h"

/*
 * On some machines it may be worthwhile to use _setjmp or sigsetjmp
 * in place of plain setjmp.  These macros will make it easier.
 */
#define SETJMP(jbuf)		setjmp(jbuf)
#define LONGJMP(jbuf,code)	longjmp(jbuf,code)
#define JMP_BUF			jmp_buf

typedef struct jpeg_destination_mgr jpeg_destination_mgr;
typedef struct jpeg_source_mgr jpeg_source_mgr;
typedef	struct jpeg_error_mgr jpeg_error_mgr;

/*
 * State block for each open TIFF file using
 * libjpeg to do JPEG compression/decompression.
 *
 * libjpeg's visible state is either a jpeg_compress_struct
 * or jpeg_decompress_struct depending on which way we
 * are going.  comm can be used to refer to the fields
 * which are common to both.
 *
 * NB: cinfo is required to be the first member of JPEGState,
 *     so we can safely cast JPEGState* -> jpeg_xxx_struct*
 *     and vice versa!
 */
typedef	struct {
	union {
		struct jpeg_compress_struct c;
		struct jpeg_decompress_struct d;
		struct jpeg_common_struct comm;
	} cinfo;			/* NB: must be first */
	jpeg_error_mgr	err;		/* libjpeg error manager */
	JMP_BUF		exit_jmpbuf;	/* for catching libjpeg failures */
	/*
	 * The following two members could be a union, but
	 * they're small enough that it's not worth the effort.
	 */
	jpeg_destination_mgr dest;	/* data dest for compression */
	jpeg_source_mgr	src;		/* data source for decompression */
					/* private state */
	TIFF*		tif;		/* back link needed by some code */
	uint16		photometric;	/* copy of PhotometricInterpretation */
	uint16		h_sampling;	/* luminance sampling factors */
	uint16		v_sampling;
	tsize_t		bytesperline;	/* decompressed bytes per scanline */
	/* pointers to intermediate buffers when processing downsampled data */
	JSAMPARRAY	ds_buffer[MAX_COMPONENTS];
	int		scancount;	/* number of "scanlines" accumulated */
	int		samplesperclump;

	TIFFVGetMethod	vgetparent;	/* super-class method */
	TIFFVSetMethod	vsetparent;	/* super-class method */
	TIFFStripMethod	defsparent;	/* super-class method */
	TIFFTileMethod	deftparent;	/* super-class method */
					/* pseudo-tag fields */
	void*		jpegtables;	/* JPEGTables tag value, or NULL */
	uint32		jpegtables_length; /* number of bytes in same */
	int		jpegquality;	/* Compression quality level */
	int		jpegcolormode;	/* Auto RGB<=>YCbCr convert? */
	int		jpegtablesmode;	/* What to put in JPEGTables */
} JPEGState;

#define	JState(tif)	((JPEGState*)(tif)->tif_data)

static	int JPEGDecode(TIFF*, tidata_t, tsize_t, tsample_t);
static	int JPEGDecodeRaw(TIFF*, tidata_t, tsize_t, tsample_t);
static	int JPEGEncode(TIFF*, tidata_t, tsize_t, tsample_t);
static	int JPEGEncodeRaw(TIFF*, tidata_t, tsize_t, tsample_t);

#define	FIELD_JPEGTABLES	(FIELD_CODEC+0)

static const TIFFFieldInfo jpegFieldInfo[] = {
    { TIFFTAG_JPEGTABLES,	 -1,-1,	TIFF_UNDEFINED,	FIELD_JPEGTABLES,
      FALSE,	TRUE,	"JPEGTables" },
    { TIFFTAG_JPEGQUALITY,	 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      TRUE,	FALSE,	"" },
    { TIFFTAG_JPEGCOLORMODE,	 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      FALSE,	FALSE,	"" },
    { TIFFTAG_JPEGTABLESMODE,	 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      FALSE,	FALSE,	"" },
};
#define	N(a)	(sizeof (a) / sizeof (a[0]))

/*
 * libjpeg interface layer.
 *
 * We use setjmp/longjmp to return control to libtiff
 * when a fatal error is encountered within the JPEG
 * library.  We also direct libjpeg error and warning
 * messages through the appropriate libtiff handlers.
 */

/*
 * Error handling routines (these replace corresponding
 * IJG routines from jerror.c).  These are used for both
 * compression and decompression.
 */
static void
TIFFjpeg_error_exit(j_common_ptr cinfo)
{
	JPEGState *sp = (JPEGState *) cinfo;	/* NB: cinfo assumed first */
	char buffer[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message) (cinfo, buffer);
	TIFFError("JPEGLib", buffer);		/* display the error message */
	jpeg_abort(cinfo);			/* clean up libjpeg state */
	LONGJMP(sp->exit_jmpbuf, 1);		/* return to libtiff caller */
}

/*
 * This routine is invoked only for warning messages,
 * since error_exit does its own thing and trace_level
 * is never set > 0.
 */
static void
TIFFjpeg_output_message(j_common_ptr cinfo)
{
	char buffer[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message) (cinfo, buffer);
	TIFFWarning("JPEGLib", buffer);
}

/*
 * Interface routines.  This layer of routines exists
 * primarily to limit side-effects from using setjmp.
 * Also, normal/error returns are converted into return
 * values per libtiff practice.
 */
#define	CALLJPEG(sp, fail, op)	(SETJMP((sp)->exit_jmpbuf) ? (fail) : (op))
#define	CALLVJPEG(sp, op)	CALLJPEG(sp, 0, ((op),1))

static int
TIFFjpeg_create_compress(JPEGState* sp)
{
	/* initialize JPEG error handling */
	sp->cinfo.c.err = jpeg_std_error(&sp->err);
	sp->err.error_exit = TIFFjpeg_error_exit;
	sp->err.output_message = TIFFjpeg_output_message;

	return CALLVJPEG(sp, jpeg_create_compress(&sp->cinfo.c));
}

static int
TIFFjpeg_create_decompress(JPEGState* sp)
{
	/* initialize JPEG error handling */
	sp->cinfo.d.err = jpeg_std_error(&sp->err);
	sp->err.error_exit = TIFFjpeg_error_exit;
	sp->err.output_message = TIFFjpeg_output_message;

	return CALLVJPEG(sp, jpeg_create_decompress(&sp->cinfo.d));
}

static int
TIFFjpeg_set_defaults(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_set_defaults(&sp->cinfo.c));
}

static int
TIFFjpeg_set_colorspace(JPEGState* sp, J_COLOR_SPACE colorspace)
{
	return CALLVJPEG(sp, jpeg_set_colorspace(&sp->cinfo.c, colorspace));
}

static int
TIFFjpeg_set_quality(JPEGState* sp, int quality, boolean force_baseline)
{
	return CALLVJPEG(sp,
	    jpeg_set_quality(&sp->cinfo.c, quality, force_baseline));
}

static int
TIFFjpeg_suppress_tables(JPEGState* sp, boolean suppress)
{
	return CALLVJPEG(sp, jpeg_suppress_tables(&sp->cinfo.c, suppress));
}

static int
TIFFjpeg_start_compress(JPEGState* sp, boolean write_all_tables)
{
	return CALLVJPEG(sp,
	    jpeg_start_compress(&sp->cinfo.c, write_all_tables));
}

static int
TIFFjpeg_write_scanlines(JPEGState* sp, JSAMPARRAY scanlines, int num_lines)
{
	return CALLJPEG(sp, -1, (int) jpeg_write_scanlines(&sp->cinfo.c,
	    scanlines, (JDIMENSION) num_lines));
}

static int
TIFFjpeg_write_raw_data(JPEGState* sp, JSAMPIMAGE data, int num_lines)
{
	return CALLJPEG(sp, -1, (int) jpeg_write_raw_data(&sp->cinfo.c,
	    data, (JDIMENSION) num_lines));
}

static int
TIFFjpeg_finish_compress(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_finish_compress(&sp->cinfo.c));
}

static int
TIFFjpeg_write_tables(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_write_tables(&sp->cinfo.c));
}

static int
TIFFjpeg_read_header(JPEGState* sp, boolean require_image)
{
	return CALLJPEG(sp, -1, jpeg_read_header(&sp->cinfo.d, require_image));
}

static int
TIFFjpeg_start_decompress(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_start_decompress(&sp->cinfo.d));
}

static int
TIFFjpeg_read_scanlines(JPEGState* sp, JSAMPARRAY scanlines, int max_lines)
{
	return CALLJPEG(sp, -1, (int) jpeg_read_scanlines(&sp->cinfo.d,
	    scanlines, (JDIMENSION) max_lines));
}

static int
TIFFjpeg_read_raw_data(JPEGState* sp, JSAMPIMAGE data, int max_lines)
{
	return CALLJPEG(sp, -1, (int) jpeg_read_raw_data(&sp->cinfo.d,
	    data, (JDIMENSION) max_lines));
}

static int
TIFFjpeg_finish_decompress(JPEGState* sp)
{
	return CALLJPEG(sp, -1, (int) jpeg_finish_decompress(&sp->cinfo.d));
}

static int
TIFFjpeg_abort(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_abort(&sp->cinfo.comm));
}

static int
TIFFjpeg_destroy(JPEGState* sp)
{
	return CALLVJPEG(sp, jpeg_destroy(&sp->cinfo.comm));
}

static JSAMPARRAY
TIFFjpeg_alloc_sarray(JPEGState* sp, int pool_id,
		      JDIMENSION samplesperrow, JDIMENSION numrows)
{
	return CALLJPEG(sp, (JSAMPARRAY) NULL,
	    (*sp->cinfo.comm.mem->alloc_sarray)
		(&sp->cinfo.comm, pool_id, samplesperrow, numrows));
}

/*
 * JPEG library destination data manager.
 * These routines direct compressed data from libjpeg into the
 * libtiff output buffer.
 */

static void
std_init_destination(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;
	TIFF* tif = sp->tif;

	sp->dest.next_output_byte = (JOCTET*) tif->tif_rawdata;
	sp->dest.free_in_buffer = (size_t) tif->tif_rawdatasize;
}

static boolean
std_empty_output_buffer(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;
	TIFF* tif = sp->tif;

	/* the entire buffer has been filled */
	tif->tif_rawcc = tif->tif_rawdatasize;
	TIFFFlushData1(tif);
	sp->dest.next_output_byte = (JOCTET*) tif->tif_rawdata;
	sp->dest.free_in_buffer = (size_t) tif->tif_rawdatasize;

	return (TRUE);
}

static void
std_term_destination(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;
	TIFF* tif = sp->tif;

	tif->tif_rawcp = (tidata_t) sp->dest.next_output_byte;
	tif->tif_rawcc =
	    tif->tif_rawdatasize - (tsize_t) sp->dest.free_in_buffer;
	/* NB: libtiff does the final buffer flush */
}

static void
TIFFjpeg_data_dest(JPEGState* sp, TIFF* tif)
{
	(void) tif;
	sp->cinfo.c.dest = &sp->dest;
	sp->dest.init_destination = std_init_destination;
	sp->dest.empty_output_buffer = std_empty_output_buffer;
	sp->dest.term_destination = std_term_destination;
}

/*
 * Alternate destination manager for outputting to JPEGTables field.
 */

static void
tables_init_destination(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;

	/* while building, jpegtables_length is allocated buffer size */
	sp->dest.next_output_byte = (JOCTET*) sp->jpegtables;
	sp->dest.free_in_buffer = (size_t) sp->jpegtables_length;
}

static boolean
tables_empty_output_buffer(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;
	void* newbuf;

	/* the entire buffer has been filled; enlarge it by 1000 bytes */
	newbuf = _TIFFrealloc((tdata_t) sp->jpegtables,
			      (tsize_t) (sp->jpegtables_length + 1000));
	if (newbuf == NULL)
		ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 100);
	sp->dest.next_output_byte = (JOCTET*) newbuf + sp->jpegtables_length;
	sp->dest.free_in_buffer = (size_t) 1000;
	sp->jpegtables = newbuf;
	sp->jpegtables_length += 1000;
	return (TRUE);
}

static void
tables_term_destination(j_compress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;

	/* set tables length to number of bytes actually emitted */
	sp->jpegtables_length -= sp->dest.free_in_buffer;
}

static int
TIFFjpeg_tables_dest(JPEGState* sp, TIFF* tif)
{
	(void) tif;
	/*
	 * Allocate a working buffer for building tables.
	 * Initial size is 1000 bytes, which is usually adequate.
	 */
	if (sp->jpegtables)
		_TIFFfree(sp->jpegtables);
	sp->jpegtables_length = 1000;
	sp->jpegtables = (void*) _TIFFmalloc((tsize_t) sp->jpegtables_length);
	if (sp->jpegtables == NULL) {
		sp->jpegtables_length = 0;
		TIFFError("TIFFjpeg_tables_dest", "No space for JPEGTables");
		return (0);
	}
	sp->cinfo.c.dest = &sp->dest;
	sp->dest.init_destination = tables_init_destination;
	sp->dest.empty_output_buffer = tables_empty_output_buffer;
	sp->dest.term_destination = tables_term_destination;
	return (1);
}

/*
 * JPEG library source data manager.
 * These routines supply compressed data to libjpeg.
 */

static void
std_init_source(j_decompress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;
	TIFF* tif = sp->tif;

	sp->src.next_input_byte = (const JOCTET*) tif->tif_rawdata;
	sp->src.bytes_in_buffer = (size_t) tif->tif_rawcc;
}

static boolean
std_fill_input_buffer(j_decompress_ptr cinfo)
{
	JPEGState* sp = (JPEGState* ) cinfo;
	static const JOCTET dummy_EOI[2] = { 0xFF, JPEG_EOI };

	/*
	 * Should never get here since entire strip/tile is
	 * read into memory before the decompressor is called,
	 * and thus was supplied by init_source.
	 */
	WARNMS(cinfo, JWRN_JPEG_EOF);
	/* insert a fake EOI marker */
	sp->src.next_input_byte = dummy_EOI;
	sp->src.bytes_in_buffer = 2;
	return (TRUE);
}

static void
std_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
	JPEGState* sp = (JPEGState*) cinfo;

	if (num_bytes > 0) {
		if (num_bytes > (long) sp->src.bytes_in_buffer) {
			/* oops, buffer overrun */
			(void) std_fill_input_buffer(cinfo);
		} else {
			sp->src.next_input_byte += (size_t) num_bytes;
			sp->src.bytes_in_buffer -= (size_t) num_bytes;
		}
	}
}

static void
std_term_source(j_decompress_ptr cinfo)
{
	/* No work necessary here */
	/* Or must we update tif->tif_rawcp, tif->tif_rawcc ??? */
	/* (if so, need empty tables_term_source!) */
	(void) cinfo;
}

static void
TIFFjpeg_data_src(JPEGState* sp, TIFF* tif)
{
	(void) tif;
	sp->cinfo.d.src = &sp->src;
	sp->src.init_source = std_init_source;
	sp->src.fill_input_buffer = std_fill_input_buffer;
	sp->src.skip_input_data = std_skip_input_data;
	sp->src.resync_to_restart = jpeg_resync_to_restart;
	sp->src.term_source = std_term_source;
	sp->src.bytes_in_buffer = 0;		/* for safety */
	sp->src.next_input_byte = NULL;
}

/*
 * Alternate source manager for reading from JPEGTables.
 * We can share all the code except for the init routine.
 */

static void
tables_init_source(j_decompress_ptr cinfo)
{
	JPEGState* sp = (JPEGState*) cinfo;

	sp->src.next_input_byte = (const JOCTET*) sp->jpegtables;
	sp->src.bytes_in_buffer = (size_t) sp->jpegtables_length;
}

static void
TIFFjpeg_tables_src(JPEGState* sp, TIFF* tif)
{
	TIFFjpeg_data_src(sp, tif);
	sp->src.init_source = tables_init_source;
}

/*
 * Allocate downsampled-data buffers needed for downsampled I/O.
 * We use values computed in jpeg_start_compress or jpeg_start_decompress.
 * We use libjpeg's allocator so that buffers will be released automatically
 * when done with strip/tile.
 * This is also a handy place to compute samplesperclump, bytesperline.
 */
static int
alloc_downsampled_buffers(TIFF* tif, jpeg_component_info* comp_info,
			  int num_components)
{
	JPEGState* sp = JState(tif);
	int ci;
	jpeg_component_info* compptr;
	JSAMPARRAY buf;
	int samples_per_clump = 0;

	for (ci = 0, compptr = comp_info; ci < num_components;
	     ci++, compptr++) {
		samples_per_clump += compptr->h_samp_factor *
			compptr->v_samp_factor;
		buf = TIFFjpeg_alloc_sarray(sp, JPOOL_IMAGE,
				compptr->width_in_blocks * DCTSIZE,
				(JDIMENSION) (compptr->v_samp_factor*DCTSIZE));
		if (buf == NULL)
			return (0);
		sp->ds_buffer[ci] = buf;
	}
	sp->samplesperclump = samples_per_clump;
	/* Cb,Cr both have sampling factors 1 */
	/* so downsampled width of Cb is # of clumps per line */
	sp->bytesperline = sizeof(JSAMPLE) * samples_per_clump *
		comp_info[1].downsampled_width;
	return (1);
}


/*
 * JPEG Decoding.
 */

static int
JPEGSetupDecode(TIFF* tif)
{
	JPEGState* sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;

	assert(sp != NULL);
	assert(sp->cinfo.comm.is_decompressor);

	/* Read JPEGTables if it is present */
	if (TIFFFieldSet(tif,FIELD_JPEGTABLES)) {
		TIFFjpeg_tables_src(sp, tif);
		if(TIFFjpeg_read_header(sp,FALSE) != JPEG_HEADER_TABLES_ONLY) {
			TIFFError("JPEGSetupDecode", "Bogus JPEGTables field");
			return (0);
		}
	}

	/* Grab parameters that are same for all strips/tiles */
	sp->photometric = td->td_photometric;
	switch (sp->photometric) {
	case PHOTOMETRIC_YCBCR:
		sp->h_sampling = td->td_ycbcrsubsampling[0];
		sp->v_sampling = td->td_ycbcrsubsampling[1];
		break;
	default:
		/* TIFF 6.0 forbids subsampling of all other color spaces */
		sp->h_sampling = 1;
		sp->v_sampling = 1;
		break;
	}

	/* Set up for reading normal data */
	TIFFjpeg_data_src(sp, tif);
	tif->tif_postdecode = _TIFFNoPostDecode; /* override byte swapping */
	return (1);
}

/*
 * Set up for decoding a strip or tile.
 */
static int
JPEGPreDecode(TIFF* tif, tsample_t s)
{
	JPEGState *sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;
	static const char module[] = "JPEGPreDecode";
	uint32 segment_width, segment_height;
	int downsampled_output;
	int ci;

	assert(sp != NULL);
	assert(sp->cinfo.comm.is_decompressor);
	/*
	 * Reset decoder state from any previous strip/tile,
	 * in case application didn't read the whole strip.
	 */
	if (!TIFFjpeg_abort(sp))
		return (0);
	/*
	 * Read the header for this strip/tile.
	 */
	if (TIFFjpeg_read_header(sp, TRUE) != JPEG_HEADER_OK)
		return (0);
	/*
	 * Check image parameters and set decompression parameters.
	 */
	if (isTiled(tif)) {
		segment_width = td->td_tilewidth;
		segment_height = td->td_tilelength;
		sp->bytesperline = TIFFTileRowSize(tif);
	} else {
		segment_width = td->td_imagewidth;
		segment_height = td->td_imagelength - tif->tif_row;
		if (segment_height > td->td_rowsperstrip)
			segment_height = td->td_rowsperstrip;
		sp->bytesperline = TIFFScanlineSize(tif);
	}
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE && s > 0) {
		/*
		 * For PC 2, scale down the expected strip/tile size
		 * to match a downsampled component
		 */
		segment_width = TIFFhowmany(segment_width, sp->h_sampling);
		segment_height = TIFFhowmany(segment_height, sp->v_sampling);
	}
	if (sp->cinfo.d.image_width != segment_width ||
	    sp->cinfo.d.image_height != segment_height) {
		TIFFError(module, "Improper JPEG strip/tile size");
		return (0);
	}
	if (sp->cinfo.d.num_components !=
	    (td->td_planarconfig == PLANARCONFIG_CONTIG ?
	     td->td_samplesperpixel : 1)) {
		TIFFError(module, "Improper JPEG component count");
		return (0);
	}
	if (sp->cinfo.d.data_precision != td->td_bitspersample) {
		TIFFError(module, "Improper JPEG data precision");
		return (0);
	}
	if (td->td_planarconfig == PLANARCONFIG_CONTIG) {
		/* Component 0 should have expected sampling factors */
		if (sp->cinfo.d.comp_info[0].h_samp_factor != sp->h_sampling ||
		    sp->cinfo.d.comp_info[0].v_samp_factor != sp->v_sampling) {
			TIFFError(module, "Improper JPEG sampling factors");
			return (0);
		}
		/* Rest should have sampling factors 1,1 */
		for (ci = 1; ci < sp->cinfo.d.num_components; ci++) {
			if (sp->cinfo.d.comp_info[ci].h_samp_factor != 1 ||
			    sp->cinfo.d.comp_info[ci].v_samp_factor != 1) {
				TIFFError(module, "Improper JPEG sampling factors");
				return (0);
			}
		}
	} else {
		/* PC 2's single component should have sampling factors 1,1 */
		if (sp->cinfo.d.comp_info[0].h_samp_factor != 1 ||
		    sp->cinfo.d.comp_info[0].v_samp_factor != 1) {
			TIFFError(module, "Improper JPEG sampling factors");
			return (0);
		}
	}
	downsampled_output = FALSE;
	if (td->td_planarconfig == PLANARCONFIG_CONTIG &&
	    sp->photometric == PHOTOMETRIC_YCBCR &&
	    sp->jpegcolormode == JPEGCOLORMODE_RGB) {
		/* Convert YCbCr to RGB */
		sp->cinfo.d.jpeg_color_space = JCS_YCbCr;
		sp->cinfo.d.out_color_space = JCS_RGB;
	} else {
		/* Suppress colorspace handling */
		sp->cinfo.d.jpeg_color_space = JCS_UNKNOWN;
		sp->cinfo.d.out_color_space = JCS_UNKNOWN;
		if (td->td_planarconfig == PLANARCONFIG_CONTIG &&
		    (sp->h_sampling != 1 || sp->v_sampling != 1))
			downsampled_output = TRUE;
		/* XXX what about up-sampling? */
	}
	if (downsampled_output) {
		/* Need to use raw-data interface to libjpeg */
		sp->cinfo.d.raw_data_out = TRUE;
		tif->tif_decoderow = JPEGDecodeRaw;
		tif->tif_decodestrip = JPEGDecodeRaw;
		tif->tif_decodetile = JPEGDecodeRaw;
	} else {
		/* Use normal interface to libjpeg */
		sp->cinfo.d.raw_data_out = FALSE;
		tif->tif_decoderow = JPEGDecode;
		tif->tif_decodestrip = JPEGDecode;
		tif->tif_decodetile = JPEGDecode;
	}
	/* Start JPEG decompressor */
	if (!TIFFjpeg_start_decompress(sp))
		return (0);
	/* Allocate downsampled-data buffers if needed */
	if (downsampled_output) {
		if (!alloc_downsampled_buffers(tif, sp->cinfo.d.comp_info,
					       sp->cinfo.d.num_components))
			return (0);
		sp->scancount = DCTSIZE;	/* mark buffer empty */
	}
	return (1);
}

/*
 * Decode a chunk of pixels.
 * "Standard" case: returned data is not downsampled.
 */
static int
JPEGDecode(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	JPEGState *sp = JState(tif);
	tsize_t nrows;
	JSAMPROW bufptr[1];

	(void) s;
	assert(sp != NULL);
	/* data is expected to be read in multiples of a scanline */
	nrows = cc / sp->bytesperline;
	if (cc % sp->bytesperline)
		TIFFWarning(tif->tif_name, "fractional scanline not read");

	while (nrows-- > 0) {
		bufptr[0] = (JSAMPROW) buf;
		if (TIFFjpeg_read_scanlines(sp, bufptr, 1) != 1)
			return (0);
		if (nrows > 0)
			tif->tif_row++;
		buf += sp->bytesperline;
	}
	/* Close down the decompressor if we've finished the strip or tile. */
	if (sp->cinfo.d.output_scanline == sp->cinfo.d.output_height) {
		if (TIFFjpeg_finish_decompress(sp) != TRUE)
			return (0);
	}
	return (1);
}

/*
 * Decode a chunk of pixels.
 * Returned data is downsampled per sampling factors.
 */
static int
JPEGDecodeRaw(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	JPEGState *sp = JState(tif);
	JSAMPLE* inptr;
	JSAMPLE* outptr;
	tsize_t nrows;
	JDIMENSION clumps_per_line, nclump;
	int clumpoffset, ci, xpos, ypos;
	jpeg_component_info* compptr;
	int samples_per_clump = sp->samplesperclump;

	(void) s;
	assert(sp != NULL);
	/* data is expected to be read in multiples of a scanline */
	nrows = cc / sp->bytesperline;
	if (cc % sp->bytesperline)
		TIFFWarning(tif->tif_name, "fractional scanline not read");

	/* Cb,Cr both have sampling factors 1, so this is correct */
	clumps_per_line = sp->cinfo.d.comp_info[1].downsampled_width;

	while (nrows-- > 0) {
		/* Reload downsampled-data buffer if needed */
		if (sp->scancount >= DCTSIZE) {
			int n = sp->cinfo.d.max_v_samp_factor * DCTSIZE;
			if (TIFFjpeg_read_raw_data(sp, sp->ds_buffer, n) != n)
				return (0);
			sp->scancount = 0;
			/* Close down the decompressor if done. */
			if (sp->cinfo.d.output_scanline >=
			    sp->cinfo.d.output_height) {
				if (TIFFjpeg_finish_decompress(sp) != TRUE)
					return (0);
			}
		}
		/*
		 * Fastest way to unseparate the data is to make one pass
		 * over the scanline for each row of each component.
		 */
		clumpoffset = 0;		/* first sample in clump */
		for (ci = 0, compptr = sp->cinfo.d.comp_info;
		     ci < sp->cinfo.d.num_components;
		     ci++, compptr++) {
		    int hsamp = compptr->h_samp_factor;
		    int vsamp = compptr->v_samp_factor;
		    for (ypos = 0; ypos < vsamp; ypos++) {
			inptr = sp->ds_buffer[ci][sp->scancount*vsamp + ypos];
			outptr = ((JSAMPLE*) buf) + clumpoffset;
			if (hsamp == 1) {
			    /* fast path for at least Cb and Cr */
			    for (nclump = clumps_per_line; nclump-- > 0; ) {
				outptr[0] = *inptr++;
				outptr += samples_per_clump;
			    }
			} else {
			    /* general case */
			    for (nclump = clumps_per_line; nclump-- > 0; ) {
				for (xpos = 0; xpos < hsamp; xpos++)
				    outptr[xpos] = *inptr++;
				outptr += samples_per_clump;
			    }
			}
			clumpoffset += hsamp;
		    }
		}
		sp->scancount++;
		if (nrows > 0)
			tif->tif_row++;
		buf += sp->bytesperline;
	}
	return (1);
}


/*
 * JPEG Encoding.
 */

static void
unsuppress_quant_table (JPEGState* sp, int tblno)
{
	JQUANT_TBL* qtbl;

	if ((qtbl = sp->cinfo.c.quant_tbl_ptrs[tblno]) != NULL)
		qtbl->sent_table = FALSE;
}

static void
unsuppress_huff_table (JPEGState* sp, int tblno)
{
	JHUFF_TBL* htbl;

	if ((htbl = sp->cinfo.c.dc_huff_tbl_ptrs[tblno]) != NULL)
		htbl->sent_table = FALSE;
	if ((htbl = sp->cinfo.c.ac_huff_tbl_ptrs[tblno]) != NULL)
		htbl->sent_table = FALSE;
}

static int
prepare_JPEGTables(TIFF* tif)
{
	JPEGState* sp = JState(tif);

	/* Initialize quant tables for current quality setting */
	if (!TIFFjpeg_set_quality(sp, sp->jpegquality, FALSE))
		return (0);
	/* Mark only the tables we want for output */
	/* NB: chrominance tables are currently used only with YCbCr */
	if (!TIFFjpeg_suppress_tables(sp, TRUE))
		return (0);
	if (sp->jpegtablesmode & JPEGTABLESMODE_QUANT) {
		unsuppress_quant_table(sp, 0);
		if (sp->photometric == PHOTOMETRIC_YCBCR)
			unsuppress_quant_table(sp, 1);
	}
	if (sp->jpegtablesmode & JPEGTABLESMODE_HUFF) {
		unsuppress_huff_table(sp, 0);
		if (sp->photometric == PHOTOMETRIC_YCBCR)
			unsuppress_huff_table(sp, 1);
	}
	/* Direct libjpeg output into jpegtables */
	if (!TIFFjpeg_tables_dest(sp, tif))
		return (0);
	/* Emit tables-only datastream */
	if (!TIFFjpeg_write_tables(sp))
		return (0);

	return (1);
}

static int
JPEGSetupEncode(TIFF* tif)
{
	JPEGState* sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;
	static const char module[] = "JPEGSetupEncode";

	assert(sp != NULL);
	assert(!sp->cinfo.comm.is_decompressor);

	/*
	 * Initialize all JPEG parameters to default values.
	 * Note that jpeg_set_defaults needs legal values for
	 * in_color_space and input_components.
	 */
	sp->cinfo.c.in_color_space = JCS_UNKNOWN;
	sp->cinfo.c.input_components = 1;
	if (!TIFFjpeg_set_defaults(sp))
		return (0);
	/* Set per-file parameters */
	sp->photometric = td->td_photometric;
	switch (sp->photometric) {
	case PHOTOMETRIC_YCBCR:
		sp->h_sampling = td->td_ycbcrsubsampling[0];
		sp->v_sampling = td->td_ycbcrsubsampling[1];
		/*
		 * A ReferenceBlackWhite field *must* be present since the
		 * default value is inappropriate for YCbCr.  Fill in the
		 * proper value if application didn't set it.
		 */
#ifdef COLORIMETRY_SUPPORT
		if (!TIFFFieldSet(tif, FIELD_REFBLACKWHITE)) {
			float refbw[6];
			long top = 1L << td->td_bitspersample;
			refbw[0] = 0;
			refbw[1] = (float)(top-1L);
			refbw[2] = (float)(top>>1);
			refbw[3] = refbw[1];
			refbw[4] = refbw[2];
			refbw[5] = refbw[1];
			TIFFSetField(tif, TIFFTAG_REFERENCEBLACKWHITE, refbw);
		}
#endif
		break;
	case PHOTOMETRIC_PALETTE:		/* disallowed by Tech Note */
	case PHOTOMETRIC_MASK:
		TIFFError(module,
			  "PhotometricInterpretation %d not allowed for JPEG",
			  (int) sp->photometric);
		return (0);
	default:
		/* TIFF 6.0 forbids subsampling of all other color spaces */
		sp->h_sampling = 1;
		sp->v_sampling = 1;
		break;
	}
	
	/* Verify miscellaneous parameters */

	/*
	 * This would need work if libtiff ever supports different
	 * depths for different components, or if libjpeg ever supports
	 * run-time selection of depth.  Neither is imminent.
	 */
	if (td->td_bitspersample != BITS_IN_JSAMPLE) {
		TIFFError(module, "BitsPerSample %d not allowed for JPEG",
			  (int) td->td_bitspersample);
		return (0);
	}
	sp->cinfo.c.data_precision = td->td_bitspersample;
	if (isTiled(tif)) {
		if ((td->td_tilelength % (sp->v_sampling * DCTSIZE)) != 0) {
			TIFFError(module,
				  "JPEG tile height must be multiple of %d",
				  sp->v_sampling * DCTSIZE);
			return (0);
		}
		if ((td->td_tilewidth % (sp->h_sampling * DCTSIZE)) != 0) {
			TIFFError(module,
				  "JPEG tile width must be multiple of %d",
				  sp->h_sampling * DCTSIZE);
			return (0);
		}
	} else {
		if (td->td_rowsperstrip < td->td_imagelength &&
		    (td->td_rowsperstrip % (sp->v_sampling * DCTSIZE)) != 0) {
			TIFFError(module,
				  "RowsPerStrip must be multiple of %d for JPEG",
				  sp->v_sampling * DCTSIZE);
			return (0);
		}
	}

	/* Create a JPEGTables field if appropriate */
	if (sp->jpegtablesmode & (JPEGTABLESMODE_QUANT|JPEGTABLESMODE_HUFF)) {
		if (!prepare_JPEGTables(tif))
			return (0);
		/* Mark the field present */
		/* Can't use TIFFSetField since BEENWRITING is already set! */
		TIFFSetFieldBit(tif, FIELD_JPEGTABLES);
		tif->tif_flags |= TIFF_DIRTYDIRECT;
	} else {
		/* We do not support application-supplied JPEGTables, */
		/* so mark the field not present */
		TIFFClrFieldBit(tif, FIELD_JPEGTABLES);
	}

	/* Direct libjpeg output to libtiff's output buffer */
	TIFFjpeg_data_dest(sp, tif);

	return (1);
}

/*
 * Set encoding state at the start of a strip or tile.
 */
static int
JPEGPreEncode(TIFF* tif, tsample_t s)
{
	JPEGState *sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;
	static const char module[] = "JPEGPreEncode";
	uint32 segment_width, segment_height;
	int downsampled_input;

	assert(sp != NULL);
	assert(!sp->cinfo.comm.is_decompressor);
	/*
	 * Set encoding parameters for this strip/tile.
	 */
	if (isTiled(tif)) {
		segment_width = td->td_tilewidth;
		segment_height = td->td_tilelength;
		sp->bytesperline = TIFFTileRowSize(tif);
	} else {
		segment_width = td->td_imagewidth;
		segment_height = td->td_imagelength - tif->tif_row;
		if (segment_height > td->td_rowsperstrip)
			segment_height = td->td_rowsperstrip;
		sp->bytesperline = TIFFScanlineSize(tif);
	}
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE && s > 0) {
		/* for PC 2, scale down the strip/tile size
		 * to match a downsampled component
		 */
		segment_width = TIFFhowmany(segment_width, sp->h_sampling);
		segment_height = TIFFhowmany(segment_height, sp->v_sampling);
	}
	if (segment_width > 65535 || segment_height > 65535) {
		TIFFError(module, "Strip/tile too large for JPEG");
		return (0);
	}
	sp->cinfo.c.image_width = segment_width;
	sp->cinfo.c.image_height = segment_height;
	downsampled_input = FALSE;
	if (td->td_planarconfig == PLANARCONFIG_CONTIG) {
		sp->cinfo.c.input_components = td->td_samplesperpixel;
		if (sp->photometric == PHOTOMETRIC_YCBCR) {
			if (sp->jpegcolormode == JPEGCOLORMODE_RGB) {
				sp->cinfo.c.in_color_space = JCS_RGB;
			} else {
				sp->cinfo.c.in_color_space = JCS_YCbCr;
				if (sp->h_sampling != 1 || sp->v_sampling != 1)
					downsampled_input = TRUE;
			}
			if (!TIFFjpeg_set_colorspace(sp, JCS_YCbCr))
				return (0);
			/*
			 * Set Y sampling factors;
			 * we assume jpeg_set_colorspace() set the rest to 1
			 */
			sp->cinfo.c.comp_info[0].h_samp_factor = sp->h_sampling;
			sp->cinfo.c.comp_info[0].v_samp_factor = sp->v_sampling;
		} else {
			sp->cinfo.c.in_color_space = JCS_UNKNOWN;
			if (!TIFFjpeg_set_colorspace(sp, JCS_UNKNOWN))
				return (0);
			/* jpeg_set_colorspace set all sampling factors to 1 */
		}
	} else {
		sp->cinfo.c.input_components = 1;
		sp->cinfo.c.in_color_space = JCS_UNKNOWN;
		if (!TIFFjpeg_set_colorspace(sp, JCS_UNKNOWN))
			return (0);
		sp->cinfo.c.comp_info[0].component_id = s;
		/* jpeg_set_colorspace() set sampling factors to 1 */
		if (sp->photometric == PHOTOMETRIC_YCBCR && s > 0) {
			sp->cinfo.c.comp_info[0].quant_tbl_no = 1;
			sp->cinfo.c.comp_info[0].dc_tbl_no = 1;
			sp->cinfo.c.comp_info[0].ac_tbl_no = 1;
		}
	}
	/* ensure libjpeg won't write any extraneous markers */
	sp->cinfo.c.write_JFIF_header = FALSE;
	sp->cinfo.c.write_Adobe_marker = FALSE;
	/* set up table handling correctly */
	if (! (sp->jpegtablesmode & JPEGTABLESMODE_QUANT)) {
		if (!TIFFjpeg_set_quality(sp, sp->jpegquality, FALSE))
			return (0);
		unsuppress_quant_table(sp, 0);
		unsuppress_quant_table(sp, 1);
	}
	if (sp->jpegtablesmode & JPEGTABLESMODE_HUFF)
		sp->cinfo.c.optimize_coding = FALSE;
	else
		sp->cinfo.c.optimize_coding = TRUE;
	if (downsampled_input) {
		/* Need to use raw-data interface to libjpeg */
		sp->cinfo.c.raw_data_in = TRUE;
		tif->tif_encoderow = JPEGEncodeRaw;
		tif->tif_encodestrip = JPEGEncodeRaw;
		tif->tif_encodetile = JPEGEncodeRaw;
	} else {
		/* Use normal interface to libjpeg */
		sp->cinfo.c.raw_data_in = FALSE;
		tif->tif_encoderow = JPEGEncode;
		tif->tif_encodestrip = JPEGEncode;
		tif->tif_encodetile = JPEGEncode;
	}
	/* Start JPEG compressor */
	if (!TIFFjpeg_start_compress(sp, FALSE))
		return (0);
	/* Allocate downsampled-data buffers if needed */
	if (downsampled_input) {
		if (!alloc_downsampled_buffers(tif, sp->cinfo.c.comp_info,
					       sp->cinfo.c.num_components))
			return (0);
	}
	sp->scancount = 0;

	return (1);
}

/*
 * Encode a chunk of pixels.
 * "Standard" case: incoming data is not downsampled.
 */
static int
JPEGEncode(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	JPEGState *sp = JState(tif);
	tsize_t nrows;
	JSAMPROW bufptr[1];

	(void) s;
	assert(sp != NULL);
	/* data is expected to be supplied in multiples of a scanline */
	nrows = cc / sp->bytesperline;
	if (cc % sp->bytesperline)
		TIFFWarning(tif->tif_name, "fractional scanline discarded");

	while (nrows-- > 0) {
		bufptr[0] = (JSAMPROW) buf;
		if (TIFFjpeg_write_scanlines(sp, bufptr, 1) != 1)
			return (0);
		if (nrows > 0)
			tif->tif_row++;
		buf += sp->bytesperline;
	}
	return (1);
}

/*
 * Encode a chunk of pixels.
 * Incoming data is expected to be downsampled per sampling factors.
 */
static int
JPEGEncodeRaw(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	JPEGState *sp = JState(tif);
	JSAMPLE* inptr;
	JSAMPLE* outptr;
	tsize_t nrows;
	JDIMENSION clumps_per_line, nclump;
	int clumpoffset, ci, xpos, ypos;
	jpeg_component_info* compptr;
	int samples_per_clump = sp->samplesperclump;

	(void) s;
	assert(sp != NULL);
	/* data is expected to be supplied in multiples of a scanline */
	nrows = cc / sp->bytesperline;
	if (cc % sp->bytesperline)
		TIFFWarning(tif->tif_name, "fractional scanline discarded");

	/* Cb,Cr both have sampling factors 1, so this is correct */
	clumps_per_line = sp->cinfo.c.comp_info[1].downsampled_width;

	while (nrows-- > 0) {
		/*
		 * Fastest way to separate the data is to make one pass
		 * over the scanline for each row of each component.
		 */
		clumpoffset = 0;		/* first sample in clump */
		for (ci = 0, compptr = sp->cinfo.c.comp_info;
		     ci < sp->cinfo.c.num_components;
		     ci++, compptr++) {
		    int hsamp = compptr->h_samp_factor;
		    int vsamp = compptr->v_samp_factor;
		    int padding = (int) (compptr->width_in_blocks * DCTSIZE -
					 clumps_per_line * hsamp);
		    for (ypos = 0; ypos < vsamp; ypos++) {
			inptr = ((JSAMPLE*) buf) + clumpoffset;
			outptr = sp->ds_buffer[ci][sp->scancount*vsamp + ypos];
			if (hsamp == 1) {
			    /* fast path for at least Cb and Cr */
			    for (nclump = clumps_per_line; nclump-- > 0; ) {
				*outptr++ = inptr[0];
				inptr += samples_per_clump;
			    }
			} else {
			    /* general case */
			    for (nclump = clumps_per_line; nclump-- > 0; ) {
				for (xpos = 0; xpos < hsamp; xpos++)
				    *outptr++ = inptr[xpos];
				inptr += samples_per_clump;
			    }
			}
			/* pad each scanline as needed */
			for (xpos = 0; xpos < padding; xpos++) {
			    *outptr = outptr[-1];
			    outptr++;
			}
			clumpoffset += hsamp;
		    }
		}
		sp->scancount++;
		if (sp->scancount >= DCTSIZE) {
			int n = sp->cinfo.c.max_v_samp_factor * DCTSIZE;
			if (TIFFjpeg_write_raw_data(sp, sp->ds_buffer, n) != n)
				return (0);
			sp->scancount = 0;
		}
		if (nrows > 0)
			tif->tif_row++;
		buf += sp->bytesperline;
	}
	return (1);
}

/*
 * Finish up at the end of a strip or tile.
 */
static int
JPEGPostEncode(TIFF* tif)
{
	JPEGState *sp = JState(tif);

	if (sp->scancount > 0) {
		/*
		 * Need to emit a partial bufferload of downsampled data.
		 * Pad the data vertically.
		 */
		int ci, ypos, n;
		jpeg_component_info* compptr;

		for (ci = 0, compptr = sp->cinfo.c.comp_info;
		     ci < sp->cinfo.c.num_components;
		     ci++, compptr++) {
			int vsamp = compptr->v_samp_factor;
			tsize_t row_width = compptr->width_in_blocks * DCTSIZE
				* sizeof(JSAMPLE);
			for (ypos = sp->scancount * vsamp;
			     ypos < DCTSIZE * vsamp; ypos++) {
				_TIFFmemcpy((tdata_t)sp->ds_buffer[ci][ypos],
					    (tdata_t)sp->ds_buffer[ci][ypos-1],
					    row_width);

			}
		}
		n = sp->cinfo.c.max_v_samp_factor * DCTSIZE;
		if (TIFFjpeg_write_raw_data(sp, sp->ds_buffer, n) != n)
			return (0);
	}

	return (TIFFjpeg_finish_compress(JState(tif)));
}

static void
JPEGCleanup(TIFF* tif)
{
	if (tif->tif_data) {
		JPEGState *sp = JState(tif);
		TIFFjpeg_destroy(sp);		/* release libjpeg resources */
		if (sp->jpegtables)		/* tag value */
			_TIFFfree(sp->jpegtables);
		_TIFFfree(tif->tif_data);	/* release local state */
		tif->tif_data = NULL;
	}
}

static int
JPEGVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	JPEGState* sp = JState(tif);
	TIFFDirectory* td = &tif->tif_dir;
	uint32 v32;

	switch (tag) {
	case TIFFTAG_JPEGTABLES:
		v32 = va_arg(ap, uint32);
		if (v32 == 0) {
			/* XXX */
			return (0);
		}
		_TIFFsetByteArray(&sp->jpegtables, va_arg(ap, void*),
		    (long) v32);
		sp->jpegtables_length = v32;
		TIFFSetFieldBit(tif, FIELD_JPEGTABLES);
		break;
	case TIFFTAG_JPEGQUALITY:
		sp->jpegquality = va_arg(ap, int);
		return (1);			/* pseudo tag */
	case TIFFTAG_JPEGCOLORMODE:
		sp->jpegcolormode = va_arg(ap, int);
		/*
		 * Mark whether returned data is up-sampled or not
		 * so TIFFStripSize and TIFFTileSize return values
		 * that reflect the true amount of data.
		 */
		tif->tif_flags &= ~TIFF_UPSAMPLED;
		if (td->td_planarconfig == PLANARCONFIG_CONTIG) {
		    if (td->td_photometric == PHOTOMETRIC_YCBCR &&
		      sp->jpegcolormode == JPEGCOLORMODE_RGB) {
			tif->tif_flags |= TIFF_UPSAMPLED;
		    } else {
			if (td->td_ycbcrsubsampling[0] != 1 ||
			    td->td_ycbcrsubsampling[1] != 1)
			    ; /* XXX what about up-sampling? */
		    }
		}
		/*
		 * Must recalculate cached tile size
		 * in case sampling state changed.
		 */
		tif->tif_tilesize = TIFFTileSize(tif);
		return (1);			/* pseudo tag */
	case TIFFTAG_JPEGTABLESMODE:
		sp->jpegtablesmode = va_arg(ap, int);
		return (1);			/* pseudo tag */
	default:
		return (*sp->vsetparent)(tif, tag, ap);
	}
	tif->tif_flags |= TIFF_DIRTYDIRECT;
	return (1);
}

static int
JPEGVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	JPEGState* sp = JState(tif);

	switch (tag) {
	case TIFFTAG_JPEGTABLES:
		/* u_short is bogus --- should be uint32 ??? */
		/* TIFFWriteNormalTag needs fixed  XXX */
		*va_arg(ap, u_short*) = (u_short) sp->jpegtables_length;
		*va_arg(ap, void**) = sp->jpegtables;
		break;
	case TIFFTAG_JPEGQUALITY:
		*va_arg(ap, int*) = sp->jpegquality;
		break;
	case TIFFTAG_JPEGCOLORMODE:
		*va_arg(ap, int*) = sp->jpegcolormode;
		break;
	case TIFFTAG_JPEGTABLESMODE:
		*va_arg(ap, int*) = sp->jpegtablesmode;
		break;
	default:
		return (*sp->vgetparent)(tif, tag, ap);
	}
	return (1);
}

static void
JPEGPrintDir(TIFF* tif, FILE* fd, long flags)
{
	JPEGState* sp = JState(tif);

	(void) flags;
	if (TIFFFieldSet(tif,FIELD_JPEGTABLES))
		fprintf(fd, "  JPEG Tables: (%lu bytes)\n",
			(u_long) sp->jpegtables_length);
}

static uint32
JPEGDefaultStripSize(TIFF* tif, uint32 s)
{
	JPEGState* sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;

	s = (*sp->defsparent)(tif, s);
	if (s < td->td_imagelength)
		s = TIFFroundup(s, td->td_ycbcrsubsampling[1] * DCTSIZE);
	return (s);
}

static void
JPEGDefaultTileSize(TIFF* tif, uint32* tw, uint32* th)
{
	JPEGState* sp = JState(tif);
	TIFFDirectory *td = &tif->tif_dir;

	(*sp->deftparent)(tif, tw, th);
	*tw = TIFFroundup(*tw, td->td_ycbcrsubsampling[0] * DCTSIZE);
	*th = TIFFroundup(*th, td->td_ycbcrsubsampling[1] * DCTSIZE);
}

int
TIFFInitJPEG(TIFF* tif, int scheme)
{
	JPEGState* sp;

	assert(scheme == COMPRESSION_JPEG);

	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	tif->tif_data = (tidata_t) _TIFFmalloc(sizeof (JPEGState));
	if (tif->tif_data == NULL) {
		TIFFError("TIFFInitJPEG", "No space for JPEG state block");
		return (0);
	}
	sp = JState(tif);
	sp->tif = tif;				/* back link */

	/*
	 * Merge codec-specific tag information and
	 * override parent get/set field methods.
	 */
	_TIFFMergeFieldInfo(tif, jpegFieldInfo, N(jpegFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = JPEGVGetField;	/* hook for codec tags */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = JPEGVSetField;	/* hook for codec tags */
	tif->tif_printdir = JPEGPrintDir;	/* hook for codec tags */

	/* Default values for codec-specific fields */
	sp->jpegtables = NULL;
	sp->jpegtables_length = 0;
	sp->jpegquality = 75;			/* Default IJG quality */
	sp->jpegcolormode = JPEGCOLORMODE_RAW;
	sp->jpegtablesmode = JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF;

	/*
	 * Install codec methods.
	 */
	tif->tif_setupdecode = JPEGSetupDecode;
	tif->tif_predecode = JPEGPreDecode;
	tif->tif_decoderow = JPEGDecode;
	tif->tif_decodestrip = JPEGDecode;
	tif->tif_decodetile = JPEGDecode;
	tif->tif_setupencode = JPEGSetupEncode;
	tif->tif_preencode = JPEGPreEncode;
	tif->tif_postencode = JPEGPostEncode;
	tif->tif_encoderow = JPEGEncode;
	tif->tif_encodestrip = JPEGEncode;
	tif->tif_encodetile = JPEGEncode;
	tif->tif_cleanup = JPEGCleanup;
	sp->defsparent = tif->tif_defstripsize;
	tif->tif_defstripsize = JPEGDefaultStripSize;
	sp->deftparent = tif->tif_deftilesize;
	tif->tif_deftilesize = JPEGDefaultTileSize;
	tif->tif_flags |= TIFF_NOBITREV;	/* no bit reversal, please */

	/*
	 * Initialize libjpeg.
	 */
	if (tif->tif_mode == O_RDONLY) {
		if (!TIFFjpeg_create_decompress(sp))
			return (0);
	} else {
		if (!TIFFjpeg_create_compress(sp))
			return (0);
	}

	return (1);
}
#endif /* JPEG_SUPPORT */


/* ==== tif_luv.c ==== */
/*
 * Copyright (c) 1997 Greg Ward Larson
 * Copyright (c) 1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler, Greg Larson and Silicon Graphics may not be used in any
 * advertising or publicity relating to the software without the specific,
 * prior written permission of Sam Leffler, Greg Larson and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER, GREG LARSON OR SILICON GRAPHICS BE LIABLE
 * FOR ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef LOGLUV_SUPPORT

/*
 * TIFF Library.
 * LogLuv compression support for high dynamic range images.
 *
 * Contributed by Greg Larson.
 *
 * LogLuv image support uses the TIFF library to store 16 or 10-bit
 * log luminance values with 8 bits each of u and v or a 14-bit index.
 *
 * The codec can take as input and produce as output 32-bit IEEE float values 
 * as well as 16-bit integer values.  A 16-bit luminance is interpreted
 * as a sign bit followed by a 15-bit integer that is converted
 * to and from a linear magnitude using the transformation:
 *
 *	L = 2^( (Le+.5)/256 - 64 )		# real from 15-bit
 *
 *	Le = floor( 256*(log2(L) + 64) )	# 15-bit from real
 *
 * The actual conversion to world luminance units in candelas per sq. meter
 * requires an additional multiplier, which is stored in the TIFFTAG_STONITS.
 * This value is usually set such that a reasonable exposure comes from
 * clamping decoded luminances above 1 to 1 in the displayed image.
 *
 * The 16-bit values for u and v may be converted to real values by dividing
 * each by 32768.  (This allows for negative values, which aren't useful as
 * far as we know, but are left in case of future improvements in human
 * color vision.)
 *
 * Conversion from (u,v), which is actually the CIE (u',v') system for
 * you color scientists, is accomplished by the following transformation:
 *
 *	u = 4*x / (-2*x + 12*y + 3)
 *	v = 9*y / (-2*x + 12*y + 3)
 *
 *	x = 9*u / (6*u - 16*v + 12)
 *	y = 4*v / (6*u - 16*v + 12)
 *
 * This process is greatly simplified by passing 32-bit IEEE floats
 * for each of three CIE XYZ coordinates.  The codec then takes care
 * of conversion to and from LogLuv, though the application is still
 * responsible for interpreting the TIFFTAG_STONITS calibration factor.
 *
 * The information is compressed into one of two basic encodings, depending on
 * the setting of the compression tag, which is one of COMPRESSION_SGILOG
 * or COMPRESSION_SGILOG24.  For COMPRESSION_SGILOG, greyscale data is
 * stored as:
 *
 *	 1       15
 *	|-+---------------|
 *
 * COMPRESSION_SGILOG color data is stored as:
 *
 *	 1       15           8        8
 *	|-+---------------|--------+--------|
 *	 S       Le           ue       ve
 *
 * For the 24-bit COMPRESSION_SGILOG24 color format, the data is stored as:
 *
 *	     10           14
 *	|----------|--------------|
 *	     Le'          Ce
 *
 * There is no sign bit in the 24-bit case, and the (u,v) chromaticity is
 * encoded as an index for optimal color resolution.  The 10 log bits are
 * defined by the following conversions:
 *
 *	L = 2^((Le'+.5)/64 - 12)		# real from 10-bit
 *
 *	Le' = floor( 64*(log2(L) + 12) )	# 10-bit from real
 *
 * The 10 bits of the smaller format may be converted into the 15 bits of
 * the larger format by multiplying by 4 and adding 13314.  Obviously,
 * a smaller range of magnitudes is covered (about 5 orders of magnitude
 * instead of 38), and the lack of a sign bit means that negative luminances
 * are not allowed.  (Well, they aren't allowed in the real world, either,
 * but they are useful for certain types of image processing.)
 *
 * The desired user format is controlled by the setting the internal
 * pseudo tag TIFFTAG_SGILOGDATAFMT to one of:
 *  SGILOGDATAFMT_FLOAT       = IEEE 32-bit float XYZ values
 *  SGILOGDATAFMT_16BIT	      = 16-bit integer encodings of logL, u and v
 * Raw data i/o is also possible using:
 *  SGILOGDATAFMT_RAW         = 32-bit unsigned integer with encoded pixel
 * In addition, the following decoding is provided for ease of display:
 *  SGILOGDATAFMT_8BIT        = 8-bit default RGB gamma-corrected values
 *
 * For grayscale images, we provide the following data formats:
 *  SGILOGDATAFMT_FLOAT       = IEEE 32-bit float Y values
 *  SGILOGDATAFMT_16BIT       = 16-bit integer w/ encoded luminance
 *  SGILOGDATAFMT_8BIT        = 8-bit gray monitor values
 *
 * Note that the COMPRESSION_SGILOG applies a simple run-length encoding
 * scheme by separating the logL, u and v bytes for each row and applying
 * a PackBits type of compression.  Since the 24-bit encoding is not
 * adaptive, the 32-bit color format takes less space in many cases.
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>

/*
 * State block for each open TIFF
 * file using LogLuv compression/decompression.
 */
typedef	struct logLuvState LogLuvState;

struct logLuvState {
	int			user_datafmt;	/* user data format */
	int			pixel_size;	/* bytes per pixel */

	tidata_t*		tbuf;		/* translation buffer */
	short			tbuflen;	/* buffer length */
	void (*tfunc)(LogLuvState*, tidata_t, int);

	TIFFVSetMethod		vgetparent;	/* super-class method */
	TIFFVSetMethod		vsetparent;	/* super-class method */
};

#define	DecoderState(tif)	((LogLuvState*) (tif)->tif_data)
#define	EncoderState(tif)	((LogLuvState*) (tif)->tif_data)

#define N(a)   (sizeof(a)/sizeof(a[0]))
#define SGILOGDATAFMT_UNKNOWN	-1

#define MINRUN		4	/* minimum run length */

/*
 * Decode a string of 16-bit gray pixels.
 */
static int
LogL16Decode(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	LogLuvState* sp = DecoderState(tif);
	int shft, i, npixels;
	u_char* bp;
	int16* tp;
	int16 b;
	int cc, rc;

	assert(s == 0);
	assert(sp != NULL);

	npixels = occ / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_16BIT)
		tp = (int16*) op;
	else {
		assert(sp->tbuflen >= npixels);
		tp = (int16*) sp->tbuf;
	}
	_TIFFmemset((tdata_t) tp, 0, npixels*sizeof (tp[0]));

	bp = (u_char*) tif->tif_rawcp;
	cc = tif->tif_rawcc;
					/* get each byte string */
	for (shft = 2*8; (shft -= 8) >= 0; ) {
		for (i = 0; i < npixels && cc > 0; )
			if (*bp >= 128) {		/* run */
				rc = *bp++ + (2-128);
				b = (int16)*bp++ << shft;
				cc -= 2;
				while (rc--)
					tp[i++] |= b;
			} else {			/* non-run */
				rc = *bp++;		/* nul is noop */
				while (--cc && rc--)
					tp[i++] |= (int16)*bp++ << shft;
			}
		if (i != npixels) {
			TIFFError(tif->tif_name,
		"LogL16Decode: Not enough data at row %d (short %d pixels)",
			    tif->tif_row, npixels - i);
			tif->tif_rawcp = (tidata_t) bp;
			tif->tif_rawcc = cc;
			return (0);
		}
	}
	(*sp->tfunc)(sp, op, npixels);
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	return (1);
}

/*
 * Decode a string of 24-bit pixels.
 */
static int
LogLuvDecode24(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	LogLuvState* sp = DecoderState(tif);
	int cc, i, npixels;
	u_char* bp;
	uint32* tp;

	assert(s == 0);
	assert(sp != NULL);

	npixels = occ / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_RAW)
		tp = (uint32 *)op;
	else {
		assert(sp->tbuflen >= npixels);
		tp = (uint32 *) sp->tbuf;
	}
	_TIFFmemset((tdata_t) tp, 0, npixels*sizeof (tp[0]));
					/* copy to array of uint32 */
	bp = (u_char*) tif->tif_rawcp;
	cc = tif->tif_rawcc;
	for (i = 0; i < npixels && cc > 0; i++) {
		tp[i] = bp[0] << 16 | bp[1] << 8 | bp[2];
		bp += 3;
		cc -= 3;
	}
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	if (i != npixels) {
		TIFFError(tif->tif_name,
	    "LogLuvDecode24: Not enough data at row %d (short %d pixels)",
		    tif->tif_row, npixels - i);
		return (0);
	}
	(*sp->tfunc)(sp, op, npixels);
	return (1);
}

/*
 * Decode a string of 32-bit pixels.
 */
static int
LogLuvDecode32(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	LogLuvState* sp;
	int shft, i, npixels;
	u_char* bp;
	uint32* tp;
	uint32 b;
	int cc, rc;

	assert(s == 0);
	sp = DecoderState(tif);
	assert(sp != NULL);

	npixels = occ / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_RAW)
		tp = (uint32*) op;
	else {
		assert(sp->tbuflen >= npixels);
		tp = (uint32*) sp->tbuf;
	}
	_TIFFmemset((tdata_t) tp, 0, npixels*sizeof (tp[0]));

	bp = (u_char*) tif->tif_rawcp;
	cc = tif->tif_rawcc;
					/* get each byte string */
	for (shft = 4*8; (shft -= 8) >= 0; ) {
		for (i = 0; i < npixels && cc > 0; )
			if (*bp >= 128) {		/* run */
				rc = *bp++ + (2-128);
				b = (uint32)*bp++ << shft;
				cc -= 2;
				while (rc--)
					tp[i++] |= b;
			} else {			/* non-run */
				rc = *bp++;		/* nul is noop */
				while (--cc && rc--)
					tp[i++] |= (uint32)*bp++ << shft;
			}
		if (i != npixels) {
			TIFFError(tif->tif_name,
		"LogLuvDecode32: Not enough data at row %d (short %d pixels)",
			    tif->tif_row, npixels - i);
			tif->tif_rawcp = (tidata_t) bp;
			tif->tif_rawcc = cc;
			return (0);
		}
	}
	(*sp->tfunc)(sp, op, npixels);
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	return (1);
}

/*
 * Decode a strip of pixels.  We break it into rows to
 * maintain synchrony with the encode algorithm, which
 * is row by row.
 */
static int
LogLuvDecodeStrip(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	tsize_t rowlen = TIFFScanlineSize(tif);

	assert(cc%rowlen == 0);
	while (cc && (*tif->tif_decoderow)(tif, bp, rowlen, s))
		bp += rowlen, cc -= rowlen;
	return (cc == 0);
}

/*
 * Decode a tile of pixels.  We break it into rows to
 * maintain synchrony with the encode algorithm, which
 * is row by row.
 */
static int
LogLuvDecodeTile(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	tsize_t rowlen = TIFFTileRowSize(tif);

	assert(cc%rowlen == 0);
	while (cc && (*tif->tif_decoderow)(tif, bp, rowlen, s))
		bp += rowlen, cc -= rowlen;
	return (cc == 0);
}

/*
 * Encode a row of 16-bit pixels.
 */
static int
LogL16Encode(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	LogLuvState* sp = EncoderState(tif);
	int shft, i, j, npixels;
	tidata_t op;
	int16* tp;
	int16 b;
	int occ, rc=0, mask, beg;

	assert(s == 0);
	assert(sp != NULL);
	npixels = cc / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_16BIT)
		tp = (int16*) bp;
	else {
		tp = (int16*) sp->tbuf;
		assert(sp->tbuflen >= npixels);
		(*sp->tfunc)(sp, bp, npixels);
	}
					/* compress each byte string */
	op = tif->tif_rawcp;
	occ = tif->tif_rawdatasize - tif->tif_rawcc;
	for (shft = 2*8; (shft -= 8) >= 0; )
		for (i = 0; i < npixels; i += rc) {
			if (occ < 4) {
				tif->tif_rawcp = op;
				tif->tif_rawcc = tif->tif_rawdatasize - occ;
				if (!TIFFFlushData1(tif))
					return (-1);
				op = tif->tif_rawcp;
				occ = tif->tif_rawdatasize - tif->tif_rawcc;
			}
			mask = 0xff << shft;		/* find next run */
			for (beg = i; beg < npixels; beg += rc) {
				b = tp[beg] & mask;
				rc = 1;
				while (rc < 127+2 && beg+rc < npixels &&
						(tp[beg+rc] & mask) == b)
					rc++;
				if (rc >= MINRUN)
					break;		/* long enough */
			}
			if (beg-i > 1 && beg-i < MINRUN) {
				b = tp[i] & mask;	/* check short run */
				j = i+1;
				while ((tp[j++] & mask) == b)
					if (j == beg) {
						*op++ = 128-2+j-i;
						*op++ = b >> shft;
						occ -= 2;
						i = beg;
						break;
					}
			}
			while (i < beg) {		/* write out non-run */
				if ((j = beg-i) > 127) j = 127;
				if (occ < j+3) {
					tif->tif_rawcp = op;
					tif->tif_rawcc = tif->tif_rawdatasize - occ;
					if (!TIFFFlushData1(tif))
						return (-1);
					op = tif->tif_rawcp;
					occ = tif->tif_rawdatasize - tif->tif_rawcc;
				}
				*op++ = j; occ--;
				while (j--) {
					*op++ = tp[i++] >> shft & 0xff;
					occ--;
				}
			}
			if (rc >= MINRUN) {		/* write out run */
				*op++ = 128-2+rc;
				*op++ = tp[beg] >> shft & 0xff;
				occ -= 2;
			} else
				rc = 0;
		}
	tif->tif_rawcp = op;
	tif->tif_rawcc = tif->tif_rawdatasize - occ;

	return (0);
}

/*
 * Encode a row of 24-bit pixels.
 */
static int
LogLuvEncode24(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	LogLuvState* sp = EncoderState(tif);
	int i, npixels, occ;
	tidata_t op;
	uint32* tp;

	assert(s == 0);
	assert(sp != NULL);
	npixels = cc / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_RAW)
		tp = (uint32*) bp;
	else {
		tp = (uint32*) sp->tbuf;
		assert(sp->tbuflen >= npixels);
		(*sp->tfunc)(sp, bp, npixels);
	}
					/* write out encoded pixels */
	op = tif->tif_rawcp;
	occ = tif->tif_rawdatasize - tif->tif_rawcc;
	for (i = npixels; i--; ) {
		if (occ < 3) {
			tif->tif_rawcp = op;
			tif->tif_rawcc = tif->tif_rawdatasize - occ;
			if (!TIFFFlushData1(tif))
				return (-1);
			op = tif->tif_rawcp;
			occ = tif->tif_rawdatasize - tif->tif_rawcc;
		}
		*op++ = (tidataval_t)(*tp >> 16);
		*op++ = (tidataval_t)(*tp >> 8 & 0xff);
		*op++ = (tidataval_t)(*tp++ & 0xff);
		occ -= 3;
	}
	tif->tif_rawcp = op;
	tif->tif_rawcc = tif->tif_rawdatasize - occ;

	return (0);
}

/*
 * Encode a row of 32-bit pixels.
 */
static int
LogLuvEncode32(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	LogLuvState* sp = EncoderState(tif);
	int shft, i, j, npixels;
	tidata_t op;
	uint32* tp;
	uint32 b;
	int occ, rc=0, mask, beg;

	assert(s == 0);
	assert(sp != NULL);

	npixels = cc / sp->pixel_size;

	if (sp->user_datafmt == SGILOGDATAFMT_RAW)
		tp = (uint32*) bp;
	else {
		tp = (uint32*) sp->tbuf;
		assert(sp->tbuflen >= npixels);
		(*sp->tfunc)(sp, bp, npixels);
	}
					/* compress each byte string */
	op = tif->tif_rawcp;
	occ = tif->tif_rawdatasize - tif->tif_rawcc;
	for (shft = 4*8; (shft -= 8) >= 0; )
		for (i = 0; i < npixels; i += rc) {
			if (occ < 4) {
				tif->tif_rawcp = op;
				tif->tif_rawcc = tif->tif_rawdatasize - occ;
				if (!TIFFFlushData1(tif))
					return (-1);
				op = tif->tif_rawcp;
				occ = tif->tif_rawdatasize - tif->tif_rawcc;
			}
			mask = 0xff << shft;		/* find next run */
			for (beg = i; beg < npixels; beg += rc) {
				b = tp[beg] & mask;
				rc = 1;
				while (rc < 127+2 && beg+rc < npixels &&
						(tp[beg+rc] & mask) == b)
					rc++;
				if (rc >= MINRUN)
					break;		/* long enough */
			}
			if (beg-i > 1 && beg-i < MINRUN) {
				b = tp[i] & mask;	/* check short run */
				j = i+1;
				while ((tp[j++] & mask) == b)
					if (j == beg) {
						*op++ = (tidataval_t)(128-2+j-i);
						*op++ = (tidataval_t)(b >> shft);
						occ -= 2;
						i = beg;
						break;
					}
			}
			while (i < beg) {		/* write out non-run */
				if ((j = beg-i) > 127) j = 127;
				if (occ < j+3) {
					tif->tif_rawcp = op;
					tif->tif_rawcc = tif->tif_rawdatasize - occ;
					if (!TIFFFlushData1(tif))
						return (-1);
					op = tif->tif_rawcp;
					occ = tif->tif_rawdatasize - tif->tif_rawcc;
				}
				*op++ = j; occ--;
				while (j--) {
					*op++ = (tidataval_t)(tp[i++] >> shft & 0xff);
					occ--;
				}
			}
			if (rc >= MINRUN) {		/* write out run */
				*op++ = 128-2+rc;
				*op++ = (tidataval_t)(tp[beg] >> shft & 0xff);
				occ -= 2;
			} else
				rc = 0;
		}
	tif->tif_rawcp = op;
	tif->tif_rawcc = tif->tif_rawdatasize - occ;

	return (0);
}

/*
 * Encode a strip of pixels.  We break it into rows to
 * avoid encoding runs across row boundaries.
 */
static int
LogLuvEncodeStrip(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	tsize_t rowlen = TIFFScanlineSize(tif);

	assert(cc%rowlen == 0);
	while (cc && (*tif->tif_encoderow)(tif, bp, rowlen, s) == 0)
		bp += rowlen, cc -= rowlen;
	return (cc == 0);
}

/*
 * Encode a tile of pixels.  We break it into rows to
 * avoid encoding runs across row boundaries.
 */
static int
LogLuvEncodeTile(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	tsize_t rowlen = TIFFTileRowSize(tif);

	assert(cc%rowlen == 0);
	while (cc && (*tif->tif_encoderow)(tif, bp, rowlen, s) == 0)
		bp += rowlen, cc -= rowlen;
	return (cc == 0);
}

/*
 * Encode/Decode functions for converting to and from user formats.
 */
#include "uvcode.h"

#define U_NEU	0.210526316
#define V_NEU	0.473684211

#ifdef	M_LN2
#define	LOGOF2		M_LN2
#else
#define LOGOF2		0.69314718055994530942
#endif
#define log2(x)		((1./LOGOF2)*log(x))
#define exp2(x)		exp(LOGOF2*(x))

#define UVSCALE		410.

static double
pix16toY(int p16)
{
	int	Le = p16 & 0x7fff;
	double	Y;

	if (!Le)
		return (0.);
	Y = exp(LOGOF2/256.*(Le+.5) - LOGOF2*64.);
	if (p16 & 0x8000)
		return (-Y);
	return (Y);
}

static int
pix16fromY(double Y)
{
	if (Y >= 1.84467e19)
		return (0x7fff);
	if (Y <= -1.84467e19)
		return (0xffff);
	if (Y > 5.43571e-20)
		return (int)(256.*(log2(Y) + 64.));
	if (Y < -5.43571e-20)
		return (~0x7fff | (int)(256.*(log2(-Y) + 64.)));
	return (0);
}

static void
L16toY(LogLuvState* sp, tidata_t op, int n)
{
	int16* l16 = (int16*) sp->tbuf;
	float* yp = (float*) op;

	while (n-- > 0)
		*yp++ = (float)pix16toY(*l16++);
}

static void
L16toGry(LogLuvState* sp, tidata_t op, int n)
{
	int16* l16 = (int16*) sp->tbuf;
	uint8* gp = (uint8*) op;

	while (n-- > 0) {
		double Y = pix16toY(*l16++);
		*gp++ = (Y <= 0.) ? 0 : (Y >= 1.) ? 255 : (int)(256.*sqrt(Y));
	}
}

static void
L16fromY(LogLuvState* sp, tidata_t op, int n)
{
	int16* l16 = (int16*) sp->tbuf;
	float* yp = (float*) op;

	while (n-- > 0)
		*l16++ = pix16fromY(*yp++);
}

static void
XYZtoRGB24(float xyz[3], uint8 rgb[3])
{
	double	r, g, b;
					/* assume CCIR-709 primaries */
	r =  2.690*xyz[0] + -1.276*xyz[1] + -0.414*xyz[2];
	g = -1.022*xyz[0] +  1.978*xyz[1] +  0.044*xyz[2];
	b =  0.061*xyz[0] + -0.224*xyz[1] +  1.163*xyz[2];
					/* assume 2.0 gamma for speed */
	/* could use integer sqrt approx., but this is probably faster */
	rgb[0] = (r <= 0.) ? 0 : (r >= 1.) ? 255 : (int)(256.*sqrt(r));
	rgb[1] = (g <= 0.) ? 0 : (g >= 1.) ? 255 : (int)(256.*sqrt(g));
	rgb[2] = (b <= 0.) ? 0 : (b >= 1.) ? 255 : (int)(256.*sqrt(b));
}

static int
uv_encode(double u, double v)		/* encode (u',v') coordinates */
{
	register int	vi, ui;

	if (v < UV_VSTART)
		return(-1);
	vi = (int)((v - UV_VSTART)*(1./UV_SQSIZ));
	if (vi >= UV_NVS)
		return(-1);
	if (u < uv_row[vi].ustart)
		return(-1);
	ui = (int)((u - uv_row[vi].ustart)*(1./UV_SQSIZ));
	if (ui >= uv_row[vi].nus)
		return(-1);
	return(uv_row[vi].ncum + ui);
}

static int
uv_decode(double *up, double *vp, int c)	/* decode (u',v') index */
{
	int	upper, lower;
	register int	ui, vi;

	if (c < 0 || c >= UV_NDIVS)
		return(-1);
	lower = 0;			/* binary search */
	upper = UV_NVS;
	do {
		vi = (lower + upper) >> 1;
		ui = c - uv_row[vi].ncum;
		if (ui > 0)
			lower = vi;
		else if (ui < 0)
			upper = vi;
		else
			break;
	} while (upper - lower > 1);
	vi = lower;
	ui = c - uv_row[vi].ncum;
	*up = uv_row[vi].ustart + (ui+.5)*UV_SQSIZ;
	*vp = UV_VSTART + (vi+.5)*UV_SQSIZ;
	return(0);
}

static void
pix24toXYZ(uint32 p, float XYZ[3])
{
	int	Le, Ce;
	double	L, u, v, s, x, y;
					/* decode luminance */
	Le = p >> 14 & 0x3ff;
	if (Le == 0) {
		XYZ[0] = XYZ[1] = XYZ[2] = 0.;
		return;
	}
	L = exp(LOGOF2/64.*(Le+.5) - LOGOF2*12.);
					/* decode color */
	Ce = p & 0x3fff;
	if (uv_decode(&u, &v, Ce) < 0) {
		u = U_NEU; v = V_NEU;
	}
	s = 1./(6.*u - 16.*v + 12.);
	x = 9.*u * s;
	y = 4.*v * s;
					/* convert to XYZ */
	XYZ[0] = (float)(x/y * L);
	XYZ[1] = (float)L;
	XYZ[2] = (float)((1.-x-y)/y * L);
}

static uint32
pix24fromXYZ(float XYZ[3])
{
	int	Le, Ce;
	double	L, u, v, s;
					/* encode luminance */
	L = XYZ[1];
	if (L >= 16.)
		Le = 0x3ff;
	else if (L <= 1./4096.)
		Le = 0;
	else
		Le = (int)(64.*(log2(L) + 12.));
					/* encode color */
	s = XYZ[0] + 15.*XYZ[1] + 3.*XYZ[2];
	if (s == 0.) {
		u = U_NEU;
		v = V_NEU;
	} else {
		u = 4.*XYZ[0] / s;
		v = 9.*XYZ[1] / s;
	}
	Ce = uv_encode(u, v);
	if (Ce < 0)
		Ce = uv_encode(U_NEU, V_NEU);
					/* combine encodings */
	return (Le << 14 | Ce);
}

static void
Luv24toXYZ(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	float* xyz = (float*) op;

	while (n-- > 0) {
		pix24toXYZ(*luv, xyz);
		xyz += 3;
		luv++;
	}
}

static void
Luv24toLuv48(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	int16* luv3 = (int16*) op;

	while (n-- > 0) {
		double u, v;

		*luv3++ = (int16)((*luv >> 12 & 0xffd) + 13314);
		if (uv_decode(&u, &v, *luv&0x3fff) < 0) {
			u = U_NEU;
			v = V_NEU;
		}
		*luv3++ = (int16)(u * (1L<<15));
		*luv3++ = (int16)(v * (1L<<15));
		luv++;
	}
}

static void
Luv24toRGB(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	uint8* rgb = (uint8*) op;

	while (n-- > 0) {
		float xyz[3];

		pix24toXYZ(*luv++, xyz);
		XYZtoRGB24(xyz, rgb);
		rgb += 3;
	}
}

static void
Luv24fromXYZ(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	float* xyz = (float*) op;

	while (n-- > 0) {
		*luv++ = pix24fromXYZ(xyz);
		xyz += 3;
	}
}

static void
Luv24fromLuv48(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	int16* luv3 = (int16*) op;

	while (n-- > 0) {
		int Le, Ce;

		if (luv3[0] <= 0)
			Le = 0;
		else if (luv3[0] >= (1<<12)+3314)
			Le = (1<<10) - 1;
		else
			Le = (luv3[0]-3314) >> 2;
		Ce = uv_encode((luv[1]+.5)/(1<<15), (luv[2]+.5)/(1<<15));
		if (Ce < 0)
			Ce = uv_encode(U_NEU, V_NEU);
		*luv++ = (uint32)Le << 14 | Ce;
		luv3 += 3;
	}
}

static void
pix32toXYZ(uint32 p, float XYZ[3])
{
	double	L, u, v, s, x, y;
					/* decode luminance */
	L = pix16toY((int)p >> 16);
	if (L == 0.) {
		XYZ[0] = XYZ[1] = XYZ[2] = 0.;
		return;
	}
					/* decode color */
	u = 1./UVSCALE * ((p>>8 & 0xff) + .5);
	v = 1./UVSCALE * ((p & 0xff) + .5);
	s = 1./(6.*u - 16.*v + 12.);
	x = 9.*u * s;
	y = 4.*v * s;
					/* convert to XYZ */
	XYZ[0] = (float)(x/y * L);
	XYZ[1] = (float)L;
	XYZ[2] = (float)((1.-x-y)/y * L);
}

static uint32
pix32fromXYZ(float XYZ[3])
{
	unsigned int	Le, ue, ve;
	double	u, v, s;
					/* encode luminance */
	Le = (unsigned int)pix16fromY(XYZ[1]);
					/* encode color */
	s = XYZ[0] + 15.*XYZ[1] + 3.*XYZ[2];
	if (s == 0.) {
		u = U_NEU;
		v = V_NEU;
	} else {
		u = 4.*XYZ[0] / s;
		v = 9.*XYZ[1] / s;
	}
	if (u <= 0.) ue = 0;
	else ue = (unsigned int)(UVSCALE * u);
	if (ue > 255) ue = 255;
	if (v <= 0.) ve = 0;
	else ve = (unsigned int)(UVSCALE * v);
	if (ve > 255) ve = 255;
					/* combine encodings */
	return (Le << 16 | ue << 8 | ve);
}

static void
Luv32toXYZ(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	float* xyz = (float*) op;

	while (n-- > 0) {
		pix32toXYZ(*luv++, xyz);
		xyz += 3;
	}
}

static void
Luv32toLuv48(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	int16* luv3 = (int16*) op;

	while (n-- > 0) {
		double u, v;

		*luv3++ = (int16)(*luv >> 16);
		u = 1./UVSCALE * ((*luv>>8 & 0xff) + .5);
		v = 1./UVSCALE * ((*luv & 0xff) + .5);
		*luv3++ = (int16)(u * (1L<<15));
		*luv3++ = (int16)(v * (1L<<15));
		luv++;
	}
}

static void
Luv32toRGB(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	uint8* rgb = (uint8*) op;

	while (n-- > 0) {
		float xyz[3];

		pix32toXYZ(*luv++, xyz);
		XYZtoRGB24(xyz, rgb);
		rgb += 3;
	}
}

static void
Luv32fromXYZ(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	float* xyz = (float*) op;

	while (n-- > 0) {
		*luv++ = pix32fromXYZ(xyz);
		xyz += 3;
	}
}

static void
Luv32fromLuv48(LogLuvState* sp, tidata_t op, int n)
{
	uint32* luv = (uint32*) sp->tbuf;
	int16* luv3 = (int16*) op;

	while (n-- > 0) {
		*luv++ = (uint32)luv3[0] << 16 |
			(luv3[1]*(uint32)(UVSCALE+.5) >> 7 & 0xff00) |
			(luv3[2]*(uint32)(UVSCALE+.5) >> 15 & 0xff);
		luv3 += 3;
	}
}

static void
_logLuvNop(LogLuvState* sp, tidata_t op, int n)
{
	(void) sp; (void) op; (void) n;
}

static int
LogL16GuessDataFmt(TIFFDirectory *td)
{
#define	PACK(s,b,f)	(((b)<<6)|((s)<<3)|(f))
	switch (PACK(td->td_samplesperpixel, td->td_bitspersample, td->td_sampleformat)) {
	case PACK(1, 32, SAMPLEFORMAT_IEEEFP):
		return (SGILOGDATAFMT_FLOAT);
	case PACK(1, 16, SAMPLEFORMAT_VOID):
	case PACK(1, 16, SAMPLEFORMAT_INT):
	case PACK(1, 16, SAMPLEFORMAT_UINT):
		return (SGILOGDATAFMT_16BIT);
	case PACK(1,  8, SAMPLEFORMAT_VOID):
	case PACK(1,  8, SAMPLEFORMAT_UINT):
		return (SGILOGDATAFMT_8BIT);
	}
#undef PACK
	return (SGILOGDATAFMT_UNKNOWN);
}

static int
LogL16InitState(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	LogLuvState* sp = DecoderState(tif);
	static const char module[] = "LogL16InitState";

	assert(sp != NULL);
	assert(td->td_photometric == PHOTOMETRIC_LOGL);

	/* for some reason, we can't do this in TIFFInitLogL16 */
	if (sp->user_datafmt == SGILOGDATAFMT_UNKNOWN)
		sp->user_datafmt = LogL16GuessDataFmt(td);
	switch (sp->user_datafmt) {
	case SGILOGDATAFMT_FLOAT:
		sp->pixel_size = sizeof (float);
		break;
	case SGILOGDATAFMT_16BIT:
		sp->pixel_size = sizeof (int16);
		break;
	case SGILOGDATAFMT_8BIT:
		sp->pixel_size = sizeof (uint8);
		break;
	default:
		TIFFError(tif->tif_name,
		    "No support for converting user data format to LogL");
		return (0);
	}
	sp->tbuflen = (short)(td->td_imagewidth * td->td_rowsperstrip);
	sp->tbuf = (tidata_t*) _TIFFmalloc(sp->tbuflen * sizeof (int16));
	if (sp->tbuf == NULL) {
		TIFFError(module, "%s: No space for SGILog translation buffer",
		    tif->tif_name);
		return (0);
	}
	return (1);
}

static int
LogLuvGuessDataFmt(TIFFDirectory *td)
{
	int guess;

	/*
	 * If the user didn't tell us their datafmt,
	 * take our best guess from the bitspersample.
	 */
#define	PACK(a,b)	(((a)<<3)|(b))
	switch (PACK(td->td_bitspersample, td->td_sampleformat)) {
	case PACK(32, SAMPLEFORMAT_IEEEFP):
		guess = SGILOGDATAFMT_FLOAT;
		break;
	case PACK(32, SAMPLEFORMAT_VOID):
	case PACK(32, SAMPLEFORMAT_UINT):
	case PACK(32, SAMPLEFORMAT_INT):
		guess = SGILOGDATAFMT_RAW;
		break;
	case PACK(16, SAMPLEFORMAT_VOID):
	case PACK(16, SAMPLEFORMAT_INT):
	case PACK(16, SAMPLEFORMAT_UINT):
		guess = SGILOGDATAFMT_16BIT;
		break;
	case PACK( 8, SAMPLEFORMAT_VOID):
	case PACK( 8, SAMPLEFORMAT_UINT):
		guess = SGILOGDATAFMT_8BIT;
		break;
	default:
		guess = SGILOGDATAFMT_UNKNOWN;
		break;
#undef PACK
	}
	/*
	 * Double-check samples per pixel.
	 */
	switch (td->td_samplesperpixel) {
	case 1:
		if (guess != SGILOGDATAFMT_RAW)
			guess = SGILOGDATAFMT_UNKNOWN;
		break;
	case 3:
		if (guess == SGILOGDATAFMT_RAW)
			guess = SGILOGDATAFMT_UNKNOWN;
		break;
	default:
		guess = SGILOGDATAFMT_UNKNOWN;
		break;
	}
	return (guess);
}

static int
LogLuvInitState(TIFF* tif)
{
	TIFFDirectory* td = &tif->tif_dir;
	LogLuvState* sp = DecoderState(tif);
	static const char module[] = "LogLuvInitState";

	assert(sp != NULL);
	assert(td->td_photometric == PHOTOMETRIC_LOGLUV);

	/* for some reason, we can't do this in TIFFInitLogLuv */
	if (td->td_planarconfig != PLANARCONFIG_CONTIG) {
		TIFFError(module,
		    "SGILog compression cannot handle non-contiguous data");
		return (0);
	}
	if (sp->user_datafmt == SGILOGDATAFMT_UNKNOWN)
		sp->user_datafmt = LogLuvGuessDataFmt(td);
	switch (sp->user_datafmt) {
	case SGILOGDATAFMT_FLOAT:
		sp->pixel_size = 3*sizeof (float);
		break;
	case SGILOGDATAFMT_16BIT:
		sp->pixel_size = 3*sizeof (int16);
		break;
	case SGILOGDATAFMT_RAW:
		sp->pixel_size = sizeof (uint32);
		break;
	case SGILOGDATAFMT_8BIT:
		sp->pixel_size = 3*sizeof (uint8);
		break;
	default:
		TIFFError(tif->tif_name,
		    "No support for converting user data format to LogLuv");
		return (0);
	}
	sp->tbuflen = (short)(td->td_imagewidth * td->td_rowsperstrip);
	sp->tbuf = (tidata_t*) _TIFFmalloc(sp->tbuflen * sizeof (uint32));
	if (sp->tbuf == NULL) {
		TIFFError(module, "%s: No space for SGILog translation buffer",
		    tif->tif_name);
		return (0);
	}
	return (1);
}

static int
LogLuvSetupDecode(TIFF* tif)
{
	LogLuvState* sp = DecoderState(tif);
	TIFFDirectory* td = &tif->tif_dir;

	tif->tif_postdecode = _TIFFNoPostDecode;
	switch (td->td_photometric) {
	case PHOTOMETRIC_LOGLUV:
		if (!LogLuvInitState(tif))
			break;
		if (td->td_compression == COMPRESSION_SGILOG24) {
			tif->tif_decoderow = LogLuvDecode24;
			switch (sp->user_datafmt) {
			case SGILOGDATAFMT_FLOAT:
				sp->tfunc = Luv24toXYZ;
				break;
			case SGILOGDATAFMT_16BIT:
				sp->tfunc = Luv24toLuv48;
				break;
			case SGILOGDATAFMT_8BIT:
				sp->tfunc = Luv24toRGB;
				break;
			}
		} else {
			tif->tif_decoderow = LogLuvDecode32;
			switch (sp->user_datafmt) {
			case SGILOGDATAFMT_FLOAT:
				sp->tfunc = Luv32toXYZ;
				break;
			case SGILOGDATAFMT_16BIT:
				sp->tfunc = Luv32toLuv48;
				break;
			case SGILOGDATAFMT_8BIT:
				sp->tfunc = Luv32toRGB;
				break;
			}
		}
		return (1);
	case PHOTOMETRIC_LOGL:
		if (!LogL16InitState(tif))
			break;
		tif->tif_decoderow = LogL16Decode;
		switch (sp->user_datafmt) {
		case SGILOGDATAFMT_FLOAT:
			sp->tfunc = L16toY;
			break;
		case SGILOGDATAFMT_8BIT:
			sp->tfunc = L16toGry;
			break;
		}
		return (1);
	default:
		TIFFError(tif->tif_name,
    "Inappropriate photometric interpretation %d for SGILog compression; %s",
		    td->td_photometric, "must be either LogLUV or LogL");
		break;
	}
	return (0);
}

static int
LogLuvSetupEncode(TIFF* tif)
{
	LogLuvState* sp = EncoderState(tif);
	TIFFDirectory* td = &tif->tif_dir;

	switch (td->td_photometric) {
	case PHOTOMETRIC_LOGLUV:
		if (!LogLuvInitState(tif))
			break;
		if (td->td_compression == COMPRESSION_SGILOG24) {
			tif->tif_encoderow = LogLuvEncode24;
			switch (sp->user_datafmt) {
			case SGILOGDATAFMT_FLOAT:
				sp->tfunc = Luv24fromXYZ;
				break;
			case SGILOGDATAFMT_16BIT:
				sp->tfunc = Luv24fromLuv48;
				break;
			case SGILOGDATAFMT_RAW:
				break;
			default:
				goto notsupported;
			}
		} else {
			tif->tif_encoderow = LogLuvEncode32;
			switch (sp->user_datafmt) {
			case SGILOGDATAFMT_FLOAT:
				sp->tfunc = Luv32fromXYZ;
				break;
			case SGILOGDATAFMT_16BIT:
				sp->tfunc = Luv32fromLuv48;
				break;
			case SGILOGDATAFMT_RAW:
				break;
			default:
				goto notsupported;
			}
		}
		break;
	case PHOTOMETRIC_LOGL:
		if (!LogL16InitState(tif))
			break;
		tif->tif_encoderow = LogL16Encode;
		switch (sp->user_datafmt) {
		case SGILOGDATAFMT_FLOAT:
			sp->tfunc = L16fromY;
			break;
		case SGILOGDATAFMT_16BIT:
			break;
		default:
			goto notsupported;
		}
		break;
	default:
		TIFFError(tif->tif_name,
    "Inappropriate photometric interpretation %d for SGILog compression; %s",
    		    td->td_photometric, "must be either LogLUV or LogL");
		break;
	}
	return (1);
notsupported:
	TIFFError(tif->tif_name,
	    "SGILog compression supported only for %s, or raw data",
	    td->td_photometric == PHOTOMETRIC_LOGL ? "Y, L" : "XYZ, Luv");
	return (0);
}

static void
LogLuvClose(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;

	/*
	 * For consistency, we always want to write out the same
	 * bitspersample and sampleformat for our TIFF file,
	 * regardless of the data format being used by the application.
	 * Since this routine is called after tags have been set but
	 * before they have been recorded in the file, we reset them here.
	 */
	td->td_samplesperpixel =
	    (td->td_photometric == PHOTOMETRIC_LOGL) ? 1 : 3;
	td->td_bitspersample = 16;
	td->td_sampleformat = SAMPLEFORMAT_INT;
}

static void
LogLuvCleanup(TIFF* tif)
{
	LogLuvState* sp = (LogLuvState *)tif->tif_data;

	if (sp) {
		if (sp->tbuf)
			_TIFFfree(sp->tbuf);
		_TIFFfree(sp);
		tif->tif_data = NULL;
	}
}

static int
LogLuvVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	LogLuvState* sp = DecoderState(tif);
	int bps, fmt;

	switch (tag) {
	case TIFFTAG_SGILOGDATAFMT:
		sp->user_datafmt = va_arg(ap, int);
		/*
		 * Tweak the TIFF header so that the rest of libtiff knows what
		 * size of data will be passed between app and library, and
		 * assume that the app knows what it is doing and is not
		 * confused by these header manipulations...
		 */
		switch (sp->user_datafmt) {
		case SGILOGDATAFMT_FLOAT:
			bps = 32, fmt = SAMPLEFORMAT_IEEEFP;
			break;
		case SGILOGDATAFMT_16BIT:
			bps = 16, fmt = SAMPLEFORMAT_INT;
			break;
		case SGILOGDATAFMT_RAW:
			bps = 32, fmt = SAMPLEFORMAT_UINT;
			break;
		case SGILOGDATAFMT_8BIT:
			bps = 8, fmt = SAMPLEFORMAT_UINT;
			break;
		default:
			TIFFError(tif->tif_name,
			    "Unknown data format %d for LogLuv compression",
			    sp->user_datafmt);
			return (0);
		}
		TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
		TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, fmt);
		/*
		 * Must recalculate sizes should bits/sample change.
		 */
		tif->tif_tilesize = TIFFTileSize(tif);
		tif->tif_scanlinesize = TIFFScanlineSize(tif);
		return (1);
	default:
		return (*sp->vsetparent)(tif, tag, ap);
	}
}

static int
LogLuvVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	LogLuvState *sp = (LogLuvState *)tif->tif_data;

	switch (tag) {
	case TIFFTAG_SGILOGDATAFMT:
		*va_arg(ap, int*) = sp->user_datafmt;
		return (1);
	default:
		return (*sp->vgetparent)(tif, tag, ap);
	}
}

static const TIFFFieldInfo LogLuvFieldInfo[] = {
    { TIFFTAG_SGILOGDATAFMT,	  0, 0,	TIFF_SHORT,	FIELD_PSEUDO,
      TRUE,	FALSE,	"SGILogDataFmt"}
};

int
TIFFInitSGILog(TIFF* tif, int scheme)
{
	static const char module[] = "TIFFInitSGILog";
	LogLuvState* sp;

	assert(scheme == COMPRESSION_SGILOG24 || scheme == COMPRESSION_SGILOG);

	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	tif->tif_data = (tidata_t) _TIFFmalloc(sizeof (LogLuvState));
	if (tif->tif_data == NULL)
		goto bad;
	sp = (LogLuvState*) tif->tif_data;
	memset(sp, 0, sizeof (*sp));
	sp->user_datafmt = SGILOGDATAFMT_UNKNOWN;
	sp->tfunc = _logLuvNop;

	/*
	 * Install codec methods.
	 * NB: tif_decoderow & tif_encoderow are filled
	 *     in at setup time.
	 */
	tif->tif_setupdecode = LogLuvSetupDecode;
	tif->tif_decodestrip = LogLuvDecodeStrip;
	tif->tif_decodetile = LogLuvDecodeTile;
	tif->tif_setupencode = LogLuvSetupEncode;
	tif->tif_encodestrip = LogLuvEncodeStrip;
	tif->tif_encodetile = LogLuvEncodeTile;
	tif->tif_close = LogLuvClose;
	tif->tif_cleanup = LogLuvCleanup;

	/* override SetField so we can handle our private pseudo-tag */
	_TIFFMergeFieldInfo(tif, LogLuvFieldInfo, N(LogLuvFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = LogLuvVGetField;   /* hook for codec tags */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = LogLuvVSetField;   /* hook for codec tags */

	return (1);
bad:
	TIFFError(module, "%s: No space for LogLuv state block", tif->tif_name);
	return (0);
}
#endif /* LOGLUV_SUPPORT */


/* ==== tif_lzw.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_lzw.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef LZW_SUPPORT
/*
 * TIFF Library.
 * Rev 5.0 Lempel-Ziv & Welch Compression Support
 *
 * This code is derived from the compress program whose code is
 * derived from software contributed to Berkeley by James A. Woods,
 * derived from original work by Spencer Thomas and Joseph Orost.
 *
 * The original Berkeley copyright notice appears below in its entirety.
 */
#include "tif_predict.h"

#include <assert.h>
#include <stdio.h>

/*
 * NB: The 5.0 spec describes a different algorithm than Aldus
 *     implements.  Specifically, Aldus does code length transitions
 *     one code earlier than should be done (for real LZW).
 *     Earlier versions of this library implemented the correct
 *     LZW algorithm, but emitted codes in a bit order opposite
 *     to the TIFF spec.  Thus, to maintain compatibility w/ Aldus
 *     we interpret MSB-LSB ordered codes to be images written w/
 *     old versions of this library, but otherwise adhere to the
 *     Aldus "off by one" algorithm.
 *
 * Future revisions to the TIFF spec are expected to "clarify this issue".
 */
#define	LZW_COMPAT		/* include backwards compatibility code */
/*
 * Each strip of data is supposed to be terminated by a CODE_EOI.
 * If the following #define is included, the decoder will also
 * check for end-of-strip w/o seeing this code.  This makes the
 * library more robust, but also slower.
 */
#define	LZW_CHECKEOS		/* include checks for strips w/o EOI code */

#define MAXCODE(n)	((1L<<(n))-1)
/*
 * The TIFF spec specifies that encoded bit
 * strings range from 9 to 12 bits.
 */
#define	BITS_MIN	9		/* start with 9 bits */
#define	BITS_MAX	12		/* max of 12 bit strings */
/* predefined codes */
#define	CODE_CLEAR	256		/* code to clear string table */
#define	CODE_EOI	257		/* end-of-information code */
#define CODE_FIRST	258		/* first free code entry */
#define	CODE_MAX	MAXCODE(BITS_MAX)
#define	HSIZE		9001L		/* 91% occupancy */
#define	HSHIFT		(13-8)
#ifdef LZW_COMPAT
/* NB: +1024 is for compatibility with old files */
#define	CSIZE		(MAXCODE(BITS_MAX)+1024L)
#else
#define	CSIZE		(MAXCODE(BITS_MAX)+1L)
#endif

/*
 * State block for each open TIFF file using LZW
 * compression/decompression.  Note that the predictor
 * state block must be first in this data structure.
 */
typedef	struct {
	TIFFPredictorState predict;	/* predictor super class */

	u_short		nbits;		/* # of bits/code */
	u_short		maxcode;	/* maximum code for lzw_nbits */
	u_short		free_ent;	/* next free entry in hash table */
	long		nextdata;	/* next bits of i/o */
	long		nextbits;	/* # of valid bits in lzw_nextdata */
} LZWBaseState;

#define	lzw_nbits	base.nbits
#define	lzw_maxcode	base.maxcode
#define	lzw_free_ent	base.free_ent
#define	lzw_nextdata	base.nextdata
#define	lzw_nextbits	base.nextbits

/*
 * Decoding-specific state.
 */
typedef struct code_ent {
	struct code_ent *next;
	u_short	length;			/* string len, including this token */
	u_char	value;			/* data value */
	u_char	firstchar;		/* first token of string */
} code_t;

typedef	int (*decodeFunc)(TIFF*, tidata_t, tsize_t, tsample_t);

typedef struct {
	LZWBaseState base;
	long	dec_nbitsmask;		/* lzw_nbits 1 bits, right adjusted */
	long	dec_restart;		/* restart count */
#ifdef LZW_CHECKEOS
	long	dec_bitsleft;		/* available bits in raw data */
#endif
	decodeFunc dec_decode;		/* regular or backwards compatible */
	code_t*	dec_codep;		/* current recognized code */
	code_t*	dec_oldcodep;		/* previously recognized code */
	code_t*	dec_free_entp;		/* next free entry */
	code_t*	dec_maxcodep;		/* max available entry */
	code_t*	dec_codetab;		/* kept separate for small machines */
} LZWDecodeState;

/*
 * Encoding-specific state.
 */
typedef uint16 hcode_t;			/* codes fit in 16 bits */
typedef struct {
	long	hash;
	hcode_t	code;
} hash_t;

typedef struct {
	LZWBaseState base;
	int	enc_oldcode;		/* last code encountered */
	long	enc_checkpoint;		/* point at which to clear table */
#define CHECK_GAP	10000		/* enc_ratio check interval */
	long	enc_ratio;		/* current compression ratio */
	long	enc_incount;		/* (input) data bytes encoded */
	long	enc_outcount;		/* encoded (output) bytes */
	tidata_t enc_rawlimit;		/* bound on tif_rawdata buffer */
	hash_t*	enc_hashtab;		/* kept separate for small machines */
} LZWEncodeState;

#define	LZWState(tif)		((LZWBaseState*) (tif)->tif_data)
#define	DecoderState(tif)	((LZWDecodeState*) LZWState(tif))
#define	EncoderState(tif)	((LZWEncodeState*) LZWState(tif))

static	int LZWDecode(TIFF*, tidata_t, tsize_t, tsample_t);
#ifdef LZW_COMPAT
static	int LZWDecodeCompat(TIFF*, tidata_t, tsize_t, tsample_t);
#endif

/*
 * LZW Decoder.
 */

#ifdef LZW_CHECKEOS
/*
 * This check shouldn't be necessary because each
 * strip is suppose to be terminated with CODE_EOI.
 */
#define	NextCode(_tif, _sp, _bp, _code, _get) {				\
	if ((_sp)->dec_bitsleft < nbits) {				\
		TIFFWarning(_tif->tif_name,				\
		    "LZWDecode: Strip %d not terminated with EOI code", \
		    _tif->tif_curstrip);				\
		_code = CODE_EOI;					\
	} else {							\
		_get(_sp,_bp,_code);					\
		(_sp)->dec_bitsleft -= nbits;				\
	}								\
}
#else
#define	NextCode(tif, sp, bp, code, get) get(sp, bp, code)
#endif

static int
LZWSetupDecode(TIFF* tif)
{
	LZWDecodeState* sp = DecoderState(tif);
	static const char module[] = " LZWSetupDecode";
	int code;

	assert(sp != NULL);
	if (sp->dec_codetab == NULL) {
		sp->dec_codetab = (code_t*)_TIFFmalloc(CSIZE*sizeof (code_t));
		if (sp->dec_codetab == NULL) {
			TIFFError(module, "No space for LZW code table");
			return (0);
		}
		/*
		 * Pre-load the table.
		 */
		for (code = 255; code >= 0; code--) {
			sp->dec_codetab[code].value = code;
			sp->dec_codetab[code].firstchar = code;
			sp->dec_codetab[code].length = 1;
			sp->dec_codetab[code].next = NULL;
		}
	}
	return (1);
}

/*
 * Setup state for decoding a strip.
 */
static int
LZWPreDecode(TIFF* tif, tsample_t s)
{
	LZWDecodeState *sp = DecoderState(tif);

	(void) s;
	assert(sp != NULL);
	/*
	 * Check for old bit-reversed codes.
	 */
	if (tif->tif_rawdata[0] == 0 && (tif->tif_rawdata[1] & 0x1)) {
#ifdef LZW_COMPAT
		if (!sp->dec_decode) {
			TIFFWarning(tif->tif_name,
			    "Old-style LZW codes, convert file");
			/*
			 * Override default decoding methods with
			 * ones that deal with the old coding.
			 * Otherwise the predictor versions set
			 * above will call the compatibility routines
			 * through the dec_decode method.
			 */
			tif->tif_decoderow = LZWDecodeCompat;
			tif->tif_decodestrip = LZWDecodeCompat;
			tif->tif_decodetile = LZWDecodeCompat;
			/*
			 * If doing horizontal differencing, must
			 * re-setup the predictor logic since we
			 * switched the basic decoder methods...
			 */
			(*tif->tif_setupdecode)(tif);
			sp->dec_decode = LZWDecodeCompat;
		}
		sp->lzw_maxcode = MAXCODE(BITS_MIN);
#else /* !LZW_COMPAT */
		if (!sp->dec_decode) {
			TIFFError(tif->tif_name,
			    "Old-style LZW codes not supported");
			sp->dec_decode = LZWDecode;
		}
		return (0);
#endif/* !LZW_COMPAT */
	} else {
		sp->lzw_maxcode = MAXCODE(BITS_MIN)-1;
		sp->dec_decode = LZWDecode;
	}
	sp->lzw_nbits = BITS_MIN;
	sp->lzw_nextbits = 0;
	sp->lzw_nextdata = 0;

	sp->dec_restart = 0;
	sp->dec_nbitsmask = MAXCODE(BITS_MIN);
#ifdef LZW_CHECKEOS
	sp->dec_bitsleft = tif->tif_rawcc << 3;
#endif
	sp->dec_free_entp = sp->dec_codetab + CODE_FIRST;
	/*
	 * Zero entries that are not yet filled in.  We do
	 * this to guard against bogus input data that causes
	 * us to index into undefined entries.  If you can
	 * come up with a way to safely bounds-check input codes
	 * while decoding then you can remove this operation.
	 */
	_TIFFmemset(sp->dec_free_entp, 0, (CSIZE-CODE_FIRST)*sizeof (code_t));
	sp->dec_oldcodep = &sp->dec_codetab[-1];
	sp->dec_maxcodep = &sp->dec_codetab[sp->dec_nbitsmask-1];
	return (1);
}

/*
 * Decode a "hunk of data".
 */
#define	GetNextCode(sp, bp, code) {				\
	nextdata = (nextdata<<8) | *(bp)++;			\
	nextbits += 8;						\
	if (nextbits < nbits) {					\
		nextdata = (nextdata<<8) | *(bp)++;		\
		nextbits += 8;					\
	}							\
	code = (hcode_t)((nextdata >> (nextbits-nbits)) & nbitsmask);	\
	nextbits -= nbits;					\
}

static void
codeLoop(TIFF* tif)
{
	TIFFError(tif->tif_name,
	    "LZWDecode: Bogus encoding, loop in the code table; scanline %d",
	    tif->tif_row);
}

static int
LZWDecode(TIFF* tif, tidata_t op0, tsize_t occ0, tsample_t s)
{
	LZWDecodeState *sp = DecoderState(tif);
	char *op = (char*) op0;
	long occ = (long) occ0;
	char *tp;
	u_char *bp;
	hcode_t code;
	int len;
	long nbits, nextbits, nextdata, nbitsmask;
	code_t *codep, *free_entp, *maxcodep, *oldcodep;

	(void) s;
	assert(sp != NULL);
	/*
	 * Restart interrupted output operation.
	 */
	if (sp->dec_restart) {
		long residue;

		codep = sp->dec_codep;
		residue = codep->length - sp->dec_restart;
		if (residue > occ) {
			/*
			 * Residue from previous decode is sufficient
			 * to satisfy decode request.  Skip to the
			 * start of the decoded string, place decoded
			 * values in the output buffer, and return.
			 */
			sp->dec_restart += occ;
			do {
				codep = codep->next;
			} while (--residue > occ && codep);
			if (codep) {
				tp = op + occ;
				do {
					*--tp = codep->value;
					codep = codep->next;
				} while (--occ && codep);
			}
			return (1);
		}
		/*
		 * Residue satisfies only part of the decode request.
		 */
		op += residue, occ -= residue;
		tp = op;
		do {
			int t;
			--tp;
			t = codep->value;
			codep = codep->next;
			*tp = t;
		} while (--residue && codep);
		sp->dec_restart = 0;
	}

	bp = (u_char *)tif->tif_rawcp;
	nbits = sp->lzw_nbits;
	nextdata = sp->lzw_nextdata;
	nextbits = sp->lzw_nextbits;
	nbitsmask = sp->dec_nbitsmask;
	oldcodep = sp->dec_oldcodep;
	free_entp = sp->dec_free_entp;
	maxcodep = sp->dec_maxcodep;

	while (occ > 0) {
		NextCode(tif, sp, bp, code, GetNextCode);
		if (code == CODE_EOI)
			break;
		if (code == CODE_CLEAR) {
			free_entp = sp->dec_codetab + CODE_FIRST;
			nbits = BITS_MIN;
			nbitsmask = MAXCODE(BITS_MIN);
			maxcodep = sp->dec_codetab + nbitsmask-1;
			NextCode(tif, sp, bp, code, GetNextCode);
			if (code == CODE_EOI)
				break;
			*op++ = (char) code, occ--;
			oldcodep = sp->dec_codetab + code;
			continue;
		}
		codep = sp->dec_codetab + code;

		/*
	 	 * Add the new entry to the code table.
	 	 */
		assert(&sp->dec_codetab[0] <= free_entp && free_entp < &sp->dec_codetab[CSIZE]);
		free_entp->next = oldcodep;
		free_entp->firstchar = free_entp->next->firstchar;
		free_entp->length = free_entp->next->length+1;
		free_entp->value = (codep < free_entp) ?
		    codep->firstchar : free_entp->firstchar;
		if (++free_entp > maxcodep) {
			if (++nbits > BITS_MAX)		/* should not happen */
				nbits = BITS_MAX;
			nbitsmask = MAXCODE(nbits);
			maxcodep = sp->dec_codetab + nbitsmask-1;
		}
		oldcodep = codep;
		if (code >= 256) {
			/*
		 	 * Code maps to a string, copy string
			 * value to output (written in reverse).
		 	 */
			if (codep->length > occ) {
				/*
				 * String is too long for decode buffer,
				 * locate portion that will fit, copy to
				 * the decode buffer, and setup restart
				 * logic for the next decoding call.
				 */
				sp->dec_codep = codep;
				do {
					codep = codep->next;
				} while (codep && codep->length > occ);
				if (codep) {
					sp->dec_restart = occ;
					tp = op + occ;
					do  {
						*--tp = codep->value;
						codep = codep->next;
					}  while (--occ && codep);
					if (codep)
						codeLoop(tif);
				}
				break;
			}
			len = codep->length;
			tp = op + len;
			do {
				int t;
				--tp;
				t = codep->value;
				codep = codep->next;
				*tp = t;
			} while (codep && tp > op);
			if (codep) {
			    codeLoop(tif);
			    break;
			}
			op += len, occ -= len;
		} else
			*op++ = (char) code, occ--;
	}

	tif->tif_rawcp = (tidata_t) bp;
	sp->lzw_nbits = (u_short) nbits;
	sp->lzw_nextdata = nextdata;
	sp->lzw_nextbits = nextbits;
	sp->dec_nbitsmask = nbitsmask;
	sp->dec_oldcodep = oldcodep;
	sp->dec_free_entp = free_entp;
	sp->dec_maxcodep = maxcodep;

	if (occ > 0) {
		TIFFError(tif->tif_name,
		"LZWDecode: Not enough data at scanline %d (short %d bytes)",
		    tif->tif_row, occ);
		return (0);
	}
	return (1);
}

#ifdef LZW_COMPAT
/*
 * Decode a "hunk of data" for old images.
 */
#define	GetNextCodeCompat(sp, bp, code) {			\
	nextdata |= (u_long) *(bp)++ << nextbits;		\
	nextbits += 8;						\
	if (nextbits < nbits) {					\
		nextdata |= (u_long) *(bp)++ << nextbits;	\
		nextbits += 8;					\
	}							\
	code = (hcode_t)(nextdata & nbitsmask);			\
	nextdata >>= nbits;					\
	nextbits -= nbits;					\
}

static int
LZWDecodeCompat(TIFF* tif, tidata_t op0, tsize_t occ0, tsample_t s)
{
	LZWDecodeState *sp = DecoderState(tif);
	char *op = (char*) op0;
	long occ = (long) occ0;
	char *tp;
	u_char *bp;
	int code, nbits;
	long nextbits, nextdata, nbitsmask;
	code_t *codep, *free_entp, *maxcodep, *oldcodep;

	(void) s;
	assert(sp != NULL);
	/*
	 * Restart interrupted output operation.
	 */
	if (sp->dec_restart) {
		long residue;

		codep = sp->dec_codep;
		residue = codep->length - sp->dec_restart;
		if (residue > occ) {
			/*
			 * Residue from previous decode is sufficient
			 * to satisfy decode request.  Skip to the
			 * start of the decoded string, place decoded
			 * values in the output buffer, and return.
			 */
			sp->dec_restart += occ;
			do {
				codep = codep->next;
			} while (--residue > occ);
			tp = op + occ;
			do {
				*--tp = codep->value;
				codep = codep->next;
			} while (--occ);
			return (1);
		}
		/*
		 * Residue satisfies only part of the decode request.
		 */
		op += residue, occ -= residue;
		tp = op;
		do {
			*--tp = codep->value;
			codep = codep->next;
		} while (--residue);
		sp->dec_restart = 0;
	}

	bp = (u_char *)tif->tif_rawcp;
	nbits = sp->lzw_nbits;
	nextdata = sp->lzw_nextdata;
	nextbits = sp->lzw_nextbits;
	nbitsmask = sp->dec_nbitsmask;
	oldcodep = sp->dec_oldcodep;
	free_entp = sp->dec_free_entp;
	maxcodep = sp->dec_maxcodep;

	while (occ > 0) {
		NextCode(tif, sp, bp, code, GetNextCodeCompat);
		if (code == CODE_EOI)
			break;
		if (code == CODE_CLEAR) {
			free_entp = sp->dec_codetab + CODE_FIRST;
			nbits = BITS_MIN;
			nbitsmask = MAXCODE(BITS_MIN);
			maxcodep = sp->dec_codetab + nbitsmask;
			NextCode(tif, sp, bp, code, GetNextCodeCompat);
			if (code == CODE_EOI)
				break;
			*op++ = code, occ--;
			oldcodep = sp->dec_codetab + code;
			continue;
		}
		codep = sp->dec_codetab + code;

		/*
	 	 * Add the new entry to the code table.
	 	 */
		assert(&sp->dec_codetab[0] <= free_entp && free_entp < &sp->dec_codetab[CSIZE]);
		free_entp->next = oldcodep;
		free_entp->firstchar = free_entp->next->firstchar;
		free_entp->length = free_entp->next->length+1;
		free_entp->value = (codep < free_entp) ?
		    codep->firstchar : free_entp->firstchar;
		if (++free_entp > maxcodep) {
			if (++nbits > BITS_MAX)		/* should not happen */
				nbits = BITS_MAX;
			nbitsmask = MAXCODE(nbits);
			maxcodep = sp->dec_codetab + nbitsmask;
		}
		oldcodep = codep;
		if (code >= 256) {
			/*
		 	 * Code maps to a string, copy string
			 * value to output (written in reverse).
		 	 */
			if (codep->length > occ) {
				/*
				 * String is too long for decode buffer,
				 * locate portion that will fit, copy to
				 * the decode buffer, and setup restart
				 * logic for the next decoding call.
				 */
				sp->dec_codep = codep;
				do {
					codep = codep->next;
				} while (codep->length > occ);
				sp->dec_restart = occ;
				tp = op + occ;
				do  {
					*--tp = codep->value;
					codep = codep->next;
				}  while (--occ);
				break;
			}
			op += codep->length, occ -= codep->length;
			tp = op;
			do {
				*--tp = codep->value;
			} while( (codep = codep->next) != NULL);
		} else
			*op++ = code, occ--;
	}

	tif->tif_rawcp = (tidata_t) bp;
	sp->lzw_nbits = nbits;
	sp->lzw_nextdata = nextdata;
	sp->lzw_nextbits = nextbits;
	sp->dec_nbitsmask = nbitsmask;
	sp->dec_oldcodep = oldcodep;
	sp->dec_free_entp = free_entp;
	sp->dec_maxcodep = maxcodep;

	if (occ > 0) {
		TIFFError(tif->tif_name,
	    "LZWDecodeCompat: Not enough data at scanline %d (short %d bytes)",
		    tif->tif_row, occ);
		return (0);
	}
	return (1);
}
static int _LZWtrue(TIFF* tif) { (void) tif; return (1); }
#endif /* LZW_COMPAT */

/*
 * LZW Encoding.
 * 
 * LZW Encoding Removed 11/22/99 due to Unisys's control of patent,
 * and their demand for payment even from software developers. 
 * 
 */

static void
LZWCleanup(TIFF* tif)
{
	if (tif->tif_data) {
		if (tif->tif_mode == O_RDONLY) {
			if (DecoderState(tif)->dec_codetab)
				_TIFFfree(DecoderState(tif)->dec_codetab);
		} 
		_TIFFfree(tif->tif_data);
		tif->tif_data = NULL;
	}
}

int
TIFFInitLZW(TIFF* tif, int scheme)
{
	assert(scheme == COMPRESSION_LZW);
	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	tif->tif_data = (tidata_t) _TIFFmalloc(sizeof (LZWDecodeState));
	if (tif->tif_data == NULL)
	  goto bad;
	if (tif->tif_mode == O_RDONLY) {
	  DecoderState(tif)->dec_codetab = NULL;
	  DecoderState(tif)->dec_decode = NULL;
	} 
	/*
	 * Install codec methods.
	 */
	tif->tif_setupdecode = LZWSetupDecode;
	tif->tif_predecode = LZWPreDecode;
	tif->tif_decoderow = LZWDecode;
	tif->tif_decodestrip = LZWDecode;
	tif->tif_decodetile = LZWDecode;
	tif->tif_setupencode = _LZWtrue; 
	tif->tif_preencode = _TIFFNoPreCode;
	tif->tif_postencode = _LZWtrue;
	tif->tif_encoderow = _TIFFNoRowEncode; 
	tif->tif_encodestrip = _TIFFNoStripEncode; 
	tif->tif_encodetile = _TIFFNoTileEncode; 
	tif->tif_cleanup = LZWCleanup;
	/*
	 * Setup predictor setup.
	 */
	(void) TIFFPredictorInit(tif);
	return (1);
bad:
	TIFFError("TIFFInitLZW", "No space for LZW state block");
	return (0);
}


/*
 * Copyright (c) 1985, 1986 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * James A. Woods, derived from original work by Spencer Thomas
 * and Joseph Orost.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#endif /* LZW_SUPPORT */


/* ==== tif_next.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_next.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef NEXT_SUPPORT
/*
 * TIFF Library.
 *
 * NeXT 2-bit Grey Scale Compression Algorithm Support
 */

#define SETPIXEL(op, v) {			\
	switch (npixels++ & 3) {		\
	case 0:	op[0]  = (v) << 6; break;	\
	case 1:	op[0] |= (v) << 4; break;	\
	case 2:	op[0] |= (v) << 2; break;	\
	case 3:	*op++ |= (v);	   break;	\
	}					\
}

#define LITERALROW	0x00
#define LITERALSPAN	0x40
#define WHITE   	((1<<2)-1)

static int
NeXTDecode(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	register u_char *bp, *op;
	register tsize_t cc;
	register int n;
	tidata_t row;
	tsize_t scanline;

	(void) s;
	/*
	 * Each scanline is assumed to start off as all
	 * white (we assume a PhotometricInterpretation
	 * of ``min-is-black'').
	 */
	for (op = buf, cc = occ; cc-- > 0;)
		*op++ = 0xff;

	bp = (u_char *)tif->tif_rawcp;
	cc = tif->tif_rawcc;
	scanline = tif->tif_scanlinesize;
	for (row = buf; (long)occ > 0; occ -= scanline, row += scanline) {
		n = *bp++, cc--;
		switch (n) {
		case LITERALROW:
			/*
			 * The entire scanline is given as literal values.
			 */
			if (cc < scanline)
				goto bad;
			_TIFFmemcpy(row, bp, scanline);
			bp += scanline;
			cc -= scanline;
			break;
		case LITERALSPAN: {
			int off;
			/*
			 * The scanline has a literal span
			 * that begins at some offset.
			 */
			off = (bp[0] * 256) + bp[1];
			n = (bp[2] * 256) + bp[3];
			if (cc < 4+n)
				goto bad;
			_TIFFmemcpy(row+off, bp+4, n);
			bp += 4+n;
			cc -= 4+n;
			break;
		}
		default: {
			register int npixels = 0, grey;
			u_long imagewidth = tif->tif_dir.td_imagewidth;

			/*
			 * The scanline is composed of a sequence
			 * of constant color ``runs''.  We shift
			 * into ``run mode'' and interpret bytes
			 * as codes of the form <color><npixels>
			 * until we've filled the scanline.
			 */
			op = row;
			for (;;) {
				grey = (n>>6) & 0x3;
				n &= 0x3f;
				while (n-- > 0)
					SETPIXEL(op, grey);
				if (npixels >= (int) imagewidth)
					break;
				if (cc == 0)
					goto bad;
				n = *bp++, cc--;
			}
			break;
		}
		}
	}
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	return (1);
bad:
	TIFFError(tif->tif_name, "NeXTDecode: Not enough data for scanline %ld",
	    (long) tif->tif_row);
	return (0);
}

int
TIFFInitNeXT(TIFF* tif, int scheme)
{
	(void) scheme;
	tif->tif_decoderow = NeXTDecode;
	tif->tif_decodestrip = NeXTDecode;
	tif->tif_decodetile = NeXTDecode;
	return (1);
}
#endif /* NEXT_SUPPORT */


/* ==== tif_open.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_open.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 */
#include "tiffiop.h"

void _TIFFSetDefaultCompressionState(TIFF* tif);

static const long typemask[13] = {
	0L,		/* TIFF_NOTYPE */
	0x000000ffL,	/* TIFF_BYTE */
	0xffffffffL,	/* TIFF_ASCII */
	0x0000ffffL,	/* TIFF_SHORT */
	0xffffffffL,	/* TIFF_LONG */
	0xffffffffL,	/* TIFF_RATIONAL */
	0x000000ffL,	/* TIFF_SBYTE */
	0x000000ffL,	/* TIFF_UNDEFINED */
	0x0000ffffL,	/* TIFF_SSHORT */
	0xffffffffL,	/* TIFF_SLONG */
	0xffffffffL,	/* TIFF_SRATIONAL */
	0xffffffffL,	/* TIFF_FLOAT */
	0xffffffffL,	/* TIFF_DOUBLE */
};
static const int bigTypeshift[13] = {
	0,		/* TIFF_NOTYPE */
	24,		/* TIFF_BYTE */
	0,		/* TIFF_ASCII */
	16,		/* TIFF_SHORT */
	0,		/* TIFF_LONG */
	0,		/* TIFF_RATIONAL */
	24,		/* TIFF_SBYTE */
	24,		/* TIFF_UNDEFINED */
	16,		/* TIFF_SSHORT */
	0,		/* TIFF_SLONG */
	0,		/* TIFF_SRATIONAL */
	0,		/* TIFF_FLOAT */
	0,		/* TIFF_DOUBLE */
};
static const int litTypeshift[13] = {
	0,		/* TIFF_NOTYPE */
	0,		/* TIFF_BYTE */
	0,		/* TIFF_ASCII */
	0,		/* TIFF_SHORT */
	0,		/* TIFF_LONG */
	0,		/* TIFF_RATIONAL */
	0,		/* TIFF_SBYTE */
	0,		/* TIFF_UNDEFINED */
	0,		/* TIFF_SSHORT */
	0,		/* TIFF_SLONG */
	0,		/* TIFF_SRATIONAL */
	0,		/* TIFF_FLOAT */
	0,		/* TIFF_DOUBLE */
};

/*
 * Initialize the shift & mask tables, and the
 * byte swapping state according to the file
 * contents and the machine architecture.
 */
static void
TIFFInitOrder(TIFF* tif, int magic, int bigendian)
{
	tif->tif_typemask = typemask;
	if (magic == TIFF_BIGENDIAN) {
		tif->tif_typeshift = bigTypeshift;
		if (!bigendian)
			tif->tif_flags |= TIFF_SWAB;
	} else {
		tif->tif_typeshift = litTypeshift;
		if (bigendian)
			tif->tif_flags |= TIFF_SWAB;
	}
}

int
_TIFFgetMode(const char* mode, const char* module)
{
	int m = -1;

	switch (mode[0]) {
	case 'r':
		m = O_RDONLY;
		if (mode[1] == '+')
			m = O_RDWR;
		break;
	case 'w':
	case 'a':
		m = O_RDWR|O_CREAT;
		if (mode[0] == 'w')
			m |= O_TRUNC;
		break;
	default:
		TIFFError(module, "\"%s\": Bad mode", mode);
		break;
	}
	return (m);
}

TIFF*
TIFFClientOpen(
	const char* name, const char* mode,
	thandle_t clientdata,
	TIFFReadWriteProc readproc,
	TIFFReadWriteProc writeproc,
	TIFFSeekProc seekproc,
	TIFFCloseProc closeproc,
	TIFFSizeProc sizeproc,
	TIFFMapFileProc mapproc,
	TIFFUnmapFileProc unmapproc
)
{
	static const char module[] = "TIFFClientOpen";
	TIFF *tif;
	int m, bigendian;
	const char* cp;

	m = _TIFFgetMode(mode, module);
	if (m == -1)
		goto bad2;
	tif = (TIFF *)_TIFFmalloc(sizeof (TIFF) + strlen(name) + 1);
	if (tif == NULL) {
		TIFFError(module, "%s: Out of memory (TIFF structure)", name);
		goto bad2;
	}
	_TIFFmemset(tif, 0, sizeof (*tif));
	tif->tif_name = (char *)tif + sizeof (TIFF);
	strcpy(tif->tif_name, name);
	tif->tif_mode = m &~ (O_CREAT|O_TRUNC);
	tif->tif_curdir = (tdir_t) -1;		/* non-existent directory */
	tif->tif_curoff = 0;
	tif->tif_curstrip = (tstrip_t) -1;	/* invalid strip */
	tif->tif_row = (uint32) -1;		/* read/write pre-increment */
	tif->tif_clientdata = clientdata;
	tif->tif_readproc = readproc;
	tif->tif_writeproc = writeproc;
	tif->tif_seekproc = seekproc;
	tif->tif_closeproc = closeproc;
	tif->tif_sizeproc = sizeproc;
	tif->tif_mapproc = mapproc;
	tif->tif_unmapproc = unmapproc;
	_TIFFSetDefaultCompressionState(tif);	/* setup default state */
	/*
	 * Default is to return data MSB2LSB and enable the
	 * use of memory-mapped files and strip chopping when
	 * a file is opened read-only.
	 */
	tif->tif_flags = FILLORDER_MSB2LSB;
	if (m == O_RDONLY)
#ifdef STRIPCHOP_DEFAULT
		tif->tif_flags |= TIFF_MAPPED|STRIPCHOP_DEFAULT;
#else
		tif->tif_flags |= TIFF_MAPPED;
#endif

	{ union { int32 i; char c[4]; } u; u.i = 1; bigendian = u.c[0] == 0; }
	/*
	 * Process library-specific flags in the open mode string.
	 * The following flags may be used to control intrinsic library
	 * behaviour that may or may not be desirable (usually for
	 * compatibility with some application that claims to support
	 * TIFF but only supports some braindead idea of what the
	 * vendor thinks TIFF is):
	 *
	 * 'l'		use little-endian byte order for creating a file
	 * 'b'		use big-endian byte order for creating a file
	 * 'L'		read/write information using LSB2MSB bit order
	 * 'B'		read/write information using MSB2LSB bit order
	 * 'H'		read/write information using host bit order
	 * 'M'		enable use of memory-mapped files when supported
	 * 'm'		disable use of memory-mapped files
	 * 'C'		enable strip chopping support when reading
	 * 'c'		disable strip chopping support
	 *
	 * The use of the 'l' and 'b' flags is strongly discouraged.
	 * These flags are provided solely because numerous vendors,
	 * typically on the PC, do not correctly support TIFF; they
	 * only support the Intel little-endian byte order.  This
	 * support is not configured by default because it supports
	 * the violation of the TIFF spec that says that readers *MUST*
	 * support both byte orders.  It is strongly recommended that
	 * you not use this feature except to deal with busted apps
	 * that write invalid TIFF.  And even in those cases you should
	 * bang on the vendors to fix their software.
	 *
	 * The 'L', 'B', and 'H' flags are intended for applications
	 * that can optimize operations on data by using a particular
	 * bit order.  By default the library returns data in MSB2LSB
	 * bit order for compatibiltiy with older versions of this
	 * library.  Returning data in the bit order of the native cpu
	 * makes the most sense but also requires applications to check
	 * the value of the FillOrder tag; something they probabyl do
	 * not do right now.
	 *
	 * The 'M' and 'm' flags are provided because some virtual memory
	 * systems exhibit poor behaviour when large images are mapped.
	 * These options permit clients to control the use of memory-mapped
	 * files on a per-file basis.
	 *
	 * The 'C' and 'c' flags are provided because the library support
	 * for chopping up large strips into multiple smaller strips is not
	 * application-transparent and as such can cause problems.  The 'c'
	 * option permits applications that only want to look at the tags,
	 * for example, to get the unadulterated TIFF tag information.
	 */
	for (cp = mode; *cp; cp++)
		switch (*cp) {
		case 'b':
			if ((m&O_CREAT) && !bigendian)
				tif->tif_flags |= TIFF_SWAB;
			break;
		case 'l':
			if ((m&O_CREAT) && bigendian)
				tif->tif_flags |= TIFF_SWAB;
			break;
		case 'B':
			tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
			    FILLORDER_MSB2LSB;
			break;
		case 'L':
			tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
			    FILLORDER_LSB2MSB;
			break;
		case 'H':
			tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
			    HOST_FILLORDER;
			break;
		case 'M':
			if (m == O_RDONLY)
				tif->tif_flags |= TIFF_MAPPED;
			break;
		case 'm':
			if (m == O_RDONLY)
				tif->tif_flags &= ~TIFF_MAPPED;
			break;
		case 'C':
			if (m == O_RDONLY)
				tif->tif_flags |= TIFF_STRIPCHOP;
			break;
		case 'c':
			if (m == O_RDONLY)
				tif->tif_flags &= ~TIFF_STRIPCHOP;
			break;
		}
	/*
	 * Read in TIFF header.
	 */
	if (!ReadOK(tif, &tif->tif_header, sizeof (TIFFHeader))) {
		if (tif->tif_mode == O_RDONLY) {
			TIFFError(name, "Cannot read TIFF header");
			goto bad;
		}
		/*
		 * Setup header and write.
		 */
		tif->tif_header.tiff_magic = tif->tif_flags & TIFF_SWAB
		    ? (bigendian ? TIFF_LITTLEENDIAN : TIFF_BIGENDIAN)
		    : (bigendian ? TIFF_BIGENDIAN : TIFF_LITTLEENDIAN);
		tif->tif_header.tiff_version = TIFF_VERSION;
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabShort(&tif->tif_header.tiff_version);
		tif->tif_header.tiff_diroff = 0;	/* filled in later */
		if (!WriteOK(tif, &tif->tif_header, sizeof (TIFFHeader))) {
			TIFFError(name, "Error writing TIFF header");
			goto bad;
		}
		/*
		 * Setup the byte order handling.
		 */
		TIFFInitOrder(tif, tif->tif_header.tiff_magic, bigendian);
		/*
		 * Setup default directory.
		 */
		if (!TIFFDefaultDirectory(tif))
			goto bad;
		tif->tif_diroff = 0;
		return (tif);
	}
	/*
	 * Setup the byte order handling.
	 */
	if (tif->tif_header.tiff_magic != TIFF_BIGENDIAN &&
	    tif->tif_header.tiff_magic != TIFF_LITTLEENDIAN) {
		TIFFError(name,  "Not a TIFF file, bad magic number %d (0x%x)",
		    tif->tif_header.tiff_magic,
		    tif->tif_header.tiff_magic);
		goto bad;
	}
	TIFFInitOrder(tif, tif->tif_header.tiff_magic, bigendian);
	/*
	 * Swap header if required.
	 */
	if (tif->tif_flags & TIFF_SWAB) {
		TIFFSwabShort(&tif->tif_header.tiff_version);
		TIFFSwabLong(&tif->tif_header.tiff_diroff);
	}
	/*
	 * Now check version (if needed, it's been byte-swapped).
	 * Note that this isn't actually a version number, it's a
	 * magic number that doesn't change (stupid).
	 */
	if (tif->tif_header.tiff_version != TIFF_VERSION) {
		TIFFError(name,
		    "Not a TIFF file, bad version number %d (0x%x)",
		    tif->tif_header.tiff_version,
		    tif->tif_header.tiff_version); 
		goto bad;
	}
	tif->tif_flags |= TIFF_MYBUFFER;
	tif->tif_rawcp = tif->tif_rawdata = 0;
	tif->tif_rawdatasize = 0;
	/*
	 * Setup initial directory.
	 */
	switch (mode[0]) {
	case 'r':
		tif->tif_nextdiroff = tif->tif_header.tiff_diroff;
		/*
		 * Try to use a memory-mapped file if the client
		 * has not explicitly suppressed usage with the
		 * 'm' flag in the open mode (see above).
		 */
		if ((tif->tif_flags & TIFF_MAPPED) &&
	!TIFFMapFileContents(tif, (tdata_t*) &tif->tif_base, &tif->tif_size))
			tif->tif_flags &= ~TIFF_MAPPED;
		if (TIFFReadDirectory(tif)) {
			tif->tif_rawcc = -1;
			tif->tif_flags |= TIFF_BUFFERSETUP;
			return (tif);
		}
		break;
	case 'a':
		/*
		 * New directories are automatically append
		 * to the end of the directory chain when they
		 * are written out (see TIFFWriteDirectory).
		 */
		if (!TIFFDefaultDirectory(tif))
			goto bad;
		return (tif);
	}
bad:
	tif->tif_mode = O_RDONLY;	/* XXX avoid flush */
	TIFFClose(tif);
	return ((TIFF*)0);
bad2:
	(void) (*closeproc)(clientdata);
	return ((TIFF*)0);
}

/*
 * Query functions to access private data.
 */

/*
 * Return open file's name.
 */
const char *
TIFFFileName(TIFF* tif)
{
	return (tif->tif_name);
}

/*
 * Return open file's I/O descriptor.
 */
int
TIFFFileno(TIFF* tif)
{
	return (tif->tif_fd);
}

/*
 * Return read/write mode.
 */
int
TIFFGetMode(TIFF* tif)
{
	return (tif->tif_mode);
}

/*
 * Return nonzero if file is organized in
 * tiles; zero if organized as strips.
 */
int
TIFFIsTiled(TIFF* tif)
{
	return (isTiled(tif));
}

/*
 * Return current row being read/written.
 */
uint32
TIFFCurrentRow(TIFF* tif)
{
	return (tif->tif_row);
}

/*
 * Return index of the current directory.
 */
tdir_t
TIFFCurrentDirectory(TIFF* tif)
{
	return (tif->tif_curdir);
}

/*
 * Return current strip.
 */
tstrip_t
TIFFCurrentStrip(TIFF* tif)
{
	return (tif->tif_curstrip);
}

/*
 * Return current tile.
 */
ttile_t
TIFFCurrentTile(TIFF* tif)
{
	return (tif->tif_curtile);
}

/*
 * Return nonzero if the file has byte-swapped data.
 */
int
TIFFIsByteSwapped(TIFF* tif)
{
	return ((tif->tif_flags & TIFF_SWAB) != 0);
}

/*
 * Return nonzero if the data is returned up-sampled.
 */
int
TIFFIsUpSampled(TIFF* tif)
{
	return (isUpSampled(tif));
}

/*
 * Return nonzero if the data is returned in MSB-to-LSB bit order.
 */
int
TIFFIsMSB2LSB(TIFF* tif)
{
	return (isFillOrder(tif, FILLORDER_MSB2LSB));
}


/* ==== tif_packbits.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_packbits.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef PACKBITS_SUPPORT
/*
 * TIFF Library.
 *
 * PackBits Compression Algorithm Support
 */
#include <assert.h>
#include <stdio.h>

static int
PackBitsPreEncode(TIFF* tif, tsample_t s)
{
	(void) s;
	/*
	 * Calculate the scanline/tile-width size in bytes.
	 */
	if (isTiled(tif))
		tif->tif_data = (tidata_t) TIFFTileRowSize(tif);
	else
		tif->tif_data = (tidata_t) TIFFScanlineSize(tif);
	return (1);
}

/*
 * NB: tidata is the type representing *(tidata_t);
 *     if tidata_t is made signed then this type must
 *     be adjusted accordingly.
 */
typedef unsigned char tidata;

/*
 * Encode a run of pixels.
 */
static int
PackBitsEncode(TIFF* tif, tidata_t buf, tsize_t cc, tsample_t s)
{
	u_char* bp = (u_char*) buf;
	tidata_t op, ep, lastliteral;
	long n, slop;
	int b;
	enum { BASE, LITERAL, RUN, LITERAL_RUN } state;

	(void) s;
	op = tif->tif_rawcp;
	ep = tif->tif_rawdata + tif->tif_rawdatasize;
	state = BASE;
	lastliteral = 0;
	while (cc > 0) {
		/*
		 * Find the longest string of identical bytes.
		 */
		b = *bp++, cc--, n = 1;
		for (; cc > 0 && b == *bp; cc--, bp++)
			n++;
	again:
		if (op + 2 >= ep) {		/* insure space for new data */
			/*
			 * Be careful about writing the last
			 * literal.  Must write up to that point
			 * and then copy the remainder to the
			 * front of the buffer.
			 */
			if (state == LITERAL || state == LITERAL_RUN) {
				slop = op - lastliteral;
				tif->tif_rawcc += lastliteral - tif->tif_rawcp;
				if (!TIFFFlushData1(tif))
					return (-1);
				op = tif->tif_rawcp;
				while (slop-- > 0)
					*op++ = *lastliteral++;
				lastliteral = tif->tif_rawcp;
			} else {
				tif->tif_rawcc += op - tif->tif_rawcp;
				if (!TIFFFlushData1(tif))
					return (-1);
				op = tif->tif_rawcp;
			}
		}
		switch (state) {
		case BASE:		/* initial state, set run/literal */
			if (n > 1) {
				state = RUN;
				if (n > 128) {
					*op++ = (tidata) -127;
					*op++ = b;
					n -= 128;
					goto again;
				}
				*op++ = (tidataval_t)(-(n-1));
				*op++ = b;
			} else {
				lastliteral = op;
				*op++ = 0;
				*op++ = b;
				state = LITERAL;
			}
			break;
		case LITERAL:		/* last object was literal string */
			if (n > 1) {
				state = LITERAL_RUN;
				if (n > 128) {
					*op++ = (tidata) -127;
					*op++ = b;
					n -= 128;
					goto again;
				}
				*op++ = (tidataval_t)(-(n-1));	/* encode run */
				*op++ = b;
			} else {			/* extend literal */
				if (++(*lastliteral) == 127)
					state = BASE;
				*op++ = b;
			}
			break;
		case RUN:		/* last object was run */
			if (n > 1) {
				if (n > 128) {
					*op++ = (tidata) -127;
					*op++ = b;
					n -= 128;
					goto again;
				}
				*op++ = (tidataval_t)(-(n-1));
				*op++ = b;
			} else {
				lastliteral = op;
				*op++ = 0;
				*op++ = b;
				state = LITERAL;
			}
			break;
		case LITERAL_RUN:	/* literal followed by a run */
			/*
			 * Check to see if previous run should
			 * be converted to a literal, in which
			 * case we convert literal-run-literal
			 * to a single literal.
			 */
			if (n == 1 && op[-2] == (tidata) -1 &&
			    *lastliteral < 126) {
				state = (((*lastliteral) += 2) == 127 ?
				    BASE : LITERAL);
				op[-2] = op[-1];	/* replicate */
			} else
				state = RUN;
			goto again;
		}
	}
	tif->tif_rawcc += op - tif->tif_rawcp;
	tif->tif_rawcp = op;
	return (1);
}

/*
 * Encode a rectangular chunk of pixels.  We break it up
 * into row-sized pieces to insure that encoded runs do
 * not span rows.  Otherwise, there can be problems with
 * the decoder if data is read, for example, by scanlines
 * when it was encoded by strips.
 */
static int
PackBitsEncodeChunk(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	tsize_t rowsize = (tsize_t) tif->tif_data;

	assert(rowsize > 0);
	while ((long)cc > 0) {
		if (PackBitsEncode(tif, bp, rowsize, s) < 0)
			return (-1);
		bp += rowsize;
		cc -= rowsize;
	}
	return (1);
}

static int
PackBitsDecode(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	char *bp;
	tsize_t cc;
	long n;
	int b;

	(void) s;
	bp = (char*) tif->tif_rawcp;
	cc = tif->tif_rawcc;
	while (cc > 0 && (long)occ > 0) {
		n = (long) *bp++, cc--;
		/*
		 * Watch out for compilers that
		 * don't sign extend chars...
		 */
		if (n >= 128)
			n -= 256;
		if (n < 0) {		/* replicate next byte -n+1 times */
			if (n == -128)	/* nop */
				continue;
			n = -n + 1;
			occ -= n;
			b = *bp++, cc--;
			while (n-- > 0)
				*op++ = b;
		} else {		/* copy next n+1 bytes literally */
			_TIFFmemcpy(op, bp, ++n);
			op += n; occ -= n;
			bp += n; cc -= n;
		}
	}
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	if (occ > 0) {
		TIFFError(tif->tif_name,
		    "PackBitsDecode: Not enough data for scanline %ld",
		    (long) tif->tif_row);
		return (0);
	}
	/* check for buffer overruns? */
	return (1);
}

int
TIFFInitPackBits(TIFF* tif, int scheme)
{
	(void) scheme;
	tif->tif_decoderow = PackBitsDecode;
	tif->tif_decodestrip = PackBitsDecode;
	tif->tif_decodetile = PackBitsDecode;
	tif->tif_preencode = PackBitsPreEncode;
	tif->tif_encoderow = PackBitsEncode;
	tif->tif_encodestrip = PackBitsEncodeChunk;
	tif->tif_encodetile = PackBitsEncodeChunk;
	return (1);
}
#endif /* PACKBITS_SUPPORT */


/* ==== tif_pixarlog.c ==== */
/*
 * Copyright (c) 1996-1997 Sam Leffler
 * Copyright (c) 1996 Pixar
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Pixar, Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Pixar, Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL PIXAR, SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef PIXARLOG_SUPPORT

/*
 * TIFF Library.
 * PixarLog Compression Support
 *
 * Contributed by Dan McCoy.
 *
 * PixarLog film support uses the TIFF library to store companded
 * 11 bit values into a tiff file, which are compressed using the 
 * zip compressor.  
 *
 * The codec can take as input and produce as output 32-bit IEEE float values 
 * as well as 16-bit or 8-bit unsigned integer values.
 *
 * On writing any of the above are converted into the internal
 * 11-bit log format.   In the case of  8 and 16 bit values, the
 * input is assumed to be unsigned linear color values that represent
 * the range 0-1.  In the case of IEEE values, the 0-1 range is assumed to
 * be the normal linear color range, in addition over 1 values are
 * accepted up to a value of about 25.0 to encode "hot" hightlights and such.
 * The encoding is lossless for 8-bit values, slightly lossy for the
 * other bit depths.  The actual color precision should be better
 * than the human eye can perceive with extra room to allow for
 * error introduced by further image computation.  As with any quantized
 * color format, it is possible to perform image calculations which
 * expose the quantization error. This format should certainly be less 
 * susceptable to such errors than standard 8-bit encodings, but more
 * susceptable than straight 16-bit or 32-bit encodings.
 *
 * On reading the internal format is converted to the desired output format.
 * The program can request which format it desires by setting the internal
 * pseudo tag TIFFTAG_PIXARLOGDATAFMT to one of these possible values:
 *  PIXARLOGDATAFMT_FLOAT     = provide IEEE float values.
 *  PIXARLOGDATAFMT_16BIT     = provide unsigned 16-bit integer values
 *  PIXARLOGDATAFMT_8BIT      = provide unsigned 8-bit integer values
 *
 * alternately PIXARLOGDATAFMT_8BITABGR provides unsigned 8-bit integer
 * values with the difference that if there are exactly three or four channels
 * (rgb or rgba) it swaps the channel order (bgr or abgr).
 *
 * PIXARLOGDATAFMT_11BITLOG provides the internal encoding directly
 * packed in 16-bit values.   However no tools are supplied for interpreting
 * these values.
 *
 * "hot" (over 1.0) areas written in floating point get clamped to
 * 1.0 in the integer data types.
 *
 * When the file is closed after writing, the bit depth and sample format
 * are set always to appear as if 8-bit data has been written into it.
 * That way a naive program unaware of the particulars of the encoding
 * gets the format it is most likely able to handle.
 *
 * The codec does it's own horizontal differencing step on the coded
 * values so the libraries predictor stuff should be turned off.
 * The codec also handle byte swapping the encoded values as necessary
 * since the library does not have the information necessary
 * to know the bit depth of the raw unencoded buffer.
 * 
 */

#include "tif_predict.h"
#include "zlib.h"
#include "zutil.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>

/* Tables for converting to/from 11 bit coded values */

#define  TSIZE	 2048		/* decode table size (11-bit tokens) */
#define  TSIZEP1 2049		/* Plus one for slop */
#define  ONE	 1250		/* token value of 1.0 exactly */
#define  RATIO	 1.004		/* nominal ratio for log part */

#define CODE_MASK 0x7ff         /* 11 bits. */

static float  Fltsize;
static float  LogK1, LogK2;

#define REPEAT(n, op)   { int i; i=n; do { i--; op; } while (i>0); }

static void
horizontalAccumulateF(uint16 *wp, int n, int stride, float *op, 
	float *ToLinearF)
{
    register unsigned int  cr, cg, cb, ca, mask;
    register float  t0, t1, t2, t3;

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    t0 = ToLinearF[cr = wp[0]];
	    t1 = ToLinearF[cg = wp[1]];
	    t2 = ToLinearF[cb = wp[2]];
	    op[0] = t0;
	    op[1] = t1;
	    op[2] = t2;
	    n -= 3;
	    while (n > 0) {
		wp += 3;
		op += 3;
		n -= 3;
		t0 = ToLinearF[(cr += wp[0]) & mask];
		t1 = ToLinearF[(cg += wp[1]) & mask];
		t2 = ToLinearF[(cb += wp[2]) & mask];
		op[0] = t0;
		op[1] = t1;
		op[2] = t2;
	    }
	} else if (stride == 4) {
	    t0 = ToLinearF[cr = wp[0]];
	    t1 = ToLinearF[cg = wp[1]];
	    t2 = ToLinearF[cb = wp[2]];
	    t3 = ToLinearF[ca = wp[3]];
	    op[0] = t0;
	    op[1] = t1;
	    op[2] = t2;
	    op[3] = t3;
	    n -= 4;
	    while (n > 0) {
		wp += 4;
		op += 4;
		n -= 4;
		t0 = ToLinearF[(cr += wp[0]) & mask];
		t1 = ToLinearF[(cg += wp[1]) & mask];
		t2 = ToLinearF[(cb += wp[2]) & mask];
		t3 = ToLinearF[(ca += wp[3]) & mask];
		op[0] = t0;
		op[1] = t1;
		op[2] = t2;
		op[3] = t3;
	    }
	} else {
	    REPEAT(stride, *op = ToLinearF[*wp&mask]; wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; *op = ToLinearF[*wp&mask]; wp++; op++)
		n -= stride;
	    }
	}
    }
}

static void
horizontalAccumulate12(uint16 *wp, int n, int stride, int16 *op,
	float *ToLinearF)
{
    register unsigned int  cr, cg, cb, ca, mask;
    register float  t0, t1, t2, t3;

#define SCALE12 2048.0
#define CLAMP12(t) (((t) < 3071) ? (uint16) (t) : 3071)

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    t0 = ToLinearF[cr = wp[0]] * SCALE12;
	    t1 = ToLinearF[cg = wp[1]] * SCALE12;
	    t2 = ToLinearF[cb = wp[2]] * SCALE12;
	    op[0] = CLAMP12(t0);
	    op[1] = CLAMP12(t1);
	    op[2] = CLAMP12(t2);
	    n -= 3;
	    while (n > 0) {
		wp += 3;
		op += 3;
		n -= 3;
		t0 = ToLinearF[(cr += wp[0]) & mask] * SCALE12;
		t1 = ToLinearF[(cg += wp[1]) & mask] * SCALE12;
		t2 = ToLinearF[(cb += wp[2]) & mask] * SCALE12;
		op[0] = CLAMP12(t0);
		op[1] = CLAMP12(t1);
		op[2] = CLAMP12(t2);
	    }
	} else if (stride == 4) {
	    t0 = ToLinearF[cr = wp[0]] * SCALE12;
	    t1 = ToLinearF[cg = wp[1]] * SCALE12;
	    t2 = ToLinearF[cb = wp[2]] * SCALE12;
	    t3 = ToLinearF[ca = wp[3]] * SCALE12;
	    op[0] = CLAMP12(t0);
	    op[1] = CLAMP12(t1);
	    op[2] = CLAMP12(t2);
	    op[3] = CLAMP12(t3);
	    n -= 4;
	    while (n > 0) {
		wp += 4;
		op += 4;
		n -= 4;
		t0 = ToLinearF[(cr += wp[0]) & mask] * SCALE12;
		t1 = ToLinearF[(cg += wp[1]) & mask] * SCALE12;
		t2 = ToLinearF[(cb += wp[2]) & mask] * SCALE12;
		t3 = ToLinearF[(ca += wp[3]) & mask] * SCALE12;
		op[0] = CLAMP12(t0);
		op[1] = CLAMP12(t1);
		op[2] = CLAMP12(t2);
		op[3] = CLAMP12(t3);
	    }
	} else {
	    REPEAT(stride, t0 = ToLinearF[*wp&mask] * SCALE12;
                           *op = CLAMP12(t0); wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; t0 = ToLinearF[wp[stride]&mask]*SCALE12;
		    *op = CLAMP12(t0);  wp++; op++)
		n -= stride;
	    }
	}
    }
}

static void
horizontalAccumulate16(uint16 *wp, int n, int stride, uint16 *op,
	uint16 *ToLinear16)
{
    register unsigned int  cr, cg, cb, ca, mask;

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    op[0] = ToLinear16[cr = wp[0]];
	    op[1] = ToLinear16[cg = wp[1]];
	    op[2] = ToLinear16[cb = wp[2]];
	    n -= 3;
	    while (n > 0) {
		wp += 3;
		op += 3;
		n -= 3;
		op[0] = ToLinear16[(cr += wp[0]) & mask];
		op[1] = ToLinear16[(cg += wp[1]) & mask];
		op[2] = ToLinear16[(cb += wp[2]) & mask];
	    }
	} else if (stride == 4) {
	    op[0] = ToLinear16[cr = wp[0]];
	    op[1] = ToLinear16[cg = wp[1]];
	    op[2] = ToLinear16[cb = wp[2]];
	    op[3] = ToLinear16[ca = wp[3]];
	    n -= 4;
	    while (n > 0) {
		wp += 4;
		op += 4;
		n -= 4;
		op[0] = ToLinear16[(cr += wp[0]) & mask];
		op[1] = ToLinear16[(cg += wp[1]) & mask];
		op[2] = ToLinear16[(cb += wp[2]) & mask];
		op[3] = ToLinear16[(ca += wp[3]) & mask];
	    }
	} else {
	    REPEAT(stride, *op = ToLinear16[*wp&mask]; wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; *op = ToLinear16[*wp&mask]; wp++; op++)
		n -= stride;
	    }
	}
    }
}

/* 
 * Returns the log encoded 11-bit values with the horizontal
 * differencing undone.
 */
static void
horizontalAccumulate11(uint16 *wp, int n, int stride, uint16 *op)
{
    register unsigned int  cr, cg, cb, ca, mask;

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    op[0] = cr = wp[0];  op[1] = cg = wp[1];  op[2] = cb = wp[2];
	    n -= 3;
	    while (n > 0) {
		wp += 3;
		op += 3;
		n -= 3;
		op[0] = (cr += wp[0]) & mask;
		op[1] = (cg += wp[1]) & mask;
		op[2] = (cb += wp[2]) & mask;
	    }
	} else if (stride == 4) {
	    op[0] = cr = wp[0];  op[1] = cg = wp[1];
	    op[2] = cb = wp[2];  op[3] = ca = wp[3];
	    n -= 4;
	    while (n > 0) {
		wp += 4;
		op += 4;
		n -= 4;
		op[0] = (cr += wp[0]) & mask;
		op[1] = (cg += wp[1]) & mask;
		op[2] = (cb += wp[2]) & mask;
		op[3] = (ca += wp[3]) & mask;
	    } 
	} else {
	    REPEAT(stride, *op = *wp&mask; wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; *op = *wp&mask; wp++; op++)
	    	n -= stride;
	    }
	}
    }
}

static void
horizontalAccumulate8(uint16 *wp, int n, int stride, unsigned char *op,
	unsigned char *ToLinear8)
{
    register unsigned int  cr, cg, cb, ca, mask;

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    op[0] = ToLinear8[cr = wp[0]];
	    op[1] = ToLinear8[cg = wp[1]];
	    op[2] = ToLinear8[cb = wp[2]];
	    n -= 3;
	    while (n > 0) {
		n -= 3;
		wp += 3;
		op += 3;
		op[0] = ToLinear8[(cr += wp[0]) & mask];
		op[1] = ToLinear8[(cg += wp[1]) & mask];
		op[2] = ToLinear8[(cb += wp[2]) & mask];
	    }
	} else if (stride == 4) {
	    op[0] = ToLinear8[cr = wp[0]];
	    op[1] = ToLinear8[cg = wp[1]];
	    op[2] = ToLinear8[cb = wp[2]];
	    op[3] = ToLinear8[ca = wp[3]];
	    n -= 4;
	    while (n > 0) {
		n -= 4;
		wp += 4;
		op += 4;
		op[0] = ToLinear8[(cr += wp[0]) & mask];
		op[1] = ToLinear8[(cg += wp[1]) & mask];
		op[2] = ToLinear8[(cb += wp[2]) & mask];
		op[3] = ToLinear8[(ca += wp[3]) & mask];
	    }
	} else {
	    REPEAT(stride, *op = ToLinear8[*wp&mask]; wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; *op = ToLinear8[*wp&mask]; wp++; op++)
		n -= stride;
	    }
	}
    }
}


static void
horizontalAccumulate8abgr(uint16 *wp, int n, int stride, unsigned char *op,
	unsigned char *ToLinear8)
{
    register unsigned int  cr, cg, cb, ca, mask;
    register unsigned char  t0, t1, t2, t3;

    if (n >= stride) {
	mask = CODE_MASK;
	if (stride == 3) {
	    op[0] = 0;
	    t1 = ToLinear8[cb = wp[2]];
	    t2 = ToLinear8[cg = wp[1]];
	    t3 = ToLinear8[cr = wp[0]];
	    op[1] = t1;
	    op[2] = t2;
	    op[3] = t3;
	    n -= 3;
	    while (n > 0) {
		n -= 3;
		wp += 3;
		op += 4;
		op[0] = 0;
		t1 = ToLinear8[(cb += wp[2]) & mask];
		t2 = ToLinear8[(cg += wp[1]) & mask];
		t3 = ToLinear8[(cr += wp[0]) & mask];
		op[1] = t1;
		op[2] = t2;
		op[3] = t3;
	    }
	} else if (stride == 4) {
	    t0 = ToLinear8[ca = wp[3]];
	    t1 = ToLinear8[cb = wp[2]];
	    t2 = ToLinear8[cg = wp[1]];
	    t3 = ToLinear8[cr = wp[0]];
	    op[0] = t0;
	    op[1] = t1;
	    op[2] = t2;
	    op[3] = t3;
	    n -= 4;
	    while (n > 0) {
		n -= 4;
		wp += 4;
		op += 4;
		t0 = ToLinear8[(ca += wp[3]) & mask];
		t1 = ToLinear8[(cb += wp[2]) & mask];
		t2 = ToLinear8[(cg += wp[1]) & mask];
		t3 = ToLinear8[(cr += wp[0]) & mask];
		op[0] = t0;
		op[1] = t1;
		op[2] = t2;
		op[3] = t3;
	    }
	} else {
	    REPEAT(stride, *op = ToLinear8[*wp&mask]; wp++; op++)
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride,
		    wp[stride] += *wp; *op = ToLinear8[*wp&mask]; wp++; op++)
		n -= stride;
	    }
	}
    }
}

/*
 * State block for each open TIFF
 * file using PixarLog compression/decompression.
 */
typedef	struct {
	TIFFPredictorState	predict;
	z_stream		stream;
	uint16			*tbuf; 
	uint16			stride;
	int			state;
	int			user_datafmt;
	int			quality;
#define PLSTATE_INIT 1

	TIFFVSetMethod		vgetparent;	/* super-class method */
	TIFFVSetMethod		vsetparent;	/* super-class method */

	float *ToLinearF;
	uint16 *ToLinear16;
	unsigned char *ToLinear8;
	uint16  *FromLT2;
	uint16  *From14; /* Really for 16-bit data, but we shift down 2 */
	uint16  *From8;
	
} PixarLogState;

static int
PixarLogMakeTables(PixarLogState *sp)
{

/*
 *    We make several tables here to convert between various external
 *    representations (float, 16-bit, and 8-bit) and the internal
 *    11-bit companded representation.  The 11-bit representation has two
 *    distinct regions.  A linear bottom end up through .018316 in steps
 *    of about .000073, and a region of constant ratio up to about 25.
 *    These floating point numbers are stored in the main table ToLinearF. 
 *    All other tables are derived from this one.  The tables (and the
 *    ratios) are continuous at the internal seam.
 */

    int  nlin, lt2size;
    int  i, j;
    double  b, c, linstep, max;
    double  k, v, dv, r, lr2, r2;
    float *ToLinearF;
    uint16 *ToLinear16;
    unsigned char *ToLinear8;
    uint16  *FromLT2;
    uint16  *From14; /* Really for 16-bit data, but we shift down 2 */
    uint16  *From8;

    c = log(RATIO);	
    nlin = 1./c;	/* nlin must be an integer */
    c = 1./nlin;
    b = exp(-c*ONE);	/* multiplicative scale factor [b*exp(c*ONE) = 1] */
    linstep = b*c*exp(1.);

    LogK1 = 1./c;	/* if (v >= 2)  token = k1*log(v*k2) */
    LogK2 = 1./b;
    lt2size = (2./linstep)+1;
    FromLT2 = (uint16 *)_TIFFmalloc(lt2size*sizeof(uint16));
    From14 = (uint16 *)_TIFFmalloc(16384*sizeof(uint16));
    From8 = (uint16 *)_TIFFmalloc(256*sizeof(uint16));
    ToLinearF = (float *)_TIFFmalloc(TSIZEP1 * sizeof(float));
    ToLinear16 = (uint16 *)_TIFFmalloc(TSIZEP1 * sizeof(uint16));
    ToLinear8 = (unsigned char *)_TIFFmalloc(TSIZEP1 * sizeof(unsigned char));
    if (FromLT2 == NULL || From14  == NULL || From8   == NULL ||
	 ToLinearF == NULL || ToLinear16 == NULL || ToLinear8 == NULL) {
	if (FromLT2) _TIFFfree(FromLT2);
	if (From14) _TIFFfree(From14);
	if (From8) _TIFFfree(From8);
	if (ToLinearF) _TIFFfree(ToLinearF);
	if (ToLinear16) _TIFFfree(ToLinear16);
	if (ToLinear8) _TIFFfree(ToLinear8);
	sp->FromLT2 = NULL;
	sp->From14 = NULL;
	sp->From8 = NULL;
	sp->ToLinearF = NULL;
	sp->ToLinear16 = NULL;
	sp->ToLinear8 = NULL;
	return 0;
    }

    j = 0;

    for (i = 0; i < nlin; i++)  {
	v = i * linstep;
	ToLinearF[j++] = v;
    }

    for (i = nlin; i < TSIZE; i++)
	ToLinearF[j++] = b*exp(c*i);

    ToLinearF[2048] = ToLinearF[2047];

    for (i = 0; i < TSIZEP1; i++)  {
	v = ToLinearF[i]*65535.0 + 0.5;
	ToLinear16[i] = (v > 65535.0) ? 65535 : v;
	v = ToLinearF[i]*255.0  + 0.5;
	ToLinear8[i]  = (v > 255.0) ? 255 : v;
    }

    j = 0;
    for (i = 0; i < lt2size; i++)  {
	if ((i*linstep)*(i*linstep) > ToLinearF[j]*ToLinearF[j+1])
	    j++;
	FromLT2[i] = j;
    }

    /*
     * Since we lose info anyway on 16-bit data, we set up a 14-bit
     * table and shift 16-bit values down two bits on input.
     * saves a little table space.
     */
    j = 0;
    for (i = 0; i < 16384; i++)  {
	while ((i/16383.)*(i/16383.) > ToLinearF[j]*ToLinearF[j+1])
	    j++;
	From14[i] = j;
    }

    j = 0;
    for (i = 0; i < 256; i++)  {
	while ((i/255.)*(i/255.) > ToLinearF[j]*ToLinearF[j+1])
	    j++;
	From8[i] = j;
    }

    Fltsize = lt2size/2;

    sp->ToLinearF = ToLinearF;
    sp->ToLinear16 = ToLinear16;
    sp->ToLinear8 = ToLinear8;
    sp->FromLT2 = FromLT2;
    sp->From14 = From14;
    sp->From8 = From8;

    return 1;
}

#define	DecoderState(tif)	((PixarLogState*) (tif)->tif_data)
#define	EncoderState(tif)	((PixarLogState*) (tif)->tif_data)

static	int PixarLogEncode(TIFF*, tidata_t, tsize_t, tsample_t);
static	int PixarLogDecode(TIFF*, tidata_t, tsize_t, tsample_t);

#define N(a)   (sizeof(a)/sizeof(a[0]))
#define PIXARLOGDATAFMT_UNKNOWN	-1

static int
PixarLogGuessDataFmt(TIFFDirectory *td)
{
	int guess = PIXARLOGDATAFMT_UNKNOWN;
	int format = td->td_sampleformat;

	/* If the user didn't tell us his datafmt,
	 * take our best guess from the bitspersample.
	 */
	switch (td->td_bitspersample) {
	 case 32:
		if (format == SAMPLEFORMAT_IEEEFP)
			guess = PIXARLOGDATAFMT_FLOAT;
		break;
	 case 16:
		if (format == SAMPLEFORMAT_VOID || format == SAMPLEFORMAT_UINT)
			guess = PIXARLOGDATAFMT_16BIT;
		break;
	 case 12:
		if (format == SAMPLEFORMAT_VOID || format == SAMPLEFORMAT_INT)
			guess = PIXARLOGDATAFMT_12BITPICIO;
		break;
	 case 11:
		if (format == SAMPLEFORMAT_VOID || format == SAMPLEFORMAT_UINT)
			guess = PIXARLOGDATAFMT_11BITLOG;
		break;
	 case 8:
		if (format == SAMPLEFORMAT_VOID || format == SAMPLEFORMAT_UINT)
			guess = PIXARLOGDATAFMT_8BIT;
		break;
	}

	return guess;
}

static int
PixarLogSetupDecode(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState* sp = DecoderState(tif);
	static const char module[] = "PixarLogSetupDecode";

	assert(sp != NULL);

	/* Make sure no byte swapping happens on the data
	 * after decompression. */
	tif->tif_postdecode = _TIFFNoPostDecode;

	/* for some reason, we can't do this in TIFFInitPixarLog */

	sp->stride = (td->td_planarconfig == PLANARCONFIG_CONTIG ?
	    td->td_samplesperpixel : 1);
	sp->tbuf = (uint16 *) _TIFFmalloc(sp->stride * 
		td->td_imagewidth * td->td_rowsperstrip * sizeof(uint16));
	if (sp->user_datafmt == PIXARLOGDATAFMT_UNKNOWN)
		sp->user_datafmt = PixarLogGuessDataFmt(td);
	if (sp->user_datafmt == PIXARLOGDATAFMT_UNKNOWN) {
		TIFFError(module, 
			"PixarLog compression can't handle bits depth/data format combination (depth: %d)", 
			td->td_bitspersample);
		return (0);
	}

	if (inflateInit(&sp->stream) != Z_OK) {
		TIFFError(module, "%s: %s", tif->tif_name, sp->stream.msg);
		return (0);
	} else {
		sp->state |= PLSTATE_INIT;
		return (1);
	}
}

/*
 * Setup state for decoding a strip.
 */
static int
PixarLogPreDecode(TIFF* tif, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState* sp = DecoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->stream.next_in = tif->tif_rawdata;
	sp->stream.avail_in = tif->tif_rawcc;
	return (inflateReset(&sp->stream) == Z_OK);
}

static int
PixarLogDecode(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState* sp = DecoderState(tif);
	static const char module[] = "PixarLogDecode";
	int i, nsamples, llen;
	uint16 *up;

	switch (sp->user_datafmt) {
	case PIXARLOGDATAFMT_FLOAT:
		nsamples = occ / sizeof(float);	/* XXX float == 32 bits */
		break;
	case PIXARLOGDATAFMT_16BIT:
	case PIXARLOGDATAFMT_12BITPICIO:
	case PIXARLOGDATAFMT_11BITLOG:
		nsamples = occ / sizeof(uint16); /* XXX uint16 == 16 bits */
		break;
	case PIXARLOGDATAFMT_8BIT:
	case PIXARLOGDATAFMT_8BITABGR:
		nsamples = occ;
		break;
	default:
		TIFFError(tif->tif_name,
			"%d bit input not supported in PixarLog",
			td->td_bitspersample);
		return 0;
	}

	llen = sp->stride * td->td_imagewidth;

	(void) s;
	assert(sp != NULL);
	sp->stream.next_out = (unsigned char *) sp->tbuf;
	sp->stream.avail_out = nsamples * sizeof(uint16);
	do {
		int state = inflate(&sp->stream, Z_PARTIAL_FLUSH);
		if (state == Z_STREAM_END) {
			break;			/* XXX */
		}
		if (state == Z_DATA_ERROR) {
			TIFFError(module,
			    "%s: Decoding error at scanline %d, %s",
			    tif->tif_name, tif->tif_row, sp->stream.msg);
			if (inflateSync(&sp->stream) != Z_OK)
				return (0);
			continue;
		}
		if (state != Z_OK) {
			TIFFError(module, "%s: zlib error: %s",
			    tif->tif_name, sp->stream.msg);
			return (0);
		}
	} while (sp->stream.avail_out > 0);

	/* hopefully, we got all the bytes we needed */
	if (sp->stream.avail_out != 0) {
		TIFFError(module,
		    "%s: Not enough data at scanline %d (short %d bytes)",
		    tif->tif_name, tif->tif_row, sp->stream.avail_out);
		return (0);
	}

	up = sp->tbuf;
	/* Swap bytes in the data if from a different endian machine. */
	if (tif->tif_flags & TIFF_SWAB)
		TIFFSwabArrayOfShort(up, nsamples);

	for (i = 0; i < nsamples; i += llen, up += llen) {
		switch (sp->user_datafmt)  {
		case PIXARLOGDATAFMT_FLOAT:
			horizontalAccumulateF(up, llen, sp->stride,
					(float *)op, sp->ToLinearF);
			op += llen * sizeof(float);
			break;
		case PIXARLOGDATAFMT_16BIT:
			horizontalAccumulate16(up, llen, sp->stride,
					(uint16 *)op, sp->ToLinear16);
			op += llen * sizeof(uint16);
			break;
		case PIXARLOGDATAFMT_12BITPICIO:
			horizontalAccumulate12(up, llen, sp->stride,
					(int16 *)op, sp->ToLinearF);
			op += llen * sizeof(int16);
			break;
		case PIXARLOGDATAFMT_11BITLOG:
			horizontalAccumulate11(up, llen, sp->stride,
					(uint16 *)op);
			op += llen * sizeof(uint16);
			break;
		case PIXARLOGDATAFMT_8BIT:
			horizontalAccumulate8(up, llen, sp->stride,
					(unsigned char *)op, sp->ToLinear8);
			op += llen * sizeof(unsigned char);
			break;
		case PIXARLOGDATAFMT_8BITABGR:
			horizontalAccumulate8abgr(up, llen, sp->stride,
					(unsigned char *)op, sp->ToLinear8);
			op += llen * sizeof(unsigned char);
			break;
		default:
			TIFFError(tif->tif_name,
				  "PixarLogDecode: unsupported bits/sample: %d", 
				  td->td_bitspersample);
			return (0);
		}
	}

	return (1);
}

static int
PixarLogSetupEncode(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState* sp = EncoderState(tif);
	static const char module[] = "PixarLogSetupEncode";

	assert(sp != NULL);

	/* for some reason, we can't do this in TIFFInitPixarLog */

	sp->stride = (td->td_planarconfig == PLANARCONFIG_CONTIG ?
	    td->td_samplesperpixel : 1);
	sp->tbuf = (uint16 *) _TIFFmalloc(sp->stride * 
		td->td_imagewidth * td->td_rowsperstrip * sizeof(uint16));
	if (sp->user_datafmt == PIXARLOGDATAFMT_UNKNOWN)
		sp->user_datafmt = PixarLogGuessDataFmt(td);
	if (sp->user_datafmt == PIXARLOGDATAFMT_UNKNOWN) {
		TIFFError(module, "PixarLog compression can't handle %d bit linear encodings", td->td_bitspersample);
		return (0);
	}

	if (deflateInit(&sp->stream, sp->quality) != Z_OK) {
		TIFFError(module, "%s: %s", tif->tif_name, sp->stream.msg);
		return (0);
	} else {
		sp->state |= PLSTATE_INIT;
		return (1);
	}
}

/*
 * Reset encoding state at the start of a strip.
 */
static int
PixarLogPreEncode(TIFF* tif, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState *sp = EncoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->stream.next_out = tif->tif_rawdata;
	sp->stream.avail_out = tif->tif_rawdatasize;
	return (deflateReset(&sp->stream) == Z_OK);
}

static void
horizontalDifferenceF(float *ip, int n, int stride, uint16 *wp, uint16 *FromLT2)
{

    register int  r1, g1, b1, a1, r2, g2, b2, a2, mask;
    register float  fltsize = Fltsize;

#define  CLAMP(v) ( (v<(float)0.)   ? 0				\
		  : (v<(float)2.)   ? FromLT2[(int)(v*fltsize)]	\
		  : (v>(float)24.2) ? 2047			\
		  : LogK1*log(v*LogK2) + 0.5 )

    mask = CODE_MASK;
    if (n >= stride) {
	if (stride == 3) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);
	    n -= 3;
	    while (n > 0) {
		n -= 3;
		wp += 3;
		ip += 3;
		r1 = CLAMP(ip[0]); wp[0] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[1]); wp[1] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[2]); wp[2] = (b1-b2) & mask; b2 = b1;
	    }
	} else if (stride == 4) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);  a2 = wp[3] = CLAMP(ip[3]);
	    n -= 4;
	    while (n > 0) {
		n -= 4;
		wp += 4;
		ip += 4;
		r1 = CLAMP(ip[0]); wp[0] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[1]); wp[1] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[2]); wp[2] = (b1-b2) & mask; b2 = b1;
		a1 = CLAMP(ip[3]); wp[3] = (a1-a2) & mask; a2 = a1;
	    }
	} else {
	    ip += n - 1;	/* point to last one */
	    wp += n - 1;	/* point to last one */
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride, wp[0] = CLAMP(ip[0]);
				wp[stride] -= wp[0];
				wp[stride] &= mask;
				wp--; ip--)
		n -= stride;
	    }
	    REPEAT(stride, wp[0] = CLAMP(ip[0]); wp--; ip--)
	}
    }
}

static void
horizontalDifference16(unsigned short *ip, int n, int stride, 
	unsigned short *wp, uint16 *From14)
{
    register int  r1, g1, b1, a1, r2, g2, b2, a2, mask;

/* assumption is unsigned pixel values */
#undef   CLAMP
#define  CLAMP(v) From14[(v) >> 2]

    mask = CODE_MASK;
    if (n >= stride) {
	if (stride == 3) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);
	    n -= 3;
	    while (n > 0) {
		n -= 3;
		wp += 3;
		ip += 3;
		r1 = CLAMP(ip[0]); wp[0] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[1]); wp[1] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[2]); wp[2] = (b1-b2) & mask; b2 = b1;
	    }
	} else if (stride == 4) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);  a2 = wp[3] = CLAMP(ip[3]);
	    n -= 4;
	    while (n > 0) {
		n -= 4;
		wp += 4;
		ip += 4;
		r1 = CLAMP(ip[0]); wp[0] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[1]); wp[1] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[2]); wp[2] = (b1-b2) & mask; b2 = b1;
		a1 = CLAMP(ip[3]); wp[3] = (a1-a2) & mask; a2 = a1;
	    }
	} else {
	    ip += n - 1;	/* point to last one */
	    wp += n - 1;	/* point to last one */
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride, wp[0] = CLAMP(ip[0]);
				wp[stride] -= wp[0];
				wp[stride] &= mask;
				wp--; ip--)
		n -= stride;
	    }
	    REPEAT(stride, wp[0] = CLAMP(ip[0]); wp--; ip--)
	}
    }
}


static void
horizontalDifference8(unsigned char *ip, int n, int stride, 
	unsigned short *wp, uint16 *From8)
{
    register int  r1, g1, b1, a1, r2, g2, b2, a2, mask;

#undef	 CLAMP
#define  CLAMP(v) (From8[(v)])

    mask = CODE_MASK;
    if (n >= stride) {
	if (stride == 3) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);
	    n -= 3;
	    while (n > 0) {
		n -= 3;
		r1 = CLAMP(ip[3]); wp[3] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[4]); wp[4] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[5]); wp[5] = (b1-b2) & mask; b2 = b1;
		wp += 3;
		ip += 3;
	    }
	} else if (stride == 4) {
	    r2 = wp[0] = CLAMP(ip[0]);  g2 = wp[1] = CLAMP(ip[1]);
	    b2 = wp[2] = CLAMP(ip[2]);  a2 = wp[3] = CLAMP(ip[3]);
	    n -= 4;
	    while (n > 0) {
		n -= 4;
		r1 = CLAMP(ip[4]); wp[4] = (r1-r2) & mask; r2 = r1;
		g1 = CLAMP(ip[5]); wp[5] = (g1-g2) & mask; g2 = g1;
		b1 = CLAMP(ip[6]); wp[6] = (b1-b2) & mask; b2 = b1;
		a1 = CLAMP(ip[7]); wp[7] = (a1-a2) & mask; a2 = a1;
		wp += 4;
		ip += 4;
	    }
	} else {
	    wp += n + stride - 1;	/* point to last one */
	    ip += n + stride - 1;	/* point to last one */
	    n -= stride;
	    while (n > 0) {
		REPEAT(stride, wp[0] = CLAMP(ip[0]);
				wp[stride] -= wp[0];
				wp[stride] &= mask;
				wp--; ip--)
		n -= stride;
	    }
	    REPEAT(stride, wp[0] = CLAMP(ip[0]); wp--; ip--)
	}
    }
}

/*
 * Encode a chunk of pixels.
 */
static int
PixarLogEncode(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;
	PixarLogState *sp = EncoderState(tif);
	static const char module[] = "PixarLogEncode";
	int 	i, n, llen;
	unsigned short * up;

	(void) s;

	switch (sp->user_datafmt) {
	case PIXARLOGDATAFMT_FLOAT:
		n = cc / sizeof(float);		/* XXX float == 32 bits */
		break;
	case PIXARLOGDATAFMT_16BIT:
	case PIXARLOGDATAFMT_12BITPICIO:
	case PIXARLOGDATAFMT_11BITLOG:
		n = cc / sizeof(uint16);	/* XXX uint16 == 16 bits */
		break;
	case PIXARLOGDATAFMT_8BIT:
	case PIXARLOGDATAFMT_8BITABGR:
		n = cc;
		break;
	default:
		TIFFError(tif->tif_name,
			"%d bit input not supported in PixarLog",
			td->td_bitspersample);
		return 0;
	}

	llen = sp->stride * td->td_imagewidth;

	for (i = 0, up = sp->tbuf; i < n; i += llen, up += llen) {
		switch (sp->user_datafmt)  {
		case PIXARLOGDATAFMT_FLOAT:
			horizontalDifferenceF((float *)bp, llen, 
				sp->stride, up, sp->FromLT2);
			bp += llen * sizeof(float);
			break;
		case PIXARLOGDATAFMT_16BIT:
			horizontalDifference16((uint16 *)bp, llen, 
				sp->stride, up, sp->From14);
			bp += llen * sizeof(uint16);
			break;
		case PIXARLOGDATAFMT_8BIT:
			horizontalDifference8((unsigned char *)bp, llen, 
				sp->stride, up, sp->From8);
			bp += llen * sizeof(unsigned char);
			break;
		default:
			TIFFError(tif->tif_name,
				"%d bit input not supported in PixarLog",
				td->td_bitspersample);
			return 0;
		}
	}
 
	sp->stream.next_in = (unsigned char *) sp->tbuf;
	sp->stream.avail_in = n * sizeof(uint16);

	do {
		if (deflate(&sp->stream, Z_NO_FLUSH) != Z_OK) {
			TIFFError(module, "%s: Encoder error: %s",
			    tif->tif_name, sp->stream.msg);
			return (0);
		}
		if (sp->stream.avail_out == 0) {
			tif->tif_rawcc = tif->tif_rawdatasize;
			TIFFFlushData1(tif);
			sp->stream.next_out = tif->tif_rawdata;
			sp->stream.avail_out = tif->tif_rawdatasize;
		}
	} while (sp->stream.avail_in > 0);
	return (1);
}

/*
 * Finish off an encoded strip by flushing the last
 * string and tacking on an End Of Information code.
 */

static int
PixarLogPostEncode(TIFF* tif)
{
	PixarLogState *sp = EncoderState(tif);
	static const char module[] = "PixarLogPostEncode";
	int state;

	sp->stream.avail_in = 0;

	do {
		state = deflate(&sp->stream, Z_FINISH);
		switch (state) {
		case Z_STREAM_END:
		case Z_OK:
		    if (sp->stream.avail_out != tif->tif_rawdatasize) {
			    tif->tif_rawcc =
				tif->tif_rawdatasize - sp->stream.avail_out;
			    TIFFFlushData1(tif);
			    sp->stream.next_out = tif->tif_rawdata;
			    sp->stream.avail_out = tif->tif_rawdatasize;
		    }
		    break;
		default:
		    TIFFError(module, "%s: zlib error: %s",
			tif->tif_name, sp->stream.msg);
		    return (0);
		}
	} while (state != Z_STREAM_END);
	return (1);
}

static void
PixarLogClose(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;

	/* In a really sneaky maneuver, on close, we covertly modify both
	 * bitspersample and sampleformat in the directory to indicate
	 * 8-bit linear.  This way, the decode "just works" even for
	 * readers that don't know about PixarLog, or how to set
	 * the PIXARLOGDATFMT pseudo-tag.
	 */
	td->td_bitspersample = 8;
	td->td_sampleformat = SAMPLEFORMAT_UINT;
}

static void
PixarLogCleanup(TIFF* tif)
{
	PixarLogState* sp = (PixarLogState*) tif->tif_data;

	if (sp) {
		if (sp->FromLT2) _TIFFfree(sp->FromLT2);
		if (sp->From14) _TIFFfree(sp->From14);
		if (sp->From8) _TIFFfree(sp->From8);
		if (sp->ToLinearF) _TIFFfree(sp->ToLinearF);
		if (sp->ToLinear16) _TIFFfree(sp->ToLinear16);
		if (sp->ToLinear8) _TIFFfree(sp->ToLinear8);
		if (sp->state&PLSTATE_INIT) {
			if (tif->tif_mode == O_RDONLY)
				inflateEnd(&sp->stream);
			else
				deflateEnd(&sp->stream);
		}
		if (sp->tbuf)
			_TIFFfree(sp->tbuf);
		_TIFFfree(sp);
		tif->tif_data = NULL;
	}
}

static int
PixarLogVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
    PixarLogState *sp = (PixarLogState *)tif->tif_data;
    int result;
    static const char module[] = "PixarLogVSetField";

    switch (tag) {
     case TIFFTAG_PIXARLOGQUALITY:
		sp->quality = va_arg(ap, int);
		if (tif->tif_mode != O_RDONLY && (sp->state&PLSTATE_INIT)) {
			if (deflateParams(&sp->stream,
			    sp->quality, Z_DEFAULT_STRATEGY) != Z_OK) {
				TIFFError(module, "%s: zlib error: %s",
					tif->tif_name, sp->stream.msg);
				return (0);
			}
		}
		return (1);
     case TIFFTAG_PIXARLOGDATAFMT:
	sp->user_datafmt = va_arg(ap, int);
	/* Tweak the TIFF header so that the rest of libtiff knows what
	 * size of data will be passed between app and library, and
	 * assume that the app knows what it is doing and is not
	 * confused by these header manipulations...
	 */
	switch (sp->user_datafmt) {
	 case PIXARLOGDATAFMT_8BIT:
	 case PIXARLOGDATAFMT_8BITABGR:
	    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
	    break;
	 case PIXARLOGDATAFMT_11BITLOG:
	    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
	    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
	    break;
	 case PIXARLOGDATAFMT_12BITPICIO:
	    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
	    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_INT);
	    break;
	 case PIXARLOGDATAFMT_16BIT:
	    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
	    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
	    break;
	 case PIXARLOGDATAFMT_FLOAT:
	    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
	    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
	    break;
	}
	/*
	 * Must recalculate sizes should bits/sample change.
	 */
	tif->tif_tilesize = TIFFTileSize(tif);
	tif->tif_scanlinesize = TIFFScanlineSize(tif);
	result = 1;		/* NB: pseudo tag */
	break;
     default:
	result = (*sp->vsetparent)(tif, tag, ap);
    }
    return (result);
}

static int
PixarLogVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
    PixarLogState *sp = (PixarLogState *)tif->tif_data;

    switch (tag) {
     case TIFFTAG_PIXARLOGQUALITY:
	*va_arg(ap, int*) = sp->quality;
	break;
     case TIFFTAG_PIXARLOGDATAFMT:
	*va_arg(ap, int*) = sp->user_datafmt;
	break;
     default:
	return (*sp->vgetparent)(tif, tag, ap);
    }
    return (1);
}

static const TIFFFieldInfo pixarlogFieldInfo[] = {
    {TIFFTAG_PIXARLOGDATAFMT,0,0,TIFF_ANY,  FIELD_PSEUDO,FALSE,FALSE,""},
    {TIFFTAG_PIXARLOGQUALITY,0,0,TIFF_ANY,  FIELD_PSEUDO,FALSE,FALSE,""}
};

int
TIFFInitPixarLog(TIFF* tif, int scheme)
{
	PixarLogState* sp;

	assert(scheme == COMPRESSION_PIXARLOG);

	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	tif->tif_data = (tidata_t) _TIFFmalloc(sizeof (PixarLogState));
	if (tif->tif_data == NULL)
		goto bad;
	sp = (PixarLogState*) tif->tif_data;
	memset(sp, 0, sizeof (*sp));
	sp->stream.data_type = Z_BINARY;
	sp->user_datafmt = PIXARLOGDATAFMT_UNKNOWN;

	/*
	 * Install codec methods.
	 */
	tif->tif_setupdecode = PixarLogSetupDecode;
	tif->tif_predecode = PixarLogPreDecode;
	tif->tif_decoderow = PixarLogDecode;
	tif->tif_decodestrip = PixarLogDecode;
	tif->tif_decodetile = PixarLogDecode;
	tif->tif_setupencode = PixarLogSetupEncode;
	tif->tif_preencode = PixarLogPreEncode;
	tif->tif_postencode = PixarLogPostEncode;
	tif->tif_encoderow = PixarLogEncode;
	tif->tif_encodestrip = PixarLogEncode;
	tif->tif_encodetile = PixarLogEncode;
	tif->tif_close = PixarLogClose;
	tif->tif_cleanup = PixarLogCleanup;

	/* Override SetField so we can handle our private pseudo-tag */
	_TIFFMergeFieldInfo(tif, pixarlogFieldInfo, N(pixarlogFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = PixarLogVGetField;   /* hook for codec tags */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = PixarLogVSetField;   /* hook for codec tags */

	/* Default values for codec-specific fields */
	sp->quality = Z_DEFAULT_COMPRESSION; /* default comp. level */
	sp->state = 0;

	/* we don't wish to use the predictor, 
	 * the default is none, which predictor value 1
	 */
	(void) TIFFPredictorInit(tif);

	/*
	 * build the companding tables 
	 */
	PixarLogMakeTables(sp);

	return (1);
bad:
	TIFFError("TIFFInitPixarLog", "No space for PixarLog state block");
	return (0);
}
#endif /* PIXARLOG_SUPPORT */


/* ==== tif_predict.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_predict.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Predictor Tag Support (used by multiple codecs).
 */
#include "tiffiop.h"
#include "tif_predict.h"

#include <assert.h>

#define	PredictorState(tif)	((TIFFPredictorState*) (tif)->tif_data)

static	void horAcc8(TIFF*, tidata_t, tsize_t);
static	void horAcc16(TIFF*, tidata_t, tsize_t);
static	void swabHorAcc16(TIFF*, tidata_t, tsize_t);
static	void horDiff8(TIFF*, tidata_t, tsize_t);
static	void horDiff16(TIFF*, tidata_t, tsize_t);
static	int PredictorDecodeRow(TIFF*, tidata_t, tsize_t, tsample_t);
static	int PredictorDecodeTile(TIFF*, tidata_t, tsize_t, tsample_t);
static	int PredictorEncodeRow(TIFF*, tidata_t, tsize_t, tsample_t);
static	int PredictorEncodeTile(TIFF*, tidata_t, tsize_t, tsample_t);

static int
PredictorSetup(TIFF* tif)
{
	TIFFPredictorState* sp = PredictorState(tif);
	TIFFDirectory* td = &tif->tif_dir;

	if (sp->predictor == 1)		/* no differencing */
		return (1);
	if (sp->predictor != 2) {
		TIFFError(tif->tif_name, "\"Predictor\" value %d not supported",
		    sp->predictor);
		return (0);
	}
	if (td->td_bitspersample != 8 && td->td_bitspersample != 16) {
		TIFFError(tif->tif_name,
    "Horizontal differencing \"Predictor\" not supported with %d-bit samples",
		    td->td_bitspersample);
		return (0);
	}
	sp->stride = (td->td_planarconfig == PLANARCONFIG_CONTIG ?
	    td->td_samplesperpixel : 1);
	/*
	 * Calculate the scanline/tile-width size in bytes.
	 */
	if (isTiled(tif))
		sp->rowsize = TIFFTileRowSize(tif);
	else
		sp->rowsize = TIFFScanlineSize(tif);
	return (1);
}

static int
PredictorSetupDecode(TIFF* tif)
{
	TIFFPredictorState* sp = PredictorState(tif);
	TIFFDirectory* td = &tif->tif_dir;

	if (!(*sp->setupdecode)(tif) || !PredictorSetup(tif))
		return (0);
	if (sp->predictor == 2) {
		switch (td->td_bitspersample) {
		case 8:  sp->pfunc = horAcc8; break;
		case 16: sp->pfunc = horAcc16; break;
		}
		/*
		 * Override default decoding method with
		 * one that does the predictor stuff.
		 */
		sp->coderow = tif->tif_decoderow;
		tif->tif_decoderow = PredictorDecodeRow;
		sp->codestrip = tif->tif_decodestrip;
		tif->tif_decodestrip = PredictorDecodeTile;
		sp->codetile = tif->tif_decodetile;
		tif->tif_decodetile = PredictorDecodeTile;
		/*
		 * If the data is horizontally differenced
		 * 16-bit data that requires byte-swapping,
		 * then it must be byte swapped before the
		 * accumulation step.  We do this with a
		 * special-purpose routine and override the
		 * normal post decoding logic that the library
		 * setup when the directory was read.
		 */
		if (tif->tif_flags&TIFF_SWAB) {
			if (sp->pfunc == horAcc16) {
				sp->pfunc = swabHorAcc16;
				tif->tif_postdecode = _TIFFNoPostDecode;
			} /* else handle 32-bit case... */
		}
	}
	return (1);
}

static int
PredictorSetupEncode(TIFF* tif)
{
	TIFFPredictorState* sp = PredictorState(tif);
	TIFFDirectory* td = &tif->tif_dir;

	if (!(*sp->setupencode)(tif) || !PredictorSetup(tif))
		return (0);
	if (sp->predictor == 2) {
		switch (td->td_bitspersample) {
		case 8:  sp->pfunc = horDiff8; break;
		case 16: sp->pfunc = horDiff16; break;
		}
		/*
		 * Override default encoding method with
		 * one that does the predictor stuff.
		 */
		sp->coderow = tif->tif_encoderow;
		tif->tif_encoderow = PredictorEncodeRow;
		sp->codestrip = tif->tif_encodestrip;
		tif->tif_encodestrip = PredictorEncodeTile;
		sp->codetile = tif->tif_encodetile;
		tif->tif_encodetile = PredictorEncodeTile;
	}
	return (1);
}

#define REPEAT4(n, op)		\
    switch (n) {		\
    default: { int i; for (i = n-4; i > 0; i--) { op; } } \
    case 4:  op;		\
    case 3:  op;		\
    case 2:  op;		\
    case 1:  op;		\
    case 0:  ;			\
    }

static void
horAcc8(TIFF* tif, tidata_t cp0, tsize_t cc)
{
	TIFFPredictorState* sp = PredictorState(tif);
	tsize_t stride = sp->stride;

	char* cp = (char*) cp0;
	if (cc > stride) {
		cc -= stride;
		/*
		 * Pipeline the most common cases.
		 */
		if (stride == 3)  {
			u_int cr = cp[0];
			u_int cg = cp[1];
			u_int cb = cp[2];
			do {
				cc -= 3, cp += 3;
				cp[0] = (cr += cp[0]);
				cp[1] = (cg += cp[1]);
				cp[2] = (cb += cp[2]);
			} while ((int32) cc > 0);
		} else if (stride == 4)  {
			u_int cr = cp[0];
			u_int cg = cp[1];
			u_int cb = cp[2];
			u_int ca = cp[3];
			do {
				cc -= 4, cp += 4;
				cp[0] = (cr += cp[0]);
				cp[1] = (cg += cp[1]);
				cp[2] = (cb += cp[2]);
				cp[3] = (ca += cp[3]);
			} while ((int32) cc > 0);
		} else  {
			do {
				REPEAT4(stride, cp[stride] += *cp; cp++)
				cc -= stride;
			} while ((int32) cc > 0);
		}
	}
}

static void
swabHorAcc16(TIFF* tif, tidata_t cp0, tsize_t cc)
{
	TIFFPredictorState* sp = PredictorState(tif);
	tsize_t stride = sp->stride;
	uint16* wp = (uint16*) cp0;
	tsize_t wc = cc / 2;

	if (wc > stride) {
		TIFFSwabArrayOfShort(wp, wc);
		wc -= stride;
		do {
			REPEAT4(stride, wp[stride] += wp[0]; wp++)
			wc -= stride;
		} while ((int32) wc > 0);
	}
}

static void
horAcc16(TIFF* tif, tidata_t cp0, tsize_t cc)
{
	tsize_t stride = PredictorState(tif)->stride;
	uint16* wp = (uint16*) cp0;
	tsize_t wc = cc / 2;

	if (wc > stride) {
		wc -= stride;
		do {
			REPEAT4(stride, wp[stride] += wp[0]; wp++)
			wc -= stride;
		} while ((int32) wc > 0);
	}
}

/*
 * Decode a scanline and apply the predictor routine.
 */
static int
PredictorDecodeRow(TIFF* tif, tidata_t op0, tsize_t occ0, tsample_t s)
{
	TIFFPredictorState *sp = PredictorState(tif);

	assert(sp != NULL);
	assert(sp->coderow != NULL);
	assert(sp->pfunc != NULL);
	if ((*sp->coderow)(tif, op0, occ0, s)) {
		(*sp->pfunc)(tif, op0, occ0);
		return (1);
	} else
		return (0);
}

/*
 * Decode a tile/strip and apply the predictor routine.
 * Note that horizontal differencing must be done on a
 * row-by-row basis.  The width of a "row" has already
 * been calculated at pre-decode time according to the
 * strip/tile dimensions.
 */
static int
PredictorDecodeTile(TIFF* tif, tidata_t op0, tsize_t occ0, tsample_t s)
{
	TIFFPredictorState *sp = PredictorState(tif);

	assert(sp != NULL);
	assert(sp->codetile != NULL);
	if ((*sp->codetile)(tif, op0, occ0, s)) {
		tsize_t rowsize = sp->rowsize;
		assert(rowsize > 0);
		assert(sp->pfunc != NULL);
		while ((long)occ0 > 0) {
			(*sp->pfunc)(tif, op0, (tsize_t) rowsize);
			occ0 -= rowsize;
			op0 += rowsize;
		}
		return (1);
	} else
		return (0);
}

static void
horDiff8(TIFF* tif, tidata_t cp0, tsize_t cc)
{
	TIFFPredictorState* sp = PredictorState(tif);
	tsize_t stride = sp->stride;
	char* cp = (char*) cp0;

	if (cc > stride) {
		cc -= stride;
		/*
		 * Pipeline the most common cases.
		 */
		if (stride == 3) {
			int r1, g1, b1;
			int r2 = cp[0];
			int g2 = cp[1];
			int b2 = cp[2];
			do {
				r1 = cp[3]; cp[3] = r1-r2; r2 = r1;
				g1 = cp[4]; cp[4] = g1-g2; g2 = g1;
				b1 = cp[5]; cp[5] = b1-b2; b2 = b1;
				cp += 3;
			} while ((int32)(cc -= 3) > 0);
		} else if (stride == 4) {
			int r1, g1, b1, a1;
			int r2 = cp[0];
			int g2 = cp[1];
			int b2 = cp[2];
			int a2 = cp[3];
			do {
				r1 = cp[4]; cp[4] = r1-r2; r2 = r1;
				g1 = cp[5]; cp[5] = g1-g2; g2 = g1;
				b1 = cp[6]; cp[6] = b1-b2; b2 = b1;
				a1 = cp[7]; cp[7] = a1-a2; a2 = a1;
				cp += 4;
			} while ((int32)(cc -= 4) > 0);
		} else {
			cp += cc - 1;
			do {
				REPEAT4(stride, cp[stride] -= cp[0]; cp--)
			} while ((int32)(cc -= stride) > 0);
		}
	}
}

static void
horDiff16(TIFF* tif, tidata_t cp0, tsize_t cc)
{
	TIFFPredictorState* sp = PredictorState(tif);
	tsize_t stride = sp->stride;
	int16 *wp = (int16*) cp0;
	tsize_t wc = cc/2;

	if (wc > stride) {
		wc -= stride;
		wp += wc - 1;
		do {
			REPEAT4(stride, wp[stride] -= wp[0]; wp--)
			wc -= stride;
		} while ((int32) wc > 0);
	}
}

static int
PredictorEncodeRow(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	TIFFPredictorState *sp = PredictorState(tif);

	assert(sp != NULL);
	assert(sp->pfunc != NULL);
	assert(sp->coderow != NULL);
/* XXX horizontal differencing alters user's data XXX */
	(*sp->pfunc)(tif, bp, cc);
	return ((*sp->coderow)(tif, bp, cc, s));
}

static int
PredictorEncodeTile(TIFF* tif, tidata_t bp0, tsize_t cc0, tsample_t s)
{
	TIFFPredictorState *sp = PredictorState(tif);
	tsize_t cc = cc0, rowsize;
	u_char* bp = bp0;

	assert(sp != NULL);
	assert(sp->pfunc != NULL);
	assert(sp->codetile != NULL);
	rowsize = sp->rowsize;
	assert(rowsize > 0);
	while ((long)cc > 0) {
		(*sp->pfunc)(tif, bp, (tsize_t) rowsize);
		cc -= rowsize;
		bp += rowsize;
	}
	return ((*sp->codetile)(tif, bp0, cc0, s));
}

#define	FIELD_PREDICTOR	(FIELD_CODEC+0)		/* XXX */

static const TIFFFieldInfo predictFieldInfo[] = {
    { TIFFTAG_PREDICTOR,	 1, 1, TIFF_SHORT,	FIELD_PREDICTOR,
      FALSE,	FALSE,	"Predictor" },
};
#define	N(a)	(sizeof (a) / sizeof (a[0]))

static int
PredictorVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	TIFFPredictorState *sp = PredictorState(tif);

	switch (tag) {
	case TIFFTAG_PREDICTOR:
		sp->predictor = (uint16) va_arg(ap, int);
		TIFFSetFieldBit(tif, FIELD_PREDICTOR);
		break;
	default:
		return (*sp->vsetparent)(tif, tag, ap);
	}
	tif->tif_flags |= TIFF_DIRTYDIRECT;
	return (1);
}

static int
PredictorVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	TIFFPredictorState *sp = PredictorState(tif);

	switch (tag) {
	case TIFFTAG_PREDICTOR:
		*va_arg(ap, uint16*) = sp->predictor;
		break;
	default:
		return (*sp->vgetparent)(tif, tag, ap);
	}
	return (1);
}

static void
PredictorPrintDir(TIFF* tif, FILE* fd, long flags)
{
	TIFFPredictorState* sp = PredictorState(tif);

	(void) flags;
	if (TIFFFieldSet(tif,FIELD_PREDICTOR)) {
		fprintf(fd, "  Predictor: ");
		switch (sp->predictor) {
		case 1: fprintf(fd, "none "); break;
		case 2: fprintf(fd, "horizontal differencing "); break;
		}
		fprintf(fd, "%u (0x%x)\n", sp->predictor, sp->predictor);
	}
	if (sp->printdir)
		(*sp->printdir)(tif, fd, flags);
}

int
TIFFPredictorInit(TIFF* tif)
{
	TIFFPredictorState* sp = PredictorState(tif);

	/*
	 * Merge codec-specific tag information and
	 * override parent get/set field methods.
	 */
	_TIFFMergeFieldInfo(tif, predictFieldInfo, N(predictFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = PredictorVGetField;/* hook for predictor tag */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = PredictorVSetField;/* hook for predictor tag */
	sp->printdir = tif->tif_printdir;
	tif->tif_printdir = PredictorPrintDir;	/* hook for predictor tag */

	sp->setupdecode = tif->tif_setupdecode;
	tif->tif_setupdecode = PredictorSetupDecode;
	sp->setupencode = tif->tif_setupencode;
	tif->tif_setupencode = PredictorSetupEncode;

	sp->predictor = 1;			/* default value */
	sp->pfunc = NULL;			/* no predictor routine */
	return (1);
}


/* ==== tif_print.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_print.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Directory Printing Support
 */
#include "tiffiop.h"
#include <stdio.h>

#include <ctype.h>

static const char *photoNames[] = {
    "min-is-white",				/* PHOTOMETRIC_MINISWHITE */
    "min-is-black",				/* PHOTOMETRIC_MINISBLACK */
    "RGB color",				/* PHOTOMETRIC_RGB */
    "palette color (RGB from colormap)",	/* PHOTOMETRIC_PALETTE */
    "transparency mask",			/* PHOTOMETRIC_MASK */
    "separated",				/* PHOTOMETRIC_SEPARATED */
    "YCbCr",					/* PHOTOMETRIC_YCBCR */
    "7 (0x7)",
    "CIE L*a*b*",				/* PHOTOMETRIC_CIELAB */
};
#define	NPHOTONAMES	(sizeof (photoNames) / sizeof (photoNames[0]))

static const char *orientNames[] = {
    "0 (0x0)",
    "row 0 top, col 0 lhs",			/* ORIENTATION_TOPLEFT */
    "row 0 top, col 0 rhs",			/* ORIENTATION_TOPRIGHT */
    "row 0 bottom, col 0 rhs",			/* ORIENTATION_BOTRIGHT */
    "row 0 bottom, col 0 lhs",			/* ORIENTATION_BOTLEFT */
    "row 0 lhs, col 0 top",			/* ORIENTATION_LEFTTOP */
    "row 0 rhs, col 0 top",			/* ORIENTATION_RIGHTTOP */
    "row 0 rhs, col 0 bottom",			/* ORIENTATION_RIGHTBOT */
    "row 0 lhs, col 0 bottom",			/* ORIENTATION_LEFTBOT */
};
#define	NORIENTNAMES	(sizeof (orientNames) / sizeof (orientNames[0]))

/*
 * Print the contents of the current directory
 * to the specified stdio file stream.
 */
void
TIFFPrintDirectory(TIFF* tif, FILE* fd, long flags)
{
	register TIFFDirectory *td;
	char *sep;
	uint16 i;
	long l, n;

	fprintf(fd, "TIFF Directory at offset 0x%lx\n", (long) tif->tif_diroff);
	td = &tif->tif_dir;
	if (TIFFFieldSet(tif,FIELD_SUBFILETYPE)) {
		fprintf(fd, "  Subfile Type:");
		sep = " ";
		if (td->td_subfiletype & FILETYPE_REDUCEDIMAGE) {
			fprintf(fd, "%sreduced-resolution image", sep);
			sep = "/";
		}
		if (td->td_subfiletype & FILETYPE_PAGE) {
			fprintf(fd, "%smulti-page document", sep);
			sep = "/";
		}
		if (td->td_subfiletype & FILETYPE_MASK)
			fprintf(fd, "%stransparency mask", sep);
		fprintf(fd, " (%lu = 0x%lx)\n",
		    (long) td->td_subfiletype, (long) td->td_subfiletype);
	}
	if (TIFFFieldSet(tif,FIELD_IMAGEDIMENSIONS)) {
		fprintf(fd, "  Image Width: %lu Image Length: %lu",
		    (u_long) td->td_imagewidth, (u_long) td->td_imagelength);
		if (TIFFFieldSet(tif,FIELD_IMAGEDEPTH))
			fprintf(fd, " Image Depth: %lu",
			    (u_long) td->td_imagedepth);
		fprintf(fd, "\n");
	}

 	/* Begin Pixar */
 	if (TIFFFieldSet(tif,FIELD_IMAGEFULLWIDTH) ||
 	    TIFFFieldSet(tif,FIELD_IMAGEFULLLENGTH)) {
	  fprintf(fd, "  Pixar Full Image Width: %lu Full Image Length: %lu\n",
		  (u_long) td->td_imagefullwidth,
		  (u_long) td->td_imagefulllength);
 	}
 	if (TIFFFieldSet(tif,FIELD_TEXTUREFORMAT))
	  _TIFFprintAsciiTag(fd, "Texture Format", td->td_textureformat);
 	if (TIFFFieldSet(tif,FIELD_WRAPMODES))
	  _TIFFprintAsciiTag(fd, "Texture Wrap Modes", td->td_wrapmodes);
 	if (TIFFFieldSet(tif,FIELD_FOVCOT))
	  fprintf(fd, "  Field of View Cotangent: %g\n", td->td_fovcot);
	if (TIFFFieldSet(tif,FIELD_MATRIX_WORLDTOSCREEN)) {
	  typedef float	Matrix[4][4];
	  Matrix*		m = (Matrix*)td->td_matrixWorldToScreen;
	  
	  fprintf(fd, "  Matrix NP:\n\t%g %g %g %g\n\t%g %g %g %g\n\t%g %g %g %g\n\t%g %g %g %g\n",
		  (*m)[0][0], (*m)[0][1], (*m)[0][2], (*m)[0][3],
		  (*m)[1][0], (*m)[1][1], (*m)[1][2], (*m)[1][3],
		  (*m)[2][0], (*m)[2][1], (*m)[2][2], (*m)[2][3],
		  (*m)[3][0], (*m)[3][1], (*m)[3][2], (*m)[3][3]);
 	}
 	if (TIFFFieldSet(tif,FIELD_MATRIX_WORLDTOCAMERA)) {
	  typedef float	Matrix[4][4];
	  Matrix*		m = (Matrix*)td->td_matrixWorldToCamera;
	  
	  fprintf(fd, "  Matrix Nl:\n\t%g %g %g %g\n\t%g %g %g %g\n\t%g %g %g %g\n\t%g %g %g %g\n",
		  (*m)[0][0], (*m)[0][1], (*m)[0][2], (*m)[0][3],
		  (*m)[1][0], (*m)[1][1], (*m)[1][2], (*m)[1][3],
		  (*m)[2][0], (*m)[2][1], (*m)[2][2], (*m)[2][3],
		  (*m)[3][0], (*m)[3][1], (*m)[3][2], (*m)[3][3]);
 	}
 	/* End Pixar */
	
	if (TIFFFieldSet(tif,FIELD_TILEDIMENSIONS)) {
		fprintf(fd, "  Tile Width: %lu Tile Length: %lu",
		    (u_long) td->td_tilewidth, (u_long) td->td_tilelength);
		if (TIFFFieldSet(tif,FIELD_TILEDEPTH))
			fprintf(fd, " Tile Depth: %lu",
			    (u_long) td->td_tiledepth);
		fprintf(fd, "\n");
	}
	if (TIFFFieldSet(tif,FIELD_RESOLUTION)) {
		fprintf(fd, "  Resolution: %g, %g",
		    td->td_xresolution, td->td_yresolution);
		if (TIFFFieldSet(tif,FIELD_RESOLUTIONUNIT)) {
			switch (td->td_resolutionunit) {
			case RESUNIT_NONE:
				fprintf(fd, " (unitless)");
				break;
			case RESUNIT_INCH:
				fprintf(fd, " pixels/inch");
				break;
			case RESUNIT_CENTIMETER:
				fprintf(fd, " pixels/cm");
				break;
			default:
				fprintf(fd, " (unit %u = 0x%x)",
				    td->td_resolutionunit,
				    td->td_resolutionunit);
				break;
			}
		}
		fprintf(fd, "\n");
	}
	if (TIFFFieldSet(tif,FIELD_POSITION))
		fprintf(fd, "  Position: %g, %g\n",
		    td->td_xposition, td->td_yposition);
	if (TIFFFieldSet(tif,FIELD_BITSPERSAMPLE))
		fprintf(fd, "  Bits/Sample: %u\n", td->td_bitspersample);
	if (TIFFFieldSet(tif,FIELD_SAMPLEFORMAT)) {
		fprintf(fd, "  Sample Format: ");
		switch (td->td_sampleformat) {
		case SAMPLEFORMAT_VOID:
			fprintf(fd, "void\n");
			break;
		case SAMPLEFORMAT_INT:
			fprintf(fd, "signed integer\n");
			break;
		case SAMPLEFORMAT_UINT:
			fprintf(fd, "unsigned integer\n");
			break;
		case SAMPLEFORMAT_IEEEFP:
			fprintf(fd, "IEEE floating point\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_sampleformat, td->td_sampleformat);
			break;
		}
	}
	if (TIFFFieldSet(tif,FIELD_COMPRESSION)) {
		const TIFFCodec* c = TIFFFindCODEC(td->td_compression);
		fprintf(fd, "  Compression Scheme: ");
		if (c)
			fprintf(fd, "%s\n", c->name);
		else
			fprintf(fd, "%u (0x%x)\n",
			    td->td_compression, td->td_compression);
	}
	if (TIFFFieldSet(tif,FIELD_PHOTOMETRIC)) {
		fprintf(fd, "  Photometric Interpretation: ");
		if (td->td_photometric < NPHOTONAMES)
			fprintf(fd, "%s\n", photoNames[td->td_photometric]);
		else {
			switch (td->td_photometric) {
			case PHOTOMETRIC_LOGL:
				fprintf(fd, "CIE Log2(L)\n");
				break;
			case PHOTOMETRIC_LOGLUV:
				fprintf(fd, "CIE Log2(L) (u',v')\n");
				break;
			default:
				fprintf(fd, "%u (0x%x)\n",
				    td->td_photometric, td->td_photometric);
				break;
			}
		}
	}
	if (TIFFFieldSet(tif,FIELD_EXTRASAMPLES) && td->td_extrasamples) {
		fprintf(fd, "  Extra Samples: %u<", td->td_extrasamples);
		sep = "";
		for (i = 0; i < td->td_extrasamples; i++) {
			switch (td->td_sampleinfo[i]) {
			case EXTRASAMPLE_UNSPECIFIED:
				fprintf(fd, "%sunspecified", sep);
				break;
			case EXTRASAMPLE_ASSOCALPHA:
				fprintf(fd, "%sassoc-alpha", sep);
				break;
			case EXTRASAMPLE_UNASSALPHA:
				fprintf(fd, "%sunassoc-alpha", sep);
				break;
			default:
				fprintf(fd, "%s%u (0x%x)", sep,
				    td->td_sampleinfo[i], td->td_sampleinfo[i]);
				break;
			}
			sep = ", ";
		}
		fprintf(fd, ">\n");
	}
	if (TIFFFieldSet(tif,FIELD_STONITS)) {
		fprintf(fd, "  Sample to Nits conversion factor: %.4e\n",
				td->td_stonits);
	}
#ifdef CMYK_SUPPORT
	if (TIFFFieldSet(tif,FIELD_INKSET)) {
		fprintf(fd, "  Ink Set: ");
		switch (td->td_inkset) {
		case INKSET_CMYK:
			fprintf(fd, "CMYK\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_inkset, td->td_inkset);
			break;
		}
	}
	if (TIFFFieldSet(tif,FIELD_INKNAMES)) {
		char* cp;
		fprintf(fd, "  Ink Names: ");
		i = td->td_samplesperpixel;
		sep = "";
		for (cp = td->td_inknames; i > 0; cp = strchr(cp,'\0')+1, i--) {
			fprintf(fd, "%s", sep);
			_TIFFprintAscii(fd, cp);
			sep = ", ";
		}
	}
	if (TIFFFieldSet(tif,FIELD_NUMBEROFINKS))
		fprintf(fd, " Number of Inks: %u\n", td->td_ninks);
	if (TIFFFieldSet(tif,FIELD_DOTRANGE))
		fprintf(fd, "  Dot Range: %u-%u\n",
		    td->td_dotrange[0], td->td_dotrange[1]);
	if (TIFFFieldSet(tif,FIELD_TARGETPRINTER))
		_TIFFprintAsciiTag(fd, "Target Printer", td->td_targetprinter);
#endif
	if (TIFFFieldSet(tif,FIELD_THRESHHOLDING)) {
		fprintf(fd, "  Thresholding: ");
		switch (td->td_threshholding) {
		case THRESHHOLD_BILEVEL:
			fprintf(fd, "bilevel art scan\n");
			break;
		case THRESHHOLD_HALFTONE:
			fprintf(fd, "halftone or dithered scan\n");
			break;
		case THRESHHOLD_ERRORDIFFUSE:
			fprintf(fd, "error diffused\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_threshholding, td->td_threshholding);
			break;
		}
	}
	if (TIFFFieldSet(tif,FIELD_FILLORDER)) {
		fprintf(fd, "  FillOrder: ");
		switch (td->td_fillorder) {
		case FILLORDER_MSB2LSB:
			fprintf(fd, "msb-to-lsb\n");
			break;
		case FILLORDER_LSB2MSB:
			fprintf(fd, "lsb-to-msb\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_fillorder, td->td_fillorder);
			break;
		}
	}
#ifdef YCBCR_SUPPORT
	if (TIFFFieldSet(tif,FIELD_YCBCRSUBSAMPLING))
		fprintf(fd, "  YCbCr Subsampling: %u, %u\n",
		    td->td_ycbcrsubsampling[0], td->td_ycbcrsubsampling[1]);
	if (TIFFFieldSet(tif,FIELD_YCBCRPOSITIONING)) {
		fprintf(fd, "  YCbCr Positioning: ");
		switch (td->td_ycbcrpositioning) {
		case YCBCRPOSITION_CENTERED:
			fprintf(fd, "centered\n");
			break;
		case YCBCRPOSITION_COSITED:
			fprintf(fd, "cosited\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_ycbcrpositioning, td->td_ycbcrpositioning);
			break;
		}
	}
	if (TIFFFieldSet(tif,FIELD_YCBCRCOEFFICIENTS))
		fprintf(fd, "  YCbCr Coefficients: %g, %g, %g\n",
		    td->td_ycbcrcoeffs[0],
		    td->td_ycbcrcoeffs[1],
		    td->td_ycbcrcoeffs[2]);
#endif
	if (TIFFFieldSet(tif,FIELD_HALFTONEHINTS))
		fprintf(fd, "  Halftone Hints: light %u dark %u\n",
		    td->td_halftonehints[0], td->td_halftonehints[1]);
	if (TIFFFieldSet(tif,FIELD_ARTIST))
		_TIFFprintAsciiTag(fd, "Artist", td->td_artist);
	if (TIFFFieldSet(tif,FIELD_DATETIME))
		_TIFFprintAsciiTag(fd, "Date & Time", td->td_datetime);
	if (TIFFFieldSet(tif,FIELD_HOSTCOMPUTER))
		_TIFFprintAsciiTag(fd, "Host Computer", td->td_hostcomputer);
	if (TIFFFieldSet(tif,FIELD_SOFTWARE))
		_TIFFprintAsciiTag(fd, "Software", td->td_software);
	if (TIFFFieldSet(tif,FIELD_DOCUMENTNAME))
		_TIFFprintAsciiTag(fd, "Document Name", td->td_documentname);
	if (TIFFFieldSet(tif,FIELD_IMAGEDESCRIPTION))
		_TIFFprintAsciiTag(fd, "Image Description", td->td_imagedescription);
	if (TIFFFieldSet(tif,FIELD_MAKE))
		_TIFFprintAsciiTag(fd, "Make", td->td_make);
	if (TIFFFieldSet(tif,FIELD_MODEL))
		_TIFFprintAsciiTag(fd, "Model", td->td_model);
	if (TIFFFieldSet(tif,FIELD_ORIENTATION)) {
		fprintf(fd, "  Orientation: ");
		if (td->td_orientation < NORIENTNAMES)
			fprintf(fd, "%s\n", orientNames[td->td_orientation]);
		else
			fprintf(fd, "%u (0x%x)\n",
			    td->td_orientation, td->td_orientation);
	}
	if (TIFFFieldSet(tif,FIELD_SAMPLESPERPIXEL))
		fprintf(fd, "  Samples/Pixel: %u\n", td->td_samplesperpixel);
	if (TIFFFieldSet(tif,FIELD_ROWSPERSTRIP)) {
		fprintf(fd, "  Rows/Strip: ");
		if (td->td_rowsperstrip == (uint32) -1)
			fprintf(fd, "(infinite)\n");
		else
			fprintf(fd, "%lu\n", (u_long) td->td_rowsperstrip);
	}
	if (TIFFFieldSet(tif,FIELD_MINSAMPLEVALUE))
		fprintf(fd, "  Min Sample Value: %u\n", td->td_minsamplevalue);
	if (TIFFFieldSet(tif,FIELD_MAXSAMPLEVALUE))
		fprintf(fd, "  Max Sample Value: %u\n", td->td_maxsamplevalue);
	if (TIFFFieldSet(tif,FIELD_SMINSAMPLEVALUE))
		fprintf(fd, "  SMin Sample Value: %g\n",
		    td->td_sminsamplevalue);
	if (TIFFFieldSet(tif,FIELD_SMAXSAMPLEVALUE))
		fprintf(fd, "  SMax Sample Value: %g\n",
		    td->td_smaxsamplevalue);
	if (TIFFFieldSet(tif,FIELD_PLANARCONFIG)) {
		fprintf(fd, "  Planar Configuration: ");
		switch (td->td_planarconfig) {
		case PLANARCONFIG_CONTIG:
			fprintf(fd, "single image plane\n");
			break;
		case PLANARCONFIG_SEPARATE:
			fprintf(fd, "separate image planes\n");
			break;
		default:
			fprintf(fd, "%u (0x%x)\n",
			    td->td_planarconfig, td->td_planarconfig);
			break;
		}
	}
	if (TIFFFieldSet(tif,FIELD_PAGENAME))
		_TIFFprintAsciiTag(fd, "Page Name", td->td_pagename);
	if (TIFFFieldSet(tif,FIELD_PAGENUMBER))
		fprintf(fd, "  Page Number: %u-%u\n",
		    td->td_pagenumber[0], td->td_pagenumber[1]);
	if (TIFFFieldSet(tif,FIELD_COLORMAP)) {
		fprintf(fd, "  Color Map: ");
		if (flags & TIFFPRINT_COLORMAP) {
			fprintf(fd, "\n");
			n = 1L<<td->td_bitspersample;
			for (l = 0; l < n; l++)
				fprintf(fd, "   %5lu: %5u %5u %5u\n",
				    l,
				    td->td_colormap[0][l],
				    td->td_colormap[1][l],
				    td->td_colormap[2][l]);
		} else
			fprintf(fd, "(present)\n");
	}
#ifdef COLORIMETRY_SUPPORT
	if (TIFFFieldSet(tif,FIELD_WHITEPOINT))
		fprintf(fd, "  White Point: %g-%g\n",
		    td->td_whitepoint[0], td->td_whitepoint[1]);
	if (TIFFFieldSet(tif,FIELD_PRIMARYCHROMAS))
		fprintf(fd, "  Primary Chromaticities: %g,%g %g,%g %g,%g\n",
		    td->td_primarychromas[0], td->td_primarychromas[1],
		    td->td_primarychromas[2], td->td_primarychromas[3],
		    td->td_primarychromas[4], td->td_primarychromas[5]);
	if (TIFFFieldSet(tif,FIELD_REFBLACKWHITE)) {
		fprintf(fd, "  Reference Black/White:\n");
		for (i = 0; i < td->td_samplesperpixel; i++)
			fprintf(fd, "    %2d: %5g %5g\n",
			    i,
			    td->td_refblackwhite[2*i+0],
			    td->td_refblackwhite[2*i+1]);
	}
	if (TIFFFieldSet(tif,FIELD_TRANSFERFUNCTION)) {
		fprintf(fd, "  Transfer Function: ");
		if (flags & TIFFPRINT_CURVES) {
			fprintf(fd, "\n");
			n = 1L<<td->td_bitspersample;
			for (l = 0; l < n; l++) {
				fprintf(fd, "    %2lu: %5u",
				    l, td->td_transferfunction[0][l]);
				for (i = 1; i < td->td_samplesperpixel; i++)
					fprintf(fd, " %5u",
					    td->td_transferfunction[i][l]);
				fputc('\n', fd);
			}
		} else
			fprintf(fd, "(present)\n");
	}
#endif
#ifdef ICC_SUPPORT
	if (TIFFFieldSet(tif,FIELD_ICCPROFILE))
		fprintf(fd, "  ICC Profile: <present>, %lu bytes\n",
		    (u_long) td->td_profileLength);
#endif
#ifdef PHOTOSHOP_SUPPORT
 	if (TIFFFieldSet(tif,FIELD_PHOTOSHOP))
 		fprintf(fd, "  Photoshop Data: <present>, %lu bytes\n",
 		    (u_long) td->td_photoshopLength);
#endif
#ifdef IPTC_SUPPORT
 	if (TIFFFieldSet(tif,FIELD_RICHTIFFIPTC))
 		fprintf(fd, "  RichTIFFIPTC Data: <present>, %lu bytes\n",
 		    (u_long) td->td_richtiffiptcLength);
#endif
#if SUBIFD_SUPPORT
	if (TIFFFieldSet(tif, FIELD_SUBIFD)) {
		fprintf(fd, "  SubIFD Offsets:");
		for (i = 0; i < td->td_nsubifd; i++)
			fprintf(fd, " %5lu", (long) td->td_subifd[i]);
		fputc('\n', fd);
	}
#endif
	if (tif->tif_printdir)
		(*tif->tif_printdir)(tif, fd, flags);
	if ((flags & TIFFPRINT_STRIPS) &&
	    TIFFFieldSet(tif,FIELD_STRIPOFFSETS)) {
		tstrip_t s;

		fprintf(fd, "  %lu %s:\n",
		    (long) td->td_nstrips,
		    isTiled(tif) ? "Tiles" : "Strips");
		for (s = 0; s < td->td_nstrips; s++)
			fprintf(fd, "    %3lu: [%8lu, %8lu]\n",
			    (u_long) s,
			    (u_long) td->td_stripoffset[s],
			    (u_long) td->td_stripbytecount[s]);
	}
}

void
_TIFFprintAscii(FILE* fd, const char* cp)
{
	for (; *cp != '\0'; cp++) {
		const char* tp;

		if (isprint(*cp)) {
			fputc(*cp, fd);
			continue;
		}
		for (tp = "\tt\bb\rr\nn\vv"; *tp; tp++)
			if (*tp++ == *cp)
				break;
		if (*tp)
			fprintf(fd, "\\%c", *tp);
		else
			fprintf(fd, "\\%03o", *cp & 0xff);
	}
}

void
_TIFFprintAsciiTag(FILE* fd, const char* name, const char* value)
{
	fprintf(fd, "  %s: \"", name);
	_TIFFprintAscii(fd, value);
	fprintf(fd, "\"\n");
}


/* ==== tif_read.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_read.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 * Scanline-oriented Read Support
 */
#include "tiffiop.h"
#include <stdio.h>
#include <assert.h>

static	int TIFFFillStrip(TIFF*, tstrip_t);
static	int TIFFFillTile(TIFF*, ttile_t);
static	int TIFFStartStrip(TIFF*, tstrip_t);
static	int TIFFStartTile(TIFF*, ttile_t);
static	int TIFFCheckRead(TIFF*, int);

#define	NOSTRIP	((tstrip_t) -1)			/* undefined state */
#define	NOTILE	((ttile_t) -1)			/* undefined state */

/*
 * Seek to a random row+sample in a file.
 */
static int
TIFFSeek(TIFF* tif, uint32 row, tsample_t sample)
{
	register TIFFDirectory *td = &tif->tif_dir;
	tstrip_t strip;

	if (row >= td->td_imagelength) {	/* out of range */
		TIFFError(tif->tif_name, "%lu: Row out of range, max %lu",
		    (u_long) row, (u_long) td->td_imagelength);
		return (0);
	}
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
		if (sample >= td->td_samplesperpixel) {
			TIFFError(tif->tif_name,
			    "%lu: Sample out of range, max %lu",
			    (u_long) sample, (u_long) td->td_samplesperpixel);
			return (0);
		}
		strip = sample*td->td_stripsperimage + row/td->td_rowsperstrip;
	} else
		strip = row / td->td_rowsperstrip;
	if (strip != tif->tif_curstrip) { 	/* different strip, refill */
		if (!TIFFFillStrip(tif, strip))
			return (0);
	} else if (row < tif->tif_row) {
		/*
		 * Moving backwards within the same strip: backup
		 * to the start and then decode forward (below).
		 *
		 * NB: If you're planning on lots of random access within a
		 * strip, it's better to just read and decode the entire
		 * strip, and then access the decoded data in a random fashion.
		 */
		if (!TIFFStartStrip(tif, strip))
			return (0);
	}
	if (row != tif->tif_row) {
		/*
		 * Seek forward to the desired row.
		 */
		if (!(*tif->tif_seek)(tif, row - tif->tif_row))
			return (0);
		tif->tif_row = row;
	}
	return (1);
}

int
TIFFReadScanline(TIFF* tif, tdata_t buf, uint32 row, tsample_t sample)
{
	int e;

	if (!TIFFCheckRead(tif, 0))
		return (-1);
	if( (e = TIFFSeek(tif, row, sample)) != 0) {
		/*
		 * Decompress desired row into user buffer.
		 */
		e = (*tif->tif_decoderow)
		    (tif, (tidata_t) buf, tif->tif_scanlinesize, sample);
		tif->tif_row++;
		if (e)
			(*tif->tif_postdecode)(tif, (tidata_t) buf,
			    tif->tif_scanlinesize);
	}
	return (e > 0 ? 1 : -1);
}

/*
 * Read a strip of data and decompress the specified
 * amount into the user-supplied buffer.
 */
tsize_t
TIFFReadEncodedStrip(TIFF* tif, tstrip_t strip, tdata_t buf, tsize_t size)
{
	TIFFDirectory *td = &tif->tif_dir;
	uint32 nrows;
	tsize_t stripsize;

	if (!TIFFCheckRead(tif, 0))
		return (-1);
	if (strip >= td->td_nstrips) {
		TIFFError(tif->tif_name, "%ld: Strip out of range, max %ld",
		    (long) strip, (long) td->td_nstrips);
		return (-1);
	}
	/*
	 * Calculate the strip size according to the number of
	 * rows in the strip (check for truncated last strip).
	 */
	if (strip != td->td_nstrips-1 ||
	    (nrows = td->td_imagelength % td->td_rowsperstrip) == 0)
		nrows = td->td_rowsperstrip;
	stripsize = TIFFVStripSize(tif, nrows);
	if (size == (tsize_t) -1)
		size = stripsize;
	else if (size > stripsize)
		size = stripsize;
	if (TIFFFillStrip(tif, strip) && (*tif->tif_decodestrip)(tif,
	    (tidata_t) buf, size, (tsample_t)(strip / td->td_stripsperimage))) {
		(*tif->tif_postdecode)(tif, (tidata_t) buf, size);
		return (size);
	} else
		return ((tsize_t) -1);
}

static tsize_t
TIFFReadRawStrip1(TIFF* tif,
    tstrip_t strip, tdata_t buf, tsize_t size, const char* module)
{
	TIFFDirectory *td = &tif->tif_dir;

	if (!isMapped(tif)) {
		tsize_t cc;

		if (!SeekOK(tif, td->td_stripoffset[strip])) {
			TIFFError(module,
			    "%s: Seek error at scanline %lu, strip %lu",
			    tif->tif_name,
			    (u_long) tif->tif_row, (u_long) strip);
			return (-1);
		}
		cc = TIFFReadFile(tif, buf, size);
		if (cc != size) {
			TIFFError(module,
		"%s: Read error at scanline %lu; got %lu bytes, expected %lu",
			    tif->tif_name,
			    (u_long) tif->tif_row,
			    (u_long) cc,
			    (u_long) size);
			return (-1);
		}
	} else {
		if (((tsize_t) (td->td_stripoffset[strip] + size))
                     > tif->tif_size) {
			TIFFError(module,
    "%s: Read error at scanline %lu, strip %lu; got %lu bytes, expected %lu",
			    tif->tif_name,
			    (u_long) tif->tif_row,
			    (u_long) strip,
			    (u_long) tif->tif_size - td->td_stripoffset[strip],
			    (u_long) size);
			return (-1);
		}
		_TIFFmemcpy(buf, tif->tif_base + td->td_stripoffset[strip], size);
	}
	return (size);
}

/*
 * Read a strip of data from the file.
 */
tsize_t
TIFFReadRawStrip(TIFF* tif, tstrip_t strip, tdata_t buf, tsize_t size)
{
	static const char module[] = "TIFFReadRawStrip";
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t bytecount;

	if (!TIFFCheckRead(tif, 0))
		return ((tsize_t) -1);
	if (strip >= td->td_nstrips) {
		TIFFError(tif->tif_name, "%lu: Strip out of range, max %lu",
		    (u_long) strip, (u_long) td->td_nstrips);
		return ((tsize_t) -1);
	}
	bytecount = td->td_stripbytecount[strip];
	if (bytecount <= 0) {
		TIFFError(tif->tif_name,
		    "%lu: Invalid strip byte count, strip %lu",
		    (u_long) bytecount, (u_long) strip);
		return ((tsize_t) -1);
	}
	if (size != (tsize_t)-1 && size < bytecount)
		bytecount = size;
	return (TIFFReadRawStrip1(tif, strip, buf, bytecount, module));
}

/*
 * Read the specified strip and setup for decoding. 
 * The data buffer is expanded, as necessary, to
 * hold the strip's data.
 */
static int
TIFFFillStrip(TIFF* tif, tstrip_t strip)
{
	static const char module[] = "TIFFFillStrip";
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t bytecount;

	bytecount = td->td_stripbytecount[strip];
	if (bytecount <= 0) {
		TIFFError(tif->tif_name,
		    "%lu: Invalid strip byte count, strip %lu",
		    (u_long) bytecount, (u_long) strip);
		return (0);
	}
	if (isMapped(tif) &&
	    (isFillOrder(tif, td->td_fillorder) || (tif->tif_flags & TIFF_NOBITREV))) {
		/*
		 * The image is mapped into memory and we either don't
		 * need to flip bits or the compression routine is going
		 * to handle this operation itself.  In this case, avoid
		 * copying the raw data and instead just reference the
		 * data from the memory mapped file image.  This assumes
		 * that the decompression routines do not modify the
		 * contents of the raw data buffer (if they try to,
		 * the application will get a fault since the file is
		 * mapped read-only).
		 */
		if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata)
			_TIFFfree(tif->tif_rawdata);
		tif->tif_flags &= ~TIFF_MYBUFFER;
		if (((tsize_t) td->td_stripoffset[strip] + bytecount)
                                                        > tif->tif_size) {
			/*
			 * This error message might seem strange, but it's
			 * what would happen if a read were done instead.
			 */
			TIFFError(module,
		    "%s: Read error on strip %lu; got %lu bytes, expected %lu",
			    tif->tif_name,
			    (u_long) strip,
			    (u_long) tif->tif_size - td->td_stripoffset[strip],
			    (u_long) bytecount);
			tif->tif_curstrip = NOSTRIP;
			return (0);
		}
		tif->tif_rawdatasize = bytecount;
		tif->tif_rawdata = tif->tif_base + td->td_stripoffset[strip];
	} else {
		/*
		 * Expand raw data buffer, if needed, to
		 * hold data strip coming from file
		 * (perhaps should set upper bound on
		 *  the size of a buffer we'll use?).
		 */
		if (bytecount > tif->tif_rawdatasize) {
			tif->tif_curstrip = NOSTRIP;
			if ((tif->tif_flags & TIFF_MYBUFFER) == 0) {
				TIFFError(module,
				"%s: Data buffer too small to hold strip %lu",
				    tif->tif_name, (u_long) strip);
				return (0);
			}
			if (!TIFFReadBufferSetup(tif, 0,
			    TIFFroundup(bytecount, 1024)))
				return (0);
		}
		if (TIFFReadRawStrip1(tif, strip, (u_char *)tif->tif_rawdata,
		    bytecount, module) != bytecount)
			return (0);
		if (!isFillOrder(tif, td->td_fillorder) &&
		    (tif->tif_flags & TIFF_NOBITREV) == 0)
			TIFFReverseBits(tif->tif_rawdata, bytecount);
	}
	return (TIFFStartStrip(tif, strip));
}

/*
 * Tile-oriented Read Support
 * Contributed by Nancy Cam (Silicon Graphics).
 */

/*
 * Read and decompress a tile of data.  The
 * tile is selected by the (x,y,z,s) coordinates.
 */
tsize_t
TIFFReadTile(TIFF* tif,
    tdata_t buf, uint32 x, uint32 y, uint32 z, tsample_t s)
{
	if (!TIFFCheckRead(tif, 1) || !TIFFCheckTile(tif, x, y, z, s))
		return (-1);
	return (TIFFReadEncodedTile(tif,
	    TIFFComputeTile(tif, x, y, z, s), buf, (tsize_t) -1));
}

/*
 * Read a tile of data and decompress the specified
 * amount into the user-supplied buffer.
 */
tsize_t
TIFFReadEncodedTile(TIFF* tif, ttile_t tile, tdata_t buf, tsize_t size)
{
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t tilesize = tif->tif_tilesize;

	if (!TIFFCheckRead(tif, 1))
		return (-1);
	if (tile >= td->td_nstrips) {
		TIFFError(tif->tif_name, "%ld: Tile out of range, max %ld",
		    (long) tile, (u_long) td->td_nstrips);
		return (-1);
	}
	if (size == (tsize_t) -1)
		size = tilesize;
	else if (size > tilesize)
		size = tilesize;
	if (TIFFFillTile(tif, tile) && (*tif->tif_decodetile)(tif,
	    (tidata_t) buf, size, (tsample_t)(tile/td->td_stripsperimage))) {
		(*tif->tif_postdecode)(tif, (tidata_t) buf, size);
		return (size);
	} else
		return (-1);
}

static tsize_t
TIFFReadRawTile1(TIFF* tif,
    ttile_t tile, tdata_t buf, tsize_t size, const char* module)
{
	TIFFDirectory *td = &tif->tif_dir;

	if (!isMapped(tif)) {
		tsize_t cc;

		if (!SeekOK(tif, td->td_stripoffset[tile])) {
			TIFFError(module,
			    "%s: Seek error at row %ld, col %ld, tile %ld",
			    tif->tif_name,
			    (long) tif->tif_row,
			    (long) tif->tif_col,
			    (long) tile);
			return ((tsize_t) -1);
		}
		cc = TIFFReadFile(tif, buf, size);
		if (cc != size) {
			TIFFError(module,
	    "%s: Read error at row %ld, col %ld; got %lu bytes, expected %lu",
			    tif->tif_name,
			    (long) tif->tif_row,
			    (long) tif->tif_col,
			    (u_long) cc,
			    (u_long) size);
			return ((tsize_t) -1);
		}
	} else {
		if (((tsize_t) (td->td_stripoffset[tile] + size))
                                                         > tif->tif_size) {
			TIFFError(module,
    "%s: Read error at row %ld, col %ld, tile %ld; got %lu bytes, expected %lu",
			    tif->tif_name,
			    (long) tif->tif_row,
			    (long) tif->tif_col,
			    (long) tile,
			    (u_long) tif->tif_size - td->td_stripoffset[tile],
			    (u_long) size);
			return ((tsize_t) -1);
		}
		_TIFFmemcpy(buf, tif->tif_base + td->td_stripoffset[tile], size);
	}
	return (size);
}

/*
 * Read a tile of data from the file.
 */
tsize_t
TIFFReadRawTile(TIFF* tif, ttile_t tile, tdata_t buf, tsize_t size)
{
	static const char module[] = "TIFFReadRawTile";
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t bytecount;

	if (!TIFFCheckRead(tif, 1))
		return ((tsize_t) -1);
	if (tile >= td->td_nstrips) {
		TIFFError(tif->tif_name, "%lu: Tile out of range, max %lu",
		    (u_long) tile, (u_long) td->td_nstrips);
		return ((tsize_t) -1);
	}
	bytecount = td->td_stripbytecount[tile];
	if (size != (tsize_t) -1 && size < bytecount)
		bytecount = size;
	return (TIFFReadRawTile1(tif, tile, buf, bytecount, module));
}

/*
 * Read the specified tile and setup for decoding. 
 * The data buffer is expanded, as necessary, to
 * hold the tile's data.
 */
static int
TIFFFillTile(TIFF* tif, ttile_t tile)
{
	static const char module[] = "TIFFFillTile";
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t bytecount;

	bytecount = td->td_stripbytecount[tile];
	if (bytecount <= 0) {
		TIFFError(tif->tif_name,
		    "%lu: Invalid tile byte count, tile %lu",
		    (u_long) bytecount, (u_long) tile);
		return (0);
	}
	if (isMapped(tif) &&
	    (isFillOrder(tif, td->td_fillorder) || (tif->tif_flags & TIFF_NOBITREV))) {
		/*
		 * The image is mapped into memory and we either don't
		 * need to flip bits or the compression routine is going
		 * to handle this operation itself.  In this case, avoid
		 * copying the raw data and instead just reference the
		 * data from the memory mapped file image.  This assumes
		 * that the decompression routines do not modify the
		 * contents of the raw data buffer (if they try to,
		 * the application will get a fault since the file is
		 * mapped read-only).
		 */
		if ((tif->tif_flags & TIFF_MYBUFFER) && tif->tif_rawdata)
			_TIFFfree(tif->tif_rawdata);
		tif->tif_flags &= ~TIFF_MYBUFFER;
		if ( ((tsize_t) (td->td_stripoffset[tile] + bytecount))
                                                         > tif->tif_size) {
			tif->tif_curtile = NOTILE;
			return (0);
		}
		tif->tif_rawdatasize = bytecount;
		tif->tif_rawdata = tif->tif_base + td->td_stripoffset[tile];
	} else {
		/*
		 * Expand raw data buffer, if needed, to
		 * hold data tile coming from file
		 * (perhaps should set upper bound on
		 *  the size of a buffer we'll use?).
		 */
		if (bytecount > tif->tif_rawdatasize) {
			tif->tif_curtile = NOTILE;
			if ((tif->tif_flags & TIFF_MYBUFFER) == 0) {
				TIFFError(module,
				"%s: Data buffer too small to hold tile %ld",
				    tif->tif_name, (long) tile);
				return (0);
			}
			if (!TIFFReadBufferSetup(tif, 0,
			    TIFFroundup(bytecount, 1024)))
				return (0);
		}
		if (TIFFReadRawTile1(tif, tile, (u_char *)tif->tif_rawdata,
		    bytecount, module) != bytecount)
			return (0);
		if (!isFillOrder(tif, td->td_fillorder) &&
		    (tif->tif_flags & TIFF_NOBITREV) == 0)
			TIFFReverseBits(tif->tif_rawdata, bytecount);
	}
	return (TIFFStartTile(tif, tile));
}

/*
 * Setup the raw data buffer in preparation for
 * reading a strip of raw data.  If the buffer
 * is specified as zero, then a buffer of appropriate
 * size is allocated by the library.  Otherwise,
 * the client must guarantee that the buffer is
 * large enough to hold any individual strip of
 * raw data.
 */
int
TIFFReadBufferSetup(TIFF* tif, tdata_t bp, tsize_t size)
{
	static const char module[] = "TIFFReadBufferSetup";

	if (tif->tif_rawdata) {
		if (tif->tif_flags & TIFF_MYBUFFER)
			_TIFFfree(tif->tif_rawdata);
		tif->tif_rawdata = NULL;
	}
	if (bp) {
		tif->tif_rawdatasize = size;
		tif->tif_rawdata = (tidata_t) bp;
		tif->tif_flags &= ~TIFF_MYBUFFER;
	} else {
		tif->tif_rawdatasize = TIFFroundup(size, 1024);
		tif->tif_rawdata = (tidata_t) _TIFFmalloc(tif->tif_rawdatasize);
		tif->tif_flags |= TIFF_MYBUFFER;
	}
	if (tif->tif_rawdata == NULL) {
		TIFFError(module,
		    "%s: No space for data buffer at scanline %ld",
		    tif->tif_name, (long) tif->tif_row);
		tif->tif_rawdatasize = 0;
		return (0);
	}
	return (1);
}

/*
 * Set state to appear as if a
 * strip has just been read in.
 */
static int
TIFFStartStrip(TIFF* tif, tstrip_t strip)
{
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupdecode)(tif))
			return (0);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
	tif->tif_curstrip = strip;
	tif->tif_row = (strip % td->td_stripsperimage) * td->td_rowsperstrip;
	tif->tif_rawcp = tif->tif_rawdata;
	tif->tif_rawcc = td->td_stripbytecount[strip];
	return ((*tif->tif_predecode)(tif,
			(tsample_t)(strip / td->td_stripsperimage)));
}

/*
 * Set state to appear as if a
 * tile has just been read in.
 */
static int
TIFFStartTile(TIFF* tif, ttile_t tile)
{
	TIFFDirectory *td = &tif->tif_dir;

	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupdecode)(tif))
			return (0);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
	tif->tif_curtile = tile;
	tif->tif_row =
	    (tile % TIFFhowmany(td->td_imagewidth, td->td_tilewidth)) *
		td->td_tilelength;
	tif->tif_col =
	    (tile % TIFFhowmany(td->td_imagelength, td->td_tilelength)) *
		td->td_tilewidth;
	tif->tif_rawcp = tif->tif_rawdata;
	tif->tif_rawcc = td->td_stripbytecount[tile];
	return ((*tif->tif_predecode)(tif,
			(tsample_t)(tile/td->td_stripsperimage)));
}

static int
TIFFCheckRead(TIFF* tif, int tiles)
{
	if (tif->tif_mode == O_WRONLY) {
		TIFFError(tif->tif_name, "File not open for reading");
		return (0);
	}
	if (tiles ^ isTiled(tif)) {
		TIFFError(tif->tif_name, tiles ?
		    "Can not read tiles from a stripped image" :
		    "Can not read scanlines from a tiled image");
		return (0);
	}
	return (1);
}

void
_TIFFNoPostDecode(TIFF* tif, tidata_t buf, tsize_t cc)
{
    (void) tif; (void) buf; (void) cc;
}

void
_TIFFSwab16BitData(TIFF* tif, tidata_t buf, tsize_t cc)
{
    (void) tif;
    assert((cc & 1) == 0);
    TIFFSwabArrayOfShort((uint16*) buf, cc/2);
}

void
_TIFFSwab32BitData(TIFF* tif, tidata_t buf, tsize_t cc)
{
    (void) tif;
    assert((cc & 3) == 0);
    TIFFSwabArrayOfLong((uint32*) buf, cc/4);
}

void
_TIFFSwab64BitData(TIFF* tif, tidata_t buf, tsize_t cc)
{
    (void) tif;
    assert((cc & 7) == 0);
    TIFFSwabArrayOfDouble((double*) buf, cc/8);
}


/* ==== tif_strip.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_strip.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1991-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Strip-organized Image Support Routines.
 */
#include "tiffiop.h"

/*
 * Compute which strip a (row,sample) value is in.
 */
tstrip_t
TIFFComputeStrip(TIFF* tif, uint32 row, tsample_t sample)
{
	TIFFDirectory *td = &tif->tif_dir;
	tstrip_t strip;

	strip = row / td->td_rowsperstrip;
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
		if (sample >= td->td_samplesperpixel) {
			TIFFError(tif->tif_name,
			    "%u: Sample out of range, max %u",
			    sample, td->td_samplesperpixel);
			return ((tstrip_t) 0);
		}
		strip += sample*td->td_stripsperimage;
	}
	return (strip);
}

/*
 * Compute how many strips are in an image.
 */
tstrip_t
TIFFNumberOfStrips(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	tstrip_t nstrips;

	nstrips = (td->td_rowsperstrip == (uint32) -1 ?
	     (td->td_imagelength != 0 ? 1 : 0) :
	     TIFFhowmany(td->td_imagelength, td->td_rowsperstrip));
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
		nstrips *= td->td_samplesperpixel;
	return (nstrips);
}

/*
 * Compute the # bytes in a variable height, row-aligned strip.
 */
tsize_t
TIFFVStripSize(TIFF* tif, uint32 nrows)
{
	TIFFDirectory *td = &tif->tif_dir;

	if (nrows == (uint32) -1)
		nrows = td->td_imagelength;
#ifdef YCBCR_SUPPORT
	if (td->td_planarconfig == PLANARCONFIG_CONTIG &&
	    td->td_photometric == PHOTOMETRIC_YCBCR &&
	    !isUpSampled(tif)) {
		/*
		 * Packed YCbCr data contain one Cb+Cr for every
		 * HorizontalSampling*VerticalSampling Y values.
		 * Must also roundup width and height when calculating
		 * since images that are not a multiple of the
		 * horizontal/vertical subsampling area include
		 * YCbCr data for the extended image.
		 */
		tsize_t w =
		    TIFFroundup(td->td_imagewidth, td->td_ycbcrsubsampling[0]);
		tsize_t scanline = TIFFhowmany(w*td->td_bitspersample, 8);
		tsize_t samplingarea =
		    td->td_ycbcrsubsampling[0]*td->td_ycbcrsubsampling[1];
		nrows = TIFFroundup(nrows, td->td_ycbcrsubsampling[1]);
		/* NB: don't need TIFFhowmany here 'cuz everything is rounded */
		return ((tsize_t)
		    (nrows*scanline + 2*(nrows*scanline / samplingarea)));
	} else
#endif
		return ((tsize_t)(nrows * TIFFScanlineSize(tif)));
}

/*
 * Compute the # bytes in a (row-aligned) strip.
 *
 * Note that if RowsPerStrip is larger than the
 * recorded ImageLength, then the strip size is
 * truncated to reflect the actual space required
 * to hold the strip.
 */
tsize_t
TIFFStripSize(TIFF* tif)
{
	TIFFDirectory* td = &tif->tif_dir;
	uint32 rps = td->td_rowsperstrip;
	if (rps > td->td_imagelength)
		rps = td->td_imagelength;
	return (TIFFVStripSize(tif, rps));
}

/*
 * Compute a default strip size based on the image
 * characteristics and a requested value.  If the
 * request is <1 then we choose a strip size according
 * to certain heuristics.
 */
uint32
TIFFDefaultStripSize(TIFF* tif, uint32 request)
{
	return (*tif->tif_defstripsize)(tif, request);
}

uint32
_TIFFDefaultStripSize(TIFF* tif, uint32 s)
{
	if ((int32) s < 1) {
		/*
		 * If RowsPerStrip is unspecified, try to break the
		 * image up into strips that are approximately 8Kbytes.
		 */
		tsize_t scanline = TIFFScanlineSize(tif);
		s = (uint32)(8*1024) / (scanline == 0 ? 1 : scanline);
		if (s == 0)		/* very wide images */
			s = 1;
	}
	return (s);
}

/*
 * Return the number of bytes to read/write in a call to
 * one of the scanline-oriented i/o routines.  Note that
 * this number may be 1/samples-per-pixel if data is
 * stored as separate planes.
 */
tsize_t
TIFFScanlineSize(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t scanline;
	
	scanline = td->td_bitspersample * td->td_imagewidth;
	if (td->td_planarconfig == PLANARCONFIG_CONTIG)
		scanline *= td->td_samplesperpixel;
	return ((tsize_t) TIFFhowmany(scanline, 8));
}

/*
 * Return the number of bytes required to store a complete
 * decoded and packed raster scanline (as opposed to the
 * I/O size returned by TIFFScanlineSize which may be less
 * if data is store as separate planes).
 */
tsize_t
TIFFRasterScanlineSize(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t scanline;
	
	scanline = td->td_bitspersample * td->td_imagewidth;
	if (td->td_planarconfig == PLANARCONFIG_CONTIG) {
		scanline *= td->td_samplesperpixel;
		return ((tsize_t) TIFFhowmany(scanline, 8));
	} else
		return ((tsize_t)
		    TIFFhowmany(scanline, 8)*td->td_samplesperpixel);
}


/* ==== tif_swab.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_swab.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library Bit & Byte Swapping Support.
 *
 * XXX We assume short = 16-bits and long = 32-bits XXX
 */
#include "tiffiop.h"

#ifndef TIFFSwabShort
void
TIFFSwabShort(uint16* wp)
{
	register u_char* cp = (u_char*) wp;
	int t;

	t = cp[1]; cp[1] = cp[0]; cp[0] = t;
}
#endif

#ifndef TIFFSwabLong
void
TIFFSwabLong(uint32* lp)
{
	register u_char* cp = (u_char*) lp;
	int t;

	t = cp[3]; cp[3] = cp[0]; cp[0] = t;
	t = cp[2]; cp[2] = cp[1]; cp[1] = t;
}
#endif

#ifndef TIFFSwabArrayOfShort
void
TIFFSwabArrayOfShort(uint16* wp, register u_long n)
{
	register u_char* cp;
	register int t;

	/* XXX unroll loop some */
	while (n-- > 0) {
		cp = (u_char*) wp;
		t = cp[1]; cp[1] = cp[0]; cp[0] = t;
		wp++;
	}
}
#endif

#ifndef TIFFSwabArrayOfLong
void
TIFFSwabArrayOfLong(register uint32* lp, register u_long n)
{
	register unsigned char *cp;
	register int t;

	/* XXX unroll loop some */
	while (n-- > 0) {
		cp = (unsigned char *)lp;
		t = cp[3]; cp[3] = cp[0]; cp[0] = t;
		t = cp[2]; cp[2] = cp[1]; cp[1] = t;
		lp++;
	}
}
#endif

#ifndef TIFFSwabDouble
void
TIFFSwabDouble(double *dp)
{
        register uint32* lp = (uint32*) dp;
        uint32 t;

	TIFFSwabArrayOfLong(lp, 2);
	t = lp[0]; lp[0] = lp[1]; lp[1] = t;
}
#endif

#ifndef TIFFSwabArrayOfDouble
void
TIFFSwabArrayOfDouble(double* dp, register u_long n)
{
	register uint32* lp = (uint32*) dp;
        register uint32 t;

	TIFFSwabArrayOfLong(lp, n + n);
        while (n-- > 0) {
		t = lp[0]; lp[0] = lp[1]; lp[1] = t;
                lp += 2;
        }
}
#endif

/*
 * Bit reversal tables.  TIFFBitRevTable[<byte>] gives
 * the bit reversed value of <byte>.  Used in various
 * places in the library when the FillOrder requires
 * bit reversal of byte values (e.g. CCITT Fax 3
 * encoding/decoding).  TIFFNoBitRevTable is provided
 * for algorithms that want an equivalent table that
 * do not reverse bit values.
 */
static const unsigned char TIFFBitRevTable[256] = {
    0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
    0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
    0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
    0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
    0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
    0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
    0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
    0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
    0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
    0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
    0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
    0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
    0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
    0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
    0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
    0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
    0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
    0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
    0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
    0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
    0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
    0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
    0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
    0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
    0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
    0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
    0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
    0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
    0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
    0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
    0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
    0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff
};
static const unsigned char TIFFNoBitRevTable[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff, 
};

const unsigned char*
TIFFGetBitRevTable(int reversed)
{
	return (reversed ? TIFFBitRevTable : TIFFNoBitRevTable);
}

void
TIFFReverseBits(register u_char* cp, register u_long n)
{
	for (; n > 8; n -= 8) {
		cp[0] = TIFFBitRevTable[cp[0]];
		cp[1] = TIFFBitRevTable[cp[1]];
		cp[2] = TIFFBitRevTable[cp[2]];
		cp[3] = TIFFBitRevTable[cp[3]];
		cp[4] = TIFFBitRevTable[cp[4]];
		cp[5] = TIFFBitRevTable[cp[5]];
		cp[6] = TIFFBitRevTable[cp[6]];
		cp[7] = TIFFBitRevTable[cp[7]];
		cp += 8;
	}
	while (n-- > 0)
		*cp = TIFFBitRevTable[*cp], cp++;
}


/* ==== tif_thunder.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_thunder.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef THUNDER_SUPPORT
/*
 * TIFF Library.
 *
 * ThunderScan 4-bit Compression Algorithm Support
 */

/*
 * ThunderScan uses an encoding scheme designed for
 * 4-bit pixel values.  Data is encoded in bytes, with
 * each byte split into a 2-bit code word and a 6-bit
 * data value.  The encoding gives raw data, runs of
 * pixels, or pixel values encoded as a delta from the
 * previous pixel value.  For the latter, either 2-bit
 * or 3-bit delta values are used, with the deltas packed
 * into a single byte.
 */
#define	THUNDER_DATA		0x3f	/* mask for 6-bit data */
#define	THUNDER_CODE		0xc0	/* mask for 2-bit code word */
/* code values */
#define	THUNDER_RUN		0x00	/* run of pixels w/ encoded count */
#define	THUNDER_2BITDELTAS	0x40	/* 3 pixels w/ encoded 2-bit deltas */
#define	    DELTA2_SKIP		2	/* skip code for 2-bit deltas */
#define	THUNDER_3BITDELTAS	0x80	/* 2 pixels w/ encoded 3-bit deltas */
#define	    DELTA3_SKIP		4	/* skip code for 3-bit deltas */
#define	THUNDER_RAW		0xc0	/* raw data encoded */

static const int twobitdeltas[4] = { 0, 1, 0, -1 };
static const int threebitdeltas[8] = { 0, 1, 2, 3, 0, -3, -2, -1 };

#define	SETPIXEL(op, v) { \
	lastpixel = (v) & 0xf; \
	if (npixels++ & 1) \
	    *op++ |= lastpixel; \
	else \
	    op[0] = lastpixel << 4; \
}

static int
ThunderDecode(TIFF* tif, tidata_t op, tsize_t maxpixels)
{
	register u_char *bp;
	register tsize_t cc;
	u_int lastpixel;
	tsize_t npixels;

	bp = (u_char *)tif->tif_rawcp;
	cc = tif->tif_rawcc;
	lastpixel = 0;
	npixels = 0;
	while (cc > 0 && npixels < maxpixels) {
		int n, delta;

		n = *bp++, cc--;
		switch (n & THUNDER_CODE) {
		case THUNDER_RUN:		/* pixel run */
			/*
			 * Replicate the last pixel n times,
			 * where n is the lower-order 6 bits.
			 */
			if (npixels & 1) {
				op[0] |= lastpixel;
				lastpixel = *op++; npixels++; n--;
			} else
				lastpixel |= lastpixel << 4;
			npixels += n;
			for (; n > 0; n -= 2)
				*op++ = lastpixel;
			if (n == -1)
				*--op &= 0xf0;
			lastpixel &= 0xf;
			break;
		case THUNDER_2BITDELTAS:	/* 2-bit deltas */
			if ((delta = ((n >> 4) & 3)) != DELTA2_SKIP)
				SETPIXEL(op, lastpixel + twobitdeltas[delta]);
			if ((delta = ((n >> 2) & 3)) != DELTA2_SKIP)
				SETPIXEL(op, lastpixel + twobitdeltas[delta]);
			if ((delta = (n & 3)) != DELTA2_SKIP)
				SETPIXEL(op, lastpixel + twobitdeltas[delta]);
			break;
		case THUNDER_3BITDELTAS:	/* 3-bit deltas */
			if ((delta = ((n >> 3) & 7)) != DELTA3_SKIP)
				SETPIXEL(op, lastpixel + threebitdeltas[delta]);
			if ((delta = (n & 7)) != DELTA3_SKIP)
				SETPIXEL(op, lastpixel + threebitdeltas[delta]);
			break;
		case THUNDER_RAW:		/* raw data */
			SETPIXEL(op, n);
			break;
		}
	}
	tif->tif_rawcp = (tidata_t) bp;
	tif->tif_rawcc = cc;
	if (npixels != maxpixels) {
		TIFFError(tif->tif_name,
		    "ThunderDecode: %s data at scanline %ld (%lu != %lu)",
		    npixels < maxpixels ? "Not enough" : "Too much",
		    (long) tif->tif_row, (long) npixels, (long) maxpixels);
		return (0);
	}
	return (1);
}

static int
ThunderDecodeRow(TIFF* tif, tidata_t buf, tsize_t occ, tsample_t s)
{
	tidata_t row = buf;
	
	(void) s;
	while ((long)occ > 0) {
		if (!ThunderDecode(tif, row, tif->tif_dir.td_imagewidth))
			return (0);
		occ -= tif->tif_scanlinesize;
		row += tif->tif_scanlinesize;
	}
	return (1);
}

int
TIFFInitThunderScan(TIFF* tif, int scheme)
{
	(void) scheme;
	tif->tif_decoderow = ThunderDecodeRow;
	tif->tif_decodestrip = ThunderDecodeRow;
	return (1);
}
#endif /* THUNDER_SUPPORT */


/* ==== tif_tile.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_tile.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1991-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Tiled Image Support Routines.
 */
#include "tiffiop.h"

/*
 * Compute which tile an (x,y,z,s) value is in.
 */
ttile_t
TIFFComputeTile(TIFF* tif, uint32 x, uint32 y, uint32 z, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;
	uint32 dx = td->td_tilewidth;
	uint32 dy = td->td_tilelength;
	uint32 dz = td->td_tiledepth;
	ttile_t tile = 1;

	if (td->td_imagedepth == 1)
		z = 0;
	if (dx == (uint32) -1)
		dx = td->td_imagewidth;
	if (dy == (uint32) -1)
		dy = td->td_imagelength;
	if (dz == (uint32) -1)
		dz = td->td_imagedepth;
	if (dx != 0 && dy != 0 && dz != 0) {
		uint32 xpt = TIFFhowmany(td->td_imagewidth, dx); 
		uint32 ypt = TIFFhowmany(td->td_imagelength, dy); 
		uint32 zpt = TIFFhowmany(td->td_imagedepth, dz); 

		if (td->td_planarconfig == PLANARCONFIG_SEPARATE) 
			tile = (xpt*ypt*zpt)*s +
			     (xpt*ypt)*(z/dz) +
			     xpt*(y/dy) +
			     x/dx;
		else
			tile = (xpt*ypt)*(z/dz) + xpt*(y/dy) + x/dx + s;
	}
	return (tile);
}

/*
 * Check an (x,y,z,s) coordinate
 * against the image bounds.
 */
int
TIFFCheckTile(TIFF* tif, uint32 x, uint32 y, uint32 z, tsample_t s)
{
	TIFFDirectory *td = &tif->tif_dir;

	if (x >= td->td_imagewidth) {
		TIFFError(tif->tif_name, "Col %ld out of range, max %lu",
		    (long) x, (u_long) td->td_imagewidth);
		return (0);
	}
	if (y >= td->td_imagelength) {
		TIFFError(tif->tif_name, "Row %ld out of range, max %lu",
		    (long) y, (u_long) td->td_imagelength);
		return (0);
	}
	if (z >= td->td_imagedepth) {
		TIFFError(tif->tif_name, "Depth %ld out of range, max %lu",
		    (long) z, (u_long) td->td_imagedepth);
		return (0);
	}
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE &&
	    s >= td->td_samplesperpixel) {
		TIFFError(tif->tif_name, "Sample %d out of range, max %u",
		    (int) s, td->td_samplesperpixel);
		return (0);
	}
	return (1);
}

/*
 * Compute how many tiles are in an image.
 */
ttile_t
TIFFNumberOfTiles(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	uint32 dx = td->td_tilewidth;
	uint32 dy = td->td_tilelength;
	uint32 dz = td->td_tiledepth;
	ttile_t ntiles;

	if (dx == (uint32) -1)
		dx = td->td_imagewidth;
	if (dy == (uint32) -1)
		dy = td->td_imagelength;
	if (dz == (uint32) -1)
		dz = td->td_imagedepth;
	ntiles = (dx == 0 || dy == 0 || dz == 0) ? 0 :
	    (TIFFhowmany(td->td_imagewidth, dx) *
	     TIFFhowmany(td->td_imagelength, dy) *
	     TIFFhowmany(td->td_imagedepth, dz));
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
		ntiles *= td->td_samplesperpixel;
	return (ntiles);
}

/*
 * Compute the # bytes in each row of a tile.
 */
tsize_t
TIFFTileRowSize(TIFF* tif)
{
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t rowsize;
	
	if (td->td_tilelength == 0 || td->td_tilewidth == 0)
		return ((tsize_t) 0);
	rowsize = td->td_bitspersample * td->td_tilewidth;
	if (td->td_planarconfig == PLANARCONFIG_CONTIG)
		rowsize *= td->td_samplesperpixel;
	return ((tsize_t) TIFFhowmany(rowsize, 8));
}

/*
 * Compute the # bytes in a variable length, row-aligned tile.
 */
tsize_t
TIFFVTileSize(TIFF* tif, uint32 nrows)
{
	TIFFDirectory *td = &tif->tif_dir;
	tsize_t tilesize;

	if (td->td_tilelength == 0 || td->td_tilewidth == 0 ||
	    td->td_tiledepth == 0)
		return ((tsize_t) 0);
#ifdef YCBCR_SUPPORT
	if (td->td_planarconfig == PLANARCONFIG_CONTIG &&
	    td->td_photometric == PHOTOMETRIC_YCBCR &&
	    !isUpSampled(tif)) {
		/*
		 * Packed YCbCr data contain one Cb+Cr for every
		 * HorizontalSampling*VerticalSampling Y values.
		 * Must also roundup width and height when calculating
		 * since images that are not a multiple of the
		 * horizontal/vertical subsampling area include
		 * YCbCr data for the extended image.
		 */
		tsize_t w =
		    TIFFroundup(td->td_tilewidth, td->td_ycbcrsubsampling[0]);
		tsize_t rowsize = TIFFhowmany(w*td->td_bitspersample, 8);
		tsize_t samplingarea =
		    td->td_ycbcrsubsampling[0]*td->td_ycbcrsubsampling[1];
		nrows = TIFFroundup(nrows, td->td_ycbcrsubsampling[1]);
		/* NB: don't need TIFFhowmany here 'cuz everything is rounded */
		tilesize = nrows*rowsize + 2*(nrows*rowsize / samplingarea);
	} else
#endif
		tilesize = nrows * TIFFTileRowSize(tif);
	return ((tsize_t)(tilesize * td->td_tiledepth));
}

/*
 * Compute the # bytes in a row-aligned tile.
 */
tsize_t
TIFFTileSize(TIFF* tif)
{
	return (TIFFVTileSize(tif, tif->tif_dir.td_tilelength));
}

/*
 * Compute a default tile size based on the image
 * characteristics and a requested value.  If a
 * request is <1 then we choose a size according
 * to certain heuristics.
 */
void
TIFFDefaultTileSize(TIFF* tif, uint32* tw, uint32* th)
{
	(*tif->tif_deftilesize)(tif, tw, th);
}

void
_TIFFDefaultTileSize(TIFF* tif, uint32* tw, uint32* th)
{
	(void) tif;
	if (*(int32*) tw < 1)
		*tw = 256;
	if (*(int32*) th < 1)
		*th = 256;
	/* roundup to a multiple of 16 per the spec */
	if (*tw & 0xf)
		*tw = TIFFroundup(*tw, 16);
	if (*th & 0xf)
		*th = TIFFroundup(*th, 16);
}


/* ==== tif_unix.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_unix.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library UNIX-specific Routines.
 */
#include "tiffiop.h"
#include <sys/types.h>
#if !defined(__GNU_CIL__) && !defined(WINDOWS)
#include <unistd.h>
#endif
#include <stdlib.h>

static tsize_t
_tiffReadProc(thandle_t fd, tdata_t buf, tsize_t size)
{
	return ((tsize_t) read((int) fd, buf, (size_t) size));
}

static tsize_t
_tiffWriteProc(thandle_t fd, tdata_t buf, tsize_t size)
{
	return ((tsize_t) write((int) fd, buf, (size_t) size));
}

static toff_t
_tiffSeekProc(thandle_t fd, toff_t off, int whence)
{
	return ((toff_t) lseek((int) fd, (off_t) off, whence));
}

static int
_tiffCloseProc(thandle_t fd)
{
	return (close((int) fd));
}

#include <sys/stat.h>

static toff_t
_tiffSizeProc(thandle_t fd)
{
#ifdef _AM29K
	long fsize;
	return ((fsize = lseek((int) fd, 0, SEEK_END)) < 0 ? 0 : fsize);
#else
	struct stat sb;
	return (toff_t) (fstat((int) fd, &sb) < 0 ? 0 : sb.st_size);
#endif
}

#ifdef HAVE_MMAP
#include <sys/mman.h>

static int
_tiffMapProc(thandle_t fd, tdata_t* pbase, toff_t* psize)
{
	toff_t size = _tiffSizeProc(fd);
	if (size != (toff_t) -1) {
		*pbase = (tdata_t)
		    mmap(0, size, PROT_READ, MAP_SHARED, (int) fd, 0);
		if (*pbase != (tdata_t) -1) {
			*psize = size;
			return (1);
		}
	}
	return (0);
}

static void
_tiffUnmapProc(thandle_t fd, tdata_t base, toff_t size)
{
	(void) fd;
	(void) munmap(base, (off_t) size);
}
#else /* !HAVE_MMAP */
static int
_tiffMapProc(thandle_t fd, tdata_t* pbase, toff_t* psize)
{
	(void) fd; (void) pbase; (void) psize;
	return (0);
}

static void
_tiffUnmapProc(thandle_t fd, tdata_t base, toff_t size)
{
	(void) fd; (void) base; (void) size;
}
#endif /* !HAVE_MMAP */

/*
 * Open a TIFF file descriptor for read/writing.
 */
TIFF*
TIFFFdOpen(int fd, const char* name, const char* mode)
{
	TIFF* tif;

	tif = TIFFClientOpen(name, mode,
	    (thandle_t) fd,
	    _tiffReadProc, _tiffWriteProc,
	    _tiffSeekProc, _tiffCloseProc, _tiffSizeProc,
	    _tiffMapProc, _tiffUnmapProc);
	if (tif)
		tif->tif_fd = fd;
	return (tif);
}

/*
 * Open a TIFF file for read/writing.
 */
TIFF*
TIFFOpen(const char* name, const char* mode)
{
	static const char module[] = "TIFFOpen";
	int m, fd;

	m = _TIFFgetMode(mode, module);
	if (m == -1)
		return ((TIFF*)0);

/* for cygwin */        
#ifdef O_BINARY
        m |= O_BINARY;
#endif        
        
#ifdef _AM29K
	fd = open(name, m);
#else
	fd = open(name, m, 0666);
#endif
	if (fd < 0) {
		TIFFError(module, "%s: Cannot open", name);
		return ((TIFF *)0);
	}
	return (TIFFFdOpen(fd, name, mode));
}

void*
_TIFFmalloc(tsize_t s)
{
	return (malloc((size_t) s));
}

void
_TIFFfree(tdata_t p)
{
	free(p);
}

void*
_TIFFrealloc(tdata_t p, tsize_t s)
{
	return (realloc(p, (size_t) s));
}

void
_TIFFmemset(tdata_t p, int v, tsize_t c)
{
	memset(p, v, (size_t) c);
}

void
_TIFFmemcpy(tdata_t d, const tdata_t s, tsize_t c)
{
	memcpy(d, s, (size_t) c);
}

int
_TIFFmemcmp(const tdata_t p1, const tdata_t p2, tsize_t c)
{
	return (memcmp(p1, p2, (size_t) c));
}

static void
unixWarningHandler(const char* module, const char* fmt, va_list ap)
{
	if (module != NULL)
		fprintf(stderr, "%s: ", module);
	fprintf(stderr, "Warning, ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ".\n");
}
TIFFErrorHandler _TIFFwarningHandler = unixWarningHandler;

static void
unixErrorHandler(const char* module, const char* fmt, va_list ap)
{
	if (module != NULL)
		fprintf(stderr, "%s: ", module);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ".\n");
}
TIFFErrorHandler _TIFFerrorHandler = unixErrorHandler;


/* ==== tif_version.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_version.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */
/*
 * Copyright (c) 1992-1997 Sam Leffler
 * Copyright (c) 1992-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */
#include "tiffiop.h"
#include "version.h"

static const char TIFFVersion[] = VERSION;

const char*
TIFFGetVersion(void)
{
	return (TIFFVersion);
}


/* ==== tif_warning.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_warning.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 */
#include "tiffiop.h"

TIFFErrorHandler
TIFFSetWarningHandler(TIFFErrorHandler handler)
{
	TIFFErrorHandler prev = _TIFFwarningHandler;
	_TIFFwarningHandler = handler;
	return (prev);
}

void
TIFFWarning(const char* module, const char* fmt, ...)
{
	if (_TIFFwarningHandler) {
		va_list ap;
		va_start(ap, fmt);
		(*_TIFFwarningHandler)(module, fmt, ap);
		va_end(ap);
	}
}


/* ==== tif_write.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_write.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library.
 *
 * Scanline-oriented Write Support
 */
#include "tiffiop.h"
#include <assert.h>
#include <stdio.h>

#define	STRIPINCR	20		/* expansion factor on strip array */

#define	WRITECHECKSTRIPS(tif, module)				\
	(((tif)->tif_flags&TIFF_BEENWRITING) || TIFFWriteCheck((tif),0,module))
#define	WRITECHECKTILES(tif, module)				\
	(((tif)->tif_flags&TIFF_BEENWRITING) || TIFFWriteCheck((tif),1,module))
#define	BUFFERCHECK(tif)					\
	(((tif)->tif_flags & TIFF_BUFFERSETUP) ||		\
	    TIFFWriteBufferSetup((tif), NULL, (tsize_t) -1))

static	int TIFFWriteCheck(TIFF*, int, const char*);
static	int TIFFGrowStrips(TIFF*, int, const char*);
static	int TIFFAppendToStrip(TIFF*, tstrip_t, tidata_t, tsize_t);
static	int TIFFSetupStrips(TIFF*);

int
TIFFWriteScanline(TIFF* tif, tdata_t buf, uint32 row, tsample_t sample)
{
	static const char module[] = "TIFFWriteScanline";
	register TIFFDirectory *td;
	int status, imagegrew = 0;
	tstrip_t strip;

	if (!WRITECHECKSTRIPS(tif, module))
		return (-1);
	/*
	 * Handle delayed allocation of data buffer.  This
	 * permits it to be sized more intelligently (using
	 * directory information).
	 */
	if (!BUFFERCHECK(tif))
		return (-1);
	td = &tif->tif_dir;
	/*
	 * Extend image length if needed
	 * (but only for PlanarConfig=1).
	 */
	if (row >= td->td_imagelength) {	/* extend image */
		if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
			TIFFError(tif->tif_name,
		"Can not change \"ImageLength\" when using separate planes");
			return (-1);
		}
		td->td_imagelength = row+1;
		imagegrew = 1;
	}
	/*
	 * Calculate strip and check for crossings.
	 */
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
		if (sample >= td->td_samplesperpixel) {
			TIFFError(tif->tif_name,
			    "%d: Sample out of range, max %d",
			    sample, td->td_samplesperpixel);
			return (-1);
		}
		strip = sample*td->td_stripsperimage + row/td->td_rowsperstrip;
	} else
		strip = row / td->td_rowsperstrip;
	if (strip != tif->tif_curstrip) {
		/*
		 * Changing strips -- flush any data present.
		 */
		if (!TIFFFlushData(tif))
			return (-1);
		tif->tif_curstrip = strip;
		/*
		 * Watch out for a growing image.  The value of
		 * strips/image will initially be 1 (since it
		 * can't be deduced until the imagelength is known).
		 */
		if (strip >= td->td_stripsperimage && imagegrew)
			td->td_stripsperimage =
			    TIFFhowmany(td->td_imagelength,td->td_rowsperstrip);
		tif->tif_row =
		    (strip % td->td_stripsperimage) * td->td_rowsperstrip;
		if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
			if (!(*tif->tif_setupencode)(tif))
				return (-1);
			tif->tif_flags |= TIFF_CODERSETUP;
		}
		if (!(*tif->tif_preencode)(tif, sample))
			return (-1);
		tif->tif_flags |= TIFF_POSTENCODE;
	}
	/*
	 * Check strip array to make sure there's space.
	 * We don't support dynamically growing files that
	 * have data organized in separate bitplanes because
	 * it's too painful.  In that case we require that
	 * the imagelength be set properly before the first
	 * write (so that the strips array will be fully
	 * allocated above).
	 */
	if (strip >= td->td_nstrips && !TIFFGrowStrips(tif, 1, module))
		return (-1);
	/*
	 * Ensure the write is either sequential or at the
	 * beginning of a strip (or that we can randomly
	 * access the data -- i.e. no encoding).
	 */
	if (row != tif->tif_row) {
		if (row < tif->tif_row) {
			/*
			 * Moving backwards within the same strip:
			 * backup to the start and then decode
			 * forward (below).
			 */
			tif->tif_row = (strip % td->td_stripsperimage) *
			    td->td_rowsperstrip;
			tif->tif_rawcp = tif->tif_rawdata;
		}
		/*
		 * Seek forward to the desired row.
		 */
		if (!(*tif->tif_seek)(tif, row - tif->tif_row))
			return (-1);
		tif->tif_row = row;
	}
	status = (*tif->tif_encoderow)(tif, (tidata_t) buf,
	    tif->tif_scanlinesize, sample);
	tif->tif_row++;
	return (status);
}

/*
 * Encode the supplied data and write it to the
 * specified strip.  There must be space for the
 * data; we don't check if strips overlap!
 *
 * NB: Image length must be setup before writing.
 */
tsize_t
TIFFWriteEncodedStrip(TIFF* tif, tstrip_t strip, tdata_t data, tsize_t cc)
{
	static const char module[] = "TIFFWriteEncodedStrip";
	TIFFDirectory *td = &tif->tif_dir;
	tsample_t sample;

	if (!WRITECHECKSTRIPS(tif, module))
		return ((tsize_t) -1);
	/*
	 * Check strip array to make sure there's space.
	 * We don't support dynamically growing files that
	 * have data organized in separate bitplanes because
	 * it's too painful.  In that case we require that
	 * the imagelength be set properly before the first
	 * write (so that the strips array will be fully
	 * allocated above).
	 */
	if (strip >= td->td_nstrips) {
		if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
			TIFFError(tif->tif_name,
		"Can not grow image by strips when using separate planes");
			return ((tsize_t) -1);
		}
		if (!TIFFGrowStrips(tif, 1, module))
			return ((tsize_t) -1);
		td->td_stripsperimage =
		    TIFFhowmany(td->td_imagelength, td->td_rowsperstrip);
	}
	/*
	 * Handle delayed allocation of data buffer.  This
	 * permits it to be sized according to the directory
	 * info.
	 */
	if (!BUFFERCHECK(tif))
		return ((tsize_t) -1);
	tif->tif_curstrip = strip;
	tif->tif_row = (strip % td->td_stripsperimage) * td->td_rowsperstrip;
	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupencode)(tif))
			return ((tsize_t) -1);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
        
#ifdef REWRITE_HACK        
	tif->tif_rawcc = 0;
	tif->tif_rawcp = tif->tif_rawdata;

        if( td->td_stripbytecount[strip] > 0 )
        {
            /* if we are writing over existing tiles, zero length. */
            td->td_stripbytecount[strip] = 0;

            /* this forces TIFFAppendToStrip() to do a seek */
            tif->tif_curoff = 0;
        }
#endif
        
	tif->tif_flags &= ~TIFF_POSTENCODE;
	sample = (tsample_t)(strip / td->td_stripsperimage);
	if (!(*tif->tif_preencode)(tif, sample))
		return ((tsize_t) -1);
	if (!(*tif->tif_encodestrip)(tif, (tidata_t) data, cc, sample))
		return ((tsize_t) 0);
	if (!(*tif->tif_postencode)(tif))
		return ((tsize_t) -1);
	if (!isFillOrder(tif, td->td_fillorder) &&
	    (tif->tif_flags & TIFF_NOBITREV) == 0)
		TIFFReverseBits(tif->tif_rawdata, tif->tif_rawcc);
	if (tif->tif_rawcc > 0 &&
	    !TIFFAppendToStrip(tif, strip, tif->tif_rawdata, tif->tif_rawcc))
		return ((tsize_t) -1);
	tif->tif_rawcc = 0;
	tif->tif_rawcp = tif->tif_rawdata;
	return (cc);
}

/*
 * Write the supplied data to the specified strip.
 * There must be space for the data; we don't check
 * if strips overlap!
 *
 * NB: Image length must be setup before writing.
 */
tsize_t
TIFFWriteRawStrip(TIFF* tif, tstrip_t strip, tdata_t data, tsize_t cc)
{
	static const char module[] = "TIFFWriteRawStrip";
	TIFFDirectory *td = &tif->tif_dir;

	if (!WRITECHECKSTRIPS(tif, module))
		return ((tsize_t) -1);
	/*
	 * Check strip array to make sure there's space.
	 * We don't support dynamically growing files that
	 * have data organized in separate bitplanes because
	 * it's too painful.  In that case we require that
	 * the imagelength be set properly before the first
	 * write (so that the strips array will be fully
	 * allocated above).
	 */
	if (strip >= td->td_nstrips) {
		if (td->td_planarconfig == PLANARCONFIG_SEPARATE) {
			TIFFError(tif->tif_name,
		"Can not grow image by strips when using separate planes");
			return ((tsize_t) -1);
		}
		/*
		 * Watch out for a growing image.  The value of
		 * strips/image will initially be 1 (since it
		 * can't be deduced until the imagelength is known).
		 */
		if (strip >= td->td_stripsperimage)
			td->td_stripsperimage =
			    TIFFhowmany(td->td_imagelength,td->td_rowsperstrip);
		if (!TIFFGrowStrips(tif, 1, module))
			return ((tsize_t) -1);
	}
	tif->tif_curstrip = strip;
	tif->tif_row = (strip % td->td_stripsperimage) * td->td_rowsperstrip;
	return (TIFFAppendToStrip(tif, strip, (tidata_t) data, cc) ?
	    cc : (tsize_t) -1);
}

/*
 * Write and compress a tile of data.  The
 * tile is selected by the (x,y,z,s) coordinates.
 */
tsize_t
TIFFWriteTile(TIFF* tif,
    tdata_t buf, uint32 x, uint32 y, uint32 z, tsample_t s)
{
	if (!TIFFCheckTile(tif, x, y, z, s))
		return (-1);
	/*
	 * NB: A tile size of -1 is used instead of tif_tilesize knowing
	 *     that TIFFWriteEncodedTile will clamp this to the tile size.
	 *     This is done because the tile size may not be defined until
	 *     after the output buffer is setup in TIFFWriteBufferSetup.
	 */
	return (TIFFWriteEncodedTile(tif,
	    TIFFComputeTile(tif, x, y, z, s), buf, (tsize_t) -1));
}

/*
 * Encode the supplied data and write it to the
 * specified tile.  There must be space for the
 * data.  The function clamps individual writes
 * to a tile to the tile size, but does not (and
 * can not) check that multiple writes to the same
 * tile do not write more than tile size data.
 *
 * NB: Image length must be setup before writing; this
 *     interface does not support automatically growing
 *     the image on each write (as TIFFWriteScanline does).
 */
tsize_t
TIFFWriteEncodedTile(TIFF* tif, ttile_t tile, tdata_t data, tsize_t cc)
{
	static const char module[] = "TIFFWriteEncodedTile";
	TIFFDirectory *td;
	tsample_t sample;

	if (!WRITECHECKTILES(tif, module))
		return ((tsize_t) -1);
	td = &tif->tif_dir;
	if (tile >= td->td_nstrips) {
		TIFFError(module, "%s: Tile %lu out of range, max %lu",
		    tif->tif_name, (u_long) tile, (u_long) td->td_nstrips);
		return ((tsize_t) -1);
	}
	/*
	 * Handle delayed allocation of data buffer.  This
	 * permits it to be sized more intelligently (using
	 * directory information).
	 */
	if (!BUFFERCHECK(tif))
		return ((tsize_t) -1);
	tif->tif_curtile = tile;

#ifdef REWRITE_HACK        
	tif->tif_rawcc = 0;
	tif->tif_rawcp = tif->tif_rawdata;

        if( td->td_stripbytecount[tile] > 0 )
        {
            /* if we are writing over existing tiles, zero length. */
            td->td_stripbytecount[tile] = 0;

            /* this forces TIFFAppendToStrip() to do a seek */
            tif->tif_curoff = 0;
        }
#endif
        
	/* 
	 * Compute tiles per row & per column to compute
	 * current row and column
	 */
	tif->tif_row = (tile % TIFFhowmany(td->td_imagelength, td->td_tilelength))
		* td->td_tilelength;
	tif->tif_col = (tile % TIFFhowmany(td->td_imagewidth, td->td_tilewidth))
		* td->td_tilewidth;

	if ((tif->tif_flags & TIFF_CODERSETUP) == 0) {
		if (!(*tif->tif_setupencode)(tif))
			return ((tsize_t) -1);
		tif->tif_flags |= TIFF_CODERSETUP;
	}
	tif->tif_flags &= ~TIFF_POSTENCODE;
	sample = (tsample_t)(tile/td->td_stripsperimage);
	if (!(*tif->tif_preencode)(tif, sample))
		return ((tsize_t) -1);
	/*
	 * Clamp write amount to the tile size.  This is mostly
	 * done so that callers can pass in some large number
	 * (e.g. -1) and have the tile size used instead.
	 */
	if ((uint32) cc > tif->tif_tilesize)
		cc = tif->tif_tilesize;
	if (!(*tif->tif_encodetile)(tif, (tidata_t) data, cc, sample))
		return ((tsize_t) 0);
	if (!(*tif->tif_postencode)(tif))
		return ((tsize_t) -1);
	if (!isFillOrder(tif, td->td_fillorder) &&
	    (tif->tif_flags & TIFF_NOBITREV) == 0)
		TIFFReverseBits((u_char *)tif->tif_rawdata, tif->tif_rawcc);
	if (tif->tif_rawcc > 0 && !TIFFAppendToStrip(tif, tile,
	    tif->tif_rawdata, tif->tif_rawcc))
		return ((tsize_t) -1);
	tif->tif_rawcc = 0;
	tif->tif_rawcp = tif->tif_rawdata;
	return (cc);
}

/*
 * Write the supplied data to the specified strip.
 * There must be space for the data; we don't check
 * if strips overlap!
 *
 * NB: Image length must be setup before writing; this
 *     interface does not support automatically growing
 *     the image on each write (as TIFFWriteScanline does).
 */
tsize_t
TIFFWriteRawTile(TIFF* tif, ttile_t tile, tdata_t data, tsize_t cc)
{
	static const char module[] = "TIFFWriteRawTile";

	if (!WRITECHECKTILES(tif, module))
		return ((tsize_t) -1);
	if (tile >= tif->tif_dir.td_nstrips) {
		TIFFError(module, "%s: Tile %lu out of range, max %lu",
		    tif->tif_name, (u_long) tile,
		    (u_long) tif->tif_dir.td_nstrips);
		return ((tsize_t) -1);
	}
	return (TIFFAppendToStrip(tif, tile, (tidata_t) data, cc) ?
	    cc : (tsize_t) -1);
}

#define	isUnspecified(tif, f) \
    (TIFFFieldSet(tif,f) && (tif)->tif_dir.td_imagelength == 0)

static int
TIFFSetupStrips(TIFF* tif)
{
	TIFFDirectory* td = &tif->tif_dir;

	if (isTiled(tif))
		td->td_stripsperimage =
		    isUnspecified(tif, FIELD_TILEDIMENSIONS) ?
			td->td_samplesperpixel : TIFFNumberOfTiles(tif);
	else
		td->td_stripsperimage =
		    isUnspecified(tif, FIELD_ROWSPERSTRIP) ?
			td->td_samplesperpixel : TIFFNumberOfStrips(tif);
	td->td_nstrips = td->td_stripsperimage;
	if (td->td_planarconfig == PLANARCONFIG_SEPARATE)
		td->td_stripsperimage /= td->td_samplesperpixel;
	td->td_stripoffset = (uint32 *)
	    _TIFFmalloc(td->td_nstrips * sizeof (uint32));
	td->td_stripbytecount = (uint32 *)
	    _TIFFmalloc(td->td_nstrips * sizeof (uint32));
	if (td->td_stripoffset == NULL || td->td_stripbytecount == NULL)
		return (0);
	/*
	 * Place data at the end-of-file
	 * (by setting offsets to zero).
	 */
	_TIFFmemset(td->td_stripoffset, 0, td->td_nstrips*sizeof (uint32));
	_TIFFmemset(td->td_stripbytecount, 0, td->td_nstrips*sizeof (uint32));
	TIFFSetFieldBit(tif, FIELD_STRIPOFFSETS);
	TIFFSetFieldBit(tif, FIELD_STRIPBYTECOUNTS);
	return (1);
}
#undef isUnspecified

/*
 * Verify file is writable and that the directory
 * information is setup properly.  In doing the latter
 * we also "freeze" the state of the directory so
 * that important information is not changed.
 */
static int
TIFFWriteCheck(TIFF* tif, int tiles, const char* module)
{
	if (tif->tif_mode == O_RDONLY) {
		TIFFError(module, "%s: File not open for writing",
		    tif->tif_name);
		return (0);
	}
	if (tiles ^ isTiled(tif)) {
		TIFFError(tif->tif_name, tiles ?
		    "Can not write tiles to a stripped image" :
		    "Can not write scanlines to a tiled image");
		return (0);
	}
	/*
	 * On the first write verify all the required information
	 * has been setup and initialize any data structures that
	 * had to wait until directory information was set.
	 * Note that a lot of our work is assumed to remain valid
	 * because we disallow any of the important parameters
	 * from changing after we start writing (i.e. once
	 * TIFF_BEENWRITING is set, TIFFSetField will only allow
	 * the image's length to be changed).
	 */
	if (!TIFFFieldSet(tif, FIELD_IMAGEDIMENSIONS)) {
		TIFFError(module,
		    "%s: Must set \"ImageWidth\" before writing data",
		    tif->tif_name);
		return (0);
	}
	if (!TIFFFieldSet(tif, FIELD_PLANARCONFIG)) {
		TIFFError(module,
	    "%s: Must set \"PlanarConfiguration\" before writing data",
		    tif->tif_name);
		return (0);
	}
	if (tif->tif_dir.td_stripoffset == NULL && !TIFFSetupStrips(tif)) {
		tif->tif_dir.td_nstrips = 0;
		TIFFError(module, "%s: No space for %s arrays",
		    tif->tif_name, isTiled(tif) ? "tile" : "strip");
		return (0);
	}
	tif->tif_tilesize = TIFFTileSize(tif);
	tif->tif_scanlinesize = TIFFScanlineSize(tif);
	tif->tif_flags |= TIFF_BEENWRITING;
	return (1);
}

/*
 * Setup the raw data buffer used for encoding.
 */
int
TIFFWriteBufferSetup(TIFF* tif, tdata_t bp, tsize_t size)
{
	static const char module[] = "TIFFWriteBufferSetup";

	if (tif->tif_rawdata) {
		if (tif->tif_flags & TIFF_MYBUFFER) {
			_TIFFfree(tif->tif_rawdata);
			tif->tif_flags &= ~TIFF_MYBUFFER;
		}
		tif->tif_rawdata = NULL;
	}
	if (size == (tsize_t) -1) {
		size = (isTiled(tif) ?
		    tif->tif_tilesize : tif->tif_scanlinesize);
		/*
		 * Make raw data buffer at least 8K
		 */
		if (size < 8*1024)
			size = 8*1024;
		bp = NULL;			/* NB: force malloc */
	}
	if (bp == NULL) {
		bp = _TIFFmalloc(size);
		if (bp == NULL) {
			TIFFError(module, "%s: No space for output buffer",
			    tif->tif_name);
			return (0);
		}
		tif->tif_flags |= TIFF_MYBUFFER;
	} else
		tif->tif_flags &= ~TIFF_MYBUFFER;
	tif->tif_rawdata = (tidata_t) bp;
	tif->tif_rawdatasize = size;
	tif->tif_rawcc = 0;
	tif->tif_rawcp = tif->tif_rawdata;
	tif->tif_flags |= TIFF_BUFFERSETUP;
	return (1);
}

/*
 * Grow the strip data structures by delta strips.
 */
static int
TIFFGrowStrips(TIFF* tif, int delta, const char* module)
{
	TIFFDirectory *td = &tif->tif_dir;

	assert(td->td_planarconfig == PLANARCONFIG_CONTIG);
	td->td_stripoffset = (uint32*)_TIFFrealloc(td->td_stripoffset,
	    (td->td_nstrips + delta) * sizeof (uint32));
	td->td_stripbytecount = (uint32*)_TIFFrealloc(td->td_stripbytecount,
	    (td->td_nstrips + delta) * sizeof (uint32));
	if (td->td_stripoffset == NULL || td->td_stripbytecount == NULL) {
		td->td_nstrips = 0;
		TIFFError(module, "%s: No space to expand strip arrays",
		    tif->tif_name);
		return (0);
	}
	_TIFFmemset(td->td_stripoffset+td->td_nstrips, 0, delta*sizeof (uint32));
	_TIFFmemset(td->td_stripbytecount+td->td_nstrips, 0, delta*sizeof (uint32));
	td->td_nstrips += delta;
	return (1);
}

/*
 * Append the data to the specified strip.
 *
 * NB: We don't check that there's space in the
 *     file (i.e. that strips do not overlap).
 */
static int
TIFFAppendToStrip(TIFF* tif, tstrip_t strip, tidata_t data, tsize_t cc)
{
	TIFFDirectory *td = &tif->tif_dir;
	static const char module[] = "TIFFAppendToStrip";

	if (td->td_stripoffset[strip] == 0 || tif->tif_curoff == 0) {
		/*
		 * No current offset, set the current strip.
		 */
		if (td->td_stripoffset[strip] != 0) {
			if (!SeekOK(tif, td->td_stripoffset[strip])) {
				TIFFError(module,
				    "%s: Seek error at scanline %lu",
				    tif->tif_name, (u_long) tif->tif_row);
				return (0);
			}
		} else
			td->td_stripoffset[strip] =
			    TIFFSeekFile(tif, (toff_t) 0, SEEK_END);
		tif->tif_curoff = td->td_stripoffset[strip];
	}
	if (!WriteOK(tif, data, cc)) {
		TIFFError(module, "%s: Write error at scanline %lu",
		    tif->tif_name, (u_long) tif->tif_row);
		return (0);
	}
	tif->tif_curoff += cc;
	td->td_stripbytecount[strip] += cc;
	return (1);
}

/*
 * Internal version of TIFFFlushData that can be
 * called by ``encodestrip routines'' w/o concern
 * for infinite recursion.
 */
int
TIFFFlushData1(TIFF* tif)
{
	if (tif->tif_rawcc > 0) {
		if (!isFillOrder(tif, tif->tif_dir.td_fillorder) &&
		    (tif->tif_flags & TIFF_NOBITREV) == 0)
			TIFFReverseBits((u_char *)tif->tif_rawdata,
			    tif->tif_rawcc);
		if (!TIFFAppendToStrip(tif,
		    isTiled(tif) ? tif->tif_curtile : tif->tif_curstrip,
		    tif->tif_rawdata, tif->tif_rawcc))
			return (0);
		tif->tif_rawcc = 0;
		tif->tif_rawcp = tif->tif_rawdata;
	}
	return (1);
}

/*
 * Set the current write offset.  This should only be
 * used to set the offset to a known previous location
 * (very carefully), or to 0 so that the next write gets
 * appended to the end of the file.
 */
void
TIFFSetWriteOffset(TIFF* tif, toff_t off)
{
	tif->tif_curoff = off;
}


/* ==== tif_zip.c ==== */
/* $Header: /home/mguthaus/.cvsroot/mibench/consumer/tiff-v3.5.4/libtiff/tif_zip.c,v 1.1.1.1 2000/11/06 19:52:27 mguthaus Exp $ */

/*
 * Copyright (c) 1995-1997 Sam Leffler
 * Copyright (c) 1995-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tiffiop.h"
#ifdef ZIP_SUPPORT
/*
 * TIFF Library.
 *
 * ZIP (aka Deflate) Compression Support
 *
 * This file is simply an interface to the zlib library written by
 * Jean-loup Gailly and Mark Adler.  You must use version 1.0 or later
 * of the library: this code assumes the 1.0 API and also depends on
 * the ability to write the zlib header multiple times (one per strip)
 * which was not possible with versions prior to 0.95.  Note also that
 * older versions of this codec avoided this bug by supressing the header
 * entirely.  This means that files written with the old library cannot
 * be read; they should be converted to a different compression scheme
 * and then reconverted.
 *
 * The data format used by the zlib library is described in the files
 * zlib-3.1.doc, deflate-1.1.doc and gzip-4.1.doc, available in the
 * directory ftp://ftp.uu.net/pub/archiving/zip/doc.  The library was
 * last found at ftp://ftp.uu.net/pub/archiving/zip/zlib/zlib-0.99.tar.gz.
 */
#include "tif_predict.h"
#include "zlib.h"

#include <stdio.h>
#include <assert.h>

/*
 * Sigh, ZLIB_VERSION is defined as a string so there's no
 * way to do a proper check here.  Instead we guess based
 * on the presence of #defines that were added between the
 * 0.95 and 1.0 distributions.
 */
#if !defined(Z_NO_COMPRESSION) || !defined(Z_DEFLATED)
#error "Antiquated ZLIB software; you must use version 1.0 or later"
#endif

/*
 * State block for each open TIFF
 * file using ZIP compression/decompression.
 */
typedef	struct {
	TIFFPredictorState predict;
	z_stream	stream;
	int		zipquality;		/* compression level */
	int		state;			/* state flags */
#define	ZSTATE_INIT	0x1		/* zlib setup successfully */

	TIFFVGetMethod	vgetparent;		/* super-class method */
	TIFFVSetMethod	vsetparent;		/* super-class method */
} ZIPState;

#define	ZState(tif)		((ZIPState*) (tif)->tif_data)
#define	DecoderState(tif)	ZState(tif)
#define	EncoderState(tif)	ZState(tif)

static	int ZIPEncode(TIFF*, tidata_t, tsize_t, tsample_t);
static	int ZIPDecode(TIFF*, tidata_t, tsize_t, tsample_t);

static int
ZIPSetupDecode(TIFF* tif)
{
	ZIPState* sp = DecoderState(tif);
	static const char module[] = "ZIPSetupDecode";

	assert(sp != NULL);
	if (inflateInit(&sp->stream) != Z_OK) {
		TIFFError(module, "%s: %s", tif->tif_name, sp->stream.msg);
		return (0);
	} else {
		sp->state |= ZSTATE_INIT;
		return (1);
	}
}

/*
 * Setup state for decoding a strip.
 */
static int
ZIPPreDecode(TIFF* tif, tsample_t s)
{
	ZIPState* sp = DecoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->stream.next_in = tif->tif_rawdata;
	sp->stream.avail_in = tif->tif_rawcc;
	return (inflateReset(&sp->stream) == Z_OK);
}

static int
ZIPDecode(TIFF* tif, tidata_t op, tsize_t occ, tsample_t s)
{
	ZIPState* sp = DecoderState(tif);
	static const char module[] = "ZIPDecode";

	(void) s;
	assert(sp != NULL);
	sp->stream.next_out = op;
	sp->stream.avail_out = occ;
	do {
		int state = inflate(&sp->stream, Z_PARTIAL_FLUSH);
		if (state == Z_STREAM_END)
			break;
		if (state == Z_DATA_ERROR) {
			TIFFError(module,
			    "%s: Decoding error at scanline %d, %s",
			    tif->tif_name, tif->tif_row, sp->stream.msg);
			if (inflateSync(&sp->stream) != Z_OK)
				return (0);
			continue;
		}
		if (state != Z_OK) {
			TIFFError(module, "%s: zlib error: %s",
			    tif->tif_name, sp->stream.msg);
			return (0);
		}
	} while (sp->stream.avail_out > 0);
	if (sp->stream.avail_out != 0) {
		TIFFError(module,
		    "%s: Not enough data at scanline %d (short %d bytes)",
		    tif->tif_name, tif->tif_row, sp->stream.avail_out);
		return (0);
	}
	return (1);
}

static int
ZIPSetupEncode(TIFF* tif)
{
	ZIPState* sp = EncoderState(tif);
	static const char module[] = "ZIPSetupEncode";

	assert(sp != NULL);
	if (deflateInit(&sp->stream, sp->zipquality) != Z_OK) {
		TIFFError(module, "%s: %s", tif->tif_name, sp->stream.msg);
		return (0);
	} else {
		sp->state |= ZSTATE_INIT;
		return (1);
	}
}

/*
 * Reset encoding state at the start of a strip.
 */
static int
ZIPPreEncode(TIFF* tif, tsample_t s)
{
	ZIPState *sp = EncoderState(tif);

	(void) s;
	assert(sp != NULL);
	sp->stream.next_out = tif->tif_rawdata;
	sp->stream.avail_out = tif->tif_rawdatasize;
	return (deflateReset(&sp->stream) == Z_OK);
}

/*
 * Encode a chunk of pixels.
 */
static int
ZIPEncode(TIFF* tif, tidata_t bp, tsize_t cc, tsample_t s)
{
	ZIPState *sp = EncoderState(tif);
	static const char module[] = "ZIPEncode";

	(void) s;
	sp->stream.next_in = bp;
	sp->stream.avail_in = cc;
	do {
		if (deflate(&sp->stream, Z_NO_FLUSH) != Z_OK) {
			TIFFError(module, "%s: Encoder error: %s",
			    tif->tif_name, sp->stream.msg);
			return (0);
		}
		if (sp->stream.avail_out == 0) {
			tif->tif_rawcc = tif->tif_rawdatasize;
			TIFFFlushData1(tif);
			sp->stream.next_out = tif->tif_rawdata;
			sp->stream.avail_out = tif->tif_rawdatasize;
		}
	} while (sp->stream.avail_in > 0);
	return (1);
}

/*
 * Finish off an encoded strip by flushing the last
 * string and tacking on an End Of Information code.
 */
static int
ZIPPostEncode(TIFF* tif)
{
	ZIPState *sp = EncoderState(tif);
	static const char module[] = "ZIPPostEncode";
	int state;

	sp->stream.avail_in = 0;
	do {
		state = deflate(&sp->stream, Z_FINISH);
		switch (state) {
		case Z_STREAM_END:
		case Z_OK:
		    if (sp->stream.avail_out != tif->tif_rawdatasize) {
			    tif->tif_rawcc =
				tif->tif_rawdatasize - sp->stream.avail_out;
			    TIFFFlushData1(tif);
			    sp->stream.next_out = tif->tif_rawdata;
			    sp->stream.avail_out = tif->tif_rawdatasize;
		    }
		    break;
		default:
		    TIFFError(module, "%s: zlib error: %s",
			tif->tif_name, sp->stream.msg);
		    return (0);
		}
	} while (state != Z_STREAM_END);
	return (1);
}

static void
ZIPCleanup(TIFF* tif)
{
	ZIPState* sp = ZState(tif);
	if (sp) {
		if (sp->state&ZSTATE_INIT) {
			/* NB: avoid problems in the library */
			if (tif->tif_mode == O_RDONLY)
				inflateEnd(&sp->stream);
			else
				deflateEnd(&sp->stream);
		}
		_TIFFfree(sp);
		tif->tif_data = NULL;
	}
}

static int
ZIPVSetField(TIFF* tif, ttag_t tag, va_list ap)
{
	ZIPState* sp = ZState(tif);
	static const char module[] = "ZIPVSetField";

	switch (tag) {
	case TIFFTAG_ZIPQUALITY:
		sp->zipquality = va_arg(ap, int);
		if (tif->tif_mode != O_RDONLY && (sp->state&ZSTATE_INIT)) {
			if (deflateParams(&sp->stream,
			    sp->zipquality, Z_DEFAULT_STRATEGY) != Z_OK) {
				TIFFError(module, "%s: zlib error: %s",
				    tif->tif_name, sp->stream.msg);
				return (0);
			}
		}
		return (1);
	default:
		return (*sp->vsetparent)(tif, tag, ap);
	}
	/*NOTREACHED*/
}

static int
ZIPVGetField(TIFF* tif, ttag_t tag, va_list ap)
{
	ZIPState* sp = ZState(tif);

	switch (tag) {
	case TIFFTAG_ZIPQUALITY:
		*va_arg(ap, int*) = sp->zipquality;
		break;
	default:
		return (*sp->vgetparent)(tif, tag, ap);
	}
	return (1);
}

static const TIFFFieldInfo zipFieldInfo[] = {
    { TIFFTAG_ZIPQUALITY,	 0, 0,	TIFF_ANY,	FIELD_PSEUDO,
      TRUE,	FALSE,	"" },
};
#define	N(a)	(sizeof (a) / sizeof (a[0]))

int
TIFFInitZIP(TIFF* tif, int scheme)
{
	ZIPState* sp;

	assert( (scheme == COMPRESSION_DEFLATE) || (scheme == COMPRESSION_ADOBE_DEFLATE));

	/*
	 * Allocate state block so tag methods have storage to record values.
	 */
	tif->tif_data = (tidata_t) _TIFFmalloc(sizeof (ZIPState));
	if (tif->tif_data == NULL)
		goto bad;
	sp = ZState(tif);
	sp->stream.zalloc = NULL;
	sp->stream.zfree = NULL;
	sp->stream.opaque = NULL;
	sp->stream.data_type = Z_BINARY;

	/*
	 * Merge codec-specific tag information and
	 * override parent get/set field methods.
	 */
	_TIFFMergeFieldInfo(tif, zipFieldInfo, N(zipFieldInfo));
	sp->vgetparent = tif->tif_vgetfield;
	tif->tif_vgetfield = ZIPVGetField;	/* hook for codec tags */
	sp->vsetparent = tif->tif_vsetfield;
	tif->tif_vsetfield = ZIPVSetField;	/* hook for codec tags */

	/* Default values for codec-specific fields */
	sp->zipquality = Z_DEFAULT_COMPRESSION;	/* default comp. level */
	sp->state = 0;

	/*
	 * Install codec methods.
	 */
	tif->tif_setupdecode = ZIPSetupDecode;
	tif->tif_predecode = ZIPPreDecode;
	tif->tif_decoderow = ZIPDecode;
	tif->tif_decodestrip = ZIPDecode;
	tif->tif_decodetile = ZIPDecode;
	tif->tif_setupencode = ZIPSetupEncode;
	tif->tif_preencode = ZIPPreEncode;
	tif->tif_postencode = ZIPPostEncode;
	tif->tif_encoderow = ZIPEncode;
	tif->tif_encodestrip = ZIPEncode;
	tif->tif_encodetile = ZIPEncode;
	tif->tif_cleanup = ZIPCleanup;
	/*
	 * Setup predictor setup.
	 */
	(void) TIFFPredictorInit(tif);
	return (1);
bad:
	TIFFError("TIFFInitZIP", "No space for ZIP state block");
	return (0);
}
#endif /* ZIP_SUPORT */


/* ==== getopt.c ==== */
/* Getopt for GNU.
   NOTE: getopt is now part of the C library, so if you don't know what
   "Keep this file name-space clean" means, talk to drepper@gnu.org
   before changing it!

   Copyright (C) 1987, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 2000
   	Free Software Foundation, Inc.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

/* This tells Alpha OSF/1 not to define a getopt prototype in <stdio.h>.
   Ditto for AIX 3.2 and <stdlib.h>.  */
#ifndef _NO_PROTO
# define _NO_PROTO
#endif

#if !defined __STDC__ || !__STDC__
/* This is a separate conditional since some stdc systems
   reject `defined (const)'.  */
# ifndef const
#  define const
# endif
#endif

#include <stdio.h>

/* Comment out all this code if we are using the GNU C Library, and are not
   actually compiling the library itself.  This code is part of the GNU C
   Library, but also included in many other GNU distributions.  Compiling
   and linking in this code is a waste when using the GNU C library
   (especially if it is a shared library).  Rather than having every GNU
   program understand `configure --with-gnu-libc' and omit the object files,
   it is simpler to just do this in the source for each such file.  */

#define GETOPT_INTERFACE_VERSION 2
#if !defined _LIBC && defined __GLIBC__ && __GLIBC__ >= 2
# include <gnu-versions.h>
# if _GNU_GETOPT_INTERFACE_VERSION == GETOPT_INTERFACE_VERSION
#  define ELIDE_CODE
# endif
#endif

#ifndef ELIDE_CODE


/* This needs to come after some library #include
   to get __GNU_LIBRARY__ defined.  */
#ifdef	__GNU_LIBRARY__
/* Don't include stdlib.h for non-GNU C libraries because some of them
   contain conflicting prototypes for getopt.  */
# include <stdlib.h>
# include <unistd.h>
#endif	/* GNU C library.  */

#ifdef VMS
# include <unixlib.h>
# if HAVE_STRING_H - 0
#  include <string.h>
# endif
#endif

#ifndef _
/* This is for other GNU distributions with internationalized messages.
   When compiling libc, the _ macro is predefined.  */
# ifdef HAVE_LIBINTL_H
#  include <libintl.h>
#  define _(msgid)	gettext (msgid)
# else
#  define _(msgid)	(msgid)
# endif
#endif

/* This version of `getopt' appears to the caller like standard Unix `getopt'
   but it behaves differently for the user, since it allows the user
   to intersperse the options with the other arguments.

   As `getopt' works, it permutes the elements of ARGV so that,
   when it is done, all the options precede everything else.  Thus
   all application programs are extended to handle flexible argument order.

   Setting the environment variable POSIXLY_CORRECT disables permutation.
   Then the behavior is completely standard.

   GNU application programs can use a third alternative mode in which
   they can distinguish the relative order of options and other arguments.  */

#include "getopt.h"

/* For communication from `getopt' to the caller.
   When `getopt' finds an option that takes an argument,
   the argument value is returned here.
   Also, when `ordering' is RETURN_IN_ORDER,
   each non-option ARGV-element is returned here.  */

char *optarg;

/* Index in ARGV of the next element to be scanned.
   This is used for communication to and from the caller
   and for communication between successive calls to `getopt'.

   On entry to `getopt', zero means this is the first call; initialize.

   When `getopt' returns -1, this is the index of the first of the
   non-option elements that the caller should itself scan.

   Otherwise, `optind' communicates from one call to the next
   how much of ARGV has been scanned so far.  */

/* 1003.2 says this must be 1 before any call.  */
int optind = 1;

/* Formerly, initialization of getopt depended on optind==0, which
   causes problems with re-calling getopt as programs generally don't
   know that. */

int __getopt_initialized;

/* The next char to be scanned in the option-element
   in which the last option character we returned was found.
   This allows us to pick up the scan where we left off.

   If this is zero, or a null string, it means resume the scan
   by advancing to the next ARGV-element.  */

static char *nextchar;

/* Callers store zero here to inhibit the error message
   for unrecognized options.  */

int opterr = 1;

/* Set to an option character which was unrecognized.
   This must be initialized on some systems to avoid linking in the
   system's own getopt implementation.  */

int optopt = '?';

/* Describe how to deal with options that follow non-option ARGV-elements.

   If the caller did not specify anything,
   the default is REQUIRE_ORDER if the environment variable
   POSIXLY_CORRECT is defined, PERMUTE otherwise.

   REQUIRE_ORDER means don't recognize them as options;
   stop option processing when the first non-option is seen.
   This is what Unix does.
   This mode of operation is selected by either setting the environment
   variable POSIXLY_CORRECT, or using `+' as the first character
   of the list of option characters.

   PERMUTE is the default.  We permute the contents of ARGV as we scan,
   so that eventually all the non-options are at the end.  This allows options
   to be given in any order, even with programs that were not written to
   expect this.

   RETURN_IN_ORDER is an option available to programs that were written
   to expect options and other ARGV-elements in any order and that care about
   the ordering of the two.  We describe each non-option ARGV-element
   as if it were the argument of an option with character code 1.
   Using `-' as the first character of the list of option characters
   selects this mode of operation.

   The special argument `--' forces an end of option-scanning regardless
   of the value of `ordering'.  In the case of RETURN_IN_ORDER, only
   `--' can cause `getopt' to return -1 with `optind' != ARGC.  */

static enum
{
  REQUIRE_ORDER, PERMUTE, RETURN_IN_ORDER
} ordering;

/* Value of POSIXLY_CORRECT environment variable.  */
static char *posixly_correct;

#ifdef	__GNU_LIBRARY__
/* We want to avoid inclusion of string.h with non-GNU libraries
   because there are many ways it can cause trouble.
   On some systems, it contains special magic macros that don't work
   in GCC.  */
# include <string.h>
# define my_index	strchr
#else

#include <string.h>

/* Avoid depending on library functions or files
   whose names are inconsistent.  */

#ifndef getenv
extern char *getenv ();
#endif

static char *
my_index (str, chr)
     const char *str;
     int chr;
{
  while (*str)
    {
      if (*str == chr)
	return (char *) str;
      str++;
    }
  return 0;
}

/* If using GCC, we can safely declare strlen this way.
   If not using GCC, it is ok not to declare it.  */
#ifdef __GNUC__
/* Note that Motorola Delta 68k R3V7 comes with GCC but not stddef.h.
   That was relevant to code that was here before.  */
# if (!defined __STDC__ || !__STDC__) && !defined strlen
/* gcc with -traditional declares the built-in strlen to return int,
   and has done so at least since version 2.4.5. -- rms.  */
extern int strlen (const char *);
# endif /* not __STDC__ */
#endif /* __GNUC__ */

#endif /* not __GNU_LIBRARY__ */

/* Handle permutation of arguments.  */

/* Describe the part of ARGV that contains non-options that have
   been skipped.  `first_nonopt' is the index in ARGV of the first of them;
   `last_nonopt' is the index after the last of them.  */

static int first_nonopt;
static int last_nonopt;

#ifdef _LIBC
/* Bash 2.0 gives us an environment variable containing flags
   indicating ARGV elements that should not be considered arguments.  */

/* Defined in getopt_init.c  */
extern char *__getopt_nonoption_flags;

static int nonoption_flags_max_len;
static int nonoption_flags_len;

static int original_argc;
static char *const *original_argv;

/* Make sure the environment variable bash 2.0 puts in the environment
   is valid for the getopt call we must make sure that the ARGV passed
   to getopt is that one passed to the process.  */
static void
__attribute__ ((unused))
store_args_and_env (int argc, char *const *argv)
{
  /* XXX This is no good solution.  We should rather copy the args so
     that we can compare them later.  But we must not use malloc(3).  */
  original_argc = argc;
  original_argv = argv;
}
# ifdef text_set_element
text_set_element (__libc_subinit, store_args_and_env);
# endif /* text_set_element */

# define SWAP_FLAGS(ch1, ch2) \
  if (nonoption_flags_len > 0)						      \
    {									      \
      char __tmp = __getopt_nonoption_flags[ch1];			      \
      __getopt_nonoption_flags[ch1] = __getopt_nonoption_flags[ch2];	      \
      __getopt_nonoption_flags[ch2] = __tmp;				      \
    }
#else	/* !_LIBC */
# define SWAP_FLAGS(ch1, ch2)
#endif	/* _LIBC */

/* Exchange two adjacent subsequences of ARGV.
   One subsequence is elements [first_nonopt,last_nonopt)
   which contains all the non-options that have been skipped so far.
   The other is elements [last_nonopt,optind), which contains all
   the options processed since those non-options were skipped.

   `first_nonopt' and `last_nonopt' are relocated so that they describe
   the new indices of the non-options in ARGV after they are moved.  */

#if defined __STDC__ && __STDC__
static void exchange (char **);
#endif

static void
exchange (argv)
     char **argv;
{
  int bottom = first_nonopt;
  int middle = last_nonopt;
  int top = optind;
  char *tem;

  /* Exchange the shorter segment with the far end of the longer segment.
     That puts the shorter segment into the right place.
     It leaves the longer segment in the right place overall,
     but it consists of two parts that need to be swapped next.  */

#ifdef _LIBC
  /* First make sure the handling of the `__getopt_nonoption_flags'
     string can work normally.  Our top argument must be in the range
     of the string.  */
  if (nonoption_flags_len > 0 && top >= nonoption_flags_max_len)
    {
      /* We must extend the array.  The user plays games with us and
	 presents new arguments.  */
      char *new_str = malloc (top + 1);
      if (new_str == NULL)
	nonoption_flags_len = nonoption_flags_max_len = 0;
      else
	{
	  memset (__mempcpy (new_str, __getopt_nonoption_flags,
			     nonoption_flags_max_len),
		  '\0', top + 1 - nonoption_flags_max_len);
	  nonoption_flags_max_len = top + 1;
	  __getopt_nonoption_flags = new_str;
	}
    }
#endif

  while (top > middle && middle > bottom)
    {
      if (top - middle > middle - bottom)
	{
	  /* Bottom segment is the short one.  */
	  int len = middle - bottom;
	  register int i;

	  /* Swap it with the top part of the top segment.  */
	  for (i = 0; i < len; i++)
	    {
	      tem = argv[bottom + i];
	      argv[bottom + i] = argv[top - (middle - bottom) + i];
	      argv[top - (middle - bottom) + i] = tem;
	      SWAP_FLAGS (bottom + i, top - (middle - bottom) + i);
	    }
	  /* Exclude the moved bottom segment from further swapping.  */
	  top -= len;
	}
      else
	{
	  /* Top segment is the short one.  */
	  int len = top - middle;
	  register int i;

	  /* Swap it with the bottom part of the bottom segment.  */
	  for (i = 0; i < len; i++)
	    {
	      tem = argv[bottom + i];
	      argv[bottom + i] = argv[middle + i];
	      argv[middle + i] = tem;
	      SWAP_FLAGS (bottom + i, middle + i);
	    }
	  /* Exclude the moved top segment from further swapping.  */
	  bottom += len;
	}
    }

  /* Update records for the slots the non-options now occupy.  */

  first_nonopt += (optind - last_nonopt);
  last_nonopt = optind;
}

/* Initialize the internal data when the first call is made.  */

#if defined __STDC__ && __STDC__
static const char *_getopt_initialize (int, char *const *, const char *);
#endif
static const char *
_getopt_initialize (argc, argv, optstring)
     int argc;
     char *const *argv;
     const char *optstring;
{
  /* Start processing options with ARGV-element 1 (since ARGV-element 0
     is the program name); the sequence of previously skipped
     non-option ARGV-elements is empty.  */

  first_nonopt = last_nonopt = optind;

  nextchar = NULL;

  posixly_correct = getenv ("POSIXLY_CORRECT");

  /* Determine how to handle the ordering of options and nonoptions.  */

  if (optstring[0] == '-')
    {
      ordering = RETURN_IN_ORDER;
      ++optstring;
    }
  else if (optstring[0] == '+')
    {
      ordering = REQUIRE_ORDER;
      ++optstring;
    }
  else if (posixly_correct != NULL)
    ordering = REQUIRE_ORDER;
  else
    ordering = PERMUTE;

#ifdef _LIBC
  if (posixly_correct == NULL
      && argc == original_argc && argv == original_argv)
    {
      if (nonoption_flags_max_len == 0)
	{
	  if (__getopt_nonoption_flags == NULL
	      || __getopt_nonoption_flags[0] == '\0')
	    nonoption_flags_max_len = -1;
	  else
	    {
	      const char *orig_str = __getopt_nonoption_flags;
	      int len = nonoption_flags_max_len = strlen (orig_str);
	      if (nonoption_flags_max_len < argc)
		nonoption_flags_max_len = argc;
	      __getopt_nonoption_flags =
		(char *) malloc (nonoption_flags_max_len);
	      if (__getopt_nonoption_flags == NULL)
		nonoption_flags_max_len = -1;
	      else
		memset (__mempcpy (__getopt_nonoption_flags, orig_str, len),
			'\0', nonoption_flags_max_len - len);
	    }
	}
      nonoption_flags_len = nonoption_flags_max_len;
    }
  else
    nonoption_flags_len = 0;
#endif

  return optstring;
}

/* Scan elements of ARGV (whose length is ARGC) for option characters
   given in OPTSTRING.

   If an element of ARGV starts with '-', and is not exactly "-" or "--",
   then it is an option element.  The characters of this element
   (aside from the initial '-') are option characters.  If `getopt'
   is called repeatedly, it returns successively each of the option characters
   from each of the option elements.

   If `getopt' finds another option character, it returns that character,
   updating `optind' and `nextchar' so that the next call to `getopt' can
   resume the scan with the following option character or ARGV-element.

   If there are no more option characters, `getopt' returns -1.
   Then `optind' is the index in ARGV of the first ARGV-element
   that is not an option.  (The ARGV-elements have been permuted
   so that those that are not options now come last.)

   OPTSTRING is a string containing the legitimate option characters.
   If an option character is seen that is not listed in OPTSTRING,
   return '?' after printing an error message.  If you set `opterr' to
   zero, the error message is suppressed but we still return '?'.

   If a char in OPTSTRING is followed by a colon, that means it wants an arg,
   so the following text in the same ARGV-element, or the text of the following
   ARGV-element, is returned in `optarg'.  Two colons mean an option that
   wants an optional arg; if there is text in the current ARGV-element,
   it is returned in `optarg', otherwise `optarg' is set to zero.

   If OPTSTRING starts with `-' or `+', it requests different methods of
   handling the non-option ARGV-elements.
   See the comments about RETURN_IN_ORDER and REQUIRE_ORDER, above.

   Long-named options begin with `--' instead of `-'.
   Their names may be abbreviated as long as the abbreviation is unique
   or is an exact match for some defined option.  If they have an
   argument, it follows the option name in the same ARGV-element, separated
   from the option name by a `=', or else the in next ARGV-element.
   When `getopt' finds a long-named option, it returns 0 if that option's
   `flag' field is nonzero, the value of the option's `val' field
   if the `flag' field is zero.

   The elements of ARGV aren't really const, because we permute them.
   But we pretend they're const in the prototype to be compatible
   with other systems.

   LONGOPTS is a vector of `struct option' terminated by an
   element containing a name which is zero.

   LONGIND returns the index in LONGOPT of the long-named option found.
   It is only valid when a long-named option has been found by the most
   recent call.

   If LONG_ONLY is nonzero, '-' as well as '--' can introduce
   long-named options.  */

int
_getopt_internal (argc, argv, optstring, longopts, longind, long_only)
     int argc;
     char *const *argv;
     const char *optstring;
     const struct option *longopts;
     int *longind;
     int long_only;
{
  int print_errors = opterr;
  if (optstring[0] == ':')
    print_errors = 0;

  optarg = NULL;

  if (optind == 0 || !__getopt_initialized)
    {
      if (optind == 0)
	optind = 1;	/* Don't scan ARGV[0], the program name.  */
      optstring = _getopt_initialize (argc, argv, optstring);
      __getopt_initialized = 1;
    }

  /* Test whether ARGV[optind] points to a non-option argument.
     Either it does not have option syntax, or there is an environment flag
     from the shell indicating it is not an option.  The later information
     is only used when the used in the GNU libc.  */
#ifdef _LIBC
# define NONOPTION_P (argv[optind][0] != '-' || argv[optind][1] == '\0'	      \
		      || (optind < nonoption_flags_len			      \
			  && __getopt_nonoption_flags[optind] == '1'))
#else
# define NONOPTION_P (argv[optind][0] != '-' || argv[optind][1] == '\0')
#endif

  if (nextchar == NULL || *nextchar == '\0')
    {
      /* Advance to the next ARGV-element.  */

      /* Give FIRST_NONOPT & LAST_NONOPT rational values if OPTIND has been
	 moved back by the user (who may also have changed the arguments).  */
      if (last_nonopt > optind)
	last_nonopt = optind;
      if (first_nonopt > optind)
	first_nonopt = optind;

      if (ordering == PERMUTE)
	{
	  /* If we have just processed some options following some non-options,
	     exchange them so that the options come first.  */

	  if (first_nonopt != last_nonopt && last_nonopt != optind)
	    exchange ((char **) argv);
	  else if (last_nonopt != optind)
	    first_nonopt = optind;

	  /* Skip any additional non-options
	     and extend the range of non-options previously skipped.  */

	  while (optind < argc && NONOPTION_P)
	    optind++;
	  last_nonopt = optind;
	}

      /* The special ARGV-element `--' means premature end of options.
	 Skip it like a null option,
	 then exchange with previous non-options as if it were an option,
	 then skip everything else like a non-option.  */

      if (optind != argc && !strcmp (argv[optind], "--"))
	{
	  optind++;

	  if (first_nonopt != last_nonopt && last_nonopt != optind)
	    exchange ((char **) argv);
	  else if (first_nonopt == last_nonopt)
	    first_nonopt = optind;
	  last_nonopt = argc;

	  optind = argc;
	}

      /* If we have done all the ARGV-elements, stop the scan
	 and back over any non-options that we skipped and permuted.  */

      if (optind == argc)
	{
	  /* Set the next-arg-index to point at the non-options
	     that we previously skipped, so the caller will digest them.  */
	  if (first_nonopt != last_nonopt)
	    optind = first_nonopt;
	  return -1;
	}

      /* If we have come to a non-option and did not permute it,
	 either stop the scan or describe it to the caller and pass it by.  */

      if (NONOPTION_P)
	{
	  if (ordering == REQUIRE_ORDER)
	    return -1;
	  optarg = argv[optind++];
	  return 1;
	}

      /* We have found another option-ARGV-element.
	 Skip the initial punctuation.  */

      nextchar = (argv[optind] + 1
		  + (longopts != NULL && argv[optind][1] == '-'));
    }

  /* Decode the current option-ARGV-element.  */

  /* Check whether the ARGV-element is a long option.

     If long_only and the ARGV-element has the form "-f", where f is
     a valid short option, don't consider it an abbreviated form of
     a long option that starts with f.  Otherwise there would be no
     way to give the -f short option.

     On the other hand, if there's a long option "fubar" and
     the ARGV-element is "-fu", do consider that an abbreviation of
     the long option, just like "--fu", and not "-f" with arg "u".

     This distinction seems to be the most useful approach.  */

  if (longopts != NULL
      && (argv[optind][1] == '-'
	  || (long_only && (argv[optind][2] || !my_index (optstring, argv[optind][1])))))
    {
      char *nameend;
      const struct option *p;
      const struct option *pfound = NULL;
      int exact = 0;
      int ambig = 0;
      int indfound = -1;
      int option_index;

      for (nameend = nextchar; *nameend && *nameend != '='; nameend++)
	/* Do nothing.  */ ;

      /* Test all long options for either exact match
	 or abbreviated matches.  */
      for (p = longopts, option_index = 0; p->name; p++, option_index++)
	if (!strncmp (p->name, nextchar, nameend - nextchar))
	  {
	    if ((unsigned int) (nameend - nextchar)
		== (unsigned int) strlen (p->name))
	      {
		/* Exact match found.  */
		pfound = p;
		indfound = option_index;
		exact = 1;
		break;
	      }
	    else if (pfound == NULL)
	      {
		/* First nonexact match found.  */
		pfound = p;
		indfound = option_index;
	      }
	    else
	      /* Second or later nonexact match found.  */
	      ambig = 1;
	  }

      if (ambig && !exact)
	{
	  if (print_errors)
	    fprintf (stderr, _("%s: option `%s' is ambiguous\n"),
		     argv[0], argv[optind]);
	  nextchar += strlen (nextchar);
	  optind++;
	  optopt = 0;
	  return '?';
	}

      if (pfound != NULL)
	{
	  option_index = indfound;
	  optind++;
	  if (*nameend)
	    {
	      /* Don't test has_arg with >, because some C compilers don't
		 allow it to be used on enums.  */
	      if (pfound->has_arg)
		optarg = nameend + 1;
	      else
		{
		  if (print_errors)
		    {
		      if (argv[optind - 1][1] == '-')
			/* --option */
			fprintf (stderr,
				 _("%s: option `--%s' doesn't allow an argument\n"),
				 argv[0], pfound->name);
		      else
			/* +option or -option */
			fprintf (stderr,
				 _("%s: option `%c%s' doesn't allow an argument\n"),
				 argv[0], argv[optind - 1][0], pfound->name);
		    }

		  nextchar += strlen (nextchar);

		  optopt = pfound->val;
		  return '?';
		}
	    }
	  else if (pfound->has_arg == 1)
	    {
	      if (optind < argc)
		optarg = argv[optind++];
	      else
		{
		  if (print_errors)
		    fprintf (stderr,
			   _("%s: option `%s' requires an argument\n"),
			   argv[0], argv[optind - 1]);
		  nextchar += strlen (nextchar);
		  optopt = pfound->val;
		  return optstring[0] == ':' ? ':' : '?';
		}
	    }
	  nextchar += strlen (nextchar);
	  if (longind != NULL)
	    *longind = option_index;
	  if (pfound->flag)
	    {
	      *(pfound->flag) = pfound->val;
	      return 0;
	    }
	  return pfound->val;
	}

      /* Can't find it as a long option.  If this is not getopt_long_only,
	 or the option starts with '--' or is not a valid short
	 option, then it's an error.
	 Otherwise interpret it as a short option.  */
      if (!long_only || argv[optind][1] == '-'
	  || my_index (optstring, *nextchar) == NULL)
	{
	  if (print_errors)
	    {
	      if (argv[optind][1] == '-')
		/* --option */
		fprintf (stderr, _("%s: unrecognized option `--%s'\n"),
			 argv[0], nextchar);
	      else
		/* +option or -option */
		fprintf (stderr, _("%s: unrecognized option `%c%s'\n"),
			 argv[0], argv[optind][0], nextchar);
	    }
	  nextchar = (char *) "";
	  optind++;
	  optopt = 0;
	  return '?';
	}
    }

  /* Look at and handle the next short option-character.  */

  {
    char c = *nextchar++;
    char *temp = my_index (optstring, c);

    /* Increment `optind' when we start to process its last character.  */
    if (*nextchar == '\0')
      ++optind;

    if (temp == NULL || c == ':')
      {
	if (print_errors)
	  {
	    if (posixly_correct)
	      /* 1003.2 specifies the format of this message.  */
	      fprintf (stderr, _("%s: illegal option -- %c\n"),
		       argv[0], c);
	    else
	      fprintf (stderr, _("%s: invalid option -- %c\n"),
		       argv[0], c);
	  }
	optopt = c;
	return '?';
      }
    /* Convenience. Treat POSIX -W foo same as long option --foo */
    if (temp[0] == 'W' && temp[1] == ';')
      {
	char *nameend;
	const struct option *p;
	const struct option *pfound = NULL;
	int exact = 0;
	int ambig = 0;
	int indfound = 0;
	int option_index;

	/* This is an option that requires an argument.  */
	if (*nextchar != '\0')
	  {
	    optarg = nextchar;
	    /* If we end this ARGV-element by taking the rest as an arg,
	       we must advance to the next element now.  */
	    optind++;
	  }
	else if (optind == argc)
	  {
	    if (print_errors)
	      {
		/* 1003.2 specifies the format of this message.  */
		fprintf (stderr, _("%s: option requires an argument -- %c\n"),
			 argv[0], c);
	      }
	    optopt = c;
	    if (optstring[0] == ':')
	      c = ':';
	    else
	      c = '?';
	    return c;
	  }
	else
	  /* We already incremented `optind' once;
	     increment it again when taking next ARGV-elt as argument.  */
	  optarg = argv[optind++];

	/* optarg is now the argument, see if it's in the
	   table of longopts.  */

	for (nextchar = nameend = optarg; *nameend && *nameend != '='; nameend++)
	  /* Do nothing.  */ ;

	/* Test all long options for either exact match
	   or abbreviated matches.  */
	for (p = longopts, option_index = 0; p->name; p++, option_index++)
	  if (!strncmp (p->name, nextchar, nameend - nextchar))
	    {
	      if ((unsigned int) (nameend - nextchar) == strlen (p->name))
		{
		  /* Exact match found.  */
		  pfound = p;
		  indfound = option_index;
		  exact = 1;
		  break;
		}
	      else if (pfound == NULL)
		{
		  /* First nonexact match found.  */
		  pfound = p;
		  indfound = option_index;
		}
	      else
		/* Second or later nonexact match found.  */
		ambig = 1;
	    }
	if (ambig && !exact)
	  {
	    if (print_errors)
	      fprintf (stderr, _("%s: option `-W %s' is ambiguous\n"),
		       argv[0], argv[optind]);
	    nextchar += strlen (nextchar);
	    optind++;
	    return '?';
	  }
	if (pfound != NULL)
	  {
	    option_index = indfound;
	    if (*nameend)
	      {
		/* Don't test has_arg with >, because some C compilers don't
		   allow it to be used on enums.  */
		if (pfound->has_arg)
		  optarg = nameend + 1;
		else
		  {
		    if (print_errors)
		      fprintf (stderr, _("\
%s: option `-W %s' doesn't allow an argument\n"),
			       argv[0], pfound->name);

		    nextchar += strlen (nextchar);
		    return '?';
		  }
	      }
	    else if (pfound->has_arg == 1)
	      {
		if (optind < argc)
		  optarg = argv[optind++];
		else
		  {
		    if (print_errors)
		      fprintf (stderr,
			       _("%s: option `%s' requires an argument\n"),
			       argv[0], argv[optind - 1]);
		    nextchar += strlen (nextchar);
		    return optstring[0] == ':' ? ':' : '?';
		  }
	      }
	    nextchar += strlen (nextchar);
	    if (longind != NULL)
	      *longind = option_index;
	    if (pfound->flag)
	      {
		*(pfound->flag) = pfound->val;
		return 0;
	      }
	    return pfound->val;
	  }
	  nextchar = NULL;
	  return 'W';	/* Let the application handle it.   */
      }
    if (temp[1] == ':')
      {
	if (temp[2] == ':')
	  {
	    /* This is an option that accepts an argument optionally.  */
	    if (*nextchar != '\0')
	      {
		optarg = nextchar;
		optind++;
	      }
	    else
	      optarg = NULL;
	    nextchar = NULL;
	  }
	else
	  {
	    /* This is an option that requires an argument.  */
	    if (*nextchar != '\0')
	      {
		optarg = nextchar;
		/* If we end this ARGV-element by taking the rest as an arg,
		   we must advance to the next element now.  */
		optind++;
	      }
	    else if (optind == argc)
	      {
		if (print_errors)
		  {
		    /* 1003.2 specifies the format of this message.  */
		    fprintf (stderr,
			     _("%s: option requires an argument -- %c\n"),
			     argv[0], c);
		  }
		optopt = c;
		if (optstring[0] == ':')
		  c = ':';
		else
		  c = '?';
	      }
	    else
	      /* We already incremented `optind' once;
		 increment it again when taking next ARGV-elt as argument.  */
	      optarg = argv[optind++];
	    nextchar = NULL;
	  }
      }
    return c;
  }
}

int
getopt (argc, argv, optstring)
     int argc;
     char *const *argv;
     const char *optstring;
{
  return _getopt_internal (argc, argv, optstring,
			   (const struct option *) 0,
			   (int *) 0,
			   0);
}

#endif	/* Not ELIDE_CODE.  */

#ifdef TEST

/* Compile with -DTEST to make an executable for use in testing
   the above definition of `getopt'.  */

int
main (argc, argv)
     int argc;
     char **argv;
{
  int c;
  int digit_optind = 0;

  while (1)
    {
      int this_option_optind = optind ? optind : 1;

      c = getopt (argc, argv, "abc:d:0123456789");
      if (c == -1)
	break;

      switch (c)
	{
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	  if (digit_optind != 0 && digit_optind != this_option_optind)
	    printf ("digits occur in two different argv-elements.\n");
	  digit_optind = this_option_optind;
	  printf ("option %c\n", c);
	  break;

	case 'a':
	  printf ("option a\n");
	  break;

	case 'b':
	  printf ("option b\n");
	  break;

	case 'c':
	  printf ("option c with value `%s'\n", optarg);
	  break;

	case '?':
	  break;

	default:
	  printf ("?? getopt returned character code 0%o ??\n", c);
	}
    }

  if (optind < argc)
    {
      printf ("non-option ARGV-elements: ");
      while (optind < argc)
	printf ("%s ", argv[optind++]);
      printf ("\n");
    }

  exit (0);
}

#endif /* TEST */
