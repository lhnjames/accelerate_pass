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


/* ==== amiga_mpega.c ==== */
/* MPGLIB replacement using mpega.library (AmigaOS)
 * Written by Thomas Wenzel and Sigbjørn (CISC) Skjæret.
 *
 * Big thanks to Stéphane Tavernard for mpega.library.
 *
 */

#ifdef AMIGA_MPEGA

#include "lame.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <dos.h>
#include <proto/mpega.h>

struct Library  *MPEGABase;
MPEGA_STREAM    *mstream=NULL;
MPEGA_CTRL      mctrl;


static int break_cleanup(void)
{
	/* Dummy break function to make atexit() work. :P */
	return 1;
}

static void exit_cleanup(void)
{
	if(mstream) {
		MPEGA_close(mstream);
		mstream = NULL;
	}
	if(MPEGABase) {
		CloseLibrary(MPEGABase);
		MPEGABase = NULL;
	}
}


int lame_decode_initfile(const char *fullname, int *stereo, int *samp, int *bitrate, unsigned long *nsamp)
{
	mctrl.bs_access = NULL;

	mctrl.layer_1_2.mono.quality    = 2;
	mctrl.layer_1_2.stereo.quality  = 2;
	mctrl.layer_1_2.mono.freq_div   = 1;
	mctrl.layer_1_2.stereo.freq_div = 1;
	mctrl.layer_1_2.mono.freq_max   = 48000;
	mctrl.layer_1_2.stereo.freq_max = 48000;
	mctrl.layer_3.mono.quality      = 2;
	mctrl.layer_3.stereo.quality    = 2;
	mctrl.layer_3.mono.freq_div     = 1;
	mctrl.layer_3.stereo.freq_div   = 1;
	mctrl.layer_3.mono.freq_max     = 48000;
	mctrl.layer_3.stereo.freq_max   = 48000;
	mctrl.layer_1_2.force_mono      = 0;
	mctrl.layer_3.force_mono        = 0;

	MPEGABase = OpenLibrary("mpega.library", 2);
	if(!MPEGABase) {
		fprintf(stderr, "Unable to open mpega.library v2\n");
		exit(1);
	}
	onbreak(break_cleanup);
	atexit(exit_cleanup);

	mstream=MPEGA_open(fullname, &mctrl);
	if(!mstream) { return (-1); }

	*stereo  = mstream->dec_channels;
	*samp    = mstream->dec_frequency;
	*bitrate = mstream->bitrate;
/*	*nsamp   = MAX_U_32_NUM; */
	*nsamp   = (FLOAT)mstream->ms_duration/1000 * mstream->dec_frequency;

	return 0;
}

int lame_decode_fromfile(FILE *fd, short pcm_l[],short pcm_r[])
{
	int outsize=0;
	WORD *b[MPEGA_MAX_CHANNELS];

	b[0]=pcm_l;
	b[1]=pcm_r;

	while (outsize == 0)
		outsize = MPEGA_decode_frame(mstream, b);

	if (outsize < 0) { return (-1); }
	else { return outsize; }
}

#endif /* AMIGA_MPEGA */


/* ==== brhist.c ==== */
#ifdef BRHIST
#include <string.h>
#include "brhist.h"
#include "util.h"
#include <termcap.h>


#define BRHIST_BARMAX 50
int disp_brhist = 1;
long brhist_count[15];
long brhist_temp[15];
int brhist_vbrmin;
int brhist_vbrmax;
long brhist_max;
char brhist_bps[15][5];
char brhist_backcur[200];
char brhist_bar[BRHIST_BARMAX+10];
char brhist_spc[BRHIST_BARMAX+1];

char stderr_buff[BUFSIZ];


void brhist_init(lame_global_flags *gfp,int br_min, int br_max)
{
  int i;
  char term_buff[1024];
  char *termname;
  char *tp;
  char tc[10];

  for(i = 0; i < 15; i++)
    {
      sprintf(brhist_bps[i], "%3d:", bitrate_table[gfp->version][i]);
      brhist_count[i] = 0;
      brhist_temp[i] = 0;
    }

  brhist_vbrmin = br_min;
  brhist_vbrmax = br_max;

  brhist_max = 0;

  memset(&brhist_bar[0], '*', BRHIST_BARMAX);
  brhist_bar[BRHIST_BARMAX] = '\0';
  memset(&brhist_spc[0], ' ', BRHIST_BARMAX);
  brhist_spc[BRHIST_BARMAX] = '\0';
  brhist_backcur[0] = '\0';

  if ((termname = getenv("TERM")) == NULL)
    {
      fprintf(stderr, "can't get TERM environment string.\n");
      disp_brhist = 0;
      return;
    }

  if (tgetent(term_buff, termname) != 1)
    {
      fprintf(stderr, "can't find termcap entry: %s\n", termname);
      disp_brhist = 0;
      return;
    }

  tc[0] = '\0';
  tp = &tc[0];
  tp=tgetstr("up", &tp);
  brhist_backcur[0] = '\0';
  for(i = br_min-1; i <= br_max; i++)
    strcat(brhist_backcur, tp);
  setbuf(stderr, stderr_buff);
}

void brhist_add_count(void)
{
  int i;

  for(i = brhist_vbrmin; i <= brhist_vbrmax; i++)
    {
      brhist_count[i] += brhist_temp[i];
      if (brhist_count[i] > brhist_max)
	brhist_max = brhist_count[i];
      brhist_temp[i] = 0;
    }
}

void brhist_disp(void)
{
  int i;
  long full;
  int barlen;

  full = (brhist_max < BRHIST_BARMAX) ? BRHIST_BARMAX : brhist_max;
  fputc('\n', stderr);
  for(i = brhist_vbrmin; i <= brhist_vbrmax; i++)
    {
      barlen = (brhist_count[i]*BRHIST_BARMAX+full-1) / full;
      fputs(brhist_bps[i], stderr);
      fputs(&brhist_bar[BRHIST_BARMAX - barlen], stderr);
      fputs(&brhist_spc[barlen], stderr);
      fputc('\n', stderr);
    }
  fputs(brhist_backcur, stderr);
  fflush(stderr);
}

void brhist_disp_total(lame_global_flags *gfp)
{
  int i;
  FLOAT ave;

  for(i = brhist_vbrmin; i <= brhist_vbrmax; i++)
    fputc('\n', stderr);

  ave=0;
  for(i = brhist_vbrmin; i <= brhist_vbrmax; i++)
    ave += bitrate_table[gfp->version][i]*
      (FLOAT)brhist_count[i] / gfp->totalframes;
  fprintf(stderr, "\naverage: %2.0f kbs\n",ave);
    
#if 0
  fprintf(stderr, "----- bitrate statistics -----\n");
  fprintf(stderr, " [kbps]      frames\n");
  for(i = brhist_vbrmin; i <= brhist_vbrmax; i++)
    {
      fprintf(stderr, "   %3d  %8ld (%.1f%%)\n",
	      bitrate_table[gfp->version][i],
	      brhist_count[i],
	      (FLOAT)brhist_count[i] / gfp->totalframes * 100.0);
    }
#endif
  fflush(stderr);
}

#endif /* BRHIST */




/* ==== common.c ==== */
#ifdef HAVEMPGLIB
#include <ctype.h>
#include <stdlib.h>

#include <sys/types.h>
#include <fcntl.h>

#include "mpg123.h"

struct parameter param = { 1 , 1 , 0 , 0 };

int tabsel_123[2][3][16] = {
   { {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,},
     {0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,},
     {0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,} },

   { {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,},
     {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,},
     {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,} }
};

long freqs[9] = { 44100, 48000, 32000,
                  22050, 24000, 16000 ,
                  11025 , 12000 , 8000 };

int bitindex;
unsigned char *wordpointer;
unsigned char *pcm_sample;
int pcm_point = 0;


#if 0
static void get_II_stuff(struct frame *fr)
{
  static int translate[3][2][16] = 
   { { { 0,2,2,2,2,2,2,0,0,0,1,1,1,1,1,0 } ,
       { 0,2,2,0,0,0,1,1,1,1,1,1,1,1,1,0 } } ,
     { { 0,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0 } ,
       { 0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0 } } ,
     { { 0,3,3,3,3,3,3,0,0,0,1,1,1,1,1,0 } ,
       { 0,3,3,0,0,0,1,1,1,1,1,1,1,1,1,0 } } };

  int table,sblim;
  static struct al_table *tables[5] = 
       { alloc_0, alloc_1, alloc_2, alloc_3 , alloc_4 };
  static int sblims[5] = { 27 , 30 , 8, 12 , 30 };

  if(fr->lsf)
    table = 4;
  else
    table = translate[fr->sampling_frequency][2-fr->stereo][fr->bitrate_index];
  sblim = sblims[table];

  fr->alloc = tables[table];
  fr->II_sblimit = sblim;
}
#endif

#define HDRCMPMASK 0xfffffd00

#if 0
int head_check(unsigned long head)
{
    if( (head & 0xffe00000) != 0xffe00000)
	return FALSE;
    if(!((head>>17)&3))
	return FALSE;
    if( ((head>>12)&0xf) == 0xf)
	return FALSE;
    if( ((head>>10)&0x3) == 0x3 )
	return FALSE;
    return TRUE;
}
#endif

/*
 * the code a header and write the information
 * into the frame structure
 */
int decode_header(struct frame *fr,unsigned long newhead)
{


    if( newhead & (1<<20) ) {
      fr->lsf = (newhead & (1<<19)) ? 0x0 : 0x1;
      fr->mpeg25 = 0;
    }
    else {
      fr->lsf = 1;
      fr->mpeg25 = 1;
    }

    
    fr->lay = 4-((newhead>>17)&3);
    if( ((newhead>>10)&0x3) == 0x3) {
      fprintf(stderr,"Stream error\n");
      exit(1);
    }
    if(fr->mpeg25) {
      fr->sampling_frequency = 6 + ((newhead>>10)&0x3);
    }
    else
      fr->sampling_frequency = ((newhead>>10)&0x3) + (fr->lsf*3);
    fr->error_protection = ((newhead>>16)&0x1)^0x1;

    if(fr->mpeg25) /* allow Bitrate change for 2.5 ... */
      fr->bitrate_index = ((newhead>>12)&0xf);

    fr->bitrate_index = ((newhead>>12)&0xf);
    fr->padding   = ((newhead>>9)&0x1);
    fr->extension = ((newhead>>8)&0x1);
    fr->mode      = ((newhead>>6)&0x3);
    fr->mode_ext  = ((newhead>>4)&0x3);
    fr->copyright = ((newhead>>3)&0x1);
    fr->original  = ((newhead>>2)&0x1);
    fr->emphasis  = newhead & 0x3;

    fr->stereo    = (fr->mode == MPG_MD_MONO) ? 1 : 2;

    if(!fr->bitrate_index)
    {
      fprintf(stderr,"Free format not supported.\n");
      return (0);
    }

    switch(fr->lay)
    {
      case 1:
#if 0
		fr->do_layer = do_layer1;
        fr->jsbound = (fr->mode == MPG_MD_JOINT_STEREO) ? 
                         (fr->mode_ext<<2)+4 : 32;
        fr->framesize  = (long) tabsel_123[fr->lsf][0][fr->bitrate_index] * 12000;
        fr->framesize /= freqs[fr->sampling_frequency];
        fr->framesize  = ((fr->framesize+fr->padding)<<2)-4;
#else
        fprintf(stderr,"layer=1 Not supported!\n");
#endif
        break;
      case 2:
#if 0
		fr->do_layer = do_layer2;
        get_II_stuff(fr);
        fr->jsbound = (fr->mode == MPG_MD_JOINT_STEREO) ?
                         (fr->mode_ext<<2)+4 : fr->II_sblimit;
        fr->framesize = (long) tabsel_123[fr->lsf][1][fr->bitrate_index] * 144000;
        fr->framesize /= freqs[fr->sampling_frequency];
        fr->framesize += fr->padding - 4;
#else
        fprintf(stderr,"layer=2 Not supported!\n");
#endif
        break;
      case 3:
#if 0
        fr->do_layer = do_layer3;
        if(fr->lsf)
          ssize = (fr->stereo == 1) ? 9 : 17;
        else
          ssize = (fr->stereo == 1) ? 17 : 32;
#endif

#if 0
        if(fr->error_protection)
          ssize += 2;
#endif
          fr->framesize  = (long) tabsel_123[fr->lsf][2][fr->bitrate_index] * 144000;
          fr->framesize /= freqs[fr->sampling_frequency]<<(fr->lsf);
          fr->framesize = fr->framesize + fr->padding - 4;
        break; 
      default:
        fprintf(stderr,"Sorry, unknown layer type.\n"); 
        return (0);
    }

    /*    print_header(fr); */

    return 1;
}


#if 1
void print_header(struct frame *fr)
{
	static char *modes[4] = { "Stereo", "Joint-Stereo", "Dual-Channel", "Single-Channel" };
	static char *layers[4] = { "Unknown" , "I", "II", "III" };

	fprintf(stderr,"MPEG %s, Layer: %s, Freq: %ld, mode: %s, modext: %d, BPF : %d\n", 
		fr->mpeg25 ? "2.5" : (fr->lsf ? "2.0" : "1.0"),
		layers[fr->lay],freqs[fr->sampling_frequency],
		modes[fr->mode],fr->mode_ext,fr->framesize+4);
	fprintf(stderr,"Channels: %d, copyright: %s, original: %s, CRC: %s, emphasis: %d.\n",
		fr->stereo,fr->copyright?"Yes":"No",
		fr->original?"Yes":"No",fr->error_protection?"Yes":"No",
		fr->emphasis);
	fprintf(stderr,"Bitrate: %d Kbits/s, Extension value: %d\n",
		tabsel_123[fr->lsf][fr->lay-1][fr->bitrate_index],fr->extension);
}

void print_header_compact(struct frame *fr)
{
	static char *modes[4] = { "stereo", "joint-stereo", "dual-channel", "mono" };
	static char *layers[4] = { "Unknown" , "I", "II", "III" };
 
	fprintf(stderr,"MPEG %s layer %s, %d kbit/s, %ld Hz %s\n",
		fr->mpeg25 ? "2.5" : (fr->lsf ? "2.0" : "1.0"),
		layers[fr->lay],
		tabsel_123[fr->lsf][fr->lay-1][fr->bitrate_index],
		freqs[fr->sampling_frequency], modes[fr->mode]);
}

#endif

unsigned int getbits(int number_of_bits)
{
  unsigned long rval;

  if(!number_of_bits)
    return 0;

  {
    rval = wordpointer[0];
    rval <<= 8;
    rval |= wordpointer[1];
    rval <<= 8;
    rval |= wordpointer[2];
    rval <<= bitindex;
    rval &= 0xffffff;

    bitindex += number_of_bits;

    rval >>= (24-number_of_bits);

    wordpointer += (bitindex>>3);
    bitindex &= 7;
  }
  return rval;
}

unsigned int getbits_fast(int number_of_bits)
{
  unsigned long rval;

  {
    rval = wordpointer[0];
    rval <<= 8;	
    rval |= wordpointer[1];
    rval <<= bitindex;
    rval &= 0xffff;
    bitindex += number_of_bits;

    rval >>= (16-number_of_bits);

    wordpointer += (bitindex>>3);
    bitindex &= 7;
  }
  return rval;
}





#endif


/* ==== dct64_i386.c ==== */
#ifdef HAVEMPGLIB
/*
 * Discrete Cosine Tansform (DCT) for subband synthesis
 * optimized for machines with no auto-increment. 
 * The performance is highly compiler dependend. Maybe
 * the dct64.c version for 'normal' processor may be faster
 * even for Intel processors.
 */

#include "mpg123.h"

static void dct64_1(real *out0,real *out1,real *b1,real *b2,real *samples)
{

 {
  register real *costab = pnts[0];

  b1[0x00] = samples[0x00] + samples[0x1F];
  b1[0x1F] = (samples[0x00] - samples[0x1F]) * costab[0x0];

  b1[0x01] = samples[0x01] + samples[0x1E];
  b1[0x1E] = (samples[0x01] - samples[0x1E]) * costab[0x1];

  b1[0x02] = samples[0x02] + samples[0x1D];
  b1[0x1D] = (samples[0x02] - samples[0x1D]) * costab[0x2];

  b1[0x03] = samples[0x03] + samples[0x1C];
  b1[0x1C] = (samples[0x03] - samples[0x1C]) * costab[0x3];

  b1[0x04] = samples[0x04] + samples[0x1B];
  b1[0x1B] = (samples[0x04] - samples[0x1B]) * costab[0x4];

  b1[0x05] = samples[0x05] + samples[0x1A];
  b1[0x1A] = (samples[0x05] - samples[0x1A]) * costab[0x5];

  b1[0x06] = samples[0x06] + samples[0x19];
  b1[0x19] = (samples[0x06] - samples[0x19]) * costab[0x6];

  b1[0x07] = samples[0x07] + samples[0x18];
  b1[0x18] = (samples[0x07] - samples[0x18]) * costab[0x7];

  b1[0x08] = samples[0x08] + samples[0x17];
  b1[0x17] = (samples[0x08] - samples[0x17]) * costab[0x8];

  b1[0x09] = samples[0x09] + samples[0x16];
  b1[0x16] = (samples[0x09] - samples[0x16]) * costab[0x9];

  b1[0x0A] = samples[0x0A] + samples[0x15];
  b1[0x15] = (samples[0x0A] - samples[0x15]) * costab[0xA];

  b1[0x0B] = samples[0x0B] + samples[0x14];
  b1[0x14] = (samples[0x0B] - samples[0x14]) * costab[0xB];

  b1[0x0C] = samples[0x0C] + samples[0x13];
  b1[0x13] = (samples[0x0C] - samples[0x13]) * costab[0xC];

  b1[0x0D] = samples[0x0D] + samples[0x12];
  b1[0x12] = (samples[0x0D] - samples[0x12]) * costab[0xD];

  b1[0x0E] = samples[0x0E] + samples[0x11];
  b1[0x11] = (samples[0x0E] - samples[0x11]) * costab[0xE];

  b1[0x0F] = samples[0x0F] + samples[0x10];
  b1[0x10] = (samples[0x0F] - samples[0x10]) * costab[0xF];
 }


 {
  register real *costab = pnts[1];

  b2[0x00] = b1[0x00] + b1[0x0F]; 
  b2[0x0F] = (b1[0x00] - b1[0x0F]) * costab[0];
  b2[0x01] = b1[0x01] + b1[0x0E]; 
  b2[0x0E] = (b1[0x01] - b1[0x0E]) * costab[1];
  b2[0x02] = b1[0x02] + b1[0x0D]; 
  b2[0x0D] = (b1[0x02] - b1[0x0D]) * costab[2];
  b2[0x03] = b1[0x03] + b1[0x0C]; 
  b2[0x0C] = (b1[0x03] - b1[0x0C]) * costab[3];
  b2[0x04] = b1[0x04] + b1[0x0B]; 
  b2[0x0B] = (b1[0x04] - b1[0x0B]) * costab[4];
  b2[0x05] = b1[0x05] + b1[0x0A]; 
  b2[0x0A] = (b1[0x05] - b1[0x0A]) * costab[5];
  b2[0x06] = b1[0x06] + b1[0x09]; 
  b2[0x09] = (b1[0x06] - b1[0x09]) * costab[6];
  b2[0x07] = b1[0x07] + b1[0x08]; 
  b2[0x08] = (b1[0x07] - b1[0x08]) * costab[7];

  b2[0x10] = b1[0x10] + b1[0x1F];
  b2[0x1F] = (b1[0x1F] - b1[0x10]) * costab[0];
  b2[0x11] = b1[0x11] + b1[0x1E];
  b2[0x1E] = (b1[0x1E] - b1[0x11]) * costab[1];
  b2[0x12] = b1[0x12] + b1[0x1D];
  b2[0x1D] = (b1[0x1D] - b1[0x12]) * costab[2];
  b2[0x13] = b1[0x13] + b1[0x1C];
  b2[0x1C] = (b1[0x1C] - b1[0x13]) * costab[3];
  b2[0x14] = b1[0x14] + b1[0x1B];
  b2[0x1B] = (b1[0x1B] - b1[0x14]) * costab[4];
  b2[0x15] = b1[0x15] + b1[0x1A];
  b2[0x1A] = (b1[0x1A] - b1[0x15]) * costab[5];
  b2[0x16] = b1[0x16] + b1[0x19];
  b2[0x19] = (b1[0x19] - b1[0x16]) * costab[6];
  b2[0x17] = b1[0x17] + b1[0x18];
  b2[0x18] = (b1[0x18] - b1[0x17]) * costab[7];
 }

 {
  register real *costab = pnts[2];

  b1[0x00] = b2[0x00] + b2[0x07];
  b1[0x07] = (b2[0x00] - b2[0x07]) * costab[0];
  b1[0x01] = b2[0x01] + b2[0x06];
  b1[0x06] = (b2[0x01] - b2[0x06]) * costab[1];
  b1[0x02] = b2[0x02] + b2[0x05];
  b1[0x05] = (b2[0x02] - b2[0x05]) * costab[2];
  b1[0x03] = b2[0x03] + b2[0x04];
  b1[0x04] = (b2[0x03] - b2[0x04]) * costab[3];

  b1[0x08] = b2[0x08] + b2[0x0F];
  b1[0x0F] = (b2[0x0F] - b2[0x08]) * costab[0];
  b1[0x09] = b2[0x09] + b2[0x0E];
  b1[0x0E] = (b2[0x0E] - b2[0x09]) * costab[1];
  b1[0x0A] = b2[0x0A] + b2[0x0D];
  b1[0x0D] = (b2[0x0D] - b2[0x0A]) * costab[2];
  b1[0x0B] = b2[0x0B] + b2[0x0C];
  b1[0x0C] = (b2[0x0C] - b2[0x0B]) * costab[3];

  b1[0x10] = b2[0x10] + b2[0x17];
  b1[0x17] = (b2[0x10] - b2[0x17]) * costab[0];
  b1[0x11] = b2[0x11] + b2[0x16];
  b1[0x16] = (b2[0x11] - b2[0x16]) * costab[1];
  b1[0x12] = b2[0x12] + b2[0x15];
  b1[0x15] = (b2[0x12] - b2[0x15]) * costab[2];
  b1[0x13] = b2[0x13] + b2[0x14];
  b1[0x14] = (b2[0x13] - b2[0x14]) * costab[3];

  b1[0x18] = b2[0x18] + b2[0x1F];
  b1[0x1F] = (b2[0x1F] - b2[0x18]) * costab[0];
  b1[0x19] = b2[0x19] + b2[0x1E];
  b1[0x1E] = (b2[0x1E] - b2[0x19]) * costab[1];
  b1[0x1A] = b2[0x1A] + b2[0x1D];
  b1[0x1D] = (b2[0x1D] - b2[0x1A]) * costab[2];
  b1[0x1B] = b2[0x1B] + b2[0x1C];
  b1[0x1C] = (b2[0x1C] - b2[0x1B]) * costab[3];
 }

 {
  register real const cos0 = pnts[3][0];
  register real const cos1 = pnts[3][1];

  b2[0x00] = b1[0x00] + b1[0x03];
  b2[0x03] = (b1[0x00] - b1[0x03]) * cos0;
  b2[0x01] = b1[0x01] + b1[0x02];
  b2[0x02] = (b1[0x01] - b1[0x02]) * cos1;

  b2[0x04] = b1[0x04] + b1[0x07];
  b2[0x07] = (b1[0x07] - b1[0x04]) * cos0;
  b2[0x05] = b1[0x05] + b1[0x06];
  b2[0x06] = (b1[0x06] - b1[0x05]) * cos1;

  b2[0x08] = b1[0x08] + b1[0x0B];
  b2[0x0B] = (b1[0x08] - b1[0x0B]) * cos0;
  b2[0x09] = b1[0x09] + b1[0x0A];
  b2[0x0A] = (b1[0x09] - b1[0x0A]) * cos1;
  
  b2[0x0C] = b1[0x0C] + b1[0x0F];
  b2[0x0F] = (b1[0x0F] - b1[0x0C]) * cos0;
  b2[0x0D] = b1[0x0D] + b1[0x0E];
  b2[0x0E] = (b1[0x0E] - b1[0x0D]) * cos1;

  b2[0x10] = b1[0x10] + b1[0x13];
  b2[0x13] = (b1[0x10] - b1[0x13]) * cos0;
  b2[0x11] = b1[0x11] + b1[0x12];
  b2[0x12] = (b1[0x11] - b1[0x12]) * cos1;

  b2[0x14] = b1[0x14] + b1[0x17];
  b2[0x17] = (b1[0x17] - b1[0x14]) * cos0;
  b2[0x15] = b1[0x15] + b1[0x16];
  b2[0x16] = (b1[0x16] - b1[0x15]) * cos1;

  b2[0x18] = b1[0x18] + b1[0x1B];
  b2[0x1B] = (b1[0x18] - b1[0x1B]) * cos0;
  b2[0x19] = b1[0x19] + b1[0x1A];
  b2[0x1A] = (b1[0x19] - b1[0x1A]) * cos1;

  b2[0x1C] = b1[0x1C] + b1[0x1F];
  b2[0x1F] = (b1[0x1F] - b1[0x1C]) * cos0;
  b2[0x1D] = b1[0x1D] + b1[0x1E];
  b2[0x1E] = (b1[0x1E] - b1[0x1D]) * cos1;
 }

 {
  register real const cos0 = pnts[4][0];

  b1[0x00] = b2[0x00] + b2[0x01];
  b1[0x01] = (b2[0x00] - b2[0x01]) * cos0;
  b1[0x02] = b2[0x02] + b2[0x03];
  b1[0x03] = (b2[0x03] - b2[0x02]) * cos0;
  b1[0x02] += b1[0x03];

  b1[0x04] = b2[0x04] + b2[0x05];
  b1[0x05] = (b2[0x04] - b2[0x05]) * cos0;
  b1[0x06] = b2[0x06] + b2[0x07];
  b1[0x07] = (b2[0x07] - b2[0x06]) * cos0;
  b1[0x06] += b1[0x07];
  b1[0x04] += b1[0x06];
  b1[0x06] += b1[0x05];
  b1[0x05] += b1[0x07];

  b1[0x08] = b2[0x08] + b2[0x09];
  b1[0x09] = (b2[0x08] - b2[0x09]) * cos0;
  b1[0x0A] = b2[0x0A] + b2[0x0B];
  b1[0x0B] = (b2[0x0B] - b2[0x0A]) * cos0;
  b1[0x0A] += b1[0x0B];

  b1[0x0C] = b2[0x0C] + b2[0x0D];
  b1[0x0D] = (b2[0x0C] - b2[0x0D]) * cos0;
  b1[0x0E] = b2[0x0E] + b2[0x0F];
  b1[0x0F] = (b2[0x0F] - b2[0x0E]) * cos0;
  b1[0x0E] += b1[0x0F];
  b1[0x0C] += b1[0x0E];
  b1[0x0E] += b1[0x0D];
  b1[0x0D] += b1[0x0F];

  b1[0x10] = b2[0x10] + b2[0x11];
  b1[0x11] = (b2[0x10] - b2[0x11]) * cos0;
  b1[0x12] = b2[0x12] + b2[0x13];
  b1[0x13] = (b2[0x13] - b2[0x12]) * cos0;
  b1[0x12] += b1[0x13];

  b1[0x14] = b2[0x14] + b2[0x15];
  b1[0x15] = (b2[0x14] - b2[0x15]) * cos0;
  b1[0x16] = b2[0x16] + b2[0x17];
  b1[0x17] = (b2[0x17] - b2[0x16]) * cos0;
  b1[0x16] += b1[0x17];
  b1[0x14] += b1[0x16];
  b1[0x16] += b1[0x15];
  b1[0x15] += b1[0x17];

  b1[0x18] = b2[0x18] + b2[0x19];
  b1[0x19] = (b2[0x18] - b2[0x19]) * cos0;
  b1[0x1A] = b2[0x1A] + b2[0x1B];
  b1[0x1B] = (b2[0x1B] - b2[0x1A]) * cos0;
  b1[0x1A] += b1[0x1B];

  b1[0x1C] = b2[0x1C] + b2[0x1D];
  b1[0x1D] = (b2[0x1C] - b2[0x1D]) * cos0;
  b1[0x1E] = b2[0x1E] + b2[0x1F];
  b1[0x1F] = (b2[0x1F] - b2[0x1E]) * cos0;
  b1[0x1E] += b1[0x1F];
  b1[0x1C] += b1[0x1E];
  b1[0x1E] += b1[0x1D];
  b1[0x1D] += b1[0x1F];
 }

 out0[0x10*16] = b1[0x00];
 out0[0x10*12] = b1[0x04];
 out0[0x10* 8] = b1[0x02];
 out0[0x10* 4] = b1[0x06];
 out0[0x10* 0] = b1[0x01];
 out1[0x10* 0] = b1[0x01];
 out1[0x10* 4] = b1[0x05];
 out1[0x10* 8] = b1[0x03];
 out1[0x10*12] = b1[0x07];

 b1[0x08] += b1[0x0C];
 out0[0x10*14] = b1[0x08];
 b1[0x0C] += b1[0x0a];
 out0[0x10*10] = b1[0x0C];
 b1[0x0A] += b1[0x0E];
 out0[0x10* 6] = b1[0x0A];
 b1[0x0E] += b1[0x09];
 out0[0x10* 2] = b1[0x0E];
 b1[0x09] += b1[0x0D];
 out1[0x10* 2] = b1[0x09];
 b1[0x0D] += b1[0x0B];
 out1[0x10* 6] = b1[0x0D];
 b1[0x0B] += b1[0x0F];
 out1[0x10*10] = b1[0x0B];
 out1[0x10*14] = b1[0x0F];

 b1[0x18] += b1[0x1C];
 out0[0x10*15] = b1[0x10] + b1[0x18];
 out0[0x10*13] = b1[0x18] + b1[0x14];
 b1[0x1C] += b1[0x1a];
 out0[0x10*11] = b1[0x14] + b1[0x1C];
 out0[0x10* 9] = b1[0x1C] + b1[0x12];
 b1[0x1A] += b1[0x1E];
 out0[0x10* 7] = b1[0x12] + b1[0x1A];
 out0[0x10* 5] = b1[0x1A] + b1[0x16];
 b1[0x1E] += b1[0x19];
 out0[0x10* 3] = b1[0x16] + b1[0x1E];
 out0[0x10* 1] = b1[0x1E] + b1[0x11];
 b1[0x19] += b1[0x1D];
 out1[0x10* 1] = b1[0x11] + b1[0x19];
 out1[0x10* 3] = b1[0x19] + b1[0x15];
 b1[0x1D] += b1[0x1B];
 out1[0x10* 5] = b1[0x15] + b1[0x1D];
 out1[0x10* 7] = b1[0x1D] + b1[0x13];
 b1[0x1B] += b1[0x1F];
 out1[0x10* 9] = b1[0x13] + b1[0x1B];
 out1[0x10*11] = b1[0x1B] + b1[0x17];
 out1[0x10*13] = b1[0x17] + b1[0x1F];
 out1[0x10*15] = b1[0x1F];
}

/*
 * the call via dct64 is a trick to force GCC to use
 * (new) registers for the b1,b2 pointer to the bufs[xx] field
 */
void dct64(real *a,real *b,real *c)
{
  real bufs[0x40];
  dct64_1(a,b,bufs,bufs+0x20,c);
}


#endif


/* ==== decode_i386.c ==== */
#ifdef HAVEMPGLIB
/* 
 * Mpeg Layer-1,2,3 audio decoder 
 * ------------------------------
 * copyright (c) 1995,1996,1997 by Michael Hipp, All rights reserved.
 * See also 'README'
 *
 * slighlty optimized for machines without autoincrement/decrement.
 * The performance is highly compiler dependend. Maybe
 * the decode.c version for 'normal' processor may be faster
 * even for Intel processors.
 */

#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "mpg123.h"
#include "mpglib.h"

extern struct mpstr *gmp;

 /* old WRITE_SAMPLE */
#define WRITE_SAMPLE(samples,sum,clip) \
  if( (sum) > 32767.0) { *(samples) = 0x7fff; (clip)++; } \
  else if( (sum) < -32768.0) { *(samples) = -0x8000; (clip)++; } \
  else { *(samples) = sum; }

int synth_1to1_mono(real *bandPtr,unsigned char *samples,int *pnt)
{
  short samples_tmp[64];
  short *tmp1 = samples_tmp;
  int i,ret;
  int pnt1 = 0;

  ret = synth_1to1(bandPtr,0,(unsigned char *) samples_tmp,&pnt1);
  samples += *pnt;

  for(i=0;i<32;i++) {
    *( (short *) samples) = *tmp1;
    samples += 2;
    tmp1 += 2;
  }
  *pnt += 64;

  return ret;
}


int synth_1to1(real *bandPtr,int channel,unsigned char *out,int *pnt)
{
  static const int step = 2;
  int bo;
  short *samples = (short *) (out + *pnt);

  real *b0,(*buf)[0x110];
  int clip = 0; 
  int bo1;

  bo = gmp->synth_bo;

  if(!channel) {
    bo--;
    bo &= 0xf;
    buf = gmp->synth_buffs[0];
  }
  else {
    samples++;
    buf = gmp->synth_buffs[1];
  }

  if(bo & 0x1) {
    b0 = buf[0];
    bo1 = bo;
    dct64(buf[1]+((bo+1)&0xf),buf[0]+bo,bandPtr);
  }
  else {
    b0 = buf[1];
    bo1 = bo+1;
    dct64(buf[0]+bo,buf[1]+bo+1,bandPtr);
  }

  gmp->synth_bo = bo;
  
  {
    register int j;
    real *window = decwin + 16 - bo1;

    for (j=16;j;j--,b0+=0x10,window+=0x20,samples+=step)
    {
      real sum;
      sum  = window[0x0] * b0[0x0];
      sum -= window[0x1] * b0[0x1];
      sum += window[0x2] * b0[0x2];
      sum -= window[0x3] * b0[0x3];
      sum += window[0x4] * b0[0x4];
      sum -= window[0x5] * b0[0x5];
      sum += window[0x6] * b0[0x6];
      sum -= window[0x7] * b0[0x7];
      sum += window[0x8] * b0[0x8];
      sum -= window[0x9] * b0[0x9];
      sum += window[0xA] * b0[0xA];
      sum -= window[0xB] * b0[0xB];
      sum += window[0xC] * b0[0xC];
      sum -= window[0xD] * b0[0xD];
      sum += window[0xE] * b0[0xE];
      sum -= window[0xF] * b0[0xF];

      WRITE_SAMPLE(samples,sum,clip);
    }

    {
      real sum;
      sum  = window[0x0] * b0[0x0];
      sum += window[0x2] * b0[0x2];
      sum += window[0x4] * b0[0x4];
      sum += window[0x6] * b0[0x6];
      sum += window[0x8] * b0[0x8];
      sum += window[0xA] * b0[0xA];
      sum += window[0xC] * b0[0xC];
      sum += window[0xE] * b0[0xE];
      WRITE_SAMPLE(samples,sum,clip);
      b0-=0x10,window-=0x20,samples+=step;
    }
    window += bo1<<1;

    for (j=15;j;j--,b0-=0x10,window-=0x20,samples+=step)
    {
      real sum;
      sum = -window[-0x1] * b0[0x0];
      sum -= window[-0x2] * b0[0x1];
      sum -= window[-0x3] * b0[0x2];
      sum -= window[-0x4] * b0[0x3];
      sum -= window[-0x5] * b0[0x4];
      sum -= window[-0x6] * b0[0x5];
      sum -= window[-0x7] * b0[0x6];
      sum -= window[-0x8] * b0[0x7];
      sum -= window[-0x9] * b0[0x8];
      sum -= window[-0xA] * b0[0x9];
      sum -= window[-0xB] * b0[0xA];
      sum -= window[-0xC] * b0[0xB];
      sum -= window[-0xD] * b0[0xC];
      sum -= window[-0xE] * b0[0xD];
      sum -= window[-0xF] * b0[0xE];
      sum -= window[-0x0] * b0[0xF];

      WRITE_SAMPLE(samples,sum,clip);
    }
  }
  *pnt += 128;

  return clip;
}


#endif


/* ==== fft.c ==== */
/*
** FFT and FHT routines
**  Copyright 1988, 1993; Ron Mayer
**  
**  fht(fz,n);
**      Does a hartley transform of "n" points in the array "fz".
**      
** NOTE: This routine uses at least 2 patented algorithms, and may be
**       under the restrictions of a bunch of different organizations.
**       Although I wrote it completely myself; it is kind of a derivative
**       of a routine I once authored and released under the GPL, so it
**       may fall under the free software foundation's restrictions;
**       it was worked on as a Stanford Univ project, so they claim
**       some rights to it; it was further optimized at work here, so
**       I think this company claims parts of it.  The patents are
**       held by R. Bracewell (the FHT algorithm) and O. Buneman (the
**       trig generator), both at Stanford Univ.
**       If it were up to me, I'd say go do whatever you want with it;
**       but it would be polite to give credit to the following people
**       if you use this anywhere:
**           Euler     - probable inventor of the fourier transform.
**           Gauss     - probable inventor of the FFT.
**           Hartley   - probable inventor of the hartley transform.
**           Buneman   - for a really cool trig generator
**           Mayer(me) - for authoring this particular version and
**                       including all the optimizations in one package.
**       Thanks,
**       Ron Mayer; mayer@acuson.com
** and added some optimization by
**           Mather    - idea of using lookup table
**           Takehiro  - some dirty hack for speed up
*/

#include <math.h>
#include "util.h"
#include "psymodel.h"
#include "lame.h"

#define TRI_SIZE (5-1) /* 1024 =  4**5 */
static FLOAT costab[TRI_SIZE*2];
static FLOAT window[BLKSIZE / 2], window_s[BLKSIZE_s / 2];

static INLINE void fht(FLOAT *fz, short n)
{
    short k4;
    FLOAT *fi, *fn, *gi;
    FLOAT *tri;

    fn = fz + n;
    tri = &costab[0];
    k4 = 4;
    do {
	FLOAT s1, c1;
	short i, k1, k2, k3, kx;
	kx  = k4 >> 1;
	k1  = k4;
	k2  = k4 << 1;
	k3  = k2 + k1;
	k4  = k2 << 1;
	fi  = fz;
	gi  = fi + kx;
	do {
	    FLOAT f0,f1,f2,f3;
	    f1      = fi[0]  - fi[k1];
	    f0      = fi[0]  + fi[k1];
	    f3      = fi[k2] - fi[k3];
	    f2      = fi[k2] + fi[k3];
	    fi[k2]  = f0     - f2;
	    fi[0 ]  = f0     + f2;
	    fi[k3]  = f1     - f3;
	    fi[k1]  = f1     + f3;
	    f1      = gi[0]  - gi[k1];
	    f0      = gi[0]  + gi[k1];
	    f3      = SQRT2  * gi[k3];
	    f2      = SQRT2  * gi[k2];
	    gi[k2]  = f0     - f2;
	    gi[0 ]  = f0     + f2;
	    gi[k3]  = f1     - f3;
	    gi[k1]  = f1     + f3;
	    gi     += k4;
	    fi     += k4;
	} while (fi<fn);
	c1 = tri[0];
	s1 = tri[1];
	for (i = 1; i < kx; i++) {
	    FLOAT c2,s2;
	    c2 = 1 - (2*s1)*s1;
	    s2 = (2*s1)*c1;
	    fi = fz + i;
	    gi = fz + k1 - i;
	    do {
		FLOAT a,b,g0,f0,f1,g1,f2,g2,f3,g3;
		b       = s2*fi[k1] - c2*gi[k1];
		a       = c2*fi[k1] + s2*gi[k1];
		f1      = fi[0 ]    - a;
		f0      = fi[0 ]    + a;
		g1      = gi[0 ]    - b;
		g0      = gi[0 ]    + b;
		b       = s2*fi[k3] - c2*gi[k3];
		a       = c2*fi[k3] + s2*gi[k3];
		f3      = fi[k2]    - a;
		f2      = fi[k2]    + a;
		g3      = gi[k2]    - b;
		g2      = gi[k2]    + b;
		b       = s1*f2     - c1*g3;
		a       = c1*f2     + s1*g3;
		fi[k2]  = f0        - a;
		fi[0 ]  = f0        + a;
		gi[k3]  = g1        - b;
		gi[k1]  = g1        + b;
		b       = c1*g2     - s1*f3;
		a       = s1*g2     + c1*f3;
		gi[k2]  = g0        - a;
		gi[0 ]  = g0        + a;
		fi[k3]  = f1        - b;
		fi[k1]  = f1        + b;
		gi     += k4;
		fi     += k4;
	    } while (fi<fn);
	    c2 = c1;
	    c1 = c2 * tri[0] - s1 * tri[1];
	    s1 = c2 * tri[1] + s1 * tri[0];
        }
	tri += 2;
    } while (k4<n);
}

static const short rv_tbl[] = {
    0x00,    0x80,    0x40,    0xc0,    0x20,    0xa0,    0x60,    0xe0,
    0x10,    0x90,    0x50,    0xd0,    0x30,    0xb0,    0x70,    0xf0,
    0x08,    0x88,    0x48,    0xc8,    0x28,    0xa8,    0x68,    0xe8,
    0x18,    0x98,    0x58,    0xd8,    0x38,    0xb8,    0x78,    0xf8,
    0x04,    0x84,    0x44,    0xc4,    0x24,    0xa4,    0x64,    0xe4,
    0x14,    0x94,    0x54,    0xd4,    0x34,    0xb4,    0x74,    0xf4,
    0x0c,    0x8c,    0x4c,    0xcc,    0x2c,    0xac,    0x6c,    0xec,
    0x1c,    0x9c,    0x5c,    0xdc,    0x3c,    0xbc,    0x7c,    0xfc,
    0x02,    0x82,    0x42,    0xc2,    0x22,    0xa2,    0x62,    0xe2,
    0x12,    0x92,    0x52,    0xd2,    0x32,    0xb2,    0x72,    0xf2,
    0x0a,    0x8a,    0x4a,    0xca,    0x2a,    0xaa,    0x6a,    0xea,
    0x1a,    0x9a,    0x5a,    0xda,    0x3a,    0xba,    0x7a,    0xfa,
    0x06,    0x86,    0x46,    0xc6,    0x26,    0xa6,    0x66,    0xe6,
    0x16,    0x96,    0x56,    0xd6,    0x36,    0xb6,    0x76,    0xf6,
    0x0e,    0x8e,    0x4e,    0xce,    0x2e,    0xae,    0x6e,    0xee,
    0x1e,    0x9e,    0x5e,    0xde,    0x3e,    0xbe,    0x7e,    0xfe
};




#define ch01(index)  (buffer[chn][index])
#define ch2(index)  (((FLOAT)(0.5*SQRT2))*(buffer[0][index] + buffer[1][index]))
#define ch3(index)  (((FLOAT)(0.5*SQRT2))*(buffer[0][index] - buffer[1][index]))

#define ml00(f)	(window[i        ] * f(i))
#define ml10(f)	(window[0x1ff - i] * f(i + 0x200))
#define ml20(f)	(window[i + 0x100] * f(i + 0x100))
#define ml30(f)	(window[0x0ff - i] * f(i + 0x300))

#define ml01(f)	(window[i + 0x001] * f(i + 0x001))
#define ml11(f)	(window[0x1fe - i] * f(i + 0x201))
#define ml21(f)	(window[i + 0x101] * f(i + 0x101))
#define ml31(f)	(window[0x0fe - i] * f(i + 0x301))

#define ms00(f)	(window_s[i       ] * f(i + k))
#define ms10(f)	(window_s[0x7f - i] * f(i + k + 0x80))
#define ms20(f)	(window_s[i + 0x40] * f(i + k + 0x40))
#define ms30(f)	(window_s[0x3f - i] * f(i + k + 0xc0))

#define ms01(f)	(window_s[i + 0x01] * f(i + k + 0x01))
#define ms11(f)	(window_s[0x7e - i] * f(i + k + 0x81))
#define ms21(f)	(window_s[i + 0x41] * f(i + k + 0x41))
#define ms31(f)	(window_s[0x3e - i] * f(i + k + 0xc1))



void fft_short(
    FLOAT x_real[3][BLKSIZE_s], int chn, short *buffer[2])
{
    short i, j, b;

    for (b = 0; b < 3; b++) {
	FLOAT *x = &x_real[b][BLKSIZE_s / 2];
	short k = (576 / 3) * (b + 1);
	j = BLKSIZE_s / 8 - 1;
	if (chn < 2) {
	    do {
		FLOAT f0,f1,f2,f3, w;

		i = rv_tbl[j << 2];

		f0 = ms00(ch01); w = ms10(ch01); f1 = f0 - w; f0 = f0 + w;
		f2 = ms20(ch01); w = ms30(ch01); f3 = f2 - w; f2 = f2 + w;

		x -= 4;
		x[0] = f0 + f2;
		x[2] = f0 - f2;
		x[1] = f1 + f3;
		x[3] = f1 - f3;

		f0 = ms01(ch01); w = ms11(ch01); f1 = f0 - w; f0 = f0 + w;
		f2 = ms21(ch01); w = ms31(ch01); f3 = f2 - w; f2 = f2 + w;

		x[BLKSIZE_s / 2 + 0] = f0 + f2;
		x[BLKSIZE_s / 2 + 2] = f0 - f2;
		x[BLKSIZE_s / 2 + 1] = f1 + f3;
		x[BLKSIZE_s / 2 + 3] = f1 - f3;
	    } while (--j >= 0);
	} else if (chn == 2) {
	    do {
		FLOAT f0,f1,f2,f3, w;

		i = rv_tbl[j << 2];

		f0 = ms00(ch2); w = ms10(ch2); f1 = f0 - w; f0 = f0 + w;
		f2 = ms20(ch2); w = ms30(ch2); f3 = f2 - w; f2 = f2 + w;

		x -= 4;
		x[0] = f0 + f2;
		x[2] = f0 - f2;
		x[1] = f1 + f3;
		x[3] = f1 - f3;

		f0 = ms01(ch2); w = ms11(ch2); f1 = f0 - w; f0 = f0 + w;
		f2 = ms21(ch2); w = ms31(ch2); f3 = f2 - w; f2 = f2 + w;

		x[BLKSIZE_s / 2 + 0] = f0 + f2;
		x[BLKSIZE_s / 2 + 2] = f0 - f2;
		x[BLKSIZE_s / 2 + 1] = f1 + f3;
		x[BLKSIZE_s / 2 + 3] = f1 - f3;
	    } while (--j >= 0);
	} else {
	    do {
		FLOAT f0,f1,f2,f3, w;

		i = rv_tbl[j << 2];

		f0 = ms00(ch3); w = ms10(ch3); f1 = f0 - w; f0 = f0 + w;
		f2 = ms20(ch3); w = ms30(ch3); f3 = f2 - w; f2 = f2 + w;

		x -= 4;
		x[0] = f0 + f2;
		x[2] = f0 - f2;
		x[1] = f1 + f3;
		x[3] = f1 - f3;

		f0 = ms01(ch3); w = ms11(ch3); f1 = f0 - w; f0 = f0 + w;
		f2 = ms21(ch3); w = ms31(ch3); f3 = f2 - w; f2 = f2 + w;

		x[BLKSIZE_s / 2 + 0] = f0 + f2;
		x[BLKSIZE_s / 2 + 2] = f0 - f2;
		x[BLKSIZE_s / 2 + 1] = f1 + f3;
		x[BLKSIZE_s / 2 + 3] = f1 - f3;
	    } while (--j >= 0);
	}

	fht(x, BLKSIZE_s);
    }
}

void fft_long(
    FLOAT x[BLKSIZE], int chn, short *buffer[2])
{
    short i,jj = BLKSIZE / 8 - 1;
    x += BLKSIZE / 2;

    if (chn < 2) {
	do {
	    FLOAT f0,f1,f2,f3, w;

	    i = rv_tbl[jj];
	    f0 = ml00(ch01); w = ml10(ch01); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml20(ch01); w = ml30(ch01); f3 = f2 - w; f2 = f2 + w;

	    x -= 4;
	    x[0] = f0 + f2;
	    x[2] = f0 - f2;
	    x[1] = f1 + f3;
	    x[3] = f1 - f3;

	    f0 = ml01(ch01); w = ml11(ch01); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml21(ch01); w = ml31(ch01); f3 = f2 - w; f2 = f2 + w;

	    x[BLKSIZE / 2 + 0] = f0 + f2;
	    x[BLKSIZE / 2 + 2] = f0 - f2;
	    x[BLKSIZE / 2 + 1] = f1 + f3;
	    x[BLKSIZE / 2 + 3] = f1 - f3;
	} while (--jj >= 0);
    } else if (chn == 2) {
	do {
	    FLOAT f0,f1,f2,f3, w;

	    i = rv_tbl[jj];
	    f0 = ml00(ch2); w = ml10(ch2); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml20(ch2); w = ml30(ch2); f3 = f2 - w; f2 = f2 + w;

	    x -= 4;
	    x[0] = f0 + f2;
	    x[2] = f0 - f2;
	    x[1] = f1 + f3;
	    x[3] = f1 - f3;

	    f0 = ml01(ch2); w = ml11(ch2); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml21(ch2); w = ml31(ch2); f3 = f2 - w; f2 = f2 + w;

	    x[BLKSIZE / 2 + 0] = f0 + f2;
	    x[BLKSIZE / 2 + 2] = f0 - f2;
	    x[BLKSIZE / 2 + 1] = f1 + f3;
	    x[BLKSIZE / 2 + 3] = f1 - f3;
	} while (--jj >= 0);
    } else {
	do {
	    FLOAT f0,f1,f2,f3, w;

	    i = rv_tbl[jj];
	    f0 = ml00(ch3); w = ml10(ch3); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml20(ch3); w = ml30(ch3); f3 = f2 - w; f2 = f2 + w;

	    x -= 4;
	    x[0] = f0 + f2;
	    x[2] = f0 - f2;
	    x[1] = f1 + f3;
	    x[3] = f1 - f3;

	    f0 = ml01(ch3); w = ml11(ch3); f1 = f0 - w; f0 = f0 + w;
	    f2 = ml21(ch3); w = ml31(ch3); f3 = f2 - w; f2 = f2 + w;

	    x[BLKSIZE / 2 + 0] = f0 + f2;
	    x[BLKSIZE / 2 + 2] = f0 - f2;
	    x[BLKSIZE / 2 + 1] = f1 + f3;
	    x[BLKSIZE / 2 + 3] = f1 - f3;
	} while (--jj >= 0);
    }

    fht(x, BLKSIZE);
}


void init_fft(void)
{
    int i;

    FLOAT r = PI*0.125;
    for (i = 0; i < TRI_SIZE; i++) {
	costab[i*2  ] = cos(r);
	costab[i*2+1] = sin(r);
	r *= 0.25;
    }

    /*
     * calculate HANN window coefficients 
     */
    for (i = 0; i < BLKSIZE / 2; i++)
	window[i] = 0.5 * (1.0 - cos(2.0 * PI * (i + 0.5) / BLKSIZE));
    for (i = 0; i < BLKSIZE_s / 2; i++)
	window_s[i] = 0.5 * (1.0 - cos(2.0 * PI * (i + 0.5) / BLKSIZE_s));
}


/* ==== formatBitstream.c ==== */
/*********************************************************************
  Copyright (c) 1995 ISO/IEC JTC1 SC29 WG1, All Rights Reserved
  formatBitstream.c
**********************************************************************/
/*
  Revision History:

  Date        Programmer                Comment
  ==========  ========================= ===============================
  1995/09/06  mc@fivebats.com           created
  1995/09/18  mc@fivebats.com           bugfix: WriteMainDataBits
  1995/09/20  mc@fivebats.com           bugfix: store_side_info
*/

#include "formatBitstream.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* globals */
static int BitCount       = 0;
static int ThisFrameSize  = 0;
static int BitsRemaining  = 0;

void InitFormatBitStream(void)
{
	BitCount		= 0;
	ThisFrameSize	= 0;
	BitsRemaining	= 0;
}

/* forward declarations */
static int store_side_info( BF_FrameData *frameInfo );
static int main_data( BF_FrameData *frameInfo, BF_FrameResults *results );
static int side_queue_elements( int *forwardFrameLength, int *forwardSILength );
static void free_side_queues(void);
static void WriteMainDataBits( u_int val,u_int nbits,BF_FrameResults *results );

/*
  BitStreamFrame is the public interface to the bitstream
  formatting package. It writes one frame of main data per call.

  Assumptions:
  - The back pointer is zero on the first call
  - An integral number of bytes is written each frame

  You should be able to change the frame length, side info
  length, #channels, #granules on a frame-by-frame basis.

  See formatBitstream.h for more information about the data
  structures and the bitstream syntax.
*/
static int elements, forwardFrameLength, forwardSILength; 
void
BF_BitstreamFrame( BF_FrameData *frameInfo, BF_FrameResults *results )
{
  /*    int elements, forwardFrameLength, forwardSILength; */

    assert( frameInfo->nGranules <= MAX_GRANULES );
    assert( frameInfo->nChannels <= MAX_CHANNELS );

    /* save SI and compute its length */
    results->SILength = store_side_info( frameInfo );

    /* write the main data, inserting SI to maintain framing */
    results->mainDataLength = main_data( frameInfo, results );

    /*
      Caller must ensure that back SI and main data are
      an integral number of bytes, since the back pointer
      can only point to a byte boundary and this code
      does not add stuffing bits
    */
    assert( (BitsRemaining % 8) == 0 );

    /* calculate nextBackPointer */
    elements = side_queue_elements( &forwardFrameLength, &forwardSILength );
    results->nextBackPtr = (BitsRemaining / 8) + (forwardFrameLength / 8) - (forwardSILength / 8);
}

/*
  FlushBitstream writes zeros into main data
  until all queued headers are written. The
  queue data buffers are also freed.
*/
void
BF_FlushBitstream( BF_FrameData *frameInfo, BF_FrameResults *results )
{
  /*    int elements, forwardFrameLength, forwardSILength; */

    if ( elements )
    {
      int bitsRemaining = forwardFrameLength - forwardSILength;
      int wordsRemaining = bitsRemaining / 32;
      while ( wordsRemaining-- ) {
	WriteMainDataBits( 0, 32, results );
      }
      WriteMainDataBits( 0, (bitsRemaining % 32), results );	
    }
    

    results->mainDataLength = forwardFrameLength - forwardSILength;
    results->SILength       = forwardSILength;
    results->nextBackPtr    = 0;

    /* reclaim queue space */
    free_side_queues();

    /* reinitialize globals */
    BitCount       = 0;
    ThisFrameSize  = 0;
    BitsRemaining  = 0;    
    return;
}

int
BF_PartLength( BF_BitstreamPart *part )
{
    BF_BitstreamElement *ep = part->element;
    u_int i;
	int	bits=0;

    for ( i = 0; i < part->nrEntries; i++, ep++ )
		bits += ep->length;
    return bits;
}


/*
  The following is all private to this file
*/

typedef struct
{
    int frameLength;
    int SILength;
    int nGranules;
    int nChannels;
    BF_PartHolder *headerPH;
    BF_PartHolder *frameSIPH;
    BF_PartHolder *channelSIPH[MAX_CHANNELS];
    BF_PartHolder *spectrumSIPH[MAX_GRANULES][MAX_CHANNELS];
} MYSideInfo;

static MYSideInfo *get_side_info(void);
static int write_side_info(void);
typedef int (*PartWriteFcnPtr)( BF_BitstreamPart *part, BF_FrameResults *results );


static int
writePartMainData( BF_BitstreamPart *part, BF_FrameResults *results )
{
    BF_BitstreamElement *ep;
    u_int	i;
	int		bits=0;

    assert( results );
    assert( part );

    ep = part->element;
    for ( i = 0; i < part->nrEntries; i++, ep++ )
    {
		WriteMainDataBits( ep->value, ep->length, results );
		bits += ep->length;
    }
    return bits;
}

static int
writePartSideInfo( BF_BitstreamPart *part, BF_FrameResults *results )
{
    BF_BitstreamElement *ep;
    u_int	i;
	int		bits=0;

    assert( part );

    ep = part->element;
    for ( i = 0; i < part->nrEntries; i++, ep++ )
    {
		putMyBits( ep->value, ep->length );
		bits += ep->length;
    }
    return bits;
}

static int
main_data( BF_FrameData *fi, BF_FrameResults *results )
{
    int gr, ch, bits;
    PartWriteFcnPtr wp = writePartMainData;
    bits = 0;
    results->mainDataLength = 0;

    for ( gr = 0; gr < fi->nGranules; gr++ )
	for ( ch = 0; ch < fi->nChannels; ch++ )
	{
	    bits += (*wp)( fi->scaleFactors[gr][ch], results );
	    bits += (*wp)( fi->codedData[gr][ch],    results );
	    bits += (*wp)( fi->userSpectrum[gr][ch], results );
	}
    bits += (*wp)( fi->userFrameData, results );
    return bits;
}

/*
  This is a wrapper around PutBits() that makes sure that the
  framing header and side info are inserted at the proper
  locations
*/

static void
WriteMainDataBits( u_int val,
		   u_int nbits,
		   BF_FrameResults *results )
{
    assert( nbits <= 32 );
    if ( nbits == 0 )
	return;
    if ( BitCount == ThisFrameSize )
    {
	BitCount = write_side_info();
	BitsRemaining = ThisFrameSize - BitCount;
    }
    if ( nbits > (u_int)BitsRemaining )
    {
	unsigned extra = val >> (nbits - BitsRemaining);
	nbits -= BitsRemaining;
	putMyBits( extra, BitsRemaining );
	BitCount = write_side_info();
	BitsRemaining = ThisFrameSize - BitCount;
	putMyBits( val, nbits );
    }
    else
	putMyBits( val, nbits );
    BitCount += nbits;
    BitsRemaining -= nbits;
    assert( BitCount <= ThisFrameSize );
    assert( BitsRemaining >= 0 );
    assert( (BitCount + BitsRemaining) == ThisFrameSize );
}


static int
write_side_info(void)
{
    MYSideInfo *si;
    int bits, ch, gr;
    PartWriteFcnPtr wp = writePartSideInfo;

    bits = 0;
    si = get_side_info();
    ThisFrameSize = si->frameLength;
    bits += (*wp)( si->headerPH->part,  NULL );
    bits += (*wp)( si->frameSIPH->part, NULL );

    for ( ch = 0; ch < si->nChannels; ch++ )
	bits += (*wp)( si->channelSIPH[ch]->part, NULL );

    for ( gr = 0; gr < si->nGranules; gr++ )
	for ( ch = 0; ch < si->nChannels; ch++ )
	    bits += (*wp)( si->spectrumSIPH[gr][ch]->part, NULL );
    return bits;
}

typedef struct side_info_link
{
    struct side_info_link *next;
    MYSideInfo           side_info;
} side_info_link;

static struct side_info_link *side_queue_head   = NULL;
static struct side_info_link *side_queue_free   = NULL;

static void free_side_info_link( side_info_link *l );

static int
side_queue_elements( int *frameLength, int *SILength )
{
    int elements = 0;
    side_info_link *l;

    *frameLength = 0;
    *SILength    = 0;

    for ( l = side_queue_head; l; l = l->next )
    {
	elements++;
	*frameLength += l->side_info.frameLength;
	*SILength    += l->side_info.SILength;
    }
    return elements;
}

static int
store_side_info( BF_FrameData *info )
{
    int ch, gr;
    side_info_link *l;
    /* obtain a side_info_link to store info */
    side_info_link *f = side_queue_free;
    int bits = 0;

    if ( f == NULL )
    { /* must allocate another */
#ifdef DEBUG
	static int n_si = 0;
	n_si += 1;
	fprintf( stderr, "allocating side_info_link number %d\n", n_si );
#endif
	l = (side_info_link *) calloc( 1, sizeof(side_info_link) );
	if ( l == NULL )
	{
	    fprintf( stderr, "cannot allocate side_info_link" );
	    exit( 1);
	}
	l->next = NULL;
	l->side_info.headerPH  = BF_newPartHolder( info->header->nrEntries );
	l->side_info.frameSIPH = BF_newPartHolder( info->frameSI->nrEntries );
	for ( ch = 0; ch < info->nChannels; ch++ )
	    l->side_info.channelSIPH[ch] = BF_newPartHolder( info->channelSI[ch]->nrEntries );
	for ( gr = 0; gr < info->nGranules; gr++ )
	    for ( ch = 0; ch < info->nChannels; ch++ )
		l->side_info.spectrumSIPH[gr][ch] = BF_newPartHolder( info->spectrumSI[gr][ch]->nrEntries );
	
    }
    else
    { /* remove from the free list */
	side_queue_free = f->next;
	f->next = NULL;
	l = f;
    }
    /* copy data */
    l->side_info.frameLength = info->frameLength;
    l->side_info.nGranules   = info->nGranules;
    l->side_info.nChannels   = info->nChannels;
    l->side_info.headerPH    = BF_LoadHolderFromBitstreamPart( l->side_info.headerPH,  info->header );
    l->side_info.frameSIPH   = BF_LoadHolderFromBitstreamPart( l->side_info.frameSIPH, info->frameSI );

    bits += BF_PartLength( info->header );
    bits += BF_PartLength( info->frameSI );

    for ( ch = 0; ch < info->nChannels; ch++ )
    {
	l->side_info.channelSIPH[ch] = BF_LoadHolderFromBitstreamPart( l->side_info.channelSIPH[ch],
								       info->channelSI[ch] );
	bits += BF_PartLength( info->channelSI[ch] );
    }

    for ( gr = 0; gr < info->nGranules; gr++ )
	for ( ch = 0; ch < info->nChannels; ch++ )
	{
	    l->side_info.spectrumSIPH[gr][ch] = BF_LoadHolderFromBitstreamPart( l->side_info.spectrumSIPH[gr][ch],
										info->spectrumSI[gr][ch] );
	    bits += BF_PartLength( info->spectrumSI[gr][ch] );
	}
    l->side_info.SILength = bits;
    /* place at end of queue */
    f = side_queue_head;
    if ( f == NULL )
    {  /* empty queue */
	side_queue_head = l;
    }
    else
    { /* find last element */
	while ( f->next )
	    f = f->next;
	f->next = l;
    }
    return bits;
}

static MYSideInfo*
get_side_info(void)
{
    side_info_link *f = side_queue_free;
    side_info_link *l = side_queue_head;
    
    /*
      If we stop here it means you didn't provide enough
      headers to support the amount of main data that was
      written.
    */
    assert( l );
    
    /* update queue head */
    side_queue_head = l->next;

    /*
      Append l to the free list. You can continue
      to use it until store_side_info is called
      again, which will not happen again for this
      frame.
    */
    side_queue_free = l;
    l->next = f;
    return &l->side_info;
}

static void
free_side_queues(void)
{
    side_info_link *l, *next;
    
    for ( l = side_queue_head; l; l = next )
    {
	next = l->next;
	free_side_info_link( l );
    }
    side_queue_head = NULL;

    for ( l = side_queue_free; l; l = next )
    {
	next = l->next;
	free_side_info_link( l );
    }
    side_queue_free = NULL;
}

static void
free_side_info_link( side_info_link *l )
{
    int gr, ch;

    l->side_info.headerPH  = BF_freePartHolder( l->side_info.headerPH );
    l->side_info.frameSIPH = BF_freePartHolder( l->side_info.frameSIPH );

    for ( ch = 0; ch < l->side_info.nChannels; ch++ )
	l->side_info.channelSIPH[ch] = BF_freePartHolder( l->side_info.channelSIPH[ch] );

    for ( gr = 0; gr < l->side_info.nGranules; gr++ )
	for ( ch = 0; ch < l->side_info.nChannels; ch++ )
	    l->side_info.spectrumSIPH[gr][ch] = BF_freePartHolder( l->side_info.spectrumSIPH[gr][ch] );

    free( l );
}
/*
  Allocate a new holder of a given size
*/
BF_PartHolder *BF_newPartHolder( int max_elements )
{
    BF_PartHolder *newPH    = (BF_PartHolder*) calloc( 1, sizeof(BF_PartHolder) );
    assert( newPH );
    newPH->max_elements  = max_elements;
    newPH->part          = (BF_BitstreamPart*) calloc( 1, sizeof(BF_BitstreamPart) );
    assert( newPH->part );
    newPH->part->element = (BF_BitstreamElement*) calloc( max_elements, sizeof(BF_BitstreamElement) );
    if (max_elements>0) assert( newPH->part->element );
    newPH->part->nrEntries = 0;
    return newPH;
}

BF_PartHolder *BF_NewHolderFromBitstreamPart( BF_BitstreamPart *thePart )
{
    BF_PartHolder *newHolder = BF_newPartHolder( thePart->nrEntries );
    return BF_LoadHolderFromBitstreamPart( newHolder, thePart );
}

BF_PartHolder *BF_LoadHolderFromBitstreamPart( BF_PartHolder *theHolder, BF_BitstreamPart *thePart )
{
    BF_BitstreamElement *pElem;
    u_int i;

    theHolder->part->nrEntries = 0;
    for ( i = 0; i < thePart->nrEntries; i++ )
    {
	pElem = &(thePart->element[i]);
	theHolder = BF_addElement( theHolder, pElem );
    }
    return theHolder;
}

/*
  Grow or shrink a part holder. Always creates a new
  one of the right length and frees the old one after
  copying the data.
*/
BF_PartHolder *BF_resizePartHolder( BF_PartHolder *oldPH, int max_elements )
{
    int elems, i;
    BF_PartHolder *newPH;

#ifdef DEBUG
    fprintf( stderr, "Resizing part holder from %d to %d\n",
	     oldPH->max_elements, max_elements );
#endif
    /* create new holder of the right length */
    newPH = BF_newPartHolder( max_elements );

    /* copy values from old to new */
    elems = (oldPH->max_elements > max_elements) ? max_elements : oldPH->max_elements;
    newPH->part->nrEntries = elems;
    for ( i = 0; i < elems; i++ )
	newPH->part->element[i] = oldPH->part->element[i];

    /* free old holder */
    BF_freePartHolder( oldPH );
    
    return newPH;
}

BF_PartHolder *BF_freePartHolder( BF_PartHolder *thePH )
{
    free( thePH->part->element );
    free( thePH->part );
    free( thePH );
    return NULL;
}

/*
  Add theElement to thePH, growing the holder if
  necessary. Returns ptr to the holder, which may
  not be the one you called it with!
*/
BF_PartHolder *BF_addElement( BF_PartHolder *thePH, BF_BitstreamElement *theElement )
{
    BF_PartHolder *retPH = thePH;
    int needed_entries = thePH->part->nrEntries + 1;
    int extraPad = 8;  /* add this many more if we need to resize */

    /* grow if necessary */
    if ( needed_entries > thePH->max_elements )
	retPH = BF_resizePartHolder( thePH, needed_entries + extraPad );

    /* copy the data */
    retPH->part->element[retPH->part->nrEntries++] = *theElement;
    return retPH;
}

/*
  Add a bit value and length to the element list in thePH
*/
BF_PartHolder *BF_addEntry( BF_PartHolder *thePH, u_int value, u_int length )
{
    BF_BitstreamElement myElement;
    myElement.value  = value;
    myElement.length = length;

    if ( length )
		return BF_addElement( thePH, &myElement );
    else
		return thePH;
}


/* ==== get_audio.c ==== */
#include <sys/stat.h>

#include "util.h"
#include "get_audio.h"
#ifdef HAVEGTK
#include "gtkanal.h"
#include <gtk/gtk.h>
#endif

#if (defined LIBSNDFILE || defined LAMESNDFILE)

#ifdef _WIN32
/* needed to set stdin to binary on windoze machines */
#include <io.h>
#endif



static FILE *musicin=NULL;  /* input file pointer */
static unsigned long num_samples;
static int samp_freq;
static int input_bitrate;
static int num_channels;
static int count_samples_carefully;

int read_samples_pcm(lame_global_flags *gfp,short sample_buffer[2304],int frame_size, int samples_to_read);
int read_samples_mp3(lame_global_flags *gfp,FILE *musicin,short int mpg123pcm[2][1152],int num_chan);





void lame_init_infile(lame_global_flags *gfp)
{
  /* open the input file */
  count_samples_carefully=0;
  OpenSndFile(gfp,gfp->inPath,gfp->in_samplerate,gfp->num_channels);  
  /* if GetSndSampleRate is non zero, use it to overwrite the default */
  if (GetSndSampleRate()) gfp->in_samplerate=GetSndSampleRate();
  if (GetSndChannels()) gfp->num_channels=GetSndChannels();
  gfp->num_samples = GetSndSamples();
}
void lame_close_infile(lame_global_flags *gfp)
{
  CloseSndFile(gfp);
}




/************************************************************************
*
* lame_readframe()
*
* PURPOSE:  reads a frame of audio data from a file to the buffer,
*   aligns the data for future processing, and separates the
*   left and right channels
*
*
************************************************************************/
int lame_readframe(lame_global_flags *gfp,short int Buffer[2][1152])
{
  int iread;

  /* note: if input is gfp->stereo and output is mono, get_audio() 
   * will return  .5*(L+R) in channel 0,  and nothing in channel 1. */
  iread = get_audio(gfp,Buffer,gfp->stereo);
  
  /* check to see if we overestimated/underestimated totalframes */
  if (iread==0)  gfp->totalframes = Min(gfp->totalframes,gfp->frameNum+2);
  if (gfp->frameNum > (gfp->totalframes-1)) gfp->totalframes = gfp->frameNum;
  return iread;
}





/************************************************************************
*
* get_audio()
*
* PURPOSE:  reads a frame of audio data from a file to the buffer,
*   aligns the data for future processing, and separates the
*   left and right channels
*
*
************************************************************************/
int get_audio(lame_global_flags *gfp,short buffer[2][1152],int stereo)
{

  int		j;
  short	insamp[2304];
  int samples_read;
  int framesize,samples_to_read;
  static unsigned long num_samples_read;
  unsigned long remaining;
  int num_channels = gfp->num_channels;

  if (gfp->frameNum==0) {
    num_samples_read=0;
    num_samples= GetSndSamples();
  }
  framesize = gfp->mode_gr*576;

  samples_to_read = framesize;
  if (count_samples_carefully) { 
    /* if this flag has been set, then we are carefull to read 
     * exactly num_samples and no more.  This is usefull for .wav and .aiff
     * files which have id3 or other tags at the end.  Note that if you
     * are using LIBSNDFILE, this is not necessary */
    remaining=num_samples-Min(num_samples,num_samples_read);
    if (remaining < (unsigned long)framesize) 
      samples_to_read = remaining;
  }


  if (gfp->input_format==sf_mp3) {
    /* decode an mp3 file for the input */
    samples_read=read_samples_mp3(gfp,musicin,buffer,num_channels);
  }else{
    samples_read = read_samples_pcm(gfp,insamp,num_channels*framesize,num_channels*samples_to_read);
    samples_read /=num_channels;

    for(j=0;j<framesize;j++) {
      buffer[0][j] = insamp[num_channels*j];
      if (num_channels==2) buffer[1][j] = insamp[2*j+1];
      else buffer[1][j]=0;
    }
  }

  /* dont count things in this case to avoid overflows */
  if (num_samples!=MAX_U_32_NUM) num_samples_read += samples_read;
  return(samples_read);

}
  






  
int GetSndBitrate(void)
{
	return input_bitrate;
}






int read_samples_mp3(lame_global_flags *gfp,FILE *musicin,short int mpg123pcm[2][1152],int stereo)
{
#if (defined  AMIGA_MPEGA || defined HAVEMPGLIB) 
  int j,out=0;
#ifdef HAVEGTK
  static int framesize=0;
  int ch;
#endif

  out=lame_decode_fromfile(musicin,mpg123pcm[0],mpg123pcm[1]);
  /* out = -1:  error, probably EOF */
  /* out = 0:   not possible with lame_decode_fromfile() */
  /* out = number of output samples */

  if (out==-1) {
    for ( j = 0; j < 1152; j++ ) {
      mpg123pcm[0][j] = 0;
      mpg123pcm[1][j] = 0;
    }
  }


#ifdef HAVEGTK
  if (gfp->gtkflag) {
    framesize=1152;
    if (out==576) framesize=576;
    
    /* add a delay of framesize-DECDELAY, which will make the total delay
     * exactly one frame, so we can sync MP3 output with WAV input */
    
    for ( ch = 0; ch < stereo; ch++ ) {
      for ( j = 0; j < framesize-DECDELAY; j++ )
	pinfo->pcmdata2[ch][j] = pinfo->pcmdata2[ch][j+framesize];
      for ( j = 0; j < framesize; j++ ) 
	pinfo->pcmdata2[ch][j+framesize-DECDELAY] = mpg123pcm[ch][j];
    }
  
  pinfo->frameNum123 = gfp->frameNum-1;
  pinfo->frameNum = gfp->frameNum;
  }
#endif
  if (out==-1) return 0;
  else return out;
#else
  fprintf(stderr,"Error: libmp3lame was not compiled with I/O support \n");
  exit(1);
#endif
}
#endif  /* LAMESNDFILE or LIBSNDFILE */




#ifdef LIBSNDFILE 
/*
** Copyright (C) 1999 Albert Faber
**  
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include <stdio.h>

/* External references */

static SNDFILE*	gs_pSndFileIn=NULL;
static SF_INFO	gs_wfInfo;


unsigned long GetSndSamples(void)
{
       return gs_wfInfo.samples;
}
int GetSndSampleRate(void)
{
	return gs_wfInfo.samplerate;
}

int GetSndChannels(void)
{
	return gs_wfInfo.channels;
}

void CloseSndFile(lame_global_flags *gfp)
{
  if (gfp->input_format==sf_mp3) {
#ifndef AMIGA_MPEGA
    if (fclose(musicin) != 0){
      fprintf(stderr, "Could not close audio input file\n");
      exit(2);
    }
#endif
  }else{
	if (gs_pSndFileIn)
	{
		if (sf_close(gs_pSndFileIn) !=0)
		{
			fprintf(stderr, "Could not close sound file \n");
			exit(2);
		}
	}
  }
}



FILE * OpenSndFile(lame_global_flags *gfp,const char* lpszFileName, int default_samp,
int default_channels)
{
  input_bitrate=0;
  if (gfp->input_format==sf_mp3) {
#ifdef AMIGA_MPEGA
    if (-1==lame_decode_initfile(lpszFileName,&num_channels,&samp_freq,&input_bitrate,&num_samples)) {
      fprintf(stderr,"Error reading headers in mp3 input file %s.\n", lpszFileName);
      exit(1);
    }
#endif
#ifdef HAVEMPGLIB
    if ((musicin = fopen(lpszFileName, "rb")) == NULL) {
      fprintf(stderr, "Could not find \"%s\".\n", lpszFileName);
      exit(1);
    }
    if (-1==lame_decode_initfile(musicin,&num_channels,&samp_freq,&input_bitrate,&num_samples)) {
      fprintf(stderr,"Error reading headers in mp3 input file %s.\n", lpszFileName);
      exit(1);
    }
#endif
    gs_wfInfo.samples=num_samples;
    gs_wfInfo.channels=num_channels;
    gs_wfInfo.samplerate=samp_freq;

  } else {

    /* Try to open the sound file */
    /* set some defaults incase input is raw PCM */
    gs_wfInfo.seekable=(gfp->input_format!=sf_raw);  /* if user specified -r, set to not seekable */
    gs_wfInfo.samplerate=default_samp;
    gs_wfInfo.pcmbitwidth=16;
    gs_wfInfo.channels=default_channels;
    if (DetermineByteOrder()==order_littleEndian) {
      if (gfp->swapbytes) gs_wfInfo.format=SF_FORMAT_RAW_BE;
      else gs_wfInfo.format=SF_FORMAT_RAW_LE;
    } else {
      if (gfp->swapbytes) gs_wfInfo.format=SF_FORMAT_RAW_LE;
      else gs_wfInfo.format=SF_FORMAT_RAW_BE;
    }

    gs_pSndFileIn=sf_open_read(lpszFileName,&gs_wfInfo);

        /* Check result */
	if (gs_pSndFileIn==NULL)
	{
	        sf_perror(gs_pSndFileIn);
		fprintf(stderr, "Could not open sound file \"%s\".\n", lpszFileName);
		exit(1);
	}

    if ((gs_wfInfo.format==SF_FORMAT_RAW_LE) || 
	(gs_wfInfo.format==SF_FORMAT_RAW_BE)) 
      gfp->input_format=sf_raw;

#ifdef _DEBUG_SND_FILE
	printf("\n\nSF_INFO structure\n");
	printf("samplerate        :%d\n",gs_wfInfo.samplerate);
	printf("samples           :%d\n",gs_wfInfo.samples);
	printf("channels          :%d\n",gs_wfInfo.channels);
	printf("pcmbitwidth       :%d\n",gs_wfInfo.pcmbitwidth);
	printf("format            :");

	/* new formats from sbellon@sbellon.de  1/2000 */
        if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_WAV)
	  printf("Microsoft WAV format (big endian). ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_AIFF)
	  printf("Apple/SGI AIFF format (little endian). ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_AU)
	  printf("Sun/NeXT AU format (big endian). ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_AULE)
	  printf("DEC AU format (little endian). ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_RAW)
	  printf("RAW PCM data. ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_PAF)
	  printf("Ensoniq PARIS file format. ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_SVX)
	  printf("Amiga IFF / SVX8 / SV16 format. ");
	if ((gs_wfInfo.format&SF_FORMAT_TYPEMASK)==SF_FORMAT_NIST)
	  printf("Sphere NIST format. ");

	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_PCM)
	  printf("PCM data in 8, 16, 24 or 32 bits.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_FLOAT)
	  printf("32 bit Intel x86 floats.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_ULAW)
	  printf("U-Law encoded.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_ALAW)
	  printf("A-Law encoded.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_IMA_ADPCM)
	  printf("IMA ADPCM.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_MS_ADPCM)
	  printf("Microsoft ADPCM.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_PCM_BE)
	  printf("Big endian PCM data.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_PCM_LE)
	  printf("Little endian PCM data.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_PCM_S8)
	  printf("Signed 8 bit PCM.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_PCM_U8)
	  printf("Unsigned 8 bit PCM.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_SVX_FIB)
	  printf("SVX Fibonacci Delta encoding.");
	if ((gs_wfInfo.format&SF_FORMAT_SUBMASK)==SF_FORMAT_SVX_EXP)
	  printf("SVX Exponential Delta encoding.");




	printf("\n");
	printf("pcmbitwidth       :%d\n",gs_wfInfo.pcmbitwidth);
	printf("sections          :%d\n",gs_wfInfo.sections);
	printf("seekable          :\n",gs_wfInfo.seekable);
#endif
  }

  if (gs_wfInfo.samples==MAX_U_32_NUM) {
    struct stat sb;
    /* try to figure out num_samples */
    if (0==stat(lpszFileName,&sb)) {
      /* try file size, assume 2 bytes per sample */
      if (gfp->input_format == sf_mp3) {
	FLOAT totalseconds = (sb.st_size*8.0/(1000.0*GetSndBitrate()));
	gs_wfInfo.samples= totalseconds*GetSndSampleRate();
      }else{
	gs_wfInfo.samples = sb.st_size/(2*GetSndChannels());
      }
    }
  }
  return musicin;    
}


/************************************************************************
*
* read_samples()
*
* PURPOSE:  reads the PCM samples from a file to the buffer
*
*  SEMANTICS:
* Reads #samples_read# number of shorts from #musicin# filepointer
* into #sample_buffer[]#.  Returns the number of samples read.
*
************************************************************************/

int read_samples_pcm(lame_global_flags *gfp,short sample_buffer[2304],int frame_size,int samples_to_read)
{
    int 		samples_read;
    int			rcode;

    samples_read=sf_read_short(gs_pSndFileIn,sample_buffer,samples_to_read);
    
    rcode = samples_read;
    if (samples_read < frame_size)
      {
	/*fprintf(stderr,"Insufficient PCM input for one frame - fillout with zeros\n"); 
	*/
	if (samples_read<0) samples_read=0;
	for (; samples_read < frame_size; sample_buffer[samples_read++] = 0);
      }

	if (8==gs_wfInfo.pcmbitwidth)
	  for (; samples_read >= 0; sample_buffer[samples_read] = sample_buffer[samples_read--] * 256);

    return(rcode);
}


#endif /* ifdef LIBSNDFILE */
#ifdef LAMESNDFILE 

/************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************
 *
 * OLD ISO/LAME routines follow.  Used if you dont have LIBSNDFILE
 * or for stdin/stdout support
 *
 ************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************
 ************************************************************************/

/* Replacement for forward fseek(,,SEEK_CUR), because fseek() fails on pipes */
int fskip(FILE *sf,long num_bytes,int dummy)
{
  char data[1024];
  int nskip = 0;
  while (num_bytes > 0) {
    nskip = (num_bytes>1024) ? 1024 : num_bytes;
    num_bytes -= fread(data,(size_t)1,(size_t)nskip,sf);
  }
  /* return 0 if last read was successful */
  return num_bytes;
}



void CloseSndFile(lame_global_flags *gfp)
{
  if (fclose(musicin) != 0){
    fprintf(stderr, "Could not close audio input file\n");
    exit(2);
  }
}


unsigned long GetSndSamples(void)
{
       return num_samples;
}
int GetSndSampleRate(void)
{
	return samp_freq;
}

int GetSndChannels(void)
{
	return num_channels;
}


FILE * OpenSndFile(lame_global_flags *gfp,const char* inPath, int default_samp,
int default_channels)
{
  struct stat sb;
  void parse_file_header(lame_global_flags *gfp,FILE *sf);
  /* set the defaults from info incase we cannot determine them from file */
  num_samples=MAX_U_32_NUM;
  samp_freq=default_samp;
  num_channels = default_channels;
  
  if (!strcmp(inPath, "-")) {
    /* Read from standard input. */
#ifdef __EMX__
    _fsetmode(stdin,"b");
#elif (defined  __BORLANDC__)
    setmode(_fileno(stdin), O_BINARY);
#elif (defined  __CYGWIN__)
    setmode(fileno(stdin), _O_BINARY);
#elif (defined _WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    musicin = stdin;
  } else {
    if ((musicin = fopen(inPath, "rb")) == NULL) {
      fprintf(stderr, "Could not find \"%s\".\n", inPath);
      exit(1);
    }
  }
  
  input_bitrate=0;
  if (gfp->input_format==sf_mp3) {
#ifdef AMIGA_MPEGA
    if (-1==lame_decode_initfile(inPath,&num_channels,&samp_freq,&input_bitrate,&num_samples)) {
      fprintf(stderr,"Error reading headers in mp3 input file %s.\n", inPath);
      exit(1);
    }
#endif
#ifdef HAVEMPGLIB
    if (-1==lame_decode_initfile(musicin,&num_channels,&samp_freq,&input_bitrate,&num_samples)) {
      fprintf(stderr,"Error reading headers in mp3 input file %s.\n", inPath);
      exit(1);
    }
#endif
 }else{
   if (gfp->input_format != sf_raw) {
     parse_file_header(gfp,musicin);
   }
   
   if (gfp->input_format==sf_raw) {
     /* assume raw PCM */
     fprintf(stderr, "Assuming raw pcm input file");
     if (gfp->swapbytes==TRUE)
       fprintf(stderr, " : Forcing byte-swapping\n");
     else
       fprintf(stderr, "\n");
   }
 }
    
  if (num_samples==MAX_U_32_NUM && musicin != stdin) {
    /* try to figure out num_samples */
    if (0==stat(inPath,&sb)) {  
      /* try file size, assume 2 bytes per sample */
      if (gfp->input_format == sf_mp3) {
	FLOAT totalseconds = (sb.st_size*8.0/(1000.0*GetSndBitrate()));
	num_samples= totalseconds*GetSndSampleRate();
      }else{
	num_samples = sb.st_size/(2*GetSndChannels());
      }
    }
  }
  return musicin;
}
  
  
/************************************************************************
*
* read_samples()
*
* PURPOSE:  reads the PCM samples from a file to the buffer
*
*  SEMANTICS:
* Reads #samples_read# number of shorts from #musicin# filepointer
* into #sample_buffer[]#.  Returns the number of samples read.
*
************************************************************************/

int read_samples_pcm(lame_global_flags *gfp,short sample_buffer[2304], int frame_size,int samples_to_read)
{
    int samples_read;
    int rcode;
    int iswav=(gfp->input_format==sf_wave);

    samples_read = fread(sample_buffer, sizeof(short), samples_to_read, musicin);
    if (ferror(musicin)) {
      fprintf(stderr, "Error reading input file\n");
      exit(2);
    }

    /*
       Samples are big-endian. If this is a little-endian machine
       we must swap
     */
    if ( NativeByteOrder == order_unknown )
      {
	NativeByteOrder = DetermineByteOrder();
	if ( NativeByteOrder == order_unknown )
	  {
	    fprintf( stderr, "byte order not determined\n" );
	    exit( 1 );
	  }
      }
    /* intel=littleEndian */
    if (!iswav && ( NativeByteOrder == order_littleEndian ))
      SwapBytesInWords( sample_buffer, samples_read );

    if (iswav && ( NativeByteOrder == order_bigEndian ))
      SwapBytesInWords( sample_buffer, samples_read );

    if (gfp->swapbytes==TRUE)
      SwapBytesInWords( sample_buffer, samples_read );


    rcode=samples_read;
    if (samples_read < frame_size) {
      if (samples_read<0) samples_read=0;
      /*fprintf(stderr,"Insufficient PCM input for one frame - fillout with zeros\n");
      */
      for (; samples_read < frame_size; sample_buffer[samples_read++] = 0);
    }
    return(rcode);
}



#define WAV_ID_RIFF 0x52494646 /* "RIFF" */
#define WAV_ID_WAVE 0x57415645 /* "WAVE" */
#define WAV_ID_FMT  0x666d7420 /* "fmt " */
#define WAV_ID_DATA 0x64617461 /* "data" */

typedef struct fmt_chunk_data_struct {
	short	format_tag;			 /* Format category */
	u_short channels;			 /* Number of channels */
	u_long	samples_per_sec;	 /* Sampling rate */
	u_long	avg_bytes_per_sec;	 /* For buffer estimation */
	u_short block_align;		 /* Data block size */
	u_short bits_per_sample;	 /* for PCM data, anyway... */
} fmt_chunk_data;






/************************************************************************
*
* wave_check
*
* PURPOSE:	Checks Wave header information to make sure it is valid.
*			Exits if not.
*
************************************************************************/

static void
wave_check(char *file_name, fmt_chunk_data *wave_info)
{
	if (wave_info->bits_per_sample != 16) {
		fprintf(stderr, "%d-bit sample-size is not supported!\n",
			wave_info->bits_per_sample);
		exit(1);
	}
}


/*****************************************************************************
 *
 *	Read Microsoft Wave headers
 *
 *	By the time we get here the first 32-bits of the file have already been
 *	read, and we're pretty sure that we're looking at a WAV file.
 *
 *****************************************************************************/

static int
parse_wave_header(FILE *sf)
{
	fmt_chunk_data wave_info;
	int is_wav = 0;
	long data_length = 0, file_length, subSize = 0;
	int loop_sanity = 0;

	memset(&wave_info, 0, sizeof(wave_info));

	file_length = Read32BitsHighLow(sf);

	if (Read32BitsHighLow(sf) != WAV_ID_WAVE)
		return 0;

	for (loop_sanity = 0; loop_sanity < 20; ++loop_sanity) {
		u_int type = Read32BitsHighLow(sf);

		if (type == WAV_ID_FMT) {
			subSize = Read32BitsLowHigh(sf);
			if (subSize < 16) {
			  /*fprintf(stderr,
			    "'fmt' chunk too short (only %ld bytes)!", subSize);  */
				return 0;
			}

			wave_info.format_tag		= Read16BitsLowHigh(sf);
			subSize -= 2;
			wave_info.channels			= Read16BitsLowHigh(sf);
			subSize -= 2;
			wave_info.samples_per_sec	= Read32BitsLowHigh(sf);
			subSize -= 4;
			wave_info.avg_bytes_per_sec = Read32BitsLowHigh(sf);
			subSize -= 4;
			wave_info.block_align		= Read16BitsLowHigh(sf);
			subSize -= 2;
			wave_info.bits_per_sample	= Read16BitsLowHigh(sf);
			subSize -= 2;

			/* fprintf(stderr, "   skipping %d bytes\n", subSize); */

			if (subSize > 0) {
				if (fskip(sf, (long)subSize, SEEK_CUR) != 0 )
					return 0;
			};

		} else if (type == WAV_ID_DATA) {
			subSize = Read32BitsLowHigh(sf);
			data_length = subSize;
			is_wav = 1;
			/* We've found the audio data.	Read no further! */
			break;

		} else {
			subSize = Read32BitsLowHigh(sf);
			if (fskip(sf, (long) subSize, SEEK_CUR) != 0 ) return 0;
		}
	}

	if (is_wav) {
		/* make sure the header is sane */
		wave_check("name", &wave_info);

		num_channels  = wave_info.channels;
		samp_freq     = wave_info.samples_per_sec;
		num_samples   = data_length / (wave_info.channels * wave_info.bits_per_sample / 8);
	}
	return is_wav;
}



/************************************************************************
*
* aiff_check
*
* PURPOSE:	Checks AIFF header information to make sure it is valid.
*			Exits if not.
*
************************************************************************/

static void
aiff_check2(const char *file_name, IFF_AIFF *pcm_aiff_data)
{
	if (pcm_aiff_data->sampleType != IFF_ID_SSND) {
	   fprintf(stderr, "Sound data is not PCM in \"%s\".\n", file_name);
	   exit(1);
	}

	if (pcm_aiff_data->sampleSize != sizeof(short) * BITS_IN_A_BYTE) {
		fprintf(stderr, "Sound data is not %d bits in \"%s\".\n",
				(unsigned int) sizeof(short) * BITS_IN_A_BYTE, file_name);
		exit(1);
	}

	if (pcm_aiff_data->numChannels != 1 &&
		pcm_aiff_data->numChannels != 2) {
	   fprintf(stderr, "Sound data is not mono or stereo in \"%s\".\n",
			   file_name);
	   exit(1);
	}

	if (pcm_aiff_data->blkAlgn.blockSize != 0) {
	   fprintf(stderr, "Block size is not %d bytes in \"%s\".\n",
			   0, file_name);
	   exit(1);
	}

	if (pcm_aiff_data->blkAlgn.offset != 0) {
	   fprintf(stderr, "Block offset is not %d bytes in \"%s\".\n",
			   0, file_name);
	   exit(1);
	}
}

/*****************************************************************************
 *
 *	Read Audio Interchange File Format (AIFF) headers.
 *
 *	By the time we get here the first 32-bits of the file have already been
 *	read, and we're pretty sure that we're looking at an AIFF file.
 *
 *****************************************************************************/

static int
parse_aiff_header(FILE *sf)
{
	int is_aiff = 0;
	long chunkSize = 0, subSize = 0;
	IFF_AIFF aiff_info;

	memset(&aiff_info, 0, sizeof(aiff_info));
	chunkSize = Read32BitsHighLow(sf);
	
	if ( Read32BitsHighLow(sf) != IFF_ID_AIFF )
		return 0;
	
	while ( chunkSize > 0 )
	{
		u_int type = 0;
		chunkSize -= 4;

		type = Read32BitsHighLow(sf);

		/* fprintf(stderr,
			"found chunk type %08x '%4.4s'\n", type, (char*)&type); */

		/* don't use a switch here to make it easier to use 'break' for SSND */
		if (type == IFF_ID_COMM) {
			subSize = Read32BitsHighLow(sf);
			chunkSize -= subSize;

			aiff_info.numChannels	  = Read16BitsHighLow(sf);
			subSize -= 2;
			aiff_info.numSampleFrames = Read32BitsHighLow(sf);
			subSize -= 4;
			aiff_info.sampleSize	  = Read16BitsHighLow(sf);
			subSize -= 2;
			aiff_info.sampleRate	  = ReadIeeeExtendedHighLow(sf);
			subSize -= 10;

			if (fskip(sf, (long) subSize, SEEK_CUR) != 0 )
				return 0;

		} else if (type == IFF_ID_SSND) {
			subSize = Read32BitsHighLow(sf);
			chunkSize -= subSize;

			aiff_info.blkAlgn.offset	= Read32BitsHighLow(sf);
			subSize -= 4;
			aiff_info.blkAlgn.blockSize = Read32BitsHighLow(sf);
			subSize -= 4;

			if (fskip(sf, aiff_info.blkAlgn.offset, SEEK_CUR) != 0 )
				return 0;

			aiff_info.sampleType = IFF_ID_SSND;
			is_aiff = 1;

			/* We've found the audio data.	Read no further! */
			break;
			
		} else {
			subSize = Read32BitsHighLow(sf);
			chunkSize -= subSize;

			if (fskip(sf, (long) subSize, SEEK_CUR) != 0 )
				return 0;
		}
	}

	/* fprintf(stderr, "Parsed AIFF %d\n", is_aiff); */
	if (is_aiff) {
		/* make sure the header is sane */
		aiff_check2("name", &aiff_info);
		num_channels  = aiff_info.numChannels;
		samp_freq     = aiff_info.sampleRate;
		num_samples   = aiff_info.numSampleFrames;
	}
	return is_aiff;
}



/************************************************************************
*
* parse_file_header
*
* PURPOSE: Read the header from a bytestream.  Try to determine whether
*		   it's a WAV file or AIFF without rewinding, since rewind
*		   doesn't work on pipes and there's a good chance we're reading
*		   from stdin (otherwise we'd probably be using libsndfile).
*
* When this function returns, the file offset will be positioned at the
* beginning of the sound data.
*
************************************************************************/

void parse_file_header(lame_global_flags *gfp,FILE *sf)
{
	u_int type = 0;
	type = Read32BitsHighLow(sf);

	/* fprintf(stderr,
		"First word of input stream: %08x '%4.4s'\n", type, (char*) &type); */

	count_samples_carefully=0;
	gfp->input_format = sf_raw;

	if (type == WAV_ID_RIFF) {
		/* It's probably a WAV file */
		if (parse_wave_header(sf)) {
			gfp->input_format = sf_wave;
			count_samples_carefully=1;
		}

	} else if (type == IFF_ID_FORM) {
		/* It's probably an AIFF file */
		if (parse_aiff_header(sf)) {
			gfp->input_format = sf_aiff;
			count_samples_carefully=1;
		}
	}
	if (gfp->input_format==sf_raw) {
	  /*
	  ** Assume it's raw PCM.	 Since the audio data is assumed to begin
	  ** at byte zero, this will unfortunately require seeking.
	  */
	  if (fseek(sf, 0L, SEEK_SET) != 0) {
	    /* ignore errors */
	  }
	  gfp->input_format = sf_raw;
	}
}
#endif  /* LAMESNDFILE */




/* ==== gpkplotting.c ==== */
#ifdef HAVEGTK
#include "gpkplotting.h"
#include "string.h"

static gint num_plotwindows = 0;
static gint max_plotwindows = 10;
static GdkPixmap *pixmaps[10];
static GtkWidget *pixmapboxes[10];




/* compute a gdkcolor */
void setcolor(GtkWidget *widget, GdkColor *color, gint red,gint green,gint blue)
{

  /* colors in GdkColor are taken from 0 to 65535, not 0 to 255.    */
  color->red = red * (65535/255);
  color->green = green * (65535/255);
  color->blue = blue * (65535/255);
  color->pixel = (gulong)(color->red*65536 + color->green*256 + color->blue);
  /* find closest in colormap, if needed */
  gdk_color_alloc(gtk_widget_get_colormap(widget),color);
}


void gpk_redraw(GdkPixmap *pixmap, GtkWidget *pixmapbox)
{
  /* redraw the entire pixmap */
  gdk_draw_pixmap(pixmapbox->window,
		  pixmapbox->style->fg_gc[GTK_WIDGET_STATE (pixmapbox)],
		  pixmap,0,0,0,0,
		  pixmapbox->allocation.width,
		  pixmapbox->allocation.height);
}


static GdkPixmap **findpixmap(GtkWidget *widget)
{
  int i;
  for (i=0; i<num_plotwindows  && widget != pixmapboxes[i] ; i++);
  if (i>=num_plotwindows) {
    g_print("findpixmap(): bad argument widget \n");
    return NULL;
  }
  return &pixmaps[i];
}

void gpk_graph_draw(GtkWidget *widget,               /* plot on this widged */
		   int n,                           /* number of data points */
		   gdouble *xcord, gdouble *ycord,  /* data */
		   gdouble xmn,gdouble ymn,         /* coordinates of corners */
		   gdouble xmx,gdouble ymx,
                   int clear,                       /* clear old plot first */
		   char *title,                     /* add a title (only if clear=1) */
                   GdkColor *color)		    
{
  GdkPixmap **ppixmap;
  GdkPoint *points;
  int i;
  gint16 width,height;
  GdkFont *fixed_font;
  GdkGC *gc;

  gc = gdk_gc_new(widget->window);
  gdk_gc_set_foreground(gc, color);



  if ((ppixmap=findpixmap(widget))) {
    width = widget->allocation.width;
    height = widget->allocation.height;


    if (clear) {
      /* white background */
      gdk_draw_rectangle (*ppixmap,
			  widget->style->white_gc,
			  TRUE,0, 0,width,height);
      /* title */
#ifndef _WIN32
      fixed_font = gdk_font_load ("-misc-fixed-medium-r-*-*-*-100-*-*-*-*-*-*");
#else
      fixed_font = gdk_font_load ("-misc-fixed-large-r-*-*-*-100-*-*-*-*-*-*");
#endif

      gdk_draw_text (*ppixmap,fixed_font,
		     widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
		     0,10,title,strlen(title));
    }
      

    points = g_malloc(n*sizeof(GdkPoint));
    for (i=0; i<n ; i++) {
      points[i].x =.5+  ((xcord[i]-xmn)*(width-1)/(xmx-xmn));
      points[i].y =.5+  ((ycord[i]-ymx)*(height-1)/(ymn-ymx));
    }
    gdk_draw_lines(*ppixmap,gc,points,n);
    g_free(points);
    gpk_redraw(*ppixmap,widget);
  }
  gdk_gc_destroy(gc);
}



void gpk_rectangle_draw(GtkWidget *widget,              /* plot on this widged */
			gdouble *xcord, gdouble *ycord, /* corners */
			gdouble xmn,gdouble ymn,        /* coordinates of corners */
			gdouble xmx,gdouble ymx,
			GdkColor *color)
{
  GdkPixmap **ppixmap;
  GdkPoint points[2];
  int i;
  gint16 width,height;
  GdkGC *gc;


  gc = gdk_gc_new(widget->window);
  gdk_gc_set_foreground(gc, color);


  if ((ppixmap=findpixmap(widget))) {
    width = widget->allocation.width;
    height = widget->allocation.height;


    for (i=0; i<2 ; i++) {
      points[i].x =.5+  ((xcord[i]-xmn)*(width-1)/(xmx-xmn));
      points[i].y =.5+  ((ycord[i]-ymx)*(height-1)/(ymn-ymx));
    }
    width=points[1].x-points[0].x + 1;
    height=points[1].y-points[0].y + 1;
    gdk_draw_rectangle(*ppixmap,gc,TRUE,
		       points[0].x,points[0].y,width,height);
    gpk_redraw(*ppixmap,widget);
  }
  gdk_gc_destroy(gc);
}



void gpk_bargraph_draw(GtkWidget *widget,           /* plot on this widged */
		   int n,                           /* number of data points */
		   gdouble *xcord, gdouble *ycord,  /* data */
		   gdouble xmn,gdouble ymn,         /* coordinates of corners */
		   gdouble xmx,gdouble ymx,
                   int clear,                       /* clear old plot first */
		   char *title,                     /* add a title (only if clear=1) */
                   int barwidth,                    /* bar width. 0=compute based on window size */    
                   GdkColor *color)		    
{
  GdkPixmap **ppixmap;
  GdkPoint points[2];
  int i;
  gint16 width,height,x,y,barheight;
  GdkFont *fixed_font;
  GdkGC *gc;


  gc = gdk_gc_new(widget->window);
  gdk_gc_set_foreground(gc, color);


  if ((ppixmap=findpixmap(widget))) {
    width = widget->allocation.width;
    height = widget->allocation.height;


    if (clear) {
      /* white background */
      gdk_draw_rectangle (*ppixmap,
			  widget->style->white_gc,
			  TRUE,0, 0,width,height);
      /* title */
#ifndef _WIN32
      fixed_font = gdk_font_load ("-misc-fixed-medium-r-*-*-*-100-*-*-*-*-*-*");
#else
      fixed_font = gdk_font_load ("-misc-fixed-large-r-*-*-*-100-*-*-*-*-*-*");
#endif

      gdk_draw_text (*ppixmap,fixed_font,
		     widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
		     0,10,title,strlen(title));
    }
      

    for (i=0; i<n ; i++) {
      points[1].x =.5+  ((xcord[i]-xmn)*(width-1)/(xmx-xmn));
      points[1].y =.5+  ((ycord[i]-ymx)*(height-1)/(ymn-ymx));
      points[0].x = points[1].x;
      points[0].y = height-1;

      x = .5+  ((xcord[i]-xmn)*(width-1)/(xmx-xmn));
      y = .5+((ycord[i]-ymx)*(height-1)/(ymn-ymx));
      if (!barwidth) barwidth  = (width/(n+1))-1;
      barwidth = barwidth > 5 ? 5 : barwidth;
      barwidth = barwidth < 1 ? 1 : barwidth;
      barheight = height-1 - y;
      /* gdk_draw_lines(*ppixmap,gc,points,2); */
      gdk_draw_rectangle(*ppixmap,gc,TRUE,x,y,barwidth,barheight);

    }
    gpk_redraw(*ppixmap,widget);
  }
  gdk_gc_destroy(gc);
}





/* Create a new backing pixmap of the appropriate size */
static gint
configure_event (GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
  GdkPixmap **ppixmap;
  if ((ppixmap=findpixmap(widget))){
    if (*ppixmap) gdk_pixmap_unref(*ppixmap);
    *ppixmap = gdk_pixmap_new(widget->window,
			    widget->allocation.width,
			    widget->allocation.height,
			    -1);
    gdk_draw_rectangle (*ppixmap,
			widget->style->white_gc,
			TRUE,
			0, 0,
			widget->allocation.width,
			widget->allocation.height);
  }
  return TRUE;
}



/* Redraw the screen from the backing pixmap */
static gint
expose_event (GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
  GdkPixmap **ppixmap;
  if ((ppixmap=findpixmap(widget))){
    gdk_draw_pixmap(widget->window,
		    widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
		    *ppixmap,
		    event->area.x, event->area.y,
		    event->area.x, event->area.y,
		    event->area.width, event->area.height);
  }

  return FALSE;
}





GtkWidget *gpk_plot_new(int width, int height)
{
  GtkWidget *pixmapbox;
   
  pixmapbox = gtk_drawing_area_new();
  gtk_drawing_area_size(GTK_DRAWING_AREA(pixmapbox),width,height);
  gtk_signal_connect (GTK_OBJECT (pixmapbox), "expose_event",
		      (GtkSignalFunc) expose_event, NULL);
  gtk_signal_connect (GTK_OBJECT(pixmapbox),"configure_event",
		      (GtkSignalFunc) configure_event, NULL);
  gtk_widget_set_events (pixmapbox, GDK_EXPOSURE_MASK);

  if (num_plotwindows < max_plotwindows) {
    pixmapboxes[num_plotwindows] = pixmapbox;
    pixmaps[num_plotwindows] = NULL;
    num_plotwindows ++;
  } else {
    g_print("gtk_plotarea_new(): exceeded maximum of 10 plotarea windows\n");
  }

  return pixmapbox;
}


#endif


/* ==== gtkanal.c ==== */
#ifdef HAVEGTK
#include <math.h>
#include <gtk/gtk.h>
#include "gpkplotting.h"
#include "util.h"
#include "gtkanal.h"
#include "version.h"
#include "lame.h"
#include "tables.h"
#include "quantize-pvt.h"
#include <assert.h>

int gtkflag;

extern int makeframe(void);

/* global variables for the state of the system */
static gint idle_keepgoing;        /* processing of frames is ON */
static gint idle_count_max;   /* number of frames to process before plotting */
static gint idle_count;       /* pause & plot when idle_count=idel_count_max */
static gint idle_end=0;      /* process all frames, stop at last frame  */
static gint idle_back = 0;     /* set when we are displaying the old data */
static int mp3done = 0;         /* last frame has been read */
static GtkWidget *frameprogress; /* progress bar */ 
static GtkWidget *framecounter;  /* progress counter */ 

static int subblock_draw[3] = { 1, 1, 1 };

/* main window */
GtkWidget *window;
/* Backing pixmap for drawing areas */
GtkWidget *pcmbox;       /* PCM data plotted here */
GtkWidget *winbox;       /* mpg123 synthesis data plotted here */
GtkWidget *enerbox[2];   /* spectrum, gr=0,1 plotted here */
GtkWidget *mdctbox[2];   /* mdct coefficients gr=0,1 plotted here */
GtkWidget *sfbbox[2];    /* scalefactors gr=0,1 plotted here */
GtkWidget *headerbox;    /* mpg123 header info shown here */

plotting_data *pinfo,*pplot;
plotting_data Pinfo[NUMPINFO];

struct gtkinfostruct {
  int filetype;           /* input file type 0=WAV, 1=MP3 */
  int msflag;             /* toggle between L&R vs M&S PCM data display */
  int chflag;             /* toggle between L & R channels */
  int kbflag;             /* toggle between wave # and barks */
  int flag123;            /* show mpg123 frame info, OR ISO encoder frame info */
  double avebits;         /* running average bits per frame */
  int approxbits;         /* (approx) bits per frame */
  int maxbits;            /* max bits per frame used so far*/
  int totemph;            /* total of frames with de-emphasis */
  int totms;              /* total frames with ms_stereo */
  int totis;              /* total frames with i_stereo */
  int totshort;           /* total granules with short blocks */
  int totmix;             /* total granules with mixed blocks */
  int pupdate;            /* plot while processing, or only when needed */
  int sfblines;           /* plot scalefactor bands in MDCT plot */
  int totalframes;
} gtkinfo;


static lame_global_flags *gfp;

/**********************************************************************
 * read one frame and encode it 
 **********************************************************************/
int gtkmakeframe(void)
{
  int iread = 0;
  static int init=0;
  static int mpglag;
  static short int Buffer[2][1152];
  int ch,j;
  int mp3count = 0;
  int mp3out = 0;
  short mpg123pcm[2][1152];
  char mp3buffer[LAME_MAXMP3BUFFER];
  

#ifndef HAVEMPGLIB
  fprintf(stderr,"Error: GTK frame analyzer requires MPGLIB\n");
  exit(1);
#else
  /* even if iread=0, get_audio hit EOF and returned Buffer=all 0's.  encode
   * and decode to flush any previous buffers from the decoder */

  pinfo->frameNum = gfp->frameNum;
  pinfo->sampfreq=gfp->out_samplerate;
  pinfo->framesize=576*gfp->mode_gr;
  pinfo->stereo = gfp->stereo;

  if (gfp->input_format == sf_mp3) {
    iread=lame_readframe(gfp,Buffer);
    gfp->frameNum++;
  }else {
    while (gfp->frameNum == pinfo->frameNum) {
      if (gfp->frameNum==0 && !init) {
	mpglag=1;
	lame_decode_init();
      }
      if (gfp->frameNum==1) init=0; /* reset for next time frameNum==0 */
      iread=lame_readframe(gfp,Buffer);
      
      
      mp3count=lame_encode(gfp,Buffer,mp3buffer,sizeof(mp3buffer)); /* encode frame */
      assert( !(mp3count > 0 && gfp->frameNum == pinfo->frameNum));
      /* not possible to produce mp3 data without encoding at least 
       * one frame of data which would increment gfp->frameNum */
    }
    mp3out=lame_decode(mp3buffer,mp3count,mpg123pcm[0],mpg123pcm[1]); /* re-synthesis to pcm */
    /* mp3out = 0:  need more data to decode */
    /* mp3out = -1:  error.  Lets assume 0 pcm output */
    /* mp3out = number of samples output */
    if (mp3out>0) assert(mp3out==pinfo->framesize);
    if (mp3out!=0) {
      /* decoded output is for frame pinfo->frameNum123 
       * add a delay of framesize-DECDELAY, which will make the total delay
       * exactly one frame */
      pinfo->frameNum123=pinfo->frameNum-mpglag;
      for ( ch = 0; ch < pinfo->stereo; ch++ ) {
	for ( j = 0; j < pinfo->framesize-DECDELAY; j++ )
	  pinfo->pcmdata2[ch][j] = pinfo->pcmdata2[ch][j+pinfo->framesize];
	for ( j = 0; j < pinfo->framesize; j++ ) {
	  pinfo->pcmdata2[ch][j+pinfo->framesize-DECDELAY] = 
	    (mp3out==-1) ? 0 : mpg123pcm[ch][j];
	}
      }
    }else{
      if (mpglag == MAXMPGLAG) {
	fprintf(stderr,"READ_AHEAD set too low - not enough frame buffering.\n");
	fprintf(stderr,"MP3x display of input and output PCM data out of sync.\n");
      }
      else mpglag++; 
      pinfo->frameNum123=-1;  /* no frame output */
    }
  }
#endif
  return iread;
}


void plot_frame(void)
{
  int i,j,n,ch,gr;
  gdouble *xcord,*ycord;
  gdouble xmx,xmn,ymx,ymn;
  double *data,*data2,*data3;
  char title2[80];
  char label[80],label2[80];
  char *title;
  plotting_data *pplot1;
  plotting_data *pplot2 = NULL;

  double en,samp;
  int sampindex,version=0;
  static int firstcall=1;
  static GdkColor *barcolor,*color,*grcolor[2];
  static GdkColor yellow,gray,cyan,magenta,orange,pink,red,green,blue,black,oncolor,offcolor;
  int blocktype[2][2];
  int headbits;
  int mode_gr = 2;

  /* find the frame where mpg123 produced output coming from input frame
   * pinfo.  i.e.:   out_frame + out_frame_lag = input_frame  */
  for (i=1; i<=MAXMPGLAG; i++ ) {
    if ((pplot-i)->frameNum123 == pplot->frameNum ) {
      pplot2 = pplot-i;
      break;
    }
  }
  if (i > MAXMPGLAG) {
    fprintf(stderr,"input/output pcm syncing problem.  should not happen!\n");
    pplot2=pplot-1;
  }


  /* however, the PCM data is delayed by 528 samples in the encoder filterbanks.
   * We added another 1152-528 delay to this so the PCM data is *exactly* one 
   * frame behind the header & MDCT information */
  pplot1 =pplot2 +1;                   /* back one frame for header info, MDCT */

  /* allocate these GC's only once */
  if (firstcall) {
    firstcall=0;
    /*    grcolor[0] = &magenta; */
    grcolor[0] = &blue;
    grcolor[1] = &green;
    barcolor = &gray;

    setcolor(headerbox,&oncolor,255,0,0);
    setcolor(headerbox,&offcolor,175,175,175);
    setcolor(pcmbox,&red,255,0,0);
    setcolor(pcmbox,&pink,255,0,255);
    setcolor(pcmbox,&magenta,255,0,100);
    setcolor(pcmbox,&orange,255,127,0);
    setcolor(pcmbox,&cyan,0,255,255);
    setcolor(pcmbox,&green,0,255,0);
    setcolor(pcmbox,&blue,0,0,255);
    setcolor(pcmbox,&black,0,0,0);
    setcolor(pcmbox,&gray,100,100,100);
    setcolor(pcmbox,&yellow,255,255,0);

  }

  /*******************************************************************
   * frame header info
   *******************************************************************/
  if (pplot1->sampfreq)
    samp=pplot1->sampfreq;
  else samp=1;
  sampindex = SmpFrqIndex((long)samp, &version);

  ch = gtkinfo.chflag;
  
  headbits = 32 + ((pplot1->stereo==2) ? 256 : 136);
  gtkinfo.approxbits = (pplot1->bitrate*1000*1152.0/samp) - headbits;
  /*font = gdk_font_load ("-misc-fixed-medium-r-*-*-*-100-*-*-*-*-*-*");*/
  sprintf(title2,"%3.1fkHz %ikbs ",samp/1000,pplot1->bitrate);
  gtk_text_freeze (GTK_TEXT(headerbox));
  gtk_text_backward_delete(GTK_TEXT(headerbox),
			    gtk_text_get_length(GTK_TEXT(headerbox)));
  gtk_text_set_point(GTK_TEXT(headerbox),0);
  gtk_text_insert(GTK_TEXT(headerbox),NULL,&oncolor,NULL,title2, -1);
  title = " mono ";
  if (2==pplot1->stereo) title = pplot1->js ? " js " : " s ";
  gtk_text_insert (GTK_TEXT(headerbox), NULL, &oncolor, NULL,title, -1);
  color = pplot1->ms_stereo ? &oncolor : &offcolor ; 
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,"ms ", -1);
  color = pplot1->i_stereo ? &oncolor : &offcolor ; 
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,"is ", -1);

  color = pplot1->crc ? &oncolor : &offcolor ; 
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,"crc ", -1);
  color = pplot1->padding ? &oncolor : &offcolor ; 
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,"pad ", -1);

  color = pplot1->emph ? &oncolor : &offcolor ; 
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,"em ", -1);

  sprintf(title2,"c1=%i,%i ",pplot1->big_values[0][ch],pplot1->big_values[1][ch]);
  gtk_text_insert (GTK_TEXT(headerbox), NULL, &black, NULL,title2, -1);

  color = pplot1->scfsi[ch] ? &oncolor : &offcolor ; 
  sprintf(title2,"scfsi=%i            ",pplot1->scfsi[ch]);
  gtk_text_insert (GTK_TEXT(headerbox), NULL, color, NULL,title2, -1);
  if (gtkinfo.filetype) 
    sprintf(title2," mdb=%i %i/NA",pplot1->maindata,pplot1->totbits);
  else
    sprintf(title2," mdb=%i   %i/%i",
	  pplot1->maindata,pplot1->totbits,pplot->resvsize);
  gtk_text_insert (GTK_TEXT(headerbox), NULL, &oncolor, NULL,title2, -1);
  gtk_text_thaw (GTK_TEXT(headerbox));



  /*******************************************************************
   * block type
   *******************************************************************/
  for (gr = 0 ; gr < mode_gr ; gr ++) 
    if (gtkinfo.flag123) 
      blocktype[gr][ch]=pplot1->mpg123blocktype[gr][ch];
    else blocktype[gr][ch]=pplot->blocktype[gr][ch]; 

  
  /*******************************************************************
   * draw the PCM data *
   *******************************************************************/
  n = 1600;  /* PCM frame + FFT window:   224 + 1152 + 224  */
  xcord = g_malloc(n*sizeof(gdouble));
  ycord = g_malloc(n*sizeof(gdouble));


  if (gtkinfo.msflag) 
    title=ch ? "Side Channel" :  "Mid Channel";
  else 
    title=ch ? "Right Channel" : "Left Channel";

  sprintf(title2,"%s  mask_ratio=%3.2f  %3.2f  ener_ratio=%3.2f  %3.2f",
	  title,
	  pplot->ms_ratio[0],pplot->ms_ratio[1],
	  pplot->ms_ener_ratio[0],pplot->ms_ener_ratio[1]);


  ymn = -32767 ; 
  ymx =  32767;
  xmn = 0;
  xmx = 1600-1;

  /*  0  ... 224      draw in black, connecting to 224 pixel
   * 1375 .. 1599     draw in black  connecting to 1375 pixel
   * 224 ... 1375     MP3 frame.  draw in blue
   */

  /* draw the title */
  gpk_graph_draw(pcmbox,0,xcord,ycord,xmn,ymn,xmx,ymx,1,title2,
		 &black);


  /* draw some hash marks dividing the frames */
  ycord[0] = ymx*.8;  ycord[1] = ymn*.8;
  for (gr=0 ; gr<=2; gr++) {
    xcord[0] = 223.5 + gr*576;   xcord[1] = 223.5 +gr*576;  
    gpk_rectangle_draw(pcmbox,xcord,ycord,xmn,ymn,xmx,ymx,&yellow);
  }
  for (gr = 0 ; gr < mode_gr ; gr++) {
    if (blocktype[gr][ch]==2) 
      for (i=1 ; i<=2; i++) {
	xcord[0] = 223.5+gr*576 + i*192; 
	xcord[1] = 223.5+gr*576 + i*192; 
	gpk_rectangle_draw(pcmbox,xcord,ycord,xmn,ymn,xmx,ymx,&yellow);
      }
  }
  /* bars representing FFT windows */
  xcord[0] = 0;       ycord[0] = ymn+3000;
  xcord[1] = 1024-1;  ycord[1] = ymn+1000;
  gpk_rectangle_draw(pcmbox,xcord,ycord,xmn,ymn,xmx,ymx,grcolor[0]);
  xcord[0] = 576;          ycord[0] = ymn+2000;
  xcord[1] = 576+1024-1;   ycord[1] = ymn;
  gpk_rectangle_draw(pcmbox,xcord,ycord,xmn,ymn,xmx,ymx,grcolor[1]);


  /* plot PCM data */
  for (i=0; i<n; i++) {
    xcord[i] = i;
    if (gtkinfo.msflag) 
      ycord[i] = ch ? .5*(pplot->pcmdata[0][i]-pplot->pcmdata[1][i]) : 
      .5*(pplot->pcmdata[0][i]+pplot->pcmdata[1][i]);
    else 
      ycord[i]=pplot->pcmdata[ch][i];
  }

  /* skip plot if we are doing an mp3 file */
  if (!gtkinfo.filetype) {
    n = 224;    /* number of points on end of blue part */
    /* data left of frame */
    gpk_graph_draw(pcmbox,n+1,xcord,ycord,xmn,ymn,xmx,ymx,0,title2,&black);
    /* data right of frame */
    gpk_graph_draw(pcmbox,n+1,&xcord[1152+n-1],&ycord[1152+n-1],
		   xmn,ymn,xmx,ymx,0,title2,&black);
    /* the actual frame */
    gpk_graph_draw(pcmbox,1152,&xcord[n],&ycord[n],xmn,ymn,xmx,ymx,0,title2,&black);
  }


  /*******************************************************************/
  /* draw the PCM re-synthesis data */
  /*******************************************************************/
  n = 1152;
  /*
  sprintf(title2,"Re-synthesis  mask_ratio=%3.2f  %3.2f  ener_ratio=%3.2f  %3.2f",
	  pplot->ms_ratio[0],pplot->ms_ratio[1],
	  pplot->ms_ener_ratio[0],pplot->ms_ener_ratio[1]);
  */
  title="Re-synthesis";


  ymn = -32767 ; 
  ymx =  32767;
  xmn = 0;
  xmx = 1600-1; 
  gpk_graph_draw(winbox,0,xcord,ycord,
		 xmn,ymn,xmx,ymx,1,title,&black);
  /* draw some hash marks dividing the frames */
  ycord[0] = ymx*.8;  ycord[1] = ymn*.8;
  for (gr=0 ; gr<=2; gr++) {
    xcord[0] = 223.5 + gr*576;   xcord[1] = 223.5 +gr*576;  
    gpk_rectangle_draw(winbox,xcord,ycord,xmn,ymn,xmx,ymx,&yellow);
  }
  for (gr = 0 ; gr < 2 ; gr++) {
    if (blocktype[gr][ch]==2) 
      for (i=1 ; i<=2; i++) {
	xcord[0] = 223.5+gr*576 + i*192; 
	xcord[1] = 223.5+gr*576 + i*192; 
	gpk_rectangle_draw(winbox,xcord,ycord,xmn,ymn,xmx,ymx,&yellow);
      }
  }



  n = 224;
  for (j=1152-n,i=0; i<=n; i++,j++) {
    xcord[i] = i;
    if (gtkinfo.msflag) 
      ycord[i] = ch ? .5*(pplot1->pcmdata2[0][j]-
                          pplot1->pcmdata2[1][j]) : 
      .5*(pplot1->pcmdata2[0][j]+pplot1->pcmdata2[1][j]);
    else 
      ycord[i]=pplot1->pcmdata2[ch][j];
  }
  gpk_graph_draw(winbox,n+1,xcord,ycord,
		 xmn,ymn,xmx,ymx,0,title,&black);

  n = 1152;
  for (i=0; i<n; i++) {
    xcord[i] = i+224;
    if (gtkinfo.msflag) 
      ycord[i] = ch ? .5*(pplot2->pcmdata2[0][i]-pplot2->pcmdata2[1][i]) : 
      .5*(pplot2->pcmdata2[0][i]+pplot2->pcmdata2[1][i]);
    else 
      ycord[i]=pplot2->pcmdata2[ch][i];
  }
  gpk_graph_draw(winbox,n,xcord,ycord,
		 xmn,ymn,xmx,ymx,0,title,&black);





  /*******************************************************************/
  /* draw the MDCT energy spectrum */
  /*******************************************************************/
  for (gr = 0 ; gr < mode_gr ; gr ++) {
    int bits;
    char *blockname="";
    switch (blocktype[gr][ch]) {
    case 0: blockname = "normal"; 	break;
    case 1:  	blockname = "start";	break;
    case 2: 	blockname = "short"; 	break;
    case 3: 	blockname = "end"; 	break;
    }
    strcpy(label,blockname);
    if (pplot1->mixed[gr][ch]) strcat(label,"(mixed)");

    
    
    
    n = 576;
    if (gtkinfo.flag123) {
      data = pplot1->mpg123xr[gr][0];
      data2 = pplot1->mpg123xr[gr][1];
    }else{
      data = pplot->xr[gr][0];
      data2 = pplot->xr[gr][1];
    }
    

    xmn = 0;
    xmx = n-1;
    ymn=0;
    ymx=11;

    /* draw title, erase old plot */
    if (gtkinfo.flag123) bits=pplot1->mainbits[gr][ch];
    else bits=pplot->LAMEmainbits[gr][ch];
    sprintf(title2,"MDCT%1i(%s) bits=%i q=%i ",gr,label,bits,
	      pplot1->qss[gr][ch]);
    gpk_bargraph_draw(mdctbox[gr],0,xcord,ycord,
		      xmn,ymn,xmx,ymx,1,title2,0,barcolor);

    /* draw some hash marks showing scalefactor bands */
    if (gtkinfo.sfblines) {
      int fac,nsfb, *scalefac;
      if (blocktype[gr][ch]==SHORT_TYPE) {
	nsfb=SBMAX_s;
	fac=3;
	scalefac = scalefac_band.s;
      }else{
	nsfb=SBMAX_l;
	fac=1;
	scalefac = scalefac_band.l;
      }
      for (i=nsfb-7 ; i<nsfb; i++) {
	ycord[0] = .8*ymx;  ycord[1] = ymn;
	xcord[0] = fac*scalefac[i];
	xcord[1] = xcord[0];
	gpk_rectangle_draw(mdctbox[gr],xcord,ycord,xmn,ymn,xmx,ymx,&yellow);
      }
    }   



    ymn=9e20;
    ymx=-9e20;
    for (i=0; i<n; i++) {
      double coeff;
      xcord[i] = i;
      if (gtkinfo.msflag){
	coeff = ch ?  .5*(data[i]-data2[i]) : .5*(data[i]+data2[i]) ;
      }else{
	coeff = ch ? data2[i] : data[i];
      }
      if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
        coeff = 0;
      ycord[i]=coeff*coeff*1e10;
      ycord[i] = log10( MAX( ycord[i],(double) 1)); 
      ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
      ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
    }
    /*  print the min/max
	sprintf(title2,"MDCT%1i %5.2f %5.2f  bits=%i",gr,ymn,ymx,
	pplot1->mainbits[gr][ch]);
    */
    if (gtkinfo.flag123) bits=pplot1->mainbits[gr][ch];
    else bits=pplot->LAMEmainbits[gr][ch];
    
    
    sprintf(title2,"MDCT%1i(%s) bits=%i q=%i ",gr,label,bits,
	      pplot1->qss[gr][ch]);

    xmn = 0;
    xmx = n-1;
    ymn=0;
    ymx=11;
    gpk_bargraph_draw(mdctbox[gr],n,xcord,ycord,
		      xmn,ymn,xmx,ymx,0,title2,0,barcolor);
  }
  


  
  /*******************************************************************
   * draw the psy model energy spectrum (k space) 
   * l3psy.c computes pe, en, thm for THIS granule.  
   *******************************************************************/
 if (gtkinfo.kbflag){
    for (gr = 0 ; gr < mode_gr ; gr ++) {
      n = HBLKSIZE; /* only show half the spectrum */

      data = &pplot->energy[gr][ch][0];
      
      ymn=9e20;
      ymx=-9e20;
      for (i=0; i<n; i++) {
	xcord[i] = i+1;
        if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
          ycord[i] = 0;
        else
	  ycord[i] = log10( MAX( data[i],(double) 1));
	ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
	ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
      }
      for (en=0 , j=0; j<BLKSIZE ; j++) 
	en += pplot->energy[gr][ch][j];

      sprintf(title2,"FFT%1i  pe=%4.1fK  en=%5.2e ",gr,
	      pplot->pe[gr][ch]/1000,en);

      ymn = 3;
      ymx = 15;
      xmn = 1;
      xmx = n;
      gpk_bargraph_draw(enerbox[gr],n,xcord,ycord,
			xmn,ymn,xmx,ymx,1,title2,0,barcolor);
      
    }
  }else{
    /*******************************************************************
     * draw the psy model energy spectrum (scalefactor bands)
     *******************************************************************/
    for (gr = 0 ; gr < mode_gr ; gr ++) {

      if (blocktype[gr][ch]==2) {
	n = 3*SBMAX_s; 
	data = &pplot->en_s[gr][ch][0];
	data2 = &pplot->thr_s[gr][ch][0];
	data3 = &pplot->xfsf_s[gr][ch][0];
      } else {
	n = SBMAX_l; 
	data = &pplot->en[gr][ch][0];
	data2 = &pplot->thr[gr][ch][0];
	data3 = &pplot->xfsf[gr][ch][0];
      }
      ymn=9e20;
      ymx=-9e20;
      for (i=0; i<n; i++) {
	xcord[i] = i+1;
        if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
          ycord[i] = 0;
        else
	  ycord[i] = log10( MAX( data[i],(double) 1));
	ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
	ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
      }



      /* en = max energy difference amoung the 3 short FFTs for this granule */
      en = pplot->ers[gr][ch];
      sprintf(title2,"FFT%1i pe=%4.1fK/%3.1f n=%i/%3.1f/%3.1f/%3.1f",gr,
	      pplot->pe[gr][ch]/1000,en,pplot->over[gr][ch],
	      pplot->max_noise[gr][ch],
	      pplot->over_noise[gr][ch],
	      pplot->tot_noise[gr][ch]);


      ymn = 3;
      ymx = 15;
      xmn = 1;
      xmx = n+1; /* a little extra because of the bar thickness */
      gpk_bargraph_draw(enerbox[gr],n,xcord,ycord,
			xmn,ymn,xmx,ymx,1,title2,0,barcolor);


      for (i=0; i<n; i++) {
	xcord[i] = i+1;
        if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
          ycord[i] = 0;
        else
	  ycord[i] = log10( MAX( data3[i], (double) 1));
	ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
	ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
      }
      gpk_bargraph_draw(enerbox[gr],n,xcord,ycord,
			xmn,ymn,xmx,ymx,0,title2,3,&red);  

      
      for (i=0; i<n; i++) {
	xcord[i] = i+1 + (.25*n)/SBMAX_l;
        if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
          ycord[i] = 0;
        else
	  ycord[i] = log10( MAX( data2[i], (double) 1));
	ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
	ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
      }
      gpk_bargraph_draw(enerbox[gr],n,xcord,ycord,
			xmn,ymn,xmx,ymx,0,title2,3,grcolor[gr]);
    }
  }

  /*******************************************************************
   * draw scalefactors 
   *******************************************************************/
  for (gr = 0 ; gr < mode_gr ; gr ++) {
      double ggain;
      if (blocktype[gr][ch]==2) {
	n = 3*SBMAX_s; 
	if (gtkinfo.flag123) data = pplot1->sfb_s[gr][ch];
	else data = pplot->LAMEsfb_s[gr][ch];
      } else {
	n = SBMAX_l; 
	if (gtkinfo.flag123) data = pplot1->sfb[gr][ch];
	else data = pplot->LAMEsfb[gr][ch];
      }

      ymn=-1;
      ymx=10;
      for (i=0; i<n; i++) {
	xcord[i] = i+1;
        if (blocktype[gr][ch]==SHORT_TYPE && !subblock_draw[i % 3])
          ycord[i] = 0;
        else
	  ycord[i] = -data[i];
	ymx=(ycord[i] > ymx) ? ycord[i] : ymx;
	ymn=(ycord[i] < ymn) ? ycord[i] : ymn;
      }

      if (blocktype[gr][ch]==2) {
	sprintf(label2,
		"SFB scale=%i %i%i%i",
		pplot1->scalefac_scale[gr][ch],
		pplot1->sub_gain[gr][ch][0],
		pplot1->sub_gain[gr][ch][1],
		pplot1->sub_gain[gr][ch][2]);
      }else{
	sprintf(label2,"SFB scale=%i",pplot1->scalefac_scale[gr][ch]);
      }
      
      if (gtkinfo.flag123) ggain = -(pplot1->qss[gr][ch]-210)/4.0;
      else ggain = -(pplot->LAMEqss[gr][ch]-210)/4.0;

      sprintf(title2," gain=%4.1f",ggain);
      strcat(label2,title2);
      
      xmn = 1;
      xmx = n+1;
      gpk_bargraph_draw(sfbbox[gr],n,xcord,ycord,
			xmn,ymn,xmx,ymx,1,label2,0,grcolor[gr]);

      ycord[0] = ycord[1] = 0;
      xcord[0] = 1;
      xcord[1] = n+1;
      gpk_rectangle_draw(sfbbox[gr],xcord,ycord,xmn,ymn,xmx,ymx,&yellow);

      
    }


}



static void update_progress(void)
{    
  char label[80];
  int tf=gfp->totalframes;
  if (gtkinfo.totalframes>0) tf=gtkinfo.totalframes;

  sprintf(label,"Frame:%4i/%4i  %6.2fs",
	 pplot->frameNum,(int)tf-1, pplot->frametime);
  gtk_progress_set_value (GTK_PROGRESS (frameprogress), (gdouble) pplot->frameNum);
  gtk_label_set_text(GTK_LABEL(framecounter),label);
}



static void analyze(void)
{
    if ( idle_keepgoing) {
      idle_count = 0;
      idle_count_max=0;
      idle_keepgoing=0;
      idle_end=0;
    }
    plot_frame();   
    update_progress(); 
}

static void plotclick( GtkWidget *widget, gpointer   data )
{   analyze(); }




static int frameadv1(GtkWidget *widget, gpointer   data )
{
  int i;
  if (idle_keepgoing ){
    if (idle_back) {
      /* frame displayed is the old frame.  to advance, just swap in new frame */
      idle_back--;
      pplot = &Pinfo[READ_AHEAD+idle_back];
    }else{
      /* advance the frame by reading in a new frame */
      pplot = &Pinfo[READ_AHEAD];
      if (mp3done) { 
	/* dont try to read any more frames, and quit if "finish MP3" was selected */
	/*	if (idle_finish) gtk_main_quit(); */
	idle_count_max=0; 
        idle_end=0;
      } else {
	/* read in the next frame */
	for (i=NUMPINFO-1 ; i>0 ; i--)
	  memcpy(&Pinfo[i],&Pinfo[i-1],sizeof(plotting_data));
	pinfo = &Pinfo[0];
	pinfo->num_samples = gtkmakeframe();
	if (pinfo->num_samples==0 && gtkinfo.totalframes==0) 
	  /* allow an extra frame to flush decoder buffers */
	  gtkinfo.totalframes = pinfo->frameNum +2;

	if (pinfo->sampfreq) 
	  pinfo->frametime = (pinfo->frameNum)*1152.0/pinfo->sampfreq;
	else pinfo->frametime=0;

        /* eof? 
	if (!pinfo->num_samples) if (idle_finish) gtk_main_quit();
	*/

	pinfo->totbits = 0;
	{ int gr,ch;
	for (gr = 0 ; gr < 2 ; gr ++) 
	  for (ch = 0 ; ch < 2 ; ch ++) {
	    gtkinfo.totshort += (pinfo->mpg123blocktype[gr][ch]==2);
	    gtkinfo.totmix  += !(pinfo->mixed[gr][ch]==0);
	    pinfo->totbits += pinfo->mainbits[gr][ch];
	  }
	}
	if (pinfo->frameNum > 0) /* start averaging at second frame */
	  gtkinfo.avebits = (gtkinfo.avebits*((pinfo->frameNum)-1)
	  + pinfo->totbits ) /(pinfo->frameNum);

	gtkinfo.maxbits=MAX(gtkinfo.maxbits,pinfo->totbits);
	gtkinfo.totemph += !(pinfo->emph==0);
	gtkinfo.totms   += !(pinfo->ms_stereo==0);
	gtkinfo.totis   += !(pinfo->i_stereo==0);

	if (gtkinfo.totalframes>0)
	  if (pplot->frameNum >= gtkinfo.totalframes-1) mp3done=1;
      }
    }

    idle_count++;
    if (gtkinfo.pupdate) plot_frame();
    update_progress();
    if ((idle_count>=idle_count_max) && (! idle_end)) analyze();
  }
  return 1;
}


static void frameadv( GtkWidget *widget, gpointer   data )
{
    int adv;

    if (!strcmp((char *) data,"-1")) {
      /* ignore if we've already gone back as far as possible */
      if (pplot->frameNum==0 || (idle_back==NUMBACK)) return;  
      idle_back++;
      pplot = &Pinfo[READ_AHEAD+idle_back];
      analyze();
      return;
    }


    adv = 1;
    if (!strcmp((char *) data,"1")) adv = 1;
    if (!strcmp((char *) data,"10")) adv = 10;
    if (!strcmp((char *) data,"100")) adv = 100;
    if (!strcmp((char *) data,"finish")) idle_end = 1;


    if (idle_keepgoing) {
      /* already running - que up additional frame advance requests */
      idle_count_max += adv; 
    }
    else {
      /* turn on idleing */
      idle_count_max = adv;
      idle_count = 0;
      idle_keepgoing = 1;
    }
}




/* another callback */
static void delete_event( GtkWidget *widget,
                   GdkEvent  *event,
		   gpointer   data )
{
    gtk_main_quit ();
}







static void channel_option (GtkWidget *widget, gpointer data)
{
  long option;
  option = (long) data;
  switch (option) {
  case 1:
    gtkinfo.msflag=0;
    gtkinfo.chflag=0; 
    break;
  case 2:
    gtkinfo.msflag=0;
    gtkinfo.chflag=1; 
    break;
  case 3:
    gtkinfo.msflag=1;
    gtkinfo.chflag=0; 
    break;
  case 4:
    gtkinfo.msflag=1;
    gtkinfo.chflag=1; 
  }
  analyze();
}
static void spec_option (GtkWidget *widget, gpointer data)
{
  long option;
  option = (long) data;
  switch (option) {
  case 1:
    gtkinfo.kbflag=0;
    break;
  case 2:
    gtkinfo.kbflag=1;
    break;
  case 3:
    gtkinfo.flag123=0;
    break;
  case 4:
    gtkinfo.flag123=1;
    break;
  case 5:
    gtkinfo.pupdate=1;
    break;
  case 6:
    gtkinfo.pupdate=0;
    break;
  case 7:
    gtkinfo.sfblines = !gtkinfo.sfblines;
    break;
  }
  analyze();
}

static gint key_press_event (GtkWidget *widget, GdkEventKey *event)
{
  if (event->keyval == '1') {
    subblock_draw[0] = 1;
    subblock_draw[1] = 0;
    subblock_draw[2] = 0;
    analyze();
  }
  else if (event->keyval == '2') {
    subblock_draw[0] = 0;
    subblock_draw[1] = 1;
    subblock_draw[2] = 0;
    analyze();
  }
  else if (event->keyval == '3') {
    subblock_draw[0] = 0;
    subblock_draw[1] = 0;
    subblock_draw[2] = 1;
    analyze();
  }
  else if (event->keyval == '0') {
    subblock_draw[0] = 1;
    subblock_draw[1] = 1;
    subblock_draw[2] = 1;
    analyze();
  }
  /* analyze(); */  /* dont redraw entire window for every key! */
  return 0;
}



static void text_window (GtkWidget *widget, gpointer data)
{
  long option;
  GtkWidget *hbox,*vbox,*button,*box;
  GtkWidget *textwindow,*vscrollbar;
  char text[80];

  option = (long) data;
  
  textwindow = gtk_window_new(GTK_WINDOW_DIALOG);
  gtk_signal_connect_object (GTK_OBJECT (window), "delete_event",
		      GTK_SIGNAL_FUNC(gtk_widget_destroy),
		      GTK_OBJECT (textwindow));

  gtk_container_set_border_width (GTK_CONTAINER (textwindow), 0);
  vbox = gtk_vbox_new(FALSE,0);
  hbox = gtk_hbox_new(FALSE,0);

  button = gtk_button_new_with_label ("close");
  gtk_signal_connect_object (GTK_OBJECT (button), "clicked",
			     GTK_SIGNAL_FUNC(gtk_widget_destroy),
			     GTK_OBJECT (textwindow));

  box = gtk_text_new (NULL, NULL);
  gtk_text_set_editable (GTK_TEXT (box), FALSE);
  vscrollbar = gtk_vscrollbar_new (GTK_TEXT(box)->vadj);


  switch (option) {
  case 0: 
    gtk_window_set_title (GTK_WINDOW (textwindow), "Documentation");
    gtk_widget_set_usize(box,450,500); 
    gtk_text_set_word_wrap(GTK_TEXT(box),TRUE);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		"Frame header information: "\
		"First the bitrate, sampling frequency and mono, stereo or jstereo "\
		"indicators are displayed .  If the bitstream is jstereo, then mid/side "\
		"stereo or intensity stereo may be on (indicated in red).  If "\
		"de-emphasis is used, this is also indicated in red.  The mdb value is "\
		"main_data_begin.  The encoded data starts this many bytes *before* the "\
		"frame header.  A large value of mdb means the bitstream has saved some "\
		"bits into the reservoir, which it may allocate for some future frame. "\
		"The two numbers after mdb are the size (in bits) used to encode the "\
		"MDCT coefficients for this frame, followed byt the size of the bit "\
		"resevoir before encoding this frame.  The maximum frame size and a "\
		"running average are given in the Stats pull down menu.  A large "\
		"maximum frame size indicates the bitstream has made use of the bit "\
		"reservoir. \n\n",-1);

    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		"PCM data (top graph): "\
		"The PCM data is plotted in black.  The layer3 frame is divided into 2 "\
		"granules of 576 samples (marked with yellow vertical lines).  In the "\
		"case of normal, start and stop blocks, the MDCT coefficients for each "\
		"granule are computed using a 1152 sample window centered over the "\
		"granule.  In the case of short blocks, the granule is further divided "\
		"into 3 blocks of 192 samples (also marked with yellow vertical lines)."\
		"The MDCT coefficients for these blocks are computed using 384 sample "\
		"windows centered over the 192 sample window.  (This info not available "\
		"when analyzing .mp3 files.)  For the psycho-acoustic model, a windowed "\
		"FFT is computed for each granule.  The range of these windows "\
		"is denoted by the blue and green bars.\n\n",-1);

		gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		"PCM re-synthesis data (second graph): "\
		"Same as the PCM window described above.  The data displayed is the "\
		"result of encoding and then decoding the original sample. \n\n",-1);

		gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		"MDCT windows: "\
		"Shows the energy in the MDCT spectrum for granule 0 (left window) "\
		"and granule 1 (right window).  The text also shows the blocktype "\
		"used, the number of bits used to encode the coefficients and the "\
		"number of extra bits allocated from the reservoir.  The MDCT pull down "\
		"window will toggle between the original unquantized MDCT coefficients "\
		"and the compressed (quantized) coefficients.\n\n",-1); 
 
		gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		"FFT window: "\
		"The gray bars show the energy in the FFT spectrum used by the "\
		"psycho-acoustic model.  Granule 0 is in the left window, granule 1 in "\
		"the right window.  The green and blue bars show how much distortion is "\
		"allowable, as computed by the psycho-acoustic model. The red bars show "\
		"the actual distortion after encoding.  There is one FFT for each "\
		"granule, computed with a 1024 Hann window centered over the "\
		"appropriate granule.  (the range of this 1024 sample window is shown "\
		"by the blue and green bars in the PCM data window).  The Spectrum pull "\
		"down window will toggle between showing the energy in equally spaced "\
		"frequency domain and the scale factor bands used by layer3.  Finally, "\
		"the perceptual entropy, total energy and number of scalefactor bands "\
		"with audible distortion is shown.  (This info not available when "\
		"analyzing .mp3 files.)",-1);

    break;
  case 1:
	/* Set the about box information */
    gtk_window_set_title (GTK_WINDOW (textwindow), "About");
    gtk_widget_set_usize(box,350,260);

    sprintf(text,"LAME version %s \nwww.sulaco.org/mp3\n\n",get_lame_version());
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);

    sprintf(text,"psycho-acoustic model:  GPSYCHO version %s\n",get_psy_version());
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    
    sprintf(text,"frame analyzer: MP3x version %s\n\n",get_mp3x_version());
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
		    "decoder:  mpg123/mpglib  .59q  \nMichael Hipp (www.mpg123.de)\n\n",-1);
    
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,
    "Encoder, decoder & psy-models based on ISO\ndemonstration source. ",-1);
    break;

  case 2:
    gtk_window_set_title (GTK_WINDOW (textwindow), "Statistics");
    gtk_widget_set_usize(box,350,260);
    sprintf(text,"frames processed so far: %i \n",Pinfo[0].frameNum+1);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"granules processed so far: %i \n\n",4*(Pinfo[0].frameNum+1));
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"mean bits/frame (approximate): %i\n",
	    gtkinfo.approxbits);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"mean bits/frame (from LAME): %i\n",
	    4*Pinfo[0].mean_bits);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"bitsize of largest frame: %i \n",gtkinfo.maxbits);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"average bits/frame: %3.1f \n\n",gtkinfo.avebits);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"ms_stereo frames: %i \n",gtkinfo.totms);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"i_stereo frames: %i \n",gtkinfo.totis);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"de-emphasis frames: %i \n",gtkinfo.totemph);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"short block granules: %i \n",gtkinfo.totshort);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    sprintf(text,"mixed block granules: %i \n",gtkinfo.totmix);
    gtk_text_insert(GTK_TEXT(box),NULL,NULL,NULL,text,-1);
    break;
  }



  gtk_widget_show (vscrollbar);
  gtk_widget_show (box);
  gtk_widget_show (vbox);
  gtk_widget_show (hbox);
  gtk_widget_show (button);

  gtk_box_pack_start (GTK_BOX(hbox), box, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vscrollbar, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, TRUE, 0);
  gtk_box_pack_end (GTK_BOX (vbox), button, FALSE, TRUE, 0);
  gtk_container_add (GTK_CONTAINER (textwindow), vbox); 
  gtk_widget_show(textwindow);

}




/* #include <strings.h>*/


/* This is the GtkItemFactoryEntry structure used to generate new menus.
   Item 1: The menu path. The letter after the underscore indicates an
           accelerator key once the menu is open.
   Item 2: The accelerator key for the entry
   Item 3: The callback function.
   Item 4: The callback action.  This changes the parameters with
           which the function is called.  The default is 0.
   Item 5: The item type, used to define what kind of an item it is.
           Here are the possible values:

           NULL               -> "<Item>"
           ""                 -> "<Item>"
           "<Title>"          -> create a title item
           "<Item>"           -> create a simple item
           "<CheckItem>"      -> create a check item
           "<ToggleItem>"     -> create a toggle item
           "<RadioItem>"      -> create a radio item
           <path>             -> path of a radio item to link against
           "<Separator>"      -> create a separator
           "<Branch>"         -> create an item to hold sub items
           "<LastBranch>"     -> create a right justified branch 
*/

static GtkItemFactoryEntry menu_items[] = {
  {"/_File",         NULL,         NULL, 0, "<Branch>"},
  /*
  {"/File/_New",     "<control>N", print_hello, 0, NULL},
  {"/File/_Open",    "<control>O", print_hello, 0, NULL},
  {"/File/_Save",    "<control>S", print_hello, 0, NULL},
  {"/File/Save _As", NULL,         NULL, 0, NULL},
  {"/File/sep1",     NULL,         NULL, 0, "<Separator>"},
  {"/File/Quit",     "<control>Q", gtk_main_quit, 0, NULL}, 
  */
  {"/File/_Quit",     "<control>Q", delete_event, 0, NULL}, 

  {"/_Plotting",            NULL,         NULL,   0,    "<Branch>"},
  {"/Plotting/_While advancing" ,  NULL,  spec_option, 5, NULL},
  {"/Plotting/_After advancing",  NULL,  spec_option, 6, NULL},

  {"/_Channel",            NULL,         NULL,   0,    "<Branch>"},
  {"/Channel/show _Left" ,  NULL,  channel_option, 1, NULL},
  {"/Channel/show _Right",  NULL,  channel_option, 2, NULL},
  {"/Channel/show _Mid" ,   NULL,  channel_option, 3, NULL},
  {"/Channel/show _Side",   NULL,  channel_option, 4, NULL},

  {"/_Spectrum",                   NULL,  NULL, 0, "<Branch>"},
  {"/Spectrum/_Scalefactor bands",  NULL,  spec_option, 1, NULL},
  {"/Spectrum/_Wave number",        NULL,  spec_option, 2, NULL},

  {"/_MDCT",                         NULL,  NULL, 0, "<Branch>"},
  {"/MDCT/_Original",               NULL,  spec_option, 3, NULL},
  {"/MDCT/_Compressed",             NULL,  spec_option, 4, NULL},
  {"/MDCT/_Toggle SFB lines",       NULL,  spec_option, 7, NULL},

  {"/_Stats",                         NULL,  NULL, 0, "<Branch>"},
  {"/Stats/_Show",               NULL,  text_window, 2, NULL},

  {"/_Help",         NULL,         NULL, 0, "<LastBranch>"},
  {"/_Help/_Documentation",   NULL,   text_window, 0, NULL},
  {"/_Help/_About",           NULL,   text_window, 1, NULL},
};


static void get_main_menu(GtkWidget *window, GtkWidget ** menubar) {
  int nmenu_items = sizeof(menu_items) / sizeof(menu_items[0]);
  GtkItemFactory *item_factory;
  GtkAccelGroup *accel_group;

  accel_group = gtk_accel_group_new();

  /* This function initializes the item factory.
     Param 1: The type of menu - can be GTK_TYPE_MENU_BAR, GTK_TYPE_MENU,
              or GTK_TYPE_OPTION_MENU.
     Param 2: The path of the menu.
     Param 3: A pointer to a gtk_accel_group.  The item factory sets up
              the accelerator table while generating menus.
  */

  item_factory = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<main>", 
				       accel_group);

  /* This function generates the menu items. Pass the item factory,
     the number of items in the array, the array itself, and any
     callback data for the the menu items. */
  gtk_item_factory_create_items(item_factory, nmenu_items, menu_items, NULL);

  /* Attach the new accelerator group to the window. */
  gtk_accel_group_attach (accel_group, GTK_OBJECT (window));

  if (menubar)
    /* Finally, return the actual menu bar created by the item factory. */ 
    *menubar = gtk_item_factory_get_widget(item_factory, "<main>");
}




int gtkcontrol(lame_global_flags *gfp2)
{
    /* GtkWidget is the storage type for widgets */
    GtkWidget *button;
    GtkAdjustment *adj;
    GtkWidget *mbox;        /* main box */
    GtkWidget *box1;        /* frame control buttons go */
    GtkWidget *box2;        /* frame counters */
    GtkWidget *box3;        /* frame header info */
    GtkWidget *table;       /* table for all the plotting areas */
    GtkWidget *menubar;

    gint tableops,graphx,graphy;
    char frameinfo[80];

    graphx = 500;  /* minimum allowed size of pixmap */
    graphy = 95;

    gfp=gfp2;

    /* set some global defaults/variables */
    gtkinfo.filetype = (gfp->input_format == sf_mp3);
    gtkinfo.msflag=0;
    gtkinfo.chflag=0;
    gtkinfo.kbflag=0;
    gtkinfo.flag123 = (gfp->input_format == sf_mp3); /* MP3 file=use mpg123 output */
    gtkinfo.pupdate=0;
    gtkinfo.avebits = 0;
    gtkinfo.maxbits = 0;
    gtkinfo.approxbits = 0;
    gtkinfo.totemph = 0;
    gtkinfo.totms = 0;
    gtkinfo.totis = 0;
    gtkinfo.totshort = 0;
    gtkinfo.totmix = 0;
    gtkinfo.sfblines= 1;
    gtkinfo.totalframes = 0;

    memset((char *) Pinfo, 0, sizeof(Pinfo));
    pplot = &Pinfo[READ_AHEAD];

    strcpy(frameinfo,"MP3x: ");
    strncat(frameinfo,gfp->inPath,70);

    window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (window), frameinfo);
    gtk_signal_connect (GTK_OBJECT (window), "delete_event",
			GTK_SIGNAL_FUNC (delete_event), NULL);

    gtk_signal_connect_object (GTK_OBJECT (window), "key_press_event",
		      GTK_SIGNAL_FUNC(key_press_event),
		      GTK_OBJECT (window));

    gtk_container_set_border_width (GTK_CONTAINER (window), 0);


    mbox = gtk_vbox_new(FALSE, 0);


    /* layout of mbox */
    box1 = gtk_hbox_new(FALSE, 0);
    box2 = gtk_hbox_new(FALSE, 0);
    box3 = gtk_hbox_new(FALSE, 0);
    table = gtk_table_new (5, 2, FALSE);
    tableops = GTK_FILL | GTK_EXPAND | GTK_SHRINK;
    get_main_menu(window, &menubar);

    gtk_box_pack_start(GTK_BOX(mbox), menubar, FALSE, TRUE, 0);
    gtk_box_pack_end (GTK_BOX (mbox), box1, FALSE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX (mbox),box2, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX (mbox),box3, FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (mbox), table, TRUE, TRUE, 0);
    gtk_container_add (GTK_CONTAINER (window), mbox); 


    /*********************************************************************/
    /* stuff in box3  frame header info */
    /*********************************************************************/
    /*
    headerbox = gtk_label_new(" ");
    gtk_label_set_justify(GTK_LABEL(headerbox),GTK_JUSTIFY_LEFT); 
    */
    headerbox = gtk_text_new (NULL, NULL);
    gtk_text_set_editable (GTK_TEXT (headerbox), FALSE);
    gtk_widget_set_usize(headerbox,200,20);
    gtk_widget_show (headerbox);
    gtk_box_pack_start(GTK_BOX (box3),headerbox, TRUE, TRUE, 0);
    


    /*********************************************************************/
    /* stuff in box2   frame counters  */
    /*********************************************************************/
    framecounter = gtk_label_new("");
    gtk_widget_show(framecounter);
    gtk_box_pack_start(GTK_BOX (box2),framecounter, FALSE, TRUE, 0);

    adj = (GtkAdjustment *) gtk_adjustment_new (0, 0,(gint) gfp->totalframes-1, 0, 0, 0);
    frameprogress = gtk_progress_bar_new_with_adjustment (adj);
    /* Set the format of the string that can be displayed in the
     * trough of the progress bar:
     * %p - percentage
     * %v - value
     * %l - lower range value
     * %u - upper range value */
    gtk_progress_set_format_string (GTK_PROGRESS (frameprogress),
	                            "%p%%");
    gtk_progress_set_value (GTK_PROGRESS (frameprogress), (gdouble) 0);
    gtk_progress_set_show_text (GTK_PROGRESS (frameprogress),TRUE);
    gtk_widget_show (frameprogress);
    gtk_box_pack_end (GTK_BOX (box2), frameprogress, FALSE, TRUE, 0);



    /*********************************************************************/
    /* stuff in box1  buttons along bottom */
    /*********************************************************************/
    button = gtk_button_new_with_label ("-1");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (frameadv), (gpointer) "-1");
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label ("+1");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (frameadv), (gpointer) "1");
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label ("+10");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (frameadv), (gpointer) "10");
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label ("+100");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (frameadv), (gpointer) "100");
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label ("last frame");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (frameadv), (gpointer) "finish");
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label ("stop/plot");
    gtk_signal_connect (GTK_OBJECT (button), "clicked",
			GTK_SIGNAL_FUNC (plotclick), NULL);
    gtk_box_pack_start(GTK_BOX(box1), button, TRUE, TRUE, 0);
    gtk_widget_show(button);


    /*********************************************************************/
    /* stuff in table.  all the plotting windows */
    /*********************************************************************/
    pcmbox = gpk_plot_new(graphx,graphy);
    gtk_table_attach (GTK_TABLE(table),pcmbox,0,2,0,1,tableops,tableops,2,2 );
    gtk_widget_show (pcmbox);

    winbox = gpk_plot_new(graphy,graphy);
    gtk_table_attach(GTK_TABLE(table),winbox,0,2,1,2,tableops,tableops,2,2);
    gtk_widget_show (winbox);


    mdctbox[0] = gpk_plot_new(graphy,graphy);
    gtk_table_attach(GTK_TABLE(table),mdctbox[0],0,1,2,3,tableops,tableops,2,2);
    gtk_widget_show (mdctbox[0]);

    mdctbox[1] = gpk_plot_new(graphy,graphy);
    gtk_table_attach (GTK_TABLE(table),mdctbox[1],1,2,2,3,tableops,tableops,2,2);
    gtk_widget_show (mdctbox[1]);

    enerbox[0] = gpk_plot_new(graphy,graphy);
    gtk_table_attach(GTK_TABLE(table),enerbox[0],0,1,3,4,tableops,tableops,2,2);
    gtk_widget_show (enerbox[0]);

    enerbox[1] = gpk_plot_new(graphy,graphy);
    gtk_table_attach (GTK_TABLE(table),enerbox[1],1,2,3,4,tableops,tableops,2,2);
    gtk_widget_show (enerbox[1]);

    sfbbox[0] = gpk_plot_new(graphy,graphy);
    gtk_table_attach(GTK_TABLE(table),sfbbox[0],0,1,4,5,tableops,tableops,2,2);
    gtk_widget_show (sfbbox[0]);

    sfbbox[1] = gpk_plot_new(graphy,graphy);
    gtk_table_attach (GTK_TABLE(table),sfbbox[1],1,2,4,5,tableops,tableops,2,2);
    gtk_widget_show (sfbbox[1]);




    gtk_idle_add((GtkFunction) frameadv1, NULL);
    gtk_widget_show(menubar); 
    gtk_widget_show(box2); 
    gtk_widget_show(box3); 
    gtk_widget_show(table);
    gtk_widget_show(box1);
    gtk_widget_show (mbox);
    gtk_widget_show (window);     /* show smallest allowed window */

    /* make window bigger.   */ 
    /* now the user will be able to shrink it, if desired */
    /* gtk_widget_set_usize(mbox,500,500);  */
    /* gtk_widget_show (window); */     /* show smallest allowed window */


    
    idle_keepgoing=1;             /* processing of frames is ON */
    idle_count_max=READ_AHEAD+1;  /* number of frames to process before plotting */
    idle_count=0;                 /* pause & plot when idle_count=idle_count_max */


    gtk_main ();
    if (!mp3done) exit(2);
    return(0);
}

#endif












/* ==== id3tag.c ==== */
/*
 * functions for writing ID3 tags in LAME
 *
 * text functions stolen from mp3info by Ricardo Cerqueira <rmc@rccn.net>
 * adapted for LAME by Conrad Sanderson <c.sanderson@me.gu.edu.au>
 *
 * 
 */ 
 
#include <stdio.h>
#include <string.h>
#include "id3tag.h"
ID3TAGDATA id3tag;
 
/*
 * If "string" is shorter than "length", pad it with ' ' (spaces)
 */

static void id3_pad(char *string, int length) {
	int l;  l=strlen(string);
	
	while(l<length) { string[l] = ' '; l++; }
	string[l]='\0';
	}


/*
 * initialize temporary fields
 */

void id3_inittag(ID3TAGDATA *tag) {
	strcpy( tag->title, "");
	strcpy( tag->artist, "");
	strcpy( tag->album, "");
	strcpy( tag->year, "");    
	strcpy( tag->comment, "");
	strcpy( tag->genre, "ÿ");	/* unset genre */
	tag->track = 0;

	tag->valid = 0;		/* not ready for writing*/
	}

/*
 * build an ID3 tag from temporary fields
 */

void id3_buildtag(ID3TAGDATA *tag) {
	strcpy(tag->tagtext,"TAG");

	id3_pad( tag->title, 30);   strncat( tag->tagtext, tag->title,30);
	id3_pad( tag->artist, 30);  strncat( tag->tagtext, tag->artist,30);
	id3_pad( tag->album, 30);   strncat( tag->tagtext, tag->album,30);
	id3_pad( tag->year, 4);     strncat( tag->tagtext, tag->year,4);
	id3_pad( tag->comment, 30); strncat( tag->tagtext, tag->comment,30);
	id3_pad( tag->genre, 1);    strncat( tag->tagtext, tag->genre,1);

	if( tag->track != 0 ) {
		tag->tagtext[125] = '\0';
		tag->tagtext[126] = tag->track;
	}
	tag->valid = 1;		/* ready for writing*/
	}

/*
 * write ID3 tag 
 */

int id3_writetag(char* filename, ID3TAGDATA *tag) {
	FILE* f;
	if( ! tag->valid ) return -1;

	f=fopen(filename,"rb+");	if(!f) return -1;

	fseek(f,0,SEEK_END); fwrite(tag->tagtext,1,128,f);
	fclose(f); return 0;
	}





int genre_last=147;
char *genre_list[]={
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
	"Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
	"Other", "Pop", "R&B", "Rap", "Reggae", "Rock",
	"Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
	"Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
	"Fusion", "Trance", "Classical", "Instrumental", "Acid", "House",
	"Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass",
	"Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
	"Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
	"Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
	"Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret",
	"New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi",
	"Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical",
	"Rock & Roll", "Hard Rock", "Folk", "Folk/Rock", "National Folk", "Swing",
	"Fast-Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
	"Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band",
	"Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson",
	"Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus",
	"Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
	"Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet",
	"Punk Rock", "Drum Solo", "A capella", "Euro-House", "Dance Hall",
	"Goa", "Drum & Bass", "Club House", "Hardcore", "Terror",
	"Indie", "BritPop", "NegerPunk", "Polsk Punk", "Beat",
	"Christian Gangsta", "Heavy Metal", "Black Metal", "Crossover", "Contemporary C",
	"Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
	"SynthPop",
};




/* ==== ieeefloat.c ==== */
/* Copyright (C) 1988-1991 Apple Computer, Inc.
 * All Rights Reserved.
 *
 * Warranty Information
 * Even though Apple has reviewed this software, Apple makes no warranty
 * or representation, either express or implied, with respect to this
 * software, its quality, accuracy, merchantability, or fitness for a 
 * particular purpose.  As a result, this software is provided "as is,"
 * and you, its user, are assuming the entire risk as to its quality
 * and accuracy.
 *
 * This code may be used and freely distributed as long as it includes
 * this copyright notice and the warranty information.
 *
 * Machine-independent I/O routines for IEEE floating-point numbers.
 *
 * NaN's and infinities are converted to HUGE_VAL or HUGE, which
 * happens to be infinity on IEEE machines.  Unfortunately, it is
 * impossible to preserve NaN's in a machine-independent way.
 * Infinities are, however, preserved on IEEE machines.
 *
 * These routines have been tested on the following machines:
 *	Apple Macintosh, MPW 3.1 C compiler
 *	Apple Macintosh, THINK C compiler
 *	Silicon Graphics IRIS, MIPS compiler
 *	Cray X/MP and Y/MP
 *	Digital Equipment VAX
 *	Sequent Balance (Multiprocesor 386)
 *	NeXT
 *
 *
 * Implemented by Malcolm Slaney and Ken Turkowski.
 *
 * Malcolm Slaney contributions during 1988-1990 include big- and little-
 * endian file I/O, conversion to and from Motorola's extended 80-bit
 * floating-point format, and conversions to and from IEEE single-
 * precision floating-point format.
 *
 * In 1991, Ken Turkowski implemented the conversions to and from
 * IEEE double-precision format, added more precision to the extended
 * conversions, and accommodated conversions involving +/- infinity,
 * NaN's, and denormalized numbers.
 *
 * $Id: ieeefloat.c,v 1.3 2000/02/21 23:05:05 markt Exp $
 *
 * $Log: ieeefloat.c,v $
 * Revision 1.3  2000/02/21 23:05:05  markt
 * some 64bit DEC Alpha patches
 *
 * Revision 1.2  2000/02/19 13:32:30  afaber
 * Fixed many warning messages when compiling with MSVC
 *
 * Revision 1.1.1.1  1999/11/24 08:42:58  markt
 * initial checkin of LAME
 * Starting with LAME 3.57beta with some modifications
 *
 * Revision 1.1  1993/06/11  17:45:46  malcolm
 * Initial revision
 *
 */

#include        <limits.h>
#include	<stdio.h>
#include	<math.h>
#include	"ieeefloat.h"


/****************************************************************
 * The following two routines make up for deficiencies in many
 * compilers to convert properly between unsigned integers and
 * floating-point.  Some compilers which have this bug are the
 * THINK_C compiler for the Macintosh and the C compiler for the
 * Silicon Graphics MIPS-based Iris.
 ****************************************************************/

#ifdef applec	/* The Apple C compiler works */
# define FloatToUnsigned(f)	((unsigned long)(f))
# define UnsignedToFloat(u)	((defdouble)(u))
#else /* applec */
# define FloatToUnsigned(f)	((unsigned long)(((long)((f) - 2147483648.0)) + 2147483647L + 1))
# define UnsignedToFloat(u)	(((defdouble)((long)((u) - 2147483647L - 1))) + 2147483648.0)
#endif /* applec */


/****************************************************************
 * Single precision IEEE floating-point conversion routines
 ****************************************************************/

#define SEXP_MAX		255
#define SEXP_OFFSET		127
#define SEXP_SIZE		8
#define SEXP_POSITION	(32-SEXP_SIZE-1)


defdouble
ConvertFromIeeeSingle(char* bytes)
{
	defdouble	f;
	long	mantissa, expon;
	long	bits;

	bits =	((unsigned long)(bytes[0] & 0xFF) << 24)
		|	((unsigned long)(bytes[1] & 0xFF) << 16)
		|	((unsigned long)(bytes[2] & 0xFF) << 8)
		|	 (unsigned long)(bytes[3] & 0xFF);		/* Assemble bytes into a long */

	if ((bits & 0x7FFFFFFF) == 0) {
		f = 0;
	}

	else {
		expon = (bits & 0x7F800000) >> SEXP_POSITION;
		if (expon == SEXP_MAX) {		/* Infinity or NaN */
			f = HUGE_VAL;		/* Map NaN's to infinity */
		}
		else {
			if (expon == 0) {	/* Denormalized number */
				mantissa = (bits & 0x7fffff);
				f = ldexp((defdouble) mantissa, (int) (expon - SEXP_OFFSET - SEXP_POSITION + 1));
			}
			else {				/* Normalized number */
				mantissa = (bits & 0x7fffff) + 0x800000;	/* Insert hidden bit */
				f = ldexp((defdouble) mantissa, (int) (expon - SEXP_OFFSET - SEXP_POSITION));
			}
		}
	}

	if (bits & LONG_MIN)
		return -f;
	else
		return f;
}


/****************************************************************/


void
ConvertToIeeeSingle(defdouble num, char* bytes)
{
	long	sign;
	register long bits;

	if (num < 0) {	/* Can't distinguish a negative zero */
		sign = LONG_MIN;
		num *= -1;
	} else {
		sign = 0;
	}

	if (num == 0) {
		bits = 0;
	}

	else {
		defdouble fMant;
		int expon;

		fMant = frexp(num, &expon);

		if ((expon > (SEXP_MAX-SEXP_OFFSET+1)) || !(fMant < 1)) {
			/* NaN's and infinities fail second test */
			bits = sign | 0x7F800000;		/* +/- infinity */
		}

		else {
			long mantissa;

			if (expon < -(SEXP_OFFSET-2)) {	/* Smaller than normalized */
				int shift = (SEXP_POSITION+1) + (SEXP_OFFSET-2) + expon;
				if (shift < 0) {	/* Way too small: flush to zero */
					bits = sign;
				}
				else {			/* Nonzero denormalized number */
					mantissa = (long)(fMant * (1L << shift));
					bits = sign | mantissa;
				}
			}

			else {				/* Normalized number */
				mantissa = (long)floor(fMant * (1L << (SEXP_POSITION+1)));
				mantissa -= (1L << SEXP_POSITION);			/* Hide MSB */
				bits = sign | ((long)((expon + SEXP_OFFSET - 1)) << SEXP_POSITION) | mantissa;
			}
		}
	}

	bytes[0] = (char)(bits >> 24);	/* Copy to byte string */
	bytes[1] = (char)(bits >> 16);
	bytes[2] = (char)(bits >> 8);
	bytes[3] = (char)(bits);
}


/****************************************************************
 * Double precision IEEE floating-point conversion routines
 ****************************************************************/

#define DEXP_MAX		2047
#define DEXP_OFFSET		1023
#define DEXP_SIZE		11
#define DEXP_POSITION	(32-DEXP_SIZE-1)


defdouble
ConvertFromIeeeDouble(char* bytes)
{
	defdouble	f;
	long	mantissa, expon;
	unsigned long first, second;

	first = ((unsigned long)(bytes[0] & 0xFF) << 24)
		|	((unsigned long)(bytes[1] & 0xFF) << 16)
		|	((unsigned long)(bytes[2] & 0xFF) << 8)
		|	 (unsigned long)(bytes[3] & 0xFF);
	second= ((unsigned long)(bytes[4] & 0xFF) << 24)
		|	((unsigned long)(bytes[5] & 0xFF) << 16)
		|	((unsigned long)(bytes[6] & 0xFF) << 8)
		|	 (unsigned long)(bytes[7] & 0xFF);
	
	if (first == 0 && second == 0) {
		f = 0;
	}

	else {
		expon = (first & 0x7FF00000) >> DEXP_POSITION;
		if (expon == DEXP_MAX) {		/* Infinity or NaN */
			f = HUGE_VAL;		/* Map NaN's to infinity */
		}
		else {
			if (expon == 0) {	/* Denormalized number */
				mantissa = (first & 0x000FFFFF);
				f = ldexp((defdouble) mantissa, (int) (expon - DEXP_OFFSET - DEXP_POSITION + 1));
				f += ldexp(UnsignedToFloat(second), (int) (expon - DEXP_OFFSET - DEXP_POSITION + 1 - 32));
			}
			else {				/* Normalized number */
				mantissa = (first & 0x000FFFFF) + 0x00100000;	/* Insert hidden bit */
				f = ldexp((defdouble) mantissa, (int) (expon - DEXP_OFFSET - DEXP_POSITION));
				f += ldexp(UnsignedToFloat(second), (int) (expon - DEXP_OFFSET - DEXP_POSITION - 32));
			}
		}
	}

	if (first & 0x80000000)
		return -f;
	else
		return f;
}


/****************************************************************/


void
ConvertToIeeeDouble(defdouble num, char *bytes)
{
	long	sign;
	long	first, second;

	if (num < 0) {	/* Can't distinguish a negative zero */
		sign = LONG_MIN;
		num *= -1;
	} else {
		sign = 0;
	}

	if (num == 0) {
		first = 0;
		second = 0;
	}

	else {
		defdouble fMant, fsMant;
		int expon;

		fMant = frexp(num, &expon);

		if ((expon > (DEXP_MAX-DEXP_OFFSET+1)) || !(fMant < 1)) {
			/* NaN's and infinities fail second test */
			first = sign | 0x7FF00000;		/* +/- infinity */
			second = 0;
		}

		else {
			long mantissa;

			if (expon < -(DEXP_OFFSET-2)) {	/* Smaller than normalized */
				int shift = (DEXP_POSITION+1) + (DEXP_OFFSET-2) + expon;
				if (shift < 0) {	/* Too small for something in the MS word */
					first = sign;
					shift += 32;
					if (shift < 0) {	/* Way too small: flush to zero */
						second = 0;
					}
					else {			/* Pretty small demorn */
						second = FloatToUnsigned(floor(ldexp(fMant, shift)));
					}
				}
				else {			/* Nonzero denormalized number */
					fsMant = ldexp(fMant, shift);
					mantissa = (long)floor(fsMant);
					first = sign | mantissa;
					second = FloatToUnsigned(floor(ldexp(fsMant - mantissa, 32)));
				}
			}

			else {				/* Normalized number */
				fsMant = ldexp(fMant, DEXP_POSITION+1);
				mantissa = (long)floor(fsMant);
				mantissa -= (1L << DEXP_POSITION);			/* Hide MSB */
				fsMant -= (1L << DEXP_POSITION);
				first = sign | ((long)((expon + DEXP_OFFSET - 1)) << DEXP_POSITION) | mantissa;
				second = FloatToUnsigned(floor(ldexp(fsMant - mantissa, 32)));
			}
		}
	}
	
	bytes[0] = (char)(first >> 24);
	bytes[1] = (char)(first >> 16);
	bytes[2] = (char)(first >> 8);
	bytes[3] = (char)(first);
	bytes[4] = (char)(second >> 24);
	bytes[5] = (char)(second >> 16);
	bytes[6] = (char)(second >> 8);
	bytes[7] = (char)(second);
}


/****************************************************************
 * Extended precision IEEE floating-point conversion routines
 ****************************************************************/

defdouble
ConvertFromIeeeExtended(char* bytes)
{
	defdouble	f;
	long	expon;
	unsigned long hiMant, loMant;

#ifdef	TEST	
printf("ConvertFromIEEEExtended(%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx\r",
	(long)bytes[0], (long)bytes[1], (long)bytes[2], (long)bytes[3], 
	(long)bytes[4], (long)bytes[5], (long)bytes[6], 
	(long)bytes[7], (long)bytes[8], (long)bytes[9]);
#endif
	
	expon = ((bytes[0] & 0x7F) << 8) | (bytes[1] & 0xFF);
	hiMant	=	((unsigned long)(bytes[2] & 0xFF) << 24)
			|	((unsigned long)(bytes[3] & 0xFF) << 16)
			|	((unsigned long)(bytes[4] & 0xFF) << 8)
			|	((unsigned long)(bytes[5] & 0xFF));
	loMant	=	((unsigned long)(bytes[6] & 0xFF) << 24)
			|	((unsigned long)(bytes[7] & 0xFF) << 16)
			|	((unsigned long)(bytes[8] & 0xFF) << 8)
			|	((unsigned long)(bytes[9] & 0xFF));

	if (expon == 0 && hiMant == 0 && loMant == 0) {
		f = 0;
	}
	else {
		if (expon == 0x7FFF) {	/* Infinity or NaN */
			f = HUGE_VAL;
		}
		else {
			expon -= 16383;
			f  = ldexp(UnsignedToFloat(hiMant), (int) (expon -= 31));
			f += ldexp(UnsignedToFloat(loMant), (int) (expon -= 32));
		}
	}

	if (bytes[0] & 0x80)
		return -f;
	else
		return f;
}


/****************************************************************/


void
ConvertToIeeeExtended(defdouble num, char *bytes)
{
	int	sign;
	int expon;
	defdouble fMant, fsMant;
	unsigned long hiMant, loMant;

	if (num < 0) {
		sign = 0x8000;
		num *= -1;
	} else {
		sign = 0;
	}

	if (num == 0) {
		expon = 0; hiMant = 0; loMant = 0;
	}
	else {
		fMant = frexp(num, &expon);
		if ((expon > 16384) || !(fMant < 1)) {	/* Infinity or NaN */
			expon = sign|0x7FFF; hiMant = 0; loMant = 0; /* infinity */
		}
		else {	/* Finite */
			expon += 16382;
			if (expon < 0) {	/* denormalized */
				fMant = ldexp(fMant, expon);
				expon = 0;
			}
			expon |= sign;
			fMant = ldexp(fMant, 32);          fsMant = floor(fMant); hiMant = FloatToUnsigned(fsMant);
			fMant = ldexp(fMant - fsMant, 32); fsMant = floor(fMant); loMant = FloatToUnsigned(fsMant);
		}
	}
	
	bytes[0] = expon >> 8;
	bytes[1] = expon;
	bytes[2] = (char)(hiMant >> 24);
	bytes[3] = (char)(hiMant >> 16);
	bytes[4] = (char)(hiMant >> 8);
	bytes[5] = (char)(hiMant);
	bytes[6] = (char)(loMant >> 24);
	bytes[7] = (char)(loMant >> 16);
	bytes[8] = (char)(loMant >> 8);
	bytes[9] = (char)(loMant);
}

/****************************************************************
 * Testing routines for the floating-point conversions.
 ****************************************************************/

#ifdef METROWERKS
#define IEEE
#endif
#ifdef applec
# define IEEE
#endif /* applec */
#ifdef THINK_C
# define IEEE
#endif /* THINK_C */
#ifdef sgi
# define IEEE
#endif /* sgi */
#ifdef sequent
# define IEEE
# define LITTLE_ENDIAN
#endif /* sequent */
#ifdef sun
# define IEEE
#endif /* sun */
#ifdef NeXT
# define IEEE
#endif /* NeXT */

#ifdef MAIN

union SParts {
	Single s;
	long i;
};
union DParts {
	Double d;
	long i[2];
};
union EParts {
	defdouble e;
	short i[6];
};


int
GetHexValue(register int x)
{
	x &= 0x7F;
	
	if ('0' <= x && x <= '9')
		x -= '0';
	else if ('a' <= x && x <= 'f')
		x = x - 'a' + 0xA;
	else if ('A' <= x && x <= 'F')
		x = x - 'A' + 0xA;
	else
		x = 0;
	
	return(x);
}


void
Hex2Bytes(register char *hex, register char *bytes)
{
	for ( ; *hex; hex += 2) {
		*bytes++ = (GetHexValue(hex[0]) << 4) | GetHexValue(hex[1]);
		if (hex[1] == 0)
			break;	/* Guard against odd bytes */
	}
}


int
GetHexSymbol(register int x)
{
	x &= 0xF;
	if (x <= 9)
		x += '0';
	else
		x += 'A' - 0xA;
	return(x);
}


void
Bytes2Hex(register char *bytes, register char *hex, register int nBytes)
{
	for ( ; nBytes--; bytes++) {
		*hex++ = GetHexSymbol(*bytes >> 4);
		*hex++ = GetHexSymbol(*bytes);
	}
	*hex = 0;
}


void
MaybeSwapBytes(char* bytes, int nBytes)
{
#ifdef LITTLE_ENDIAN
	register char *p, *q, t;
	for (p = bytes, q = bytes+nBytes-1; p < q; p++, q--) {
		t = *p;
		*p = *q;
		*q = t;
	}
#else
	if (bytes, nBytes);		/* Just so it's used */
#endif /* LITTLE_ENDIAN */
	
}


float
MachineIEEESingle(char* bytes)
{
	float t;
	MaybeSwapBytes(bytes, 4);
	t = *((float*)(bytes));
	MaybeSwapBytes(bytes, 4);
	return (t);
}


Double
MachineIEEEDouble(char* bytes)
{
	Double t;
	MaybeSwapBytes(bytes, 8);
	t = *((Double*)(bytes));
	MaybeSwapBytes(bytes, 8);
	return (t);
}


void
TestFromIeeeSingle(char *hex)
{
	defdouble f;
	union SParts p;
	char bytes[4];

	Hex2Bytes(hex, bytes);
	f = ConvertFromIeeeSingle(bytes);
	p.s = f;

#ifdef IEEE
	fprintf(stderr, "IEEE(%g) [%s] --> float(%g) [%08lX]\n",
	MachineIEEESingle(bytes),
	hex, f, p.i);
#else /* IEEE */
	fprintf(stderr, "IEEE[%s] --> float(%g) [%08lX]\n", hex, f, p.i);
#endif /* IEEE */
}


void
TestToIeeeSingle(defdouble f)
{
	union SParts p;
	char bytes[4];
	char hex[8+1];

	p.s = f;

	ConvertToIeeeSingle(f, bytes);
	Bytes2Hex(bytes, hex, 4);
#ifdef IEEE
	fprintf(stderr, "float(%g) [%08lX] --> IEEE(%g) [%s]\n",
		f, p.i,
		MachineIEEESingle(bytes),
		hex
	);
#else /* IEEE */
	fprintf(stderr, "float(%g) [%08lX] --> IEEE[%s]\n", f, p.i, hex);
#endif /* IEEE */
}


void
TestFromIeeeDouble(char *hex)
{
	defdouble f;
	union DParts p;
	char bytes[8];
	
	Hex2Bytes(hex, bytes);
	f = ConvertFromIeeeDouble(bytes);
	p.d = f;

#ifdef IEEE
	fprintf(stderr, "IEEE(%g) [%.8s %.8s] --> double(%g) [%08lX %08lX]\n",
	MachineIEEEDouble(bytes),
	hex, hex+8, f, p.i[0], p.i[1]);
#else /* IEEE */
	fprintf(stderr, "IEEE[%.8s %.8s] --> double(%g) [%08lX %08lX]\n",
		hex, hex+8, f, p.i[0], p.i[1]);
#endif /* IEEE */

}

void
TestToIeeeDouble(defdouble f)
{
	union DParts p;
	char bytes[8];
	char hex[16+1];

	p.d = f;

	ConvertToIeeeDouble(f, bytes);
	Bytes2Hex(bytes, hex, 8);
#ifdef IEEE
	fprintf(stderr, "double(%g) [%08lX %08lX] --> IEEE(%g) [%.8s %.8s]\n",
		f, p.i[0], p.i[1],
		MachineIEEEDouble(bytes),
		hex, hex+8
	);
#else /* IEEE */
	fprintf(stderr, "double(%g) [%08lX %08lX] --> IEEE[%.8s %.8s]\n",
		f, p.i[0], p.i[1], hex, hex+8
	);
#endif /* IEEE */

}


void
TestFromIeeeExtended(char *hex)
{
	defdouble f;
	union EParts p;
	char bytes[12];

	Hex2Bytes(hex, bytes);
	f = ConvertFromIeeeExtended(bytes);
	p.e = f;

	bytes[11] = bytes[9];
	bytes[10] = bytes[8];
	bytes[9] = bytes[7];
	bytes[8] = bytes[6];
	bytes[7] = bytes[5];
	bytes[6] = bytes[4];
	bytes[5] = bytes[3];
	bytes[4] = bytes[2];
	bytes[3] = 0;
	bytes[2] = 0;

#if defined(applec) || defined(THINK_C) || defined(METROWERKS)
	fprintf(stderr, "IEEE(%g) [%.4s %.8s %.8s] --> extended(%g) [%04X %04X%04X %04X%04X]\n",
		*((defdouble*)(bytes)),
		hex, hex+4, hex+12, f,
		p.i[0]&0xFFFF, p.i[2]&0xFFFF, p.i[3]&0xFFFF, p.i[4]&0xFFFF, p.i[5]&0xFFFF
	);
#else /* !Macintosh */
	fprintf(stderr, "IEEE[%.4s %.8s %.8s] --> extended(%g) [%04X %04X%04X %04X%04X]\n",
		hex, hex+4, hex+12, f,
		p.i[0]&0xFFFF, p.i[2]&0xFFFF, p.i[3]&0xFFFF, p.i[4]&0xFFFF, p.i[5]&0xFFFF
	);
#endif /* Macintosh */
}


void
TestToIeeeExtended(defdouble f)
{
	char bytes[12];
	char hex[24+1];

	ConvertToIeeeExtended(f, bytes);
	Bytes2Hex(bytes, hex, 10);

	bytes[11] = bytes[9];
	bytes[10] = bytes[8];
	bytes[9] = bytes[7];
	bytes[8] = bytes[6];
	bytes[7] = bytes[5];
	bytes[6] = bytes[4];
	bytes[5] = bytes[3];
	bytes[4] = bytes[2];
	bytes[3] = 0;
	bytes[2] = 0;

#if defined(applec) || defined(THINK_C) || defined(METROWERKS)
	fprintf(stderr, "extended(%g) --> IEEE(%g) [%.4s %.8s %.8s]\n",
		f, *((defdouble*)(bytes)),
		hex, hex+4, hex+12
	);
#else /* !Macintosh */
	fprintf(stderr, "extended(%g) --> IEEE[%.4s %.8s %.8s]\n",
		f,
		hex, hex+4, hex+12
	);
#endif /* Macintosh */
}

#include	<signal.h>

void SignalFPE(int i, void (*j)())
{
	printf("[Floating Point Interrupt Caught.]\n", i, j);
	signal(SIGFPE, SignalFPE);
}
	
void
main(void)
{
	long d[3];
	char bytes[12];

	signal(SIGFPE, SignalFPE);

	TestFromIeeeSingle("00000000");
	TestFromIeeeSingle("80000000");
	TestFromIeeeSingle("3F800000");
	TestFromIeeeSingle("BF800000");
	TestFromIeeeSingle("40000000");
	TestFromIeeeSingle("C0000000");
	TestFromIeeeSingle("7F800000");
	TestFromIeeeSingle("FF800000");
	TestFromIeeeSingle("00800000");
	TestFromIeeeSingle("00400000");
	TestFromIeeeSingle("00000001");
	TestFromIeeeSingle("80000001");
	TestFromIeeeSingle("3F8FEDCB");
	TestFromIeeeSingle("7FC00100");	/* Quiet NaN(1) */
	TestFromIeeeSingle("7F800100");	/* Signalling NaN(1) */

	TestToIeeeSingle(0.0);
	TestToIeeeSingle(-0.0);
	TestToIeeeSingle(1.0);
	TestToIeeeSingle(-1.0);
	TestToIeeeSingle(2.0);
	TestToIeeeSingle(-2.0);
	TestToIeeeSingle(3.0);
	TestToIeeeSingle(-3.0);
#if !(defined(sgi) || defined(NeXT))
	TestToIeeeSingle(HUGE_VAL);
	TestToIeeeSingle(-HUGE_VAL);
#endif

#ifdef IEEE
	/* These only work on big-endian IEEE machines */
	d[0] = 0x00800000L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));		/* Smallest normalized */
	d[0] = 0x00400000L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));		/* Almost largest denormalized */
	d[0] = 0x00000001L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));		/* Smallest denormalized */
	d[0] = 0x00000001L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])) * 0.5);	/* Smaller than smallest denorm */
	d[0] = 0x3F8FEDCBL; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));
#if !(defined(sgi) || defined(NeXT))
	d[0] = 0x7FC00100L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));		/* Quiet NaN(1) */
	d[0] = 0x7F800100L; MaybeSwapBytes(d,4); TestToIeeeSingle(*((float*)(&d[0])));		/* Signalling NaN(1) */
#endif /* sgi */
#endif /* IEEE */



	TestFromIeeeDouble("0000000000000000");
	TestFromIeeeDouble("8000000000000000");
	TestFromIeeeDouble("3FF0000000000000");
	TestFromIeeeDouble("BFF0000000000000");
	TestFromIeeeDouble("4000000000000000");
	TestFromIeeeDouble("C000000000000000");
	TestFromIeeeDouble("7FF0000000000000");
	TestFromIeeeDouble("FFF0000000000000");
	TestFromIeeeDouble("0010000000000000");
	TestFromIeeeDouble("0008000000000000");
	TestFromIeeeDouble("0000000000000001");
	TestFromIeeeDouble("8000000000000001");
	TestFromIeeeDouble("3FFFEDCBA9876543");
	TestFromIeeeDouble("7FF8002000000000");	/* Quiet NaN(1) */
	TestFromIeeeDouble("7FF0002000000000");	/* Signalling NaN(1) */

	TestToIeeeDouble(0.0);
	TestToIeeeDouble(-0.0);
	TestToIeeeDouble(1.0);
	TestToIeeeDouble(-1.0);
	TestToIeeeDouble(2.0);
	TestToIeeeDouble(-2.0);
	TestToIeeeDouble(3.0);
	TestToIeeeDouble(-3.0);
#if !(defined(sgi) || defined(NeXT))
	TestToIeeeDouble(HUGE_VAL);
	TestToIeeeDouble(-HUGE_VAL);
#endif

#ifdef IEEE
	/* These only work on big-endian IEEE machines */
	Hex2Bytes("0010000000000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Smallest normalized */
	Hex2Bytes("0010000080000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Normalized, problem with unsigned */
	Hex2Bytes("0008000000000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Almost largest denormalized */
	Hex2Bytes("0000000080000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Denorm problem with unsigned */
	Hex2Bytes("0000000000000001", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Smallest denormalized */
	Hex2Bytes("0000000000000001", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)) * 0.5);	/* Smaller than smallest denorm */
	Hex2Bytes("3FFFEDCBA9876543", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* accuracy test */
#if !(defined(sgi) || defined(NeXT))
	Hex2Bytes("7FF8002000000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Quiet NaN(1) */
	Hex2Bytes("7FF0002000000000", bytes); MaybeSwapBytes(d,8); TestToIeeeDouble(*((Double*)(bytes)));	/* Signalling NaN(1) */
#endif /* sgi */
#endif /* IEEE */

	TestFromIeeeExtended("00000000000000000000");	/* +0 */
	TestFromIeeeExtended("80000000000000000000");	/* -0 */
	TestFromIeeeExtended("3FFF8000000000000000");	/* +1 */
	TestFromIeeeExtended("BFFF8000000000000000");	/* -1 */
	TestFromIeeeExtended("40008000000000000000");	/* +2 */
	TestFromIeeeExtended("C0008000000000000000");	/* -2 */
	TestFromIeeeExtended("7FFF0000000000000000");	/* +infinity */
	TestFromIeeeExtended("FFFF0000000000000000");	/* -infinity */
	TestFromIeeeExtended("7FFF8001000000000000");	/* Quiet NaN(1) */
	TestFromIeeeExtended("7FFF0001000000000000");	/* Signalling NaN(1) */
	TestFromIeeeExtended("3FFFFEDCBA9876543210");	/* accuracy test */

	TestToIeeeExtended(0.0);
	TestToIeeeExtended(-0.0);
	TestToIeeeExtended(1.0);
	TestToIeeeExtended(-1.0);
	TestToIeeeExtended(2.0);
	TestToIeeeExtended(-2.0);
#if !(defined(sgi) || defined(NeXT))
	TestToIeeeExtended(HUGE_VAL);
	TestToIeeeExtended(-HUGE_VAL);
#endif /* sgi */

#if defined(applec) || defined(THINK_C) || defined(METROWERKS)
	Hex2Bytes("7FFF00008001000000000000", bytes); TestToIeeeExtended(*((long double*)(bytes)));	/* Quiet NaN(1) */
	Hex2Bytes("7FFF00000001000000000000", bytes); TestToIeeeExtended(*((long double*)(bytes)));	/* Signalling NaN(1) */
	Hex2Bytes("7FFE00008000000000000000", bytes); TestToIeeeExtended(*((long double*)(bytes)));
	Hex2Bytes("000000008000000000000000", bytes); TestToIeeeExtended(*((long double*)(bytes)));
	Hex2Bytes("000000000000000000000001", bytes); TestToIeeeExtended(*((long double*)(bytes)));
	Hex2Bytes("3FFF0000FEDCBA9876543210", bytes); TestToIeeeExtended(*((long double*)(bytes)));
#endif
}


/* This is the output of the test program on an IEEE machine:
IEEE(0) [00000000] --> float(0) [00000000]
IEEE(-0) [80000000] --> float(-0) [80000000]
IEEE(1) [3F800000] --> float(1) [3F800000]
IEEE(-1) [BF800000] --> float(-1) [BF800000]
IEEE(2) [40000000] --> float(2) [40000000]
IEEE(-2) [C0000000] --> float(-2) [C0000000]
IEEE(INF) [7F800000] --> float(INF) [7F800000]
IEEE(-INF) [FF800000] --> float(-INF) [FF800000]
IEEE(1.17549e-38) [00800000] --> float(1.17549e-38) [00800000]
IEEE(5.87747e-39) [00400000] --> float(5.87747e-39) [00400000]
IEEE(1.4013e-45) [00000001] --> float(1.4013e-45) [00000001]
IEEE(-1.4013e-45) [80000001] --> float(-1.4013e-45) [80000001]
IEEE(1.12444) [3F8FEDCB] --> float(1.12444) [3F8FEDCB]
IEEE(NAN(001)) [7FC00100] --> float(INF) [7F800000]
IEEE(NAN(001)) [7F800100] --> float(INF) [7F800000]
float(0) [00000000] --> IEEE(0) [00000000]
float(-0) [80000000] --> IEEE(0) [00000000]
float(1) [3F800000] --> IEEE(1) [3F800000]
float(-1) [BF800000] --> IEEE(-1) [BF800000]
float(2) [40000000] --> IEEE(2) [40000000]
float(-2) [C0000000] --> IEEE(-2) [C0000000]
float(3) [40400000] --> IEEE(3) [40400000]
float(-3) [C0400000] --> IEEE(-3) [C0400000]
float(INF) [7F800000] --> IEEE(INF) [7F800000]
float(-INF) [FF800000] --> IEEE(-INF) [FF800000]
float(1.17549e-38) [00800000] --> IEEE(1.17549e-38) [00800000]
float(5.87747e-39) [00400000] --> IEEE(5.87747e-39) [00400000]
float(1.4013e-45) [00000001] --> IEEE(1.4013e-45) [00000001]
float(7.00649e-46) [00000000] --> IEEE(0) [00000000]
float(1.12444) [3F8FEDCB] --> IEEE(1.12444) [3F8FEDCB]
float(NAN(001)) [7FC00100] --> IEEE(INF) [7F800000]
float(NAN(001)) [7FC00100] --> IEEE(INF) [7F800000]
IEEE(0) [00000000 00000000] --> double(0) [00000000 00000000]
IEEE(-0) [80000000 00000000] --> double(-0) [80000000 00000000]
IEEE(1) [3FF00000 00000000] --> double(1) [3FF00000 00000000]
IEEE(-1) [BFF00000 00000000] --> double(-1) [BFF00000 00000000]
IEEE(2) [40000000 00000000] --> double(2) [40000000 00000000]
IEEE(-2) [C0000000 00000000] --> double(-2) [C0000000 00000000]
IEEE(INF) [7FF00000 00000000] --> double(INF) [7FF00000 00000000]
IEEE(-INF) [FFF00000 00000000] --> double(-INF) [FFF00000 00000000]
IEEE(2.22507e-308) [00100000 00000000] --> double(2.22507e-308) [00100000 00000000]
IEEE(1.11254e-308) [00080000 00000000] --> double(1.11254e-308) [00080000 00000000]
IEEE(4.94066e-324) [00000000 00000001] --> double(4.94066e-324) [00000000 00000001]
IEEE(-4.94066e-324) [80000000 00000001] --> double(-4.94066e-324) [80000000 00000001]
IEEE(1.99556) [3FFFEDCB A9876543] --> double(1.99556) [3FFFEDCB A9876543]
IEEE(NAN(001)) [7FF80020 00000000] --> double(INF) [7FF00000 00000000]
IEEE(NAN(001)) [7FF00020 00000000] --> double(INF) [7FF00000 00000000]
double(0) [00000000 00000000] --> IEEE(0) [00000000 00000000]
double(-0) [80000000 00000000] --> IEEE(0) [00000000 00000000]
double(1) [3FF00000 00000000] --> IEEE(1) [3FF00000 00000000]
double(-1) [BFF00000 00000000] --> IEEE(-1) [BFF00000 00000000]
double(2) [40000000 00000000] --> IEEE(2) [40000000 00000000]
double(-2) [C0000000 00000000] --> IEEE(-2) [C0000000 00000000]
double(3) [40080000 00000000] --> IEEE(3) [40080000 00000000]
double(-3) [C0080000 00000000] --> IEEE(-3) [C0080000 00000000]
double(INF) [7FF00000 00000000] --> IEEE(INF) [7FF00000 00000000]
double(-INF) [FFF00000 00000000] --> IEEE(-INF) [FFF00000 00000000]
double(2.22507e-308) [00100000 00000000] --> IEEE(2.22507e-308) [00100000 00000000]
double(2.22507e-308) [00100000 80000000] --> IEEE(2.22507e-308) [00100000 80000000]
double(1.11254e-308) [00080000 00000000] --> IEEE(1.11254e-308) [00080000 00000000]
double(1.061e-314) [00000000 80000000] --> IEEE(1.061e-314) [00000000 80000000]
double(4.94066e-324) [00000000 00000001] --> IEEE(4.94066e-324) [00000000 00000001]
double(4.94066e-324) [00000000 00000001] --> IEEE(4.94066e-324) [00000000 00000001]
double(1.99556) [3FFFEDCB A9876543] --> IEEE(1.99556) [3FFFEDCB A9876543]
double(NAN(001)) [7FF80020 00000000] --> IEEE(INF) [7FF00000 00000000]
double(NAN(001)) [7FF80020 00000000] --> IEEE(INF) [7FF00000 00000000]
IEEE(0) [0000 00000000 00000000] --> extended(0) [0000 00000000 00000000]
IEEE(-0) [8000 00000000 00000000] --> extended(-0) [8000 00000000 00000000]
IEEE(1) [3FFF 80000000 00000000] --> extended(1) [3FFF 80000000 00000000]
IEEE(-1) [BFFF 80000000 00000000] --> extended(-1) [BFFF 80000000 00000000]
IEEE(2) [4000 80000000 00000000] --> extended(2) [4000 80000000 00000000]
IEEE(-2) [C000 80000000 00000000] --> extended(-2) [C000 80000000 00000000]
IEEE(INF) [7FFF 00000000 00000000] --> extended(INF) [7FFF 00000000 00000000]
IEEE(-INF) [FFFF 00000000 00000000] --> extended(-INF) [FFFF 00000000 00000000]
IEEE(NAN(001)) [7FFF 80010000 00000000] --> extended(INF) [7FFF 00000000 00000000]
IEEE(NAN(001)) [7FFF 00010000 00000000] --> extended(INF) [7FFF 00000000 00000000]
IEEE(1.99111) [3FFF FEDCBA98 76543210] --> extended(1.99111) [3FFF FEDCBA98 76543210]
extended(0) --> IEEE(0) [0000 00000000 00000000]
extended(-0) --> IEEE(0) [0000 00000000 00000000]
extended(1) --> IEEE(1) [3FFF 80000000 00000000]
extended(-1) --> IEEE(-1) [BFFF 80000000 00000000]
extended(2) --> IEEE(2) [4000 80000000 00000000]
extended(-2) --> IEEE(-2) [C000 80000000 00000000]
extended(INF) --> IEEE(INF) [7FFF 00000000 00000000]
extended(-INF) --> IEEE(-INF) [FFFF 00000000 00000000]
extended(NAN(001)) --> IEEE(INF) [7FFF 00000000 00000000]
extended(NAN(001)) --> IEEE(INF) [7FFF 00000000 00000000]
extended(5.94866e+4931) --> IEEE(5.94866e+4931) [7FFE 80000000 00000000]
extended(1e-4927) --> IEEE(1e-4927) [0000 80000000 00000000]
extended(1e-4927) --> IEEE(1e-4927) [0000 00000000 00000001]
extended(1.99111) --> IEEE(1.99111) [3FFF FEDCBA98 76543210]
*/
 
#endif /* TEST_FP */


/* ==== interface.c ==== */
#ifdef HAVEMPGLIB
#include <stdlib.h>
#include <stdio.h>

#include "mpg123.h"
#include "mpglib.h"


/* Global mp .. it's a hack */
struct mpstr *gmp;


BOOL InitMP3(struct mpstr *mp) 
{
	memset(mp,0,sizeof(struct mpstr));

	mp->framesize = 0;
	mp->fsizeold = -1;
	mp->bsize = 0;
	mp->head = mp->tail = NULL;
	mp->fr.single = -1;
	mp->bsnum = 0;
	mp->synth_bo = 1;

	make_decode_tables(32767);
	init_layer3(SBLIMIT);

	return !0;
}

void ExitMP3(struct mpstr *mp)
{
	struct buf *b,*bn;
	
	b = mp->tail;
	while(b) {
		free(b->pnt);
		bn = b->next;
		free(b);
		b = bn;
	}
}

static struct buf *addbuf(struct mpstr *mp,char *buf,int size)
{
	struct buf *nbuf;

	nbuf = (struct buf*) malloc( sizeof(struct buf) );
	if(!nbuf) {
		fprintf(stderr,"Out of memory!\n");
		return NULL;
	}
	nbuf->pnt = (unsigned char*) malloc(size);
	if(!nbuf->pnt) {
		free(nbuf);
		return NULL;
	}
	nbuf->size = size;
	memcpy(nbuf->pnt,buf,size);
	nbuf->next = NULL;
	nbuf->prev = mp->head;
	nbuf->pos = 0;

	if(!mp->tail) {
		mp->tail = nbuf;
	}
	else {
	  mp->head->next = nbuf;
	}

	mp->head = nbuf;
	mp->bsize += size;

	return nbuf;
}

static void remove_buf(struct mpstr *mp)
{
  struct buf *buf = mp->tail;
  
  mp->tail = buf->next;
  if(mp->tail)
    mp->tail->prev = NULL;
  else {
    mp->tail = mp->head = NULL;
  }
  
  free(buf->pnt);
  free(buf);

}

static int read_buf_byte(struct mpstr *mp)
{
	unsigned int b;

	int pos;

	pos = mp->tail->pos;
	while(pos >= mp->tail->size) {
		remove_buf(mp);
		pos = mp->tail->pos;
		if(!mp->tail) {
			fprintf(stderr,"Fatal error!\n");
			exit(1);
		}
	}

	b = mp->tail->pnt[pos];
	mp->bsize--;
	mp->tail->pos++;
	

	return b;
}

static void read_head(struct mpstr *mp)
{
	unsigned long head;

	head = read_buf_byte(mp);
	head <<= 8;
	head |= read_buf_byte(mp);
	head <<= 8;
	head |= read_buf_byte(mp);
	head <<= 8;
	head |= read_buf_byte(mp);

	mp->header = head;
}

int decodeMP3(struct mpstr *mp,char *in,int isize,char *out,
		int osize,int *done)
{
	int len;

	gmp = mp;

	if(osize < 4608) {
		fprintf(stderr,"To less out space\n");
		return MP3_ERR;
	}

	if(in) {
		if(addbuf(mp,in,isize) == NULL) {
			return MP3_ERR;
		}
	}


	/* First decode header */
	if(mp->framesize == 0) {
		if(mp->bsize < 4) {
			return MP3_NEED_MORE;
		}
		read_head(mp);
		decode_header(&mp->fr,mp->header);
		mp->framesize = mp->fr.framesize;
	}

	/*	  printf(" fr.framesize = %i \n",mp->fr.framesize);
		  printf(" bsize        = %i \n",mp->bsize);
	*/

	if(mp->fr.framesize > mp->bsize) {
	  return MP3_NEED_MORE;
	}
	wordpointer = mp->bsspace[mp->bsnum] + 512;
	mp->bsnum = (mp->bsnum + 1) & 0x1;
	bitindex = 0;

	len = 0;
	while(len < mp->framesize) {
		int nlen;
		int blen = mp->tail->size - mp->tail->pos;
		if( (mp->framesize - len) <= blen) {
                  nlen = mp->framesize-len;
		}
		else {
                  nlen = blen;
                }
		memcpy(wordpointer+len,mp->tail->pnt+mp->tail->pos,nlen);
                len += nlen;
                mp->tail->pos += nlen;
		mp->bsize -= nlen;
                if(mp->tail->pos == mp->tail->size) {
                   remove_buf(mp);
                }
	}

	*done = 0;
	if(mp->fr.error_protection)
           getbits(16);
	do_layer3(&mp->fr,(unsigned char *) out,done);

	mp->fsizeold = mp->framesize;
	mp->framesize = 0;
	return MP3_OK;
}

int set_pointer(long backstep)
{
  unsigned char *bsbufold;
  if(gmp->fsizeold < 0 && backstep > 0) {
    fprintf(stderr,"Can't step back %ld!\n",backstep);
    return MP3_ERR; 
  }
  bsbufold = gmp->bsspace[gmp->bsnum] + 512;
  wordpointer -= backstep;
  if (backstep)
    memcpy(wordpointer,bsbufold+gmp->fsizeold-backstep,backstep);
  bitindex = 0;
  return MP3_OK;
}





#endif


/* ==== l3bitstream.c ==== */
/**********************************************************************
 * ISO MPEG Audio Subgroup Software Simulation Group (1996)
 * ISO 13818-3 MPEG-2 Audio Encoder - Lower Sampling Frequency Extension
 *
 **********************************************************************/
/*
  Revision History:

  Date        Programmer                Comment
  ==========  ========================= ===============================
  1995/08/06  mc@fivebats.com           created
  1995/09/06  mc@fivebats.com           modified to use formatBitstream
*/

#include <stdlib.h>
#include "lame.h"
#include "l3bitstream.h" /* the public interface */
#include "encoder.h"
#include "quantize.h"
#include "quantize-pvt.h"
#include "formatBitstream.h"
#include "tables.h"
#include <assert.h>
#include "l3bitstream-pvt.h"

static Bit_stream_struc *bs = NULL;

BF_FrameData    *frameData    = NULL;
BF_FrameResults *frameResults = NULL;

int PartHoldersInitialized = 0;

BF_PartHolder *headerPH;
BF_PartHolder *frameSIPH;
BF_PartHolder *channelSIPH[ MAX_CHANNELS ];
BF_PartHolder *spectrumSIPH[ MAX_GRANULES ][ MAX_CHANNELS ];
BF_PartHolder *scaleFactorsPH[ MAX_GRANULES ][ MAX_CHANNELS ];
BF_PartHolder *codedDataPH[ MAX_GRANULES ][ MAX_CHANNELS ];
BF_PartHolder *userSpectrumPH[ MAX_GRANULES ][ MAX_CHANNELS ];
BF_PartHolder *userFrameDataPH;


void putMyBits( u_int val, u_int len )
{
    putbits( bs, val, len );
}

/*
  III_format_bitstream()
  
  This is called after a frame of audio has been quantized and coded.
  It will write the encoded audio to the bitstream. Note that
  from a layer3 encoder's perspective the bit stream is primarily
  a series of main_data() blocks, with header and side information
  inserted at the proper locations to maintain framing. (See Figure A.7
  in the IS).
  */

void
III_format_bitstream( lame_global_flags *gfp,
                      int              bitsPerFrame,
		      int              l3_enc[2][2][576],
		      III_side_info_t  *l3_side,
		      III_scalefac_t   scalefac[2][2],
		      Bit_stream_struc *in_bs)
{
    int gr, ch;
    bs = in_bs;

    if ( frameData == NULL )
    {
	frameData = calloc( 1,sizeof *frameData);
	assert( frameData );
    }
    if ( frameResults == NULL )
    {
	frameResults = calloc( 1,sizeof *frameResults);
	assert( frameResults );
    }

    if ( !PartHoldersInitialized )
    {
	headerPH = BF_newPartHolder( 14 ); 
	frameSIPH = BF_newPartHolder( 12 );

	for ( ch = 0; ch < MAX_CHANNELS; ch++ )
	    channelSIPH[ch] = BF_newPartHolder( 8 );

	for ( gr = 0; gr < MAX_GRANULES; gr++ )	
	    for ( ch = 0; ch < MAX_CHANNELS; ch++ )
	    {
		spectrumSIPH[gr][ch]   = BF_newPartHolder( 32 );
		scaleFactorsPH[gr][ch] = BF_newPartHolder( 64 );
		codedDataPH[gr][ch]    = BF_newPartHolder( 576 );
		userSpectrumPH[gr][ch] = BF_newPartHolder( 4 );
	    }
	userFrameDataPH = BF_newPartHolder( 8 );
	PartHoldersInitialized = 1;
    }

    encodeSideInfo( gfp,l3_side );
    encodeMainData( gfp,l3_enc, l3_side, scalefac );



    drain_into_ancillary_data( l3_side->resvDrain );
    /*
      Put frameData together for the call
      to BitstreamFrame()
    */
    frameData->frameLength = bitsPerFrame;
    frameData->nGranules   = gfp->mode_gr;
    frameData->nChannels   = gfp->stereo;
    frameData->header      = headerPH->part;
    frameData->frameSI     = frameSIPH->part;

    for ( ch = 0; ch < gfp->stereo; ch++ )
	frameData->channelSI[ch] = channelSIPH[ch]->part;

    for ( gr = 0; gr < gfp->mode_gr; gr++ )
	for ( ch = 0; ch < gfp->stereo; ch++ )
	{
	    frameData->spectrumSI[gr][ch]   = spectrumSIPH[gr][ch]->part;
	    frameData->scaleFactors[gr][ch] = scaleFactorsPH[gr][ch]->part;
	    frameData->codedData[gr][ch]    = codedDataPH[gr][ch]->part;
	    frameData->userSpectrum[gr][ch] = userSpectrumPH[gr][ch]->part;
	}
    frameData->userFrameData = userFrameDataPH->part;

    BF_BitstreamFrame( frameData, frameResults );

    /* we set this here -- it will be tested in the next loops iteration */
    l3_side->main_data_begin = frameResults->nextBackPtr;

}

void
III_FlushBitstream(void)
{
    if (PartHoldersInitialized!=0)
		BF_FlushBitstream( frameData, frameResults );
}

static unsigned slen1_tab[16] = { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 };
static unsigned slen2_tab[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 };

static void
encodeMainData( lame_global_flags *gfp,
		int              l3_enc[2][2][576],
		III_side_info_t  *si,
		III_scalefac_t   scalefac[2][2] )
{
    int i, gr, ch, sfb, window;


    for ( gr = 0; gr < gfp->mode_gr; gr++ )
	for ( ch = 0; ch < gfp->stereo; ch++ )
	    scaleFactorsPH[gr][ch]->part->nrEntries = 0;

    for ( gr = 0; gr < gfp->mode_gr; gr++ )
	for ( ch = 0; ch < gfp->stereo; ch++ )
	    codedDataPH[gr][ch]->part->nrEntries = 0;

    if ( gfp->version == 1 )
    {  /* MPEG 1 */
	for ( gr = 0; gr < 2; gr++ )
	{
	    for ( ch = 0; ch < gfp->stereo; ch++ )
	    {
		BF_PartHolder **pph = &scaleFactorsPH[gr][ch];		
		gr_info *gi = &(si->gr[gr].ch[ch].tt);
		unsigned slen1 = slen1_tab[ gi->scalefac_compress ];
		unsigned slen2 = slen2_tab[ gi->scalefac_compress ];
		int *ix = &l3_enc[gr][ch][0];

		if (gi->block_type == SHORT_TYPE)
		{
#ifdef ALLOW_MIXED
		    if ( gi->mixed_block_flag )
		    {
			for ( sfb = 0; sfb < 8; sfb++ )
			    *pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen1 );

			for ( sfb = 3; sfb < 6; sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen1 );

			for ( sfb = 6; sfb < 12; sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen2 );

		    }
		    else
#endif
		    {
			for ( sfb = 0; sfb < 6; sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen1 );

			for ( sfb = 6; sfb < 12; sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen2 );
		    }
		}
		else
		{
		    if ( (gr == 0) || (si->scfsi[ch][0] == 0) )
			for ( sfb = 0; sfb < 6; sfb++ )
			    *pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen1 );

		    if ( (gr == 0) || (si->scfsi[ch][1] == 0) )
			for ( sfb = 6; sfb < 11; sfb++ )
			    *pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen1 );

		    if ( (gr == 0) || (si->scfsi[ch][2] == 0) )
			for ( sfb = 11; sfb < 16; sfb++ )
			    *pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen2 );

		    if ( (gr == 0) || (si->scfsi[ch][3] == 0) )
			for ( sfb = 16; sfb < 21; sfb++ )
			    *pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen2 );
		}
		Huffmancodebits( &codedDataPH[gr][ch], ix, gi );
	    } /* for ch */
	} /* for gr */
    }
    else
    {  /* MPEG 2 */
	gr = 0;
	for ( ch = 0; ch < gfp->stereo; ch++ )
	{
	    BF_PartHolder **pph = &scaleFactorsPH[gr][ch];		
	    gr_info *gi = &(si->gr[gr].ch[ch].tt);
	    int *ix = &l3_enc[gr][ch][0];
	    int sfb_partition;
	    assert( gi->sfb_partition_table );

	    if (gi->block_type == SHORT_TYPE)
	    {
#ifdef ALLOW_MIXED
		if ( gi->mixed_block_flag )
		{
		    sfb_partition = 0;
		    for ( sfb = 0; sfb < 8; sfb++ )
			*pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], gi->slen[sfb_partition] );

		    for ( sfb = 3, sfb_partition = 1; sfb_partition < 4; sfb_partition++ )
		    {
			int sfbs = gi->sfb_partition_table[ sfb_partition ] / 3;
			int slen = gi->slen[ sfb_partition ];
			for ( i = 0; i < sfbs; i++, sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen );
		    }
		}
		else
#endif
		{
		    for ( sfb = 0, sfb_partition = 0; sfb_partition < 4; sfb_partition++ )
		    {
			int sfbs = gi->sfb_partition_table[ sfb_partition ] / 3;
			int slen = gi->slen[ sfb_partition ];
			for ( i = 0; i < sfbs; i++, sfb++ )
			    for ( window = 0; window < 3; window++ )
				*pph = BF_addEntry( *pph,  scalefac[gr][ch].s[sfb][window], slen );
		    }
		}
	    }
	    else
	    {
		for ( sfb = 0, sfb_partition = 0; sfb_partition < 4; sfb_partition++ )
		{
		    int sfbs = gi->sfb_partition_table[ sfb_partition ];
		    int slen = gi->slen[ sfb_partition ];
		    for ( i = 0; i < sfbs; i++, sfb++ )
			*pph = BF_addEntry( *pph,  scalefac[gr][ch].l[sfb], slen );
		}
	    }



	    Huffmancodebits( &codedDataPH[gr][ch], ix, gi );
	} /* for ch */
    }
} /* main_data */

static unsigned int crc = 0; /* (jo) current crc */

/* (jo) this wrapper function for BF_addEntry() updates also the crc */
static BF_PartHolder *CRC_BF_addEntry( BF_PartHolder *thePH, u_int value, u_int length )
{
   u_int bit = 1 << length;
   
   while((bit >>= 1)){
      crc <<= 1;
      if (!(crc & 0x10000) ^ !(value & bit))
	crc ^= CRC16_POLYNOMIAL;
   }
   crc &= 0xffff;   
   return BF_addEntry(thePH, value, length);
}




static int encodeSideInfo( lame_global_flags *gfp,III_side_info_t  *si )
{
    int gr, ch, scfsi_band, region, window, bits_sent;
    
    crc = 0xffff; /* (jo) init crc16 for error_protection */

    headerPH->part->nrEntries = 0;
    headerPH = BF_addEntry( headerPH, 0xfff,                    12 );
    headerPH = BF_addEntry( headerPH, gfp->version,            1 );
    headerPH = BF_addEntry( headerPH, 1,                        2 );
    headerPH = BF_addEntry( headerPH, !gfp->error_protection,     1 );
    /* (jo) from now on call the CRC_BF_addEntry() wrapper to update crc */
    headerPH = CRC_BF_addEntry( headerPH, gfp->bitrate_index,      4 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->samplerate_index,   2 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->padding,            1 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->extension,          1 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->mode,               2 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->mode_ext,           2 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->copyright,          1 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->original,           1 );
    headerPH = CRC_BF_addEntry( headerPH, gfp->emphasis,           2 );
    
    bits_sent = 32;
   
    /* (jo) see below for BF_addEntry( headerPH, crc, 16 ); */

    frameSIPH->part->nrEntries = 0;

    for (ch = 0; ch < gfp->stereo; ch++ )
	channelSIPH[ch]->part->nrEntries = 0;

    for ( gr = 0; gr < gfp->mode_gr; gr++ )
	for ( ch = 0; ch < gfp->stereo; ch++ )
	    spectrumSIPH[gr][ch]->part->nrEntries = 0;

    if ( gfp->version == 1 )
    {  /* MPEG1 */
	frameSIPH = CRC_BF_addEntry( frameSIPH, si->main_data_begin, 9 );

	if ( gfp->stereo == 2 )
	    frameSIPH = CRC_BF_addEntry( frameSIPH, si->private_bits, 3 );
	else
	    frameSIPH = CRC_BF_addEntry( frameSIPH, si->private_bits, 5 );
	
	for ( ch = 0; ch < gfp->stereo; ch++ )
	    for ( scfsi_band = 0; scfsi_band < 4; scfsi_band++ )
	    {
		BF_PartHolder **pph = &channelSIPH[ch];
		*pph = CRC_BF_addEntry( *pph, si->scfsi[ch][scfsi_band], 1 );
	    }

	for ( gr = 0; gr < 2; gr++ )
	    for ( ch = 0; ch < gfp->stereo; ch++ )
	    {
		BF_PartHolder **pph = &spectrumSIPH[gr][ch];
		gr_info *gi = &(si->gr[gr].ch[ch].tt);
		*pph = CRC_BF_addEntry( *pph, gi->part2_3_length,        12 );
		*pph = CRC_BF_addEntry( *pph, gi->big_values,            9 );
		*pph = CRC_BF_addEntry( *pph, gi->global_gain,           8 );
		*pph = CRC_BF_addEntry( *pph, gi->scalefac_compress,     4 );
		*pph = CRC_BF_addEntry( *pph, gi->window_switching_flag, 1 );

		if ( gi->window_switching_flag )
		{   
		    *pph = CRC_BF_addEntry( *pph, gi->block_type,       2 );
		    *pph = CRC_BF_addEntry( *pph, gi->mixed_block_flag, 1 );

		    for ( region = 0; region < 2; region++ )
			*pph = CRC_BF_addEntry( *pph, gi->table_select[region],  5 );
		    for ( window = 0; window < 3; window++ )
			*pph = CRC_BF_addEntry( *pph, gi->subblock_gain[window], 3 );
		}
		else
		{
		    assert( gi->block_type == NORM_TYPE );
		    for ( region = 0; region < 3; region++ )
			*pph = CRC_BF_addEntry( *pph, gi->table_select[region], 5 );

		    *pph = CRC_BF_addEntry( *pph, gi->region0_count, 4 );
		    *pph = CRC_BF_addEntry( *pph, gi->region1_count, 3 );
		}

		*pph = CRC_BF_addEntry( *pph, gi->preflag,            1 );
		*pph = CRC_BF_addEntry( *pph, gi->scalefac_scale,     1 );
		*pph = CRC_BF_addEntry( *pph, gi->count1table_select, 1 );
	    }

	if ( gfp->stereo == 2 )
	    bits_sent += 256;
	else
	    bits_sent += 136;
    }
    else
    {  /* MPEG2 */
	frameSIPH = CRC_BF_addEntry( frameSIPH, si->main_data_begin, 8 );

	if ( gfp->stereo == 2 )
	    frameSIPH = CRC_BF_addEntry( frameSIPH, si->private_bits, 2 );
	else
	    frameSIPH = CRC_BF_addEntry( frameSIPH, si->private_bits, 1 );
	
	gr = 0;
	for ( ch = 0; ch < gfp->stereo; ch++ )
	{
	    BF_PartHolder **pph = &spectrumSIPH[gr][ch];
	    gr_info *gi = &(si->gr[gr].ch[ch].tt);
	    *pph = CRC_BF_addEntry( *pph, gi->part2_3_length,        12 );
	    *pph = CRC_BF_addEntry( *pph, gi->big_values,            9 );
	    *pph = CRC_BF_addEntry( *pph, gi->global_gain,           8 );
	    *pph = CRC_BF_addEntry( *pph, gi->scalefac_compress,     9 );
	    *pph = CRC_BF_addEntry( *pph, gi->window_switching_flag, 1 );

	    if ( gi->window_switching_flag )
	    {   
		*pph = CRC_BF_addEntry( *pph, gi->block_type,       2 );
		*pph = CRC_BF_addEntry( *pph, gi->mixed_block_flag, 1 );

		for ( region = 0; region < 2; region++ )
		    *pph = CRC_BF_addEntry( *pph, gi->table_select[region],  5 );
		for ( window = 0; window < 3; window++ )
		    *pph = CRC_BF_addEntry( *pph, gi->subblock_gain[window], 3 );
	    }
	    else
	    {
		for ( region = 0; region < 3; region++ )
		    *pph = CRC_BF_addEntry( *pph, gi->table_select[region], 5 );

		*pph = CRC_BF_addEntry( *pph, gi->region0_count, 4 );
		*pph = CRC_BF_addEntry( *pph, gi->region1_count, 3 );
	    }

	    *pph = CRC_BF_addEntry( *pph, gi->scalefac_scale,     1 );
	    *pph = CRC_BF_addEntry( *pph, gi->count1table_select, 1 );
	}
	if ( gfp->stereo == 2 )
	    bits_sent += 136;
	else
	    bits_sent += 72;
    }

    if ( gfp->error_protection )
    {   /* (jo) error_protection: add crc16 information to header */
	headerPH = BF_addEntry( headerPH, crc, 16 );
	bits_sent += 16;
    }

    return bits_sent;
}

/*
  Some combinations of bitrate, Fs, and stereo make it impossible to stuff
  out a frame using just main_data, due to the limited number of bits to
  indicate main_data_length. In these situations, we put stuffing bits into
  the ancillary data...
*/
static void
drain_into_ancillary_data( int lengthInBits )
{
    /*
     */
    int wordsToSend   = lengthInBits / 32;
    int remainingBits = lengthInBits % 32;
    int i;

    /*
      userFrameDataPH->part->nrEntries set by call to write_ancillary_data()
    */
    
    userFrameDataPH->part->nrEntries = 0;
    for ( i = 0; i < wordsToSend; i++ )
	userFrameDataPH = BF_addEntry( userFrameDataPH, 0, 32 );
    if ( remainingBits )
	userFrameDataPH = BF_addEntry( userFrameDataPH, 0, remainingBits );
}

/*
  Note the discussion of huffmancodebits() on pages 28
  and 29 of the IS, as well as the definitions of the side
  information on pages 26 and 27.
  */
static void
Huffmancodebits( BF_PartHolder **pph, int *ix, gr_info *gi )
{
    int L3_huffman_coder_count1( BF_PartHolder **pph, struct huffcodetab *h, int v, int w, int x, int y );

    int region1Start;
    int region2Start;
    int i, bigvalues, count1End;
    int v, w, x, y, bits, cbits, xbits, stuffingBits;
    unsigned int code, ext;
#ifdef DEBUG
    int bvbits, c1bits;
#endif
    int bitsWritten = 0;

    
    /* 1: Write the bigvalues */
    bigvalues = gi->big_values * 2;
    if ( bigvalues )
    {
	if ( !(gi->mixed_block_flag) && (gi->block_type == SHORT_TYPE) )
	{ /* Three short blocks */
	    /*
	      Within each scalefactor band, data is given for successive
	      time windows, beginning with window 0 and ending with window 2.
	      Within each window, the quantized values are then arranged in
	      order of increasing frequency...
	      */
	    int sfb, window, line, start, end;

	    I192_3 *ix_s;
	    
	    ix_s = (I192_3 *) ix;
	    region1Start = 12;
	    region2Start = 576;

	    for ( sfb = 0; sfb < 13; sfb++ )
	    {
		unsigned tableindex = 100;
		start = scalefac_band.s[ sfb ];
		end   = scalefac_band.s[ sfb+1 ];

		if ( start < region1Start )
		    tableindex = gi->table_select[ 0 ];
		else
		    tableindex = gi->table_select[ 1 ];
		assert( tableindex < 32 );

		for ( window = 0; window < 3; window++ )
		    for ( line = start; line < end; line += 2 )
		    {
			x = (*ix_s)[line][window];
			y = (*ix_s)[line + 1][window];
			bits = HuffmanCode( tableindex, x, y, &code, &ext, &cbits, &xbits );
			*pph = BF_addEntry( *pph,  code, cbits );
			*pph = BF_addEntry( *pph,  ext, xbits );
			bitsWritten += bits;
		    }
		
	    }
	}
	else
#ifdef ALLOW_MIXED
	    if ( gi->mixed_block_flag && gi->block_type == SHORT_TYPE )
	    {  /* Mixed blocks long, short */
		int sfb, window, line, start, end;
		unsigned tableindex;
		I192_3 *ix_s;
		
		ix_s = (I192_3 *) ix;

		/* Write the long block region */
		tableindex = gi->table_select[0];
		if ( tableindex )
		    for ( i = 0; i < 36; i += 2 )
		    {
			x = ix[i];
			y = ix[i + 1];
			bits = HuffmanCode( tableindex, x, y, &code, &ext, &cbits, &xbits );
			*pph = BF_addEntry( *pph,  code, cbits );
			*pph = BF_addEntry( *pph,  ext, xbits );
			bitsWritten += bits;
			
		    }
		/* Write the short block region */
		tableindex = gi->table_select[ 1 ];
		assert( tableindex < 32 );

		for ( sfb = 3; sfb < 13; sfb++ )
		{
		    start = scalefac_band.s[ sfb ];
		    end   = scalefac_band.s[ sfb+1 ];           
		    
		    for ( window = 0; window < 3; window++ )
			for ( line = start; line < end; line += 2 )
			{
			    x = (*ix_s)[line][window];
			    y = (*ix_s)[line + 1][window];
			    bits = HuffmanCode( tableindex, x, y, &code, &ext, &cbits, &xbits );
			    *pph = BF_addEntry( *pph,  code, cbits );
			    *pph = BF_addEntry( *pph,  ext, xbits );
			    bitsWritten += bits;
			}
		}

	    }
	    else
#endif
	    { /* Long blocks */
		unsigned scalefac_index = 100;
		
		if ( gi->mixed_block_flag )
		{
		    region1Start = 36;
		    region2Start = 576;
		}
		else
		{
		    scalefac_index = gi->region0_count + 1;
		    assert( scalefac_index < 23 );
		    region1Start = scalefac_band.l[ scalefac_index ];
		    scalefac_index += gi->region1_count + 1;
		    assert( scalefac_index < 23 );    
		    region2Start = scalefac_band.l[ scalefac_index ];
		}

		for ( i = 0; i < bigvalues; i += 2 )
		{
		    unsigned tableindex = 100;
		    /* get table pointer */
		    if ( i < region1Start )
		    {
			tableindex = gi->table_select[0];
		    }
		    else
			if ( i < region2Start )
			{
			    tableindex = gi->table_select[1];
			}
			else
			{
			    tableindex = gi->table_select[2];
			}
		    assert( tableindex < 32 );
		    /* get huffman code */
		    x = ix[i];
		    y = ix[i + 1];

		    if ( tableindex )
		    {
			bits = HuffmanCode( tableindex, x, y, &code, &ext, &cbits, &xbits );
			*pph = BF_addEntry( *pph,  code, cbits );
			*pph = BF_addEntry( *pph,  ext, xbits );
			bitsWritten += bits;
		    }
		}
	    }
    }
#ifdef DEBUG
    bvbits = bitsWritten; 
#endif

    /* 2: Write count1 area */
    assert( (gi->count1table_select < 2) );
    count1End = bigvalues + (gi->count1 * 4);

    assert( count1End <= 576 );

    for ( i = bigvalues; i < count1End; i += 4 )
    {
	v = ix[i];
	w = ix[i+1];
	x = ix[i+2];
	y = ix[i+3];
	bitsWritten += L3_huffman_coder_count1( pph, &ht[gi->count1table_select + 32], v, w, x, y );
    }
#ifdef DEBUG
    c1bits = bitsWritten - bvbits;
#endif
    if ( (stuffingBits = gi->part2_3_length - gi->part2_length - bitsWritten) )
    {
	int stuffingWords = stuffingBits / 32;
	int remainingBits = stuffingBits % 32;

        fprintf(stderr,"opps - adding stuffing bits = %i.\n",stuffingBits);
        fprintf(stderr,"this should not happen...\n");

	/*
	  Due to the nature of the Huffman code
	  tables, we will pad with ones
	*/
	while ( stuffingWords-- )
	    *pph = BF_addEntry( *pph, ~(u_int)0, 32 );
	if ( remainingBits )
	    *pph = BF_addEntry( *pph, ~(u_int)0, remainingBits );
	bitsWritten += stuffingBits;
    }
    assert( bitsWritten == (int)(gi->part2_3_length - gi->part2_length) );
#ifdef DEBUG
    fprintf(stderr, "## %d Huffman bits written (%02d + %02d), part2_length = %d, part2_3_length = %d, %d stuffing ##\n",
	    bitsWritten, bvbits, c1bits, gi->part2_length, gi->part2_3_length, stuffingBits );
#endif
}

int
abs_and_sign( int *x )
{
    if ( *x > 0 )
	return 0;
    *x *= -1;
    return 1;
}

int
L3_huffman_coder_count1( BF_PartHolder **pph, struct huffcodetab *h, int v, int w, int x, int y )
{
    HUFFBITS huffbits;
    unsigned int signv, signw, signx, signy, p;
    int len;
    int totalBits = 0;
    
    signv = abs_and_sign( &v );
    signw = abs_and_sign( &w );
    signx = abs_and_sign( &x );
    signy = abs_and_sign( &y );
    
    /* bug fix from Leonid A. Kulakov 9/1999:*/
    p = (v << 3) + (w << 2) + (x << 1) + y;  

    huffbits = h->table[p];
    len = h->hlen[ p ];
    *pph = BF_addEntry(*pph, huffbits, len);
    totalBits= 0;
#if 0
    if ( v )
    {
	*pph = BF_addEntry( *pph,  signv, 1 );
	totalBits += 1;
    }
    if ( w )
    {
	*pph = BF_addEntry( *pph,  signw, 1 );
	totalBits += 1;
    }

    if ( x )
    {
	*pph = BF_addEntry( *pph,  signx, 1 );
	totalBits += 1;
    }
    if ( y )
    {
	*pph = BF_addEntry( *pph,  signy, 1 );
	totalBits += 1;
    }
#endif   

    p=0;
    if ( v ) {
	p = signv;
	++totalBits;
    }

    if ( w ){
	p = 2*p + signw;
	++totalBits;
    }

    if ( x ) {
	p = 2*p + signx;
	++totalBits;
    }

    if ( y ) {
	p = 2*p + signy;
	++totalBits;
    }

    *pph = BF_addEntry(*pph, p, totalBits);

    return totalBits+len;  
}

/*
  Implements the pseudocode of page 98 of the IS
  */
int
HuffmanCode( int table_select, int x, int y, unsigned int *code, unsigned int *ext, int *cbits, int *xbits )
{
    unsigned signx, signy, linbitsx, linbitsy, linbits, idx;
    struct huffcodetab *h;

    *cbits = 0;
    *xbits = 0;
    *code  = 0;
    *ext   = 0;
    
    if ( table_select == 0 )
	return 0;
    
    signx = abs_and_sign( &x );
    signy = abs_and_sign( &y );
    h = &(ht[table_select]);

    if ( table_select > 15 )
    { /* ESC-table is used */
      linbits = h->xlen;
      linbitsx = linbitsy = 0;
	if ( x > 14 )
	{
	    linbitsx = x - 15;
	    assert( linbitsx <= h->linmax );
	    x = 15;
	}
	if ( y > 14 )
	{
	    linbitsy = y - 15;
	    assert( linbitsy <= h->linmax );
	    y = 15;
	}
	idx = x * 16 + y;
	*code = h->table[idx];
        *cbits = h->hlen[ idx ];
	if ( x > 14 )
	{
	    *ext |= linbitsx;
	    *xbits += linbits;
	}
	if ( x != 0 )
	{
	    *ext <<= 1;
	    *ext |= signx;
	    *xbits += 1;
	}
	if ( y > 14 )
	{
	    *ext <<= linbits;
	    *ext |= linbitsy;
	    *xbits += linbits;
	}
	if ( y != 0 )
	{
	    *ext <<= 1;
	    *ext |= signy;
	    *xbits += 1;
	}
    }
    else
    { /* No ESC-words */
	idx = x * 16 + y;
	*code = h->table[idx];
	*cbits += h->hlen[ idx ];
	if ( x != 0 )
	{
	    *code <<= 1;
	    *code |= signx;
	    *cbits += 1;
	}
	if ( y != 0 )
	{
	    *code <<= 1;
	    *code |= signy;
            *cbits += 1;
	}
    }
    assert( *cbits <= 32 );
    assert( *xbits <= 32 );
    return *cbits + *xbits;
}


/* ==== lame.c ==== */
/*
 *	LAME MP3 encoding engine
 *
 *	Copyright (c) 1999 Mark Taylor
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include <assert.h>

#ifdef HAVEGTK
#include "gtkanal.h"
#include <gtk/gtk.h>
#endif
#include "lame.h"
#include "util.h"
#include "timestatus.h"
#include "psymodel.h"
#include "newmdct.h"
#include "quantize.h"
#include "quantize-pvt.h"
#include "l3bitstream.h"
#include "formatBitstream.h"
#include "version.h"
#include "VbrTag.h"
#include "id3tag.h"
#include "tables.h"
#include "brhist.h"
#include "get_audio.h"

#ifdef __riscos__
#include "asmstuff.h"
#endif


/* Global variable definitions for lame.c */
static Bit_stream_struc   bs;
static III_side_info_t l3_side;
#define MFSIZE (1152+1152+ENCDELAY-MDCTDELAY)
static short int mfbuf[2][MFSIZE];
static int mf_size;
static int mf_samples_to_encode;



/********************************************************************
 *   initialize internal params based on data in gf
 *   (globalflags struct filled in by calling program)
 *
 ********************************************************************/
void lame_init_params(lame_global_flags *gfp)
{
  int i;
  FLOAT compression_ratio;


  memset(&bs, 0, sizeof(Bit_stream_struc));
  memset(&l3_side,0x00,sizeof(III_side_info_t));


  gfp->frameNum=0;
  InitFormatBitStream();
  if (gfp->num_channels==1) {
    gfp->mode = MPG_MD_MONO;
  }
  gfp->stereo=2;
  if (gfp->mode == MPG_MD_MONO) gfp->stereo=1;

#ifdef BRHIST
  if (gfp->silent) {
    disp_brhist=0;  /* turn of VBR historgram */
  }
  if (!gfp->VBR) {
    disp_brhist=0;  /* turn of VBR historgram */
  }
#endif

  /* set the output sampling rate, and resample options if necessary
     samplerate = input sample rate
     resamplerate = ouput sample rate
  */
  if (gfp->out_samplerate==0) {
    /* user did not specify output sample rate */
    gfp->out_samplerate=gfp->in_samplerate;   /* default */


    /* if resamplerate is not valid, find a valid value */
    if (gfp->out_samplerate>=48000) gfp->out_samplerate=48000;
    else if (gfp->out_samplerate>=44100) gfp->out_samplerate=44100;
    else if (gfp->out_samplerate>=32000) gfp->out_samplerate=32000;
    else if (gfp->out_samplerate>=24000) gfp->out_samplerate=24000;
    else if (gfp->out_samplerate>=22050) gfp->out_samplerate=22050;
    else gfp->out_samplerate=16000;


    if (gfp->brate>0) {
      /* check if user specified bitrate requires downsampling */
      compression_ratio = gfp->out_samplerate*16*gfp->stereo/(1000.0*gfp->brate);
      if (!gfp->VBR && compression_ratio > 13 ) {
	/* automatic downsample, if possible */
	gfp->out_samplerate = (10*1000.0*gfp->brate)/(16*gfp->stereo);
	if (gfp->out_samplerate<=16000) gfp->out_samplerate=16000;
	else if (gfp->out_samplerate<=22050) gfp->out_samplerate=22050;
	else if (gfp->out_samplerate<=24000) gfp->out_samplerate=24000;
	else if (gfp->out_samplerate<=32000) gfp->out_samplerate=32000;
	else if (gfp->out_samplerate<=44100) gfp->out_samplerate=44100;
	else gfp->out_samplerate=48000;
      }
    }
  }

  gfp->mode_gr = (gfp->out_samplerate <= 24000) ? 1 : 2;  /* mode_gr = 2 */
  gfp->encoder_delay = ENCDELAY;
  gfp->framesize = gfp->mode_gr*576;

  if (gfp->brate==0) { /* user didn't specify a bitrate, use default */
    gfp->brate=128;
    if (gfp->mode_gr==1) gfp->brate=64;
  }


  gfp->resample_ratio=1;
  if (gfp->out_samplerate != gfp->in_samplerate) gfp->resample_ratio = (FLOAT)gfp->in_samplerate/(FLOAT)gfp->out_samplerate;

  /* estimate total frames.  must be done after setting sampling rate so
   * we know the framesize.  */
  gfp->totalframes=0;
  gfp->totalframes = 2+ gfp->num_samples/(gfp->resample_ratio*gfp->framesize);



  /* 44.1kHz at 56kbs/channel: compression factor of 12.6
     44.1kHz at 64kbs/channel: compression factor of 11.025
     44.1kHz at 80kbs/channel: compression factor of 8.82
     22.05kHz at 24kbs:  14.7
     22.05kHz at 32kbs:  11.025
     22.05kHz at 40kbs:  8.82
     16kHz at 16kbs:  16.0
     16kHz at 24kbs:  10.7

     compression_ratio
        11                                .70?
        12                   sox resample .66
        14.7                 sox resample .45

  */
  if (gfp->brate >= 320) gfp->VBR=0;  /* dont bother with VBR at 320kbs */
  compression_ratio = gfp->out_samplerate*16*gfp->stereo/(1000.0*gfp->brate);


  /* for VBR, take a guess at the compression_ratio */
  /* VBR_q           compression       like
     0                4.4             320kbs
     1                5.4             256kbs
     3                7.4             192kbs
     4                8.8             160kbs
     6                10.4            128kbs
  */
  if (gfp->VBR && compression_ratio>11) {
    compression_ratio = 4.4 + gfp->VBR_q;
  }


  /* At higher quality (lower compression) use STEREO instead of JSTEREO.
   * (unless the user explicitly specified a mode ) */
  if ( (!gfp->mode_fixed) && (gfp->mode !=MPG_MD_MONO)) {
    if (compression_ratio < 9 ) {
      gfp->mode = MPG_MD_STEREO;
    }
  }



  /****************************************************************/
  /* if a filter has not been enabled, see if we should add one: */
  /****************************************************************/
  if (gfp->lowpassfreq == 0) {
    /* If the user has not selected their own filter, add a lowpass
     * filter based on the compression ratio.  Formula based on
          44.1   /160    4.4x
          44.1   /128    5.5x      keep all bands
          44.1   /96kbs  7.3x      keep band 28
          44.1   /80kbs  8.8x      keep band 25
          44.1khz/64kbs  11x       keep band 21  22?

	  16khz/24kbs  10.7x       keep band 21
	  22kHz/32kbs  11x         keep band ?
	  22kHz/24kbs  14.7x       keep band 16
          16    16     16x         keep band 14
    */


    /* Should we use some lowpass filters? */
    int band = 1+floor(.5 + 14-18*log(compression_ratio/16.0));
    if (band < 31) {
      gfp->lowpass1 = band/31.0;
      gfp->lowpass2 = band/31.0;
    }
  }

  /****************************************************************/
  /* apply user driven filters*/
  /****************************************************************/
  if ( gfp->highpassfreq > 0 ) {
    gfp->highpass1 = 2.0*gfp->highpassfreq/gfp->out_samplerate; /* will always be >=0 */
    if ( gfp->highpasswidth >= 0 ) {
      gfp->highpass2 = 2.0*(gfp->highpassfreq+gfp->highpasswidth)/gfp->out_samplerate;
    } else {
      /* 15% above on default */
      /* gfp->highpass2 = 1.15*2.0*gfp->highpassfreq/gfp->out_samplerate;  */
      gfp->highpass2 = 1.00*2.0*gfp->highpassfreq/gfp->out_samplerate; 
    }
    gfp->highpass1 = Min( 1, gfp->highpass1 );
    gfp->highpass2 = Min( 1, gfp->highpass2 );
  }

  if ( gfp->lowpassfreq > 0 ) {
    gfp->lowpass2 = 2.0*gfp->lowpassfreq/gfp->out_samplerate; /* will always be >=0 */
    if ( gfp->lowpasswidth >= 0 ) {
      gfp->lowpass1 = 2.0*(gfp->lowpassfreq-gfp->lowpasswidth)/gfp->out_samplerate;
      if ( gfp->lowpass1 < 0 ) { /* has to be >= 0 */
	gfp->lowpass1 = 0;
      }
    } else {
      /* 15% below on default */
      /* gfp->lowpass1 = 0.85*2.0*gfp->lowpassfreq/gfp->out_samplerate;  */
      gfp->lowpass1 = 1.00*2.0*gfp->lowpassfreq/gfp->out_samplerate;
    }
    gfp->lowpass1 = Min( 1, gfp->lowpass1 );
    gfp->lowpass2 = Min( 1, gfp->lowpass2 );
  }


  /***************************************************************/
  /* compute info needed for polyphase filter                    */
  /***************************************************************/
  if (gfp->filter_type==0) {
    int band,maxband,minband;
    FLOAT8 amp,freq;
    if (gfp->lowpass1 > 0) {
      minband=999;
      maxband=-1;
      for (band=0;  band <=31 ; ++band) { 
	freq = band/31.0;
	amp = 1;
	/* this band and above will be zeroed: */
	if (freq >= gfp->lowpass2) {
	  gfp->lowpass_band= Min(gfp->lowpass_band,band);
	  amp=0;
	}
	if (gfp->lowpass1 < freq && freq < gfp->lowpass2) {
          minband = Min(minband,band);
          maxband = Max(maxband,band);
	  amp = cos((PI/2)*(gfp->lowpass1-freq)/(gfp->lowpass2-gfp->lowpass1));
	}
	/* printf("lowpass band=%i  amp=%f \n",band,amp);*/
      }
      /* compute the *actual* transition band implemented by the polyphase filter */
      if (minband==999) gfp->lowpass1 = (gfp->lowpass_band-.75)/31.0;
      else gfp->lowpass1 = (minband-.75)/31.0;
      gfp->lowpass2 = gfp->lowpass_band/31.0;
    }

    /* make sure highpass filter is within 90% of whan the effective highpass
     * frequency will be */
    if (gfp->highpass2 > 0) 
      if (gfp->highpass2 <  .9*(.75/31.0) ) {
	gfp->highpass1=0; gfp->highpass2=0;
	fprintf(stderr,"Warning: highpass filter disabled.  highpass frequency to small\n");
      }
    

    if (gfp->highpass2 > 0) {
      minband=999;
      maxband=-1;
      for (band=0;  band <=31; ++band) { 
	freq = band/31.0;
	amp = 1;
	/* this band and below will be zereod */
	if (freq <= gfp->highpass1) {
	  gfp->highpass_band = Max(gfp->highpass_band,band);
	  amp=0;
	}
	if (gfp->highpass1 < freq && freq < gfp->highpass2) {
          minband = Min(minband,band);
          maxband = Max(maxband,band);
	  amp = cos((PI/2)*(gfp->highpass2-freq)/(gfp->highpass2-gfp->highpass1));
	}
	/*	printf("highpass band=%i  amp=%f \n",band,amp);*/
      }
      /* compute the *actual* transition band implemented by the polyphase filter */
      gfp->highpass1 = gfp->highpass_band/31.0;
      if (maxband==-1) gfp->highpass2 = (gfp->highpass_band+.75)/31.0;
      else gfp->highpass2 = (maxband+.75)/31.0;
    }
    /*
    printf("lowpass band with amp=0:  %i \n",gfp->lowpass_band);
    printf("highpass band with amp=0:  %i \n",gfp->highpass_band);
    */
  }



  /***************************************************************/
  /* compute info needed for FIR filter */
  /***************************************************************/
  if (gfp->filter_type==1) {
  }




  gfp->mode_ext=MPG_MD_LR_LR;
  gfp->stereo = (gfp->mode == MPG_MD_MONO) ? 1 : 2;


  gfp->samplerate_index = SmpFrqIndex((long)gfp->out_samplerate, &gfp->version);
  if( gfp->samplerate_index < 0) {
    display_bitrates(stderr);
    exit(1);
  }
  if( (gfp->bitrate_index = BitrateIndex(gfp->brate, gfp->version,gfp->out_samplerate)) < 0) {
    display_bitrates(stderr);
    exit(1);
  }


  /* choose a min/max bitrate for VBR */
  if (gfp->VBR) {
    /* if the user didn't specify VBR_max_bitrate: */
    if (0==gfp->VBR_max_bitrate_kbps) {
      /* default max bitrate is 256kbs */
      /* we do not normally allow 320bps frams with VBR, unless: */
      gfp->VBR_max_bitrate=13;   /* default: allow 256kbs */
      if (gfp->VBR_min_bitrate_kbps>=256) gfp->VBR_max_bitrate=14;
      if (gfp->VBR_q == 0) gfp->VBR_max_bitrate=14;   /* allow 320kbs */
      if (gfp->VBR_q >= 4) gfp->VBR_max_bitrate=12;   /* max = 224kbs */
      if (gfp->VBR_q >= 8) gfp->VBR_max_bitrate=9;    /* low quality, max = 128kbs */
    }else{
      if( (gfp->VBR_max_bitrate  = BitrateIndex(gfp->VBR_max_bitrate_kbps, gfp->version,gfp->out_samplerate)) < 0) {
	display_bitrates(stderr);
	exit(1);
      }
    }
    if (0==gfp->VBR_min_bitrate_kbps) {
      gfp->VBR_min_bitrate=1;  /* 32 kbps */
    }else{
      if( (gfp->VBR_min_bitrate  = BitrateIndex(gfp->VBR_min_bitrate_kbps, gfp->version,gfp->out_samplerate)) < 0) {
	display_bitrates(stderr);
	exit(1);
      }
    }

  }


  if (gfp->VBR) gfp->quality=Min(gfp->quality,2);    /* always use quality <=2  with VBR */
  /* dont allow forced mid/side stereo for mono output */
  if (gfp->mode == MPG_MD_MONO) gfp->force_ms=0;


  /* Do not write VBR tag if VBR flag is not specified */
  if (gfp->VBR==0) gfp->bWriteVbrTag=0;

  /* some file options not allowed if output is: not specified or stdout */

  if (gfp->outPath!=NULL && gfp->outPath[0]=='-' ) {
    gfp->bWriteVbrTag=0; /* turn off VBR tag */
  }

  if (gfp->outPath==NULL || gfp->outPath[0]=='-' ) {
    id3tag.used=0;         /* turn of id3 tagging */
  }



  if (gfp->gtkflag) {
    gfp->bWriteVbrTag=0;  /* disable Xing VBR tag */
  }

  init_bit_stream_w(&bs);



  /* set internal feature flags.  USER should not access these since
   * some combinations will produce strange results */

  /* no psymodel, no noise shaping */
  if (gfp->quality==9) {
    gfp->filter_type=0;
    gfp->psymodel=0;
    gfp->quantization=0;
    gfp->noise_shaping=0;
    gfp->noise_shaping_stop=0;
    gfp->use_best_huffman=0;
  }

  if (gfp->quality==8) gfp->quality=7;

  /* use psymodel (for short block and m/s switching), but no noise shapping */
  if (gfp->quality==7) {
    gfp->filter_type=0;
    gfp->psymodel=1;
    gfp->quantization=0;
    gfp->noise_shaping=0;
    gfp->noise_shaping_stop=0;
    gfp->use_best_huffman=0;
  }

  if (gfp->quality==6) gfp->quality=5;

  if (gfp->quality==5) {
    /* the default */
    gfp->filter_type=0;
    gfp->psymodel=1;
    gfp->quantization=0;
    gfp->noise_shaping=1;
    gfp->noise_shaping_stop=0;
    gfp->use_best_huffman=0;
  }

  if (gfp->quality==4) gfp->quality=2;
  if (gfp->quality==3) gfp->quality=2;

  if (gfp->quality==2) {
    gfp->filter_type=0;
    gfp->psymodel=1;
    gfp->quantization=1;
    gfp->noise_shaping=1;
    gfp->noise_shaping_stop=0;
    gfp->use_best_huffman=1;
  }

  if (gfp->quality==1) {
    gfp->filter_type=0;
    gfp->psymodel=1;
    gfp->quantization=1;
    gfp->noise_shaping=1;
    gfp->noise_shaping_stop=1;
    gfp->use_best_huffman=1;
  }

  if (gfp->quality==0) {
    /* 0..1 quality */
    gfp->filter_type=1;         /* not yet coded */
    gfp->psymodel=1;
    gfp->quantization=1;
    gfp->noise_shaping=3;       /* not yet coded */
    gfp->noise_shaping_stop=2;  /* not yet coded */
    gfp->use_best_huffman=2;   /* not yet coded */
    exit(-99);
  }


  for (i = 0; i < SBMAX_l + 1; i++) {
    scalefac_band.l[i] =
      sfBandIndex[gfp->samplerate_index + (gfp->version * 3)].l[i];
  }
  for (i = 0; i < SBMAX_s + 1; i++) {
    scalefac_band.s[i] =
      sfBandIndex[gfp->samplerate_index + (gfp->version * 3)].s[i];
  }



  if (gfp->bWriteVbrTag)
    {
      /* Write initial VBR Header to bitstream */
      InitVbrTag(&bs,1-gfp->version,gfp->mode,gfp->samplerate_index);
    }

#ifdef HAVEGTK
  gtkflag=gfp->gtkflag;
#endif

#ifdef BRHIST
  if (gfp->VBR) {
    if (disp_brhist)
      brhist_init(gfp,1, 14);
  } else
    disp_brhist = 0;
#endif
  return;
}









/************************************************************************
 *
 * print_config
 *
 * PURPOSE:  Prints the encoding parameters used
 *
 ************************************************************************/
void lame_print_config(lame_global_flags *gfp)
{
  static const char *mode_names[4] = { "stereo", "j-stereo", "dual-ch", "single-ch" };
  FLOAT out_samplerate=gfp->out_samplerate/1000.0;
  FLOAT in_samplerate = gfp->resample_ratio*out_samplerate;
  FLOAT compression=
    (FLOAT)(gfp->stereo*16*out_samplerate)/(FLOAT)(gfp->brate);

  lame_print_version(stderr);
  if (gfp->num_channels==2 && gfp->stereo==1) {
    fprintf(stderr, "Autoconverting from stereo to mono. Setting encoding to mono mode.\n");
  }
  if (gfp->resample_ratio!=1) {
    fprintf(stderr,"Resampling:  input=%ikHz  output=%ikHz\n",
	    (int)in_samplerate,(int)out_samplerate);
  }
  if (gfp->highpass2>0.0)
    fprintf(stderr, "Using polyphase highpass filter, transition band: %.0f Hz -  %.0f Hz\n",
	    gfp->highpass1*out_samplerate*500,
	    gfp->highpass2*out_samplerate*500);
  if (gfp->lowpass1>0.0)
    fprintf(stderr, "Using polyphase lowpass filter,  transition band:  %.0f Hz - %.0f Hz\n",
	    gfp->lowpass1*out_samplerate*500,
	    gfp->lowpass2*out_samplerate*500);

  if (gfp->gtkflag) {
    fprintf(stderr, "Analyzing %s \n",gfp->inPath);
  }
  else {
    fprintf(stderr, "Encoding %s to %s\n",
	    (strcmp(gfp->inPath, "-")? gfp->inPath : "stdin"),
	    (strcmp(gfp->outPath, "-")? gfp->outPath : "stdout"));
    if (gfp->VBR)
      fprintf(stderr, "Encoding as %.1fkHz VBR(q=%i) %s MPEG%i LayerIII  qval=%i\n",
	      gfp->out_samplerate/1000.0,
	      gfp->VBR_q,mode_names[gfp->mode],2-gfp->version,gfp->quality);
    else
      fprintf(stderr, "Encoding as %.1f kHz %d kbps %s MPEG%i LayerIII (%4.1fx)  qval=%i\n",
	      gfp->out_samplerate/1000.0,gfp->brate,
	      mode_names[gfp->mode],2-gfp->version,compression,gfp->quality);
  }
  fflush(stderr);
}












/************************************************************************
*
* encodeframe()           Layer 3
*
* encode a single frame
*
************************************************************************
lame_encode_frame()


                       gr 0            gr 1
inbuf:           |--------------|---------------|-------------|
MDCT output:  |--------------|---------------|-------------|

FFT's                    <---------1024---------->
                                         <---------1024-------->



    inbuf = buffer of PCM data size=MP3 framesize
    encoder acts on inbuf[ch][0], but output is delayed by MDCTDELAY
    so the MDCT coefficints are from inbuf[ch][-MDCTDELAY]

    psy-model FFT has a 1 granule day, so we feed it data for the next granule.
    FFT is centered over granule:  224+576+224
    So FFT starts at:   576-224-MDCTDELAY

    MPEG2:  FFT ends at:  BLKSIZE+576-224-MDCTDELAY
    MPEG1:  FFT ends at:  BLKSIZE+2*576-224-MDCTDELAY    (1904)

    FFT starts at 576-224-MDCTDELAY (304)  = 576-FFTOFFSET

*/
int lame_encode_frame(lame_global_flags *gfp,
short int inbuf_l[],short int inbuf_r[],
int mf_size,char *mp3buf, int mp3buf_size)
{
  static unsigned long frameBits;
  static unsigned long bitsPerSlot;
  static FLOAT8 frac_SpF;
  static FLOAT8 slot_lag;
  static unsigned long sentBits = 0;
  FLOAT8 xr[2][2][576];
  int l3_enc[2][2][576];
  int mp3count;
  III_psy_ratio masking_ratio[2][2];    /*LR ratios */
  III_psy_ratio masking_MS_ratio[2][2]; /*MS ratios */
  III_psy_ratio (*masking)[2][2];  /*LR ratios and MS ratios*/
  III_scalefac_t scalefac[2][2];
  short int *inbuf[2];

  typedef FLOAT8 pedata[2][2];
  pedata pe,pe_MS;
  pedata *pe_use;

  int ch,gr,mean_bits;
  int bitsPerFrame;

  int check_ms_stereo;
  static FLOAT8 ms_ratio[2]={0,0};
  FLOAT8 ms_ratio_next=0;
  FLOAT8 ms_ratio_prev=0;
  static FLOAT8 ms_ener_ratio[2]={0,0};

  memset((char *) masking_ratio, 0, sizeof(masking_ratio));
  memset((char *) masking_MS_ratio, 0, sizeof(masking_MS_ratio));
  memset((char *) scalefac, 0, sizeof(scalefac));
  inbuf[0]=inbuf_l;
  inbuf[1]=inbuf_r;

  gfp->mode_ext = MPG_MD_LR_LR;

  if (gfp->frameNum==0 )  {
    /* Figure average number of 'slots' per frame. */
    FLOAT8 avg_slots_per_frame;
    FLOAT8 sampfreq =   gfp->out_samplerate/1000.0;
    int bit_rate = gfp->brate;
    sentBits = 0;
    bitsPerSlot = 8;
    avg_slots_per_frame = (bit_rate*gfp->framesize) /
           (sampfreq* bitsPerSlot);
    /* -f fast-math option causes some strange rounding here, be carefull: */
    frac_SpF  = avg_slots_per_frame - floor(avg_slots_per_frame + 1e-9);
    if (fabs(frac_SpF) < 1e-9) frac_SpF = 0;

    slot_lag  = -frac_SpF;
    gfp->padding = 1;
    if (frac_SpF==0) gfp->padding = 0;
    /* check FFT will not use a negative starting offset */
    assert(576>=FFTOFFSET);
    /* check if we have enough data for FFT */
    assert(mf_size>=(BLKSIZE+gfp->framesize-FFTOFFSET));
  }


  /********************** padding *****************************/
  switch (gfp->padding_type) {
  case 0:
    gfp->padding=0;
    break;
  case 1:
    gfp->padding=1;
    break;
  case 2:
  default:
    if (gfp->VBR) {
      gfp->padding=0;
    } else {
      if (gfp->disable_reservoir) {
	gfp->padding = 0;
	/* if the user specified --nores, dont very gfp->padding either */
	/* tiny changes in frac_SpF rounding will cause file differences */
      }else{
	if (frac_SpF != 0) {
	  if (slot_lag > (frac_SpF-1.0) ) {
	    slot_lag -= frac_SpF;
	    gfp->padding = 0;
	  }
	  else {
	    gfp->padding = 1;
	    slot_lag += (1-frac_SpF);
	  }
	}
      }
    }
  }


  /********************** status display  *****************************/
  if (!gfp->gtkflag && !gfp->silent) {
    int mod = gfp->version == 0 ? 200 : 50;
    if (gfp->frameNum%mod==0) {
      timestatus(gfp->out_samplerate,gfp->frameNum,gfp->totalframes,gfp->framesize);
#ifdef BRHIST
      if (disp_brhist)
	{
	  brhist_add_count();
	  brhist_disp();
	}
#endif
    }
  }


  if (gfp->psymodel) {
    /* psychoacoustic model
     * psy model has a 1 granule (576) delay that we must compensate for
     * (mt 6/99).
     */
    short int *bufp[2];  /* address of beginning of left & right granule */
    int blocktype[2];

    ms_ratio_prev=ms_ratio[gfp->mode_gr-1];
    for (gr=0; gr < gfp->mode_gr ; gr++) {

      for ( ch = 0; ch < gfp->stereo; ch++ )
	bufp[ch] = &inbuf[ch][576 + gr*576-FFTOFFSET];

      L3psycho_anal( gfp,bufp, gr, 
		     &ms_ratio[gr],&ms_ratio_next,&ms_ener_ratio[gr],
		     masking_ratio, masking_MS_ratio,
		     pe[gr],pe_MS[gr],blocktype);

      for ( ch = 0; ch < gfp->stereo; ch++ )
	l3_side.gr[gr].ch[ch].tt.block_type=blocktype[ch];

    }
  }else{
    for (gr=0; gr < gfp->mode_gr ; gr++)
      for ( ch = 0; ch < gfp->stereo; ch++ ) {
	l3_side.gr[gr].ch[ch].tt.block_type=NORM_TYPE;
	pe[gr][ch]=700;
      }
  }


  /* block type flags */
  for( gr = 0; gr < gfp->mode_gr; gr++ ) {
    for ( ch = 0; ch < gfp->stereo; ch++ ) {
      gr_info *cod_info = &l3_side.gr[gr].ch[ch].tt;
      cod_info->mixed_block_flag = 0;     /* never used by this model */
      if (cod_info->block_type == NORM_TYPE )
	cod_info->window_switching_flag = 0;
      else
	cod_info->window_switching_flag = 1;
    }
  }

  /* polyphase filtering / mdct */
  mdct_sub48(gfp,inbuf[0], inbuf[1], xr, &l3_side);

  /* use m/s gfp->stereo? */
  check_ms_stereo =  (gfp->mode == MPG_MD_JOINT_STEREO);
  if (check_ms_stereo) {
    /* make sure block type is the same in each channel */
    check_ms_stereo =
      (l3_side.gr[0].ch[0].tt.block_type==l3_side.gr[0].ch[1].tt.block_type) &&
      (l3_side.gr[1].ch[0].tt.block_type==l3_side.gr[1].ch[1].tt.block_type);
  }
  if (check_ms_stereo) {
    /* ms_ratio = is like the ratio of side_energy/total_energy */
    FLOAT8 ms_ratio_ave,ms_ener_ratio_ave;
    /*     ms_ratio_ave = .5*(ms_ratio[0] + ms_ratio[1]);*/
    ms_ratio_ave = .25*(ms_ratio[0] + ms_ratio[1]+
			 ms_ratio_prev + ms_ratio_next);
    ms_ener_ratio_ave = .5*(ms_ener_ratio[0]+ms_ener_ratio[1]);
    if ( ms_ratio_ave <.35 /*&& ms_ener_ratio_ave<.75*/ ) gfp->mode_ext = MPG_MD_MS_LR;
  }
  if (gfp->force_ms) gfp->mode_ext = MPG_MD_MS_LR;


#ifdef HAVEGTK
  if (gfp->gtkflag) {
    int j;
    for ( gr = 0; gr < gfp->mode_gr; gr++ ) {
      for ( ch = 0; ch < gfp->stereo; ch++ ) {
	pinfo->ms_ratio[gr]=ms_ratio[gr];
	pinfo->ms_ener_ratio[gr]=ms_ener_ratio[gr];
	pinfo->blocktype[gr][ch]=
	  l3_side.gr[gr].ch[ch].tt.block_type;
	for ( j = 0; j < 576; j++ ) pinfo->xr[gr][ch][j]=xr[gr][ch][j];
	/* if MS stereo, switch to MS psy data */
	if (gfp->mode_ext==MPG_MD_MS_LR) {
	  pinfo->pe[gr][ch]=pinfo->pe[gr][ch+2];
	  pinfo->ers[gr][ch]=pinfo->ers[gr][ch+2];
	  memcpy(pinfo->energy[gr][ch],pinfo->energy[gr][ch+2],
		 sizeof(pinfo->energy[gr][ch]));
	}
      }
    }
  }
#endif




  /* bit and noise allocation */
  if (MPG_MD_MS_LR == gfp->mode_ext) {
    masking = &masking_MS_ratio;    /* use MS masking */
    pe_use=&pe_MS;
  } else {
    masking = &masking_ratio;    /* use LR masking */
    pe_use=&pe;
  }


  /*
  VBR_iteration_loop_new( gfp,*pe_use, ms_ratio, xr, masking, &l3_side, l3_enc,
  	  &scalefac);
  */


  if (gfp->VBR) {
    VBR_iteration_loop( gfp,*pe_use, ms_ratio, xr, *masking, &l3_side, l3_enc,
			scalefac);
  }else{
    iteration_loop( gfp,*pe_use, ms_ratio, xr, *masking, &l3_side, l3_enc,
		    scalefac);
  }




#ifdef BRHIST
  brhist_temp[gfp->bitrate_index]++;
#endif


  /*  write the frame to the bitstream  */
  getframebits(gfp,&bitsPerFrame,&mean_bits);
  III_format_bitstream( gfp,bitsPerFrame, l3_enc, &l3_side,
			scalefac, &bs);


  frameBits = bs.totbit - sentBits;


  if ( frameBits % bitsPerSlot )   /* a program failure */
    fprintf( stderr, "Sent %ld bits = %ld slots plus %ld\n",
	     frameBits, frameBits/bitsPerSlot,
	     frameBits%bitsPerSlot );
  sentBits += frameBits;

  /* copy mp3 bit buffer into array */
  mp3count = copy_buffer(mp3buf,mp3buf_size,&bs);

  if (gfp->bWriteVbrTag) AddVbrFrame((int)(sentBits/8));

#ifdef HAVEGTK
  if (gfp->gtkflag) {
    int j;
    for ( ch = 0; ch < gfp->stereo; ch++ ) {
      for ( j = 0; j < FFTOFFSET; j++ )
	pinfo->pcmdata[ch][j] = pinfo->pcmdata[ch][j+gfp->framesize];
      for ( j = FFTOFFSET; j < 1600; j++ ) {
	pinfo->pcmdata[ch][j] = inbuf[ch][j-FFTOFFSET];
      }
    }
  }
#endif
  gfp->frameNum++;

  return mp3count;
}



int fill_buffer_resample(lame_global_flags *gfp,short int *outbuf,int desired_len,
        short int *inbuf,int len,int *num_used,int ch) {

  static FLOAT8 itime[2];
#define OLDBUFSIZE 5
  static short int inbuf_old[2][OLDBUFSIZE];
  static int init[2]={0,0};
  int i,j=0,k,linear,value;

  if (gfp->frameNum==0 && !init[ch]) {
    init[ch]=1;
    itime[ch]=0;
    memset((char *) inbuf_old[ch], 0, sizeof(short int)*OLDBUFSIZE);
  }
  if (gfp->frameNum!=0) init[ch]=0; /* reset, for next time framenum=0 */


  /* if downsampling by an integer multiple, use linear resampling,
   * otherwise use quadratic */
  linear = ( fabs(gfp->resample_ratio - floor(.5+gfp->resample_ratio)) < .0001 );

  /* time of j'th element in inbuf = itime + j/ifreq; */
  /* time of k'th element in outbuf   =  j/ofreq */
  for (k=0;k<desired_len;k++) {
    int y0,y1,y2,y3;
    FLOAT8 x0,x1,x2,x3;
    FLOAT8 time0;

    time0 = k*gfp->resample_ratio;       /* time of k'th output sample */
    j = floor( time0 -itime[ch]  );
    /* itime[ch] + j;    */            /* time of j'th input sample */
    if (j+2 >= len) break;             /* not enough data in input buffer */

    x1 = time0-(itime[ch]+j);
    x2 = x1-1;
    y1 = (j<0) ? inbuf_old[ch][OLDBUFSIZE+j] : inbuf[j];
    y2 = ((1+j)<0) ? inbuf_old[ch][OLDBUFSIZE+1+j] : inbuf[1+j];

    /* linear resample */
    if (linear) {
      outbuf[k] = floor(.5 +  (y2*x1-y1*x2) );
    } else {
      /* quadratic */
      x0 = x1+1;
      x3 = x1-2;
      y0 = ((j-1)<0) ? inbuf_old[ch][OLDBUFSIZE+(j-1)] : inbuf[j-1];
      y3 = ((j+2)<0) ? inbuf_old[ch][OLDBUFSIZE+(j+2)] : inbuf[j+2];
      value = floor(.5 +
			-y0*x1*x2*x3/6 + y1*x0*x2*x3/2 - y2*x0*x1*x3/2 +y3*x0*x1*x2/6
			);
      if (value > 32767) outbuf[k]=32767;
      else if (value < -32767) outbuf[k]=-32767;
      else outbuf[k]=value;

      /*
      printf("k=%i  new=%i   [ %i %i %i %i ]\n",k,outbuf[k],
	     y0,y1,y2,y3);
      */
    }
  }


  /* k = number of samples added to outbuf */
  /* last k sample used data from j,j+1, or j+1 overflowed buffer */
  /* remove num_used samples from inbuf: */
  *num_used = Min(len,j+2);
  itime[ch] += *num_used - k*gfp->resample_ratio;
  for (i=0;i<OLDBUFSIZE;i++)
    inbuf_old[ch][i]=inbuf[*num_used + i -OLDBUFSIZE];
  return k;
}




int fill_buffer(lame_global_flags *gfp,short int *outbuf,int desired_len,short int *inbuf,int len) {
  int j;
  j=Min(desired_len,len);
  memcpy( (char *) outbuf,(char *)inbuf,sizeof(short int)*j);
  return j;
}




/*
 * THE MAIN LAME ENCODING INTERFACE
 * mt 3/00
 *
 * input pcm data, output (maybe) mp3 frames.
 * This routine handles all buffering, resampling and filtering for you.
 * The required mp3buffer_size can be computed from num_samples,
 * samplerate and encoding rate, but here is a worst case estimate:
 *
 * mp3buffer_size in bytes = 1.25*num_samples + 7200
 *
 * return code = number of bytes output in mp3buffer.  can be 0
*/
int lame_encode_buffer(lame_global_flags *gfp,
   short int buffer_l[], short int buffer_r[],int nsamples,
   char *mp3buf, int mp3buf_size)
{
  static int frame_buffered=0;
  int mp3size=0,ret,i,ch,mf_needed;

  short int *in_buffer[2];
  in_buffer[0] = buffer_l;
  in_buffer[1] = buffer_r;

  /* some sanity checks */
  assert(ENCDELAY>=MDCTDELAY);
  assert(BLKSIZE-FFTOFFSET >= 0);
  mf_needed = BLKSIZE+gfp->framesize-FFTOFFSET;
  assert(MFSIZE>=mf_needed);

  /* The reason for
   *       int mf_samples_to_encode = ENCDELAY + 288;
   * ENCDELAY = internal encoder delay.  And then we have to add 288
   * because of the 50% MDCT overlap.  A 576 MDCT granule decodes to
   * 1152 samples.  To synthesize the 576 samples centered under this granule
   * we need the previous granule for the first 288 samples (no problem), and
   * the next granule for the next 288 samples (not possible if this is last
   * granule).  So we need to pad with 288 samples to make sure we can
   * encode the 576 samples we are interested in.
   */
  if (gfp->frameNum==0 && !frame_buffered) {
    memset((char *) mfbuf, 0, sizeof(mfbuf));
    frame_buffered=1;
    mf_samples_to_encode = ENCDELAY+288;
    mf_size=ENCDELAY-MDCTDELAY;  /* we pad input with this many 0's */
  }
  if (gfp->frameNum==1) {
    /* reset, for the next time frameNum==0 */
    frame_buffered=0;
  }

  if (gfp->num_channels==2  && gfp->stereo==1) {
    /* downsample to mono */
    for (i=0; i<nsamples; ++i) {
      in_buffer[0][i]=((int)in_buffer[0][i]+(int)in_buffer[1][i])/2;
      in_buffer[1][i]=0;
    }
  }


  while (nsamples > 0) {
    int n_in=0;
    int n_out=0;
    /* copy in new samples */
    for (ch=0; ch<gfp->stereo; ch++) {
      if (gfp->resample_ratio!=1)  {
	n_out=fill_buffer_resample(gfp,&mfbuf[ch][mf_size],gfp->framesize,
					  in_buffer[ch],nsamples,&n_in,ch);
      } else {
	n_out=fill_buffer(gfp,&mfbuf[ch][mf_size],gfp->framesize,in_buffer[ch],nsamples);
	n_in = n_out;
      }
      in_buffer[ch] += n_in;
    }


    nsamples -= n_in;
    mf_size += n_out;
    assert(mf_size<=MFSIZE);
    mf_samples_to_encode += n_out;

    if (mf_size >= mf_needed) {
      /* encode the frame */
      ret = lame_encode_frame(gfp,mfbuf[0],mfbuf[1],mf_size,mp3buf,mp3buf_size);
      if (ret == -1) {
	/* fatel error: mp3buffer was too small */
	return -1;
      }
      mp3buf += ret;
      mp3size += ret;

      /* shift out old samples */
      mf_size -= gfp->framesize;
      mf_samples_to_encode -= gfp->framesize;
      for (ch=0; ch<gfp->stereo; ch++)
	for (i=0; i<mf_size; i++)
	  mfbuf[ch][i]=mfbuf[ch][i+gfp->framesize];
    }
  }
  assert(nsamples==0);
  return mp3size;
}




int lame_encode_buffer_interleaved(lame_global_flags *gfp,
   short int buffer[], int nsamples, char *mp3buf, int mp3buf_size)
{
  static int frame_buffered=0;
  int mp3size=0,ret,i,ch,mf_needed;

  /* some sanity checks */
  assert(ENCDELAY>=MDCTDELAY);
  assert(BLKSIZE-FFTOFFSET >= 0);
  mf_needed = BLKSIZE+gfp->framesize-FFTOFFSET;
  assert(MFSIZE>=mf_needed);

  if (gfp->num_channels == 1) {
    return lame_encode_buffer(gfp,buffer, NULL ,nsamples,mp3buf,mp3buf_size);
  }

  if (gfp->resample_ratio!=1)  {
    short int *buffer_l;
    short int *buffer_r;
    buffer_l=malloc(sizeof(short int)*nsamples);
    buffer_r=malloc(sizeof(short int)*nsamples);
    if (buffer_l == NULL || buffer_r == NULL) {
      return -1;
    }
    for (i=0; i<nsamples; i++) {
      buffer_l[i]=buffer[2*i];
      buffer_r[i]=buffer[2*i+1];
    }
    ret = lame_encode_buffer(gfp,buffer_l,buffer_r,nsamples,mp3buf,mp3buf_size);
    free(buffer_l);
    free(buffer_r);
    return ret;
  }


  if (gfp->frameNum==0 && !frame_buffered) {
    memset((char *) mfbuf, 0, sizeof(mfbuf));
    frame_buffered=1;
    mf_samples_to_encode = ENCDELAY+288;
    mf_size=ENCDELAY-MDCTDELAY;  /* we pad input with this many 0's */
  }
  if (gfp->frameNum==1) {
    /* reset, for the next time frameNum==0 */
    frame_buffered=0;
  }

  if (gfp->num_channels==2  && gfp->stereo==1) {
    /* downsample to mono */
    for (i=0; i<nsamples; ++i) {
      buffer[2*i]=((int)buffer[2*i]+(int)buffer[2*i+1])/2;
      buffer[2*i+1]=0;
    }
  }


  while (nsamples > 0) {
    int n_out;
    /* copy in new samples */
    n_out = Min(gfp->framesize,nsamples);
    for (i=0; i<n_out; ++i) {
      mfbuf[0][mf_size+i]=buffer[2*i];
      mfbuf[1][mf_size+i]=buffer[2*i+1];
    }
    buffer += 2*n_out;

    nsamples -= n_out;
    mf_size += n_out;
    assert(mf_size<=MFSIZE);
    mf_samples_to_encode += n_out;

    if (mf_size >= mf_needed) {
      /* encode the frame */
      ret = lame_encode_frame(gfp,mfbuf[0],mfbuf[1],mf_size,mp3buf,mp3buf_size);
      if (ret == -1) {
	/* fatel error: mp3buffer was too small */
	return -1;
      }
      mp3buf += ret;
      mp3size += ret;

      /* shift out old samples */
      mf_size -= gfp->framesize;
      mf_samples_to_encode -= gfp->framesize;
      for (ch=0; ch<gfp->stereo; ch++)
	for (i=0; i<mf_size; i++)
	  mfbuf[ch][i]=mfbuf[ch][i+gfp->framesize];
    }
  }
  assert(nsamples==0);
  return mp3size;
}














/* old LAME interface */
/* With this interface, it is the users responsibilty to keep track of the
 * buffered, unencoded samples.  Thus mf_samples_to_encode is not incremented.
 *
 * lame_encode() is also used to flush the PCM input buffer by
 * lame_encode_finish()
 */
int lame_encode(lame_global_flags *gfp, short int in_buffer[2][1152],char *mp3buf,int size){
  int imp3,save;
  save = mf_samples_to_encode;
  imp3= lame_encode_buffer(gfp,in_buffer[0],in_buffer[1],576*gfp->mode_gr,
        mp3buf,size);
  mf_samples_to_encode = save;
  return imp3;
}




/* initialize mp3 encoder */
void lame_init(lame_global_flags *gfp)
{

  /*
   *  Disable floating point exepctions
   */
#ifdef __FreeBSD__
# include <floatingpoint.h>
  {
  /* seet floating point mask to the Linux default */
  fp_except_t mask;
  mask=fpgetmask();
  /* if bit is set, we get SIGFPE on that error! */
  fpsetmask(mask & ~(FP_X_INV|FP_X_DZ));
  /*  fprintf(stderr,"FreeBSD mask is 0x%x\n",mask); */
  }
#endif
#if defined(__riscos__) && !defined(ABORTFP)
  /* Disable FPE's under RISC OS */
  /* if bit is set, we disable trapping that error! */
  /*   _FPE_IVO : invalid operation */
  /*   _FPE_DVZ : divide by zero */
  /*   _FPE_OFL : overflow */
  /*   _FPE_UFL : underflow */
  /*   _FPE_INX : inexact */
  DisableFPETraps( _FPE_IVO | _FPE_DVZ | _FPE_OFL );
#endif


  /*
   *  Debugging stuff
   *  The default is to ignore FPE's, unless compiled with -DABORTFP
   *  so add code below to ENABLE FPE's.
   */

#if defined(ABORTFP) && !defined(__riscos__)
#if defined(_MSC_VER)
  {
	#include <float.h>
	unsigned int mask;
	mask=_controlfp( 0, 0 );
	mask&=~(_EM_OVERFLOW|_EM_UNDERFLOW|_EM_ZERODIVIDE|_EM_INVALID);
	mask=_controlfp( mask, _MCW_EM );
	}
#elif defined(__CYGWIN__)
#  define _FPU_GETCW(cw) __asm__ ("fnstcw %0" : "=m" (*&cw))
#  define _FPU_SETCW(cw) __asm__ ("fldcw %0" : : "m" (*&cw))

#  define _EM_INEXACT     0x00000001 /* inexact (precision) */
#  define _EM_UNDERFLOW   0x00000002 /* underflow */
#  define _EM_OVERFLOW    0x00000004 /* overflow */
#  define _EM_ZERODIVIDE  0x00000008 /* zero divide */
#  define _EM_INVALID     0x00000010 /* invalid */
  {
    unsigned int mask;
    _FPU_GETCW(mask);
    /* Set the FPU control word to abort on most FPEs */
    mask &= ~(_EM_UNDERFLOW | _EM_OVERFLOW | _EM_ZERODIVIDE | _EM_INVALID);
    _FPU_SETCW(mask);
  }
# else
  {
#  include <fpu_control.h>
#ifndef _FPU_GETCW
#define _FPU_GETCW(cw) __asm__ ("fnstcw %0" : "=m" (*&cw))
#endif
#ifndef _FPU_SETCW
#define _FPU_SETCW(cw) __asm__ ("fldcw %0" : : "m" (*&cw))
#endif
    unsigned int mask;
    _FPU_GETCW(mask);
    /* Set the Linux mask to abort on most FPE's */
    /* if bit is set, we _mask_ SIGFPE on that error! */
    /*  mask &= ~( _FPU_MASK_IM | _FPU_MASK_ZM | _FPU_MASK_OM | _FPU_MASK_UM );*/
    mask &= ~( _FPU_MASK_IM | _FPU_MASK_ZM | _FPU_MASK_OM );
    _FPU_SETCW(mask);
  }
#endif
#endif /* ABORTFP && !__riscos__ */



  /* Global flags.  set defaults here */
  gfp->allow_diff_short=0;
  gfp->ATHonly=0;
  gfp->noATH=0;
  gfp->bWriteVbrTag=1;
  gfp->cwlimit=0;
  gfp->disable_reservoir=0;
  gfp->experimentalX = 0;
  gfp->experimentalY = 0;
  gfp->experimentalZ = 0;
  gfp->frameNum=0;
  gfp->gtkflag=0;
  gfp->quality=5;
  gfp->input_format=sf_unknown;

  gfp->filter_type=0;
  gfp->lowpassfreq=0;
  gfp->highpassfreq=0;
  gfp->lowpasswidth=-1;
  gfp->highpasswidth=-1;
  gfp->lowpass1=0;
  gfp->lowpass2=0;
  gfp->highpass1=0;
  gfp->highpass2=0;
  gfp->lowpass_band=32;
  gfp->highpass_band=-1;

  gfp->no_short_blocks=0;
  gfp->resample_ratio=1;
  gfp->padding_type=2;
  gfp->padding=0;
  gfp->swapbytes=0;
  gfp->silent=0;
  gfp->totalframes=0;
  gfp->VBR=0;
  gfp->VBR_q=4;
  gfp->VBR_min_bitrate_kbps=0;
  gfp->VBR_max_bitrate_kbps=0;
  gfp->VBR_min_bitrate=1;
  gfp->VBR_max_bitrate=13;


  gfp->version = 1;   /* =1   Default: MPEG-1 */
  gfp->mode = MPG_MD_JOINT_STEREO;
  gfp->mode_fixed=0;
  gfp->force_ms=0;
  gfp->brate=0;
  gfp->copyright=0;
  gfp->original=1;
  gfp->extension=0;
  gfp->error_protection=0;
  gfp->emphasis=0;
  gfp->in_samplerate=1000*44.1;
  gfp->out_samplerate=0;
  gfp->num_channels=2;
  gfp->num_samples=MAX_U_32_NUM;

  gfp->inPath=NULL;
  gfp->outPath=NULL;
  id3tag.used=0;

}



/*****************************************************************/
/* flush internal mp3 buffers,                                   */
/*****************************************************************/
int lame_encode_finish(lame_global_flags *gfp,char *mp3buffer, int mp3buffer_size)
{
  int imp3,mp3count,mp3buffer_size_remaining;
  short int buffer[2][1152];
  memset((char *)buffer,0,sizeof(buffer));
  mp3count = 0;

  while (mf_samples_to_encode > 0) {

    mp3buffer_size_remaining = mp3buffer_size - mp3count;
    /* if user specifed buffer size = 0, dont check size */
    if (mp3buffer_size == 0) mp3buffer_size_remaining=0;  
    imp3=lame_encode(gfp,buffer,mp3buffer,mp3buffer_size_remaining);

    if (imp3 == -1) {
      /* fatel error: mp3buffer too small */
      desalloc_buffer(&bs);    /* Deallocate all buffers */
      return -1;
    }
    mp3buffer += imp3;
    mp3count += imp3;
    mf_samples_to_encode -= gfp->framesize;
  }


  gfp->frameNum--;
  if (!gfp->gtkflag && !gfp->silent) {
      timestatus(gfp->out_samplerate,gfp->frameNum,gfp->totalframes,gfp->framesize);
#ifdef BRHIST
      if (disp_brhist)
	{
	  brhist_add_count();
	  brhist_disp();
	  brhist_disp_total(gfp);
	}
#endif
      fprintf(stderr,"\n");
      fflush(stderr);
  }


  III_FlushBitstream();
  mp3buffer_size_remaining = mp3buffer_size - mp3count;
  /* if user specifed buffer size = 0, dont check size */
  if (mp3buffer_size == 0) mp3buffer_size_remaining=0;  

  imp3= copy_buffer(mp3buffer,mp3buffer_size_remaining,&bs);
  if (imp3 == -1) {
    /* fatel error: mp3buffer too small */
    desalloc_buffer(&bs);    /* Deallocate all buffers */
    return -1;
  }

  mp3count += imp3;
  desalloc_buffer(&bs);    /* Deallocate all buffers */
  return mp3count;
}


/*****************************************************************/
/* write VBR Xing header, and ID3 tag, if asked for               */
/*****************************************************************/
void lame_mp3_tags(lame_global_flags *gfp)
{
  if (gfp->bWriteVbrTag)
    {
      /* Calculate relative quality of VBR stream
       * 0=best, 100=worst */
      int nQuality=gfp->VBR_q*100/9;
      /* Write Xing header again */
      PutVbrTag(gfp->outPath,nQuality,1-gfp->version);
    }


  /* write an ID3 tag  */
  if(id3tag.used) {
    id3_buildtag(&id3tag);
    id3_writetag(gfp->outPath, &id3tag);
  }
}


void lame_version(lame_global_flags *gfp,char *ostring) {
  strncpy(ostring,get_lame_version(),20);
}



/* ==== layer3.c ==== */
#ifdef HAVEMPGLIB
/* 
 * Mpeg Layer-3 audio decoder 
 * --------------------------
 * copyright (c) 1995,1996,1997 by Michael Hipp.
 * All rights reserved. See also 'README'
 */ 

#include <stdlib.h>
#include "mpg123.h"
#include "mpglib.h"
#include "huffman.h"
#ifdef HAVEGTK
#include "../gtkanal.h"
#endif

extern struct mpstr *gmp;

#define MPEG1


static real ispow[8207];
static real aa_ca[8],aa_cs[8];
static real COS1[12][6];
static real win[4][36];
static real win1[4][36];
static real gainpow2[256+118+4];
static real COS9[9];
static real COS6_1,COS6_2;
static real tfcos36[9];
static real tfcos12[3];

struct bandInfoStruct {
  short longIdx[23];
  short longDiff[22];
  short shortIdx[14];
  short shortDiff[13];
};

int longLimit[9][23];
int shortLimit[9][14];

struct bandInfoStruct bandInfo[9] = { 

/* MPEG 1.0 */
 { {0,4,8,12,16,20,24,30,36,44,52,62,74, 90,110,134,162,196,238,288,342,418,576},
   {4,4,4,4,4,4,6,6,8, 8,10,12,16,20,24,28,34,42,50,54, 76,158},
   {0,4*3,8*3,12*3,16*3,22*3,30*3,40*3,52*3,66*3, 84*3,106*3,136*3,192*3},
   {4,4,4,4,6,8,10,12,14,18,22,30,56} } ,

 { {0,4,8,12,16,20,24,30,36,42,50,60,72, 88,106,128,156,190,230,276,330,384,576},
   {4,4,4,4,4,4,6,6,6, 8,10,12,16,18,22,28,34,40,46,54, 54,192},
   {0,4*3,8*3,12*3,16*3,22*3,28*3,38*3,50*3,64*3, 80*3,100*3,126*3,192*3},
   {4,4,4,4,6,6,10,12,14,16,20,26,66} } ,

 { {0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550,576} ,
   {4,4,4,4,4,4,6,6,8,10,12,16,20,24,30,38,46,56,68,84,102, 26} ,
   {0,4*3,8*3,12*3,16*3,22*3,30*3,42*3,58*3,78*3,104*3,138*3,180*3,192*3} ,
   {4,4,4,4,6,8,12,16,20,26,34,42,12} }  ,

/* MPEG 2.0 */
 { {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
   {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54 } ,
   {0,4*3,8*3,12*3,18*3,24*3,32*3,42*3,56*3,74*3,100*3,132*3,174*3,192*3} ,
   {4,4,4,6,6,8,10,14,18,26,32,42,18 } } ,
                                             /* docs: 332. mpg123: 330 */
 { {0,6,12,18,24,30,36,44,54,66,80,96,114,136,162,194,232,278,332,394,464,540,576},
   {6,6,6,6,6,6,8,10,12,14,16,18,22,26,32,38,46,52,64,70,76,36 } ,
   {0,4*3,8*3,12*3,18*3,26*3,36*3,48*3,62*3,80*3,104*3,136*3,180*3,192*3} ,
   {4,4,4,6,8,10,12,14,18,24,32,44,12 } } ,

 { {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
   {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54 },
   {0,4*3,8*3,12*3,18*3,26*3,36*3,48*3,62*3,80*3,104*3,134*3,174*3,192*3},
   {4,4,4,6,8,10,12,14,18,24,30,40,18 } } ,
/* MPEG 2.5 */
 { {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576} ,
   {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
   {0,12,24,36,54,78,108,144,186,240,312,402,522,576},
   {4,4,4,6,8,10,12,14,18,24,30,40,18} },
 { {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576} ,
   {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
   {0,12,24,36,54,78,108,144,186,240,312,402,522,576},
   {4,4,4,6,8,10,12,14,18,24,30,40,18} },
 { {0,12,24,36,48,60,72,88,108,132,160,192,232,280,336,400,476,566,568,570,572,574,576},
   {12,12,12,12,12,12,16,20,24,28,32,40,48,56,64,76,90,2,2,2,2,2},
   {0, 24, 48, 72,108,156,216,288,372,480,486,492,498,576},
   {8,8,8,12,16,20,24,28,36,2,2,2,26} } ,
};

static int mapbuf0[9][152];
static int mapbuf1[9][156];
static int mapbuf2[9][44];
static int *map[9][3];
static int *mapend[9][3];

static unsigned int n_slen2[512]; /* MPEG 2.0 slen for 'normal' mode */
static unsigned int i_slen2[256]; /* MPEG 2.0 slen for intensity stereo */

static real tan1_1[16],tan2_1[16],tan1_2[16],tan2_2[16];
static real pow1_1[2][16],pow2_1[2][16],pow1_2[2][16],pow2_2[2][16];

static unsigned int get1bit(void)
{
  unsigned char rval;
  rval = *wordpointer << bitindex;

  bitindex++;
  wordpointer += (bitindex>>3);
  bitindex &= 7;

  return rval>>7;
}




/* 
 * init tables for layer-3 
 */
void init_layer3(int down_sample_sblimit)
{
  int i,j,k,l;

  for(i=-256;i<118+4;i++)
    gainpow2[i+256] = pow((double)2.0,-0.25 * (double) (i+210) );

  for(i=0;i<8207;i++)
    ispow[i] = pow((double)i,(double)4.0/3.0);

  for (i=0;i<8;i++)
  {
    static double Ci[8]={-0.6,-0.535,-0.33,-0.185,-0.095,-0.041,-0.0142,-0.0037};
    double sq=sqrt(1.0+Ci[i]*Ci[i]);
    aa_cs[i] = 1.0/sq;
    aa_ca[i] = Ci[i]/sq;
  }

  for(i=0;i<18;i++)
  {
    win[0][i]    = win[1][i]    = 0.5 * sin( M_PI / 72.0 * (double) (2*(i+0) +1) ) / cos ( M_PI * (double) (2*(i+0) +19) / 72.0 );
    win[0][i+18] = win[3][i+18] = 0.5 * sin( M_PI / 72.0 * (double) (2*(i+18)+1) ) / cos ( M_PI * (double) (2*(i+18)+19) / 72.0 );
  }
  for(i=0;i<6;i++)
  {
    win[1][i+18] = 0.5 / cos ( M_PI * (double) (2*(i+18)+19) / 72.0 );
    win[3][i+12] = 0.5 / cos ( M_PI * (double) (2*(i+12)+19) / 72.0 );
    win[1][i+24] = 0.5 * sin( M_PI / 24.0 * (double) (2*i+13) ) / cos ( M_PI * (double) (2*(i+24)+19) / 72.0 );
    win[1][i+30] = win[3][i] = 0.0;
    win[3][i+6 ] = 0.5 * sin( M_PI / 24.0 * (double) (2*i+1) )  / cos ( M_PI * (double) (2*(i+6 )+19) / 72.0 );
  }

  for(i=0;i<9;i++)
    COS9[i] = cos( M_PI / 18.0 * (double) i);

  for(i=0;i<9;i++)
    tfcos36[i] = 0.5 / cos ( M_PI * (double) (i*2+1) / 36.0 );
  for(i=0;i<3;i++)
    tfcos12[i] = 0.5 / cos ( M_PI * (double) (i*2+1) / 12.0 );

  COS6_1 = cos( M_PI / 6.0 * (double) 1);
  COS6_2 = cos( M_PI / 6.0 * (double) 2);

  for(i=0;i<12;i++)
  {
    win[2][i]  = 0.5 * sin( M_PI / 24.0 * (double) (2*i+1) ) / cos ( M_PI * (double) (2*i+7) / 24.0 );
    for(j=0;j<6;j++)
      COS1[i][j] = cos( M_PI / 24.0 * (double) ((2*i+7)*(2*j+1)) );
  }

  for(j=0;j<4;j++) {
    static int len[4] = { 36,36,12,36 };
    for(i=0;i<len[j];i+=2)
      win1[j][i] = + win[j][i];
    for(i=1;i<len[j];i+=2)
      win1[j][i] = - win[j][i];
  }

  for(i=0;i<16;i++)
  {
    double t = tan( (double) i * M_PI / 12.0 );
    tan1_1[i] = t / (1.0+t);
    tan2_1[i] = 1.0 / (1.0 + t);
    tan1_2[i] = M_SQRT2 * t / (1.0+t);
    tan2_2[i] = M_SQRT2 / (1.0 + t);

    for(j=0;j<2;j++) {
      double base = pow(2.0,-0.25*(j+1.0));
      double p1=1.0,p2=1.0;
      if(i > 0) {
        if( i & 1 )
          p1 = pow(base,(i+1.0)*0.5);
        else
          p2 = pow(base,i*0.5);
      }
      pow1_1[j][i] = p1;
      pow2_1[j][i] = p2;
      pow1_2[j][i] = M_SQRT2 * p1;
      pow2_2[j][i] = M_SQRT2 * p2;
    }
  }

  for(j=0;j<9;j++)
  {
   struct bandInfoStruct *bi = &bandInfo[j];
   int *mp;
   int cb,lwin;
   short *bdf;

   mp = map[j][0] = mapbuf0[j];
   bdf = bi->longDiff;
   for(i=0,cb = 0; cb < 8 ; cb++,i+=*bdf++) {
     *mp++ = (*bdf) >> 1;
     *mp++ = i;
     *mp++ = 3;
     *mp++ = cb;
   }
   bdf = bi->shortDiff+3;
   for(cb=3;cb<13;cb++) {
     int l = (*bdf++) >> 1;
     for(lwin=0;lwin<3;lwin++) {
       *mp++ = l;
       *mp++ = i + lwin;
       *mp++ = lwin;
       *mp++ = cb;
     }
     i += 6*l;
   }
   mapend[j][0] = mp;

   mp = map[j][1] = mapbuf1[j];
   bdf = bi->shortDiff+0;
   for(i=0,cb=0;cb<13;cb++) {
     int l = (*bdf++) >> 1;
     for(lwin=0;lwin<3;lwin++) {
       *mp++ = l;
       *mp++ = i + lwin;
       *mp++ = lwin;
       *mp++ = cb;
     }
     i += 6*l;
   }
   mapend[j][1] = mp;

   mp = map[j][2] = mapbuf2[j];
   bdf = bi->longDiff;
   for(cb = 0; cb < 22 ; cb++) {
     *mp++ = (*bdf++) >> 1;
     *mp++ = cb;
   }
   mapend[j][2] = mp;

  }

  for(j=0;j<9;j++) {
    for(i=0;i<23;i++) {
      longLimit[j][i] = (bandInfo[j].longIdx[i] - 1 + 8) / 18 + 1;
      if(longLimit[j][i] > (down_sample_sblimit) )
        longLimit[j][i] = down_sample_sblimit;
    }
    for(i=0;i<14;i++) {
      shortLimit[j][i] = (bandInfo[j].shortIdx[i] - 1) / 18 + 1;
      if(shortLimit[j][i] > (down_sample_sblimit) )
        shortLimit[j][i] = down_sample_sblimit;
    }
  }

  for(i=0;i<5;i++) {
    for(j=0;j<6;j++) {
      for(k=0;k<6;k++) {
        int n = k + j * 6 + i * 36;
        i_slen2[n] = i|(j<<3)|(k<<6)|(3<<12);
      }
    }
  }
  for(i=0;i<4;i++) {
    for(j=0;j<4;j++) {
      for(k=0;k<4;k++) {
        int n = k + j * 4 + i * 16;
        i_slen2[n+180] = i|(j<<3)|(k<<6)|(4<<12);
      }
    }
  }
  for(i=0;i<4;i++) {
    for(j=0;j<3;j++) {
      int n = j + i * 3;
      i_slen2[n+244] = i|(j<<3) | (5<<12);
      n_slen2[n+500] = i|(j<<3) | (2<<12) | (1<<15);
    }
  }

  for(i=0;i<5;i++) {
    for(j=0;j<5;j++) {
      for(k=0;k<4;k++) {
        for(l=0;l<4;l++) {
          int n = l + k * 4 + j * 16 + i * 80;
          n_slen2[n] = i|(j<<3)|(k<<6)|(l<<9)|(0<<12);
        }
      }
    }
  }
  for(i=0;i<5;i++) {
    for(j=0;j<5;j++) {
      for(k=0;k<4;k++) {
        int n = k + j * 4 + i * 20;
        n_slen2[n+400] = i|(j<<3)|(k<<6)|(1<<12);
      }
    }
  }
}

/*
 * read additional side information
 */
#ifdef MPEG1 
static void III_get_side_info_1(struct III_sideinfo *si,int stereo,
 int ms_stereo,long sfreq,int single)
{
   int ch, gr;
   int powdiff = (single == 3) ? 4 : 0;

   si->main_data_begin = getbits(9);
   if (stereo == 1)
     si->private_bits = getbits_fast(5);
   else 
     si->private_bits = getbits_fast(3);

   for (ch=0; ch<stereo; ch++) {
       si->ch[ch].gr[0].scfsi = -1;
       si->ch[ch].gr[1].scfsi = getbits_fast(4);
   }

   for (gr=0; gr<2; gr++) 
   {
     for (ch=0; ch<stereo; ch++) 
     {
       register struct gr_info_s *gr_info = &(si->ch[ch].gr[gr]);

       gr_info->part2_3_length = getbits(12);
       gr_info->big_values = getbits_fast(9);
       if(gr_info->big_values > 288) {
          fprintf(stderr,"big_values too large!\n");
          gr_info->big_values = 288;
       }
       {
	 unsigned int qss = getbits_fast(8);
	 gr_info->pow2gain = gainpow2+256 - qss + powdiff;
#ifdef HAVEGTK
	 if (gtkflag) {
	   pinfo->qss[gr][ch]=qss;
	   pinfo->big_values[gr][ch]=gr_info->big_values;
	 }
#endif
       }
       if(ms_stereo)
         gr_info->pow2gain += 2;
       gr_info->scalefac_compress = getbits_fast(4);
/* window-switching flag == 1 for block_Type != 0 .. and block-type == 0 -> win-sw-flag = 0 */
       if(get1bit()) 
       {
         int i;
         gr_info->block_type = getbits_fast(2);
         gr_info->mixed_block_flag = get1bit();
         gr_info->table_select[0] = getbits_fast(5);
         gr_info->table_select[1] = getbits_fast(5);


         /*
          * table_select[2] not needed, because there is no region2,
          * but to satisfy some verifications tools we set it either.
          */
         gr_info->table_select[2] = 0;
         for(i=0;i<3;i++) {
	   unsigned int sbg = (getbits_fast(3)<<3);
           gr_info->full_gain[i] = gr_info->pow2gain + sbg;
#ifdef HAVEGTK
	   if (gtkflag)
	     pinfo->sub_gain[gr][ch][i]=sbg/8;
#endif
	 }


         if(gr_info->block_type == 0) {
           fprintf(stderr,"Blocktype == 0 and window-switching == 1 not allowed.\n");
           exit(1);
         }
         /* region_count/start parameters are implicit in this case. */       
         gr_info->region1start = 36>>1;
         gr_info->region2start = 576>>1;
       }
       else 
       {
         int i,r0c,r1c;
         for (i=0; i<3; i++)
           gr_info->table_select[i] = getbits_fast(5);
         r0c = getbits_fast(4);
         r1c = getbits_fast(3);
         gr_info->region1start = bandInfo[sfreq].longIdx[r0c+1] >> 1 ;
         gr_info->region2start = bandInfo[sfreq].longIdx[r0c+1+r1c+1] >> 1;
         gr_info->block_type = 0;
         gr_info->mixed_block_flag = 0;
       }
       gr_info->preflag = get1bit();
       gr_info->scalefac_scale = get1bit();
       gr_info->count1table_select = get1bit();
#ifdef HAVEGTK
       if (gtkflag)
	 pinfo->scalefac_scale[gr][ch]=gr_info->scalefac_scale;
#endif
     }
   }
}
#endif

/*
 * Side Info for MPEG 2.0 / LSF
 */
static void III_get_side_info_2(struct III_sideinfo *si,int stereo,
 int ms_stereo,long sfreq,int single)
{
   int ch;
   int powdiff = (single == 3) ? 4 : 0;

   si->main_data_begin = getbits(8);
   if (stereo == 1)
     si->private_bits = get1bit();
   else 
     si->private_bits = getbits_fast(2);

   for (ch=0; ch<stereo; ch++) 
   {
       register struct gr_info_s *gr_info = &(si->ch[ch].gr[0]);
       unsigned int qss;

       gr_info->part2_3_length = getbits(12);
       gr_info->big_values = getbits_fast(9);
       if(gr_info->big_values > 288) {
         fprintf(stderr,"big_values too large!\n");
         gr_info->big_values = 288;
       }
       qss=getbits_fast(8);
       gr_info->pow2gain = gainpow2+256 - qss + powdiff;
#ifdef HAVEGTK
       if (gtkflag) {
	   pinfo->qss[0][ch]=qss;
	   pinfo->big_values[0][ch]=gr_info->big_values;
       }
#endif

       if(ms_stereo)
         gr_info->pow2gain += 2;
       gr_info->scalefac_compress = getbits(9);
/* window-switching flag == 1 for block_Type != 0 .. and block-type == 0 -> win-sw-flag = 0 */
       if(get1bit()) 
       {
         int i;
         gr_info->block_type = getbits_fast(2);
         gr_info->mixed_block_flag = get1bit();
         gr_info->table_select[0] = getbits_fast(5);
         gr_info->table_select[1] = getbits_fast(5);
         /*
          * table_select[2] not needed, because there is no region2,
          * but to satisfy some verifications tools we set it either.
          */
         gr_info->table_select[2] = 0;
         for(i=0;i<3;i++) {
	   unsigned int sbg = (getbits_fast(3)<<3);
           gr_info->full_gain[i] = gr_info->pow2gain + sbg;
#ifdef HAVEGTK
	   if (gtkflag)
	     pinfo->sub_gain[0][ch][i]=sbg/8;
#endif
	 }

         if(gr_info->block_type == 0) {
           fprintf(stderr,"Blocktype == 0 and window-switching == 1 not allowed.\n");
           exit(1);
         }
         /* region_count/start parameters are implicit in this case. */       
/* check this again! */
         if(gr_info->block_type == 2)
           gr_info->region1start = 36>>1;
         else if(sfreq == 8)
/* check this for 2.5 and sfreq=8 */
           gr_info->region1start = 108>>1;
         else
           gr_info->region1start = 54>>1;
         gr_info->region2start = 576>>1;
       }
       else 
       {
         int i,r0c,r1c;
         for (i=0; i<3; i++)
           gr_info->table_select[i] = getbits_fast(5);
         r0c = getbits_fast(4);
         r1c = getbits_fast(3);
         gr_info->region1start = bandInfo[sfreq].longIdx[r0c+1] >> 1 ;
         gr_info->region2start = bandInfo[sfreq].longIdx[r0c+1+r1c+1] >> 1;
         gr_info->block_type = 0;
         gr_info->mixed_block_flag = 0;
       }
       gr_info->scalefac_scale = get1bit();
       gr_info->count1table_select = get1bit();
#ifdef HAVEGTK
       if (gtkflag)
	 pinfo->scalefac_scale[0][ch]=gr_info->scalefac_scale;
#endif
   }
}

/*
 * read scalefactors
 */
#ifdef MPEG1
static int III_get_scale_factors_1(int *scf,struct gr_info_s *gr_info)
{
   static unsigned char slen[2][16] = {
     {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4},
     {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}
   };
   int numbits;
   int num0 = slen[0][gr_info->scalefac_compress];
   int num1 = slen[1][gr_info->scalefac_compress];

    if (gr_info->block_type == 2) 
    {
      int i=18;
      numbits = (num0 + num1) * 18;

      if (gr_info->mixed_block_flag) {
         for (i=8;i;i--)
           *scf++ = getbits_fast(num0);
         i = 9;
         numbits -= num0; /* num0 * 17 + num1 * 18 */
      }

      for (;i;i--)
        *scf++ = getbits_fast(num0);
      for (i = 18; i; i--)
        *scf++ = getbits_fast(num1);
      *scf++ = 0; *scf++ = 0; *scf++ = 0; /* short[13][0..2] = 0 */
    }
    else 
    {
      int i;
      int scfsi = gr_info->scfsi;

      if(scfsi < 0) { /* scfsi < 0 => granule == 0 */
         for(i=11;i;i--)
           *scf++ = getbits_fast(num0);
         for(i=10;i;i--)
           *scf++ = getbits_fast(num1);
         numbits = (num0 + num1) * 10 + num0;
      }
      else {
        numbits = 0;
        if(!(scfsi & 0x8)) {
          for (i=6;i;i--)
            *scf++ = getbits_fast(num0);
          numbits += num0 * 6;
        }
        else {
          scf += 6;
        }

        if(!(scfsi & 0x4)) {
          for (i=5;i;i--)
            *scf++ = getbits_fast(num0);
          numbits += num0 * 5;
        }
        else {
          scf += 5;
        }

        if(!(scfsi & 0x2)) {
          for(i=5;i;i--)
            *scf++ = getbits_fast(num1);
          numbits += num1 * 5;
        }
        else {
          scf += 5;
        }

        if(!(scfsi & 0x1)) {
          for (i=5;i;i--)
            *scf++ = getbits_fast(num1);
          numbits += num1 * 5;
        }
        else {
          scf += 5;
        }
      }

      *scf++ = 0;  /* no l[21] in original sources */
    }
    return numbits;
}
#endif

static int III_get_scale_factors_2(int *scf,struct gr_info_s *gr_info,int i_stereo)
{
  unsigned char *pnt;
  int i,j;
  unsigned int slen;
  int n = 0;
  int numbits = 0;

  static unsigned char stab[3][6][4] = {
   { { 6, 5, 5,5 } , { 6, 5, 7,3 } , { 11,10,0,0} ,
     { 7, 7, 7,0 } , { 6, 6, 6,3 } , {  8, 8,5,0} } ,
   { { 9, 9, 9,9 } , { 9, 9,12,6 } , { 18,18,0,0} ,
     {12,12,12,0 } , {12, 9, 9,6 } , { 15,12,9,0} } ,
   { { 6, 9, 9,9 } , { 6, 9,12,6 } , { 15,18,0,0} ,
     { 6,15,12,0 } , { 6,12, 9,6 } , {  6,18,9,0} } }; 

  if(i_stereo) /* i_stereo AND second channel -> do_layer3() checks this */
    slen = i_slen2[gr_info->scalefac_compress>>1];
  else
    slen = n_slen2[gr_info->scalefac_compress];

  gr_info->preflag = (slen>>15) & 0x1;

  n = 0;  
  if( gr_info->block_type == 2 ) {
    n++;
    if(gr_info->mixed_block_flag)
      n++;
  }

  pnt = stab[n][(slen>>12)&0x7];

  for(i=0;i<4;i++) {
    int num = slen & 0x7;
    slen >>= 3;
    if(num) {
      for(j=0;j<(int)(pnt[i]);j++)
        *scf++ = getbits_fast(num);
      numbits += pnt[i] * num;
    }
    else {
      for(j=0;j<(int)(pnt[i]);j++)
        *scf++ = 0;
    }
  }
  
  n = (n << 1) + 1;
  for(i=0;i<n;i++)
    *scf++ = 0;

  return numbits;
}

static int pretab1[22] = {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0};
static int pretab2[22] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

/*
 * don't forget to apply the same changes to III_dequantize_sample_ms() !!! 
 */
static int III_dequantize_sample(real xr[SBLIMIT][SSLIMIT],int *scf,
   struct gr_info_s *gr_info,int sfreq,int part2bits)
{
  int shift = 1 + gr_info->scalefac_scale;
  real *xrpnt = (real *) xr;
  int l[3],l3;
  int part2remain = gr_info->part2_3_length - part2bits;
  int *me;

  {
    int bv       = gr_info->big_values;
    int region1  = gr_info->region1start;
    int region2  = gr_info->region2start;

    l3 = ((576>>1)-bv)>>1;   
/*
 * we may lose the 'odd' bit here !! 
 * check this later again 
 */
    if(bv <= region1) {
      l[0] = bv; l[1] = 0; l[2] = 0;
    }
    else {
      l[0] = region1;
      if(bv <= region2) {
        l[1] = bv - l[0];  l[2] = 0;
      }
      else {
        l[1] = region2 - l[0]; l[2] = bv - region2;
      }
    }
  }
 
  if(gr_info->block_type == 2) {
    /*
     * decoding with short or mixed mode BandIndex table 
     */
    int i,max[4];
    int step=0,lwin=0,cb=0;
    register real v = 0.0;
    register int *m,mc;

    if(gr_info->mixed_block_flag) {
      max[3] = -1;
      max[0] = max[1] = max[2] = 2;
      m = map[sfreq][0];
      me = mapend[sfreq][0];
    }
    else {
      max[0] = max[1] = max[2] = max[3] = -1;
      /* max[3] not really needed in this case */
      m = map[sfreq][1];
      me = mapend[sfreq][1];
    }

    mc = 0;
    for(i=0;i<2;i++) {
      int lp = l[i];
      struct newhuff *h = ht+gr_info->table_select[i];
      for(;lp;lp--,mc--) {
        register int x,y;
        if( (!mc) ) {
          mc = *m++;
          xrpnt = ((real *) xr) + (*m++);
          lwin = *m++;
          cb = *m++;
          if(lwin == 3) {
            v = gr_info->pow2gain[(*scf++) << shift];
            step = 1;
          }
          else {
            v = gr_info->full_gain[lwin][(*scf++) << shift];
            step = 3;
          }
        }
        {
          register short *val = h->table;
          while((y=*val++)<0) {
            if (get1bit())
              val -= y;
            part2remain--;
          }
          x = y >> 4;
          y &= 0xf;
        }
        if(x == 15) {
          max[lwin] = cb;
          part2remain -= h->linbits+1;
          x += getbits(h->linbits);
          if(get1bit())
            *xrpnt = -ispow[x] * v;
          else
            *xrpnt =  ispow[x] * v;
        }
        else if(x) {
          max[lwin] = cb;
          if(get1bit())
            *xrpnt = -ispow[x] * v;
          else
            *xrpnt =  ispow[x] * v;
          part2remain--;
        }
        else
          *xrpnt = 0.0;
        xrpnt += step;
        if(y == 15) {
          max[lwin] = cb;
          part2remain -= h->linbits+1;
          y += getbits(h->linbits);
          if(get1bit())
            *xrpnt = -ispow[y] * v;
          else
            *xrpnt =  ispow[y] * v;
        }
        else if(y) {
          max[lwin] = cb;
          if(get1bit())
            *xrpnt = -ispow[y] * v;
          else
            *xrpnt =  ispow[y] * v;
          part2remain--;
        }
        else
          *xrpnt = 0.0;
        xrpnt += step;
      }
    }
    for(;l3 && (part2remain > 0);l3--) {
      struct newhuff *h = htc+gr_info->count1table_select;
      register short *val = h->table,a;

      while((a=*val++)<0) {
        part2remain--;
        if(part2remain < 0) {
          part2remain++;
          a = 0;
          break;
        }
        if (get1bit())
          val -= a;
      }
      for(i=0;i<4;i++) {
        if(!(i & 1)) {
          if(!mc) {
            mc = *m++;
            xrpnt = ((real *) xr) + (*m++);
            lwin = *m++;
            cb = *m++;
            if(lwin == 3) {
              v = gr_info->pow2gain[(*scf++) << shift];
              step = 1;
            }
            else {
              v = gr_info->full_gain[lwin][(*scf++) << shift];
              step = 3;
            }
          }
          mc--;
        }
        if( (a & (0x8>>i)) ) {
          max[lwin] = cb;
          part2remain--;
          if(part2remain < 0) {
            part2remain++;
            break;
          }
          if(get1bit()) 
            *xrpnt = -v;
          else
            *xrpnt = v;
        }
        else
          *xrpnt = 0.0;
        xrpnt += step;
      }
    }
 
    while( m < me ) {
      if(!mc) {
        mc = *m++;
        xrpnt = ((real *) xr) + *m++;
        if( (*m++) == 3)
          step = 1;
        else
          step = 3;
        m++; /* cb */
      }
      mc--;
      *xrpnt = 0.0;
      xrpnt += step;
      *xrpnt = 0.0;
      xrpnt += step;
/* we could add a little opt. here:
 * if we finished a band for window 3 or a long band
 * further bands could copied in a simple loop without a
 * special 'map' decoding
 */
    }

    gr_info->maxband[0] = max[0]+1;
    gr_info->maxband[1] = max[1]+1;
    gr_info->maxband[2] = max[2]+1;
    gr_info->maxbandl = max[3]+1;

    {
      int rmax = max[0] > max[1] ? max[0] : max[1];
      rmax = (rmax > max[2] ? rmax : max[2]) + 1;
      gr_info->maxb = rmax ? shortLimit[sfreq][rmax] : longLimit[sfreq][max[3]+1];
    }

  }
  else {
	/*
     * decoding with 'long' BandIndex table (block_type != 2)
     */
    int *pretab = gr_info->preflag ? pretab1 : pretab2;
    int i,max = -1;
    int cb = 0;
    register int *m = map[sfreq][2];
    register real v = 0.0;
    register int mc = 0;
#if 0
    me = mapend[sfreq][2];
#endif

	/*
     * long hash table values
     */
    for(i=0;i<3;i++) {
      int lp = l[i];
      struct newhuff *h = ht+gr_info->table_select[i];

      for(;lp;lp--,mc--) {
        int x,y;

        if(!mc) {
          mc = *m++;
          v = gr_info->pow2gain[((*scf++) + (*pretab++)) << shift];
          cb = *m++;
        }
        {
          register short *val = h->table;
          while((y=*val++)<0) {
            if (get1bit())
              val -= y;
            part2remain--;
          }
          x = y >> 4;
          y &= 0xf;
        }
        if (x == 15) {
          max = cb;
          part2remain -= h->linbits+1;
          x += getbits(h->linbits);
          if(get1bit())
            *xrpnt++ = -ispow[x] * v;
          else
            *xrpnt++ =  ispow[x] * v;
        }
        else if(x) {
          max = cb;
          if(get1bit())
            *xrpnt++ = -ispow[x] * v;
          else
            *xrpnt++ =  ispow[x] * v;
          part2remain--;
        }
        else
          *xrpnt++ = 0.0;

        if (y == 15) {
          max = cb;
          part2remain -= h->linbits+1;
          y += getbits(h->linbits);
          if(get1bit())
            *xrpnt++ = -ispow[y] * v;
          else
            *xrpnt++ =  ispow[y] * v;
        }
        else if(y) {
          max = cb;
          if(get1bit())
            *xrpnt++ = -ispow[y] * v;
          else
            *xrpnt++ =  ispow[y] * v;
          part2remain--;
        }
        else
          *xrpnt++ = 0.0;
      }
    }

	/*
     * short (count1table) values
     */
    for(;l3 && (part2remain > 0);l3--) {
      struct newhuff *h = htc+gr_info->count1table_select;
      register short *val = h->table,a;

      while((a=*val++)<0) {
        part2remain--;
        if(part2remain < 0) {
          part2remain++;
          a = 0;
          break;
        }
        if (get1bit())
          val -= a;
      }
      for(i=0;i<4;i++) {
        if(!(i & 1)) {
          if(!mc) {
            mc = *m++;
            cb = *m++;
            v = gr_info->pow2gain[((*scf++) + (*pretab++)) << shift];
          }
          mc--;
        }
        if ( (a & (0x8>>i)) ) {
          max = cb;
          part2remain--;
          if(part2remain < 0) {
            part2remain++;
            break;
          }
          if(get1bit())
            *xrpnt++ = -v;
          else
            *xrpnt++ = v;
        }
        else
          *xrpnt++ = 0.0;
      }
    }

	/* 
     * zero part
     */
    for(i=(&xr[SBLIMIT][0]-xrpnt)>>1;i;i--) {
      *xrpnt++ = 0.0;
      *xrpnt++ = 0.0;
    }

    gr_info->maxbandl = max+1;
    gr_info->maxb = longLimit[sfreq][gr_info->maxbandl];
  }

  while( part2remain > 16 ) {
    getbits(16); /* Dismiss stuffing Bits */
    part2remain -= 16;
  }
  if(part2remain > 0)
    getbits(part2remain);
  else if(part2remain < 0) {
    fprintf(stderr,"mpg123: Can't rewind stream by %d bits!\n",-part2remain);
    return 1; /* -> error */
  }
  return 0;
}


/* 
 * III_stereo: calculate real channel values for Joint-I-Stereo-mode
 */
static void III_i_stereo(real xr_buf[2][SBLIMIT][SSLIMIT],int *scalefac,
   struct gr_info_s *gr_info,int sfreq,int ms_stereo,int lsf)
{
      real (*xr)[SBLIMIT*SSLIMIT] = (real (*)[SBLIMIT*SSLIMIT] ) xr_buf;
      struct bandInfoStruct *bi = &bandInfo[sfreq];
      real *tab1,*tab2;

      if(lsf) {
        int p = gr_info->scalefac_compress & 0x1;
	    if(ms_stereo) {
          tab1 = pow1_2[p]; tab2 = pow2_2[p];
        }
        else {
          tab1 = pow1_1[p]; tab2 = pow2_1[p];
        }
      }
      else {
        if(ms_stereo) {
          tab1 = tan1_2; tab2 = tan2_2;
        }
        else {
          tab1 = tan1_1; tab2 = tan2_1;
        }
      }

      if (gr_info->block_type == 2)
      {
         int lwin,do_l = 0;
         if( gr_info->mixed_block_flag )
           do_l = 1;

         for (lwin=0;lwin<3;lwin++) /* process each window */
         {
             /* get first band with zero values */
           int is_p,sb,idx,sfb = gr_info->maxband[lwin];  /* sfb is minimal 3 for mixed mode */
           if(sfb > 3)
             do_l = 0;

           for(;sfb<12;sfb++)
           {
             is_p = scalefac[sfb*3+lwin-gr_info->mixed_block_flag]; /* scale: 0-15 */ 
             if(is_p != 7) {
               real t1,t2;
               sb = bi->shortDiff[sfb];
               idx = bi->shortIdx[sfb] + lwin;
               t1 = tab1[is_p]; t2 = tab2[is_p];
               for (; sb > 0; sb--,idx+=3)
               {
                 real v = xr[0][idx];
                 xr[0][idx] = v * t1;
                 xr[1][idx] = v * t2;
               }
             }
           }

#if 1
/* in the original: copy 10 to 11 , here: copy 11 to 12 
maybe still wrong??? (copy 12 to 13?) */
           is_p = scalefac[11*3+lwin-gr_info->mixed_block_flag]; /* scale: 0-15 */
           sb = bi->shortDiff[12];
           idx = bi->shortIdx[12] + lwin;
#else
           is_p = scalefac[10*3+lwin-gr_info->mixed_block_flag]; /* scale: 0-15 */
           sb = bi->shortDiff[11];
           idx = bi->shortIdx[11] + lwin;
#endif
           if(is_p != 7)
           {
             real t1,t2;
             t1 = tab1[is_p]; t2 = tab2[is_p];
             for ( ; sb > 0; sb--,idx+=3 )
             {  
               real v = xr[0][idx];
               xr[0][idx] = v * t1;
               xr[1][idx] = v * t2;
             }
           }
         } /* end for(lwin; .. ; . ) */

         if (do_l)
         {
/* also check l-part, if ALL bands in the three windows are 'empty'
 * and mode = mixed_mode 
 */
           int sfb = gr_info->maxbandl;
           int idx = bi->longIdx[sfb];

           for ( ; sfb<8; sfb++ )
           {
             int sb = bi->longDiff[sfb];
             int is_p = scalefac[sfb]; /* scale: 0-15 */
             if(is_p != 7) {
               real t1,t2;
               t1 = tab1[is_p]; t2 = tab2[is_p];
               for ( ; sb > 0; sb--,idx++)
               {
                 real v = xr[0][idx];
                 xr[0][idx] = v * t1;
                 xr[1][idx] = v * t2;
               }
             }
             else 
               idx += sb;
           }
         }     
      } 
      else /* ((gr_info->block_type != 2)) */
      {
        int sfb = gr_info->maxbandl;
        int is_p,idx = bi->longIdx[sfb];
        for ( ; sfb<21; sfb++)
        {
          int sb = bi->longDiff[sfb];
          is_p = scalefac[sfb]; /* scale: 0-15 */
          if(is_p != 7) {
            real t1,t2;
            t1 = tab1[is_p]; t2 = tab2[is_p];
            for ( ; sb > 0; sb--,idx++)
            {
               real v = xr[0][idx];
               xr[0][idx] = v * t1;
               xr[1][idx] = v * t2;
            }
          }
          else
            idx += sb;
        }

        is_p = scalefac[20]; /* copy l-band 20 to l-band 21 */
        if(is_p != 7)
        {
          int sb;
          real t1 = tab1[is_p],t2 = tab2[is_p]; 

          for ( sb = bi->longDiff[21]; sb > 0; sb--,idx++ )
          {
            real v = xr[0][idx];
            xr[0][idx] = v * t1;
            xr[1][idx] = v * t2;
          }
        }
      } /* ... */
}

static void III_antialias(real xr[SBLIMIT][SSLIMIT],struct gr_info_s *gr_info)
{
   int sblim;

   if(gr_info->block_type == 2)
   {
      if(!gr_info->mixed_block_flag) 
        return;
      sblim = 1; 
   }
   else {
     sblim = gr_info->maxb-1;
   }

   /* 31 alias-reduction operations between each pair of sub-bands */
   /* with 8 butterflies between each pair                         */

   {
     int sb;
     real *xr1=(real *) xr[1];

     for(sb=sblim;sb;sb--,xr1+=10)
     {
       int ss;
       real *cs=aa_cs,*ca=aa_ca;
       real *xr2 = xr1;

       for(ss=7;ss>=0;ss--)
       {       /* upper and lower butterfly inputs */
         register real bu = *--xr2,bd = *xr1;
         *xr2   = (bu * (*cs)   ) - (bd * (*ca)   );
         *xr1++ = (bd * (*cs++) ) + (bu * (*ca++) );
       }
     }
  }
}

/*
 DCT insipired by Jeff Tsay's DCT from the maplay package
 this is an optimized version with manual unroll.

 References:
 [1] S. Winograd: "On Computing the Discrete Fourier Transform",
     Mathematics of Computation, Volume 32, Number 141, January 1978,
     Pages 175-199
*/

static void dct36(real *inbuf,real *o1,real *o2,real *wintab,real *tsbuf)
{
  {
    register real *in = inbuf;

    in[17]+=in[16]; in[16]+=in[15]; in[15]+=in[14];
    in[14]+=in[13]; in[13]+=in[12]; in[12]+=in[11];
    in[11]+=in[10]; in[10]+=in[9];  in[9] +=in[8];
    in[8] +=in[7];  in[7] +=in[6];  in[6] +=in[5];
    in[5] +=in[4];  in[4] +=in[3];  in[3] +=in[2];
    in[2] +=in[1];  in[1] +=in[0];

    in[17]+=in[15]; in[15]+=in[13]; in[13]+=in[11]; in[11]+=in[9];
    in[9] +=in[7];  in[7] +=in[5];  in[5] +=in[3];  in[3] +=in[1];


  {

#define MACRO0(v) { \
    real tmp; \
    out2[9+(v)] = (tmp = sum0 + sum1) * w[27+(v)]; \
    out2[8-(v)] = tmp * w[26-(v)];  } \
    sum0 -= sum1; \
    ts[SBLIMIT*(8-(v))] = out1[8-(v)] + sum0 * w[8-(v)]; \
    ts[SBLIMIT*(9+(v))] = out1[9+(v)] + sum0 * w[9+(v)]; 
#define MACRO1(v) { \
	real sum0,sum1; \
    sum0 = tmp1a + tmp2a; \
	sum1 = (tmp1b + tmp2b) * tfcos36[(v)]; \
	MACRO0(v); }
#define MACRO2(v) { \
    real sum0,sum1; \
    sum0 = tmp2a - tmp1a; \
    sum1 = (tmp2b - tmp1b) * tfcos36[(v)]; \
	MACRO0(v); }

    register const real *c = COS9;
    register real *out2 = o2;
	register real *w = wintab;
	register real *out1 = o1;
	register real *ts = tsbuf;

    real ta33,ta66,tb33,tb66;

    ta33 = in[2*3+0] * c[3];
    ta66 = in[2*6+0] * c[6];
    tb33 = in[2*3+1] * c[3];
    tb66 = in[2*6+1] * c[6];

    { 
      real tmp1a,tmp2a,tmp1b,tmp2b;
      tmp1a =             in[2*1+0] * c[1] + ta33 + in[2*5+0] * c[5] + in[2*7+0] * c[7];
      tmp1b =             in[2*1+1] * c[1] + tb33 + in[2*5+1] * c[5] + in[2*7+1] * c[7];
      tmp2a = in[2*0+0] + in[2*2+0] * c[2] + in[2*4+0] * c[4] + ta66 + in[2*8+0] * c[8];
      tmp2b = in[2*0+1] + in[2*2+1] * c[2] + in[2*4+1] * c[4] + tb66 + in[2*8+1] * c[8];

      MACRO1(0);
      MACRO2(8);
    }

    {
      real tmp1a,tmp2a,tmp1b,tmp2b;
      tmp1a = ( in[2*1+0] - in[2*5+0] - in[2*7+0] ) * c[3];
      tmp1b = ( in[2*1+1] - in[2*5+1] - in[2*7+1] ) * c[3];
      tmp2a = ( in[2*2+0] - in[2*4+0] - in[2*8+0] ) * c[6] - in[2*6+0] + in[2*0+0];
      tmp2b = ( in[2*2+1] - in[2*4+1] - in[2*8+1] ) * c[6] - in[2*6+1] + in[2*0+1];

      MACRO1(1);
      MACRO2(7);
    }

    {
      real tmp1a,tmp2a,tmp1b,tmp2b;
      tmp1a =             in[2*1+0] * c[5] - ta33 - in[2*5+0] * c[7] + in[2*7+0] * c[1];
      tmp1b =             in[2*1+1] * c[5] - tb33 - in[2*5+1] * c[7] + in[2*7+1] * c[1];
      tmp2a = in[2*0+0] - in[2*2+0] * c[8] - in[2*4+0] * c[2] + ta66 + in[2*8+0] * c[4];
      tmp2b = in[2*0+1] - in[2*2+1] * c[8] - in[2*4+1] * c[2] + tb66 + in[2*8+1] * c[4];

      MACRO1(2);
      MACRO2(6);
    }

    {
      real tmp1a,tmp2a,tmp1b,tmp2b;
      tmp1a =             in[2*1+0] * c[7] - ta33 + in[2*5+0] * c[1] - in[2*7+0] * c[5];
      tmp1b =             in[2*1+1] * c[7] - tb33 + in[2*5+1] * c[1] - in[2*7+1] * c[5];
      tmp2a = in[2*0+0] - in[2*2+0] * c[4] + in[2*4+0] * c[8] + ta66 - in[2*8+0] * c[2];
      tmp2b = in[2*0+1] - in[2*2+1] * c[4] + in[2*4+1] * c[8] + tb66 - in[2*8+1] * c[2];

      MACRO1(3);
      MACRO2(5);
    }

	{
		real sum0,sum1;
    	sum0 =  in[2*0+0] - in[2*2+0] + in[2*4+0] - in[2*6+0] + in[2*8+0];
    	sum1 = (in[2*0+1] - in[2*2+1] + in[2*4+1] - in[2*6+1] + in[2*8+1] ) * tfcos36[4];
		MACRO0(4);
	}
  }

  }
}

/*
 * new DCT12
 */
static void dct12(real *in,real *rawout1,real *rawout2,register real *wi,register real *ts)
{
#define DCT12_PART1 \
             in5 = in[5*3];  \
     in5 += (in4 = in[4*3]); \
     in4 += (in3 = in[3*3]); \
     in3 += (in2 = in[2*3]); \
     in2 += (in1 = in[1*3]); \
     in1 += (in0 = in[0*3]); \
                             \
     in5 += in3; in3 += in1; \
                             \
     in2 *= COS6_1; \
     in3 *= COS6_1; \

#define DCT12_PART2 \
     in0 += in4 * COS6_2; \
                          \
     in4 = in0 + in2;     \
     in0 -= in2;          \
                          \
     in1 += in5 * COS6_2; \
                          \
     in5 = (in1 + in3) * tfcos12[0]; \
     in1 = (in1 - in3) * tfcos12[2]; \
                         \
     in3 = in4 + in5;    \
     in4 -= in5;         \
                         \
     in2 = in0 + in1;    \
     in0 -= in1;


   {
     real in0,in1,in2,in3,in4,in5;
     register real *out1 = rawout1;
     ts[SBLIMIT*0] = out1[0]; ts[SBLIMIT*1] = out1[1]; ts[SBLIMIT*2] = out1[2];
     ts[SBLIMIT*3] = out1[3]; ts[SBLIMIT*4] = out1[4]; ts[SBLIMIT*5] = out1[5];
 
     DCT12_PART1

     {
       real tmp0,tmp1 = (in0 - in4);
       {
         real tmp2 = (in1 - in5) * tfcos12[1];
         tmp0 = tmp1 + tmp2;
         tmp1 -= tmp2;
       }
       ts[(17-1)*SBLIMIT] = out1[17-1] + tmp0 * wi[11-1];
       ts[(12+1)*SBLIMIT] = out1[12+1] + tmp0 * wi[6+1];
       ts[(6 +1)*SBLIMIT] = out1[6 +1] + tmp1 * wi[1];
       ts[(11-1)*SBLIMIT] = out1[11-1] + tmp1 * wi[5-1];
     }

     DCT12_PART2

     ts[(17-0)*SBLIMIT] = out1[17-0] + in2 * wi[11-0];
     ts[(12+0)*SBLIMIT] = out1[12+0] + in2 * wi[6+0];
     ts[(12+2)*SBLIMIT] = out1[12+2] + in3 * wi[6+2];
     ts[(17-2)*SBLIMIT] = out1[17-2] + in3 * wi[11-2];

     ts[(6+0)*SBLIMIT]  = out1[6+0] + in0 * wi[0];
     ts[(11-0)*SBLIMIT] = out1[11-0] + in0 * wi[5-0];
     ts[(6+2)*SBLIMIT]  = out1[6+2] + in4 * wi[2];
     ts[(11-2)*SBLIMIT] = out1[11-2] + in4 * wi[5-2];
  }

  in++;

  {
     real in0,in1,in2,in3,in4,in5;
     register real *out2 = rawout2;
 
     DCT12_PART1

     {
       real tmp0,tmp1 = (in0 - in4);
       {
         real tmp2 = (in1 - in5) * tfcos12[1];
         tmp0 = tmp1 + tmp2;
         tmp1 -= tmp2;
       }
       out2[5-1] = tmp0 * wi[11-1];
       out2[0+1] = tmp0 * wi[6+1];
       ts[(12+1)*SBLIMIT] += tmp1 * wi[1];
       ts[(17-1)*SBLIMIT] += tmp1 * wi[5-1];
     }

     DCT12_PART2

     out2[5-0] = in2 * wi[11-0];
     out2[0+0] = in2 * wi[6+0];
     out2[0+2] = in3 * wi[6+2];
     out2[5-2] = in3 * wi[11-2];

     ts[(12+0)*SBLIMIT] += in0 * wi[0];
     ts[(17-0)*SBLIMIT] += in0 * wi[5-0];
     ts[(12+2)*SBLIMIT] += in4 * wi[2];
     ts[(17-2)*SBLIMIT] += in4 * wi[5-2];
  }

  in++; 

  {
     real in0,in1,in2,in3,in4,in5;
     register real *out2 = rawout2;
     out2[12]=out2[13]=out2[14]=out2[15]=out2[16]=out2[17]=0.0;

     DCT12_PART1

     {
       real tmp0,tmp1 = (in0 - in4);
       {
         real tmp2 = (in1 - in5) * tfcos12[1];
         tmp0 = tmp1 + tmp2;
         tmp1 -= tmp2;
       }
       out2[11-1] = tmp0 * wi[11-1];
       out2[6 +1] = tmp0 * wi[6+1];
       out2[0+1] += tmp1 * wi[1];
       out2[5-1] += tmp1 * wi[5-1];
     }

     DCT12_PART2

     out2[11-0] = in2 * wi[11-0];
     out2[6 +0] = in2 * wi[6+0];
     out2[6 +2] = in3 * wi[6+2];
     out2[11-2] = in3 * wi[11-2];

     out2[0+0] += in0 * wi[0];
     out2[5-0] += in0 * wi[5-0];
     out2[0+2] += in4 * wi[2];
     out2[5-2] += in4 * wi[5-2];
  }
}

/*
 * III_hybrid
 */
static void III_hybrid(real fsIn[SBLIMIT][SSLIMIT],real tsOut[SSLIMIT][SBLIMIT],
   int ch,struct gr_info_s *gr_info)
{
   real *tspnt = (real *) tsOut;
   real (*block)[2][SBLIMIT*SSLIMIT] = gmp->hybrid_block;
   int *blc = gmp->hybrid_blc;
   real *rawout1,*rawout2;
   int bt;
   int sb = 0;

   {
     int b = blc[ch];
     rawout1=block[b][ch];
     b=-b+1;
     rawout2=block[b][ch];
     blc[ch] = b;
   }

  
   if(gr_info->mixed_block_flag) {
     sb = 2;
     dct36(fsIn[0],rawout1,rawout2,win[0],tspnt);
     dct36(fsIn[1],rawout1+18,rawout2+18,win1[0],tspnt+1);
     rawout1 += 36; rawout2 += 36; tspnt += 2;
   }
 
   bt = gr_info->block_type;
   if(bt == 2) {
     for (; sb<gr_info->maxb; sb+=2,tspnt+=2,rawout1+=36,rawout2+=36) {
       dct12(fsIn[sb],rawout1,rawout2,win[2],tspnt);
       dct12(fsIn[sb+1],rawout1+18,rawout2+18,win1[2],tspnt+1);
     }
   }
   else {
     for (; sb<gr_info->maxb; sb+=2,tspnt+=2,rawout1+=36,rawout2+=36) {
       dct36(fsIn[sb],rawout1,rawout2,win[bt],tspnt);
       dct36(fsIn[sb+1],rawout1+18,rawout2+18,win1[bt],tspnt+1);
     }
   }

   for(;sb<SBLIMIT;sb++,tspnt++) {
     int i;
     for(i=0;i<SSLIMIT;i++) {
       tspnt[i*SBLIMIT] = *rawout1++;
       *rawout2++ = 0.0;
     }
   }
}

/*
 * main layer3 handler
 */
int do_layer3(struct frame *fr,unsigned char *pcm_sample,int *pcm_point)
{
  int gr, ch, ss,clip=0;
  int scalefacs[2][39]; /* max 39 for short[13][3] mode, mixed: 38, long: 22 */
  struct III_sideinfo sideinfo;
  int stereo = fr->stereo;
  int single = fr->single;
  int ms_stereo,i_stereo;
  int sfreq = fr->sampling_frequency;
  int stereo1,granules;



  if(stereo == 1) { /* stream is mono */
    stereo1 = 1;
    single = 0;
  }
  else if(single >= 0) /* stream is stereo, but force to mono */
    stereo1 = 1;
  else
    stereo1 = 2;

  if(fr->mode == MPG_MD_JOINT_STEREO) {
    ms_stereo = fr->mode_ext & 0x2;
    i_stereo  = fr->mode_ext & 0x1;
  }
  else
    ms_stereo = i_stereo = 0;


  if(fr->lsf) {
    granules = 1;
    III_get_side_info_2(&sideinfo,stereo,ms_stereo,sfreq,single);
  }
  else {
    granules = 2;
#ifdef MPEG1
    III_get_side_info_1(&sideinfo,stereo,ms_stereo,sfreq,single);
#else
    fprintf(stderr,"Not supported\n");
#endif
  }

  if(set_pointer(sideinfo.main_data_begin) == MP3_ERR)
    return 0; 


  for (gr=0;gr<granules;gr++) 
  {
    static real hybridIn[2][SBLIMIT][SSLIMIT];
    static real hybridOut[2][SSLIMIT][SBLIMIT];

    {
      struct gr_info_s *gr_info = &(sideinfo.ch[0].gr[gr]);
      long part2bits;
      if(fr->lsf)
        part2bits = III_get_scale_factors_2(scalefacs[0],gr_info,0);
      else {
#ifdef MPEG1
        part2bits = III_get_scale_factors_1(scalefacs[0],gr_info);
#else
	fprintf(stderr,"Not supported\n");
#endif
      }
#ifdef HAVEGTK
      if (gtkflag) {
	int i;
	for (i=0; i<39; i++) 
	  pinfo->sfb_s[gr][0][i]=scalefacs[0][i];
      }
#endif
      if(III_dequantize_sample(hybridIn[0], scalefacs[0],gr_info,sfreq,part2bits))
        return clip;
    }
    if(stereo == 2) {
      struct gr_info_s *gr_info = &(sideinfo.ch[1].gr[gr]);
      long part2bits;
      if(fr->lsf) 
        part2bits = III_get_scale_factors_2(scalefacs[1],gr_info,i_stereo);
      else {
#ifdef MPEG1
        part2bits = III_get_scale_factors_1(scalefacs[1],gr_info);
#else
	fprintf(stderr,"Not supported\n");
#endif
      }
#ifdef HAVEGTK
      if (gtkflag) {
	int i;
	for (i=0; i<39; i++) 
	  pinfo->sfb_s[gr][1][i]=scalefacs[1][i];
      }
#endif

      if(III_dequantize_sample(hybridIn[1],scalefacs[1],gr_info,sfreq,part2bits))
          return clip;

      if(ms_stereo) {
        int i;
        for(i=0;i<SBLIMIT*SSLIMIT;i++) {
          real tmp0,tmp1;
          tmp0 = ((real *) hybridIn[0])[i];
          tmp1 = ((real *) hybridIn[1])[i];
          ((real *) hybridIn[1])[i] = tmp0 - tmp1;  
          ((real *) hybridIn[0])[i] = tmp0 + tmp1;
        }
      }

      if(i_stereo)
        III_i_stereo(hybridIn,scalefacs[1],gr_info,sfreq,ms_stereo,fr->lsf);

      if(ms_stereo || i_stereo || (single == 3) ) {
        if(gr_info->maxb > sideinfo.ch[0].gr[gr].maxb) 
          sideinfo.ch[0].gr[gr].maxb = gr_info->maxb;
        else
          gr_info->maxb = sideinfo.ch[0].gr[gr].maxb;
      }

      switch(single) {
        case 3:
          {
            register int i;
            register real *in0 = (real *) hybridIn[0],*in1 = (real *) hybridIn[1];
            for(i=0;i<SSLIMIT*gr_info->maxb;i++,in0++)
              *in0 = (*in0 + *in1++); /* *0.5 done by pow-scale */ 
          }
          break;
        case 1:
          {
            register int i;
            register real *in0 = (real *) hybridIn[0],*in1 = (real *) hybridIn[1];
            for(i=0;i<SSLIMIT*gr_info->maxb;i++)
              *in0++ = *in1++;
          }
          break;
      }
    }

#ifdef HAVEGTK
    if (gtkflag) {
    extern int tabsel_123[2][3][16];
    extern int pretab[21];
    int i,j,sb;
    float ifqstep;

    for (ch=0;ch<stereo1;ch++) {
      struct gr_info_s *gr_info = &(sideinfo.ch[ch].gr[gr]);
      ifqstep = ( pinfo->scalefac_scale[gr][ch] == 0 ) ? .5 : 1.0;
      if (2==gr_info->block_type) {
	for (i=0; i<3; i++) {
	  for (sb=0; sb<12; sb++) {
	    j = 3*sb+i;
	    /*
           is_p = scalefac[sfb*3+lwin-gr_info->mixed_block_flag]; 
	    */
	    /* scalefac was copied into pinfo->sfb_s[] above */
	    pinfo->sfb_s[gr][ch][j] = -ifqstep*pinfo->sfb_s[gr][ch][j-gr_info->mixed_block_flag];
	    pinfo->sfb_s[gr][ch][j] -= 2*(pinfo->sub_gain[gr][ch][i]);
	  }
	  pinfo->sfb_s[gr][ch][3*sb+i] = - 2*(pinfo->sub_gain[gr][ch][i]);
	}
      }else{
	for (sb=0; sb<21; sb++) {
	  /* scalefac was copied into pinfo->sfb[] above */
	  pinfo->sfb[gr][ch][sb] = pinfo->sfb_s[gr][ch][sb];
	  if (gr_info->preflag) pinfo->sfb[gr][ch][sb] += pretab[sb];
	  pinfo->sfb[gr][ch][sb] *= -ifqstep;
	}
      }
    }


    
    pinfo->bitrate = 
      tabsel_123[fr->lsf][fr->lay-1][fr->bitrate_index];
    pinfo->sampfreq = freqs[sfreq];
    pinfo->emph = fr->emphasis;
    pinfo->crc = fr->error_protection;
    pinfo->padding = fr->padding;
    pinfo->stereo = fr->stereo;
    pinfo->js =   (fr->mode == MPG_MD_JOINT_STEREO);
    pinfo->ms_stereo = ms_stereo;
    pinfo->i_stereo = i_stereo;
    pinfo->maindata = sideinfo.main_data_begin;

    for(ch=0;ch<stereo1;ch++) {
      struct gr_info_s *gr_info = &(sideinfo.ch[ch].gr[gr]);
      pinfo->mixed[gr][ch] = gr_info->mixed_block_flag;
      pinfo->mpg123blocktype[gr][ch]=gr_info->block_type;
      pinfo->mainbits[gr][ch] = gr_info->part2_3_length;
      if (gr==1) pinfo->scfsi[ch] = gr_info->scfsi;
    }
    for(ch=0;ch<stereo1;ch++) { 
      int j=0;
      for (sb=0;sb<SBLIMIT;sb++)
	for(ss=0;ss<SSLIMIT;ss++,j++) 
	  pinfo->mpg123xr[gr][ch][j]=hybridIn[ch][sb][ss];
    }
  }

#endif

    for(ch=0;ch<stereo1;ch++) {
      struct gr_info_s *gr_info = &(sideinfo.ch[ch].gr[gr]);
      III_antialias(hybridIn[ch],gr_info);
      III_hybrid(hybridIn[ch], hybridOut[ch], ch,gr_info);
    }

    for(ss=0;ss<SSLIMIT;ss++) {
      if(single >= 0) {
        clip += synth_1to1_mono(hybridOut[0][ss],pcm_sample,pcm_point);
      }
      else {
        int p1 = *pcm_point;
        clip += synth_1to1(hybridOut[0][ss],0,pcm_sample,&p1);
        clip += synth_1to1(hybridOut[1][ss],1,pcm_sample,pcm_point);
      }
    }
  }
  
  return clip;
}



#endif


/* ==== mainmpglib.c ==== */
#ifdef HAVEMPGLIB

#include "mpg123.h"
#include "mpglib.h"

#ifdef OS_AMIGAOS
#include "/lame.h"
#include "/util.h"
#include "/VbrTag.h"
#else
#include "lame.h"
#include "util.h"
#include "VbrTag.h"
#endif /* OS_AMIGAOS */

#include <stdlib.h>

static char buf[16384];
#define FSIZE 8192  
static char out[FSIZE];
struct mpstr mp;


int is_syncword(char *header)
{

/*
unsigned int s0,s1;
s0 = (unsigned char) header[0];
s1 = (unsigned char) header[1] ;
printf(" syncword:  %2X   %2X   \n ",s0, s1);
*/

/*
printf(" integer  %i \n",(int) ( header[0] == (char) 0xFF));
printf(" integer  %i \n",(int) ( (header[1] & (char) 0xF0) == (char) 0xF0));
*/

return 
((int) ( header[0] == (char) 0xFF)) &&
((int) ( (header[1] & (char) 0xF0) == (char) 0xF0));


}


int lame_decode_initfile(FILE *fd, int *stereo, int *samp, int *bitrate, 
unsigned long *num_samples)
{
  extern int tabsel_123[2][3][16];
  VBRTAGDATA pTagData;
  int ret,size,framesize;
  unsigned long num_frames=0;
  size_t len;
  int xing_header;


  InitMP3(&mp);
  memset(buf, 0, sizeof(buf));
  
  /* skip RIFF type proprietary headers  */
  /* look for sync word  FFF */
  while (!is_syncword(buf)) {
    buf[0]=buf[1]; 
    if (fread(&buf[1],1,1,fd) == 0) return -1;  /* failed */
  }
  /*  ret = decodeMP3(&mp,buf,2,out,FSIZE,&size); */

  /* read the header */
  len = fread(&buf[2],1,46,fd);
  if (len ==0 ) return -1;
  len +=2;

  /* check for Xing header */
  xing_header = GetVbrTag(&pTagData,(unsigned char*)buf);
  if (xing_header) {
    num_frames=pTagData.frames;
  }

  size=0;
  ret = decodeMP3(&mp,buf,len,out,FSIZE,&size);
  if (size>0 && !xing_header) {
    fprintf(stderr,"Opps: first frame of mpglib output will be lost \n");
  }

  *stereo = mp.fr.stereo;
  *samp = freqs[mp.fr.sampling_frequency];
  *bitrate = tabsel_123[mp.fr.lsf][mp.fr.lay-1][mp.fr.bitrate_index];
  framesize = (mp.fr.lsf == 0) ? 1152 : 576;
  *num_samples=MAX_U_32_NUM;
  if (xing_header && num_frames) {
    *num_samples=framesize * num_frames;
  }

  /*
  printf("ret = %i NEED_MORE=%i \n",ret,MP3_NEED_MORE);
  printf("stereo = %i \n",mp.fr.stereo);
  printf("samp = %i  \n",(int)freqs[mp.fr.sampling_frequency]);
  printf("framesize = %i  \n",framesize);
  printf("num frames = %i  \n",(int)num_frames);
  printf("num samp = %i  \n",(int)*num_samples);
  */
  return 0;
}


int lame_decode_init(void)
{
  InitMP3(&mp);
  memset(buf, 0, sizeof(buf));
  return 0;
}


/*
For lame_decode_fromfile:  return code
  -1     error
   0     ok, but need more data before outputing any samples
   n     number of samples output.  either 576 or 1152 depending on MP3 file.
*/
int lame_decode_fromfile(FILE *fd, short pcm_l[], short pcm_r[])
{
  int size,stereo;
  int outsize=0,j,i,ret;
  size_t len;

  size=0;
  len = fread(buf,1,64,fd);
  if (len ==0 ) return 0;
  ret = decodeMP3(&mp,buf,len,out,FSIZE,&size);

  /* read more until we get a valid output frame */
  while((ret == MP3_NEED_MORE) || !size) {
    len = fread(buf,1,100,fd);
    if (len ==0 ) return -1;
    ret = decodeMP3(&mp,buf,len,out,FSIZE,&size);
    /* if (ret ==MP3_ERR) return -1;  lets ignore errors and keep reading... */
    /*
    printf("ret = %i size= %i  %i   %i  %i \n",ret,size,
	   MP3_NEED_MORE,MP3_ERR,MP3_OK); 
    */
  }

  stereo=mp.fr.stereo;

  if (ret == MP3_OK) 
  {
    /*    write(1,out,size); */
    outsize = size/(2*(stereo));
    if ((outsize!=576) && (outsize!=1152)) {
      fprintf(stderr,"Opps: mpg123 returned more than one frame!  Cant handle this... \n");
      exit(-50);
    }

    for (j=0; j<stereo; j++)
      for (i=0; i<outsize; i++) 
	if (j==0) pcm_l[i] = ((short *) out)[mp.fr.stereo*i+j];
	else pcm_r[i] = ((short *) out)[mp.fr.stereo*i+j];

  }
  if (ret==MP3_ERR) return -1;
  else return outsize;
}




/*
For lame_decode:  return code
  -1     error
   0     ok, but need more data before outputing any samples
   n     number of samples output.  either 576 or 1152 depending on MP3 file.
*/
int lame_decode(char *buf,int len,short pcm_l[],short pcm_r[])
{
  int size;
  int outsize=0,j,i,ret;

  ret = decodeMP3(&mp,buf,len,out,FSIZE,&size);
  if (ret==MP3_OK) {
    /*    printf("mpg123 output one frame out=%i \n",size/4);  */
    outsize = size/(2*mp.fr.stereo);
    if (outsize > 1152) {
      fprintf(stderr,"Opps: mpg123 returned more than one frame!  shouldn't happen... \n");
      exit(-50);
    }

    for (j=0; j<mp.fr.stereo; j++)
      for (i=0; i<outsize; i++) 
	if (j==0) pcm_l[i] = ((short *) out)[mp.fr.stereo*i+j];
	else pcm_r[i] = ((short *) out)[mp.fr.stereo*i+j];

  }
  /*
  printf("ok, more, err:  %i %i %i  \n",MP3_OK, MP3_NEED_MORE, MP3_ERR);
  printf("ret = %i out=%i \n",ret,outsize);
  */
  if (ret==MP3_ERR) return -1;
  else return outsize;
}

#endif /* HAVEMPGLIB */



/* ==== newmdct.c ==== */
/*
 *	MP3 window subband -> subband filtering -> mdct routine
 *
 *	Copyright (c) 1999 Takehiro TOMINAGA
 *
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 *         Special Thanks to Patrick De Smet for your advices.
 */


#include "util.h"
#include "l3side.h"
#include "newmdct.h"

#define SCALE 32768

static FLOAT8 enwindow[] = 
{
  3.5780907e-02,1.7876148e-02,3.134727e-03,2.457142e-03,
    9.71317e-04,  2.18868e-04, 1.01566e-04,  1.3828e-05,

  3.5758972e-02, 3.401756e-03,  9.83715e-04,   9.9182e-05,
      -4.77e-07,  1.03951e-04,  9.53674e-04, 2.841473e-03,
     1.2398e-05,  1.91212e-04, 2.283096e-03,1.6994476e-02,
  1.8756866e-02, 2.630711e-03,  2.47478e-04,   1.4782e-05,

  3.5694122e-02, 3.643036e-03,  9.91821e-04,   9.6321e-05,
      -4.77e-07,  1.05858e-04,  9.30786e-04, 2.521515e-03,
     1.1444e-05,  1.65462e-04, 2.110004e-03,1.6112804e-02,
  1.9634247e-02, 2.803326e-03,  2.77042e-04,   1.6689e-05,

  3.5586357e-02, 3.858566e-03,  9.95159e-04,   9.3460e-05,
      -4.77e-07,  1.07288e-04,  9.02653e-04, 2.174854e-03,
     1.0014e-05,  1.40190e-04, 1.937389e-03,1.5233517e-02,
  2.0506859e-02, 2.974033e-03,  3.07560e-04,   1.8120e-05,

  3.5435200e-02, 4.049301e-03,  9.94205e-04,   9.0599e-05,
      -4.77e-07,  1.08242e-04,  8.68797e-04, 1.800537e-03,
      9.060e-06,  1.16348e-04, 1.766682e-03,1.4358521e-02,
  2.1372318e-02,  3.14188e-03,  3.39031e-04,   1.9550e-05,

  3.5242081e-02, 4.215240e-03,  9.89437e-04,   8.7261e-05,
      -4.77e-07,  1.08719e-04,  8.29220e-04, 1.399517e-03,
      8.106e-06,   9.3937e-05, 1.597881e-03,1.3489246e-02,
  2.2228718e-02, 3.306866e-03,  3.71456e-04,   2.1458e-05,

  3.5007000e-02, 4.357815e-03,  9.80854e-04,   8.3923e-05,
      -4.77e-07,  1.08719e-04,   7.8392e-04,  9.71317e-04,
      7.629e-06,   7.2956e-05, 1.432419e-03,1.2627602e-02,
  2.3074150e-02, 3.467083e-03,  4.04358e-04,   2.3365e-05,

  3.4730434e-02, 4.477024e-03,  9.68933e-04,   8.0585e-05,
      -9.54e-07,  1.08242e-04,  7.31945e-04,  5.15938e-04,
      6.676e-06,   5.2929e-05, 1.269817e-03,1.1775017e-02,
  2.3907185e-02, 3.622532e-03,  4.38213e-04,   2.5272e-05,

  3.4412861e-02, 4.573822e-03,  9.54151e-04,   7.6771e-05,
      -9.54e-07,  1.06812e-04,  6.74248e-04,   3.3379e-05,
      6.199e-06,   3.4332e-05, 1.111031e-03,1.0933399e-02,
  2.4725437e-02, 3.771782e-03,  4.72546e-04,   2.7657e-05,

  3.4055710e-02, 4.649162e-03,  9.35555e-04,   7.3433e-05,
      -9.54e-07,  1.05381e-04,  6.10352e-04, -4.75883e-04,
      5.245e-06,   1.7166e-05,  9.56535e-04,1.0103703e-02,
  2.5527000e-02, 3.914356e-03,  5.07355e-04,   3.0041e-05,

  3.3659935e-02, 4.703045e-03,  9.15051e-04,   7.0095e-05,
      -9.54e-07,  1.02520e-04,  5.39303e-04,-1.011848e-03,
      4.768e-06,     9.54e-07,  8.06808e-04, 9.287834e-03,
  2.6310921e-02, 4.048824e-03,  5.42164e-04,   3.2425e-05,

  3.3225536e-02, 4.737377e-03,  8.91685e-04,   6.6280e-05,
     -1.431e-06,   9.9182e-05,  4.62532e-04,-1.573563e-03,
      4.292e-06,  -1.3828e-05,  6.61850e-04, 8.487225e-03,
  2.7073860e-02, 4.174709e-03,  5.76973e-04,   3.4809e-05,

  3.2754898e-02, 4.752159e-03,  8.66413e-04,   6.2943e-05,
     -1.431e-06,   9.5367e-05,  3.78609e-04,-2.161503e-03,
      3.815e-06,   -2.718e-05,  5.22137e-04, 7.703304e-03,
  2.7815342e-02, 4.290581e-03,  6.11782e-04,   3.7670e-05,

  3.2248020e-02, 4.748821e-03,  8.38757e-04,   5.9605e-05,
     -1.907e-06,   9.0122e-05,  2.88486e-04,-2.774239e-03,
      3.338e-06,  -3.9577e-05,  3.88145e-04, 6.937027e-03,
  2.8532982e-02, 4.395962e-03,  6.46591e-04,   4.0531e-05,

  3.1706810e-02, 4.728317e-03,  8.09669e-04,    5.579e-05,
     -1.907e-06,   8.4400e-05,  1.91689e-04,-3.411293e-03,
      3.338e-06,  -5.0545e-05,  2.59876e-04, 6.189346e-03,
  2.9224873e-02, 4.489899e-03,  6.80923e-04,   4.3392e-05,

  3.1132698e-02, 4.691124e-03,  7.79152e-04,   5.2929e-05,
     -2.384e-06,   7.7724e-05,   8.8215e-05,-4.072189e-03,
      2.861e-06,  -6.0558e-05,  1.37329e-04, 5.462170e-03,
  2.9890060e-02, 4.570484e-03,  7.14302e-04,   4.6253e-05,

  3.0526638e-02, 4.638195e-03,  7.47204e-04,   4.9591e-05,
   4.756451e-03,   2.1458e-05,  -6.9618e-05,    2.384e-06
};

static FLOAT8 sb_sample[2][2][18][SBLIMIT];
static FLOAT8 mm[16][SBLIMIT - 1];

#define NS 12
#define NL 36

static const int all[] = {0,2,3,5,6,8,9,11,12,14,15,17};
static FLOAT8 ca[8], cs[8];
static FLOAT8 cos_s[NS / 2][NS / 2];
static FLOAT8 cos_l[(NL / 2) * 12 + (NL / 6) * 4 + (NL / 18) * 2];
static FLOAT8 win[4][36];

#define work (&win[2][4])

/************************************************************************
*
* window_subband()
*
* PURPOSE:  Overlapping window on PCM samples
*
* SEMANTICS:
* 32 16-bit pcm samples are scaled to fractional 2's complement and
* concatenated to the end of the window buffer #x#. The updated window
* buffer #x# is then windowed by the analysis window #c# to produce the
* windowed sample #z#
*
************************************************************************/

static void window_subband(short *xk, FLOAT8 d[SBLIMIT], FLOAT8 *in)
{
    int i;
    FLOAT8 s, t, *wp;
    wp = enwindow;
    {
	t  =  xk[255];
	t += (xk[223] - xk[287]) * *wp++;
	t += (xk[191] + xk[319]) * *wp++;
	t += (xk[159] - xk[351]) * *wp++;
	t += (xk[127] + xk[383]) * *wp++;
	t += (xk[ 95] - xk[415]) * *wp++;
	t += (xk[ 63] + xk[447]) * *wp++;
	t += (xk[ 31] - xk[479]) * *wp++;
	in[15] = t;
    }

    for (i = 14; i >= 0; --i) {
	short *x1 = &xk[i];
	short *x2 = &xk[-i];
	FLOAT8 w;

	s = x2[270]; t = x1[240];
	w = *wp++; s += x2[334] * w; t += x1[176] * w;
	w = *wp++; s += x2[398] * w; t += x1[112] * w;
	w = *wp++; s += x2[462] * w; t += x1[ 48] * w;
	w = *wp++; s += x2[ 14] * w; t += x1[496] * w;
	w = *wp++; s += x2[ 78] * w; t += x1[432] * w;
	w = *wp++; s += x2[142] * w; t += x1[368] * w;
	w = *wp++; s += x2[206] * w; t += x1[304] * w;

	w = *wp++; s += x1[ 16] * w; t -= x2[494] * w;
	w = *wp++; s += x1[ 80] * w; t -= x2[430] * w;
	w = *wp++; s += x1[144] * w; t -= x2[366] * w;
	w = *wp++; s += x1[208] * w; t -= x2[302] * w;
	w = *wp++; s -= x1[272] * w; t += x2[238] * w;
	w = *wp++; s -= x1[336] * w; t += x2[174] * w;
	w = *wp++; s -= x1[400] * w; t += x2[110] * w;
	w = *wp++; s -= x1[464] * w; t += x2[ 46] * w;

	in[30 - i] = s;
	in[i] = t;
    }

    {
	s  = xk[239];
	s += xk[175] * *wp++;
	s += xk[111] * *wp++;
	s += xk[ 47] * *wp++;
	s -= xk[303] * *wp++;
	s -= xk[367] * *wp++;
	s -= xk[431] * *wp++;
	s -= xk[495] * *wp++;
	/* in[-1] = s;  */
    }

    in++;
    wp = &mm[0][0];
    for (i = 15; i >= 0; --i) {
	int j;
	FLOAT8 s0 = s; /* mm[i][0] is always 1 */
	FLOAT8 s1 = t * *wp++;
	for (j = 14; j >= 0; j--) {
	    s0 += *wp++ * *in++;
	    s1 += *wp++ * *in++;
	}
	in -= 30;
	d[i     ] = s0 + s1;
	d[31 - i] = s0 - s1;
    }
}


/*-------------------------------------------------------------------*/
/*                                                                   */
/*   Function: Calculation of the MDCT                               */
/*   In the case of long blocks (type 0,1,3) there are               */
/*   36 coefficents in the time domain and 18 in the frequency       */
/*   domain.                                                         */
/*   In the case of short blocks (type 2) there are 3                */
/*   transformations with short length. This leads to 12 coefficents */
/*   in the time and 6 in the frequency domain. In this case the     */
/*   results are stored side by side in the vector out[].            */
/*                                                                   */
/*   New layer3                                                      */
/*                                                                   */
/*-------------------------------------------------------------------*/

static void mdct_short(FLOAT8 *out, FLOAT8 *in)
{
    int m;
    for (m = NS / 2 - 1; m >= 0; --m) {
	int l;
	FLOAT8 a0, a1, a2, a3, a4, a5;
	a0 = cos_s[m][0];
	a1 = cos_s[m][1];
	a2 = cos_s[m][2];
	a3 = cos_s[m][3];
	a4 = cos_s[m][4];
	a5 = cos_s[m][5];
	for (l = 2; l >= 0; l--) {
	    out[3 * m + l] =
		a0 * in[6 * l    ] +
		a1 * in[6 * l + 1] +
		a2 * in[6 * l + 2] +
		a3 * in[6 * l + 3] +
		a4 * in[6 * l + 4] +
		a5 * in[6 * l + 5];
	}
    }
}

static void mdct_long(FLOAT8 *out, FLOAT8 *in)
{
    FLOAT8 s0, s1, s2, s3, s4, s5;
    int j = sizeof(all) / sizeof(int) - 1;
    FLOAT8 *cos_l0 = cos_l;
    do {
	out[all[j]] =
	    in[ 0] * cos_l0[ 0] +
	    in[ 1] * cos_l0[ 1] +
	    in[ 2] * cos_l0[ 2] +
	    in[ 3] * cos_l0[ 3] +
	    in[ 4] * cos_l0[ 4] +
	    in[ 5] * cos_l0[ 5] +
	    in[ 6] * cos_l0[ 6] +
	    in[ 7] * cos_l0[ 7] +
	    in[ 8] * cos_l0[ 8] +
	    in[ 9] * cos_l0[ 9] +
	    in[10] * cos_l0[10] +
	    in[11] * cos_l0[11] +
	    in[12] * cos_l0[12] +
	    in[13] * cos_l0[13] +
	    in[14] * cos_l0[14] +
	    in[15] * cos_l0[15] +
	    in[16] * cos_l0[16] +
	    in[17] * cos_l0[17];
	cos_l0 += 18;
    } while (--j >= 0);

    s0 = in[0] + in[ 5] + in[15];
    s1 = in[1] + in[ 4] + in[16];
    s2 = in[2] + in[ 3] + in[17];
    s3 = in[6] - in[ 9] + in[14];
    s4 = in[7] - in[10] + in[13];
    s5 = in[8] - in[11] + in[12];

    /* 16 */
    out[16] =
	s0 * cos_l0[0] + s1 * cos_l0[1] + s2 * cos_l0[2] +
	s3 * cos_l0[3] + s4 * cos_l0[4] + s5 * cos_l0[5];
    cos_l0 += 6;

    /* 10 */
    out[10] =
	s0 * cos_l0[0] + s1 * cos_l0[1] + s2 * cos_l0[2] +
	s3 * cos_l0[3] + s4 * cos_l0[4] + s5 * cos_l0[5];
    cos_l0 += 6;

    /* 7 */
    out[7] =
	s0 * cos_l0[0] + s1 * cos_l0[1] + s2 * cos_l0[2] +
	s3 * cos_l0[3] + s4 * cos_l0[4] + s5 * cos_l0[5];
    cos_l0 += 6;

    /* 1 */
    out[1] =
	s0 * cos_l0[0] + s1 * cos_l0[1] + s2 * cos_l0[2] +
	s3 * cos_l0[3] + s4 * cos_l0[4] + s5 * cos_l0[5];
    cos_l0 += 6;

    s0 = s0 - s1 + s5;
    s2 = s2 - s3 - s4;
    /* 13 */
    out[13] = s0 * cos_l0[0] + s2 * cos_l0[1];

    /* 4 */
    out[4] = s0 * cos_l0[2] + s2 * cos_l0[3];
}


void mdct_sub48(lame_global_flags *gfp,
    short *w0, short *w1,
    FLOAT8 mdct_freq[2][2][576],
    III_side_info_t *l3_side)
{
    int gr, k, ch;
    short *wk;
    static int init = 0;

    if ( init == 0 ) {
        void mdct_init48(void);
	mdct_init48();
	init++;
    }

    wk = w0;
    /* thinking cache performance, ch->gr loop is better than gr->ch loop */
    for (ch = 0; ch < gfp->stereo; ch++) {
	for (gr = 0; gr < gfp->mode_gr; gr++) {
	    int	band;
	    FLOAT8 *mdct_enc = mdct_freq[gr][ch];
	    gr_info *gi = &(l3_side->gr[gr].ch[ch].tt);
	    FLOAT8 *samp = sb_sample[ch][1 - gr][0];

	    for (k = 0; k < 18 / 2; k++) {
		window_subband(wk, samp, work);
		window_subband(wk + 32, samp + 32, work);
		/*
		 * Compensate for inversion in the analysis filter
		 */
		for (band = 1; band < 32; band += 2)
		    samp[band + 32] *= -1.0;
		samp += 64;
		wk += 64;
	    }


	    /* apply filters on the polyphase filterbank outputs */
	    /* bands <= gfp->highpass_band will be zeroed out below */
	    /* bands >= gfp->lowpass_band  will be zeroed out below */
	    if (gfp->filter_type==0) {
	      FLOAT8 amp,freq;
	      for (band=gfp->highpass_band+1;  band < gfp->lowpass_band ; band++) { 
		freq = band/31.0;
		if (gfp->lowpass1 < freq && freq < gfp->lowpass2) {
		  amp = cos((PI/2)*(gfp->lowpass1-freq)/(gfp->lowpass2-gfp->lowpass1));
		  for (k=0; k<18; k++) 
		    sb_sample[ch][1-gr][k][band]*=amp;
		}
		if (gfp->highpass1 < freq && freq < gfp->highpass2) {
		  amp = cos((PI/2)*(gfp->highpass2-freq)/(gfp->highpass2-gfp->highpass1));
		  for (k=0; k<18; k++) 
		    sb_sample[ch][1-gr][k][band]*=amp;
		}
	      }
	    }
	    


	    /*
	     * Perform imdct of 18 previous subband samples
	     * + 18 current subband samples
	     */
	    for (band = 0; band < 32; band++, mdct_enc += 18) 
              {
		int type = gi->block_type;
#ifdef ALLOW_MIXED
		if (gi->mixed_block_flag && band < 2)
		    type = 0;
#endif
		if (band >= gfp->lowpass_band || band <= gfp->highpass_band) {
		    memset((char *)mdct_enc,0,18*sizeof(FLOAT8));
		}else {
		  if (type == SHORT_TYPE) {
		    for (k = 2; k >= 0; --k) {
		      FLOAT8 w1 = win[SHORT_TYPE][k];
		      work[k] =
			sb_sample[ch][gr][k+6][band] * w1 -
			sb_sample[ch][gr][11-k][band];
		      work[k+3] =
			sb_sample[ch][gr][k+12][band] +
			sb_sample[ch][gr][17-k][band] * w1;
		      
		      work[k+6] =
			sb_sample[ch][gr][k+12][band] * w1 -
			sb_sample[ch][gr][17-k][band];
		      work[k+9] =
			sb_sample[ch][1-gr][k][band] +
			sb_sample[ch][1-gr][5-k][band] * w1;
		      
		      work[k+12] =
			sb_sample[ch][1-gr][k][band] * w1 -
			sb_sample[ch][1-gr][5-k][band];
		      work[k+15] =
			sb_sample[ch][1-gr][k+6][band] +
			sb_sample[ch][1-gr][11-k][band] * w1;
		    }
		    mdct_short(mdct_enc, work);
		  } else {
		    for (k = 8; k >= 0; --k) {
		      work[k] =
			win[type][k  ] * sb_sample[ch][gr][k   ][band]
			- win[type][k+9] * sb_sample[ch][gr][17-k][band];
		      
		      work[9+k] =
			win[type][k+18] * sb_sample[ch][1-gr][k   ][band]
			+ win[type][k+27] * sb_sample[ch][1-gr][17-k][band];
		    }
		    mdct_long(mdct_enc, work);
		  }
		}
		
		
		/*
		  Perform aliasing reduction butterfly
		*/
		if (type != SHORT_TYPE) {
		  if (band == 0)
		    continue;
		  for (k = 7; k >= 0; --k) {
		    FLOAT8 bu,bd;
		    bu = mdct_enc[k] * ca[k] + mdct_enc[-1-k] * cs[k];
		    bd = mdct_enc[k] * cs[k] - mdct_enc[-1-k] * ca[k];
		    
		    mdct_enc[-1-k] = bu;
		    mdct_enc[k]    = bd;
		  }
		}
	      }
	}
	wk = w1;
	if (gfp->mode_gr == 1) {
	    memcpy(sb_sample[ch][0], sb_sample[ch][1], 576 * sizeof(FLOAT8));
	}
    }
}



void mdct_init48(void)
{
    int i, k, m;
    FLOAT8 sq;
    FLOAT8 max;

    /* prepare the aliasing reduction butterflies */
    for (k = 0; k < 8; k++) {
	/*
	  This is table B.9: coefficients for aliasing reduction
	  */
	static const FLOAT8 c[8] = {
	    -0.6,-0.535,-0.33,-0.185,-0.095,-0.041,-0.0142, -0.0037
	};
	sq = 1.0 + c[k] * c[k];
	sq = sqrt(sq);
	ca[k] = c[k] / sq;
	cs[k] = 1.0 / sq;
    }

    /* type 0*/
    for (i = 0; i < 36; i++)
	win[0][i] = sin(PI/36 * (i + 0.5));
    /* type 1*/
    for (i = 0; i < 18; i++) 
	win[1][i] = win[0][i];
    for (; i < 24; i++)
	win[1][i] = 1.0;
    for (; i < 30; i++)
	win[1][i] = cos(PI/12 * (i + 0.5));
    for (; i < 36; i++)
	win[1][i] = 0.0;
    /* type 3*/
    for (i = 0; i < 36; i++)
	win[3][i] = win[1][35 - i];

    sq = 4.0 / NL;
    {
	FLOAT8 *cos_l0 = cos_l;
	static const int d3[] = {1,7,10,16};
	static const int d9[] = {4,13};

	int j = sizeof(all) / sizeof(int) - 1;
	do {
	    m = all[j];
	    for (k = 0; k < NL / 4; k++) {
		*cos_l0++ = sq *
		    cos((PI / (4 * NL)) * (2 * m + 1) * (4 * k + 2 + NL));
	    }
	    for (k = 0; k < NL / 4; k++) {
		*cos_l0++ = sq *
		    cos((PI / (4 * NL)) * (2 * m + 1) * (4 * k + 2 + NL * 3));
	    }
	} while (--j >= 0);

	j = sizeof(d3) / sizeof(int) - 1;
	do {
	    m = d3[j];
	    for (k = 0; k < 3; k++) {
		*cos_l0++ = sq *
		    cos((PI / (4 * NL)) * (2 * m + 1) * (4 * k + 2 + NL));
	    }
	    for (k = 6; k < 9; k++) {
		*cos_l0++ = sq *
		    cos((PI / (4 * NL)) * (2 * m + 1) * (4 * k + 2 + NL));
	    }
	} while (--j >= 0);

	j = sizeof(d9) / sizeof(int) - 1;
	do {
	    m = d9[j];
	    *cos_l0++ = sq *
		cos((PI / (4 * NL)) * (2 * m + 1) * (2 + NL));
	    *cos_l0++ = sq *
		cos((PI / (4 * NL)) * (2 * m + 1) * (4 * 2 + 2 + NL));
	} while (--j >= 0);
    }

    max = enwindow[256 - 8];
    {
	FLOAT8 *wp = enwindow;
	FLOAT8 *wr = enwindow;
	FLOAT8 mmax[32 - 1];

	{
	    FLOAT8 w = *wp++;
	    mmax[15] = w / max;

	    for (k = 0; k < 7; k++) {
		*wr++ = *wp++ / w;
	    }
	}

	for (i = 14; i >= 0; --i) {
	    FLOAT8 w = *wp++;
	    mmax[i] = mmax[30 - i] = w / max;

	    for (k = 0; k < 15; k++) {
		*wr++ = *wp++ / w;
	    }
	}

	{
	    wp++;
	    for (k = 0; k < 7; k++) {
		*wr++ = *wp++ / max;
	    }
	}

	wp = &mm[0][0];
	for (i = 15; i >= 0; --i) {
	    for (k = 1; k < 32; k++) {
		*wp++ = cos((2 * i + 1) * k * PI/64) * mmax[k - 1];
	    }
	}
    }

    /* swap window data*/
    for (k = 0; k < 4; k++) {
	FLOAT8 a;

	a = win[0][17-k];
	win[0][17-k] = win[0][9+k];
	win[0][9+k] = a;

	a = win[0][35-k];
	win[0][35-k] = win[0][27+k];
	win[0][27+k] = a;

	a = win[1][17-k];
	win[1][17-k] = win[1][9+k];
	win[1][9+k] = a;

	a = win[1][35-k];
	win[1][35-k] = win[1][27+k];
	win[1][27+k] = a;

	a = win[3][17-k];
	win[3][17-k] = win[3][9+k];
	win[3][9+k] = a;

	a = win[3][35-k];
	win[3][35-k] = win[3][27+k];
	win[3][27+k] = a;
    }

    for (i = 0; i < 36; i++) {
	win[0][i] *= max / SCALE;
	win[1][i] *= max / SCALE;
	win[3][i] *= max / SCALE;
    }

    /* type 2(short)*/
    sq = 4.0 / NS;
    for (i = 0; i < NS / 4; i++) {
	FLOAT8 w2 = cos(PI/12 * (i + 0.5)) * max / SCALE * sq;
	win[SHORT_TYPE][i] = tan(PI/12 * (i + 0.5));

	for (m = 0; m < NS / 2; m++) {
	    cos_s[m][i] = w2 *
		cos((PI / (4 * NS)) * (2 * m + 1) * (4 * i + 2 + NS));
	    cos_s[m][i + NS / 4] = w2 *
		cos((PI / (4 * NS)) * (2 * m + 1) * (4 * i + 2 + NS * 3));
	}
    }
}


/* ==== parse.c ==== */
#ifdef LAMEPARSE

#include "util.h"
#include "id3tag.h"
#include "get_audio.h"
#include "brhist.h"
#include "version.h"



#define         MAX_NAME_SIZE           300
  char    inPath[MAX_NAME_SIZE];
  char    outPath[MAX_NAME_SIZE];


/************************************************************************
*
* usage
*
* PURPOSE:  Writes command line syntax to the file specified by #stderr#
*
************************************************************************/

void lame_usage(lame_global_flags *gfp,char *name)  /* print syntax & exit */
{
  lame_print_version(stderr);
  fprintf(stderr,"\n");
  fprintf(stderr,"USAGE   :  %s [options] <infile> [outfile]\n",name);
  fprintf(stderr,"\n<infile> and/or <outfile> can be \"-\", which means stdin/stdout.\n");
  fprintf(stderr,"\n");
  fprintf(stderr,"Try \"%s --help\" for more information\n",name);
  exit(1);
}



/************************************************************************
*
* usage
*
* PURPOSE:  Writes command line syntax to the file specified by #stdout#
*
************************************************************************/

void lame_help(lame_global_flags *gfp,char *name)  /* print syntax & exit */
{
  lame_print_version(stdout);
  fprintf(stdout,"\n");
  fprintf(stdout,"USAGE   :  %s [options] <infile> [outfile]\n",name);
  fprintf(stdout,"\n<infile> and/or <outfile> can be \"-\", which means stdin/stdout.\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"OPTIONS :\n");
  fprintf(stdout,"  Input options:\n");
  fprintf(stdout,"    -r              input is raw pcm\n");
  fprintf(stdout,"    -x              force byte-swapping of input\n");
  fprintf(stdout,"    -s sfreq        sampling frequency of input file(kHz) - default 44.1kHz\n");
  fprintf(stdout,"    --mp3input      input file is a MP3 file\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  Filter options:\n");
  fprintf(stdout,"    -k              keep ALL frequencies (disables all filters)\n");
  fprintf(stdout,"  --lowpass freq         frequency(kHz), lowpass filter cutoff above freq\n");
  fprintf(stdout,"  --lowpass-width freq   frequency(kHz) - default 15%% of lowpass freq\n");
  fprintf(stdout,"  --highpass freq        frequency(kHz), highpass filter cutoff below freq\n");
  fprintf(stdout,"  --highpass-width freq  frequency(kHz) - default 15%% of highpass freq\n");
  fprintf(stdout,"  --resample sfreq  sampling frequency of output file(kHz)- default=input sfreq\n");
  fprintf(stdout,"  --cwlimit freq    compute tonality up to freq (in kHz) default 8.8717\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  Operational options:\n");
  fprintf(stdout,"    -m mode         (s)tereo, (j)oint, (f)orce or (m)ono  (default j)\n");
  fprintf(stdout,"                    force = force ms_stereo on all frames. Faster\n");
  fprintf(stdout,"    -a              downmix from stereo to mono file for mono encoding\n");
  fprintf(stdout,"    -d              allow channels to have different blocktypes\n");
  fprintf(stdout,"    -S              don't print progress report, VBR histograms\n");
  fprintf(stdout,"    --athonly       only use the ATH for masking\n");
  fprintf(stdout,"    --noath         disable the ATH for masking\n");
  fprintf(stdout,"    --noshort       do not use short blocks\n");
  fprintf(stdout,"    --voice         experimental voice mode\n");
  fprintf(stdout,"    --preset type   type must be phone, voice, fm, tape, hifi, cd or studio\n");
  fprintf(stdout,"                    help gives some more infos on these\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  CBR (constant bitrate, the default) options:\n");
  fprintf(stdout,"    -h              higher quality, but a little slower.  Recommended.\n");
  fprintf(stdout,"    -f              fast mode (very low quality)\n");
  fprintf(stdout,"    -b bitrate      set the bitrate, default 128kbps\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  VBR options:\n");
  fprintf(stdout,"    -v              use variable bitrate (VBR)\n");
  fprintf(stdout,"    -V n            quality setting for VBR.  default n=%i\n",gfp->VBR_q);
  fprintf(stdout,"                    0=high quality,bigger files. 9=smaller files\n");
  fprintf(stdout,"    -b bitrate      specify minimum allowed bitrate, default 32kbs\n");
  fprintf(stdout,"    -B bitrate      specify maximum allowed bitrate, default 256kbs\n");
  fprintf(stdout,"    -t              disable Xing VBR informational tag\n");
  fprintf(stdout,"    --nohist        disable VBR histogram display\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  MP3 header/stream options:\n");
  fprintf(stdout,"    -e emp          de-emphasis n/5/c  (obsolete)\n");
  fprintf(stdout,"    -c              mark as copyright\n");
  fprintf(stdout,"    -o              mark as non-original\n");
  fprintf(stdout,"    -p              error protection.  adds 16bit checksum to every frame\n");
  fprintf(stdout,"                    (the checksum is computed correctly)\n");
  fprintf(stdout,"    --nores         disable the bit reservoir\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  Specifying any of the following options will add an ID3 tag:\n");
  fprintf(stdout,"     --tt \"title\"     title of song (max 30 chars)\n");
  fprintf(stdout,"     --ta \"artist\"    artist who did the song (max 30 chars)\n");
  fprintf(stdout,"     --tl \"album\"     album where it came from (max 30 chars)\n");
  fprintf(stdout,"     --ty \"year\"      year in which the song/album was made (max 4 chars)\n");
  fprintf(stdout,"     --tc \"comment\"   additional info (max 30 chars)\n");
  fprintf(stdout,"                      (or max 28 chars if using the \"track\" option)\n");
  fprintf(stdout,"     --tn \"track\"     track number of the song on the CD (1 to 99)\n");
  fprintf(stdout,"                      (using this option will add an ID3v1.1 tag)\n");
  fprintf(stdout,"     --tg \"genre\"     genre of song (name or number)\n");
  fprintf(stdout,"\n");
#ifdef HAVEGTK
  fprintf(stdout,"    -g              run graphical analysis on <infile>\n");
#endif
  display_bitrates(stdout);
  exit(0);
}



/************************************************************************
*
* usage
*
* PURPOSE:  Writes presetting info to #stdout#
*
************************************************************************/

void lame_presets_info(lame_global_flags *gfp,char *name)  /* print syntax & exit */
{
  lame_print_version(stdout);
  fprintf(stdout,"\n");
  fprintf(stdout,"Presets are some shortcuts for common settings.\n");
  fprintf(stdout,"They can be combined with -v if you want VBR MP3s.\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset phone    =>  --resample      16\n");
  fprintf(stdout,"                        --highpass       0.260\n");
  fprintf(stdout,"                        --highpasswidth  0.040\n");
  fprintf(stdout,"                        --lowpass        3.700\n");
  fprintf(stdout,"                        --lowpasswidth   0.300\n");
  fprintf(stdout,"                        --noshort\n");
  fprintf(stdout,"                        -m   m\n");
  fprintf(stdout,"                        -b  16\n");
  fprintf(stdout,"                  plus  -b   8  \\\n");
  fprintf(stdout,"                        -B  56   > in combination with -v\n");
  fprintf(stdout,"                        -V   5  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset voice:   =>  --resample      24\n");
  fprintf(stdout,"                        --highpass       0.100\n");
  fprintf(stdout,"                        --highpasswidth  0.020\n");
  fprintf(stdout,"                        --lowpass       11\n");
  fprintf(stdout,"                        --lowpasswidth   2\n");
  fprintf(stdout,"                        --noshort\n");
  fprintf(stdout,"                        -m   m\n");
  fprintf(stdout,"                        -b  32\n");
  fprintf(stdout,"                  plus  -b   8  \\\n");
  fprintf(stdout,"                        -B  96   > in combination with -v\n");
  fprintf(stdout,"                        -V   4  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset fm:      =>  --resample      32\n");
  fprintf(stdout,"                        --highpass       0.030\n");
  fprintf(stdout,"                        --highpasswidth  0\n");
  fprintf(stdout,"                        --lowpass       11.4\n");
  fprintf(stdout,"                        --lowpasswidth   0\n");
  fprintf(stdout,"                        -m   j\n");
  fprintf(stdout,"                        -b  96\n");
  fprintf(stdout,"                  plus  -b  32  \\\n");
  fprintf(stdout,"                        -B 192   > in combination with -v\n");
  fprintf(stdout,"                        -V   4  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset tape:    =>  --lowpass       17\n");
  fprintf(stdout,"                        --lowpasswidth   2\n");
  fprintf(stdout,"                        --highpass       0.015\n");
  fprintf(stdout,"                        --highpasswidth  0.015\n");
  fprintf(stdout,"                        -m   j\n");
  fprintf(stdout,"                        -b 128\n");
  fprintf(stdout,"                  plus  -b  32  \\\n");
  fprintf(stdout,"                        -B 192   > in combination with -v\n");
  fprintf(stdout,"                        -V   4  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset hifi:    =>  --lowpass       20\n");
  fprintf(stdout,"                        --lowpasswidth   3\n");
  fprintf(stdout,"                        --highpass       0.015\n");
  fprintf(stdout,"                        --highpasswidth  0.015\n");
  fprintf(stdout,"                        -h\n");
  fprintf(stdout,"                        -m   j\n");
  fprintf(stdout,"                        -b 160\n");
  fprintf(stdout,"                  plus  -b  32  \\\n");
  fprintf(stdout,"                        -B 224   > in combination with -v\n");
  fprintf(stdout,"                        -V   3  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset cd:      =>  -k\n");
  fprintf(stdout,"                        -h\n");
  fprintf(stdout,"                        -m   s\n");
  fprintf(stdout,"                        -b 192\n");
  fprintf(stdout,"                  plus  -b  80  \\\n");
  fprintf(stdout,"                        -B 256   > in combination with -v\n");
  fprintf(stdout,"                        -V   2  /\n");
  fprintf(stdout,"\n");
  fprintf(stdout,"  --preset studio:  =>  -k\n");
  fprintf(stdout,"                        -h\n");
  fprintf(stdout,"                        -m   s\n");
  fprintf(stdout,"                        -b 256\n");
  fprintf(stdout,"                  plus  -b 112  \\\n");
  fprintf(stdout,"                        -B 320   > in combination with -v\n");
  fprintf(stdout,"                        -V   0  /\n");
  fprintf(stdout,"\n");

  exit(0);
}



/************************************************************************
*
* parse_args
*
* PURPOSE:  Sets encoding parameters to the specifications of the
* command line.  Default settings are used for parameters
* not specified in the command line.
*
* If the input file is in WAVE or AIFF format, the sampling frequency is read
* from the AIFF header.
*
* The input and output filenames are read into #inpath# and #outpath#.
*
************************************************************************/
void lame_parse_args(lame_global_flags *gfp,int argc, char **argv)
{
  FLOAT srate;
  int   err = 0, i = 0;
  int autoconvert=0;
  int user_quality=0;

  char *programName = argv[0]; 
  int track = 0;

  inPath[0] = '\0';   
  outPath[0] = '\0';
  gfp->inPath=inPath;
  gfp->outPath=outPath;

  id3_inittag(&id3tag);
  id3tag.used = 0;

  /* process args */
  while(++i<argc && err == 0) {
    char c, *token, *arg, *nextArg;
    int  argUsed;
    
    token = argv[i];
    if(*token++ == '-') {
      if(i+1 < argc) nextArg = argv[i+1];
      else           nextArg = "";
      argUsed = 0;
      if (! *token) {
	/* The user wants to use stdin and/or stdout. */
	if(inPath[0] == '\0')       strncpy(inPath, argv[i],MAX_NAME_SIZE);
	else if(outPath[0] == '\0') strncpy(outPath, argv[i],MAX_NAME_SIZE);
      } 
      if (*token == '-') {
	/* GNU style */
	token++;

	if (strcmp(token, "resample")==0) {
	  argUsed=1;
	  srate = atof( nextArg );
	  /* samplerate = rint( 1000.0 * srate ); $A  */
	  gfp->out_samplerate =  (( 1000.0 * srate ) + 0.5);
	  if (srate  < 1) {
	    fprintf(stderr,"Must specify a samplerate with --resample\n");
	    exit(1);
	  }
	}
	else if (strcmp(token, "mp3input")==0) {
	  gfp->input_format=sf_mp3;
	}
	else if (strcmp(token, "voice")==0) {
	  gfp->lowpassfreq=12000;
	  gfp->VBR_max_bitrate_kbps=160;
	  gfp->no_short_blocks=1;
	}
	else if (strcmp(token, "noshort")==0) {
	  gfp->no_short_blocks=1;
	}
	else if (strcmp(token, "noath")==0) {
	  gfp->noATH=1;
	}
	else if (strcmp(token, "nores")==0) {
	  gfp->disable_reservoir=1;
	  gfp->padding=0;
	}
	else if (strcmp(token, "athonly")==0) {
	  gfp->ATHonly=1;
	}
	else if (strcmp(token, "nohist")==0) {
#ifdef BRHIST
	  disp_brhist = 0;
#endif
	}
	/* options for ID3 tag */
 	else if (strcmp(token, "tt")==0) {
 		id3tag.used=1;      argUsed = 1;
  		strncpy(id3tag.title, nextArg, 30);
 		}
 	else if (strcmp(token, "ta")==0) {
 		id3tag.used=1; argUsed = 1;
  		strncpy(id3tag.artist, nextArg, 30);
 		}
 	else if (strcmp(token, "tl")==0) {
 		id3tag.used=1; argUsed = 1;
  		strncpy(id3tag.album, nextArg, 30);
 		}
 	else if (strcmp(token, "ty")==0) {
 		id3tag.used=1; argUsed = 1;
  		strncpy(id3tag.year, nextArg, 4);
 		}
 	else if (strcmp(token, "tc")==0) {
 		id3tag.used=1; argUsed = 1;
  		strncpy(id3tag.comment, nextArg, 30);
 		}
 	else if (strcmp(token, "tn")==0) {
 		id3tag.used=1; argUsed = 1;
  		track = atoi(nextArg);
  		if (track < 1) { track = 1; }
  		if (track > 99) { track = 99; }
  		id3tag.track = track;
 		}
 	else if (strcmp(token, "tg")==0) {
		argUsed = strtol (nextArg, &token, 10);
		if (nextArg==token) {
		  /* Genere was given as a string, so it's number*/
		  for (argUsed=0; argUsed<=genre_last; argUsed++) {
		    if (!strcmp (genre_list[argUsed], nextArg)) { break; }
		  }
 		}
		if (argUsed>genre_last) { 
		  argUsed=255; 
		  fprintf(stderr,"Unknown genre: %s.  Specifiy genre number \n", nextArg);
		}
	        argUsed &= 255; c=(char)(argUsed);

 		id3tag.used=1; argUsed = 1;
  		strncpy(id3tag.genre, &c, 1);
	       }
	else if (strcmp(token, "lowpass")==0) {
	  argUsed=1;
	  gfp->lowpassfreq =  (( 1000.0 * atof( nextArg ) ) + 0.5);
	  if (gfp->lowpassfreq  < 1) {
	    fprintf(stderr,"Must specify lowpass with --lowpass freq, freq >= 0.001 kHz\n");
	    exit(1);
	  }
	}
	else if (strcmp(token, "lowpass-width")==0) {
	  argUsed=1;
	  gfp->lowpasswidth =  (( 1000.0 * atof( nextArg ) ) + 0.5);
	  if (gfp->lowpasswidth  < 0) {
	    fprintf(stderr,"Must specify lowpass width with --lowpass-width freq, freq >= 0 kHz\n");
	    exit(1);
	  }
	}
	else if (strcmp(token, "highpass")==0) {
	  argUsed=1;
	  gfp->highpassfreq =  (( 1000.0 * atof( nextArg ) ) + 0.5);
	  if (gfp->highpassfreq  < 1) {
	    fprintf(stderr,"Must specify highpass with --highpass freq, freq >= 0.001 kHz\n");
	    exit(1);
	  }
	}
	else if (strcmp(token, "highpass-width")==0) {
	  argUsed=1;
	  gfp->highpasswidth =  (( 1000.0 * atof( nextArg ) ) + 0.5);
	  if (gfp->highpasswidth  < 0) {
	    fprintf(stderr,"Must specify highpass width with --highpass-width freq, freq >= 0 kHz\n");
	    exit(1);
	  }
	}
	else if (strcmp(token, "cwlimit")==0) {
	  argUsed=1;
	  gfp->cwlimit =  atof( nextArg );
	  if (gfp->cwlimit <= 0 ) {
	    fprintf(stderr,"Must specify cwlimit in kHz\n");
	    exit(1);
	  }
	} /* some more GNU-ish options could be added
	   * version       => complete name, version and license info (normal exit)  
	   * quiet/silent  => no messages on screen
	   * brief         => few messages on screen (name, status report)
	   * verbose       => all infos to screen (brhist, internal flags/filters)
	   * o/output file => specifies output filename
	   * O             => stdout
	   * i/input file  => specifies input filename
	   * I             => stdin
	   */
	else if (strcmp(token, "help") ==0
	       ||strcmp(token, "usage")==0){
	  lame_help(gfp,programName);  /* doesn't return */
	}
	else if (strcmp(token, "preset")==0) {
	  argUsed=1;
	  if (strcmp(nextArg,"phone")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 16; 
	    gfp->highpassfreq=260;
            gfp->highpasswidth=40; 
	    gfp->lowpassfreq=3700;
	    gfp->lowpasswidth=300;
	    gfp->VBR_q=5;
	    gfp->VBR_min_bitrate_kbps=8;
	    gfp->VBR_max_bitrate_kbps=56;
	    gfp->no_short_blocks=1;
	    gfp->out_samplerate =  16000;
	    gfp->mode = MPG_MD_MONO; 
	    gfp->mode_fixed = 1; 
	    gfp->quality = 5;
	  }
	  else if (strcmp(nextArg,"voice")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 56; 
	    gfp->highpassfreq=100;  
	    gfp->highpasswidth=20;
	    gfp->lowpasswidth=2000;
	    gfp->lowpassfreq=11000;
	    gfp->VBR_q=4;
	    gfp->VBR_min_bitrate_kbps=8;
	    gfp->VBR_max_bitrate_kbps=96;
	    gfp->no_short_blocks=1;
	    gfp->mode = MPG_MD_MONO; 
	    gfp->mode_fixed = 1; 
	    gfp->out_samplerate =  24000; 
	    gfp->quality = 5;
	  }
	  else if (strcmp(nextArg,"fm")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 96; 
            gfp->highpassfreq=30;
            gfp->highpasswidth=0;
            gfp->lowpassfreq=15000;
            gfp->lowpasswidth=0;
	    gfp->VBR_q=4;
	    gfp->VBR_min_bitrate_kbps=32;
	    gfp->VBR_max_bitrate_kbps=192;
	    gfp->mode = MPG_MD_JOINT_STEREO; 
	    gfp->mode_fixed = 1; 
	    /*gfp->out_samplerate =  32000; */ /* determined automatically based on bitrate & sample freq. */
	    gfp->quality = 5;
	  }
	  else if (strcmp(nextArg,"tape")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 128; 
            gfp->highpassfreq=15;
            gfp->highpasswidth=15;
            gfp->lowpassfreq=17000;
            gfp->lowpasswidth=2000;
	    gfp->VBR_q=4;
	    gfp->VBR_min_bitrate_kbps=32;
	    gfp->VBR_max_bitrate_kbps=192;
	    gfp->mode = MPG_MD_JOINT_STEREO; 
	    gfp->mode_fixed = 1; 
	    gfp->quality = 5;
	  }
	  else if (strcmp(nextArg,"hifi")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 160;            
	    gfp->highpassfreq=15;
            gfp->highpasswidth=15;
            gfp->lowpassfreq=20000;
            gfp->lowpasswidth=3000;
	    gfp->VBR_q=3;
	    gfp->VBR_min_bitrate_kbps=32;
	    gfp->VBR_max_bitrate_kbps=224;
	    gfp->mode = MPG_MD_JOINT_STEREO; 
	    gfp->mode_fixed = 1; 
	    gfp->quality = 2;
	  }
	  else if (strcmp(nextArg,"cd")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 192;  
	    gfp->lowpassfreq=-1;
            gfp->highpassfreq=-1;
	    gfp->VBR_q=2;
	    gfp->VBR_min_bitrate_kbps=80;
	    gfp->VBR_max_bitrate_kbps=256;
	    gfp->mode = MPG_MD_STEREO; 
	    gfp->mode_fixed = 1; 
	    gfp->quality = 2;
	  }
	  else if (strcmp(nextArg,"studio")==0)
	  { /* when making changes, please update help text too */
	    gfp->brate = 256; 
	    gfp->lowpassfreq=-1;
            gfp->highpassfreq=-1;
	    gfp->VBR_q=0;
	    gfp->VBR_min_bitrate_kbps=112;
	    gfp->VBR_max_bitrate_kbps=320;
	    gfp->mode = MPG_MD_STEREO; 
	    gfp->mode_fixed = 1; 
	    gfp->quality = 2; /* should be 0, but does not work now */
	  }
	  else if (strcmp(nextArg,"help")==0)
	  {
	    lame_presets_info(gfp,programName);  /* doesn't return */
	  }
	  else
	    {
	      fprintf(stderr,"%s: --preset type, type must be phone, voice, fm, tape, hifi, cd or studio, not %s\n",
		      programName, nextArg);
	      exit(1);
	    }
	} /* --preset */
	else
	  {
	    fprintf(stderr,"%s: unrec option --%s\n",
		    programName, token);
	  }
	i += argUsed;
	
      } else  while( (c = *token++) ) {
	if(*token ) arg = token;
	else                             arg = nextArg;
	switch(c) {
	case 'm':        argUsed = 1;   gfp->mode_fixed = 1;
	  if (*arg == 's')
	    { gfp->mode = MPG_MD_STEREO; }
	  else if (*arg == 'd')
	    { gfp->mode = MPG_MD_DUAL_CHANNEL; }
	  else if (*arg == 'j')
	    { gfp->mode = MPG_MD_JOINT_STEREO; }
	  else if (*arg == 'f')
	    { gfp->mode = MPG_MD_JOINT_STEREO; gfp->force_ms=1; }
	  else if (*arg == 'm')
	    { gfp->mode = MPG_MD_MONO; }
	  else {
	    fprintf(stderr,"%s: -m mode must be s/d/j/f/m not %s\n",
		    programName, arg);
	    err = 1;
	  }
	  break;
	case 'V':        argUsed = 1;   gfp->VBR = 1;  
	  gfp->VBR_q = atoi(arg);
	  if (gfp->VBR_q <0) gfp->VBR_q=0;
	  if (gfp->VBR_q >9) gfp->VBR_q=9;
	  break;
	case 'q':        argUsed = 1; 
	  user_quality = atoi(arg);
	  if (user_quality<0) user_quality=0;
	  if (user_quality>9) user_quality=9;
	  break;
	case 's':
	  argUsed = 1;
	  srate = atof( arg );
	  /* samplerate = rint( 1000.0 * srate ); $A  */
	  gfp->in_samplerate =  (( 1000.0 * srate ) + 0.5);
	  break;
	case 'b':        
	  argUsed = 1;
	  gfp->brate = atoi(arg); 
	  gfp->VBR_min_bitrate_kbps=gfp->brate;
	  break;
	case 'B':        
	  argUsed = 1;
	  gfp->VBR_max_bitrate_kbps=atoi(arg); 
	  break;	
	case 't':  /* dont write VBR tag */
	  gfp->bWriteVbrTag=0;
	  break;
	case 'r':  /* force raw pcm input file */
#ifdef LIBSNDFILE
	  fprintf(stderr,"WARNING: libsndfile may ignore -r and perform fseek's on the input.\n");
	  fprintf(stderr,"Compile without libsndfile if this is a problem.\n");
#endif
	  gfp->input_format=sf_raw;
	  break;
	case 'x':  /* force byte swapping */
	  gfp->swapbytes=TRUE;
	  break;
	case 'p': /* (jo) error_protection: add crc16 information to stream */
	  gfp->error_protection = 1; 
	  break;
	case 'a': /* autoconvert input file from stereo to mono - for mono mp3 encoding */
	  autoconvert=1;
	  gfp->mode=MPG_MD_MONO;
	  gfp->mode_fixed=1;
	  break;
	case 'h': 
	  gfp->quality = 2;
	  break;
	case 'k': 
	  gfp->lowpassfreq=-1;
	  gfp->highpassfreq=-1;
	  break;
	case 'd': 
	  gfp->allow_diff_short = 1;
	  break;
	case 'v': 
	  gfp->VBR = 1; 
	  break;
	case 'S': 
	  gfp->silent = TRUE;
	  break;
	case 'X':        argUsed = 1;   gfp->experimentalX = 0;
	  if (*arg == '0')
	    { gfp->experimentalX=0; }
	  else if (*arg == '1')
	    { gfp->experimentalX=1; }
	  else if (*arg == '2')
	    { gfp->experimentalX=2; }
	  else if (*arg == '3')
	    { gfp->experimentalX=3; }
	  else if (*arg == '4')
	    { gfp->experimentalX=4; }
	  else if (*arg == '5')
	    { gfp->experimentalX=5; }
	  else if (*arg == '6')
	    { gfp->experimentalX=6; }
	  else {
	    fprintf(stderr,"%s: -X n must be 0-6, not %s\n",
		    programName, arg);
	    err = 1;
	  }
	  break;


	case 'Y': 
	  gfp->experimentalY = TRUE;
	  break;
	case 'Z': 
	  gfp->experimentalZ = TRUE;
	  break;
	case 'f': 
	  gfp->quality= 9;
	  break;
	case 'g': /* turn on gtk analysis */
#ifdef HAVEGTK
	  gfp->gtkflag = TRUE;
#else
	    fprintf(stderr,"LAME not compiled with GTK support, -g not supported.\n");
#endif
	  break;

	case 'e':        argUsed = 1;
	  if (*arg == 'n')                    gfp->emphasis = 0;
	  else if (*arg == '5')               gfp->emphasis = 1;
	  else if (*arg == 'c')               gfp->emphasis = 3;
	  else {
	    fprintf(stderr,"%s: -e emp must be n/5/c not %s\n",
		    programName, arg);
	    err = 1;
	  }
	  break;
	case 'c':   gfp->copyright = 1; break;
	case 'o':   gfp->original  = 0; break;
	
	case '?':   lame_help(gfp,programName);  /* doesn't return */
	default:    fprintf(stderr,"%s: unrec option %c\n",
				programName, c);
	err = 1; break;
	}
	if(argUsed) {
	  if(arg == token)    token = "";   /* no more from token */
	  else                ++i;          /* skip arg we used */
	  arg = ""; argUsed = 0;
	}
      }
    } else {
      if(inPath[0] == '\0')       strncpy(inPath, argv[i], MAX_NAME_SIZE);
      else if(outPath[0] == '\0') strncpy(outPath, argv[i], MAX_NAME_SIZE);
      else {
	fprintf(stderr,"%s: excess arg %s\n", programName, argv[i]);
	err = 1;
      }
    }
  }  /* loop over args */



  if(err || inPath[0] == '\0') lame_usage(gfp,programName);  /* never returns */
  if (inPath[0]=='-') gfp->silent=1;  /* turn off status - it's broken for stdin */
  if(outPath[0] == '\0') {
    if (inPath[0]=='-') {
      /* if input is stdin, default output is stdout */
      strcpy(outPath,"-");
    }else {
      strncpy(outPath, inPath, MAX_NAME_SIZE - 4);
      strncat(outPath, ".mp3", 4 );
    }
  }
  /* some file options not allowed with stdout */
  if (outPath[0]=='-') {
    gfp->bWriteVbrTag=0; /* turn off VBR tag */
    if (id3tag.used) {
      id3tag.used=0;         /* turn of id3 tagging */
      fprintf(stderr,"id3tag ignored: id3 tagging not supported for stdout.\n");
    }
  }


  /* if user did not explicitly specify input is mp3, check file name */
  if (gfp->input_format != sf_mp3)
    if (!(strcmp((char *) &inPath[strlen(inPath)-4],".mp3")))
      gfp->input_format = sf_mp3;

#if !(defined HAVEMPGLIB || defined AMIGA_MPEGA)
  if (gfp->input_format == sf_mp3) {
    fprintf(stderr,"Error: libmp3lame not compiled with mp3 *decoding* support \n");
    exit(1);
  }
#endif
  /* default guess for number of channels */
  if (autoconvert) gfp->num_channels=2; 
  else if (gfp->mode == MPG_MD_MONO) gfp->num_channels=1;
  else gfp->num_channels=2;

  /* user specified a quality value.  override any defaults set above */
  if (user_quality)   gfp->quality=user_quality;

}



#endif


/* ==== portableio.c ==== */
/* Copyright (C) 1988-1991 Apple Computer, Inc.
 * All Rights Reserved.
 *
 * Warranty Information
 * Even though Apple has reviewed this software, Apple makes no warranty
 * or representation, either express or implied, with respect to this
 * software, its quality, accuracy, merchantability, or fitness for a 
 * particular purpose.  As a result, this software is provided "as is,"
 * and you, its user, are assuming the entire risk as to its quality
 * and accuracy.
 *
 * This code may be used and freely distributed as long as it includes
 * this copyright notice and the warranty information.
 *
 *
 * Motorola processors (Macintosh, Sun, Sparc, MIPS, etc)
 * pack bytes from high to low (they are big-endian).
 * Use the HighLow routines to match the native format
 * of these machines.
 *
 * Intel-like machines (PCs, Sequent)
 * pack bytes from low to high (the are little-endian).
 * Use the LowHigh routines to match the native format
 * of these machines.
 *
 * These routines have been tested on the following machines:
 *	Apple Macintosh, MPW 3.1 C compiler
 *	Apple Macintosh, THINK C compiler
 *	Silicon Graphics IRIS, MIPS compiler
 *	Cray X/MP and Y/MP
 *	Digital Equipment VAX
 *
 *
 * Implemented by Malcolm Slaney and Ken Turkowski.
 *
 * Malcolm Slaney contributions during 1988-1990 include big- and little-
 * endian file I/O, conversion to and from Motorola's extended 80-bit
 * floating-point format, and conversions to and from IEEE single-
 * precision floating-point format.
 *
 * In 1991, Ken Turkowski implemented the conversions to and from
 * IEEE double-precision format, added more precision to the extended
 * conversions, and accommodated conversions involving +/- infinity,
 * NaN's, and denormalized numbers.
 *
 * $Id: portableio.c,v 1.1.1.1 1999/11/24 08:43:35 markt Exp $
 *
 * $Log: portableio.c,v $
 * Revision 1.1.1.1  1999/11/24 08:43:35  markt
 * initial checkin of LAME
 * Starting with LAME 3.57beta with some modifications
 *
 * Revision 2.6  91/04/30  17:06:02  malcolm
 */

#include	<stdio.h>
#include	<math.h>
#include	"portableio.h"

/****************************************************************
 * Big/little-endian independent I/O routines.
 ****************************************************************/


int
ReadByte(FILE *fp)
{
	int	result;

	result = getc(fp) & 0xff;
	if (result & 0x80)
		result = result - 0x100;
	return result;
}


int
Read16BitsLowHigh(FILE *fp)
{
	int	first, second, result;

	first = 0xff & getc(fp);
	second = 0xff & getc(fp);

	result = (second << 8) + first;
#ifndef	THINK_C42
	if (result & 0x8000)
		result = result - 0x10000;
#endif	/* THINK_C */
	return(result);
}


int
Read16BitsHighLow(FILE *fp)
{
	int	first, second, result;

	first = 0xff & getc(fp);
	second = 0xff & getc(fp);

	result = (first << 8) + second;
#ifndef	THINK_C42
	if (result & 0x8000)
		result = result - 0x10000;
#endif	/* THINK_C */
	return(result);
}


void
Write8Bits(FILE *fp, int i)
{
	putc(i&0xff,fp);
}


void
Write16BitsLowHigh(FILE *fp, int i)
{
	putc(i&0xff,fp);
	putc((i>>8)&0xff,fp);
}


void
Write16BitsHighLow(FILE *fp, int i)
{
	putc((i>>8)&0xff,fp);
	putc(i&0xff,fp);
}


int
Read24BitsHighLow(FILE *fp)
{
	int	first, second, third;
	int	result;

	first = 0xff & getc(fp);
	second = 0xff & getc(fp);
	third = 0xff & getc(fp);

	result = (first << 16) + (second << 8) + third;
	if (result & 0x800000)
		result = result - 0x1000000;
	return(result);
}

#define	Read32BitsLowHigh(f)	Read32Bits(f)


int
Read32Bits(FILE *fp)
{
	int	first, second, result;

	first = 0xffff & Read16BitsLowHigh(fp);
	second = 0xffff & Read16BitsLowHigh(fp);

	result = (second << 16) + first;
#ifdef	CRAY
	if (result & 0x80000000)
		result = result - 0x100000000;
#endif	/* CRAY */
	return(result);
}


int
Read32BitsHighLow(FILE *fp)
{
	int	first, second, result;

	first = 0xffff & Read16BitsHighLow(fp);
	second = 0xffff & Read16BitsHighLow(fp);

	result = (first << 16) + second;
#ifdef	CRAY
	if (result & 0x80000000)
		result = result - 0x100000000;
#endif
	return(result);
}


void
Write32Bits(FILE *fp, int i)
{
	Write16BitsLowHigh(fp,(int)(i&0xffffL));
	Write16BitsLowHigh(fp,(int)((i>>16)&0xffffL));
}


void
Write32BitsLowHigh(FILE *fp, int i)
{
	Write16BitsLowHigh(fp,(int)(i&0xffffL));
	Write16BitsLowHigh(fp,(int)((i>>16)&0xffffL));
}


void
Write32BitsHighLow(FILE *fp, int i)
{
	Write16BitsHighLow(fp,(int)((i>>16)&0xffffL));
	Write16BitsHighLow(fp,(int)(i&0xffffL));
}

void ReadBytes(FILE	*fp, char *p, int n)
{
	while (!feof(fp) & (n-- > 0))
		*p++ = getc(fp);
}

void ReadBytesSwapped(FILE *fp, char *p, int n)
{
	register char	*q = p;

	while (!feof(fp) & (n-- > 0))
		*q++ = getc(fp);

	for (q--; p < q; p++, q--){
		n = *p;
		*p = *q;
		*q = n;
	}
}

void WriteBytes(FILE *fp, char *p, int n)
{
	while (n-- > 0)
		putc(*p++, fp);
}

void WriteBytesSwapped(FILE *fp, char *p, int n)
{
	p += n-1;
	while (n-- > 0)
		putc(*p--, fp);
}

defdouble
ReadIeeeFloatHighLow(FILE *fp)
{
	char	bits[kFloatLength];

	ReadBytes(fp, bits, kFloatLength);
	return ConvertFromIeeeSingle(bits);
}

defdouble
ReadIeeeFloatLowHigh(FILE *fp)
{
	char	bits[kFloatLength];

	ReadBytesSwapped(fp, bits, kFloatLength);
	return ConvertFromIeeeSingle(bits);
}

defdouble
ReadIeeeDoubleHighLow(FILE *fp)
{
	char	bits[kDoubleLength];

	ReadBytes(fp, bits, kDoubleLength);
	return ConvertFromIeeeDouble(bits);
}

defdouble
ReadIeeeDoubleLowHigh(FILE *fp)
{
	char	bits[kDoubleLength];

	ReadBytesSwapped(fp, bits, kDoubleLength);
	return ConvertFromIeeeDouble(bits);
}

defdouble
ReadIeeeExtendedHighLow(FILE *fp)
{
	char	bits[kExtendedLength];

	ReadBytes(fp, bits, kExtendedLength);
	return ConvertFromIeeeExtended(bits);
}

defdouble
ReadIeeeExtendedLowHigh(FILE *fp)
{
	char	bits[kExtendedLength];

	ReadBytesSwapped(fp, bits, kExtendedLength);
	return ConvertFromIeeeExtended(bits);
}

void
WriteIeeeFloatLowHigh(FILE *fp, defdouble num)
{
	char	bits[kFloatLength];

	ConvertToIeeeSingle(num,bits);
	WriteBytesSwapped(fp,bits,kFloatLength);
}

void
WriteIeeeFloatHighLow(FILE *fp, defdouble num)
{
	char	bits[kFloatLength];

	ConvertToIeeeSingle(num,bits);
	WriteBytes(fp,bits,kFloatLength);
}

void
WriteIeeeDoubleLowHigh(FILE *fp, defdouble num)
{
	char	bits[kDoubleLength];

	ConvertToIeeeDouble(num,bits);
	WriteBytesSwapped(fp,bits,kDoubleLength);
}

void
WriteIeeeDoubleHighLow(FILE *fp, defdouble num)
{
	char	bits[kDoubleLength];

	ConvertToIeeeDouble(num,bits);
	WriteBytes(fp,bits,kDoubleLength);
}

void
WriteIeeeExtendedLowHigh(FILE *fp, defdouble num)
{
	char	bits[kExtendedLength];

	ConvertToIeeeExtended(num,bits);
	WriteBytesSwapped(fp,bits,kExtendedLength);
}


void
WriteIeeeExtendedHighLow(FILE *fp, defdouble num)
{
	char	bits[kExtendedLength];

	ConvertToIeeeExtended(num,bits);
	WriteBytes(fp,bits,kExtendedLength);
}




/* ==== psymodel.c ==== */
/**********************************************************************
 *   date   programmers         comment                               *
 * 2/25/91  Davis Pan           start of version 1.0 records          *
 * 5/10/91  W. Joseph Carter    Ported to Macintosh and Unix.         *
 * 7/10/91  Earle Jennings      Ported to MsDos.                      *
 *                              replace of floats with FLOAT          *
 * 2/11/92  W. Joseph Carter    Fixed mem_alloc() arg for "absthr".   *
 * 3/16/92  Masahiro Iwadare	Modification for Layer III            *
 * 17/4/93  Masahiro Iwadare    Updated for IS Modification           *
 **********************************************************************/

#include "util.h"
#include "encoder.h"
#include "psymodel.h"
#include "l3side.h"
#include <assert.h>
#ifdef HAVEGTK
#include "gtkanal.h"
#endif
#include "tables.h"
#include "fft.h"

#ifdef M_LN10
#define		LN_TO_LOG10		(M_LN10/10)
#else
#define         LN_TO_LOG10             0.2302585093
#endif


void L3para_read( FLOAT8 sfreq, int numlines[CBANDS],int numlines_s[CBANDS], int partition_l[HBLKSIZE],
		  FLOAT8 minval[CBANDS], FLOAT8 qthr_l[CBANDS], 
		  FLOAT8 s3_l[CBANDS + 1][CBANDS + 1],
		  FLOAT8 s3_s[CBANDS + 1][CBANDS + 1],
                  FLOAT8 qthr_s[CBANDS],
		  FLOAT8 SNR_s[CBANDS],
		  int bu_l[SBPSY_l], int bo_l[SBPSY_l],
		  FLOAT8 w1_l[SBPSY_l], FLOAT8 w2_l[SBPSY_l],
		  int bu_s[SBPSY_s], int bo_s[SBPSY_s],
		  FLOAT8 w1_s[SBPSY_s], FLOAT8 w2_s[SBPSY_s] );
									







 

void L3psycho_anal( lame_global_flags *gfp,
                    short int *buffer[2],int gr_out , 
                    FLOAT8 *ms_ratio,
                    FLOAT8 *ms_ratio_next,
		    FLOAT8 *ms_ener_ratio,
		    III_psy_ratio masking_ratio[2][2],
		    III_psy_ratio masking_MS_ratio[2][2],
		    FLOAT8 percep_entropy[2],FLOAT8 percep_MS_entropy[2], 
                    int blocktype_d[2])
{

/* to get a good cache performance, one has to think about
 * the sequence, in which the variables are used
 */
  
/* The static variables "r", "phi_sav", "new", "old" and "oldest" have    */
/* to be remembered for the unpredictability measure.  For "r" and        */
/* "phi_sav", the first index from the left is the channel select and     */
/* the second index is the "age" of the data.                             */
  static FLOAT8	minval[CBANDS],qthr_l[CBANDS];
  static FLOAT8	qthr_s[CBANDS];
  static FLOAT8	nb_1[4][CBANDS], nb_2[4][CBANDS];
  static FLOAT8 s3_s[CBANDS + 1][CBANDS + 1];
  static FLOAT8 s3_l[CBANDS + 1][CBANDS + 1];

  static III_psy_xmin thm[4];
  static III_psy_xmin en[4];
  
  /* unpredictability calculation
   */
  static int cw_upper_index;
  static int cw_lower_index;
  static FLOAT ax_sav[4][2][HBLKSIZE];
  static FLOAT bx_sav[4][2][HBLKSIZE];
  static FLOAT rx_sav[4][2][HBLKSIZE];
  static FLOAT cw[HBLKSIZE];

  /* fft and energy calculation
   */
  FLOAT (*wsamp_l)[BLKSIZE];
  FLOAT (*wsamp_s)[3][BLKSIZE_s];
  FLOAT tot_ener[4];
  static FLOAT wsamp_L[2][BLKSIZE];
  static FLOAT energy[HBLKSIZE];
  static FLOAT wsamp_S[2][3][BLKSIZE_s];
  static FLOAT energy_s[3][HBLKSIZE_s];

  /* convolution
   */
  static FLOAT8 eb[CBANDS];
  static FLOAT8 cb[CBANDS];
  static FLOAT8 thr[CBANDS];
  
  /* Scale Factor Bands
   */
  static FLOAT8	w1_l[SBPSY_l], w2_l[SBPSY_l];
  static FLOAT8	w1_s[SBPSY_s], w2_s[SBPSY_s];
  static FLOAT8 mld_l[SBPSY_l],mld_s[SBPSY_s];
  static int	bu_l[SBPSY_l],bo_l[SBPSY_l] ;
  static int	bu_s[SBPSY_s],bo_s[SBPSY_s] ;
  static int	npart_l,npart_s;
  static int	npart_l_orig,npart_s_orig;
  
  static int	s3ind[CBANDS][2];
  static int	s3ind_s[CBANDS][2];

  static int	numlines_s[CBANDS] ;
  static int	numlines_l[CBANDS];
  static int	partition_l[HBLKSIZE];
  
  /* frame analyzer 
   */
#ifdef HAVEGTK
  static FLOAT energy_save[4][HBLKSIZE];
  static FLOAT8 pe_save[4];
  static FLOAT8 ers_save[4];
#endif

  /* ratios 
   */
  static FLOAT8 pe[4]={0,0,0,0};
  static FLOAT8 ms_ratio_s_old=0,ms_ratio_l_old=0;
  static FLOAT8 ms_ener_ratio_old=.25;
  FLOAT8 ms_ratio_l=0,ms_ratio_s=0;

  /* block type 
   */
  static int	blocktype_old[2];
  int blocktype[2],uselongblock[2];
  
  /* usual variables like loop indices, etc..
   */
  int numchn, chn;
  int   b, i, j, k;
  int	sb,sblock;
  FLOAT cwlimit;


  /* initialization of static variables
   */
  if((gfp->frameNum==0) && (gr_out==0)){
    FLOAT8	SNR_s[CBANDS];
    
    blocktype_old[0]=STOP_TYPE;
    blocktype_old[1]=STOP_TYPE;
    i = gfp->out_samplerate;
    switch(i){
    case 32000: break;
    case 44100: break;
    case 48000: break;
    case 16000: break;
    case 22050: break;
    case 24000: break;
    default:    fprintf(stderr,"error, invalid sampling frequency: %d Hz\n",i);
      exit(-1);
    }
    
    /* reset states used in unpredictability measure */
    memset (rx_sav,0, sizeof(rx_sav));
    memset (ax_sav,0, sizeof(ax_sav));
    memset (bx_sav,0, sizeof(bx_sav));
    memset (en,0, sizeof(en));
    memset (thm,0, sizeof(thm));
    

    /*  gfp->cwlimit = sfreq*j/1024.0;  */
    cw_lower_index=6;
    if (gfp->cwlimit>0) 
      cwlimit=gfp->cwlimit;
    else
      cwlimit=8.8717;
    cw_upper_index = cwlimit*1000.0*1024.0/((FLOAT8) gfp->out_samplerate);
    cw_upper_index=Min(HBLKSIZE-4,cw_upper_index);      /* j+3 < HBLKSIZE-1 */
    cw_upper_index=Max(6,cw_upper_index);

    for ( j = 0; j < HBLKSIZE; j++ )
      cw[j] = 0.4;
    
    /* setup stereo demasking thresholds */
    /* formula reverse enginerred from plot in paper */
    for ( sb = 0; sb < SBPSY_s; sb++ ) {
      FLOAT8 mld = 1.25*(1-cos(PI*sb/SBPSY_s))-2.5;
      mld_s[sb] = pow(10.0,mld);
    }
    for ( sb = 0; sb < SBPSY_l; sb++ ) {
      FLOAT8 mld = 1.25*(1-cos(PI*sb/SBPSY_l))-2.5;
      mld_l[sb] = pow(10.0,mld);
    }
    
    for (i=0;i<HBLKSIZE;i++) partition_l[i]=-1;

    L3para_read( (FLOAT8) gfp->out_samplerate,numlines_l,numlines_s,partition_l,minval,qthr_l,s3_l,s3_s,
		 qthr_s,SNR_s,
		 bu_l,bo_l,w1_l,w2_l, bu_s,bo_s,w1_s,w2_s );
    
    
    /* npart_l_orig   = number of partition bands before convolution */
    /* npart_l  = number of partition bands after convolution */
    npart_l_orig=0; npart_s_orig=0;
    for (i=0;i<HBLKSIZE;i++) 
      if (partition_l[i]>npart_l_orig) npart_l_orig=partition_l[i];
    npart_l_orig++;

    for (i=0;numlines_s[i]>=0;i++)
      ;
    npart_s_orig = i;
    
    npart_l=bo_l[SBPSY_l-1]+1;
    npart_s=bo_s[SBPSY_s-1]+1;

    /* MPEG2 tables are screwed up 
     * the mapping from paritition bands to scalefactor bands will use
     * more paritition bands than we have.  
     * So we will not compute these fictitious partition bands by reducing
     * npart_l below.  */
    if (npart_l > npart_l_orig) {
      npart_l=npart_l_orig;
      bo_l[SBPSY_l-1]=npart_l-1;
      w2_l[SBPSY_l-1]=1.0;
    }
    if (npart_s > npart_s_orig) {
      npart_s=npart_s_orig;
      bo_s[SBPSY_s-1]=npart_s-1;
      w2_s[SBPSY_s-1]=1.0;
    }
    
    
    
    for (i=0; i<npart_l; i++) {
      for (j = 0; j < npart_l_orig; j++) {
	if (s3_l[i][j] != 0.0)
	  break;
      }
      s3ind[i][0] = j;
      
      for (j = npart_l_orig - 1; j > 0; j--) {
	if (s3_l[i][j] != 0.0)
	  break;
      }
      s3ind[i][1] = j;
    }


    for (i=0; i<npart_s; i++) {
      for (j = 0; j < npart_s_orig; j++) {
	if (s3_s[i][j] != 0.0)
	  break;
      }
      s3ind_s[i][0] = j;
      
      for (j = npart_s_orig - 1; j > 0; j--) {
	if (s3_s[i][j] != 0.0)
	  break;
      }
      s3ind_s[i][1] = j;
    }
    
    
    /*  
      #include "debugscalefac.c"
    */
    

#define AACS3
#define NEWS3XX

#ifdef AACS3
    /* AAC values, results in more masking over MP3 values */
# define TMN 18
# define NMT 6
#else
    /* MP3 values */
# define TMN 29
# define NMT 6
#endif

#define rpelev 2
#define rpelev2 16

    /* compute norm_l, norm_s instead of relying on table data */
    for ( b = 0;b < npart_l; b++ ) {
      FLOAT8 norm=0;
      for ( k = s3ind[b][0]; k <= s3ind[b][1]; k++ ) {
	norm += s3_l[b][k];
      }
      for ( k = s3ind[b][0]; k <= s3ind[b][1]; k++ ) {
	s3_l[b][k] *= exp(-LN_TO_LOG10 * NMT) / norm;
      }
      /*printf("%i  norm=%f  norm_l=%f \n",b,1/norm,norm_l[b]);*/
    }

    /* MPEG1 SNR_s data is given in db, convert to energy */
    if (gfp->version == 1) {
      for ( b = 0;b < npart_s; b++ ) {
	SNR_s[b]=exp( (FLOAT8) SNR_s[b] * LN_TO_LOG10 );
      }
    }

    for ( b = 0;b < npart_s; b++ ) {
      FLOAT8 norm=0;
      for ( k = s3ind_s[b][0]; k <= s3ind_s[b][1]; k++ ) {
	norm += s3_s[b][k];
      }
      for ( k = s3ind_s[b][0]; k <= s3ind_s[b][1]; k++ ) {
	s3_s[b][k] *= SNR_s[b] / norm;
      }
      /*printf("%i  norm=%f  norm_s=%f \n",b,1/norm,norm_l[b]);*/
    }
    
    init_fft();
  }
  /************************* End of Initialization *****************************/
  


  
  
  numchn = gfp->stereo;
  /* chn=2 and 3 = Mid and Side channels */
  if (gfp->mode == MPG_MD_JOINT_STEREO) numchn=4;
  for (chn=0; chn<numchn; chn++) {
  
    wsamp_s = wsamp_S+(chn & 1);
    wsamp_l = wsamp_L+(chn & 1);


    if (chn<2) {    
      /**********************************************************************
       *  compute FFTs
       **********************************************************************/
      fft_long ( *wsamp_l, chn, buffer);
      fft_short( *wsamp_s, chn, buffer); 
      
      /* LR maskings  */
      percep_entropy[chn] = pe[chn]; 
      masking_ratio[gr_out][chn].thm = thm[chn];
      masking_ratio[gr_out][chn].en = en[chn];
    }else{
      /* MS maskings  */
      percep_MS_entropy[chn-2] = pe[chn]; 
      masking_MS_ratio[gr_out][chn-2].en = en[chn];
      masking_MS_ratio[gr_out][chn-2].thm = thm[chn];
      
      if (chn == 2)
      {
        for (j = BLKSIZE-1; j >=0 ; --j)
        {
          FLOAT l = wsamp_L[0][j];
          FLOAT r = wsamp_L[1][j];
          wsamp_L[0][j] = (l+r)*(FLOAT)(SQRT2*0.5);
          wsamp_L[1][j] = (l-r)*(FLOAT)(SQRT2*0.5);
        }
        for (b = 2; b >= 0; --b)
        {
          for (j = BLKSIZE_s-1; j >= 0 ; --j)
          {
            FLOAT l = wsamp_S[0][b][j];
            FLOAT r = wsamp_S[1][b][j];
            wsamp_S[0][b][j] = (l+r)*(FLOAT)(SQRT2*0.5);
            wsamp_S[1][b][j] = (l-r)*(FLOAT)(SQRT2*0.5);
          }
        }
      }
    }

    /**********************************************************************
     *  compute energies
     **********************************************************************/
    
    
    
    energy[0]  = (*wsamp_l)[0];
    energy[0] *= energy[0];
    
    tot_ener[chn] = energy[0]; /* sum total energy at nearly no extra cost */
    
    for (j=BLKSIZE/2-1; j >= 0; --j)
    {
      FLOAT re = (*wsamp_l)[BLKSIZE/2-j];
      FLOAT im = (*wsamp_l)[BLKSIZE/2+j];
      energy[BLKSIZE/2-j] = (re * re + im * im) * (FLOAT)0.5;
      
      tot_ener[chn] += energy[BLKSIZE/2-j];
    }
    for (b = 2; b >= 0; --b)
    {
      energy_s[b][0]  = (*wsamp_s)[b][0];
      energy_s[b][0] *=  energy_s [b][0];
      for (j=BLKSIZE_s/2-1; j >= 0; --j)
      {
        FLOAT re = (*wsamp_s)[b][BLKSIZE_s/2-j];
        FLOAT im = (*wsamp_s)[b][BLKSIZE_s/2+j];
        energy_s[b][BLKSIZE_s/2-j] = (re * re + im * im) * (FLOAT)0.5;
      }
    }


#ifdef HAVEGTK
  if(gfp->gtkflag) {
    for (j=0; j<HBLKSIZE ; j++) {
      pinfo->energy[gr_out][chn][j]=energy_save[chn][j];
      energy_save[chn][j]=energy[j];
    }
  }
#endif
    
    /**********************************************************************
     *    compute unpredicatability of first six spectral lines            * 
     **********************************************************************/
    for ( j = 0; j < cw_lower_index; j++ )
      {	 /* calculate unpredictability measure cw */
	FLOAT an, a1, a2;
	FLOAT bn, b1, b2;
	FLOAT rn, r1, r2;
	FLOAT numre, numim, den;

	a2 = ax_sav[chn][1][j];
	b2 = bx_sav[chn][1][j];
	r2 = rx_sav[chn][1][j];
	a1 = ax_sav[chn][1][j] = ax_sav[chn][0][j];
	b1 = bx_sav[chn][1][j] = bx_sav[chn][0][j];
	r1 = rx_sav[chn][1][j] = rx_sav[chn][0][j];
	an = ax_sav[chn][0][j] = (*wsamp_l)[j];
	bn = bx_sav[chn][0][j] = j==0 ? (*wsamp_l)[0] : (*wsamp_l)[BLKSIZE-j];  
	rn = rx_sav[chn][0][j] = sqrt(energy[j]);

	{ /* square (x1,y1) */
	  if( r1 != 0 ) {
	    numre = (a1*b1);
	    numim = (a1*a1-b1*b1)*(FLOAT)0.5;
	    den = r1*r1;
	  } else {
	    numre = 1;
	    numim = 0;
	    den = 1;
	  }
	}
	
	{ /* multiply by (x2,-y2) */
	  if( r2 != 0 ) {
	    FLOAT tmp2 = (numim+numre)*(a2+b2)*(FLOAT)0.5;
	    FLOAT tmp1 = -a2*numre+tmp2;
	    numre =       -b2*numim+tmp2;
	    numim = tmp1;
	    den *= r2;
	  } else {
	    /* do nothing */
	  }
	}
	
	{ /* r-prime factor */
	  FLOAT tmp = (2*r1-r2)/den;
	  numre *= tmp;
	  numim *= tmp;
	}
	den=rn+fabs(2*r1-r2);
	if( den != 0 ) {
	  numre = (an+bn)*(FLOAT)0.5-numre;
	  numim = (an-bn)*(FLOAT)0.5-numim;
	  den = sqrt(numre*numre+numim*numim)/den;
	}
	cw[j] = den;
      }



    /**********************************************************************
     *     compute unpredicatibility of next 200 spectral lines            *
     **********************************************************************/ 
    for ( j = cw_lower_index; j < cw_upper_index; j += 4 )
      {/* calculate unpredictability measure cw */
	FLOAT rn, r1, r2;
	FLOAT numre, numim, den;
	
	k = (j+2) / 4; 
	
	{ /* square (x1,y1) */
	  r1 = energy_s[0][k];
	  if( r1 != 0 ) {
	    FLOAT a1 = (*wsamp_s)[0][k]; 
	    FLOAT b1 = (*wsamp_s)[0][BLKSIZE_s-k]; /* k is never 0 */
	    numre = (a1*b1);
	    numim = (a1*a1-b1*b1)*(FLOAT)0.5;
	    den = r1;
	    r1 = sqrt(r1);
	  } else {
	    numre = 1;
	    numim = 0;
	    den = 1;
	  }
	}
	
	
	{ /* multiply by (x2,-y2) */
	  r2 = energy_s[2][k];
	  if( r2 != 0 ) {
	    FLOAT a2 = (*wsamp_s)[2][k]; 
	    FLOAT b2 = (*wsamp_s)[2][BLKSIZE_s-k];
	    
	    
	    FLOAT tmp2 = (numim+numre)*(a2+b2)*(FLOAT)0.5;
	    FLOAT tmp1 = -a2*numre+tmp2;
	    numre =       -b2*numim+tmp2;
	    numim = tmp1;
	    
	    r2 = sqrt(r2);
	    den *= r2;
	  } else {
	    /* do nothing */
	  }
	}
	
	{ /* r-prime factor */
	  FLOAT tmp = (2*r1-r2)/den;
	  numre *= tmp;
	  numim *= tmp;
	}
	
	rn = sqrt(energy_s[1][k]);
	den=rn+fabs(2*r1-r2);
	if( den != 0 ) {
	  FLOAT an = (*wsamp_s)[1][k]; 
	  FLOAT bn = (*wsamp_s)[1][BLKSIZE_s-k];
	  numre = (an+bn)*(FLOAT)0.5-numre;
	  numim = (an-bn)*(FLOAT)0.5-numim;
	  den = sqrt(numre*numre+numim*numim)/den;
	}
	
	cw[j+1] = cw[j+2] = cw[j+3] = cw[j] = den;
      }
    
#if 0
    for ( j = 14; j < HBLKSIZE-4; j += 4 )
      {/* calculate energy from short ffts */
	FLOAT8 tot,ave;
	k = (j+2) / 4; 
	for (tot=0, sblock=0; sblock < 3; sblock++)
	  tot+=energy_s[sblock][k];
	ave = energy[j+1]+ energy[j+2]+ energy[j+3]+ energy[j];
	ave /= 4.;
	/*
	  printf("energy / tot %i %5.2f   %e  %e\n",j,ave/(tot*16./3.),
	  ave,tot*16./3.);
	*/
	energy[j+1] = energy[j+2] = energy[j+3] =  energy[j]=tot;
      }
#endif
    
    
    
    
    
    
    
    
    /**********************************************************************
     *    Calculate the energy and the unpredictability in the threshold   *
     *    calculation partitions                                           *
     **********************************************************************/
#if 0
    for ( b = 0; b < CBANDS; b++ )
      {
	eb[b] = 0;
	cb[b] = 0;
      }
    for ( j = 0; j < HBLKSIZE; j++ )
      {
	int tp = partition_l[j];
	
	if ( tp >= 0 )
	  {
	    eb[tp] += energy[j];
	    cb[tp] += cw[j] * energy[j];
	  }
	assert(tp<npart_l_orig);
      }
#else
    b = 0;
    for (j = 0; j < cw_upper_index;)
      {
	FLOAT8 ebb, cbb;
	int i;

	ebb = energy[j];
	cbb = energy[j] * cw[j];
	j++;

	for (i = numlines_l[b] - 1; i > 0; i--)
	  {
	    ebb += energy[j];
	    cbb += energy[j] * cw[j];
	    j++;
	  }
	eb[b] = ebb;
	cb[b] = cbb;
	b++;
      }

    for (; b < npart_l_orig; b++ )
      {
	int i;
	FLOAT8 ebb = energy[j++];

	for (i = numlines_l[b] - 1; i > 0; i--)
	  {
	    ebb += energy[j++];
	  }
	eb[b] = ebb;
	cb[b] = ebb * 0.4;
      }
#endif

    /**********************************************************************
     *      convolve the partitioned energy and unpredictability           *
     *      with the spreading function, s3_l[b][k]                        *
     ******************************************************************** */
    pe[chn] = 0;		/*  calculate percetual entropy */
    for ( b = 0;b < npart_l; b++ )
      {
	FLOAT8 tbb,ecb,ctb;
	FLOAT8 temp_1; /* BUG of IS */

	ecb = 0;
	ctb = 0;
	for ( k = s3ind[b][0]; k <= s3ind[b][1]; k++ )
	  {
	    ecb += s3_l[b][k] * eb[k];	/* sprdngf for Layer III */
	    ctb += s3_l[b][k] * cb[k];
	  }

	/* calculate the tonality of each threshold calculation partition */
	/* calculate the SNR in each threshhold calculation partition */

	tbb = ecb;
	if (tbb != 0)
	  {
	    tbb = ctb / tbb;
	    if (tbb <= 0.04875584301)
	      {
		tbb = exp(-LN_TO_LOG10 * (TMN - NMT));
	      }
	    else if (tbb > 0.4989003827)
	      {
		tbb = 1;
	      }
	    else
	      {
		tbb = log(tbb);
		tbb = exp(((TMN - NMT)*(LN_TO_LOG10*0.299))
			+ ((TMN - NMT)*(LN_TO_LOG10*0.43 ))*tbb);  /* conv1=-0.299, conv2=-0.43 */
	      }
	  }

	tbb = Min(minval[b], tbb);
	ecb *= tbb;

	/* pre-echo control */
	/* rpelev=2.0, rpelev2=16.0 */
	temp_1 = Min(ecb, Min(rpelev*nb_1[chn][b],rpelev2*nb_2[chn][b]) );
	thr[b] = Max( qthr_l[b], temp_1 ); 
	nb_2[chn][b] = nb_1[chn][b];
	nb_1[chn][b] = ecb;

	/* note: all surges in PE are because of the above pre-echo formula
	 * for temp_1.  it this is not used, PE is always around 600
	 */

	if (thr[b] < eb[b])
	  {
	    /* there's no non sound portition, because thr[b] is
	     maximum of qthr_l and temp_1 */
	    pe[chn] -= numlines_l[b] * log(thr[b] / eb[b]);
	  }
      }


#ifdef HAVEGTK
    if (gfp->gtkflag) {
      FLOAT mn,mx,ma=0,mb=0,mc=0;

      for ( j = HBLKSIZE_s/2; j < HBLKSIZE_s; j ++)
      {
        ma += energy_s[0][j];
        mb += energy_s[1][j];
        mc += energy_s[2][j];
      }
      mn = Min(ma,mb);
      mn = Min(mn,mc);
      mx = Max(ma,mb);
      mx = Max(mx,mc);

      pinfo->ers[gr_out][chn]=ers_save[chn];
      ers_save[chn]=mx/(1e-12+mn);
      pinfo->pe[gr_out][chn]=pe_save[chn];
      pe_save[chn]=pe[chn];
    }
#endif
    
    /*************************************************************** 
     * determine the block type (window type) based on L & R channels
     * 
     ***************************************************************/
    if (chn<2) {
      if (gfp->no_short_blocks){
	uselongblock[chn]=1;
      } else {
	/* tuned for t1.wav.  doesnt effect most other samples */
	if (pe[chn] > 3000) {
	  uselongblock[chn]=0;
	} else { 
	  FLOAT mn,mx,ma=0,mb=0,mc=0;
	
	  for ( j = HBLKSIZE_s/2; j < HBLKSIZE_s; j ++)
	  {
	      ma += energy_s[0][j];
	      mb += energy_s[1][j];
	      mc += energy_s[2][j];
	  }
	  mn = Min(ma,mb);
	  mn = Min(mn,mc);
	  mx = Max(ma,mb);
	  mx = Max(mx,mc);

	  uselongblock[chn] = 1;
	  
	  if ( mx > 30*mn ) 
	  {/* big surge of energy - always use short blocks */
	    uselongblock[chn] = 0;
	  } 
	  else if ((mx > 10*mn) && (pe[chn] > 1000))
	  {/* medium surge, medium pe - use short blocks */
	    uselongblock[chn] = 0;
	  }
	} 
      }
    }



    /*************************************************************** 
     * compute masking thresholds for both short and long blocks
     ***************************************************************/
    /* longblock threshold calculation (part 2) */
    for ( sb = 0; sb < SBPSY_l; sb++ )
      {
	FLOAT8 enn = w1_l[sb] * eb[bu_l[sb]] + w2_l[sb] * eb[bo_l[sb]];
	FLOAT8 thmm = w1_l[sb] *thr[bu_l[sb]] + w2_l[sb] * thr[bo_l[sb]];
	for ( b = bu_l[sb]+1; b < bo_l[sb]; b++ )
	  {
	    enn  += eb[b];
	    thmm += thr[b];
	  }
	en[chn].l[sb] = enn;
	thm[chn].l[sb] = thmm;
      }
    
    
    /* threshold calculation for short blocks */
    for ( sblock = 0; sblock < 3; sblock++ )
      {
	j = 0;
	for ( b = 0; b < npart_s_orig; b++ )
	  {
	    int i;
	    FLOAT ecb = energy_s[sblock][j++];
	    for (i = numlines_s[b]; i > 0; i--)
	      {
		ecb += energy_s[sblock][j++];
	      }
	    eb[b] = ecb;
	  }

	for ( b = 0; b < npart_s; b++ )
	  {
	    FLOAT8 ecb = 0;
	    for ( k = s3ind_s[b][0]; k <= s3ind_s[b][1]; k++ )
	      {
		ecb += s3_s[b][k] * eb[k];
	      }
	    thr[b] = Max (qthr_s[b], ecb);
	  }

	for ( sb = 0; sb < SBPSY_s; sb++ )
	  {
	    FLOAT8 enn  = w1_s[sb] * eb[bu_s[sb]] + w2_s[sb] * eb[bo_s[sb]];
	    FLOAT8 thmm = w1_s[sb] *thr[bu_s[sb]] + w2_s[sb] * thr[bo_s[sb]];
	    for ( b = bu_s[sb]+1; b < bo_s[sb]; b++ )
	      {
		enn  += eb[b];
		thmm += thr[b];
	      }
	    en[chn].s[sb][sblock] = enn;
	    thm[chn].s[sb][sblock] = thmm;
	  }
      }
  } /* end loop over chn */


  /* compute M/S thresholds from Johnston & Ferreira 1992 ICASSP paper */
  if ( numchn==4 /* mid/side and r/l */) {
    FLOAT8 rside,rmid,mld;
    int chmid=2,chside=3; 
    
    for ( sb = 0; sb < SBPSY_l; sb++ ) {
      /* use this fix if L & R masking differs by 2db or less */
      /* if db = 10*log10(x2/x1) < 2 */
      /* if (x2 < 1.58*x1) { */
      if (thm[0].l[sb] <= 1.58*thm[1].l[sb]
	  && thm[1].l[sb] <= 1.58*thm[0].l[sb]) {

	mld = mld_l[sb]*en[chside].l[sb];
	rmid = Max(thm[chmid].l[sb], Min(thm[chside].l[sb],mld));

	mld = mld_l[sb]*en[chmid].l[sb];
	rside = Max(thm[chside].l[sb],Min(thm[chmid].l[sb],mld));

	thm[chmid].l[sb]=rmid;
	thm[chside].l[sb]=rside;
      }
    }
    for ( sb = 0; sb < SBPSY_s; sb++ ) {
      for ( sblock = 0; sblock < 3; sblock++ ) {
	if (thm[0].s[sb][sblock] <= 1.58*thm[1].s[sb][sblock]
	    && thm[1].s[sb][sblock] <= 1.58*thm[0].s[sb][sblock]) {

	  mld = mld_s[sb]*en[chside].s[sb][sblock];
	  rmid = Max(thm[chmid].s[sb][sblock],Min(thm[chside].s[sb][sblock],mld));

	  mld = mld_s[sb]*en[chmid].s[sb][sblock];
	  rside = Max(thm[chside].s[sb][sblock],Min(thm[chmid].s[sb][sblock],mld));

	  thm[chmid].s[sb][sblock]=rmid;
	  thm[chside].s[sb][sblock]=rside;
	}
      }
    }
  }


  

  
  
  if (gfp->mode == MPG_MD_JOINT_STEREO)  {
    /* determin ms_ratio from masking thresholds*/
    /* use ms_stereo (ms_ratio < .35) if average thresh. diff < 5 db */
    FLOAT8 db,x1,x2,sidetot=0,tot=0;
    for (sb= SBPSY_l/4 ; sb< SBPSY_l; sb ++ ) {
      x1 = Min(thm[0].l[sb],thm[1].l[sb]);
      x2 = Max(thm[0].l[sb],thm[1].l[sb]);
      /* thresholds difference in db */
      if (x2 >= 1000*x1)  db=3;
      else db = log10(x2/x1);  
      /*  printf("db = %f %e %e  \n",db,thm[0].l[sb],thm[1].l[sb]);*/
      sidetot += db;
      tot++;
    }
    ms_ratio_l= (sidetot/tot)*0.7; /* was .35*(sidetot/tot)/5.0*10 */
    ms_ratio_l = Min(ms_ratio_l,0.5);
    
    sidetot=0; tot=0;
    for ( sblock = 0; sblock < 3; sblock++ )
      for ( sb = SBPSY_s/4; sb < SBPSY_s; sb++ ) {
	x1 = Min(thm[0].s[sb][sblock],thm[1].s[sb][sblock]);
	x2 = Max(thm[0].s[sb][sblock],thm[1].s[sb][sblock]);
	/* thresholds difference in db */
	if (x2 >= 1000*x1)  db=3;
	else db = log10(x2/x1);  
	sidetot += db;
	tot++;
      }
    ms_ratio_s = (sidetot/tot)*0.7; /* was .35*(sidetot/tot)/5.0*10 */
    ms_ratio_s = Min(ms_ratio_s,.5);
  }

  /*************************************************************** 
   * determin final block type
   ***************************************************************/

  for (chn=0; chn<gfp->stereo; chn++) {
    blocktype[chn] = NORM_TYPE;
  }


  if (gfp->stereo==2) {
    if (!gfp->allow_diff_short || gfp->mode==MPG_MD_JOINT_STEREO) {
      /* force both channels to use the same block type */
      /* this is necessary if the frame is to be encoded in ms_stereo.  */
      /* But even without ms_stereo, FhG  does this */
      int bothlong= (uselongblock[0] && uselongblock[1]);
      if (!bothlong) {
	uselongblock[0]=0;
	uselongblock[1]=0;
      }
    }
  }

  
  
  /* update the blocktype of the previous granule, since it depends on what
   * happend in this granule */
  for (chn=0; chn<gfp->stereo; chn++) {
    if ( uselongblock[chn])
      {				/* no attack : use long blocks */
	switch( blocktype_old[chn] ) 
	  {
	  case NORM_TYPE:
	  case STOP_TYPE:
	    blocktype[chn] = NORM_TYPE;
	    break;
	  case SHORT_TYPE:
	    blocktype[chn] = STOP_TYPE; 
	    break;
	  case START_TYPE:
	    fprintf( stderr, "Error in block selecting\n" );
	    abort();
	    break; /* problem */
	  }
      } else   {
	/* attack : use short blocks */
	blocktype[chn] = SHORT_TYPE;
	if ( blocktype_old[chn] == NORM_TYPE ) {
	  blocktype_old[chn] = START_TYPE;
	}
	if ( blocktype_old[chn] == STOP_TYPE ) {
	  blocktype_old[chn] = SHORT_TYPE ;
	}
      }
    
    blocktype_d[chn] = blocktype_old[chn];  /* value returned to calling program */
    blocktype_old[chn] = blocktype[chn];    /* save for next call to l3psy_anal */
  }
  
  if (blocktype_d[0]==2) 
    *ms_ratio = ms_ratio_s_old;
  else
    *ms_ratio = ms_ratio_l_old;

  ms_ratio_s_old = ms_ratio_s;
  ms_ratio_l_old = ms_ratio_l;

  /* we dont know the block type of this frame yet - assume long */
  *ms_ratio_next = ms_ratio_l;



  /*********************************************************************/
  /* compute side_energy / (side+mid)_energy */
  /* 0 = no energy in side channel */
  /* .5 = half of total energy in side channel */
  /*********************************************************************/
  if (numchn==4)  {
    FLOAT tmp = tot_ener[3]+tot_ener[2];
    *ms_ener_ratio = ms_ener_ratio_old;
    ms_ener_ratio_old=0;
    if (tmp>0) ms_ener_ratio_old=tot_ener[3]/tmp;
  } else
    /* we didn't compute ms_ener_ratios */
    *ms_ener_ratio = 0;
 
}






void L3para_read(FLOAT8 sfreq, int *numlines_l,int *numlines_s, int *partition_l, FLOAT8 *minval,
FLOAT8 *qthr_l, FLOAT8 s3_l[64][64], FLOAT8 s3_s[CBANDS + 1][CBANDS + 1],
FLOAT8 *qthr_s, FLOAT8 *SNR, 
int *bu_l, int *bo_l, FLOAT8 *w1_l, FLOAT8 *w2_l, 
int *bu_s, int *bo_s, FLOAT8 *w1_s, FLOAT8 *w2_s)
{
  FLOAT8 freq_tp;
  FLOAT8 bval_l[CBANDS], bval_s[CBANDS];
  int   cbmax=0, cbmax_tp;
  FLOAT8 *p = psy_data;

  int  sbmax ;
  int  i,j,k,k2,loop, part_max ;
  int freq_scale=1;


  /* use MPEG1 tables.  The MPEG2 tables in tables.c appear to be 
   * junk.  MPEG2 doc claims data for these tables is the same as the
   * MPEG1 data for 2x sampling frequency */
  /*  if (sfreq<32000) freq_scale=2; */
  


  /* Read long block data */

  for(loop=0;loop<6;loop++)
    {
      freq_tp = *p++;
      cbmax_tp = (int) *p++;
      cbmax_tp++;

      if (sfreq == freq_tp/freq_scale )
	{
	  cbmax = cbmax_tp;
	  for(i=0,k2=0;i<cbmax_tp;i++)
	    {
	      j = (int) *p++;
	      numlines_l[i] = (int) *p++;
	      minval[i] = exp(-((*p++) - NMT) * LN_TO_LOG10);
	      qthr_l[i] = *p++;
	      /* norm_l[i] = *p++*/ p++;
	      bval_l[i] = *p++;
	      if (j!=i)
		{
		  fprintf(stderr,"1. please check \"psy_data\"");
		  exit(-1);
		}
	      for(k=0;k<numlines_l[i];k++)
		partition_l[k2++] = i ;
	    }
	}
      else
	p += cbmax_tp * 6;
    }

#define NEWBARKXXX
#ifdef NEWBARK
  /* compute bark values of each critical band */
  j = 0;
  for(i=0;i<cbmax;i++)
    {
      FLOAT8 ji, freq, bark;

      ji = j + (numlines_l[i]-1)/2.0;
      freq = sfreq*ji/1024000.0;
      bark = 13*atan(.76*freq) + 3.5*atan(freq*freq/(7.5*7.5));

      printf("%i %i bval_l table=%f  f=%f  formaula=%f \n",i,j,bval_l[i],freq,bark);
      bval_l[i]=bark;
      j += numlines_l[i];
    }
#endif

  /************************************************************************
   * Now compute the spreading function, s[j][i], the value of the spread-*
   * ing function, centered at band j, for band i, store for later use    *
   ************************************************************************/
  /* i.e.: sum over j to spread into signal barkval=i  
     NOTE: i and j are used opposite as in the ISO docs */
  part_max = cbmax ;
  for(i=0;i<part_max;i++)
    {
      FLOAT8 tempx,x,tempy,temp;
      for(j=0;j<part_max;j++)
	{
	  /*tempx = (bval_l[i] - bval_l[j])*1.05;*/
	  if (j>=i) tempx = (bval_l[i] - bval_l[j])*3.0;
	  else    tempx = (bval_l[i] - bval_l[j])*1.5;

#ifdef AACS3	
          if (i>=j) tempx = (bval_l[i] - bval_l[j])*3.0;
	  else    tempx = (bval_l[i] - bval_l[j])*1.5; 
#endif

	  if(tempx>=0.5 && tempx<=2.5)
	    {
	      temp = tempx - 0.5;
	      x = 8.0 * (temp*temp - 2.0 * temp);
	    }
	  else x = 0.0;
	  tempx += 0.474;
	  tempy = 15.811389 + 7.5*tempx - 17.5*sqrt(1.0+tempx*tempx);

#ifdef NEWS3
	  if (j>=i) tempy = (bval_l[j] - bval_l[i])*(-15);
	  else    tempy = (bval_l[j] - bval_l[i])*25;
	  x=0; 
#endif
	  /*
	  if ((i==part_max/2)  && (fabs(bval_l[j] - bval_l[i])) < 3) {
	    printf("bark=%f   x+tempy = %f  \n",bval_l[j] - bval_l[i],x+tempy);
	  }
	  */

	  if (tempy <= -60.0) s3_l[i][j] = 0.0;
	  else                s3_l[i][j] = exp( (x + tempy)*LN_TO_LOG10 ); 
	}
    }

  /* Read short block data */
  for(loop=0;loop<6;loop++)
    {
      freq_tp = *p++;
      cbmax_tp = (int) *p++;
      cbmax_tp++;

      if (sfreq == freq_tp/freq_scale )
	{
	  cbmax = cbmax_tp;
	  for(i=0,k2=0;i<cbmax_tp;i++)
	    {
	      j = (int) *p++;
	      numlines_s[i] = (int) *p++;
	      qthr_s[i] = *p++;         
	      /* norm_s[i] =*p++ */ p++;         
	      SNR[i] = *p++;            
	      bval_s[i] = *p++;
	      if (j!=i)
		{
		  fprintf(stderr,"3. please check \"psy_data\"");
		  exit(-1);
		}
	      numlines_s[i]--;
	    }
	  numlines_s[i] = -1;
	}
      else
	p += cbmax_tp * 6;
    }


#ifdef NEWBARK
  /* compute bark values of each critical band */
  j = 0;
  for(i=0;i<cbmax;i++)
    {
      FLOAT8 ji, freq, bark;
      ji = (j * 2 + numlines_s[i]) / 2.0;
      freq = sfreq*ji/256000.0;
      bark = 13*atan(.76*freq) + 3.5*atan(freq*freq/(7.5*7.5));
      printf("%i %i bval_s = %f  %f  %f \n",i,j,bval_s[i],freq,bark);
      bval_s[i]=bark;
      j += numlines_s[i] + 1;
    }
#endif



  /************************************************************************
   * Now compute the spreading function, s[j][i], the value of the spread-*
   * ing function, centered at band j, for band i, store for later use    *
   ************************************************************************/
  part_max = cbmax ;
  for(i=0;i<part_max;i++)
    {
      FLOAT8 tempx,x,tempy,temp;
      for(j=0;j<part_max;j++)
	{
	  /* tempx = (bval_s[i] - bval_s[j])*1.05;*/
	  if (j>=i) tempx = (bval_s[i] - bval_s[j])*3.0;
	  else    tempx = (bval_s[i] - bval_s[j])*1.5;
#ifdef AACS3
          if (i>=j) tempx = (bval_s[i] - bval_s[j])*3.0;
	  else    tempx = (bval_s[i] - bval_s[j])*1.5; 
#endif
	  if(tempx>=0.5 && tempx<=2.5)
	    {
	      temp = tempx - 0.5;
	      x = 8.0 * (temp*temp - 2.0 * temp);
	    }
	  else x = 0.0;
	  tempx += 0.474;
	  tempy = 15.811389 + 7.5*tempx - 17.5*sqrt(1.0+tempx*tempx);
#ifdef NEWS3
	  if (j>=i) tempy = (bval_s[j] - bval_s[i])*(-15);
	  else    tempy = (bval_s[j] - bval_s[i])*25;
	  x=0; 
#endif
	  if (tempy <= -60.0) s3_s[i][j] = 0.0;
	  else                s3_s[i][j] = exp( (x + tempy)*LN_TO_LOG10 );
	}
    }
  /* Read long block data for converting threshold calculation 
     partitions to scale factor bands */

  for(loop=0;loop<6;loop++)
    {
      freq_tp = *p++;
      sbmax =  (int) *p++;
      sbmax++;

      if (sfreq == freq_tp/freq_scale)
	{
	  for(i=0;i<sbmax;i++)
	    {
	      j = (int) *p++;
	      p++;             
	      bu_l[i] = (int) *p++;
	      bo_l[i] = (int) *p++;
	      w1_l[i] = (FLOAT8) *p++;
	      w2_l[i] = (FLOAT8) *p++;
	      if (j!=i)
		{ fprintf(stderr,"30:please check \"psy_data\"\n");
		exit(-1);
		}

	      if (i!=0)
		if ( (fabs(1.0-w1_l[i]-w2_l[i-1]) > 0.01 ) )
		  {
		    fprintf(stderr,"31l: please check \"psy_data.\"\n");
                  fprintf(stderr,"w1,w2: %f %f \n",w1_l[i],w2_l[i-1]);
		    exit(-1);
		  }
	    }
	}
      else
	p += sbmax * 6;
    }

  /* Read short block data for converting threshold calculation 
     partitions to scale factor bands */

  for(loop=0;loop<6;loop++)
    {
      freq_tp = *p++;
      sbmax = (int) *p++;
      sbmax++;

      if (sfreq == freq_tp/freq_scale)
	{
	  for(i=0;i<sbmax;i++)
	    {
	      j = (int) *p++;
	      p++;
	      bu_s[i] = (int) *p++;
	      bo_s[i] = (int) *p++;
	      w1_s[i] = *p++;
	      w2_s[i] = *p++;
	      if (j!=i)
		{ fprintf(stderr,"30:please check \"psy_data\"\n");
		exit(-1);
		}

	      if (i!=0)
		if ( (fabs(1.0-w1_s[i]-w2_s[i-1]) > 0.01 ) )
		  { 
                  fprintf(stderr,"31s: please check \"psy_data.\"\n");
                  fprintf(stderr,"w1,w2: %f %f \n",w1_s[i],w2_s[i-1]);
		  exit(-1);
		  }
	    }
	}
      else
	p += sbmax * 6;
    }

}


/* ==== quantize-pvt.c ==== */
#include <assert.h>
#include "util.h"
#include "tables.h"
#include "reservoir.h"
#include "quantize-pvt.h"

FLOAT masking_lower=1;
int convert_mdct, reduce_sidechannel;
/*
mt 5/99.  These global flags denote 4 possibilities:
                                                                mode    l3_xmin
1   MDCT input L/R, quantize L/R,   psy-model thresholds: L/R   -m s     either
2   MDCT input L/R, quantize M/S,   psy-model thresholds: L/R   -m j     orig
3   MDCT input M/S, quantize M/S,   psy-model thresholds: M/S   -m f     either
4   MDCT input L/R, quantize M/S,   psy-model thresholds: M/S   -m j -h  m/s

1:  convert_mdct = 0, convert_psy=0,  reduce_sidechannel=0          
2:  convert_mdct = 1, convert_psy=1,  reduce_sidechannel=1
3:  convert_mdct = 0, convert_psy=0,  reduce_sidechannel=1   (this mode no longer used)
4:  convert_mdct = 1, convert_psy=0,  reduce_sidechannel=1

if (convert_mdct), then iteration_loop will quantize M/S data from
the L/R input MDCT coefficients.

if (convert_psy), then calc_noise will compute the noise for the L/R
channels from M/S MDCT data and L/R psy-model threshold information.
Distortion in ether L or R channel will be marked as distortion in
both Mid and Side channels.  
NOTE: 3/00: this mode has been removed.  

if (reduce_sidechannel) then outer_loop will allocate less bits
to the side channel and more bits to the mid channel based on relative 
energies.
*/



/*
  The following table is used to implement the scalefactor
  partitioning for MPEG2 as described in section
  2.4.3.2 of the IS. The indexing corresponds to the
  way the tables are presented in the IS:

  [table_number][row_in_table][column of nr_of_sfb]
*/
unsigned nr_of_sfb_block[6][3][4] =
{
  {
    {6, 5, 5, 5},
    {9, 9, 9, 9},
    {6, 9, 9, 9}
  },
  {
    {6, 5, 7, 3},
    {9, 9, 12, 6},
    {6, 9, 12, 6}
  },
  {
    {11, 10, 0, 0},
    {18, 18, 0, 0},
    {15,18,0,0}
  },
  {
    {7, 7, 7, 0},
    {12, 12, 12, 0},
    {6, 15, 12, 0}
  },
  {
    {6, 6, 6, 3},
    {12, 9, 9, 6},
    {6, 12, 9, 6}
  },
  {
    {8, 8, 5, 0},
    {15,12,9,0},
    {6,18,9,0}
  }
};


/* Table B.6: layer3 preemphasis */
int  pretab[21] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 3, 2
};

/*
  Here are MPEG1 Table B.8 and MPEG2 Table B.1
  -- Layer III scalefactor bands. 
  Index into this using a method such as:
    idx  = fr_ps->header->sampling_frequency
           + (fr_ps->header->version * 3)
*/

struct scalefac_struct sfBandIndex[6] =
{
  { /* Table B.2.b: 22.05 kHz */
    {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
    {0,4,8,12,18,24,32,42,56,74,100,132,174,192}
  },
  { /* Table B.2.c: 24 kHz */                 /* docs: 332. mpg123: 330 */
    {0,6,12,18,24,30,36,44,54,66,80,96,114,136,162,194,232,278, 332, 394,464,540,576},
    {0,4,8,12,18,26,36,48,62,80,104,136,180,192}
  },
  { /* Table B.2.a: 16 kHz */
    {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
    {0,4,8,12,18,26,36,48,62,80,104,134,174,192}
  },
  { /* Table B.8.b: 44.1 kHz */
    {0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418,576},
    {0,4,8,12,16,22,30,40,52,66,84,106,136,192}
  },
  { /* Table B.8.c: 48 kHz */
    {0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384,576},
    {0,4,8,12,16,22,28,38,50,64,80,100,126,192}
  },
  { /* Table B.8.a: 32 kHz */
    {0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550,576},
    {0,4,8,12,16,22,30,42,58,78,104,138,180,192}
  }
};

struct scalefac_struct scalefac_band;


FLOAT8 pow20[Q_MAX];
FLOAT8 ipow20[Q_MAX];
FLOAT8 pow43[PRECALC_SIZE];
static FLOAT8 adj43[PRECALC_SIZE];
static FLOAT8 adj43asm[PRECALC_SIZE];
static FLOAT8 ATH_l[SBPSY_l];
static FLOAT8 ATH_s[SBPSY_l];

FLOAT8 ATH_mdct_long[576];
FLOAT8 ATH_mdct_short[192];


/************************************************************************/
/*  initialization for iteration_loop */
/************************************************************************/
void
iteration_init( lame_global_flags *gfp,III_side_info_t *l3_side, int l3_enc[2][2][576])
{
  gr_info *cod_info;
  int ch, gr, i;

  l3_side->resvDrain = 0;

  if ( gfp->frameNum==0 ) {
    for (i = 0; i < SBMAX_l + 1; i++) {
      scalefac_band.l[i] =
	sfBandIndex[gfp->samplerate_index + (gfp->version * 3)].l[i];
    }
    for (i = 0; i < SBMAX_s + 1; i++) {
      scalefac_band.s[i] =
	sfBandIndex[gfp->samplerate_index + (gfp->version * 3)].s[i];
    }

    l3_side->main_data_begin = 0;
    compute_ath(gfp,ATH_l,ATH_s);

    for(i=0;i<PRECALC_SIZE;i++)
        pow43[i] = pow((FLOAT8)i, 4.0/3.0);

    for (i = 0; i < PRECALC_SIZE-1; i++)
	adj43[i] = (i + 1) - pow(0.5 * (pow43[i] + pow43[i + 1]), 0.75);
    adj43[i] = 0.5;


    adj43asm[0] = 0.0;
    for (i = 1; i < PRECALC_SIZE; i++)
      adj43asm[i] = i - 0.5 - pow(0.5 * (pow43[i - 1] + pow43[i]),0.75);

    for (i = 0; i < Q_MAX; i++) {
	ipow20[i] = pow(2.0, (double)(i - 210) * -0.1875);
	pow20[i] = pow(2.0, (double)(i - 210) * 0.25);
    }
  }


  convert_mdct=0;
  reduce_sidechannel=0;
  if (gfp->mode_ext==MPG_MD_MS_LR) {
    convert_mdct = 1;
    reduce_sidechannel=1;
  }
  
  /* some intializations. */
  for ( gr = 0; gr < gfp->mode_gr; gr++ ){
    for ( ch = 0; ch < gfp->stereo; ch++ ){
      cod_info = (gr_info *) &(l3_side->gr[gr].ch[ch]);

      if (cod_info->block_type == SHORT_TYPE)
        {
	  cod_info->sfb_lmax = 0; /* No sb*/
	  cod_info->sfb_smax = 0;
        }
      else
	{
	  /* MPEG 1 doesnt use last scalefactor band */
	  cod_info->sfb_lmax = SBPSY_l;
	  cod_info->sfb_smax = SBPSY_s;    /* No sb */
	}

    }
  }


  /* dont bother with scfsi. */
  for ( ch = 0; ch < gfp->stereo; ch++ )
    for ( i = 0; i < 4; i++ )
      l3_side->scfsi[ch][i] = 0;
}





/* 
compute the ATH for each scalefactor band 
cd range:  0..96db

Input:  3.3kHz signal  32767 amplitude  (3.3kHz is where ATH is smallest = -5db)
longblocks:  sfb=12   en0/bw=-11db    max_en0 = 1.3db
shortblocks: sfb=5           -9db              0db

Input:  1 1 1 1 1 1 1 -1 -1 -1 -1 -1 -1 -1 (repeated)
longblocks:  amp=1      sfb=12   en0/bw=-103 db      max_en0 = -92db
            amp=32767   sfb=12           -12 db                 -1.4db 

Input:  1 1 1 1 1 1 1 -1 -1 -1 -1 -1 -1 -1 (repeated)
shortblocks: amp=1      sfb=5   en0/bw= -99                    -86 
            amp=32767   sfb=5           -9  db                  4db 


MAX energy of largest wave at 3.3kHz = 1db
AVE energy of largest wave at 3.3kHz = -11db
Let's take AVE:  -11db = maximum signal in sfb=12.  
Dynamic range of CD: 96db.  Therefor energy of smallest audible wave 
in sfb=12  = -11  - 96 = -107db = ATH at 3.3kHz.  

ATH formula for this wave: -5db.  To adjust to LAME scaling, we need
ATH = ATH_formula  - 103  (db)
ATH = ATH * 2.5e-10      (ener)

*/
FLOAT8 ATHformula(lame_global_flags *gfp,FLOAT8 f)
{
  FLOAT8 ath;
  f  = Max(0.02, f);
  /* from Painter & Spanias, 1997 */
  /* minimum: (i=77) 3.3kHz = -5db */
  ath=(3.640 * pow(f,-0.8)
       -  6.500 * exp(-0.6*pow(f-3.3,2.0))
       +  0.001 * pow(f,4.0));
  /* convert to energy */
  if (gfp->noATH)
    ath -= 200; /* disables ATH */
  else {
    ath -= 114;    /* MDCT scaling.  From tests by macik and MUS420 code */
    /* ath -= 109; */
  }
#ifdef RH_QUALITY_CONTROL 
  /* purpose of RH_QUALITY_CONTROL:
   * at higher quality lower ATH masking abilities   => needs more bits
   * at lower quality increase ATH masking abilities => needs less bits
   * works together with adjusted masking lowering of GPSYCHO thresholds
   * (Robert.Hegemann@gmx.de 2000-01-30)
   */
  ath -= (4-gfp->VBR_q)*4.0; 
#endif
  ath = pow( 10.0, ath/10.0 );
  return ath;
}
 

void compute_ath(lame_global_flags *gfp,FLOAT8 ATH_l[SBPSY_l],FLOAT8 ATH_s[SBPSY_l])
{
  int sfb,i,start,end;
  FLOAT8 ATH_f;
  FLOAT8 samp_freq = gfp->out_samplerate/1000.0;
#ifdef RH_ATH
  /* going from average to peak level ATH masking
   */
  FLOAT8 adjust_mdct_scaling = 10.0; 
#endif
  

  /* last sfb is not used */
  for ( sfb = 0; sfb < SBPSY_l; sfb++ ) {
    start = scalefac_band.l[ sfb ];
    end   = scalefac_band.l[ sfb+1 ];
    ATH_l[sfb]=1e99;
    for (i=start ; i < end; i++) {
      ATH_f = ATHformula(gfp,samp_freq*i/(2*576)); /* freq in kHz */
      ATH_l[sfb]=Min(ATH_l[sfb],ATH_f);
#ifdef RH_ATH
      ATH_mdct_long[i] = ATH_f*adjust_mdct_scaling;
#endif
    }
    /*
    printf("sfb=%i %f  ATH=%f %f  %f   \n",sfb,samp_freq*start/(2*576),
10*log10(ATH_l[sfb]),
10*log10( ATHformula(samp_freq*start/(2*576)))  ,
10*log10(ATHformula(samp_freq*end/(2*576))));
    */
  }

  for ( sfb = 0; sfb < SBPSY_s; sfb++ ){
    start = scalefac_band.s[ sfb ];
    end   = scalefac_band.s[ sfb+1 ];
    ATH_s[sfb]=1e99;
    for (i=start ; i < end; i++) {
      ATH_f = ATHformula(gfp,samp_freq*i/(2*192));     /* freq in kHz */
      ATH_s[sfb]=Min(ATH_s[sfb],ATH_f);
#ifdef RH_ATH
      ATH_mdct_short[i] = ATH_f*adjust_mdct_scaling;
#endif
    }
  }
}





/* convert from L/R <-> Mid/Side */
void ms_convert(FLOAT8 xr[2][576],FLOAT8 xr_org[2][576])
{
  int i;
  for ( i = 0; i < 576; i++ ) {
    FLOAT8 l = xr_org[0][i];
    FLOAT8 r = xr_org[1][i];
    xr[0][i] = (l+r)*(SQRT2*0.5);
    xr[1][i] = (l-r)*(SQRT2*0.5);
  }
}



/************************************************************************
 * allocate bits among 2 channels based on PE
 * mt 6/99
 ************************************************************************/
void on_pe(lame_global_flags *gfp,FLOAT8 pe[2][2],III_side_info_t *l3_side,
int targ_bits[2],int mean_bits, int gr)
{
  gr_info *cod_info;
  int extra_bits,tbits,bits;
  int add_bits[2]; 
  int ch;

  /* allocate targ_bits for granule */
  ResvMaxBits( mean_bits, &tbits, &extra_bits, gr);
    

  for (ch=0 ; ch < gfp->stereo ; ch ++) {
    /******************************************************************
     * allocate bits for each channel 
     ******************************************************************/
    cod_info = &l3_side->gr[gr].ch[ch].tt;
    
    targ_bits[ch]=tbits/gfp->stereo;
    
    /* allocate extra bits from reservoir based on PE */
    bits=0;
    
    /* extra bits based on PE > 700 */
    add_bits[ch]=(pe[gr][ch]-750)/1.55;  /* 1.4; */
    
    /* short blocks need extra, no matter what the pe */
    if (cod_info->block_type==SHORT_TYPE) 
      if (add_bits[ch]<500) add_bits[ch]=500;
    
    if (add_bits[ch] < 0) add_bits[ch]=0;
    bits += add_bits[ch];
    
    if (bits > extra_bits) add_bits[ch] = (extra_bits*add_bits[ch])/bits;
    if ((targ_bits[ch]+add_bits[ch]) > 4095) 
      add_bits[ch]=4095-targ_bits[ch];

    targ_bits[ch] = targ_bits[ch] + add_bits[ch];
    extra_bits -= add_bits[ch];
  }
}

void reduce_side(int targ_bits[2],FLOAT8 ms_ener_ratio,int mean_bits)
{
int ch;
int numchn=2;
    /*  ms_ener_ratio = 0:  allocate 66/33  mid/side  fac=.33  
     *  ms_ener_ratio =.5:  allocate 50/50 mid/side   fac= 0 */
    /* 75/25 split is fac=.5 */
    /* float fac = .50*(.5-ms_ener_ratio[gr])/.5;*/
    float fac = .33*(.5-ms_ener_ratio)/.5;
    if (fac<0) fac=0;
    
    if (targ_bits[1] >= 125) {
      /* dont reduce side channel below 125 bits */
      if (targ_bits[1]-targ_bits[1]*fac > 125) {
	targ_bits[0] += targ_bits[1]*fac;
	targ_bits[1] -= targ_bits[1]*fac;
      } else {
	targ_bits[0] += targ_bits[1] - 125;
	targ_bits[1] = 125;
      }
    }
    
    /* dont allow to many bits per channel */  
    for (ch=0; ch<numchn; ch++) {
      int max_bits = Min(4095,mean_bits/2 + 1200);
      if (targ_bits[ch] > max_bits) {
	targ_bits[ch] = max_bits;
      }
    }

}

/*************************************************************************** 
 *         inner_loop                                                      * 
 *************************************************************************** 
 * The code selects the best global gain for a particular set of scalefacs */
 
int
inner_loop( lame_global_flags *gfp,FLOAT8 xrpow[576],
	    int l3_enc[576], int max_bits,
	    gr_info *cod_info)
{
    int bits;
    assert( max_bits >= 0 );
    cod_info->global_gain--;
    do
    {
      cod_info->global_gain++;
      bits = count_bits(gfp,l3_enc, xrpow, cod_info);
    }
    while ( bits > max_bits );
    return bits;
}



/*************************************************************************/
/*            scale_bitcount                                             */
/*************************************************************************/

/* Also calculates the number of bits necessary to code the scalefactors. */

int scale_bitcount( III_scalefac_t *scalefac, gr_info *cod_info)
{
    int i, k, sfb, max_slen1 = 0, max_slen2 = 0, /*a, b, */ ep = 2;

    static int slen1[16] = { 1, 1, 1, 1, 8, 2, 2, 2, 4, 4, 4, 8, 8, 8,16,16 };
    static int slen2[16] = { 1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8, 2, 4, 8, 4, 8 };

    static int slen1_tab[16] = {0,
	18, 36, 54, 54, 36, 54, 72, 54, 72, 90, 72, 90,108,108,126
    };
    static int slen2_tab[16] = {0,
	10, 20, 30, 33, 21, 31, 41, 32, 42, 52, 43, 53, 63, 64, 74
    };
    int *tab;


    if ( cod_info->block_type == SHORT_TYPE )
    {
            tab = slen1_tab;
            /* a = 18; b = 18;  */
            for ( i = 0; i < 3; i++ )
            {
                for ( sfb = 0; sfb < 6; sfb++ )
                    if (scalefac->s[sfb][i] > max_slen1 )
                        max_slen1 = scalefac->s[sfb][i];
                for (sfb = 6; sfb < SBPSY_s; sfb++ )
                    if ( scalefac->s[sfb][i] > max_slen2 )
                        max_slen2 = scalefac->s[sfb][i];
            }
    }
    else
    { /* block_type == 1,2,or 3 */
        tab = slen2_tab;
        /* a = 11; b = 10;   */
        for ( sfb = 0; sfb < 11; sfb++ )
            if ( scalefac->l[sfb] > max_slen1 )
                max_slen1 = scalefac->l[sfb];

	if (!cod_info->preflag) {
	    for ( sfb = 11; sfb < SBPSY_l; sfb++ )
		if (scalefac->l[sfb] < pretab[sfb])
		    break;

	    if (sfb == SBPSY_l) {
		cod_info->preflag = 1;
		for ( sfb = 11; sfb < SBPSY_l; sfb++ )
		    scalefac->l[sfb] -= pretab[sfb];
	    }
	}

        for ( sfb = 11; sfb < SBPSY_l; sfb++ )
            if ( scalefac->l[sfb] > max_slen2 )
                max_slen2 = scalefac->l[sfb];
    }



    /* from Takehiro TOMINAGA <tominaga@isoternet.org> 10/99
     * loop over *all* posible values of scalefac_compress to find the
     * one which uses the smallest number of bits.  ISO would stop
     * at first valid index */
    cod_info->part2_length = LARGE_BITS;
    for ( k = 0; k < 16; k++ )
    {
        if ( (max_slen1 < slen1[k]) && (max_slen2 < slen2[k]) &&
             ((int)cod_info->part2_length > tab[k])) {
	  cod_info->part2_length=tab[k];
	  cod_info->scalefac_compress=k;
	  ep=0;  /* we found a suitable scalefac_compress */
	}
    }
    return ep;
}



/*
  table of largest scalefactors (number of bits) for MPEG2
*/
/*
static unsigned max_sfac_tab[6][4] =
{
    {4, 4, 3, 3},
    {4, 4, 3, 0},
    {3, 2, 0, 0},
    {4, 5, 5, 0},
    {3, 3, 3, 0},
    {2, 2, 0, 0}
};
*/
/*
  table of largest scalefactor values for MPEG2
*/
static unsigned max_range_sfac_tab[6][4] =
{
 { 15, 15, 7,  7},
 { 15, 15, 7,  0},
 { 7,  3,  0,  0},
 { 15, 31, 31, 0},
 { 7,  7,  7,  0},
 { 3,  3,  0,  0}
};





/*************************************************************************/
/*            scale_bitcount_lsf                                         */
/*************************************************************************/

/* Also counts the number of bits to encode the scalefacs but for MPEG 2 */ 
/* Lower sampling frequencies  (24, 22.05 and 16 kHz.)                   */
 
/*  This is reverse-engineered from section 2.4.3.2 of the MPEG2 IS,     */
/* "Audio Decoding Layer III"                                            */

int scale_bitcount_lsf(III_scalefac_t *scalefac, gr_info *cod_info)
{
    int table_number, row_in_table, partition, nr_sfb, window, over;
    int i, sfb, max_sfac[ 4 ];
    unsigned *partition_table;

    /*
      Set partition table. Note that should try to use table one,
      but do not yet...
    */
    if ( cod_info->preflag )
	table_number = 2;
    else
	table_number = 0;

    for ( i = 0; i < 4; i++ )
	max_sfac[i] = 0;

    if ( cod_info->block_type == SHORT_TYPE )
    {
	    row_in_table = 1;
	    partition_table = &nr_of_sfb_block[table_number][row_in_table][0];
	    for ( sfb = 0, partition = 0; partition < 4; partition++ )
	    {
		nr_sfb = partition_table[ partition ] / 3;
		for ( i = 0; i < nr_sfb; i++, sfb++ )
		    for ( window = 0; window < 3; window++ )
			if ( scalefac->s[sfb][window] > max_sfac[partition] )
			    max_sfac[partition] = scalefac->s[sfb][window];
	    }
    }
    else
    {
	row_in_table = 0;
	partition_table = &nr_of_sfb_block[table_number][row_in_table][0];
	for ( sfb = 0, partition = 0; partition < 4; partition++ )
	{
	    nr_sfb = partition_table[ partition ];
	    for ( i = 0; i < nr_sfb; i++, sfb++ )
		if ( scalefac->l[sfb] > max_sfac[partition] )
		    max_sfac[partition] = scalefac->l[sfb];
	}
    }

    for ( over = 0, partition = 0; partition < 4; partition++ )
    {
	if ( max_sfac[partition] > (int)max_range_sfac_tab[table_number][partition] )
	    over++;
    }
    if ( !over )
    {
	/*
	  Since no bands have been over-amplified, we can set scalefac_compress
	  and slen[] for the formatter
	*/
	static int log2tab[] = { 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4 };

	unsigned slen1, slen2, slen3, slen4;

        cod_info->sfb_partition_table = &nr_of_sfb_block[table_number][row_in_table][0];
	for ( partition = 0; partition < 4; partition++ )
	    cod_info->slen[partition] = log2tab[max_sfac[partition]];

	/* set scalefac_compress */
	slen1 = cod_info->slen[ 0 ];
	slen2 = cod_info->slen[ 1 ];
	slen3 = cod_info->slen[ 2 ];
	slen4 = cod_info->slen[ 3 ];

	switch ( table_number )
	{
	  case 0:
	    cod_info->scalefac_compress = (((slen1 * 5) + slen2) << 4)
		+ (slen3 << 2)
		+ slen4;
	    break;

	  case 1:
	    cod_info->scalefac_compress = 400
		+ (((slen1 * 5) + slen2) << 2)
		+ slen3;
	    break;

	  case 2:
	    cod_info->scalefac_compress = 500 + (slen1 * 3) + slen2;
	    break;

	  default:
	    fprintf( stderr, "intensity stereo not implemented yet\n" );
	    exit( EXIT_FAILURE );
	    break;
	}
    }
#ifdef DEBUG
    if ( over ) 
        printf( "---WARNING !! Amplification of some bands over limits\n" );
#endif
    if (!over) {
      assert( cod_info->sfb_partition_table );     
      cod_info->part2_length=0;
      for ( partition = 0; partition < 4; partition++ )
	cod_info->part2_length += cod_info->slen[partition] * cod_info->sfb_partition_table[partition];
    }
    return over;
}





/*************************************************************************/
/*            calc_xmin                                                  */
/*************************************************************************/

/*
  Calculate the allowed distortion for each scalefactor band,
  as determined by the psychoacoustic model.
  xmin(sb) = ratio(sb) * en(sb) / bw(sb)

  returns number of sfb's with energy > ATH
*/
int calc_xmin( lame_global_flags *gfp,FLOAT8 xr[576], III_psy_ratio *ratio,
	       gr_info *cod_info, III_psy_xmin *l3_xmin)
{
    int start, end, bw,l, b, ath_over=0;
	u_int	sfb;
    FLOAT8 en0, xmin, ener;

    if (gfp->ATHonly) {    
      for ( sfb = cod_info->sfb_smax; sfb < SBPSY_s; sfb++ )
	  for ( b = 0; b < 3; b++ )
	      l3_xmin->s[sfb][b]=ATH_s[sfb];
      for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ )
	  l3_xmin->l[sfb]=ATH_l[sfb];

    }else{

      for ( sfb = cod_info->sfb_smax; sfb < SBPSY_s; sfb++ ) {
	start = scalefac_band.s[ sfb ];
        end   = scalefac_band.s[ sfb + 1 ];
	bw = end - start;
        for ( b = 0; b < 3; b++ ) {
	  for (en0 = 0.0, l = start; l < end; l++) {
	    ener = xr[l * 3 + b];
	    ener = ener * ener;
	    en0 += ener;
	  }
	  en0 /= bw;

	  xmin = ratio->en.s[sfb][b];
	  if (xmin > 0.0)
	    xmin = en0 * ratio->thm.s[sfb][b] * masking_lower / xmin;

#ifdef RH_ATH
          /* do not mix up ATH masking with GPSYCHO thresholds
	   */
	  l3_xmin->s[sfb][b] = Max(1e-20, xmin);
#else
	  l3_xmin->s[sfb][b] = Max(ATH_s[sfb], xmin);
#endif
	  if (en0 > ATH_s[sfb]) ath_over++;
	}
      }

      for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ ){
	start = scalefac_band.l[ sfb ];
	end   = scalefac_band.l[ sfb+1 ];
	bw = end - start;

        for (en0 = 0.0, l = start; l < end; l++ ) {
	  ener = xr[l] * xr[l];
	  en0 += ener;
	}
	en0 /= bw;

	xmin = ratio->en.l[sfb];
	if (xmin > 0.0)
	  xmin = en0 * ratio->thm.l[sfb] * masking_lower / xmin;


#ifdef RH_ATH
        /* do not mix up ATH masking with GPSYCHO thresholds
	 */
	l3_xmin->l[sfb]=Max(1e-20, xmin);
#else
	l3_xmin->l[sfb]=Max(ATH_l[sfb], xmin);
#endif
	if (en0 > ATH_l[sfb]) ath_over++;
      }
    }
    return ath_over;
}



/*************************************************************************/
/*            loop_break                                                 */
/*************************************************************************/

/*  Function: Returns zero if there is a scalefac which has not been
    amplified. Otherwise it returns one. 
*/

int loop_break( III_scalefac_t *scalefac, gr_info *cod_info)
{
    int i;
	u_int sfb;

    for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ )
        if ( scalefac->l[sfb] == 0 )
	    return 0;

    for ( sfb = cod_info->sfb_smax; sfb < SBPSY_s; sfb++ )
      for ( i = 0; i < 3; i++ ) 
            if ( scalefac->s[sfb][i] == 0 )
		return 0;

    return 1;
}













/*
 ----------------------------------------------------------------------
  if someone wants to try to find a faster step search function,
  here is some code which gives a lower bound for the step size:
  
  for (max_xrspow = 0, i = 0; i < 576; ++i)
  {
    max_xrspow = Max(max_xrspow, xrspow[i]);
  }
  lowerbound = 210+log10(max_xrspow/IXMAX_VAL)/(0.1875*LOG2);
 
 
                                                 Robert.Hegemann@gmx.de
 ----------------------------------------------------------------------
*/


typedef enum {
    BINSEARCH_NONE,
    BINSEARCH_UP, 
    BINSEARCH_DOWN
} binsearchDirection_t;

/*-------------------------------------------------------------------------*/
int 
bin_search_StepSize2 (lame_global_flags *gfp,int desired_rate, int start, int *ix, 
                      FLOAT8 xrspow[576], gr_info *cod_info)
/*-------------------------------------------------------------------------*/
{
    static int CurrentStep = 4;
    int nBits;
    int flag_GoneOver = 0;
    int StepSize = start;
    binsearchDirection_t Direction = BINSEARCH_NONE;

    do
    {
	cod_info->global_gain = StepSize;
	nBits = count_bits(gfp,ix, xrspow, cod_info);  

	if (CurrentStep == 1 )
        {
	    break; /* nothing to adjust anymore */
	}
	if (flag_GoneOver)
	{
	    CurrentStep /= 2;
	}
	if (nBits > desired_rate)  /* increase Quantize_StepSize */
	{
	    if (Direction == BINSEARCH_DOWN && !flag_GoneOver)
	    {
		flag_GoneOver = 1;
		CurrentStep /= 2; /* late adjust */
	    }
	    Direction = BINSEARCH_UP;
	    StepSize += CurrentStep;
	    if (StepSize > 255) break;
	}
	else if (nBits < desired_rate)
	{
	    if (Direction == BINSEARCH_UP && !flag_GoneOver)
	    {
		flag_GoneOver = 1;
		CurrentStep /= 2; /* late adjust */
	    }
	    Direction = BINSEARCH_DOWN;
	    StepSize -= CurrentStep;
	    if (StepSize < 0) break;
	}
	else break; /* nBits == desired_rate;; most unlikely to happen.*/
    } while (1); /* For-ever, break is adjusted. */

    CurrentStep = abs(start - StepSize);
    
    if (CurrentStep >= 4) {
	CurrentStep = 4;
    } else {
	CurrentStep = 2;
    }

    return nBits;
}








#if (defined(__GNUC__) && defined(__i386__))
#define USE_GNUC_ASM
#endif
#ifdef _MSC_VER
#define USE_MSC_ASM
#endif



/*********************************************************************
 * XRPOW_FTOI is a macro to convert floats to ints.  
 * if XRPOW_FTOI(x) = nearest_int(x), then QUANTFAC(x)=adj43asm[x]
 *                                         ROUNDFAC= -0.0946
 *
 * if XRPOW_FTOI(x) = floor(x), then QUANTFAC(x)=asj43[x]   
 *                                   ROUNDFAC=0.4054
 *********************************************************************/
#ifdef USE_GNUC_ASM
#  define QUANTFAC(rx)  adj43asm[rx]
#  define ROUNDFAC -0.0946
#  define XRPOW_FTOI(src, dest) \
     asm ("fistpl %0 " : "=m"(dest) : "t"(src) : "st")
#elif defined (USE_MSC_ASM)
#  define QUANTFAC(rx)  adj43asm[rx]
#  define ROUNDFAC -0.0946
#  define XRPOW_FTOI(src, dest) do { \
     FLOAT8 src_ = (src); \
     int dest_; \
     { \
       __asm fld src_ \
       __asm fistp dest_ \
     } \
     (dest) = dest_; \
   } while (0)
#else
#  define QUANTFAC(rx)  adj43[rx]
#  define ROUNDFAC 0.4054
#  define XRPOW_FTOI(src,dest) ((dest) = (int)(src))
#endif

#ifdef USE_MSC_ASM
/* define F8type and F8size according to type of FLOAT8 */
# if defined FLOAT8_is_double
#  define F8type qword
#  define F8size 8
# elif defined FLOAT8_is_float
#  define F8type dword
#  define F8size 4
# else
/* only float and double supported */
#  error invalid FLOAT8 type for USE_MSC_ASM
# endif
#endif

#ifdef USE_GNUC_ASM
/* define F8type and F8size according to type of FLOAT8 */
# if defined FLOAT8_is_double
#  define F8type "l"
#  define F8size "8"
# elif defined FLOAT8_is_float
#  define F8type "s"
#  define F8size "4"
# else
/* only float and double supported */
#  error invalid FLOAT8 type for USE_GNUC_ASM
# endif
#endif

/*********************************************************************
 * nonlinear quantization of xr 
 * More accurate formula than the ISO formula.  Takes into account
 * the fact that we are quantizing xr -> ix, but we want ix^4/3 to be 
 * as close as possible to x^4/3.  (taking the nearest int would mean
 * ix is as close as possible to xr, which is different.)
 * From Segher Boessenkool <segher@eastsite.nl>  11/1999
 * ASM optimization from 
 *    Mathew Hendry <scampi@dial.pipex.com> 11/1999
 *    Acy Stapp <AStapp@austin.rr.com> 11/1999
 *    Takehiro Tominaga <tominaga@isoternet.org> 11/1999
 *********************************************************************/

void quantize_xrpow(FLOAT8 xr[576], int ix[576], gr_info *cod_info) {
  /* quantize on xr^(3/4) instead of xr */
  const FLOAT8 istep = IPOW20(cod_info->global_gain);

/*FGG

#if defined (USE_GNUC_ASM) 
  {
      int rx[4];
      __asm__ __volatile__(
        "\n\nloop1:\n\t"

        "fld" F8type " 0*" F8size "(%1)\n\t"
        "fld" F8type " 1*" F8size "(%1)\n\t"
        "fld" F8type " 2*" F8size "(%1)\n\t"
        "fld" F8type " 3*" F8size "(%1)\n\t"

        "fxch %%st(3)\n\t"
        "fmul %%st(4)\n\t"
        "fxch %%st(2)\n\t"
        "fmul %%st(4)\n\t"
        "fxch %%st(1)\n\t"
        "fmul %%st(4)\n\t"
        "fxch %%st(3)\n\t"
        "fmul %%st(4)\n\t"

        "addl $4*" F8size ", %1\n\t"
        "addl $16, %3\n\t"

        "fxch %%st(2)\n\t"
        "fistl %5\n\t"
        "fxch %%st(1)\n\t"
        "fistl 4+%5\n\t"
        "fxch %%st(3)\n\t"
        "fistl 8+%5\n\t"
        "fxch %%st(2)\n\t"
        "fistl 12+%5\n\t"

        "dec %4\n\t"

        "movl %5, %%eax\n\t"
        "movl 4+%5, %%ebx\n\t"
        "fxch %%st(1)\n\t"
        "fadd" F8type " (%2,%%eax," F8size ")\n\t"
        "fxch %%st(3)\n\t"
        "fadd" F8type " (%2,%%ebx," F8size ")\n\t"

        "movl 8+%5, %%eax\n\t"
        "movl 12+%5, %%ebx\n\t"
        "fxch %%st(2)\n\t"
        "fadd" F8type " (%2,%%eax," F8size ")\n\t"
        "fxch %%st(1)\n\t"
        "fadd" F8type " (%2,%%ebx," F8size ")\n\t"

        "fxch %%st(3)\n\t"
        "fistpl -16(%3)\n\t"
        "fxch %%st(1)\n\t"
        "fistpl -12(%3)\n\t"
        "fistpl -8(%3)\n\t"
        "fistpl -4(%3)\n\t"

        "jnz loop1\n\n"
        : "t" (istep), "r" (xr), "r" (adj43asm), "r" (ix), "r" (576 / 4), "m" (rx)
        : "%eax", "%ebx", "memory", "cc"
      );
  }
#elif defined (USE_MSC_ASM)
  {
      int rx[4];
      _asm {
          fld F8type ptr [istep]
          mov esi, dword ptr [xr]
          lea edi, dword ptr [adj43asm]
          mov edx, dword ptr [ix]
          mov ecx, 576/4
      } loop1: _asm {
          fld F8type ptr [esi+(0*F8size)] // 0
          fld F8type ptr [esi+(1*F8size)] // 1 0
          fld F8type ptr [esi+(2*F8size)] // 2 1 0
          fld F8type ptr [esi+(3*F8size)] // 3 2 1 0
          fxch st(3)                  // 0 2 1 3
          fmul st(0), st(4)
          fxch st(2)                  // 1 2 0 3
          fmul st(0), st(4)
          fxch st(1)                  // 2 1 0 3
          fmul st(0), st(4)
          fxch st(3)                  // 3 1 0 2
          fmul st(0), st(4)

          add esi, 4*F8size
          add edx, 16

          fxch st(2)                  // 0 1 3 2
          fist dword ptr [rx]
          fxch st(1)                  // 1 0 3 2
          fist dword ptr [rx+4]
          fxch st(3)                  // 2 0 3 1
          fist dword ptr [rx+8]
          fxch st(2)                  // 3 0 2 1
          fist dword ptr [rx+12]

          dec ecx

          mov eax, dword ptr [rx]
          mov ebx, dword ptr [rx+4]
          fxch st(1)                  // 0 3 2 1
          fadd F8type ptr [edi+eax*F8size]
          fxch st(3)                  // 1 3 2 0
          fadd F8type ptr [edi+ebx*F8size]

          mov eax, dword ptr [rx+8]
          mov ebx, dword ptr [rx+12]
          fxch st(2)                  // 2 3 1 0
          fadd F8type ptr [edi+eax*F8size]
          fxch st(1)                  // 3 2 1 0
          fadd F8type ptr [edi+ebx*F8size]
          fxch st(3)                  // 0 2 1 3
          fistp dword ptr [edx-16]    // 2 1 3
          fxch st(1)                  // 1 2 3
          fistp dword ptr [edx-12]    // 2 3
          fistp dword ptr [edx-8]     // 3
          fistp dword ptr [edx-4]

          jnz loop1

          mov dword ptr [xr], esi
          mov dword ptr [ix], edx
          fstp st(0)
      }
  }
#else
#if 0
*/
  {   /* generic code if you write ASM for XRPOW_FTOI() */
      FLOAT8 x;
      int j, rx;
      for (j = 576 / 4; j > 0; --j) {
          x = *xr++ * istep;
          XRPOW_FTOI(x, rx);
          XRPOW_FTOI(x + QUANTFAC(rx), *ix++);

          x = *xr++ * istep;
          XRPOW_FTOI(x, rx);
          XRPOW_FTOI(x + QUANTFAC(rx), *ix++);

          x = *xr++ * istep;
          XRPOW_FTOI(x, rx);
          XRPOW_FTOI(x + QUANTFAC(rx), *ix++);

          x = *xr++ * istep;
          XRPOW_FTOI(x, rx);
          XRPOW_FTOI(x + QUANTFAC(rx), *ix++);
      }
  }
/* FGG
#endif

  {
*/
   /* from Wilfried.Behne@t-online.de.  Reported to be 2x faster than 
      the above code (when not using ASM) on PowerPC */
/* FGG
     	int j;
     	
     	for ( j = 576/8; j > 0; --j)
     	{
			FLOAT8	x1, x2, x3, x4, x5, x6, x7, x8;
			int		rx1, rx2, rx3, rx4, rx5, rx6, rx7, rx8;
			x1 = *xr++ * istep;
			x2 = *xr++ * istep;
			XRPOW_FTOI(x1, rx1);
			x3 = *xr++ * istep;
			XRPOW_FTOI(x2, rx2);
			x4 = *xr++ * istep;
			XRPOW_FTOI(x3, rx3);
			x5 = *xr++ * istep;
			XRPOW_FTOI(x4, rx4);
			x6 = *xr++ * istep;
			XRPOW_FTOI(x5, rx5);
			x7 = *xr++ * istep;
			XRPOW_FTOI(x6, rx6);
			x8 = *xr++ * istep;
			XRPOW_FTOI(x7, rx7);
			x1 += QUANTFAC(rx1);
			XRPOW_FTOI(x8, rx8);
			x2 += QUANTFAC(rx2);
			XRPOW_FTOI(x1,*ix++);
			x3 += QUANTFAC(rx3);
			XRPOW_FTOI(x2,*ix++);
			x4 += QUANTFAC(rx4);		
			XRPOW_FTOI(x3,*ix++);
			x5 += QUANTFAC(rx5);
			XRPOW_FTOI(x4,*ix++);
			x6 += QUANTFAC(rx6);
			XRPOW_FTOI(x5,*ix++);
			x7 += QUANTFAC(rx7);
			XRPOW_FTOI(x6,*ix++);
			x8 += QUANTFAC(rx8);		
			XRPOW_FTOI(x7,*ix++);
			XRPOW_FTOI(x8,*ix++);
     	}
	}
#endif
*/
}






void quantize_xrpow_ISO( FLOAT8 xr[576], int ix[576], gr_info *cod_info )
{
  /* quantize on xr^(3/4) instead of xr */
  const FLOAT8 istep = IPOW20(cod_info->global_gain);

  /* FGG removed assembler part */
      register int j;
      const FLOAT8 compareval0 = (1.0 - 0.4054)/istep;
      /* depending on architecture, it may be worth calculating a few more compareval's.
         eg.  compareval1 = (2.0 - 0.4054/istep); 
              .. and then after the first compare do this ...
              if compareval1>*xr then ix = 1;
         On a pentium166, it's only worth doing the one compare (as done here), as the second
         compare becomes more expensive than just calculating the value. Architectures with 
         slow FP operations may want to add some more comparevals. try it and send your diffs 
         statistically speaking
         73% of all xr*istep values give ix=0
         16% will give 1
         4%  will give 2
      */
      for (j=576;j>0;j--) 
        {
          if (compareval0 > *xr) {
            *(ix++) = 0;
            xr++;
          } else
	    /*    *(ix++) = (int)( istep*(*(xr++))  + 0.4054); */
            XRPOW_FTOI(  istep*(*(xr++))  + ROUNDFAC , *(ix++) );
        }
}


/* ==== quantize.c ==== */
#define MAXNOISEXX
/*
 *	MP3 quantization
 *
 *	Copyright (c) 1999 Mark Taylor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */



#include <assert.h>
#include "util.h"
#include "l3side.h"
#include "quantize.h"
#include "l3bitstream.h"
#include "reservoir.h"
#include "quantize-pvt.h"
#ifdef HAVEGTK
#include "gtkanal.h"
#endif



#ifdef HAVEGTK

/************************************************************************/
/*  updates plotting data                                               */
/************************************************************************/
void 
set_pinfo (
    gr_info *cod_info,
    III_psy_ratio *ratio, 
    III_scalefac_t *scalefac,
    FLOAT8 xr[576],        
    FLOAT8 xfsf[4][SBPSY_l],
    FLOAT8 noise[4],
    int gr,
    int ch
)
{
  int sfb;
  FLOAT ifqstep;
  int i,l,start,end,bw;
  FLOAT8 en0;
  D192_3 *xr_s = (D192_3 *)xr;
  ifqstep = ( cod_info->scalefac_scale == 0 ) ? .5 : 1.0;
	  
  if (cod_info->block_type == SHORT_TYPE) {
    for ( i = 0; i < 3; i++ ) {
      for ( sfb = 0; sfb < SBPSY_s; sfb++ )  {
	start = scalefac_band.s[ sfb ];
	end   = scalefac_band.s[ sfb + 1 ];
	bw = end - start;
	for ( en0 = 0.0, l = start; l < end; l++ ) 
	  en0 += (*xr_s)[l][i] * (*xr_s)[l][i];
	en0=Max(en0/bw,1e-20);
		
	/* conversion to FFT units */
	en0 = ratio->en.s[sfb][i]/en0;
	
	pinfo->xfsf_s[gr][ch][3*sfb+i] =  xfsf[i+1][sfb]*en0;
	pinfo->thr_s[gr][ch][3*sfb+i] = ratio->thm.s[sfb][i];
	pinfo->en_s[gr][ch][3*sfb+i] = ratio->en.s[sfb][i]; 
	
	pinfo->LAMEsfb_s[gr][ch][3*sfb+i]=
	  -2*cod_info->subblock_gain[i]-ifqstep*scalefac->s[sfb][i];
      }
    }
  }else{
    for ( sfb = 0; sfb < SBPSY_l; sfb++ )   {
      start = scalefac_band.l[ sfb ];
      end   = scalefac_band.l[ sfb+1 ];
      bw = end - start;
      for ( en0 = 0.0, l = start; l < end; l++ ) 
	en0 += xr[l] * xr[l];
      en0=Max(en0/bw,1e-20);
      /*
	printf("diff  = %f \n",10*log10(Max(ratio[gr][ch].en.l[sfb],1e-20))
	-(10*log10(en0)+150));
      */
      
      /* convert to FFT units */
      en0 =   ratio->en.l[sfb]/en0;
      
      pinfo->xfsf[gr][ch][sfb] =  xfsf[0][sfb]*en0;
      pinfo->thr[gr][ch][sfb] = ratio->thm.l[sfb];
      pinfo->en[gr][ch][sfb] = ratio->en.l[sfb];
      
      pinfo->LAMEsfb[gr][ch][sfb]=-ifqstep*scalefac->l[sfb];
      if (cod_info->preflag && sfb>=11) 
	pinfo->LAMEsfb[gr][ch][sfb]-=ifqstep*pretab[sfb];
    }
  }
  pinfo->LAMEqss[gr][ch] = cod_info->global_gain;
  pinfo->LAMEmainbits[gr][ch] = cod_info->part2_3_length;

  pinfo->over      [gr][ch] = noise[0];
  pinfo->max_noise [gr][ch] = noise[1];
  pinfo->over_noise[gr][ch] = noise[2];
  pinfo->tot_noise [gr][ch] = noise[3];
}

#endif



/************************************************************************/
/*  iteration_loop()                                                    */
/************************************************************************/
void
iteration_loop( lame_global_flags *gfp,
                FLOAT8 pe[2][2], FLOAT8 ms_ener_ratio[2],
		FLOAT8 xr[2][2][576], III_psy_ratio ratio[2][2],
		III_side_info_t *l3_side, int l3_enc[2][2][576],
		III_scalefac_t scalefac[2][2])
{
  FLOAT8 xfsf[4][SBPSY_l];
  FLOAT8 noise[4]; /* over,max_noise,over_noise,tot_noise; */
  III_psy_xmin l3_xmin[2];
  gr_info *cod_info;
  int bitsPerFrame;
  int mean_bits;
  int ch, gr, i, bit_rate;


  iteration_init(gfp,l3_side,l3_enc);
  bit_rate = bitrate_table[gfp->version][gfp->bitrate_index];


  getframebits(gfp,&bitsPerFrame, &mean_bits);
  ResvFrameBegin(gfp, l3_side, mean_bits, bitsPerFrame );

  /* quantize! */



  for ( gr = 0; gr < gfp->mode_gr; gr++ ) {
    int targ_bits[2];

    if (convert_mdct) 
      ms_convert(xr[gr], xr[gr]);
    
    on_pe(gfp,pe,l3_side,targ_bits,mean_bits, gr);
#ifdef RH_SIDE_CBR
#else
    if (reduce_sidechannel) 
      reduce_side(targ_bits,ms_ener_ratio[gr],mean_bits);
#endif      
    
    for (ch=0 ; ch < gfp->stereo ; ch ++) {
      cod_info = &l3_side->gr[gr].ch[ch].tt;	
      if (!init_outer_loop(gfp,xr[gr][ch], cod_info))
        {
          /* xr contains no energy 
           * cod_info was set in init_outer_loop above
	   */
          memset(&scalefac[gr][ch],0,sizeof(III_scalefac_t));
          memset(l3_enc[gr][ch],0,576*sizeof(int));
	  noise[0]=noise[1]=noise[2]=noise[3]=0;
        }
      else
	{
          calc_xmin(gfp,xr[gr][ch], &ratio[gr][ch], cod_info, &l3_xmin[ch]);
	  outer_loop( gfp,xr[gr][ch], targ_bits[ch], noise,
		      &l3_xmin[ch], l3_enc[gr][ch], 
		      &scalefac[gr][ch], cod_info, xfsf, ch);
        }
      best_scalefac_store(gfp,gr, ch, l3_enc, l3_side, scalefac);
      if (gfp->use_best_huffman==1 && cod_info->block_type == NORM_TYPE) {
	best_huffman_divide(gr, ch, cod_info, l3_enc[gr][ch]);
      }
#ifdef HAVEGTK
      if (gfp->gtkflag)
	set_pinfo (cod_info, &ratio[gr][ch], &scalefac[gr][ch], xr[gr][ch], xfsf, noise, gr, ch);
#endif

/*#define NORES_TEST */
#ifndef NORES_TEST
      ResvAdjust(gfp,cod_info, l3_side, mean_bits );
#endif
      /* set the sign of l3_enc */
      for ( i = 0; i < 576; i++) {
	if (xr[gr][ch][i] < 0)
	  l3_enc[gr][ch][i] *= -1;
      }
    }
  } /* loop over gr */

#ifdef NORES_TEST
  /* replace ResvAdjust above with this code if you do not want
     the second granule to use bits saved by the first granule.
     when combined with --nores, this is usefull for testing only */
  for ( gr = 0; gr < gfp->mode_gr; gr++ ) {
    for ( ch =  0; ch < gfp->stereo; ch++ ) {
	cod_info = &l3_side->gr[gr].ch[ch].tt;
	ResvAdjust(gfp, cod_info, l3_side, mean_bits );
    }
  }
#endif



  ResvFrameEnd(gfp,l3_side, mean_bits );
}


void 
set_masking_lower (int VBR_q,int nbits)
{
	FLOAT masking_lower_db, adjust;
	
	/* quality setting */
	/* Adjust allowed masking based on quality setting */
	
#ifdef  RH_QUALITY_CONTROL	
	/* - lower masking depending on Quality setting
	 * - quality control together with adjusted ATH MDCT scaling
	 *   on lower quality setting allocate more noise from
	 *   ATH masking, and on higher quality setting allocate
	 *   less noise from ATH masking.
	 * - experiments show that going more than 2dB over GPSYCHO's
	 *   limits ends up in very annoying artefacts
	 */
	static FLOAT dbQ[10]={-6.0,-4.5,-3.0,-1.5,0,0.3,0.6,1.0,1.5,2.0};
	
	assert( VBR_q <= 9 );
	assert( VBR_q >= 0 );
	
	masking_lower_db = dbQ[VBR_q];	
	adjust = 0;
#else
	/* masking_lower varies from -8 to +10 db */
	masking_lower_db = -6 + 2*VBR_q;
	/* adjust by -6(min)..0(max) depending on bitrate */
	adjust = (nbits-125)/(2500.0-125.0);
	adjust = 4*(adjust-1);
#endif
	masking_lower_db += adjust;
	masking_lower = pow(10.0,masking_lower_db/10);
}

/************************************************************************
 *
 * VBR_iteration_loop()   
 *
 * tries to find out how many bits are needed for each granule and channel
 * to get an acceptable quantization. An appropriate bitrate will then be
 * choosed for quantization.  rh 8/99                                                
 *
 ************************************************************************/
void
VBR_iteration_loop (lame_global_flags *gfp,
                FLOAT8 pe[2][2], FLOAT8 ms_ener_ratio[2],
                FLOAT8 xr[2][2][576], III_psy_ratio ratio[2][2],
                III_side_info_t * l3_side, int l3_enc[2][2][576],
                III_scalefac_t scalefac[2][2])
{
#ifdef HAVEGTK
  plotting_data bst_pinfo;
#endif
  gr_info         bst_cod_info, clean_cod_info;
  III_scalefac_t  bst_scalefac;
  int             bst_l3_enc[576]; 
  
  III_psy_xmin l3_xmin;
  gr_info  *cod_info = NULL;
  int       save_bits[2][2];
  FLOAT8    noise[4];      /* over,max_noise,over_noise,tot_noise; */
  FLOAT8    targ_noise[4]; /* over,max_noise,over_noise,tot_noise; */
  FLOAT8    xfsf[4][SBPSY_l];
  int       this_bits, dbits;
  int       used_bits=0;
  int       min_bits,max_bits,min_mean_bits=0;
  int       frameBits[15];
  int       bitsPerFrame;
  int       bits;
  int       mean_bits;
  int       i,ch, gr, analog_silence;
  int	    reparted = 0;

  iteration_init(gfp,l3_side,l3_enc);

#ifdef RH_QUALITY_CONTROL
  /* with RH_QUALITY_CONTROL we have to set masking_lower only once */
  set_masking_lower(gfp->VBR_q, 0 );
#endif      

  /*******************************************************************
   * how many bits are available for each bitrate?
   *******************************************************************/
  for( gfp->bitrate_index = 1;
       gfp->bitrate_index <= gfp->VBR_max_bitrate;
       gfp->bitrate_index++    ) {
    getframebits (gfp,&bitsPerFrame, &mean_bits);
    if (gfp->bitrate_index == gfp->VBR_min_bitrate) {
      /* always use at least this many bits per granule per channel */
      /* unless we detect analog silence, see below */
      min_mean_bits=mean_bits/gfp->stereo;
    }
    frameBits[gfp->bitrate_index]=
      ResvFrameBegin (gfp,l3_side, mean_bits, bitsPerFrame);
  }

  gfp->bitrate_index=gfp->VBR_max_bitrate;

  
  /*******************************************************************
   * how many bits would we use of it?
   *******************************************************************/
  analog_silence=0;
  for (gr = 0; gr < gfp->mode_gr; gr++) {
    int num_chan=gfp->stereo;
#ifdef  RH_SIDE_VBR
    /* my experiences are, that side channel reduction  
     * does more harm than good when VBR encoding
     * (Robert.Hegemann@gmx.de 2000-02-18)
     */
#else
    /* determine quality based on mid channel only */
    if (reduce_sidechannel) num_chan=1;  
#endif

    /* copy data to be quantized into xr */
    if (convert_mdct)
	ms_convert(xr[gr],xr[gr]);

    for (ch = 0; ch < num_chan; ch++) { 
      int real_bits;
      
      /******************************************************************
       * find smallest number of bits for an allowable quantization
       ******************************************************************/
      cod_info = &l3_side->gr[gr].ch[ch].tt;
      min_bits = Max(125,min_mean_bits);

      if (!init_outer_loop(gfp,xr[gr][ch], cod_info))
      {
        /* xr contains no energy 
         * cod_info was set in init_outer_loop above
	 */
        memset(&scalefac[gr][ch],0,sizeof(III_scalefac_t));
        memset(l3_enc[gr][ch],0,576*sizeof(int));
        save_bits[gr][ch] = 0;
#ifdef HAVEGTK
	if (gfp->gtkflag)
	  set_pinfo(cod_info, &ratio[gr][ch], &scalefac[gr][ch], xr[gr][ch], xfsf, noise, gr, ch);
#endif
	analog_silence=1;
	continue; /* with next channel */
      }
      
      memcpy( &clean_cod_info, cod_info, sizeof(gr_info) );
      
#ifdef RH_QUALITY_CONTROL
      /*
       * masking lower already set in the beginning
       */
#else
      /*
       * has to be set before calculating l3_xmin
       */
      set_masking_lower( gfp->VBR_q,2500 );
#endif      
      /* check for analolg silence */
      /* if energy < ATH, set min_bits = 125 */
      if (0==calc_xmin(gfp,xr[gr][ch], &ratio[gr][ch], cod_info, &l3_xmin)) {
	  analog_silence=1;
	  min_bits=125;
      }

      if (cod_info->block_type==SHORT_TYPE) {
	  min_bits += Max(1100,pe[gr][ch]);
	  min_bits=Min(min_bits,1800);
      }

      max_bits = 1200 + frameBits[gfp->VBR_max_bitrate]/(gfp->stereo*gfp->mode_gr);
      max_bits=Min(max_bits,2500);
      max_bits=Max(max_bits,min_bits);

      dbits = (max_bits-min_bits)/4;
      this_bits = (max_bits+min_bits)/2;
      real_bits = max_bits+1;

      /* bin search to within +/- 10 bits of optimal */
      do {
	  int better;
	  assert(this_bits>=min_bits);
	  assert(this_bits<=max_bits);

	  if( this_bits >= real_bits ){
	      /* 
	       * we already found a quantization with fewer bits
	       * so we can skip this try
	       */
	      this_bits -= dbits;
	      dbits /= 2;
	      continue; /* skips the rest of this do-while loop */
	  }

	  /* VBR will look for a quantization which has better values
	   * then those specified below.*/
	  targ_noise[0]=0;          /* over */
	  targ_noise[1]=0;          /* max_noise */
	  targ_noise[2]=0;          /* over_noise */
	  targ_noise[3]=0;          /* tot_noise */
	
	  targ_noise[0]=Max(0,targ_noise[0]);
	  targ_noise[2]=Max(0,targ_noise[2]);

	  /*
	   *  OK, start with a fresh setting
	   *  - scalefac  will be set up by outer_loop
	   *  - l3_enc    will be set up by outer_loop
	   *  + cod_info  we will restore our initialized one, see below
	   */
	  memcpy( cod_info, &clean_cod_info, sizeof(gr_info) );

#ifdef RH_QUALITY_CONTROL
          /*
	   * there is no need for a recalculation of l3_xmin,
	   * because masking_lower did not change within this do-while
	   */
#else
	  /* quality setting */
	  set_masking_lower( gfp->VBR_q,this_bits );
          /* 
	   * compute max allowed distortion, masking lower has changed
	   */
          calc_xmin(gfp,xr[gr][ch], &ratio[gr][ch], cod_info, &l3_xmin);
#endif
	  outer_loop( gfp,xr[gr][ch], this_bits, noise, 
		      &l3_xmin, l3_enc[gr][ch],
		      &scalefac[gr][ch], cod_info, xfsf,
		      ch);

	  /* is quantization as good as we are looking for ? */
	  better=VBR_compare((int)targ_noise[0],targ_noise[3],targ_noise[2],
			     targ_noise[1],(int)noise[0],noise[3],noise[2],
			     noise[1]);
#ifdef HAVEGTK
	  if (gfp->gtkflag)
	    set_pinfo(cod_info, &ratio[gr][ch], &scalefac[gr][ch], xr[gr][ch], xfsf, noise, gr, ch);
#endif
	  if (better) {
	      /* 
	       * we now know it can be done with "real_bits"
	       * and maybe we can skip some iterations
	       */
	      real_bits = cod_info->part2_3_length;
	      /*
	       * save best quantization so far
	       */
              memcpy( &bst_scalefac, &scalefac[gr][ch], sizeof(III_scalefac_t)  );
              memcpy(  bst_l3_enc,    l3_enc  [gr][ch], sizeof(int)*576         );
              memcpy( &bst_cod_info,  cod_info,         sizeof(gr_info)         );
#ifdef HAVEGTK
              if (gfp->gtkflag) 
                memcpy( &bst_pinfo, pinfo, sizeof(plotting_data) );
#endif
	      /*
	       * try with fewer bits
	       */
	      this_bits -= dbits;
	  } else {
	      /*
	       * try with more bits
	       */
	      this_bits += dbits;
	  }
	  dbits /= 2;
      } while (dbits>10) ;
      
      if (real_bits <= max_bits)
      {
        /* restore best quantization found */
        memcpy(  cod_info,         &bst_cod_info, sizeof(gr_info)        );
        memcpy( &scalefac[gr][ch], &bst_scalefac, sizeof(III_scalefac_t) );
        memcpy(  l3_enc  [gr][ch],  bst_l3_enc,   sizeof(int)*576        );
#ifdef HAVEGTK
        if (gfp->gtkflag) 
          memcpy( pinfo, &bst_pinfo, sizeof(plotting_data) );
#endif
      }
      assert((int)cod_info->part2_3_length <= max_bits);
      save_bits[gr][ch] = cod_info->part2_3_length;
      used_bits += save_bits[gr][ch];
      
    } /* for ch */
  } /* for gr */


#ifdef  RH_SIDE_VBR
  /* my experiences are, that side channel reduction  
   * does more harm than good when VBR encoding
   * (Robert.Hegemann@gmx.de 2000-02-18)
   */
#else	
  if (reduce_sidechannel) {
    /* number of bits needed was found for MID channel above.  Use formula
     * (fixed bitrate code) to set the side channel bits */
    for (gr = 0; gr < gfp->mode_gr; gr++) {
      FLOAT8 fac = .33*(.5-ms_ener_ratio[gr])/.5;
      save_bits[gr][1]=((1-fac)/(1+fac))*save_bits[gr][0];
      save_bits[gr][1]=Max(125,save_bits[gr][1]);
      used_bits += save_bits[gr][1];
    }
  }
#endif

  /******************************************************************
   * find lowest bitrate able to hold used bits
   ******************************************************************/
  for( gfp->bitrate_index =   (analog_silence ? 1 : gfp->VBR_min_bitrate );
       gfp->bitrate_index < gfp->VBR_max_bitrate;
       gfp->bitrate_index++    )
    if( used_bits <= frameBits[gfp->bitrate_index] ) break;

  /*******************************************************************
   * calculate quantization for this bitrate
   *******************************************************************/  
  getframebits (gfp,&bitsPerFrame, &mean_bits);
  bits=ResvFrameBegin (gfp,l3_side, mean_bits, bitsPerFrame);

  /* repartion available bits in same proportion */
  if (used_bits > bits ) {
    reparted = 1;
    for( gr = 0; gr < gfp->mode_gr; gr++) {
      for(ch = 0; ch < gfp->stereo; ch++) {
	save_bits[gr][ch]=(save_bits[gr][ch]*frameBits[gfp->bitrate_index])/used_bits;
      }
    }
    used_bits=0;
    for( gr = 0; gr < gfp->mode_gr; gr++) {
      for(ch = 0; ch < gfp->stereo; ch++) {
	used_bits += save_bits[gr][ch];
      }
    }
  }
  assert(used_bits <= bits);

  for(gr = 0; gr < gfp->mode_gr; gr++) {
    for(ch = 0; ch < gfp->stereo; ch++) {
#ifdef RH_SIDE_VBR
      if (reparted)
#else
      if (reparted || (reduce_sidechannel && ch == 1))
#endif
      {
        cod_info = &l3_side->gr[gr].ch[ch].tt;
	       
	if (!init_outer_loop(gfp,xr[gr][ch], cod_info))
        {
          /* xr contains no energy 
           * cod_info was set in init_outer_loop above
	   */
          memset(&scalefac[gr][ch],0,sizeof(III_scalefac_t));
          memset(l3_enc[gr][ch],0,576*sizeof(int));
	  noise[0]=noise[1]=noise[2]=noise[3]=0;
        }
	else
	{
#ifdef RH_QUALITY_CONTROL
          /*
           * masking lower already set in the beginning
           */
#else
          /* quality setting */
          set_masking_lower( gfp->VBR_q,save_bits[gr][ch] );
#endif
          calc_xmin(gfp,xr[gr][ch], &ratio[gr][ch], cod_info, &l3_xmin);
	
          outer_loop( gfp,xr[gr][ch], save_bits[gr][ch], noise,
	 	      &l3_xmin, l3_enc[gr][ch], 
		      &scalefac[gr][ch], cod_info, xfsf, ch);
	}
#ifdef HAVEGTK
	if (gfp->gtkflag)
	  set_pinfo(cod_info, &ratio[gr][ch], &scalefac[gr][ch], xr[gr][ch], xfsf, noise, gr, ch);
#endif
      }
    }
  }

  /*******************************************************************
   * update reservoir status after FINAL quantization/bitrate 
   *******************************************************************/
  for (gr = 0; gr < gfp->mode_gr; gr++)
    for (ch = 0; ch < gfp->stereo; ch++) {
      cod_info = &l3_side->gr[gr].ch[ch].tt;
      best_scalefac_store(gfp,gr, ch, l3_enc, l3_side, scalefac);
      if (cod_info->block_type == NORM_TYPE) {
	best_huffman_divide(gr, ch, cod_info, l3_enc[gr][ch]);
      }
#ifdef HAVEGTK
      if (gfp->gtkflag)
	pinfo->LAMEmainbits[gr][ch]=cod_info->part2_3_length;
#endif
      ResvAdjust (gfp,cod_info, l3_side, mean_bits);
    }

  /*******************************************************************
   * set the sign of l3_enc 
   *******************************************************************/
  for (gr = 0; gr < gfp->mode_gr; gr++)
    for (ch = 0; ch < gfp->stereo; ch++) {
/*
 * is the following code correct?
 *
      int      *pi = &l3_enc[gr][ch][0];

      for (i = 0; i < 576; i++) {
        FLOAT8    pr = xr[gr][ch][i];

        if ((pr < 0) && (pi[i] > 0))
          pi[i] *= -1;
      }
 *
 * or is the code used for CBR correct?
 */
      for ( i = 0; i < 576; i++) {
        if (xr[gr][ch][i] < 0) l3_enc[gr][ch][i] *= -1;
      }
    }

  ResvFrameEnd (gfp,l3_side, mean_bits);
}




/************************************************************************/
/*  init_outer_loop  mt 6/99                                            */
/*  returns 0 if all energies in xr are zero, else 1                    */
/************************************************************************/
int init_outer_loop(lame_global_flags *gfp,
    FLOAT8 xr[576],        /*  could be L/R OR MID/SIDE */
    gr_info *cod_info)
{
  int i;


  for ( i = 0; i < 4; i++ )
    cod_info->slen[i] = 0;
  cod_info->sfb_partition_table = &nr_of_sfb_block[0][0][0];

  cod_info->part2_3_length    = 0;
  cod_info->big_values        = 0;
  cod_info->count1            = 0;
  cod_info->scalefac_compress = 0;
  cod_info->table_select[0]   = 0;
  cod_info->table_select[1]   = 0;
  cod_info->table_select[2]   = 0;
  cod_info->subblock_gain[0]  = 0;
  cod_info->subblock_gain[1]  = 0;
  cod_info->subblock_gain[2]  = 0;
  cod_info->region0_count     = 0;
  cod_info->region1_count     = 0;
  cod_info->part2_length      = 0;
  cod_info->preflag           = 0;
  cod_info->scalefac_scale    = 0;
  cod_info->global_gain       = 210;
  cod_info->count1table_select= 0;
  cod_info->count1bits        = 0;
  
  
  if (gfp->experimentalZ) {
    /* compute subblock gains */
    int j,b;  FLOAT8 en[3],mx;
    if ((cod_info->block_type==SHORT_TYPE) ) {
      /* estimate energy within each subblock */
      for (b=0; b<3; b++) en[b]=0;
      for ( i=0,j = 0; j < 192; j++ ) {
	for (b=0; b<3; b++) {
	  en[b]+=xr[i] * xr[i];
	  i++;
	}
      }
      mx = 1e-12;
      for (b=0; b<3; b++) mx=Max(mx,en[b]);
      for (b=0; b<3; b++) en[b] = Max(en[b],1e-12)/mx;
      /*printf("ener = %4.2f  %4.2f  %4.2f  \n",en[0],en[1],en[2]);*/
      /* pick gain so that 2^(2gain)*en[0] = 1  */
      /* gain = .5* log( 1/en[0] )/LOG2 = -.5*log(en[])/LOG2 */
      for (b=0; b<3; b++) {
	cod_info->subblock_gain[b] = (int)(-.5*log(en[b])/LOG2 + 0.5);
	if (cod_info->subblock_gain[b] > 2) 
	  cod_info->subblock_gain[b]=2;
	if (cod_info->subblock_gain[b] < 0) 
	  cod_info->subblock_gain[b]=0;
      }
      /*
       *  check if there is some energy we have to quantize
       *  if so, then return 1 else 0
       */
      if (1e-99 < en[0]+en[1]+en[2])
        return 1;
      else
        return 0;
    }
  }
  /*
   *  check if there is some energy we have to quantize
   *  if so, then return 1 else 0
   */
  for (i=0; i<576; i++) 
    if ( 1e-99 < fabs (xr[i]) )
      return 1;
  
  return 0;
}




/************************************************************************/
/*  outer_loop                                                         */
/************************************************************************/
/*  Function: The outer iteration loop controls the masking conditions  */
/*  of all scalefactorbands. It computes the best scalefac and          */
/*  global gain. This module calls the inner iteration loop             
 * 
 *  mt 5/99 completely rewritten to allow for bit reservoir control,   
 *  mid/side channels with L/R or mid/side masking thresholds, 
 *  and chooses best quantization instead of last quantization when 
 *  no distortion free quantization can be found.  
 *  
 *  added VBR support mt 5/99
 ************************************************************************/
void outer_loop(
    lame_global_flags *gfp,
    FLOAT8 xr[576],        
    int targ_bits,
    FLOAT8 best_noise[4],
    III_psy_xmin *l3_xmin,   /* the allowed distortion of the scalefactor */
    int l3_enc[576],         /* vector of quantized values ix(0..575) */
    III_scalefac_t *scalefac, /* scalefactors */
    gr_info *cod_info,
    FLOAT8 xfsf[4][SBPSY_l],
    int ch)
{
  III_scalefac_t scalefac_w;
  gr_info save_cod_info;
  int l3_enc_w[576]; 
  int i, iteration;
  int status,bits_found=0;
  int huff_bits;
  FLOAT8 xrpow[576],temp;
  int better;
  int over=0;
  FLOAT8 max_noise;
  FLOAT8 over_noise;
  FLOAT8 tot_noise;
  int best_over=100;
  FLOAT8 best_max_noise=0;
  FLOAT8 best_over_noise=0;
  FLOAT8 best_tot_noise=0;
  FLOAT8 xfsf_w[4][SBPSY_l];
  FLOAT8 distort[4][SBPSY_l];

  int compute_stepsize=1;
  int notdone=1;

  /* BEGIN MAIN LOOP */
  iteration = 0;
  while ( notdone  ) {
    static int OldValue[2] = {180, 180};
    int try_scale=0;
    iteration ++;

    if (compute_stepsize) {
      /* init and compute initial quantization step */
      compute_stepsize=0;
      /* reset of iteration variables */
      memset(&scalefac_w, 0, sizeof(III_scalefac_t));
      for (i=0;i<576;i++) {
	temp=fabs(xr[i]);
	xrpow[i]=sqrt(sqrt(temp)*temp);
      }
      bits_found=bin_search_StepSize2(gfp,targ_bits,OldValue[ch],
				      l3_enc_w,xrpow,cod_info);
      OldValue[ch] = cod_info->global_gain;
    }


    /* inner_loop starts with the initial quantization step computed above
     * and slowly increases until the bits < huff_bits.
     * Thus it is important not to start with too large of an inital
     * quantization step.  Too small is ok, but inner_loop will take longer 
     */
    huff_bits = targ_bits - cod_info->part2_length;
    if (huff_bits < 0) {
      assert(iteration != 1);
      /* scale factors too large, not enough bits. use previous quantizaton */
      notdone=0;
    } else {
      /* if this is the first iteration, see if we can reuse the quantization
       * computed in bin_search_StepSize above */
      int real_bits;
      if (iteration==1) {
	if(bits_found>huff_bits) {
	  cod_info->global_gain++;
	  real_bits = inner_loop(gfp,xrpow, l3_enc_w, huff_bits, cod_info);
	} else real_bits=bits_found;
      }
      else 
	real_bits=inner_loop(gfp,xrpow, l3_enc_w, huff_bits, cod_info);
      cod_info->part2_3_length = real_bits;

      /* compute the distortion in this quantization */
      if (gfp->noise_shaping==0) {
      	over=0;
      }else{
	/* coefficients and thresholds both l/r (or both mid/side) */
	over=calc_noise1( xr, l3_enc_w, cod_info, 
			  xfsf_w,distort, l3_xmin, &scalefac_w, &over_noise, 
			  &tot_noise, &max_noise);

      }

      /* check if this quantization is better the our saved quantization */
      if (iteration == 1) better=1;
      else 
	better=quant_compare(gfp->experimentalX,
	     best_over,best_tot_noise,best_over_noise,best_max_noise,
                  over,     tot_noise,     over_noise,     max_noise);

      /* save data so we can restore this quantization later */    
      if (better) {
	best_over=over;
	best_max_noise=max_noise;
	best_over_noise=over_noise;
	best_tot_noise=tot_noise;
	
	memcpy(scalefac, &scalefac_w, sizeof(III_scalefac_t));
	memcpy(l3_enc,l3_enc_w,sizeof(int)*576);
	memcpy(&save_cod_info,cod_info,sizeof(save_cod_info));

#ifdef HAVEGTK
	if (gfp->gtkflag) {
	  memcpy(xfsf, xfsf_w, sizeof(xfsf_w));
	}
#endif
      }
    }
    
    /* if no bands with distortion, we are done */
    if (gfp->noise_shaping_stop==0)
      if (over==0) notdone=0;

    if (notdone) {
	amp_scalefac_bands( xrpow, cod_info, &scalefac_w, distort);
	/* check to make sure we have not amplified too much */
	/* loop_break returns 0 if there is an unamplified scalefac */
	/* scale_bitcount returns 0 if no scalefactors are too large */
	if ( (status = loop_break(&scalefac_w, cod_info)) == 0 ) {
	    if ( gfp->version == 1 ) {
		status = scale_bitcount(&scalefac_w, cod_info);
	    }else{
		status = scale_bitcount_lsf(&scalefac_w, cod_info);
	    }
	    if (status && (cod_info->scalefac_scale==0)) try_scale=1; 
	}
	notdone = !status;
    }

    if (try_scale && gfp->experimentalY) {
      init_outer_loop(gfp,xr, cod_info);
      compute_stepsize=1;  /* compute a new global gain */
      notdone=1;
      cod_info->scalefac_scale=1;
    }
  }    /* done with main iteration */

  memcpy(cod_info,&save_cod_info,sizeof(save_cod_info));
  cod_info->part2_3_length += cod_info->part2_length;

  /* finish up */
  assert( cod_info->global_gain < 256 );

  best_noise[0]=best_over;
  best_noise[1]=best_max_noise;
  best_noise[2]=best_over_noise;
  best_noise[3]=best_tot_noise;
}





  










/*************************************************************************/
/*            calc_noise                                                 */
/*************************************************************************/
/*  mt 5/99:  Function: Improved calc_noise for a single channel   */
int calc_noise1( FLOAT8 xr[576], int ix[576], gr_info *cod_info,
		 FLOAT8 xfsf[4][SBPSY_l], FLOAT8 distort[4][SBPSY_l],
		 III_psy_xmin *l3_xmin, III_scalefac_t *scalefac,
		 FLOAT8 *over_noise,
		 FLOAT8 *tot_noise, FLOAT8 *max_noise)
{
    int start, end, l, i, over=0;
	u_int sfb;
    FLOAT8 sum,step,bw;
#ifdef RH_ATH
    FLOAT8 ath_max;
#endif

    int count=0;
    FLOAT8 noise;
    *over_noise=0;
    *tot_noise=0;
    *max_noise = -999;

    for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ ) {
	FLOAT8 step;
	int s = scalefac->l[sfb];

	if (cod_info->preflag)
	    s += pretab[sfb];

	s = cod_info->global_gain - (s << (cod_info->scalefac_scale + 1));
	assert(s<Q_MAX);
	assert(s>=0);
	step = POW20(s);

	start = scalefac_band.l[ sfb ];
        end   = scalefac_band.l[ sfb+1 ];
        bw = end - start;

#ifdef RH_ATH
        ath_max = 0;
#endif
        for ( sum = 0.0, l = start; l < end; l++ )
        {
            FLOAT8 temp;
            temp = fabs(xr[l]) - pow43[ix[l]] * step;
#ifdef MAXNOISE
	    temp = bw*temp*temp;
	    sum = Max(sum,temp);
#elif RH_ATH
	    temp = temp*temp;
            sum += temp;
	    ath_max = Max( ath_max, temp/ATH_mdct_long[l] );
#else
            sum += temp * temp;
#endif
	    
        }
        xfsf[0][sfb] = sum / bw;

	/* max -30db noise below threshold */
#ifdef RH_ATH
	noise = 10*log10(Max(.001,Min(ath_max,xfsf[0][sfb]/l3_xmin->l[sfb])));
#else
	noise = 10*log10(Max(.001,xfsf[0][sfb] / l3_xmin->l[sfb]));
#endif
        distort[0][sfb] = noise;
        if (noise>0) {
	  over++;
	  *over_noise += noise;
	}
	*tot_noise += noise;
	*max_noise=Max(*max_noise,noise);
	count++;

    }


    for ( i = 0; i < 3; i++ ) {
        for ( sfb = cod_info->sfb_smax; sfb < SBPSY_s; sfb++ ) {
	    int s;

	    s = (scalefac->s[sfb][i] << (cod_info->scalefac_scale + 1))
		+ cod_info->subblock_gain[i] * 8;
	    s = cod_info->global_gain - s;

	    assert(s<Q_MAX);
	    assert(s>=0);
	    step = POW20(s);
	    start = scalefac_band.s[ sfb ];
	    end   = scalefac_band.s[ sfb+1 ];
            bw = end - start;
#ifdef RH_ATH
	    ath_max = 0;
#endif
	    for ( sum = 0.0, l = start; l < end; l++ ) {
		FLOAT8 temp;
		temp = fabs(xr[l * 3 + i]) - pow43[ix[l * 3 + i]] * step;
#ifdef MAXNOISE
		temp = bw*temp*temp;
		sum = Max(sum,temp);
#elif RH_ATH
		temp = temp*temp;
		sum += temp;
		ath_max = Max( ath_max, temp/ATH_mdct_short[l] );
#else
		sum += temp * temp;
#endif
            }       
	    xfsf[i+1][sfb] = sum / bw;
	    /* max -30db noise below threshold */
#ifdef RH_ATH
	    noise = 10*log10(Max(.001,Min(ath_max,xfsf[i+1][sfb]/l3_xmin->s[sfb][i])));
#else
	    noise = 10*log10(Max(.001,xfsf[i+1][sfb] / l3_xmin->s[sfb][i] ));
#endif
            distort[i+1][sfb] = noise;
            if (noise > 0) {
		over++;
		*over_noise += noise;
	    }
	    *tot_noise += noise;
	    *max_noise=Max(*max_noise,noise);
	    count++;	    
        }
    }

    if (count>1) *tot_noise /= count;
    if (over>1) *over_noise /= over;

    return over;
}







/*************************************************************************/
/*            amp_scalefac_bands                                         */
/*************************************************************************/

/* 
  Amplify the scalefactor bands that violate the masking threshold.
  See ISO 11172-3 Section C.1.5.4.3.5
*/
void amp_scalefac_bands(FLOAT8 xrpow[576], 
			gr_info *cod_info,
			III_scalefac_t *scalefac,
			FLOAT8 distort[4][SBPSY_l])
{
    int start, end, l,i;
	u_int	sfb;
    FLOAT8 ifqstep34;
    FLOAT8 distort_thresh;

    if ( cod_info->scalefac_scale == 0 )
	ifqstep34 = 1.29683955465100964055;
    else
	ifqstep34 = 1.68179283050742922612;

    /* distort_thresh = 0, unless all bands have distortion 
     * less than masking.  In that case, just amplify bands with distortion
     * within 95% of largest distortion/masking ratio */
    distort_thresh = -900;
    for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ ) {
	distort_thresh = Max(distort[0][sfb],distort_thresh);
    }

    for ( sfb = cod_info->sfb_smax; sfb < 12; sfb++ ) {
	for ( i = 0; i < 3; i++ ) {
	    distort_thresh = Max(distort[i+1][sfb],distort_thresh);
	}
    }
    distort_thresh=Min(distort_thresh * 1.05, 0.0);



    for ( sfb = 0; sfb < cod_info->sfb_lmax; sfb++ ) {
	if ( distort[0][sfb]>distort_thresh  ) {
	    scalefac->l[sfb]++;
	    start = scalefac_band.l[sfb];
	    end   = scalefac_band.l[sfb+1];
	    for ( l = start; l < end; l++ )
		xrpow[l] *= ifqstep34;
	}
    }


    for ( i = 0; i < 3; i++ ) {
	for ( sfb = cod_info->sfb_smax; sfb < 12; sfb++ ) {
            if ( distort[i+1][sfb]>distort_thresh) {
                scalefac->s[sfb][i]++;
                start = scalefac_band.s[sfb];
                end   = scalefac_band.s[sfb+1];
		for (l = start; l < end; l++)
		    xrpow[l * 3 + i] *= ifqstep34;
            }
	}
    }
}



int quant_compare(int experimentalX,
int best_over,FLOAT8 best_tot_noise,FLOAT8 best_over_noise,FLOAT8 best_max_noise,
int over,FLOAT8 tot_noise, FLOAT8 over_noise, FLOAT8 max_noise)
{
  /*
    noise is given in decibals (db) relative to masking thesholds.

    over_noise:  sum of quantization noise > masking
    tot_noise:   sum of all quantization noise
    max_noise:   max quantization noise 

   */
  int better=0;

  if (experimentalX==0) {
    better = ((over < best_over) ||
	      ((over==best_over) && (over_noise<=best_over_noise)) ) ;
  }

  if (experimentalX==1) 
    better = max_noise < best_max_noise;

  if (experimentalX==2) {
    better = tot_noise < best_tot_noise;
  }
  if (experimentalX==3) {
    better = (tot_noise < best_tot_noise) &&
      (max_noise < best_max_noise + 2);
  }
  if (experimentalX==4) {
    better = ( ( (0>=max_noise) && (best_max_noise>2)) ||
     ( (0>=max_noise) && (best_max_noise<0) && ((best_max_noise+2)>max_noise) && (tot_noise<best_tot_noise) ) ||
     ( (0>=max_noise) && (best_max_noise>0) && ((best_max_noise+2)>max_noise) && (tot_noise<(best_tot_noise+best_over_noise)) ) ||
     ( (0<max_noise) && (best_max_noise>-0.5) && ((best_max_noise+1)>max_noise) && ((tot_noise+over_noise)<(best_tot_noise+best_over_noise)) ) ||
     ( (0<max_noise) && (best_max_noise>-1) && ((best_max_noise+1.5)>max_noise) && ((tot_noise+over_noise+over_noise)<(best_tot_noise+best_over_noise+best_over_noise)) ) );
  }
  if (experimentalX==5) {
    better =   (over_noise <  best_over_noise)
      || ((over_noise == best_over_noise)&&(tot_noise < best_tot_noise));
  }
  if (experimentalX==6) {
    better = (over_noise < best_over_noise)
           ||( (over_noise == best_over_noise)
             &&( (max_noise < best_max_noise)
               ||( (max_noise == best_max_noise)
                 &&(tot_noise <= best_tot_noise)
                 )
               ) 
	     );
  }

  return better;
}


int VBR_compare(
int best_over,FLOAT8 best_tot_noise,FLOAT8 best_over_noise,FLOAT8 best_max_noise,
int over,FLOAT8 tot_noise, FLOAT8 over_noise, FLOAT8 max_noise)
{
  /*
    noise is given in decibals (db) relative to masking thesholds.

    over_noise:  sum of quantization noise > masking
    tot_noise:   sum of all quantization noise
    max_noise:   max quantization noise 

   */
  int better=0;

  better = ((over <= best_over) &&
	    (over_noise<=best_over_noise) &&
	    (tot_noise<=best_tot_noise) &&
	    (max_noise<=best_max_noise));
  return better;
}
  









/* ==== reservoir.c ==== */
/**********************************************************************
 * ISO MPEG Audio Subgroup Software Simulation Group (1996)
 * ISO 13818-3 MPEG-2 Audio Encoder - Lower Sampling Frequency Extension
 *
 **********************************************************************/
/*
  Revision History:

  Date        Programmer                Comment
  ==========  ========================= ===============================
  1995/09/06  mc@fivebats.com           created

*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "util.h"
#ifdef HAVEGTK
#include "gtkanal.h"
#endif


/*
  Layer3 bit reservoir:
  Described in C.1.5.4.2.2 of the IS
*/

static int ResvSize = 0; /* in bits */
static int ResvMax  = 0; /* in bits */

/*
  ResvFrameBegin:
  Called (repeatedly) at the beginning of a frame. Updates the maximum
  size of the reservoir, and checks to make sure main_data_begin
  was set properly by the formatter
*/
int
ResvFrameBegin(lame_global_flags *gfp,III_side_info_t *l3_side, int mean_bits, int frameLength )
{
    int fullFrameBits;
    int resvLimit;

    if (gfp->frameNum==0) {
      ResvSize=0;
    }


    if ( gfp->version == 1 )
    {
	resvLimit = 4088; /* main_data_begin has 9 bits in MPEG 1 */
    }
    else
    {
	resvLimit = 2040; /* main_data_begin has 8 bits in MPEG 2 */
    }

    /*
      main_data_begin was set by the formatter to the
      expected value for the next call -- this should
      agree with our reservoir size
    */

#ifdef DEBUG
    fprintf( stderr, ">>> ResvSize = %d\n", ResvSize );
#endif
    /* check expected resvsize */
    assert( (l3_side->main_data_begin * 8) == ResvSize );
    fullFrameBits = mean_bits * gfp->mode_gr + ResvSize;

    /*
      determine maximum size of reservoir:
      ResvMax + frameLength <= 7680;
    */
    if ( frameLength > 7680 )
	ResvMax = 0;
    else
	ResvMax = 7680 - frameLength;
    if (gfp->disable_reservoir) ResvMax=0;


    /*
      limit max size to resvLimit bits because
      main_data_begin cannot indicate a
      larger value
      */
    if ( ResvMax > resvLimit )
	ResvMax = resvLimit;

#ifdef HAVEGTK
  if (gfp->gtkflag){
    pinfo->mean_bits=mean_bits/2;  /* expected bits per channel per granule */
    pinfo->resvsize=ResvSize;
  }
#endif

    return fullFrameBits;
}


/*
  ResvMaxBits2:
  As above, but now it *really* is bits per granule (both channels).  
  Mark Taylor 4/99
*/
void ResvMaxBits(int mean_bits, int *targ_bits, int *extra_bits, int gr)
{
  int add_bits;
  *targ_bits = mean_bits ;
  /* extra bits if the reservoir is almost full */
  if (ResvSize > ((ResvMax * 9) / 10)) {
    add_bits= ResvSize-((ResvMax * 9) / 10);
    *targ_bits += add_bits;
  }else {
    add_bits =0 ;
    /* build up reservoir.  this builds the reservoir a little slower
     * than FhG.  It could simple be mean_bits/15, but this was rigged
     * to always produce 100 (the old value) at 128kbs */
    *targ_bits -= (int) (mean_bits/15.2);
  }

  
  /* amount from the reservoir we are allowed to use. ISO says 6/10 */
  *extra_bits =    
    (ResvSize  < (ResvMax*6)/10  ? ResvSize : (ResvMax*6)/10);
  *extra_bits -= add_bits;
  
  if (*extra_bits < 0) *extra_bits=0;

  
}

/*
  ResvAdjust:
  Called after a granule's bit allocation. Readjusts the size of
  the reservoir to reflect the granule's usage.
*/
void
ResvAdjust(lame_global_flags *gfp,gr_info *gi, III_side_info_t *l3_side, int mean_bits )
{
    ResvSize += (mean_bits / gfp->stereo) - gi->part2_3_length;
}


/*
  ResvFrameEnd:
  Called after all granules in a frame have been allocated. Makes sure
  that the reservoir size is within limits, possibly by adding stuffing
  bits. Note that stuffing bits are added by increasing a granule's
  part2_3_length. The bitstream formatter will detect this and write the
  appropriate stuffing bits to the bitstream.
*/
void
ResvFrameEnd(lame_global_flags *gfp,III_side_info_t *l3_side, int mean_bits)
{
    int stuffingBits;
    int over_bits;

    /* just in case mean_bits is odd, this is necessary... */
    if ( gfp->stereo == 2 && mean_bits & 1)
	ResvSize += 1;

    over_bits = ResvSize - ResvMax;
    if ( over_bits < 0 )
	over_bits = 0;
    
    ResvSize -= over_bits;
    stuffingBits = over_bits;

    /* we must be byte aligned */
    if ( (over_bits = ResvSize % 8) )
    {
	stuffingBits += over_bits;
	ResvSize -= over_bits;
    }


    l3_side->resvDrain = stuffingBits;
    return;

}




/* ==== tabinit.c ==== */
#ifdef HAVEMPGLIB
#include <stdlib.h>

#include "mpg123.h"

real decwin[512+32];
static real cos64[16],cos32[8],cos16[4],cos8[2],cos4[1];
real *pnts[] = { cos64,cos32,cos16,cos8,cos4 };

#if 0
static unsigned char *conv16to8_buf = NULL;
unsigned char *conv16to8;
#endif

static long intwinbase[] = {
     0,    -1,    -1,    -1,    -1,    -1,    -1,    -2,    -2,    -2,
    -2,    -3,    -3,    -4,    -4,    -5,    -5,    -6,    -7,    -7,
    -8,    -9,   -10,   -11,   -13,   -14,   -16,   -17,   -19,   -21,
   -24,   -26,   -29,   -31,   -35,   -38,   -41,   -45,   -49,   -53,
   -58,   -63,   -68,   -73,   -79,   -85,   -91,   -97,  -104,  -111,
  -117,  -125,  -132,  -139,  -147,  -154,  -161,  -169,  -176,  -183,
  -190,  -196,  -202,  -208,  -213,  -218,  -222,  -225,  -227,  -228,
  -228,  -227,  -224,  -221,  -215,  -208,  -200,  -189,  -177,  -163,
  -146,  -127,  -106,   -83,   -57,   -29,     2,    36,    72,   111,
   153,   197,   244,   294,   347,   401,   459,   519,   581,   645,
   711,   779,   848,   919,   991,  1064,  1137,  1210,  1283,  1356,
  1428,  1498,  1567,  1634,  1698,  1759,  1817,  1870,  1919,  1962,
  2001,  2032,  2057,  2075,  2085,  2087,  2080,  2063,  2037,  2000,
  1952,  1893,  1822,  1739,  1644,  1535,  1414,  1280,  1131,   970,
   794,   605,   402,   185,   -45,  -288,  -545,  -814, -1095, -1388,
 -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
 -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209,
 -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
 -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092,
 -7640, -7134, -6574, -5959, -5288, -4561, -3776, -2935, -2037, -1082,
   -70,   998,  2122,  3300,  4533,  5818,  7154,  8540,  9975, 11455,
 12980, 14548, 16155, 17799, 19478, 21189, 22929, 24694, 26482, 28289,
 30112, 31947, 33791, 35640, 37489, 39336, 41176, 43006, 44821, 46617,
 48390, 50137, 51853, 53534, 55178, 56778, 58333, 59838, 61289, 62684,
 64019, 65290, 66494, 67629, 68692, 69679, 70590, 71420, 72169, 72835,
 73415, 73908, 74313, 74630, 74856, 74992, 75038 };

void make_decode_tables(long scaleval)
{
  int i,j,k,kr,divv;
  real *table,*costab;

  
  for(i=0;i<5;i++)
  {
    kr=0x10>>i; divv=0x40>>i;
    costab = pnts[i];
    for(k=0;k<kr;k++)
      costab[k] = 1.0 / (2.0 * cos(M_PI * ((double) k * 2.0 + 1.0) / (double) divv));
  }

  table = decwin;
  scaleval = -scaleval;
  for(i=0,j=0;i<256;i++,j++,table+=32)
  {
    if(table < decwin+512+16)
      table[16] = table[0] = (double) intwinbase[j] / 65536.0 * (double) scaleval;
    if(i % 32 == 31)
      table -= 1023;
    if(i % 64 == 63)
      scaleval = - scaleval;
  }

  for( /* i=256 */ ;i<512;i++,j--,table+=32)
  {
    if(table < decwin+512+16)
      table[16] = table[0] = (double) intwinbase[j] / 65536.0 * (double) scaleval;
    if(i % 32 == 31)
      table -= 1023;
    if(i % 64 == 63)
      scaleval = - scaleval;
  }
}



#endif


/* ==== tables.c ==== */
#include "util.h"
#include "tables.h"

/*
  Here are MPEG1 Table B.8 and MPEG2 Table B.1
  -- Layer III scalefactor bands. 
  Index into this using a method such as:
    idx  = fr_ps->header->sampling_frequency
           + (fr_ps->header->version * 3)
*/




unsigned int hs = sizeof(HUFFBITS)*8;

static HUFFBITS      t1HB[]   = {
  1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 1, 0}; 

static HUFFBITS      t2HB[]   = {
  1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 0};

static HUFFBITS      t3HB[]   = {
  3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 0};

static HUFFBITS      t5HB[]   = {
  1, 2, 6, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 1, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 5, 7, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 1, 1, 0};

static HUFFBITS      t6HB[]   = {
  7, 3, 5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 2, 3, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 2, 0};

static HUFFBITS      t7HB[]   = {
   1, 2,10,19,16,10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   3, 3, 7,10, 5, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  11, 4,13,17, 8, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  12,11,18,15,11, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   7, 6, 9,14, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   6, 4, 5, 3, 2, 0};

static HUFFBITS      t8HB[]   = {
  3, 4, 6, 18,12, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 1, 2, 16, 9, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 3, 5, 14, 7, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 19,17,15, 13,10, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 13, 5, 8, 11, 5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 12, 4, 4,  1, 1, 0};

static HUFFBITS      t9HB[]   = {
  7, 5, 9, 14, 15, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 4, 5,  5,  6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 6, 8,  8,  8, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 15, 6, 9, 10,  5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 11, 7, 9,  6,  4, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 14, 4, 6,  2,  6, 0};

static HUFFBITS      t10HB[]   = {
  1, 2, 10, 23, 35, 30, 12, 17, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3,  8, 12, 18, 21, 12,  7, 0, 0, 0, 0, 0, 0, 0, 0,
 11, 9, 15, 21, 32, 40, 19,  6, 0, 0, 0, 0, 0, 0, 0, 0,
 14,13, 22, 34, 46, 23, 18,  7, 0, 0, 0, 0, 0, 0, 0, 0,
 20,19, 33, 47, 27, 22,  9,  3, 0, 0, 0, 0, 0, 0, 0, 0,
 31,22, 41, 26, 21, 20,  5,  3, 0, 0, 0, 0, 0, 0, 0, 0,
 14,13, 10, 11, 16,  6,  5,  1, 0, 0, 0, 0, 0, 0, 0, 0,
  9, 8,  7,  8,  4,  4,  2,  0};

static HUFFBITS      t11HB[]   = {
  3, 4, 10, 24, 34, 33, 21, 15, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 3,  4, 10, 32, 17, 11, 10, 0, 0, 0, 0, 0, 0, 0, 0,
 11, 7, 13, 18, 30, 31, 20,  5, 0, 0, 0, 0, 0, 0, 0, 0,
 25,11, 19, 59, 27, 18, 12,  5, 0, 0, 0, 0, 0, 0, 0, 0,
 35,33, 31, 58, 30, 16,  7,  5, 0, 0, 0, 0, 0, 0, 0, 0,
 28,26, 32, 19, 17, 15,  8, 14, 0, 0, 0, 0, 0, 0, 0, 0,
 14,12,  9, 13, 14,  9,  4,  1, 0, 0, 0, 0, 0, 0, 0, 0,
 11, 4,  6,  6,  6,  3,  2,  0};

static HUFFBITS      t12HB[]   = {
  9,  6, 16, 33, 41, 39, 38,26, 0, 0, 0, 0, 0, 0, 0, 0,
  7,  5,  6,  9, 23, 16, 26,11, 0, 0, 0, 0, 0, 0, 0, 0,
 17,  7, 11, 14, 21, 30, 10, 7, 0, 0, 0, 0, 0, 0, 0, 0,
 17, 10, 15, 12, 18, 28, 14, 5, 0, 0, 0, 0, 0, 0, 0, 0,
 32, 13, 22, 19, 18, 16,  9, 5, 0, 0, 0, 0, 0, 0, 0, 0,
 40, 17, 31, 29, 17, 13,  4, 2, 0, 0, 0, 0, 0, 0, 0, 0,
 27, 12, 11, 15, 10,  7,  4, 1, 0, 0, 0, 0, 0, 0, 0, 0,
 27, 12,  8, 12,  6,  3,  1, 0};

static HUFFBITS      t13HB[]   = {
  1,  5, 14, 21, 34, 51, 46, 71, 42, 52, 68, 52, 67, 44, 43, 19,
  3,  4, 12, 19, 31, 26, 44, 33, 31, 24, 32, 24, 31, 35, 22, 14,
 15, 13, 23, 36, 59, 49, 77, 65, 29, 40, 30, 40, 27, 33, 42, 16,
 22, 20, 37, 61, 56, 79, 73, 64, 43, 76, 56, 37, 26, 31, 25, 14,
 35, 16, 60, 57, 97, 75,114, 91, 54, 73, 55, 41, 48, 53, 23, 24,
 58, 27, 50, 96, 76, 70, 93, 84, 77, 58, 79, 29, 74, 49, 41, 17,
 47, 45, 78, 74,115, 94, 90, 79, 69, 83, 71, 50, 59, 38, 36, 15,
 72, 34, 56, 95, 92, 85, 91, 90, 86, 73, 77, 65, 51, 44, 43, 42,
 43, 20, 30, 44, 55, 78, 72, 87, 78, 61, 46, 54, 37, 30, 20, 16,
 53, 25, 41, 37, 44, 59, 54, 81, 66, 76, 57, 54, 37, 18, 39, 11,
 35, 33, 31, 57, 42, 82, 72, 80, 47, 58, 55, 21, 22, 26, 38, 22,
 53, 25, 23, 38, 70, 60, 51, 36, 55, 26, 34, 23, 27, 14,  9,  7,
 34, 32, 28, 39, 49, 75, 30, 52, 48, 40, 52, 28, 18, 17,  9,  5,
 45, 21, 34, 64, 56, 50, 49, 45, 31, 19, 12, 15, 10,  7,  6,  3,
 48, 23, 20, 39, 36, 35, 53, 21, 16, 23, 13, 10,  6,  1,  4,  2,
 16, 15, 17, 27, 25, 20, 29, 11, 17, 12, 16,  8,  1,  1,  0,  1};

static HUFFBITS      t15HB[]   = {
   7, 12, 18, 53, 47, 76,124,108, 89,123,108,119,107, 81,122, 63,
  13,  5, 16, 27, 46, 36, 61, 51, 42, 70, 52, 83, 65, 41, 59, 36,
  19, 17, 15, 24, 41, 34, 59, 48, 40, 64, 50, 78, 62, 80, 56, 33,
  29, 28, 25, 43, 39, 63, 55, 93, 76, 59, 93, 72, 54, 75, 50, 29,
  52, 22, 42, 40, 67, 57, 95, 79, 72, 57, 89, 69, 49, 66, 46, 27,
  77, 37, 35, 66, 58, 52, 91, 74, 62, 48, 79, 63, 90, 62, 40, 38,
 125, 32, 60, 56, 50, 92, 78, 65, 55, 87, 71, 51, 73, 51, 70, 30,
 109, 53, 49, 94, 88, 75, 66,122, 91, 73, 56, 42, 64, 44, 21, 25,
  90, 43, 41, 77, 73, 63, 56, 92, 77, 66, 47, 67, 48, 53, 36, 20,
  71, 34, 67, 60, 58, 49, 88, 76, 67,106, 71, 54, 38, 39, 23, 15,
 109, 53, 51, 47, 90, 82, 58, 57, 48, 72, 57, 41, 23, 27, 62,  9,
  86, 42, 40, 37, 70, 64, 52, 43, 70, 55, 42, 25, 29, 18, 11, 11, 
 118, 68, 30, 55, 50, 46, 74, 65, 49, 39, 24, 16, 22, 13, 14,  7,
  91, 44, 39, 38, 34, 63, 52, 45, 31, 52, 28, 19, 14,  8,  9,  3,
 123, 60, 58, 53, 47, 43, 32, 22, 37, 24, 17, 12, 15, 10,  2,  1,
  71, 37, 34, 30, 28, 20, 17, 26, 21, 16, 10,  6,  8,  6,  2,  0};

static HUFFBITS      t16HB[]   = {
   1,   5, 14, 44, 74, 63, 110, 93, 172, 149, 138, 242, 225, 195, 376, 17,
   3,   4, 12, 20, 35, 62,  53, 47,  83,  75,  68, 119, 201, 107, 207,  9,
  15,  13, 23, 38, 67, 58, 103, 90, 161,  72, 127, 117, 110, 209, 206, 16,
  45,  21, 39, 69, 64,114,  99, 87, 158, 140, 252, 212, 199, 387, 365, 26,
  75,  36, 68, 65,115,101, 179,164, 155, 264, 246, 226, 395, 382, 362,  9,
  66,  30, 59, 56,102,185, 173,265, 142, 253, 232, 400, 388, 378, 445, 16,
 111,  54, 52,100,184,178, 160,133, 257, 244, 228, 217, 385, 366, 715, 10,
  98,  48, 91, 88,165,157, 148,261, 248, 407, 397, 372, 380, 889, 884,  8,
  85,  84, 81,159,156,143, 260,249, 427, 401, 392, 383, 727, 713, 708,  7,
 154,  76, 73,141,131,256, 245,426, 406, 394, 384, 735, 359, 710, 352, 11,
 139, 129, 67,125,247,233, 229,219, 393, 743, 737, 720, 885, 882, 439,  4,
 243, 120,118,115,227,223, 396,746, 742, 736, 721, 712, 706, 223, 436,  6,
 202, 224,222,218,216,389, 386,381, 364, 888, 443, 707, 440, 437,1728,  4,
 747, 211,210,208,370, 379,734,723, 714,1735, 883, 877, 876,3459, 865,  2,
 377, 369,102,187, 726,722,358,711, 709, 866,1734, 871,3458, 870, 434,  0,
  12,  10,  7, 11,  10, 17, 11,  9,  13,  12,  10,   7,   5,   3,   1,  3};

static HUFFBITS      t24HB[]   = {
   15, 13, 46, 80, 146, 262, 248, 434, 426, 669, 653, 649, 621, 517, 1032, 88,
   14, 12, 21, 38,  71, 130, 122, 216, 209, 198, 327, 345, 319, 297,  279, 42,
   47, 22, 41, 74,  68, 128, 120, 221, 207, 194, 182, 340, 315, 295,  541, 18,
   81, 39, 75, 70, 134, 125, 116, 220, 204, 190, 178, 325, 311, 293,  271, 16,
  147, 72, 69,135, 127, 118, 112, 210, 200, 188, 352, 323, 306, 285,  540, 14,
  263, 66,129,126, 119, 114, 214, 202, 192, 180, 341, 317, 301, 281,  262, 12,
  249,123,121,117, 113, 215, 206, 195, 185, 347, 330, 308, 291, 272,  520, 10,
  435,115,111,109, 211, 203, 196, 187, 353, 332, 313, 298, 283, 531,  381, 17,
  427,212,208,205, 201, 193, 186, 177, 169, 320, 303, 286, 268, 514,  377, 16,
  335,199,197,191, 189, 181, 174, 333, 321, 305, 289, 275, 521, 379,  371, 11,
  668,184,183,179, 175, 344, 331, 314, 304, 290, 277, 530, 383, 373,  366, 10,
  652,346,171,168, 164, 318, 309, 299, 287, 276, 263, 513, 375, 368,  362,  6,
  648,322,316,312, 307, 302, 292, 284, 269, 261, 512, 376, 370, 364,  359,  4,
  620,300,296,294, 288, 282, 273, 266, 515, 380, 374, 369, 365, 361,  357,  2,
 1033,280,278,274, 267, 264, 259, 382, 378, 372, 367, 363, 360, 358,  356,  0,
   43, 20, 19, 17,  15,  13,  11,   9,   7,   6,   4,   7,   5,   3,    1,  3};

static HUFFBITS      t32HB[]   = {
  1, 5, 4, 5, 6, 5, 4, 4, 7, 3, 6, 0, 7, 2, 3, 1};
static HUFFBITS      t33HB[]   = {
  15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

static unsigned char t1l[]  = {
  1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  2, 3};

static unsigned char t2l[]  = {
  1, 3, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 5, 6};

static unsigned char t3l[]  = {
  2, 2, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 5, 6};

static unsigned char t5l[]  = {
  1, 3, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 6, 7, 8};

static unsigned char t6l[]  = {
  3, 3, 5, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  4, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 5, 6, 7};

static unsigned char t7l[]  = {
  1, 3, 6, 8, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 4, 6, 7, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 5, 7, 8, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 7, 8, 9, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 7, 8, 9, 9,10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 9,10,10,10};

static unsigned char t8l[]  = {
  2, 3, 6, 8, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 4, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 4, 6, 8, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 8, 9, 9,10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 8, 9,10,10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  9, 8, 9, 9,11,11};

static unsigned char t9l[]  = {
  3, 3, 5, 6, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 4, 5, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  4, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 5, 6, 7, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 6, 7, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 8, 8, 9, 9};

static unsigned char t10l[]  = {
  1, 3, 6, 8, 9, 9, 9, 10, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 4, 6, 7, 8, 9, 8,  8, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 6, 7, 8, 9,10, 9,  9, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 7, 8, 9,10,10, 9, 10, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 9,10,10,10,10, 10, 0, 0, 0, 0, 0, 0, 0, 0,
  9, 9,10,10,11,11,10, 11, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 9,10,10,10,11, 11, 0, 0, 0, 0, 0, 0, 0, 0,
  9, 8, 9,10,10,11,11, 11};

static unsigned char t11l[]  = {
  2, 3, 5, 7, 8,  9,  8,  9, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 4, 6, 8,  8,  7,  8, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 5, 6, 7, 8,  9,  8,  8, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 6, 7, 9, 8, 10,  8,  9, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 8, 9, 9, 10,  9, 10, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 8, 9,10,10, 11, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 7, 8, 9, 10, 10, 10, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 8, 9,10, 10, 10, 10};

static unsigned char t12l[]  = {
  4, 3, 5, 7, 8, 9, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 4, 5, 7, 7, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 4, 5, 6, 7, 8, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0,
  6, 5, 6, 6, 7, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0,
  7, 6, 7, 7, 8, 8, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 8, 8, 8, 9, 8, 9, 0, 0, 0, 0, 0, 0, 0, 0,
  8, 7, 7, 8, 8, 9, 9,10, 0, 0, 0, 0, 0, 0, 0, 0,
  9, 8, 8, 9, 9, 9, 9,10};

static unsigned char t13l[]  = {
  1,  4,  6,  7,  8,  9,  9, 10,  9, 10, 11, 11, 12, 12, 13, 13,
  3,  4,  6,  7,  8,  8,  9,  9,  9,  9, 10, 10, 11, 12, 12, 12,
  6,  6,  7,  8,  9,  9, 10, 10,  9, 10, 10, 11, 11, 12, 13, 13,
  7,  7,  8,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 13,
  8,  7,  9,  9, 10, 10, 11, 11, 10, 11, 11, 12, 12, 13, 13, 14,
  9,  8,  9, 10, 10, 10, 11, 11, 11, 11, 12, 11, 13, 13, 14, 14,
  9,  9, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14,
 10,  9, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 16, 16,
  9,  8,  9, 10, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 15, 15,
 10,  9, 10, 10, 11, 11, 11, 13, 12, 13, 13, 14, 14, 14, 16, 15,
 10, 10, 10, 11, 11, 12, 12, 13, 12, 13, 14, 13, 14, 15, 16, 17,
 11, 10, 10, 11, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 15, 16,
 11, 11, 11, 12, 12, 13, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16,
 12, 11, 12, 13, 13, 13, 14, 14, 14, 14, 14, 15, 16, 15, 16, 16,
 13, 12, 12, 13, 13, 13, 15, 14, 14, 17, 15, 15, 15, 17, 16, 16,
 12, 12, 13, 14, 14, 14, 15, 14, 15, 15, 16, 16, 19, 18, 19, 16}; 

static unsigned char t15l[]  = {
  3,  4,  5,  7,  7,  8,  9,  9,  9, 10, 10, 11, 11, 11, 12, 13,
  4,  3,  5,  6,  7,  7,  8,  8,  8,  9,  9, 10, 10, 10, 11, 11,
  5,  5,  5,  6,  7,  7,  8,  8,  8,  9,  9, 10, 10, 11, 11, 11,
  6,  6,  6,  7,  7,  8,  8,  9,  9,  9, 10, 10, 10, 11, 11, 11,
  7,  6,  7,  7,  8,  8,  9,  9,  9,  9, 10, 10, 10, 11, 11, 11,
  8,  7,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 11, 11, 11, 12,
  9,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 11, 11, 12, 12,
  9,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11, 11, 11, 12,
  9,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11, 12, 12, 12,
  9,  8,  9,  9,  9,  9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12,
 10,  9,  9,  9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 12,
 10,  9,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13,
 11, 10,  9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 13, 13,
 11, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13,
 12, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 12, 13,
 12, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13};

static unsigned char t16l[]  = {
  1,  4,  6,  8,  9,  9, 10, 10, 11, 11, 11, 12, 12, 12, 13,  9,
  3,  4,  6,  7,  8,  9,  9,  9, 10, 10, 10, 11, 12, 11, 12,  8,
  6,  6,  7,  8,  9,  9, 10, 10, 11, 10, 11, 11, 11, 12, 12,  9,
  8,  7,  8,  9,  9, 10, 10, 10, 11, 11, 12, 12, 12, 13, 13, 10,
  9,  8,  9,  9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13,  9,
  9,  8,  9,  9, 10, 11, 11, 12, 11, 12, 12, 13, 13, 13, 14, 10,
 10,  9,  9, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 14, 10,
 10,  9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 15, 15, 10,
 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 10,
 11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 13, 14, 13, 11,
 11, 11, 10, 11, 12, 12, 12, 12, 13, 14, 14, 14, 15, 15, 14, 10,
 12, 11, 11, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 13, 14, 11,
 12, 12, 12, 12, 12, 13, 13, 13, 13, 15, 14, 14, 14, 14, 16, 11,
 14, 12, 12, 12, 13, 13, 14, 14, 14, 16, 15, 15, 15, 17, 15, 11,
 13, 13, 11, 12, 14, 14, 13, 14, 14, 15, 16, 15, 17, 15, 14, 11,
  9,  8,  8,  9,  9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8};

static unsigned char t24l[]  = {
  4,  4,  6,  7,  8,  9,  9, 10, 10, 11, 11, 11, 11, 11, 12,  9,
  4,  4,  5,  6,  7,  8,  8,  9,  9,  9, 10, 10, 10, 10, 10,  8,
  6,  5,  6,  7,  7,  8,  8,  9,  9,  9,  9, 10, 10, 10, 11,  7,
  7,  6,  7,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10,  7,
  8,  7,  7,  8,  8,  8,  8,  9,  9,  9, 10, 10, 10, 10, 11,  7,
  9,  7,  8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10,  7,
  9,  8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11,  7,
 10,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11, 11,  8,
 10,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11,  8,
 10,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11, 11, 11,  8,
 11,  9,  9,  9,  9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11,  8,
 11, 10,  9,  9,  9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11,  8,
 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11,  8,
 11, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11,  8,
 12, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11,  8,
  8,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  4};

static unsigned char t32l[]  = {
  1, 4, 4, 5, 4, 6, 5, 6, 4, 5, 5, 6, 5, 6, 6, 6};
static unsigned char t33l[]  = {
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

struct huffcodetab ht[HTN] =
{
  /* xlen, linmax, table, hlen */
  { 0,   0,NULL,NULL},
  { 2,   0,t1HB, t1l},
  { 3,   0,t2HB, t2l},
  { 3,   0,t3HB, t3l},
  { 0,   0,NULL,NULL},/* Apparently not used */
  { 4,   0,t5HB, t5l},
  { 4,   0,t6HB, t6l},
  { 6,   0,t7HB, t7l},
  { 6,   0,t8HB, t8l},
  { 6,   0,t9HB, t9l},
  { 8,   0,t10HB, t10l},
  { 8,   0,t11HB, t11l},
  { 8,   0,t12HB, t12l},
  {16,   0,t13HB, t13l},
  { 0,   0,NULL,NULL},/* Apparently not used */
  {16,   0,t15HB, t15l},

  { 1,   1,t16HB, t16l},
  { 2,   3,t16HB, t16l},
  { 3,   7,t16HB, t16l},
  { 4,  15,t16HB, t16l},
  { 6,  63,t16HB, t16l},
  { 8, 255,t16HB, t16l},
  {10,1023,t16HB, t16l},
  {13,8191,t16HB, t16l},

  { 4,  15,t24HB, t24l},
  { 5,  31,t24HB, t24l},
  { 6,  63,t24HB, t24l},
  { 7, 127,t24HB, t24l},
  { 8, 255,t24HB, t24l},
  { 9, 511,t24HB, t24l},
  {11,2047,t24HB, t24l},
  {13,8191,t24HB, t24l},

  { 0,   0,t32HB, t32l},
  { 0,   0,t33HB, t33l},
};	





FLOAT8 psy_data[] =
{48000.0, 61,
 0, 1, 24.5, 4.532, 0.970, 0.000,
 1, 1, 24.5, 4.532, 0.755, 0.469,
 2, 1, 24.5, 4.532, 0.738, 0.938,
 3, 1, 24.5, 0.904, 0.730, 1.406,
 4, 1, 24.5, 0.904, 0.724, 1.875,
 5, 1, 20.0, 0.090, 0.723, 2.344,
 6, 1, 20.0, 0.090, 0.723, 2.813,
 7, 1, 20.0, 0.029, 0.723, 3.281,
 8, 1, 20.0, 0.029, 0.718, 3.750,
 9, 1, 20.0, 0.009, 0.690, 4.199,
 10, 1, 20.0, 0.009, 0.660, 4.625,
 11, 1, 18.0, 0.009, 0.641, 5.047,
 12, 1, 18.0, 0.009, 0.600, 5.438,
 13, 1, 18.0, 0.009, 0.584, 5.828,
 14, 1, 12.0, 0.009, 0.532, 6.188,
 15, 1, 12.0, 0.009, 0.537, 6.522,
 16, 2, 6.0, 0.018, 0.857, 7.174,
 17, 2, 6.0, 0.018, 0.858, 7.801,
 18, 2, 3.0, 0.018, 0.853, 8.402,
 19, 2, 3.0, 0.018, 0.824, 8.966,
 20, 2, 3.0, 0.018, 0.778, 9.484,
 21, 2, 3.0, 0.018, 0.740, 9.966,
 22, 2, 0.0, 0.018, 0.709, 10.426,
 23, 2, 0.0, 0.018, 0.676, 10.866,
 24, 2, 0.0, 0.018, 0.632, 11.279,
 25, 2, 0.0, 0.018, 0.592, 11.669,
 26, 2, 0.0, 0.018, 0.553, 12.042,
 27, 2, 0.0, 0.018, 0.510, 12.386,
 28, 2, 0.0, 0.018, 0.513, 12.721,
 29, 3, 0.0, 0.027, 0.608, 13.115,
 30, 3, 0.0, 0.027, 0.673, 13.562,
 31, 3, 0.0, 0.027, 0.637, 13.984,
 32, 3, 0.0, 0.027, 0.586, 14.371,
 33, 3, 0.0, 0.027, 0.571, 14.741,
 34, 4, 0.0, 0.036, 0.616, 15.140,
 35, 4, 0.0, 0.036, 0.640, 15.563,
 36, 4, 0.0, 0.036, 0.598, 15.962,
 37, 4, 0.0, 0.036, 0.538, 16.324,
 38, 4, 0.0, 0.036, 0.512, 16.665,
 39, 5, 0.0, 0.045, 0.528, 17.020,
 40, 5, 0.0, 0.045, 0.517, 17.373,
 41, 5, 0.0, 0.045, 0.493, 17.708,
 42, 6, 0.0, 0.054, 0.499, 18.045,
 43, 7, 0.0, 0.063, 0.525, 18.398,
 44, 7, 0.0, 0.063, 0.541, 18.762,
 45, 8, 0.0, 0.072, 0.528, 19.120,
 46, 8, 0.0, 0.072, 0.510, 19.466,
 47, 8, 0.0, 0.072, 0.506, 19.807,
 48, 10, 0.0, 0.180, 0.525, 20.159,
 49, 10, 0.0, 0.180, 0.536, 20.522,
 50, 10, 0.0, 0.180, 0.518, 20.874,
 51, 13, 0.0, 0.372, 0.501, 21.214,
 52, 13, 0.0, 0.372, 0.497, 21.553,
 53, 14, 0.0, 0.400, 0.497, 21.892,
 54, 18, 0.0, 1.627, 0.495, 22.231,
 55, 18, 0.0, 1.627, 0.494, 22.569,
 56, 20, 0.0, 1.808, 0.497, 22.909,
 57, 25, 0.0, 22.607, 0.494, 23.248,
 58, 25, 0.0, 22.607, 0.487, 23.583,
 59, 35, 0.0, 31.650, 0.483, 23.915,
 60, 67, 0.0, 605.867, 0.482, 24.246,
 61, 67, 0.0, 605.867, 0.524, 24.576,
 44100.0, 62,
 0, 1, 24.5, 4.532, 0.951, 0.000,
 1, 1, 24.5, 4.532, 0.700, 0.431,
 2, 1, 24.5, 4.532, 0.681, 0.861,
 3, 1, 24.5, 0.904, 0.675, 1.292,
 4, 1, 24.5, 0.904, 0.667, 1.723,
 5, 1, 20.0, 0.090, 0.665, 2.153,
 6, 1, 20.0, 0.090, 0.664, 2.584,
 7, 1, 20.0, 0.029, 0.664, 3.015,
 8, 1, 20.0, 0.029, 0.664, 3.445,
 9, 1, 20.0, 0.029, 0.655, 3.876,
 10, 1, 20.0, 0.009, 0.616, 4.279,
 11, 1, 20.0, 0.009, 0.597, 4.670,
 12, 1, 18.0, 0.009, 0.578, 5.057,
 13, 1, 18.0, 0.009, 0.541, 5.416,
 14, 1, 18.0, 0.009, 0.575, 5.774,
 15, 2, 12.0, 0.018, 0.856, 6.422,
 16, 2, 6.0, 0.018, 0.846, 7.026,
 17, 2, 6.0, 0.018, 0.840, 7.609,
 18, 2, 3.0, 0.018, 0.822, 8.168,
 19, 2, 3.0, 0.018, 0.800, 8.710,
 20, 2, 3.0, 0.018, 0.753, 9.207,
 21, 2, 3.0, 0.018, 0.704, 9.662,
 22, 2, 0.0, 0.018, 0.674, 10.099,
 23, 2, 0.0, 0.018, 0.640, 10.515,
 24, 2, 0.0, 0.018, 0.609, 10.917,
 25, 2, 0.0, 0.018, 0.566, 11.293,
 26, 2, 0.0, 0.018, 0.535, 11.652,
 27, 2, 0.0, 0.018, 0.531, 11.997,
 28, 3, 0.0, 0.027, 0.615, 12.394,
 29, 3, 0.0, 0.027, 0.686, 12.850,
 30, 3, 0.0, 0.027, 0.650, 13.277,
 31, 3, 0.0, 0.027, 0.612, 13.681,
 32, 3, 0.0, 0.027, 0.567, 14.062,
 33, 3, 0.0, 0.027, 0.520, 14.411,
 34, 3, 0.0, 0.027, 0.513, 14.751,
 35, 4, 0.0, 0.036, 0.557, 15.119,
 36, 4, 0.0, 0.036, 0.584, 15.508,
 37, 4, 0.0, 0.036, 0.570, 15.883,
 38, 5, 0.0, 0.045, 0.579, 16.263,
 39, 5, 0.0, 0.045, 0.585, 16.654,
 40, 5, 0.0, 0.045, 0.548, 17.020,
 41, 6, 0.0, 0.054, 0.536, 17.374,
 42, 6, 0.0, 0.054, 0.550, 17.744,
 43, 7, 0.0, 0.063, 0.532, 18.104,
 44, 7, 0.0, 0.063, 0.504, 18.447,
 45, 7, 0.0, 0.063, 0.496, 18.782,
 46, 9, 0.0, 0.081, 0.517, 19.130,
 47, 9, 0.0, 0.081, 0.527, 19.487,
 48, 9, 0.0, 0.081, 0.516, 19.838,
 49, 10, 0.0, 0.180, 0.497, 20.179,
 50, 10, 0.0, 0.180, 0.489, 20.510,
 51, 11, 0.0, 0.198, 0.502, 20.852,
 52, 14, 0.0, 0.400, 0.501, 21.196,
 53, 14, 0.0, 0.400, 0.491, 21.531,
 54, 15, 0.0, 0.429, 0.497, 21.870,
 55, 20, 0.0, 1.808, 0.504, 22.214,
 56, 20, 0.0, 1.808, 0.504, 22.558,
 57, 21, 0.0, 1.898, 0.495, 22.898,
 58, 27, 0.0, 24.416, 0.486, 23.232,
 59, 27, 0.0, 24.416, 0.484, 23.564,
 60, 36, 0.0, 32.554, 0.483, 23.897,
 61, 73, 0.0, 660.124, 0.475, 24.229,
 62, 18, 0.0, 162.770, 0.515, 24.442,
 32000.0, 58,
 0, 2, 24.5, 4.532, 0.997, 0.313,
 1, 2, 24.5, 4.532, 0.893, 0.938,
 2, 2, 24.5, 1.809, 0.881, 1.563,
 3, 2, 20.0, 0.181, 0.873, 2.188,
 4, 2, 20.0, 0.181, 0.872, 2.813,
 5, 2, 20.0, 0.057, 0.871, 3.438,
 6, 2, 20.0, 0.018, 0.860, 4.045,
 7, 2, 20.0, 0.018, 0.839, 4.625,
 8, 2, 18.0, 0.018, 0.812, 5.173,
 9, 2, 18.0, 0.018, 0.784, 5.698,
 10, 2, 12.0, 0.018, 0.741, 6.185,
 11, 2, 12.0, 0.018, 0.697, 6.634,
 12, 2, 6.0, 0.018, 0.674, 7.070,
 13, 2, 6.0, 0.018, 0.651, 7.492,
 14, 2, 6.0, 0.018, 0.633, 7.905,
 15, 2, 3.0, 0.018, 0.611, 8.305,
 16, 2, 3.0, 0.018, 0.589, 8.695,
 17, 2, 3.0, 0.018, 0.575, 9.064,
 18, 3, 3.0, 0.027, 0.654, 9.484,
 19, 3, 3.0, 0.027, 0.724, 9.966,
 20, 3, 0.0, 0.027, 0.701, 10.426,
 21, 3, 0.0, 0.027, 0.673, 10.866,
 22, 3, 0.0, 0.027, 0.631, 11.279,
 23, 3, 0.0, 0.027, 0.592, 11.669,
 24, 3, 0.0, 0.027, 0.553, 12.042,
 25, 3, 0.0, 0.027, 0.510, 12.386,
 26, 3, 0.0, 0.027, 0.506, 12.721,
 27, 4, 0.0, 0.036, 0.562, 13.091,
 28, 4, 0.0, 0.036, 0.598, 13.488,
 29, 4, 0.0, 0.036, 0.589, 13.873,
 30, 5, 0.0, 0.045, 0.607, 14.268,
 31, 5, 0.0, 0.045, 0.620, 14.679,
 32, 5, 0.0, 0.045, 0.580, 15.067,
 33, 5, 0.0, 0.045, 0.532, 15.424,
 34, 5, 0.0, 0.045, 0.517, 15.771,
 35, 6, 0.0, 0.054, 0.517, 16.120,
 36, 6, 0.0, 0.054, 0.509, 16.466,
 37, 6, 0.0, 0.054, 0.506, 16.807,
 38, 8, 0.0, 0.072, 0.522, 17.158,
 39, 8, 0.0, 0.072, 0.531, 17.518,
 40, 8, 0.0, 0.072, 0.519, 17.869,
 41, 10, 0.0, 0.090, 0.512, 18.215,
 42, 10, 0.0, 0.090, 0.509, 18.563,
 43, 10, 0.0, 0.090, 0.498, 18.902,
 44, 12, 0.0, 0.109, 0.494, 19.239,
 45, 12, 0.0, 0.109, 0.501, 19.580,
 46, 13, 0.0, 0.118, 0.508, 19.925,
 47, 14, 0.0, 0.252, 0.502, 20.269,
 48, 14, 0.0, 0.252, 0.493, 20.606,
 49, 16, 0.0, 0.288, 0.497, 20.944,
 50, 20, 0.0, 0.572, 0.506, 21.288,
 51, 20, 0.0, 0.572, 0.510, 21.635,
 52, 23, 0.0, 0.658, 0.504, 21.980,
 53, 27, 0.0, 2.441, 0.496, 22.319,
 54, 27, 0.0, 2.441, 0.493, 22.656,
 55, 32, 0.0, 2.893, 0.490, 22.993,
 56, 37, 0.0, 33.458, 0.482, 23.326,
 57, 37, 0.0, 33.458, 0.458, 23.656,
 58, 12, 0.0, 10.851, 0.500, 23.937,
 24000, 59,
 0, 2, 15, 17.8250179, 0.697374165, 0.236874461,
 1, 2, 15, 17.8250179, 0.455024809, 0.71016103,
 2, 2, 15, 1.78250182, 0.431440443, 1.18193281,
 3, 2, 15, 1.78250182, 0.42391625, 1.65102732,
 4, 2, 13, 0.178250194, 0.418206781, 2.11632562,
 5, 2, 13, 0.178250194, 0.41158545, 2.57676744,
 6, 2, 13, 0.0563676581, 0.405409157, 3.03136396,
 7, 2, 13, 0.0563676581, 0.399695486, 3.47920918,
 8, 2, 13, 0.0563676581, 0.393753231, 3.91948748,
 9, 2, 12, 0.0178250186, 0.387357473, 4.35147953,
 10, 2, 12, 0.0178250186, 0.38045457, 4.77456427,
 11, 2, 10, 0.0178250186, 0.373053908, 5.18822002,
 12, 2, 10, 0.0178250186, 0.365188122, 5.59202194,
 13, 2, 10, 0.0178250186, 0.356897771, 5.98564005,
 14, 2, 9, 0.0178250186, 0.348700613, 6.36883163,
 15, 2, 9, 0.0178250186, 0.340260029, 6.74143791,
 16, 2, 6, 0.0178250186, 0.332341045, 7.10337448,
 17, 2, 6, 0.0178250186, 0.330462843, 7.45462418,
 18, 2, 6, 0.0178250186, 0.345568359, 7.79523182,
 19, 3, 3, 0.0267375279, 0.377859652, 8.20455742,
 20, 3, 3, 0.0267375279, 0.396689415, 8.67640114,
 21, 3, 3, 0.0267375279, 0.391237885, 9.12561035,
 22, 3, 3, 0.0267375279, 0.37761277, 9.55298138,
 23, 3, 3, 0.0267375279, 0.362836808, 9.95940971,
 24, 3, 0, 0.0267375279, 0.349010617, 10.3458519,
 25, 3, 0, 0.0267375279, 0.339673489, 10.7132998,
 26, 3, 0, 0.0267375279, 0.343845725, 11.0627575,
 27, 4, 0, 0.0356500372, 0.355822682, 11.447506,
 28, 4, 0, 0.0356500372, 0.358104348, 11.8627586,
 29, 4, 0, 0.0356500372, 0.34745428, 12.2520256,
 30, 4, 0, 0.0356500372, 0.334927917, 12.6173973,
 31, 4, 0, 0.0356500372, 0.331643254, 12.9608269,
 32, 5, 0, 0.0445625484, 0.333368897, 13.3219252,
 33, 5, 0, 0.0445625484, 0.332313001, 13.6976833,
 34, 5, 0, 0.0445625484, 0.3314417, 14.047802,
 35, 6, 0, 0.0534750558, 0.330947191, 14.405302,
 36, 6, 0, 0.0534750558, 0.332477689, 14.7684803,
 37, 7, 0, 0.062387567, 0.332647532, 15.1315956,
 38, 7, 0, 0.062387567, 0.330841452, 15.4940481,
 39, 8, 0, 0.0713000745, 0.327769846, 15.8516159,
 40, 8, 0, 0.0713000745, 0.324572712, 16.204628,
 41, 9, 0, 0.0802125856, 0.323825002, 16.5502281,
 42, 10, 0, 0.0891250968, 0.321414798, 16.9067478,
 43, 10, 0, 0.0891250968, 0.318189293, 17.2537231,
 44, 11, 0, 0.0980376005, 0.315934151, 17.5901108,
 45, 12, 0, 0.106950112, 0.315639287, 17.931406,
 46, 13, 0, 0.115862623, 0.316569835, 18.2750721,
 47, 14, 0, 0.124775134, 0.31656, 18.6191597,
 48, 15, 0, 0.133687645, 0.315465957, 18.9621754,
 49, 16, 0, 0.142600149, 0.313576341, 19.3029613,
 50, 17, 0, 0.151512653, 0.311635971, 19.6405869,
 51, 18, 0, 0.160425171, 0.311066717, 19.9742699,
 52, 20, 0, 0.355655879, 0.311465651, 20.3115921,
 53, 21, 0, 0.373438686, 0.311872005, 20.6507797,
 54, 23, 0, 0.409004271, 0.311015964, 20.9890823,
 55, 24, 0, 0.676411927, 0.309207708, 21.3251152,
 56, 26, 0, 0.732779562, 0.3081128, 21.6565971,
 57, 28, 0, 0.789147198, 0.310006589, 21.9881554,
 58, 31, 0, 2.76287794, 0.327113092, 22.3222847,
 59, 34, 0, 3.03025317, 0.416082352, 22.6605186,
 22050, 59,
 0, 2, 15, 17.8250179, 0.658683598, 0.217637643,
 1, 2, 15, 17.8250179, 0.432554901, 0.652563453,
 2, 2, 15, 1.78250182, 0.405113578, 1.08633137,
 3, 2, 15, 1.78250182, 0.397231787, 1.51803517,
 4, 2, 15, 1.78250182, 0.392088681, 1.94679713,
 5, 2, 13, 0.178250194, 0.386788279, 2.37177849,
 6, 2, 13, 0.178250194, 0.380574644, 2.79218864,
 7, 2, 13, 0.0563676581, 0.375309765, 3.20729256,
 8, 2, 13, 0.0563676581, 0.370087624, 3.61641645,
 9, 2, 12, 0.0178250186, 0.364568561, 4.01895428,
 10, 2, 12, 0.0178250186, 0.358959526, 4.4143672,
 11, 2, 12, 0.0178250186, 0.352938265, 4.80218887,
 12, 2, 10, 0.0178250186, 0.3465029, 5.18202305,
 13, 2, 10, 0.0178250186, 0.33968094, 5.55354261,
 14, 2, 10, 0.0178250186, 0.332571507, 5.91648674,
 15, 2, 9, 0.0178250186, 0.326015651, 6.27065945,
 16, 2, 9, 0.0178250186, 0.325442046, 6.61592293,
 17, 2, 9, 0.0178250186, 0.341315031, 6.95219517,
 18, 3, 6, 0.0267375279, 0.374984443, 7.3584404,
 19, 3, 6, 0.0267375279, 0.396138102, 7.8290925,
 20, 3, 3, 0.0267375279, 0.39271906, 8.27975655,
 21, 3, 3, 0.0267375279, 0.380755007, 8.71083069,
 22, 3, 3, 0.0267375279, 0.367386311, 9.12284088,
 23, 3, 3, 0.0267375279, 0.354351997, 9.51640987,
 24, 3, 3, 0.0267375279, 0.341508389, 9.89222908,
 25, 3, 0, 0.0267375279, 0.333577901, 10.2510386,
 26, 3, 0, 0.0267375279, 0.338108748, 10.5936022,
 27, 4, 0, 0.0356500372, 0.350744486, 10.9723492,
 28, 4, 0, 0.0356500372, 0.354519457, 11.38272,
 29, 4, 0, 0.0356500372, 0.345274031, 11.7689981,
 30, 4, 0, 0.0356500372, 0.333828837, 12.1329184,
 31, 4, 0, 0.0356500372, 0.331436664, 12.4761295,
 32, 5, 0, 0.0445625484, 0.334172577, 12.8381901,
 33, 5, 0, 0.0445625484, 0.334024847, 13.2160273,
 34, 5, 0, 0.0445625484, 0.33392629, 13.5690479,
 35, 6, 0, 0.0534750558, 0.334218502, 13.9303951,
 36, 6, 0, 0.0534750558, 0.336405039, 14.298193,
 37, 7, 0, 0.062387567, 0.337080389, 14.666563,
 38, 7, 0, 0.062387567, 0.335603535, 15.0346909,
 39, 8, 0, 0.0713000745, 0.332515866, 15.398139,
 40, 8, 0, 0.0713000745, 0.327727586, 15.7570457,
 41, 9, 0, 0.0802125856, 0.322346836, 16.1083431,
 42, 9, 0, 0.0802125856, 0.317575186, 16.4528522,
 43, 10, 0, 0.0891250968, 0.31632933, 16.7886105,
 44, 11, 0, 0.0980376005, 0.317602783, 17.132,
 45, 12, 0, 0.106950112, 0.319945186, 17.4796028,
 46, 13, 0, 0.115862623, 0.320881754, 17.8287659,
 47, 14, 0, 0.124775134, 0.320346534, 18.1774921,
 48, 15, 0, 0.133687645, 0.318628669, 18.5243168,
 49, 16, 0, 0.142600149, 0.316125751, 18.8681736,
 50, 17, 0, 0.151512653, 0.313746184, 19.2082729,
 51, 18, 0, 0.160425171, 0.312971771, 19.5440025,
 52, 20, 0, 0.178250194, 0.313278913, 19.8831882,
 53, 21, 0, 0.373438686, 0.313735574, 20.224247,
 54, 23, 0, 0.409004271, 0.31308493, 20.5646286,
 55, 24, 0, 0.426787049, 0.31156227, 20.903141,
 56, 26, 0, 0.732779562, 0.310435742, 21.2376747,
 57, 28, 0, 0.789147198, 0.31132248, 21.5730591,
 58, 30, 0, 0.845514894, 0.32730341, 21.9066811,
 59, 33, 0, 2.94112802, 0.414659739, 22.2411156,
 16000, 55,
 0, 3, 15, 26.7375278, 0.697374165, 0.236874461,
 1, 3, 15, 26.7375278, 0.455024809, 0.71016103,
 2, 3, 15, 2.67375278, 0.431440443, 1.18193281,
 3, 3, 15, 2.67375278, 0.42391625, 1.65102732,
 4, 3, 13, 0.26737529, 0.418206781, 2.11632562,
 5, 3, 13, 0.26737529, 0.41158545, 2.57676744,
 6, 3, 13, 0.0845514908, 0.405409157, 3.03136396,
 7, 3, 13, 0.0845514908, 0.399695486, 3.47920918,
 8, 3, 13, 0.0845514908, 0.393753231, 3.91948748,
 9, 3, 12, 0.0267375279, 0.387357473, 4.35147953,
 10, 3, 12, 0.0267375279, 0.38045457, 4.77456427,
 11, 3, 10, 0.0267375279, 0.373053908, 5.18822002,
 12, 3, 10, 0.0267375279, 0.365188122, 5.59202194,
 13, 3, 10, 0.0267375279, 0.356897742, 5.98564005,
 14, 3, 9, 0.0267375279, 0.34869957, 6.36883163,
 15, 3, 9, 0.0267375279, 0.340241522, 6.74143791,
 16, 3, 6, 0.0267375279, 0.332089454, 7.10337448,
 17, 3, 6, 0.0267375279, 0.328292668, 7.45462418,
 18, 3, 6, 0.0267375279, 0.336574793, 7.79523182,
 19, 4, 3, 0.0356500372, 0.354600489, 8.17827797,
 20, 4, 3, 0.0356500372, 0.364343345, 8.59994984,
 21, 4, 3, 0.0356500372, 0.359369367, 9.00363636,
 22, 4, 3, 0.0356500372, 0.347775847, 9.38988018,
 23, 4, 3, 0.0356500372, 0.335562587, 9.7592926,
 24, 4, 0, 0.0356500372, 0.326988578, 10.1125278,
 25, 4, 0, 0.0356500372, 0.327966213, 10.4502735,
 26, 5, 0, 0.0445625484, 0.334450752, 10.811614,
 27, 5, 0, 0.0445625484, 0.335228145, 11.1935263,
 28, 5, 0, 0.0445625484, 0.329595625, 11.5549288,
 29, 5, 0, 0.0445625484, 0.326683223, 11.8971443,
 30, 6, 0, 0.0534750558, 0.326986551, 12.2520256,
 31, 6, 0, 0.0534750558, 0.325072199, 12.6173973,
 32, 6, 0, 0.0534750558, 0.323560268, 12.9608269,
 33, 7, 0, 0.062387567, 0.322494298, 13.3093863,
 34, 7, 0, 0.062387567, 0.323403448, 13.6617231,
 35, 8, 0, 0.0713000745, 0.323232353, 14.0134668,
 36, 8, 0, 0.0713000745, 0.322662383, 14.3639784,
 37, 9, 0, 0.0802125856, 0.324054241, 14.7098465,
 38, 10, 0, 0.0891250968, 0.323228806, 15.0686541,
 39, 10, 0, 0.0891250968, 0.320751846, 15.4191036,
 40, 11, 0, 0.0980376005, 0.318823338, 15.7594051,
 41, 12, 0, 0.106950112, 0.318418682, 16.104557,
 42, 13, 0, 0.115862623, 0.318762124, 16.451416,
 43, 14, 0, 0.124775134, 0.317806393, 16.7975388,
 44, 15, 0, 0.133687645, 0.315653771, 17.1411018,
 45, 16, 0, 0.142600149, 0.313369036, 17.4808159,
 46, 17, 0, 0.151512653, 0.312513858, 17.8158207,
 47, 19, 0, 0.169337675, 0.312785119, 18.1543369,
 48, 20, 0, 0.178250194, 0.31343773, 18.4948578,
 49, 22, 0, 0.196075201, 0.313258767, 18.8350143,
 50, 23, 0, 0.20498772, 0.312570423, 19.1740704,
 51, 25, 0, 0.222812727, 0.312572777, 19.5104179,
 52, 27, 0, 0.240637749, 0.313047856, 19.8497677,
 53, 29, 0, 0.515701056, 0.315029174, 20.1900635,
 54, 31, 0, 0.551266611, 0.330613613, 20.5294952,
 55, 33, 0, 0.586832225, 0.41819948, 20.8664398,
 48000.0, 37,
 0, 1, 4.532, 1.000, -8.240, 0.000,
 1, 1, 0.904, 0.989, -8.240, 1.875,
 2, 1, 0.029, 0.989, -8.240, 3.750,
 3, 1, 0.009, 0.981, -8.240, 5.438,
 4, 1, 0.009, 0.985, -8.240, 6.857,
 5, 1, 0.009, 0.984, -8.240, 8.109,
 6, 1, 0.009, 0.980, -8.240, 9.237,
 7, 1, 0.009, 0.968, -8.240, 10.202,
 8, 1, 0.009, 0.954, -8.240, 11.083,
 9, 1, 0.009, 0.929, -8.240, 11.865,
 10, 1, 0.009, 0.906, -7.447, 12.554,
 11, 1, 0.009, 0.883, -7.447, 13.195,
 12, 1, 0.009, 0.844, -7.447, 13.781,
 13, 1, 0.009, 0.792, -7.447, 14.309,
 14, 1, 0.009, 0.747, -7.447, 14.803,
 15, 1, 0.009, 0.689, -7.447, 15.250,
 16, 1, 0.009, 0.644, -7.447, 15.667,
 17, 1, 0.009, 0.592, -7.447, 16.068,
 18, 1, 0.009, 0.553, -7.447, 16.409,
 19, 2, 0.018, 0.850, -7.447, 17.045,
 20, 2, 0.018, 0.811, -6.990, 17.607,
 21, 2, 0.018, 0.736, -6.990, 18.097,
 22, 2, 0.018, 0.665, -6.990, 18.528,
 23, 2, 0.018, 0.610, -6.990, 18.931,
 24, 2, 0.018, 0.544, -6.990, 19.295,
 25, 2, 0.018, 0.528, -6.990, 19.636,
 26, 3, 0.054, 0.621, -6.990, 20.038,
 27, 3, 0.054, 0.673, -6.990, 20.486,
 28, 3, 0.054, 0.635, -6.990, 20.900,
 29, 4, 0.114, 0.626, -6.990, 21.306,
 30, 4, 0.114, 0.636, -6.020, 21.722,
 31, 5, 0.452, 0.615, -6.020, 22.128,
 32, 5, 0.452, 0.579, -6.020, 22.513,
 33, 5, 0.452, 0.551, -6.020, 22.877,
 34, 7, 6.330, 0.552, -5.229, 23.241,
 35, 7, 6.330, 0.559, -5.229, 23.616,
 36, 11, 9.947, 0.528, -5.229, 23.974,
 37, 17, 153.727, 0.479, -5.229, 24.313,
 44100.0, 38,
 0, 1, 4.532, 1.000, -8.240, 0.000,
 1, 1, 0.904, 0.983, -8.240, 1.723,
 2, 1, 0.029, 0.983, -8.240, 3.445,
 3, 1, 0.009, 0.982, -8.240, 5.057,
 4, 1, 0.009, 0.985, -8.240, 6.422,
 5, 1, 0.009, 0.983, -8.240, 7.609,
 6, 1, 0.009, 0.978, -8.240, 8.710,
 7, 1, 0.009, 0.967, -8.240, 9.662,
 8, 1, 0.009, 0.948, -8.240, 10.515,
 9, 1, 0.009, 0.930, -8.240, 11.293,
 10, 1, 0.009, 0.914, -7.447, 12.009,
 11, 1, 0.009, 0.870, -7.447, 12.625,
 12, 1, 0.009, 0.845, -7.447, 13.210,
 13, 1, 0.009, 0.800, -7.447, 13.748,
 14, 1, 0.009, 0.749, -7.447, 14.241,
 15, 1, 0.009, 0.701, -7.447, 14.695,
 16, 1, 0.009, 0.653, -7.447, 15.125,
 17, 1, 0.009, 0.590, -7.447, 15.508,
 18, 1, 0.009, 0.616, -7.447, 15.891,
 19, 2, 0.018, 0.860, -7.447, 16.537,
 20, 2, 0.018, 0.823, -6.990, 17.112,
 21, 2, 0.018, 0.762, -6.990, 17.621,
 22, 2, 0.018, 0.688, -6.990, 18.073,
 23, 2, 0.018, 0.612, -6.990, 18.470,
 24, 2, 0.018, 0.594, -6.990, 18.849,
 25, 3, 0.027, 0.658, -6.990, 19.271,
 26, 3, 0.027, 0.706, -6.990, 19.741,
 27, 3, 0.054, 0.660, -6.990, 20.177,
 28, 3, 0.054, 0.606, -6.990, 20.576,
 29, 3, 0.054, 0.565, -6.990, 20.950,
 30, 4, 0.114, 0.560, -6.020, 21.316,
 31, 4, 0.114, 0.579, -6.020, 21.699,
 32, 5, 0.452, 0.567, -6.020, 22.078,
 33, 5, 0.452, 0.534, -6.020, 22.438,
 34, 5, 0.452, 0.514, -5.229, 22.782,
 35, 7, 6.330, 0.520, -5.229, 23.133,
 36, 7, 6.330, 0.518, -5.229, 23.484,
 37, 7, 6.330, 0.507, -5.229, 23.828,
 38, 19, 171.813, 0.447, -4.559, 24.173,
 32000.0, 41,
 0, 1, 4.532, 1.000, -8.240, 0.000,
 1, 1, 0.904, 0.985, -8.240, 1.250,
 2, 1, 0.090, 0.983, -8.240, 2.500,
 3, 1, 0.029, 0.983, -8.240, 3.750,
 4, 1, 0.009, 0.981, -8.240, 4.909,
 5, 1, 0.009, 0.975, -8.240, 5.958,
 6, 1, 0.009, 0.959, -8.240, 6.857,
 7, 1, 0.009, 0.944, -8.240, 7.700,
 8, 1, 0.009, 0.933, -8.240, 8.500,
 9, 1, 0.009, 0.920, -8.240, 9.237,
 10, 1, 0.009, 0.892, -7.447, 9.895,
 11, 1, 0.009, 0.863, -7.447, 10.500,
 12, 1, 0.009, 0.839, -7.447, 11.083,
 13, 1, 0.009, 0.786, -7.447, 11.604,
 14, 1, 0.009, 0.755, -7.447, 12.107,
 15, 1, 0.009, 0.698, -7.447, 12.554,
 16, 1, 0.009, 0.673, -7.447, 13.000,
 17, 1, 0.009, 0.605, -7.447, 13.391,
 18, 1, 0.009, 0.629, -7.447, 13.781,
 19, 2, 0.018, 0.883, -7.447, 14.474,
 20, 2, 0.018, 0.858, -6.990, 15.096,
 21, 2, 0.018, 0.829, -6.990, 15.667,
 22, 2, 0.018, 0.767, -6.990, 16.177,
 23, 2, 0.018, 0.705, -6.990, 16.636,
 24, 2, 0.018, 0.637, -6.990, 17.057,
 25, 2, 0.018, 0.564, -6.990, 17.429,
 26, 2, 0.018, 0.550, -6.990, 17.786,
 27, 3, 0.027, 0.603, -6.990, 18.177,
 28, 3, 0.027, 0.635, -6.990, 18.597,
 29, 3, 0.027, 0.592, -6.990, 18.994,
 30, 3, 0.027, 0.533, -6.020, 19.352,
 31, 3, 0.027, 0.518, -6.020, 19.693,
 32, 4, 0.072, 0.568, -6.020, 20.066,
 33, 4, 0.072, 0.594, -6.020, 20.462,
 34, 4, 0.072, 0.568, -5.229, 20.841,
 35, 5, 0.143, 0.536, -5.229, 21.201,
 36, 5, 0.143, 0.522, -5.229, 21.549,
 37, 6, 0.172, 0.542, -5.229, 21.911,
 38, 7, 0.633, 0.539, -4.559, 22.275,
 39, 7, 0.633, 0.519, -4.559, 22.625,
 40, 8, 0.723, 0.514, -3.980, 22.971,
 41, 10, 9.043, 0.518, -3.980, 23.321,
 24000, 44,
 0, 1, 8.91250896, 0.971850038, 0.150000006, 0,
 1, 1, 8.91250896, 0.874727964, 0.150000006, 0.946573138,
 2, 1, 0.891250908, 0.85779953, 0.150000006, 1.88476217,
 3, 1, 0.0891250968, 0.839743853, 0.150000006, 2.8056457,
 4, 1, 0.028183829, 0.82260257, 0.150000006, 3.70133615,
 5, 1, 0.00891250931, 0.80018574, 0.150000006, 4.56532001,
 6, 1, 0.00891250931, 0.771475196, 0.150000006, 5.39263105,
 7, 1, 0.00891250931, 0.737389982, 0.150000006, 6.17986727,
 8, 1, 0.00891250931, 0.701111019, 0.150000006, 6.92507982,
 9, 1, 0.00891250931, 0.65977633, 0.150000006, 7.62757969,
 10, 1, 0.00891250931, 0.615037441, 0.150000006, 8.28770351,
 11, 1, 0.00891250931, 0.568658054, 0.150000006, 8.90657234,
 12, 1, 0.00891250931, 0.522260666, 0.180000007, 9.48587132,
 13, 1, 0.00891250931, 0.478903115, 0.180000007, 10.0276566,
 14, 1, 0.00891250931, 0.43808648, 0.180000007, 10.5341988,
 15, 1, 0.00891250931, 0.412505627, 0.180000007, 11.0078659,
 16, 1, 0.00891250931, 0.39070797, 0.180000007, 11.4510288,
 17, 1, 0.00891250931, 0.371887118, 0.180000007, 11.866004,
 18, 1, 0.00891250931, 0.367617637, 0.180000007, 12.2550087,
 19, 1, 0.00891250931, 0.422220588, 0.180000007, 12.6201363,
 20, 2, 0.0178250186, 0.564990044, 0.180000007, 13.2772083,
 21, 2, 0.0178250186, 0.519700944, 0.180000007, 13.871047,
 22, 2, 0.0178250186, 0.455360681, 0.200000003, 14.4024391,
 23, 2, 0.0178250186, 0.408867925, 0.200000003, 14.8811684,
 24, 2, 0.0178250186, 0.381538749, 0.200000003, 15.3153324,
 25, 2, 0.0178250186, 0.362357527, 0.200000003, 15.7116165,
 26, 2, 0.0178250186, 0.365735918, 0.200000003, 16.0755405,
 27, 3, 0.0267375279, 0.38064, 0.200000003, 16.4882088,
 28, 3, 0.0267375279, 0.379183382, 0.200000003, 16.9410992,
 29, 3, 0.0267375279, 0.360672712, 0.200000003, 17.3513336,
 30, 3, 0.0267375279, 0.343065977, 0.200000003, 17.7264423,
 31, 3, 0.0267375279, 0.339290261, 0.200000003, 18.0722466,
 32, 4, 0.0356500372, 0.342963994, 0.200000003, 18.4426575,
 33, 4, 0.0356500372, 0.343128443, 0.200000003, 18.8344078,
 34, 4, 0.0356500372, 0.343988508, 0.25, 19.1955795,
 35, 5, 0.0445625484, 0.343928397, 0.25, 19.5697021,
 36, 5, 0.0445625484, 0.339527696, 0.25, 19.9551182,
 37, 5, 0.0889139697, 0.336541563, 0.280000001, 20.3115921,
 38, 6, 0.106696762, 0.334955156, 0.280000001, 20.6737747,
 39, 6, 0.169102982, 0.335601568, 0.300000012, 21.0404968,
 40, 7, 0.1972868, 0.334716886, 0.300000012, 21.4060211,
 41, 7, 0.1972868, 0.331676662, 0.300000012, 21.7696877,
 42, 8, 0.713000774, 0.328550965, 0.400000006, 22.1267223,
 43, 8, 0.713000774, 0.339241952, 0.400000006, 22.4769249,
 44, 9, 0.802125871, 0.425207615, 0.400000006, 22.8164864,
 22050, 44,
 0, 1, 8.91250896, 0.954045713, 0.150000006, 0,
 1, 1, 8.91250896, 0.833381653, 0.150000006, 0.869851649,
 2, 1, 0.891250908, 0.815945923, 0.150000006, 1.73325908,
 3, 1, 0.0891250968, 0.794244766, 0.150000006, 2.58322191,
 4, 1, 0.028183829, 0.776486695, 0.150000006, 3.4134295,
 5, 1, 0.00891250931, 0.755260408, 0.150000006, 4.21850443,
 6, 1, 0.00891250931, 0.731070817, 0.150000006, 4.99414825,
 7, 1, 0.00891250931, 0.701775849, 0.150000006, 5.73718691,
 8, 1, 0.00891250931, 0.667876124, 0.150000006, 6.44553185,
 9, 1, 0.00891250931, 0.630284071, 0.150000006, 7.11807632,
 10, 1, 0.00891250931, 0.590170324, 0.150000006, 7.75455618,
 11, 1, 0.00891250931, 0.548788548, 0.150000006, 8.3553915,
 12, 1, 0.00891250931, 0.507795513, 0.150000006, 8.92152882,
 13, 1, 0.00891250931, 0.469515711, 0.180000007, 9.45430183,
 14, 1, 0.00891250931, 0.432291716, 0.180000007, 9.95530319,
 15, 1, 0.00891250931, 0.411131173, 0.180000007, 10.4262848,
 16, 1, 0.00891250931, 0.390771538, 0.180000007, 10.8690758,
 17, 1, 0.00891250931, 0.373318017, 0.180000007, 11.2855215,
 18, 1, 0.00891250931, 0.36956048, 0.180000007, 11.6774378,
 19, 1, 0.00891250931, 0.42595759, 0.180000007, 12.0465794,
 20, 2, 0.0178250186, 0.576900065, 0.180000007, 12.7141209,
 21, 2, 0.0178250186, 0.533114731, 0.180000007, 13.3197365,
 22, 2, 0.0178250186, 0.469967514, 0.180000007, 13.8634901,
 23, 2, 0.0178250186, 0.417268544, 0.200000003, 14.3544445,
 24, 2, 0.0178250186, 0.389299124, 0.200000003, 14.8002586,
 25, 2, 0.0178250186, 0.362824857, 0.200000003, 15.2073727,
 26, 2, 0.0178250186, 0.346801281, 0.200000003, 15.5811834,
 27, 2, 0.0178250186, 0.349400043, 0.200000003, 15.926218,
 28, 3, 0.0267375279, 0.364026934, 0.200000003, 16.3194923,
 29, 3, 0.0267375279, 0.36560446, 0.200000003, 16.752903,
 30, 3, 0.0267375279, 0.354275256, 0.200000003, 17.1470814,
 31, 3, 0.0267375279, 0.351219416, 0.200000003, 17.5086212,
 32, 4, 0.0356500372, 0.354364097, 0.200000003, 17.8938141,
 33, 4, 0.0356500372, 0.348915905, 0.200000003, 18.2992878,
 34, 4, 0.0356500372, 0.337649345, 0.200000003, 18.6713982,
 35, 4, 0.0356500372, 0.332076877, 0.25, 19.015646,
 36, 5, 0.0445625484, 0.330793113, 0.25, 19.3734016,
 37, 5, 0.0445625484, 0.327528268, 0.25, 19.7430382,
 38, 5, 0.0889139697, 0.32551071, 0.280000001, 20.0859604,
 39, 6, 0.106696762, 0.324436843, 0.280000001, 20.4354992,
 40, 6, 0.106696762, 0.325835049, 0.280000001, 20.7905579,
 41, 7, 0.1972868, 0.326221824, 0.300000012, 21.1458054,
 42, 7, 0.1972868, 0.325960994, 0.300000012, 21.5005951,
 43, 8, 0.225470632, 0.339019388, 0.300000012, 21.8504524,
 44, 8, 0.713000774, 0.426850349, 0.400000006, 22.1951065,
 16000, 45,
 0, 1, 8.91250896, 0.834739447, 0.150000006, 0,
 1, 1, 8.91250896, 0.623757005, 0.150000006, 0.631518543,
 2, 1, 0.891250908, 0.60420388, 0.150000006, 1.2606914,
 3, 1, 0.891250908, 0.591974258, 0.150000006, 1.88476217,
 4, 1, 0.0891250968, 0.575301588, 0.150000006, 2.50111985,
 5, 1, 0.028183829, 0.561547697, 0.150000006, 3.1073606,
 6, 1, 0.028183829, 0.546665847, 0.150000006, 3.70133615,
 7, 1, 0.00891250931, 0.52986443, 0.150000006, 4.28118753,
 8, 1, 0.00891250931, 0.511183441, 0.150000006, 4.84536505,
 9, 1, 0.00891250931, 0.490902334, 0.150000006, 5.39263105,
 10, 1, 0.00891250931, 0.46938166, 0.150000006, 5.92205667,
 11, 1, 0.00891250931, 0.447003782, 0.150000006, 6.43299866,
 12, 1, 0.00891250931, 0.428170592, 0.150000006, 6.92507982,
 13, 1, 0.00891250931, 0.414536625, 0.150000006, 7.39815664,
 14, 1, 0.00891250931, 0.401033074, 0.150000006, 7.85228777,
 15, 1, 0.00891250931, 0.38779071, 0.150000006, 8.28770351,
 16, 1, 0.00891250931, 0.374230444, 0.150000006, 8.704772,
 17, 1, 0.00891250931, 0.360547513, 0.180000007, 9.10397339,
 18, 1, 0.00891250931, 0.348256677, 0.180000007, 9.48587132,
 19, 1, 0.00891250931, 0.350327015, 0.180000007, 9.85109234,
 20, 1, 0.00891250931, 0.406330824, 0.180000007, 10.200304,
 21, 2, 0.0178250186, 0.554098248, 0.180000007, 10.846529,
 22, 2, 0.0178250186, 0.528312504, 0.180000007, 11.4447651,
 23, 2, 0.0178250186, 0.476527005, 0.180000007, 11.9928398,
 24, 2, 0.0178250186, 0.428205669, 0.180000007, 12.495945,
 25, 2, 0.0178250186, 0.402271926, 0.180000007, 12.9588718,
 26, 2, 0.0178250186, 0.378024429, 0.180000007, 13.3859692,
 27, 2, 0.0178250186, 0.36254698, 0.180000007, 13.7811394,
 28, 2, 0.0178250186, 0.368058592, 0.200000003, 14.1478529,
 29, 3, 0.0267375279, 0.385963261, 0.200000003, 14.5674343,
 30, 3, 0.0267375279, 0.38640517, 0.200000003, 15.0304852,
 31, 3, 0.0267375279, 0.367834061, 0.200000003, 15.4513416,
 32, 3, 0.0267375279, 0.349686563, 0.200000003, 15.836277,
 33, 3, 0.0267375279, 0.345709383, 0.200000003, 16.1904697,
 34, 4, 0.0356500372, 0.34871915, 0.200000003, 16.5683517,
 35, 4, 0.0356500372, 0.347054332, 0.200000003, 16.9660263,
 36, 4, 0.0356500372, 0.346329987, 0.200000003, 17.3304482,
 37, 5, 0.0445625484, 0.344658494, 0.200000003, 17.7055588,
 38, 5, 0.0445625484, 0.338779271, 0.200000003, 18.0899811,
 39, 5, 0.0445625484, 0.334878683, 0.200000003, 18.4440536,
 40, 6, 0.0534750558, 0.332811534, 0.200000003, 18.8030052,
 41, 6, 0.0534750558, 0.333717585, 0.25, 19.1665268,
 42, 7, 0.062387567, 0.333986402, 0.25, 19.5299358,
 43, 7, 0.062387567, 0.334142625, 0.25, 19.8934898,
 44, 8, 0.142262354, 0.34677428, 0.280000001, 20.2535706,
 45, 8, 0.142262354, 0.436254472, 0.280000001, 20.610569,
 48000.0, 20,
 0, 3, 0, 4, 1.000, 0.056,
 1, 3, 4, 7, 0.944, 0.611,
 2, 4, 7, 11, 0.389, 0.167,
 3, 3, 11, 14, 0.833, 0.722,
 4, 3, 14, 17, 0.278, 0.639,
 5, 2, 17, 19, 0.361, 0.417,
 6, 3, 19, 22, 0.583, 0.083,
 7, 2, 22, 24, 0.917, 0.750,
 8, 3, 24, 27, 0.250, 0.417,
 9, 3, 27, 30, 0.583, 0.648,
 10, 3, 30, 33, 0.352, 0.611,
 11, 3, 33, 36, 0.389, 0.625,
 12, 4, 36, 40, 0.375, 0.144,
 13, 3, 40, 43, 0.856, 0.389,
 14, 3, 43, 46, 0.611, 0.160,
 15, 3, 46, 49, 0.840, 0.217,
 16, 3, 49, 52, 0.783, 0.184,
 17, 2, 52, 54, 0.816, 0.886,
 18, 3, 54, 57, 0.114, 0.313,
 19, 2, 57, 59, 0.687, 0.452,
 20, 1, 59, 60, 0.548, 0.908,
 44100.0, 20,
 0, 3, 0, 4, 1.000, 0.056,
 1, 3, 4, 7, 0.944, 0.611,
 2, 4, 7, 11, 0.389, 0.167,
 3, 3, 11, 14, 0.833, 0.722,
 4, 3, 14, 17, 0.278, 0.139,
 5, 1, 17, 18, 0.861, 0.917,
 6, 3, 18, 21, 0.083, 0.583,
 7, 3, 21, 24, 0.417, 0.250,
 8, 3, 24, 27, 0.750, 0.805,
 9, 3, 27, 30, 0.194, 0.574,
 10, 3, 30, 33, 0.426, 0.537,
 11, 3, 33, 36, 0.463, 0.819,
 12, 4, 36, 40, 0.180, 0.100,
 13, 3, 40, 43, 0.900, 0.468,
 14, 3, 43, 46, 0.532, 0.623,
 15, 3, 46, 49, 0.376, 0.450,
 16, 3, 49, 52, 0.550, 0.552,
 17, 3, 52, 55, 0.448, 0.403,
 18, 2, 55, 57, 0.597, 0.643,
 19, 2, 57, 59, 0.357, 0.722,
 20, 2, 59, 61, 0.278, 0.960,
 32000, 20,
 0, 1, 0, 2, 1.000, 0.528,
 1, 2, 2, 4, 0.472, 0.305,
 2, 2, 4, 6, 0.694, 0.083,
 3, 1, 6, 7, 0.917, 0.861,
 4, 2, 7, 9, 0.139, 0.639,
 5, 2, 9, 11, 0.361, 0.417,
 6, 3, 11, 14, 0.583, 0.083,
 7, 2, 14, 16, 0.917, 0.750,
 8, 3, 16, 19, 0.250, 0.870,
 9, 3, 19, 22, 0.130, 0.833,
 10, 4, 22, 26, 0.167, 0.389,
 11, 4, 26, 30, 0.611, 0.478,
 12, 4, 30, 34, 0.522, 0.033,
 13, 3, 34, 37, 0.967, 0.917,
 14, 4, 37, 41, 0.083, 0.617,
 15, 3, 41, 44, 0.383, 0.995,
 16, 4, 44, 48, 0.005, 0.274,
 17, 3, 48, 51, 0.726, 0.480,
 18, 3, 51, 54, 0.519, 0.261,
 19, 2, 54, 56, 0.739, 0.884,
 20, 2, 56, 58, 0.116, 1.000,
 24000, 20,
 0, 2, 0, 3, 1, 0.916666746,
 1, 3, 3, 6, 0.0833332539, 0.583333492,
 2, 3, 6, 9, 0.416666508, 0.25,
 3, 2, 9, 11, 0.75, 0.916666985,
 4, 3, 11, 14, 0.0833330154, 0.583333969,
 5, 3, 14, 17, 0.416666031, 0.25,
 6, 3, 17, 20, 0.75, 0.537036896,
 7, 3, 20, 23, 0.462963104, 0.5,
 8, 4, 23, 27, 0.5, 0.0555559993,
 9, 3, 27, 30, 0.944444001, 0.402778625,
 10, 3, 30, 33, 0.597221375, 0.766667187,
 11, 3, 33, 36, 0.233332828, 0.805555999,
 12, 3, 36, 39, 0.194444016, 0.769841909,
 13, 3, 39, 42, 0.23015812, 0.611111104,
 14, 3, 42, 45, 0.388888896, 0.449494779,
 15, 3, 45, 48, 0.550505221, 0.194444954,
 16, 2, 48, 50, 0.805555046, 0.913194656,
 17, 3, 50, 53, 0.0868053436, 0.580555737,
 18, 3, 53, 56, 0.419444263, 0.113426208,
 19, 2, 56, 58, 0.886573792, 0.533730626,
 20, 2, 58, 60, 0.466269344, 0.691176474,
 22050, 20,
 0, 2, 0, 3, 1, 0.916666746,
 1, 3, 3, 6, 0.0833332539, 0.583333492,
 2, 3, 6, 9, 0.416666508, 0.25,
 3, 2, 9, 11, 0.75, 0.916666985,
 4, 3, 11, 14, 0.0833330154, 0.583333969,
 5, 3, 14, 17, 0.416666031, 0.25,
 6, 3, 17, 20, 0.75, 0.203703582,
 7, 3, 20, 23, 0.796296418, 0.166666687,
 8, 3, 23, 26, 0.833333313, 0.722222686,
 9, 4, 26, 30, 0.277777344, 0.152778625,
 10, 3, 30, 33, 0.847221375, 0.566667199,
 11, 3, 33, 36, 0.433332831, 0.93518573,
 12, 4, 36, 40, 0.0648142472, 0.118056297,
 13, 3, 40, 43, 0.881943703, 0.0925937295,
 14, 2, 43, 45, 0.907406271, 0.934344172,
 15, 3, 45, 48, 0.0656557977, 0.575398028,
 16, 3, 48, 51, 0.424601972, 0.232026935,
 17, 2, 51, 53, 0.767973065, 0.758334339,
 18, 3, 53, 56, 0.241665646, 0.187501252,
 19, 2, 56, 58, 0.812498748, 0.533731699,
 20, 2, 58, 60, 0.466268271, 0.257577598,
 16000, 20,
 0, 1, 0, 2, 1, 0.944444478,
 1, 2, 2, 4, 0.0555555038, 0.722222328,
 2, 2, 4, 6, 0.277777672, 0.5,
 3, 2, 6, 8, 0.5, 0.27777797,
 4, 2, 8, 10, 0.72222203, 0.0555559993,
 5, 1, 10, 11, 0.944444001, 0.833333313,
 6, 3, 11, 14, 0.166666672, 0.203703582,
 7, 3, 14, 17, 0.796296418, 0.166666687,
 8, 3, 17, 20, 0.833333313, 0.54166698,
 9, 3, 20, 23, 0.458333015, 0.652778625,
 10, 4, 23, 27, 0.347221375, 0.166667163,
 11, 3, 27, 30, 0.833332837, 0.722222924,
 12, 4, 30, 34, 0.277777106, 0.277778625,
 13, 3, 34, 37, 0.722221375, 0.604167938,
 14, 3, 37, 40, 0.395832062, 0.627778649,
 15, 3, 40, 43, 0.37222138, 0.542736351,
 16, 3, 43, 46, 0.457263649, 0.371528625,
 17, 3, 46, 49, 0.628471375, 0.00833433867,
 18, 2, 49, 51, 0.991665661, 0.500001311,
 19, 2, 51, 53, 0.499998659, 0.886832893,
 20, 2, 53, 55, 0.113167092, 0.629034221,
 48000.0, 11,
 0, 2, 0, 2, 1.000, 0.167,
 1, 2, 3, 5, 0.833, 0.833,
 2, 3, 5, 8, 0.167, 0.500,
 3, 3, 8, 11, 0.500, 0.167,
 4, 4, 11, 15, 0.833, 0.167,
 5, 4, 15, 19, 0.833, 0.583,
 6, 3, 19, 22, 0.417, 0.917,
 7, 4, 22, 26, 0.083, 0.944,
 8, 4, 26, 30, 0.055, 0.042,
 9, 2, 30, 32, 0.958, 0.567,
 10, 3, 32, 35, 0.433, 0.167,
 11, 2, 35, 37, 0.833, 0.618,
 44100.0, 11,
 0, 2, 0, 2, 1.000, 0.167,
 1, 2, 3, 5, 0.833, 0.833,
 2, 3, 5, 8, 0.167, 0.500,
 3, 3, 8, 11, 0.500, 0.167,
 4, 4, 11, 15, 0.833, 0.167,
 5, 5, 15, 20, 0.833, 0.250,
 6, 3, 20, 23, 0.750, 0.583,
 7, 4, 23, 27, 0.417, 0.055,
 8, 3, 27, 30, 0.944, 0.375,
 9, 3, 30, 33, 0.625, 0.300,
 10, 3, 33, 36, 0.700, 0.167,
 11, 2, 36, 38, 0.833, 1.000,
 32000, 11,
 0, 2, 0, 2, 1.000, 0.167,
 1, 2, 3, 5, 0.833, 0.833,
 2, 3, 5, 8, 0.167, 0.500,
 3, 3, 8, 11, 0.500, 0.167,
 4, 4, 11, 15, 0.833, 0.167,
 5, 5, 15, 20, 0.833, 0.250,
 6, 4, 20, 24, 0.750, 0.250,
 7, 5, 24, 29, 0.750, 0.055,
 8, 4, 29, 33, 0.944, 0.375,
 9, 4, 33, 37, 0.625, 0.472,
 10, 3, 37, 40, 0.528, 0.937,
 11, 1, 40, 41, 0.062, 1.000,
 24000, 11,
 0, 3, 0, 4, 1, 0.166666746,
 1, 2, 4, 6, 0.833333254, 0.833333492,
 2, 3, 6, 9, 0.166666508, 0.5,
 3, 4, 9, 13, 0.5, 0.5,
 4, 5, 13, 18, 0.5, 0.833333969,
 5, 5, 18, 23, 0.166666031, 0.25,
 6, 4, 23, 27, 0.75, 0.25,
 7, 3, 27, 30, 0.75, 0.611111999,
 8, 4, 30, 34, 0.388888031, 0.208333969,
 9, 3, 34, 37, 0.791666031, 0.766667187,
 10, 4, 37, 41, 0.233332828, 0.45238167,
 11, 4, 41, 45, 0.54761833, 0.277778625,
 22050, 11,
 0, 3, 0, 4, 1, 0.166666746,
 1, 2, 4, 6, 0.833333254, 0.833333492,
 2, 3, 6, 9, 0.166666508, 0.5,
 3, 4, 9, 13, 0.5, 0.5,
 4, 4, 13, 17, 0.5, 0.5,
 5, 4, 17, 21, 0.5, 0.916666985,
 6, 4, 21, 25, 0.0833330154, 0.25,
 7, 4, 25, 29, 0.75, 0.611111999,
 8, 4, 29, 33, 0.388888031, 0.458333969,
 9, 4, 33, 37, 0.541666031, 0.633334339,
 10, 4, 37, 41, 0.366665661, 0.583334565,
 11, 4, 41, 45, 0.416665405, 0.437500954,
 16000, 11,
 0, 0, 0, 4, 1, 0.166666746,
 1, 0, 4, 6, 0.833333254, 0.833333492,
 2, 0, 6, 9, 0.166666508, 0.5,
 3, 0, 9, 13, 0.5, 0.5,
 4, 0, 13, 18, 0.5, 0.833333969,
 5, 0, 18, 23, 0.166666031, 0.75,
 6, 0, 23, 27, 0.25, 0.75,
 7, 0, 27, 31, 0.25, 0.611111999,
 8, 0, 31, 35, 0.388888031, 0.458333969,
 9, 0, 35, 39, 0.541666031, 0.166667163,
 10, 0, 39, 42, 0.833332837, 0.805555999,
 11, 0, 42, 46, 0.194444016, 0.4375};



/* ==== takehiro.c ==== */
/*
 *	MP3 huffman table selecting and bit counting
 *
 *	Copyright (c) 1999 Takehiro TOMINAGA
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include "util.h"
#include "l3side.h"
#include "tables.h"
#include "quantize-pvt.h"




struct
{
    unsigned region0_count;
    unsigned region1_count;
} subdv_table[ 23 ] =
{
{0, 0}, /* 0 bands */
{0, 0}, /* 1 bands */
{0, 0}, /* 2 bands */
{0, 0}, /* 3 bands */
{0, 0}, /* 4 bands */
{0, 1}, /* 5 bands */
{1, 1}, /* 6 bands */
{1, 1}, /* 7 bands */
{1, 2}, /* 8 bands */
{2, 2}, /* 9 bands */
{2, 3}, /* 10 bands */
{2, 3}, /* 11 bands */
{3, 4}, /* 12 bands */
{3, 4}, /* 13 bands */
{3, 4}, /* 14 bands */
{4, 5}, /* 15 bands */
{4, 5}, /* 16 bands */
{4, 6}, /* 17 bands */
{5, 6}, /* 18 bands */
{5, 6}, /* 19 bands */
{5, 7}, /* 20 bands */
{6, 7}, /* 21 bands */
{6, 7}, /* 22 bands */
};


/*************************************************************************/
/*	      ix_max							 */
/*************************************************************************/

 static int ix_max(int *ix, int *end)
{
    int max = 0;

    while (ix < end) {
	int x =	 *ix++;
	if (max < x) 
	    max = x;

	x = *ix++;
	if (max < x) 
	    max = x;
    }
    return max;
}


/*************************************************************************/
/*	      count_bit							 */
/*************************************************************************/

/*
 Function: Count the number of bits necessary to code the subregion. 
*/

static int cb_esc_buf[288];
static int cb_esc_sign;
static int *cb_esc_end;
static const int huf_tbl_noESC[15] = {
    1, 2, 5, 7, 7,10,10,13,13,13,13,13,13,13,13
};

 static int
count_bit_ESC(int *ix, int *end, int t1, int t2, int *s)
{
    /* ESC-table is used */
    int linbits1 = ht[t1].xlen;
    int linbits2 = ht[t2].xlen;
    int	sum = 0;
    int	sum1 = 0;
    int	sum2 = 0;

    while (ix < end) {
	int x = *ix++;
	int y = *ix++;

	if (x != 0) {
	    sum++;
	    if (x > 14) {
		x = 15;
		sum1 += linbits1;
		sum2 += linbits2;
	    }
	    x *= 16;
	}

	if (y != 0) {
	    sum++;
	    if (y > 14) {
		y = 15;
		sum1 += linbits1;
		sum2 += linbits2;
	    }
	    x += y;
	}

	sum1 += ht[16].hlen[x];
	sum2 += ht[24].hlen[x];
    }

    if (sum1 > sum2)  {
	sum1 = sum2;
	t1 = t2;
    }

    *s += sum + sum1;
    return t1;
}

 static int
count_bit_noESC(int *ix, int *end, unsigned int table) 
{
    /* No ESC-words */
    int	sum = 0, sign = 0;
    unsigned char *hlen = ht[table].hlen;
    int *p = cb_esc_buf;

    do {
	int x = *ix++;
	int y = *ix++;
	if (x != 0) {
	    sign++;
	    x *= 16;
	}

	if (y != 0) {
	    sign++;
	    x += y;
	}

	*p++ = x;
	sum += hlen[x];
    } while (ix < end);

    cb_esc_sign = sign;
    cb_esc_end = p;
    return sum + sign;
}



 static int
count_bit_noESC2(unsigned int table) 
{
    /* No ESC-words */
    int	sum = cb_esc_sign;
    int *p = cb_esc_buf;

    do {
	sum += ht[table].hlen[*p++];
    } while (p < cb_esc_end);

    return sum;
}



 static int
count_bit_short_ESC(int *ix, int *end, int t1, int t2, int *s)
{
    /* ESC-table is used */
    int linbits1 = ht[t1].xlen;
    int linbits2 = ht[t2].xlen;
    int	sum = 0;
    int	sum1 = 0;
    int	sum2 = 0;

    do {
	int i;
	for (i = 0; i < 3; i++) {
	    int y = *(ix + 3);
	    int x = *ix++;

	    if (x != 0) {
		sum++;
		if (x > 14) {
		    x = 15;
		    sum1 += linbits1;
		    sum2 += linbits2;
		}
		x *= 16;
	    }

	    if (y != 0) {
		sum++;
		if (y > 14) {
		    y = 15;
		    sum1 += linbits1;
		    sum2 += linbits2;
		}
		x += y;
	    }

	    sum1 += ht[16].hlen[x];
	    sum2 += ht[24].hlen[x];
	}
	ix += 3;
    } while (ix < end);

    if (sum1 > sum2)  {
	sum1 = sum2;
	t1 = t2;
    }

    *s += sum + sum1;
    return t1;
}



 static int
count_bit_short_noESC(int *ix, int *end, unsigned int table) 
{
    /* No ESC-words */
    int	sum = 0, sign = 0;
    unsigned char *hlen = ht[table].hlen;
    int *p = cb_esc_buf;

    do {
	int i;
	for (i = 0; i < 3; i++) {
	    int y = *(ix + 3);
	    int x = *ix++;
	    if (x != 0) {
		sign++;
		x *= 16;
	    }

	    if (y != 0) {
		sign++;
		x += y;
	    }

	    *p++ = x;
	    sum += hlen[x];
	}
	ix += 3;
    } while (ix < end);

    cb_esc_sign = sign;
    cb_esc_end = p;
    return sum + sign;
}



/*************************************************************************/
/*	      new_choose table						 */
/*************************************************************************/

/*
  Choose the Huffman table that will encode ix[begin..end] with
  the fewest bits.

  Note: This code contains knowledge about the sizes and characteristics
  of the Huffman tables as defined in the IS (Table B.7), and will not work
  with any arbitrary tables.
*/

static int choose_table(int *ix, int *end, int *s)
{
    int max;
    int choice0, sum0;
    int choice1, sum1;

    max = ix_max(ix, end);

    if (max > IXMAX_VAL) {
        *s = 100000;
        return -1;
    }

    if (max <= 15)  {
	if (max == 0) {
	    return 0;
	}
	/* try tables with no linbits */
	choice0 = huf_tbl_noESC[max - 1];
	sum0 = count_bit_noESC(ix, end, choice0);
	choice1 = choice0;

	switch (choice0) {
	case 7:
	case 10:
	    choice1++;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    /*fall*/
	case 2:
	case 5:
	    choice1++;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    break;

	case 13:
	    choice1 += 2;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    break;

	default:
	    break;
	}
	*s += sum0;
    } else {
	/* try tables with linbits */
	max -= 15;

	for (choice1 = 24; choice1 < 32; choice1++) {
	    if ((int)ht[choice1].linmax >= max) {
		break;
	    }
	}

	for (choice0 = choice1 - 8; choice0 < 24; choice0++) {
	    if ((int)ht[choice0].linmax >= max) {
		break;
	    }
	}

	choice0 = count_bit_ESC(ix, end, choice0, choice1, s);
    }

    return choice0;
}

static int choose_table_short(int *ix, int *end, int * s)
{
    int max;
    int choice0, sum0;
    int choice1, sum1;

    max = ix_max(ix, end);

    if (max > IXMAX_VAL) {
        *s = 100000;
        return -1;
    }

    if (max <= 15)  {
	if (max == 0) {
	    return 0;
	}
	/* try tables with no linbits */
	choice0 = huf_tbl_noESC[max - 1];
	sum0 = count_bit_short_noESC(ix, end, choice0);
	choice1 = choice0;

	switch (choice0) {
	case 7:
	case 10:
	    choice1++;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    /*fall*/
	case 2:
	case 5:
	    choice1++;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    break;

	case 13:
	    choice1 += 2;
	    sum1 = count_bit_noESC2(choice1);
	    if (sum0 > sum1) {
		sum0 = sum1;
		choice0 = choice1;
	    }
	    break;

	default:
	    break;
	}
	*s += sum0;
    } else {
	/* try tables with linbits */
	max -= 15;
	for (choice1 = 24; choice1 < 32; choice1++) {
	    if ((int)ht[choice1].linmax >= max) {
		break;
	    }
	}

	for (choice0 = choice1 - 8; choice0 < 24; choice0++) {
	    if ((int)ht[choice0].linmax >= max) {
		break;
	}
	}
	choice0 = count_bit_short_ESC(ix, end, choice0, choice1, s);
    }

    return choice0;
}



static int count_bits_long(int ix[576], gr_info *gi)
{
    int i, a1, a2;
    int bits = 0;

    i=576;
    for (; i > 1; i -= 2) 
	if (ix[i - 1] | ix[i - 2])
	    break;

    /* Determines the number of bits to encode the quadruples. */
    gi->count1 = i;
    a1 = 0;
    for (; i > 3; i -= 4) {
	int p, v;
	if ((unsigned int)(ix[i-1] | ix[i-2] | ix[i-3] | ix[i-4]) > 1)
	    break;

	v = ix[i-1];
	p = v;
	bits += v;

	v = ix[i-2];
	if (v != 0) {
	    p += 2;
	    bits++;
	}

	v = ix[i-3];
	if (v != 0) {
	    p += 4;
	    bits++;
	}

	v = ix[i-4];
	if (v != 0) {
	    p += 8;
	    bits++;
	}

	a1 += ht[32].hlen[p];
    }
    a2 = gi->count1 - i;
    if (a1 < a2) {
	bits += a1;
	gi->count1table_select = 0;
    } else {
	bits += a2;
	gi->count1table_select = 1;
    }

    gi->count1bits = bits;
    gi->big_values = i;
    if (i == 0)
	return bits;

    if (gi->block_type == NORM_TYPE) {
	int index;
	int scfb_anz = 0;

	while (scalefac_band.l[++scfb_anz] < i) 
	    ;
	index = subdv_table[scfb_anz].region0_count;
	while (scalefac_band.l[index + 1] > i)
	    index--;
	gi->region0_count = index;

	index = subdv_table[scfb_anz].region1_count;
	while (scalefac_band.l[index + gi->region0_count + 2] > i)
	    index--;
	gi->region1_count = index;

	a1 = scalefac_band.l[gi->region0_count + 1];
	a2 = scalefac_band.l[index + gi->region0_count + 2];
	gi->table_select[2] = choose_table(ix + a2, ix + i, &bits);

    } else {
	gi->region0_count = 7;
	/*gi->region1_count = SBPSY_l - 7 - 1;*/
	gi->region1_count = SBMAX_l -1 - 7 - 1;
	a1 = scalefac_band.l[7 + 1];
	a2 = i;
	if (a1 > a2) {
	    a1 = a2;
	}
    }

    /* Count the number of bits necessary to code the bigvalues region. */
    gi->table_select[0] = choose_table(ix, ix + a1, &bits);
    gi->table_select[1] = choose_table(ix + a1, ix + a2, &bits);
    return bits;
}




int count_bits(lame_global_flags *gfp,int *ix, FLOAT8 *xr, gr_info *cod_info)  
{
  int bits=0,i;
  /* since quantize_xrpow uses table lookup, we need to check this first: */
  FLOAT8 w = (IXMAX_VAL) / IPOW20(cod_info->global_gain);
  for ( i = 0; i < 576; i++ )  {
    if (xr[i] > w)
      return 100000;
  }
  if (gfp->quantization) 
    quantize_xrpow(xr, ix, cod_info);
  else
    quantize_xrpow_ISO(xr, ix, cod_info);



  if (cod_info->block_type==SHORT_TYPE) {
    cod_info->table_select[0] = choose_table_short(ix, ix + 36, &bits);
    cod_info->table_select[1] = choose_table_short(ix + 36, ix + 576, &bits);
    cod_info->big_values = 288;
  }else{
    bits=count_bits_long(ix, cod_info);
    cod_info->count1 = (cod_info->count1 - cod_info->big_values) / 4;
    cod_info->big_values /= 2;
  }
  return bits;

}

void best_huffman_divide(int gr, int ch, gr_info *gi, int *ix)
{
    int *bits, r0, r1, a1, a2, bigv;
    int r1_bits;
    int r3_bits[7 + 15 + 2 + 1];
    int r3_tbl[7 + 15 + 2 + 1];
    gr_info cod_info;

    memcpy(&cod_info, gi, sizeof(gr_info));
    bigv = cod_info.big_values * 2;
    bits = (int *) &cod_info.part2_3_length;

    for (r0 = 2; r0 < SBMAX_l + 1; r0++) {
	a2 = scalefac_band.l[r0];
	if (a2 > bigv)
	    break;

	r3_bits[r0] = cod_info.count1bits + cod_info.part2_length;
	r3_tbl[r0] = choose_table(ix + a2, ix + bigv, &r3_bits[r0]);
    }
    for (; r0 <= 7 + 15 + 2; r0++) {
	r3_bits[r0] = 100000;
    }

    for (r0 = 0; r0 < 16; r0++) {
	a1 = scalefac_band.l[r0 + 1];
	if (a1 > bigv)
	    break;
	cod_info.region0_count = r0;
	r1_bits = 0;
	cod_info.table_select[0] = choose_table(ix, ix + a1, &r1_bits);
	if ((int)gi->part2_3_length < r1_bits)
	    break;

	for (r1 = 0; r1 < 8; r1++) {
	    *bits = r1_bits + r3_bits[r0 + r1 + 2];
	    if ((int)gi->part2_3_length < *bits)
		continue;

	    a2 = scalefac_band.l[r0 + r1 + 2];

	    cod_info.table_select[1] = choose_table(ix + a1, ix + a2, bits);
	    if ((int)gi->part2_3_length < *bits)
		continue;

	    cod_info.region1_count = r1;
	    cod_info.table_select[2] = r3_tbl[r0 + r1 + 2];
	    memcpy(gi, &cod_info, sizeof(gr_info));
	}
    }
}

static void
scfsi_calc(int ch,
	   III_side_info_t *l3_side,
	   III_scalefac_t scalefac[2][2])
{
    int i, s1, s2, c1, c2;
    int sfb;
    gr_info *gi = &l3_side->gr[1].ch[ch].tt;

    static const int scfsi_band[5] = { 0, 6, 11, 16, 21 };

    static const int slen1_n[16] = { 0, 1, 1, 1, 8, 2, 2, 2, 4, 4, 4, 8, 8, 8,16,16 };
    static const int slen2_n[16] = { 0, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8, 2, 4, 8, 4, 8 };

    static const int slen1_tab[16] = { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 };
    static const int slen2_tab[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 };

    for (i = 0; i < 4; i++) 
	l3_side->scfsi[ch][i] = 0;

    for (i = 0; i < (int)(sizeof(scfsi_band) / sizeof(int)) - 1; i++) {
	for (sfb = scfsi_band[i]; sfb < scfsi_band[i + 1]; sfb++) {
	    if (scalefac[0][ch].l[sfb] != scalefac[1][ch].l[sfb])
		break;
	}
	if (sfb == scfsi_band[i + 1]) {
	    for (sfb = scfsi_band[i]; sfb < scfsi_band[i + 1]; sfb++) {
		scalefac[1][ch].l[sfb] = -1;
	    }
	    l3_side->scfsi[ch][i] = 1;
	}
    }

    s1 = c1 = 0;
    for (sfb = 0; sfb < 11; sfb++) {
	if (scalefac[1][ch].l[sfb] < 0)
	    continue;
	c1++;
	if (s1 < scalefac[1][ch].l[sfb])
	    s1 = scalefac[1][ch].l[sfb];
    }

    s2 = c2 = 0;
    for (; sfb < SBPSY_l; sfb++) {
	if (scalefac[1][ch].l[sfb] < 0)
	    continue;
	c2++;
	if (s2 < scalefac[1][ch].l[sfb])
	    s2 = scalefac[1][ch].l[sfb];
    }
    for (i = 0; i < 16; i++) {
	if (s1 < slen1_n[i] && s2 < slen2_n[i]) {
	    int c = slen1_tab[i] * c1 + slen2_tab[i] * c2;
	    if ((int)gi->part2_length > c) {
		gi->part2_length = c;
		gi->scalefac_compress = i;
	    }
	}
    }
}

void best_scalefac_store(lame_global_flags *gfp,int gr, int ch,
			 int l3_enc[2][2][576],
			 III_side_info_t *l3_side,
			 III_scalefac_t scalefac[2][2])
{
    /* use scalefac_scale if we can */
    gr_info *gi = &l3_side->gr[gr].ch[ch].tt;

    /* remove scalefacs from bands with ix=0.  This idea comes
     * from the AAC ISO docs.  added mt 3/00 */
    int sfb,i,l,start,end;
    /* check if l3_enc=0 */
    for ( sfb = 0; sfb < gi->sfb_lmax; sfb++ ) {
      if (scalefac[gr][ch].l[sfb]>0) { 
	start = scalefac_band.l[ sfb ];
	end   = scalefac_band.l[ sfb+1 ];
	for ( l = start; l < end; l++ ) if (l3_enc[gr][ch][l]!=0) break;
	if (l==end) scalefac[gr][ch].l[sfb]=0;
      }
    }
    for ( i = 0; i < 3; i++ ) {
      for ( sfb = gi->sfb_smax; sfb < SBPSY_s; sfb++ ) {
	if (scalefac[gr][ch].s[sfb][i]>0) {
	  start = scalefac_band.s[ sfb ];
	  end   = scalefac_band.s[ sfb+1 ];
	  for ( l = start; l < end; l++ ) 
	    if (l3_enc[gr][ch][3*l+i]!=0) break;
	  if (l==end) scalefac[gr][ch].s[sfb][i]=0;
        }
      }
    }


    gi->part2_3_length -= gi->part2_length;
    if (!gi->scalefac_scale && !gi->preflag) {
	u_int sfb;
	int b, s = 0;
	for (sfb = 0; sfb < gi->sfb_lmax; sfb++) {
	    s |= scalefac[gr][ch].l[sfb];
	}

	for (sfb = gi->sfb_smax; sfb < SBPSY_s; sfb++) {
	    for (b = 0; b < 3; b++) {
		s |= scalefac[gr][ch].s[sfb][b];
	    }
	}

	if (!(s & 1) && s != 0) {
	    for (sfb = 0; sfb < gi->sfb_lmax; sfb++) {
		scalefac[gr][ch].l[sfb] /= 2;
	    }
	    for (sfb = gi->sfb_smax; sfb < SBPSY_s; sfb++) {
		for (b = 0; b < 3; b++) {
		    scalefac[gr][ch].s[sfb][b] /= 2;
		}
	    }

	    gi->scalefac_scale = 1;
	    gi->part2_length = 99999999;
	    if (gfp->mode_gr == 2) {
	        scale_bitcount(&scalefac[gr][ch], gi);
	    } else {
		scale_bitcount_lsf(&scalefac[gr][ch], gi);
	    }
	}
    }

    if (gfp->mode_gr == 2 && gr == 1
	&& l3_side->gr[0].ch[ch].tt.block_type != SHORT_TYPE
	&& l3_side->gr[1].ch[ch].tt.block_type != SHORT_TYPE
	&& l3_side->gr[0].ch[ch].tt.scalefac_scale
	== l3_side->gr[1].ch[ch].tt.scalefac_scale
	&& l3_side->gr[0].ch[ch].tt.preflag
	== l3_side->gr[1].ch[ch].tt.preflag) {
      	scfsi_calc(ch, l3_side, scalefac);
    }
    gi->part2_3_length += gi->part2_length;
}


/* ==== timestatus.c ==== */
#include "timestatus.h"
#include "util.h"
#include <time.h>

#if defined(CLOCKS_PER_SEC)
/* ANSI/ISO systems */
# define TS_CLOCKS_PER_SEC CLOCKS_PER_SEC
#elif defined(CLK_TCK)
/* Non-standard systems */
# define TS_CLOCKS_PER_SEC CLK_TCK
#elif defined(HZ)
/* Older BSD systems */
# define TS_CLOCKS_PER_SEC HZ
#else
# error no suitable value for TS_CLOCKS_PER_SEC
#endif

/*********************************************************/
/* ts_real_time: real time elapsed in seconds            */
/*********************************************************/
FLOAT ts_real_time(long frame) {

  static time_t initial_time;
  time_t current_time;

  time(&current_time);

  if (frame==0) {
    initial_time = current_time;
  }

  return (FLOAT) difftime(current_time, initial_time);
}

/*********************************************************/
/* ts_process_time: process time elapsed in seconds      */
/*********************************************************/
FLOAT ts_process_time(long frame) {
  static clock_t initial_time;
  clock_t current_time;

#if ( defined(_MSC_VER) || defined(__BORLANDC__) ) 

  { static HANDLE hProcess;
    FILETIME Ignored1, Ignored2, KernelTime, UserTime;

    if ( frame==0 ) {
      hProcess = GetCurrentProcess();
    }
        
    /* GetProcessTimes() always fails under Win9x */
    if (GetProcessTimes(hProcess, &Ignored1, &Ignored2, &KernelTime, &UserTime)) {
      LARGE_INTEGER Kernel = { KernelTime.dwLowDateTime, KernelTime.dwHighDateTime };
      LARGE_INTEGER User = { UserTime.dwLowDateTime, UserTime.dwHighDateTime };

      current_time = (clock_t)((FLOAT)(Kernel.QuadPart + User.QuadPart) * TS_CLOCKS_PER_SEC / 10000000);
    } else {
      current_time = clock();
	}
  }
#else
  current_time = clock();
#endif

  if (frame==0) {
    initial_time = current_time;
  }

  return (FLOAT)(current_time - initial_time) / TS_CLOCKS_PER_SEC;
}

#undef TS_CLOCKS_PER_SEC

typedef struct ts_times {
  FLOAT so_far;
  FLOAT estimated;
  FLOAT speed;
  FLOAT eta;
} ts_times;

/*********************************************************/
/* ts_calc_times: calculate time info (eta, speed, etc.) */
/*********************************************************/
void ts_calc_times(ts_times *time, int samp_rate, long frame, long frames,int framesize)
{
  if (frame > 0) {
    time->estimated = time->so_far * frames / frame;
    if (samp_rate * time->estimated > 0) {
      time->speed = frames * framesize / (samp_rate * time->estimated);
    } else {
      time->speed = 0;
    }
    time->eta = time->estimated - time->so_far;
  } else {
    time->estimated = 0;
	time->speed = 0;
	time->eta = 0;
  }
}

/*********************************************************/
/* timestatus: display encoding process time information */
/*********************************************************/
void timestatus(int samp_rate,long frameNum,long totalframes,int framesize)
{
  ts_times real_time, process_time;
  int percent;

  real_time.so_far = ts_real_time(frameNum);
  process_time.so_far = ts_process_time(frameNum);

  if (frameNum == 0) {
    fprintf(stderr, "    Frame          |  CPU/estimated  |  time/estimated | play/CPU |   ETA\n");
    return;
  }  

  ts_calc_times(&real_time, samp_rate, frameNum, totalframes, framesize);
  ts_calc_times(&process_time, samp_rate, frameNum, totalframes, framesize);

  if (totalframes > 1) {
    percent = (int)(100.0 * frameNum / (totalframes - 1));
  } else {
    percent = 100;
  }

#  define TS_TIME_DECOMPOSE(time) \
    (int)((long)(time+.5) / 3600), \
    (int)((long)((time+.5) / 60) % 60), \
    (int)((long)(time+.5) % 60)

  fprintf(stderr,
    "\r%6ld/%6ld(%3d%%)|%2d:%02d:%02d/%2d:%02d:%02d|%2d:%02d:%02d/%2d:%02d:%02d|%10.4f|%2d:%02d:%02d ",
    frameNum,
    totalframes - 1,
    percent,
    TS_TIME_DECOMPOSE(process_time.so_far),
    TS_TIME_DECOMPOSE(process_time.estimated),
    TS_TIME_DECOMPOSE(real_time.so_far),
	TS_TIME_DECOMPOSE(real_time.estimated),
    process_time.speed,
    TS_TIME_DECOMPOSE(real_time.eta)
  );

  fflush(stderr);
}



/* ==== util.c ==== */
#include "util.h"
#include <assert.h>

/***********************************************************************
*
*  Global Variable Definitions
*
***********************************************************************/


/* 1: MPEG-1, 0: MPEG-2 LSF, 1995-07-11 shn */
FLOAT8  s_freq_table[2][4] = {{22.05, 24, 16, 0}, {44.1, 48, 32, 0}};

/* 1: MPEG-1, 0: MPEG-2 LSF, 1995-07-11 shn */
int     bitrate_table[2][15] = {
          {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
          {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}};


enum byte_order NativeByteOrder = order_unknown;

/***********************************************************************
*
*  Global Function Definitions
*
***********************************************************************/


/***********************************************************************
 * compute bitsperframe and mean_bits for a layer III frame 
 **********************************************************************/
void getframebits(lame_global_flags *gfp,int *bitsPerFrame, int *mean_bits) {
  int whole_SpF;
  FLOAT8 bit_rate,samp;
  int bitsPerSlot;
  int sideinfo_len;
  
  samp =      gfp->out_samplerate/1000.0;
  bit_rate = bitrate_table[gfp->version][gfp->bitrate_index];
  bitsPerSlot = 8;

  /* determine the mean bitrate for main data */
  sideinfo_len = 32;
  if ( gfp->version == 1 )
    {   /* MPEG 1 */
      if ( gfp->stereo == 1 )
	sideinfo_len += 136;
      else
	sideinfo_len += 256;
    }
  else
    {   /* MPEG 2 */
      if ( gfp->stereo == 1 )
	sideinfo_len += 72;
      else
	sideinfo_len += 136;
    }
  
  if (gfp->error_protection) sideinfo_len += 16;
  
  /* -f fast-math option causes some strange rounding here, be carefull: */  
  whole_SpF = floor( (gfp->framesize /samp)*(bit_rate /  (FLOAT8)bitsPerSlot) + 1e-9);
  *bitsPerFrame = 8 * whole_SpF + (gfp->padding * 8);
  *mean_bits = (*bitsPerFrame - sideinfo_len) / gfp->mode_gr;
}




void display_bitrates(FILE *out_fh)
{
  int index,version;

  version = 1;
  fprintf(out_fh,"\n");
  fprintf(out_fh,"MPEG1 samplerates(kHz): 32 44.1 48 \n");

  fprintf(out_fh,"bitrates(kbs): ");
  for (index=1;index<15;index++) {
    fprintf(out_fh,"%i ",bitrate_table[version][index]);
  }
  fprintf(out_fh,"\n");
  
  
  version = 0;
  fprintf(out_fh,"\n");
  fprintf(out_fh,"MPEG2 samplerates(kHz): 16 22.05 24 \n");
  fprintf(out_fh,"bitrates(kbs): ");
  for (index=1;index<15;index++) {
    fprintf(out_fh,"%i ",bitrate_table[version][index]);
  }
  fprintf(out_fh,"\n");
}


int BitrateIndex(
int bRate,        /* legal rates from 32 to 448 */
int version,      /* MPEG-1 or MPEG-2 LSF */
int samplerate)   /* convert bitrate in kbps to index */
{
int     index = 0;
int     found = 0;

    while(!found && index<15)   {
        if(bitrate_table[version][index] == bRate)
            found = 1;
        else
            ++index;
    }
    if(found)
        return(index);
    else {
        fprintf(stderr,"Bitrate %dkbs not legal for %iHz output sampling.\n",
                bRate, samplerate);
        return(-1);     /* Error! */
    }
}

int SmpFrqIndex(  /* convert samp frq in Hz to index */
long sRate,             /* legal rates 16000, 22050, 24000, 32000, 44100, 48000 */
int  *version)
{
	/* Assign default value */
	*version=0;

    if (sRate == 44100L) {
        *version = 1; return(0);
    }
    else if (sRate == 48000L) {
        *version = 1; return(1);
    }
    else if (sRate == 32000L) {
        *version = 1; return(2);
    }
    else if (sRate == 24000L) {
        *version = 0; return(1);
    }
    else if (sRate == 22050L) {
        *version = 0; return(0);
    }
    else if (sRate == 16000L) {
        *version = 0; return(2);
    }
    else {
        fprintf(stderr, "SmpFrqIndex: %ldHz is not a legal sample rate\n", sRate);
        return(-1);     /* Error! */
    }
}

/*******************************************************************************
*
*  Allocate number of bytes of memory equal to "block".
*
*******************************************************************************/
/* exit(0) changed to exit(1) on memory allocation
 * error -- 1999/06 Alvaro Martinez Echevarria */

void  *mem_alloc(unsigned long block, char *item)
{

    void    *ptr;

    /* what kind of shit does ISO put out?  */
    ptr = (void *) malloc((size_t) block /* <<1 */ ); /* allocate twice as much memory as needed. fixes dodgy
					    memory problem on most systems */


    if (ptr != NULL) {
        memset(ptr, 0, (size_t) block);
    } else {
        fprintf(stderr,"Unable to allocate %s\n", item);
        exit(1);
    }
    return(ptr);
}



/*****************************************************************************
*
*  Routines to determine byte order and swap bytes
*
*****************************************************************************/

enum byte_order DetermineByteOrder(void)
{
    char s[ sizeof(long) + 1 ];
    union
    {
        long longval;
        char charval[ sizeof(long) ];
    } probe;
    probe.longval = 0x41424344L;  /* ABCD in ASCII */
    strncpy( s, probe.charval, sizeof(long) );
    s[ sizeof(long) ] = '\0';
    /* fprintf( stderr, "byte order is %s\n", s ); */
    if ( strcmp(s, "ABCD") == 0 )
        return order_bigEndian;
    else
        if ( strcmp(s, "DCBA") == 0 )
            return order_littleEndian;
        else
            return order_unknown;
}

void SwapBytesInWords( short *loc, int words )
{
    int i;
    short thisval;
    char *dst, *src;
    src = (char *) &thisval;
    for ( i = 0; i < words; i++ )
    {
        thisval = *loc;
        dst = (char *) loc++;
        dst[0] = src[1];
        dst[1] = src[0];
    }
}






/*****************************************************************************
*
*  bit_stream.c package
*  Author:  Jean-Georges Fritsch, C-Cube Microsystems
*
*****************************************************************************/

/********************************************************************
  This package provides functions to write (exclusive or read)
  information from (exclusive or to) the bit stream.

  If the bit stream is opened in read mode only the get functions are
  available. If the bit stream is opened in write mode only the put
  functions are available.
********************************************************************/

/*alloc_buffer();      open and initialize the buffer;                    */
/*desalloc_buffer();   empty and close the buffer                         */
/*back_track_buffer();     goes back N bits in the buffer                 */
/*unsigned int get1bit();  read 1 bit from the bit stream                 */
/*unsigned long look_ahead(); grep the next N bits in the bit stream without*/
/*                            changing the buffer pointer                   */
/*putbits(); write N bits from the bit stream */
/*int seek_sync(); return 1 if a sync word was found in the bit stream      */
/*                 otherwise returns 0                                      */



void empty_buffer(Bit_stream_struc *bs)
{
   int minimum=1+bs->buf_byte_idx;    /* end of the buffer to empty */
   if (bs->buf_size-minimum <= 0) return;
   bs->buf_byte_idx = bs->buf_size -1;
   bs->buf_bit_idx = 8;

   bs->buf[bs->buf_byte_idx] = 0;  /* what does this do? */

}
int copy_buffer(char *buffer,int size,Bit_stream_struc *bs)
{
  int i,j=0;
  if (size!=0 && (bs->buf_size-1 - bs->buf_byte_idx) > size ) return -1;
  for (i=bs->buf_size-1 ; i > bs->buf_byte_idx ; (i-- ))
    buffer[j++]=bs->buf[i];
  assert(j == (bs->buf_size-1 - bs->buf_byte_idx));
  empty_buffer(bs);  /* empty buffer, (changes bs->buf_size) */
  return j;
}





void init_bit_stream_w(Bit_stream_struc* bs)
{
   alloc_buffer(bs, BUFFER_SIZE);
   bs->buf_byte_idx = BUFFER_SIZE-1;
   bs->buf_bit_idx=8;
   bs->totbit=0;
}


/*open and initialize the buffer; */
void alloc_buffer(
Bit_stream_struc *bs,   /* bit stream structure */
int size)
{
   bs->buf = (unsigned char *)
	mem_alloc((unsigned long) (size * sizeof(unsigned char)), "buffer");
   bs->buf_size = size;
}

/*empty and close the buffer */
void desalloc_buffer(Bit_stream_struc *bs)   /* bit stream structure */
{
   free(bs->buf);
}

int putmask[9]={0x0, 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff};


/*write N bits into the bit stream */
void putbits(
Bit_stream_struc *bs,   /* bit stream structure */
unsigned int val,       /* val to write into the buffer */
int N)                  /* number of bits of val */
{
 register int j = N;
 register int k, tmp;

 if (N > MAX_LENGTH)
    fprintf(stderr,"Cannot read or write more than %d bits at a time.\n", MAX_LENGTH);

 bs->totbit += N;
 while (j > 0) {
   k = Min(j, bs->buf_bit_idx);
   tmp = val >> (j-k);
   bs->buf[bs->buf_byte_idx] |= (tmp&putmask[k]) << (bs->buf_bit_idx-k);
   bs->buf_bit_idx -= k;
   if (!bs->buf_bit_idx) {
       bs->buf_bit_idx = 8;
       bs->buf_byte_idx--;
       assert(bs->buf_byte_idx >= 0);
       bs->buf[bs->buf_byte_idx] = 0;
   }
   j -= k;
 }
}



/*****************************************************************************
*
*  End of bit_stream.c package
*
*****************************************************************************/



/* ==== vbrquantize.c ==== */
/*
 *	MP3 quantization
 *
 *	Copyright (c) 1999 Mark Taylor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <assert.h>
#include "util.h"
#include "l3side.h"
#include "quantize.h"
#include "l3bitstream.h"
#include "reservoir.h"
#include "quantize-pvt.h"
#ifdef HAVEGTK
#include "gtkanal.h"
#endif



#define DEBUGXX
FLOAT8 calc_sfb_ave_noise(FLOAT8 *xr, FLOAT8 *xr34, int stride, int bw, FLOAT8 sfpow)
{
  int j;
  FLOAT8 xfsf=0;
  FLOAT8 sfpow34 = pow(sfpow,3.0/4.0);

  for ( j=0; j < stride*bw ; j += stride) {
    int ix;
    FLOAT8 temp,temp2;

    /*    ix=(int)( xr34[j]/sfpow34  + 0.4054);*/
    ix=floor( xr34[j]/sfpow34);
    if (ix > IXMAX_VAL) return -1.0;

    temp = fabs(xr[j])- pow43[ix]*sfpow;
    if (ix < IXMAX_VAL) {
      temp2 = fabs(xr[j])- pow43[ix+1]*sfpow;
      if (fabs(temp2)<fabs(temp)) temp=temp2;
    }
#ifdef MAXQUANTERROR
    temp *= temp;
    xfsf = bw*Max(xfsf,temp);
#else
    xfsf += temp * temp;
#endif
  }
  return xfsf/bw;
}



FLOAT8 find_scalefac(FLOAT8 *xr,FLOAT8 *xr34,int stride,int sfb,
		     FLOAT8 l3_xmin,int bw)
{
  FLOAT8 xfsf,sfpow,sf,sf_ok,delsf;
  int sf4,sf_ok4,delsf4;
  int i;

  /* search will range from sf:  -52.25 -> 11.25  */
  /* search will range from sf4:  -209 -> 45  */
  sf = -20.5;
  sf4 = -82;
  delsf = 32;
  delsf4 = 128;

  sf_ok =10000; 
  sf_ok4=10000;
  for (i=0; i<7; i++) {
    delsf /= 2;
    delsf4 /= 2;
    sfpow = pow(2.0,sf);
    /* sfpow = pow(2.0,sf4/4.0); */
    xfsf = calc_sfb_ave_noise(xr,xr34,stride,bw,sfpow);

    if (xfsf < 0) {
      /* scalefactors too small */
      sf += delsf; 
      sf4 += delsf4;
    }else{
      if (sf_ok==10000) sf_ok=sf;  
      if (sf_ok4==10000) sf_ok4=sf4;  
      if (xfsf > l3_xmin)  {
	/* distortion.  try a smaller scalefactor */
	sf -= delsf;
	sf4 -= delsf4;
      }else{
	sf_ok=sf;
	sf_ok4 = sf4;
	sf += delsf;
	sf4 += delsf4;
      }
    }
  } 
  /* sf_ok accurate to within +/- 2*final_value_of_delsf */
  assert(sf_ok!=10000);

  /* NOTE: noise is not a monotone function of the sf, even though
   * the number of bits used is!  do a brute force search in the 
   * neighborhood of sf_ok: 
   * 
   *  sf = sf_ok + 1.75     works  1% of the time 
   *  sf = sf_ok + 1.50     works  1% of the time 
   *  sf = sf_ok + 1.25     works  2% of the time 
   *  sf = sf_ok + 1.00     works  3% of the time 
   *  sf = sf_ok + 0.75     works  9% of the time 
   *  sf = sf_ok + 0.50     0 %  (because it was tried above)
   *  sf = sf_ok + 0.25     works 39% of the time 
   *  sf = sf_ok + 0.00     works the rest of the time
   */

  sf = sf_ok + 0.75;
  sf4 = sf_ok4 + 3;

  while (sf>(sf_ok+.01)) { 
    /* sf = sf_ok + 2*delsf was tried above, skip it:  */
    if (fabs(sf-(sf_ok+2*delsf))  < .01) sf -=.25;
    if (sf4 == sf_ok4+2*delsf4) sf4 -=1;

    sfpow = pow(2.0,sf);
    /* sfpow = pow(2.0,sf4/4.0) */
    xfsf = calc_sfb_ave_noise(xr,xr34,stride,bw,sfpow);
    if (xfsf > 0) {
      if (xfsf <= l3_xmin) return sf;
    }
    sf -= .25;
    sf4 -= 1;
  }
  return sf_ok;
}



/*
    sfb=0..5  scalefac < 16 
    sfb>5     scalefac < 8

    ifqstep = ( cod_info->scalefac_scale == 0 ) ? .5 : 1.0;
    ol_sf =  (cod_info->global_gain-210.0)/4.0;
    ol_sf -= 2*cod_info->subblock_gain[i];
    ol_sf -= ifqstep*scalefac[gr][ch].s[sfb][i];
*/
FLOAT8 compute_scalefacs_short(FLOAT8 vbrsf[SBPSY_s][3],gr_info *cod_info,int scalefac[SBPSY_s][3])
{
  FLOAT8 maxrange,maxover;
  FLOAT8 sf[SBPSY_s][3];
  int sfb,i;
  int ifqstep_inv = ( cod_info->scalefac_scale == 0 ) ? 2 : 1;

  /* make a working copy of the desired scalefacs */
  memcpy(sf,vbrsf,SBPSY_s*3*sizeof(FLOAT8));

  /* see if we should use subblock gain */


  maxover=0;
  for ( sfb = 0; sfb < SBPSY_s; sfb++ ) {
    for (i=0; i<3; ++i) {
      /* ifqstep*scalefac + 2*subblock_gain >= -sf[sfb] */
      scalefac[sfb][i]=floor( -sf[sfb][i]*ifqstep_inv  +.75 + .0001)   ;
      
      if (sfb < 6) maxrange = 15.0/ifqstep_inv;
      else maxrange = 7.0/ifqstep_inv;
      
      if (maxrange + sf[sfb][i] > maxover) maxover = maxrange+sf[sfb][i];
    }
  }
  return maxover;
}




/*
	  sfb=0..10  scalefac < 16 
	  sfb>10     scalefac < 8
		
	  ifqstep = ( cod_info->scalefac_scale == 0 ) ? .5 : 1.0;
	  ol_sf =  (cod_info->global_gain-210.0)/4.0;
	  ol_sf -= ifqstep*scalefac[gr][ch].l[sfb];
	  if (cod_info->preflag && sfb>=11) 
	  ol_sf -= ifqstep*pretab[sfb];
*/
FLOAT8 compute_scalefacs_long(FLOAT8 vbrsf[SBPSY_l],gr_info *cod_info,int scalefac[SBPSY_l])
{
  int sfb;
  FLOAT8 sf[SBPSY_l];
  FLOAT8 maxrange,maxover;
  int ifqstep_inv = ( cod_info->scalefac_scale == 0 ) ? 2 : 1;

  /* make a working copy of the desired scalefacs */
  memcpy(sf,vbrsf,SBPSY_l*sizeof(FLOAT8));

  cod_info->preflag=0;
  for ( sfb = 11; sfb < SBPSY_l; sfb++ ) {
    if (sf[sfb] + pretab[sfb]/ifqstep_inv > 0) break;
  }
  if (sfb==SBPSY_l) {
    cod_info->preflag=1;
    for ( sfb = 11; sfb < SBPSY_l; sfb++ ) 
      sf[sfb] += pretab[sfb]/ifqstep_inv;
  }

  maxover=0;
  for ( sfb = 0; sfb < SBPSY_l; sfb++ ) {
    /* ifqstep*scalefac >= -sf[sfb] */
    scalefac[sfb]=floor( -sf[sfb]*ifqstep_inv  +.75 + .0001)   ;

    if (sfb < 11) maxrange = 15.0/ifqstep_inv;
    else maxrange = 7.0/ifqstep_inv;

    if (maxrange + sf[sfb] > maxover) maxover = maxrange+sf[sfb];
  }
  return maxover;
}
  
  



/************************************************************************
 *
 * VBR_iteration_loop()   
 *
 *
 ************************************************************************/
void
VBR_iteration_loop_new (lame_global_flags *gfp,
                FLOAT8 pe[2][2], FLOAT8 ms_ener_ratio[2],
                FLOAT8 xr[2][2][576], III_psy_ratio ratio[2][2],
                III_side_info_t * l3_side, int l3_enc[2][2][576],
                III_scalefac_t scalefac[2][2])
{
  III_psy_xmin l3_xmin[2][2];
  FLOAT8    masking_lower_db;
  int       start,end,bw,sfb, i,ch, gr, over;
  III_psy_xmin vbrsf;
  FLOAT8 vbrmax;


  iteration_init(gfp,l3_side,l3_enc);

  /* Adjust allowed masking based on quality setting */
  /* db_lower varies from -10 to +8 db */
  masking_lower_db = -10 + 2*gfp->VBR_q;
  /* adjust by -6(min)..0(max) depending on bitrate */
  masking_lower = pow(10.0,masking_lower_db/10);
  masking_lower = 1;


  for (gr = 0; gr < gfp->mode_gr; gr++) {
    if (convert_mdct)
      ms_convert(xr[gr],xr[gr]);
    for (ch = 0; ch < gfp->stereo; ch++) { 
      FLOAT8 xr34[576];
      gr_info *cod_info = &l3_side->gr[gr].ch[ch].tt;
      int shortblock;
      over = 0;
      shortblock = (cod_info->block_type == SHORT_TYPE);

      for(i=0;i<576;i++) {
	FLOAT8 temp=fabs(xr[gr][ch][i]);
	xr34[i]=sqrt(sqrt(temp)*temp);
      }

      calc_xmin( gfp,xr[gr][ch], &ratio[gr][ch], cod_info, &l3_xmin[gr][ch]);

      vbrmax=0;
      if (shortblock) {
	for ( sfb = 0; sfb < SBPSY_s; sfb++ )  {
	  for ( i = 0; i < 3; i++ ) {
	    start = scalefac_band.s[ sfb ];
	    end   = scalefac_band.s[ sfb+1 ];
	    bw = end - start;
	    vbrsf.s[sfb][i] = find_scalefac(&xr[gr][ch][3*start+i],&xr34[3*start+i],3,sfb,
		   masking_lower*l3_xmin[gr][ch].s[sfb][i],bw);
	    if (vbrsf.s[sfb][i]>vbrmax) vbrmax=vbrsf.s[sfb][i];
	  }
	}
      }else{
	for ( sfb = 0; sfb < SBPSY_l; sfb++ )   {
	  start = scalefac_band.l[ sfb ];
	  end   = scalefac_band.l[ sfb+1 ];
	  bw = end - start;
	  vbrsf.l[sfb] = find_scalefac(&xr[gr][ch][start],&xr34[start],1,sfb,
	  		 masking_lower*l3_xmin[gr][ch].l[sfb],bw);
	  if (vbrsf.l[sfb]>vbrmax) vbrmax = vbrsf.l[sfb];
	}

      } /* compute scalefactors */

      /* sf =  (cod_info->global_gain-210.0)/4.0; */
      cod_info->global_gain = floor(4*vbrmax +210 + .5);


      if (shortblock) {
	for ( sfb = 0; sfb < SBPSY_s; sfb++ ) {
	  for ( i = 0; i < 3; i++ ) {
	    vbrsf.s[sfb][i] -= vbrmax;
	  }
	}
	cod_info->scalefac_scale = 0;
	if (compute_scalefacs_short(vbrsf.s,cod_info,scalefac[gr][ch].s) > 0) {
	  cod_info->scalefac_scale = 1;
	  if (compute_scalefacs_short(vbrsf.s,cod_info,scalefac[gr][ch].s) >0) {
	    /* what do we do now? */
	    exit(32);
	  }
	}
      }else{
	for ( sfb = 0; sfb < SBPSY_l; sfb++ )   
	  vbrsf.l[sfb] -= vbrmax;

	/* can we get away with scalefac_scale=0? */
	cod_info->scalefac_scale = 0;
	if (compute_scalefacs_long(vbrsf.l,cod_info,scalefac[gr][ch].l) > 0) {
	  cod_info->scalefac_scale = 1;
	  if (compute_scalefacs_long(vbrsf.l,cod_info,scalefac[gr][ch].l) >0) {
	    /* what do we do now? */
	    exit(32);
	  }
	}
      } 
    } /* ch */
  } /* gr */
}





/* ==== VbrTag.c ==== */
/*
 *	Xing VBR tagging for LAME.
 *
 *	Copyright (c) 1999 A.L. Faber
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "machine.h"
#include <math.h>
#include "VbrTag.h"
#include "version.h"

#ifdef _DEBUG
/*  #define DEBUG_VBRTAG */
#endif


int SizeOfEmptyFrame[2][2]=
{
	{32,17},
	{17,9},
};

static u_char pbtStreamBuffer[216];   
static long g_Position[NUMTOCENTRIES];
static int nZeroStreamSize=0;
static int TotalFrameSize=0;
static char	VBRTag[]={"Xing"};


int* pVbrFrames=NULL;
int nVbrNumFrames=0;
int nVbrFrameBufferSize=0;

/****************************************************************************
 * AddVbrFrame: Add VBR entry, used to fill the VBR the TOC entries
 * Paramters:
 *	nStreamPos: how many bytes did we write to the bitstream so far
 *				(in Bytes NOT Bits)
 ****************************************************************************
*/
void AddVbrFrame(int nStreamPos)
{
        /* Simple exponential growing buffer */
	if (pVbrFrames==NULL || nVbrFrameBufferSize==0)
	{
                /* Start with 100 frames */
		nVbrFrameBufferSize=100;

		/* Allocate them */
		pVbrFrames=(int*)malloc((size_t)(nVbrFrameBufferSize*sizeof(int)));
	}

	/* Is buffer big enough to store this new frame */
	if (nVbrNumFrames==nVbrFrameBufferSize)
	{
                /* Guess not, double th e buffer size */
		nVbrFrameBufferSize*=2;

		/* Allocate new buffer */
		pVbrFrames=(int*)realloc(pVbrFrames,(size_t)(nVbrFrameBufferSize*sizeof(int)));
	}

	/* Store values */
	pVbrFrames[nVbrNumFrames++]=nStreamPos;
}


/*-------------------------------------------------------------*/
static int ExtractI4(unsigned char *buf)
{
	int x;
	/* big endian extract */
	x = buf[0];
	x <<= 8;
	x |= buf[1];
	x <<= 8;
	x |= buf[2];
	x <<= 8;
	x |= buf[3];
	return x;
}

void CreateI4(unsigned char *buf, int nValue)
{
        /* big endian create */
	buf[0]=(nValue>>24)&0xff;
	buf[1]=(nValue>>16)&0xff;
	buf[2]=(nValue>> 8)&0xff;
	buf[3]=(nValue    )&0xff;
}


/*-------------------------------------------------------------*/
/* Same as GetVbrTag below, but only checks for the Xing tag.
   requires buf to contain only 40 bytes */
/*-------------------------------------------------------------*/
int CheckVbrTag(unsigned char *buf)
{
	int			h_id, h_mode, h_sr_index;

	/* get selected MPEG header data */
	h_id       = (buf[1] >> 3) & 1;
	h_sr_index = (buf[2] >> 2) & 3;
	h_mode     = (buf[3] >> 6) & 3;

	/*  determine offset of header */
	if( h_id ) 
	{
                /* mpeg1 */
		if( h_mode != 3 )	buf+=(32+4);
		else				buf+=(17+4);
	}
	else
	{
                /* mpeg2 */
		if( h_mode != 3 ) buf+=(17+4);
		else              buf+=(9+4);
	}

	if( buf[0] != VBRTag[0] ) return 0;    /* fail */
	if( buf[1] != VBRTag[1] ) return 0;    /* header not found*/
	if( buf[2] != VBRTag[2] ) return 0;
	if( buf[3] != VBRTag[3] ) return 0;
	return 1;
}

int GetVbrTag(VBRTAGDATA *pTagData,  unsigned char *buf)
{
	int			i, head_flags;
	int			h_id, h_mode, h_sr_index;
	static int	sr_table[4] = { 44100, 48000, 32000, 99999 };

	/* get Vbr header data */
	pTagData->flags = 0;     

	/* get selected MPEG header data */
	h_id       = (buf[1] >> 3) & 1;
	h_sr_index = (buf[2] >> 2) & 3;
	h_mode     = (buf[3] >> 6) & 3;

	/*  determine offset of header */
	if( h_id ) 
	{
                /* mpeg1 */
		if( h_mode != 3 )	buf+=(32+4);
		else				buf+=(17+4);
	}
	else
	{
                /* mpeg2 */
		if( h_mode != 3 ) buf+=(17+4);
		else              buf+=(9+4);
	}

	if( buf[0] != VBRTag[0] ) return 0;    /* fail */
	if( buf[1] != VBRTag[1] ) return 0;    /* header not found*/
	if( buf[2] != VBRTag[2] ) return 0;
	if( buf[3] != VBRTag[3] ) return 0;

	buf+=4;

	pTagData->h_id = h_id;

	pTagData->samprate = sr_table[h_sr_index];

	if( h_id == 0 )
		pTagData->samprate >>= 1;

	head_flags = pTagData->flags = ExtractI4(buf); buf+=4;      /* get flags */

	if( head_flags & FRAMES_FLAG )
	{
		pTagData->frames   = ExtractI4(buf); buf+=4;
	}

	if( head_flags & BYTES_FLAG )
	{
		pTagData->bytes = ExtractI4(buf); buf+=4;
	}

	if( head_flags & TOC_FLAG )
	{
		if( pTagData->toc != NULL )
		{
			for(i=0;i<NUMTOCENTRIES;i++)
				pTagData->toc[i] = buf[i];
		}
		buf+=NUMTOCENTRIES;
	}

	pTagData->vbr_scale = -1;

	if( head_flags & VBR_SCALE_FLAG )
	{
		pTagData->vbr_scale = ExtractI4(buf); buf+=4;
	}

#ifdef DEBUG_VBRTAG
	printf("\n\n********************* VBR TAG INFO *****************\n");
	printf("tag         :%s\n",VBRTag);
	printf("head_flags  :%d\n",head_flags);
	printf("bytes       :%d\n",pTagData->bytes);
	printf("frames      :%d\n",pTagData->frames);
	printf("VBR Scale   :%d\n",pTagData->vbr_scale);
	printf("toc:\n");
	if( pTagData->toc != NULL )
	{
		for(i=0;i<NUMTOCENTRIES;i++)
		{
			if( (i%10) == 0 ) printf("\n");
			printf(" %3d", (int)(pTagData->toc[i]));
		}
	}
	printf("\n***************** END OF VBR TAG INFO ***************\n");
#endif
	return 1;       /* success */
}


/****************************************************************************
 * InitVbrTag: Initializes the header, and write empty frame to stream
 * Paramters:
 *				fpStream: pointer to output file stream
 *				nVersion: 0= MPEG1 1=MPEG2
 *				nMode	: Channel Mode: 0=STEREO 1=JS 2=DS 3=MONO
 ****************************************************************************
*/
int InitVbrTag(Bit_stream_struc* pBs,int nVersion, int nMode, int SampIndex)
{
	int i;

	/* Clear Frame position array variables */
	pVbrFrames=NULL;
	nVbrNumFrames=0;
	nVbrFrameBufferSize=0;

	/* Clear struct */
	memset(g_Position,0x00,sizeof(g_Position));

	/* Clear stream buffer */
	memset(pbtStreamBuffer,0x00,sizeof(pbtStreamBuffer));

	/* Set TOC values to 255 */
	for (i=0;i<NUMTOCENTRIES;i++)
	{
		g_Position[i]=-1;
	}



	/* Reserve the proper amount of bytes */
	if (nMode==3)
	{
		nZeroStreamSize=SizeOfEmptyFrame[nVersion][1]+4;
	}
	else
	{
		nZeroStreamSize=SizeOfEmptyFrame[nVersion][0]+4;
	}

	/*
	// Xing VBR pretends to be a 48kbs layer III frame.  (at 44.1kHz).
        // (at 48kHz they use 56kbs since 48kbs frame not big enough for 
        // table of contents)
	// let's always embed Xing header inside a 64kbs layer III frame.  
	// this gives us enough room for a LAME version string too.
	// size determined by sampling frequency (MPEG1)
	// 32kHz:    216 bytes@48kbs    288bytes@ 64kbs    
	// 44.1kHz:  156 bytes          208bytes@64kbs     (+1 if padding = 1)
	// 48kHz:    144 bytes          192
	// 
	// MPEG 2 values are the since the framesize and samplerate
        // are each reduced by a factor of 2.
	*/
	{
	int tot;
	static const int framesize[3]={208,192,288};  /* 64kbs MPEG1 or MPEG2  framesize */
	/* static int framesize[3]={156,144,216}; */ /* 48kbs framesize */
	
	if (SampIndex>2) {
	  fprintf(stderr,"illegal sampling frequency index\n");
	  exit(-1);
	}
	TotalFrameSize= framesize[SampIndex];
	tot = (nZeroStreamSize+VBRHEADERSIZE);
	tot += 20;  /* extra 20 bytes for LAME & version string */
	
	if (TotalFrameSize < tot ) {
	  fprintf(stderr,"Xing VBR header problem...use -t\n");
	  exit(-1);
	}
	}


	/* Put empty bytes into the bitstream */
	for (i=0;i<TotalFrameSize;i++)
	{
                /* Write a byte to the bitstream */
		putbits(pBs,0,8);
	}

	/* Success */
	return 0;
}



/****************************************************************************
 * PutVbrTag: Write final VBR tag to the file
 * Paramters:
 *				lpszFileName: filename of MP3 bit stream
 *				nVersion: 0= MPEG1 1=MPEG2
 *				nVbrScale	: encoder quality indicator (0..100)
 ****************************************************************************
*/
int PutVbrTag(char* lpszFileName,int nVbrScale,int nVersion)
{
	int			i;
	long lFileSize;
	int nStreamIndex;
	char abyte;
	u_char		btToc[NUMTOCENTRIES];
	FILE *fpStream;
	char str1[80];


	if (nVbrNumFrames==0 || pVbrFrames==NULL)
		return -1;

	/* Open the bitstream again */
	fpStream=fopen(lpszFileName,"rb+");

	/* Assert stream is valid */
	if (fpStream==NULL)
		return -1;

	/* Clear stream buffer */
	memset(pbtStreamBuffer,0x00,sizeof(pbtStreamBuffer));

	/* Seek to end of file*/
	fseek(fpStream,0,SEEK_END);

	/* Get file size */
	lFileSize=ftell(fpStream);
	
	/* Abort if file has zero length. Yes, it can happen :) */
	if (lFileSize==0)
		return -1;

	/* Seek to first real frame */
	fseek(fpStream,(long)TotalFrameSize,SEEK_SET);

	/* Read the header (first valid frame) */
	fread(pbtStreamBuffer,4,1,fpStream);

	/* the default VBR header.  48kbs layer III, no padding, no crc */
	/* but sampling freq, mode andy copyright/copy protection taken */
	/* from first valid frame */
	pbtStreamBuffer[0]=(u_char) 0xff;    
	if (nVersion==0) {
	  pbtStreamBuffer[1]=(u_char) 0xfb;    
	  abyte = pbtStreamBuffer[2] & (char) 0x0c;   
	  pbtStreamBuffer[2]=(char) 0x50 | abyte;     /* 64kbs MPEG1 frame */
	}else{
	  pbtStreamBuffer[1]=(u_char) 0xf3;    
	  abyte = pbtStreamBuffer[2] & (char) 0x0c;   
	  pbtStreamBuffer[2]=(char) 0x80 | abyte;     /* 64kbs MPEG2 frame */
	}


	/*Seek to the beginning of the stream */
	fseek(fpStream,0,SEEK_SET);

	/* Clear all TOC entries */
	memset(btToc,0,sizeof(btToc));

        for (i=1;i<NUMTOCENTRIES;i++) /* Don't touch zero point... */
        {
                /* Calculate frame from given percentage */
                int frameNum=(int)(floor(0.01*i*nVbrNumFrames));

                /*  Calculate relative file postion, normalized to 0..256!(?) */
                float fRelStreamPos=(float)256.0*(float)pVbrFrames[frameNum]/(float)lFileSize;

                /* Just to be safe */
                if (fRelStreamPos>255) fRelStreamPos=255;

                /* Assign toc entry value */
                btToc[i]=(u_char) fRelStreamPos;
        }



	/* Start writing the tag after the zero frame */
	nStreamIndex=nZeroStreamSize;

	/* Put Vbr tag */
	pbtStreamBuffer[nStreamIndex++]=VBRTag[0];
	pbtStreamBuffer[nStreamIndex++]=VBRTag[1];
	pbtStreamBuffer[nStreamIndex++]=VBRTag[2];
	pbtStreamBuffer[nStreamIndex++]=VBRTag[3];

	/* Put header flags */
	CreateI4(&pbtStreamBuffer[nStreamIndex],FRAMES_FLAG+BYTES_FLAG+TOC_FLAG+VBR_SCALE_FLAG);
	nStreamIndex+=4;

	/* Put Total Number of frames */
	CreateI4(&pbtStreamBuffer[nStreamIndex],nVbrNumFrames);
	nStreamIndex+=4;

	/* Put Total file size */
	CreateI4(&pbtStreamBuffer[nStreamIndex],(int)lFileSize);
	nStreamIndex+=4;

	/* Put TOC */
	memcpy(&pbtStreamBuffer[nStreamIndex],btToc,sizeof(btToc));
	nStreamIndex+=sizeof(btToc);

	/* Put VBR SCALE */
	CreateI4(&pbtStreamBuffer[nStreamIndex],nVbrScale);
	nStreamIndex+=4;

	/* Put LAME id */
	sprintf(str1,"LAME%s",get_lame_version());
	strncpy((char *)&pbtStreamBuffer[nStreamIndex],str1,(size_t) 20);
	nStreamIndex+=20;


#ifdef DEBUG_VBRTAG
{
	VBRTAGDATA TestHeader;
	GetVbrTag(&TestHeader,pbtStreamBuffer);
}
#endif

        /* Put it all to disk again */
	if (fwrite(pbtStreamBuffer,TotalFrameSize,1,fpStream)!=1)
	{
		return -1;
	}
	fclose(fpStream);

	/* Save to delete the frame buffer */
	free(pVbrFrames);
	pVbrFrames=NULL;

	return 0;       /* success */
}

/*-------------------------------------------------------------*/
int SeekPoint(unsigned char TOC[NUMTOCENTRIES], int file_bytes, float percent)
{
/* interpolate in TOC to get file seek point in bytes */
int a, seekpoint;
float fa, fb, fx;


if( percent < (float)0.0 )   percent = (float)0.0;
if( percent > (float)100.0 ) percent = (float)100.0;

a = (int)percent;
if( a > 99 ) a = 99;
fa = TOC[a];
if( a < 99 ) {
    fb = TOC[a+1];
}
else {
    fb = (float)256.0;
}


fx = fa + (fb-fa)*(percent-a);

seekpoint = (int)(((float)(1.0/256.0))*fx*file_bytes); 


return seekpoint;
}
/*-------------------------------------------------------------*/


/* ==== version.c ==== */
/*
 *	Version numbering for LAME.
 *
 *	Copyright (c) 1999 A.L. Faber
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include "version.h"
#include "lame.h"
#include <stdio.h>

static char lpszVersion[80];

void lame_print_version(FILE *ofile) {
  fprintf(ofile,"LAME version %s (www.sulaco.org/mp3) \n",get_lame_version());
  fprintf(ofile,"GPSYCHO: GPL psycho-acoustic and noise shaping model version %s. \n",get_psy_version());
#ifdef LIBSNDFILE
  fprintf(ofile,"Input handled by libsndfile (www.zip.com.au/~erikd/libsndfile)\n");
#endif
}


char* get_lame_version(void)
{
	if (LAME_ALPHAVERSION>0)
		sprintf(lpszVersion,"%d.%02d (alpha %d)",LAME_MAJOR_VERSION,LAME_MINOR_VERSION,LAME_ALPHAVERSION);
	else if (LAME_BETAVERSION>0)
		sprintf(lpszVersion,"%d.%02d (beta %d)",LAME_MAJOR_VERSION,LAME_MINOR_VERSION,LAME_BETAVERSION);
	else
		sprintf(lpszVersion,"%d.%02d",LAME_MAJOR_VERSION,LAME_MINOR_VERSION);
	return lpszVersion;
}

char* get_psy_version(void)
{
	if (PSY_ALPHAVERSION>0)
		sprintf(lpszVersion,"%d.%02d (alpha %d)",PSY_MAJOR_VERSION,PSY_MINOR_VERSION,PSY_ALPHAVERSION);
	else if (PSY_BETAVERSION>0)
		sprintf(lpszVersion,"%d.%02d (beta %d)",PSY_MAJOR_VERSION,PSY_MINOR_VERSION,PSY_BETAVERSION);
	else
		sprintf(lpszVersion,"%d.%02d",PSY_MAJOR_VERSION,PSY_MINOR_VERSION);
	return lpszVersion;
}

char* get_mp3x_version(void)
{
	if (MP3X_ALPHAVERSION>0)
		sprintf(lpszVersion,"%d:%02d (alpha %d)",MP3X_MAJOR_VERSION,MP3X_MINOR_VERSION,MP3X_ALPHAVERSION);
	else if (MP3X_BETAVERSION>0)
		sprintf(lpszVersion,"%d:%02d (beta %d)",MP3X_MAJOR_VERSION,MP3X_MINOR_VERSION,MP3X_BETAVERSION);
	else
		sprintf(lpszVersion,"%d:%02d",MP3X_MAJOR_VERSION,MP3X_MINOR_VERSION);
	return lpszVersion;
}
