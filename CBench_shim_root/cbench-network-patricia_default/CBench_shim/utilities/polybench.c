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


/* ==== patricia.c ==== */
/*
 * patricia.c
 *
 * Patricia trie implementation.
 *
 * Functions for inserting nodes, removing nodes, and searching in
 * a Patricia trie designed for IP addresses and netmasks.  A
 * head node must be created with (key,mask) = (0,0).
 *
 * NOTE: The fact that we keep multiple masks per node makes this
 *       more complicated/computationally expensive then a standard
 *       trie.  This is because we need to do longest prefix matching,
 *       which is useful for computer networks, but not as useful
 *       elsewhere.
 *
 * Matthew Smart <mcsmart@eecs.umich.edu>
 *
 * Copyright (c) 2000
 * The Regents of the University of Michigan
 * All rights reserved
 *
 * $Id: patricia.c,v 1.1.1.1 2000/11/06 19:53:17 mguthaus Exp $
 */

#include <stdlib.h>	/* free(), malloc() */
#include <string.h>	/* memmove() */
#include "patricia.h"


/*
 * Private function used to return whether
 * or not bit 'i' is set in 'key'.
 */
static __inline
unsigned long
bit(int i, unsigned long key)
{
	return key & (1 << (31-i));
}


/*
 * Count the number of masks (and therefore entries)
 * in the Patricia trie.
 */
static int
pat_count(struct ptree *t, int b)
{
	int count;
	
	if (t->p_b <= b) return 0;

	count = t->p_mlen;
	
	count += pat_count(t->p_left,  t->p_b);
	count += pat_count(t->p_right, t->p_b);

	return count;
}


/*
 * Private function used for inserting a node recursively.
 */
static struct ptree *
insertR(struct ptree *h, struct ptree *n, int d, struct ptree *p)
{
	if ((h->p_b >= d) || (h->p_b <= p->p_b)) {
		n->p_b = d;
		n->p_left = bit(d, n->p_key) ? h : n;
		n->p_right = bit(d, n->p_key) ? n : h;
		return n;
	}

	if (bit(h->p_b, n->p_key))
		h->p_right = insertR(h->p_right, n, d, h);
	else
		h->p_left = insertR(h->p_left, n, d, h);
	return h;
}


/*
 * Patricia trie insert.
 *
 * 1) Go down to leaf.
 * 2) Determine longest prefix match with leaf node.
 * 3) Insert new internal node at appropriate location and
 *    attach new external node.
 */
struct ptree *
pat_insert(struct ptree *n, struct ptree *head)
{
	struct ptree *t;
	struct ptree_mask *buf, *pm;
	int i, copied;

	if (!head || !n || !n->p_m)
		return 0;

	/*
	 * Make sure the key matches the mask.
	 */
	n->p_key &= n->p_m->pm_mask;

	/*
	 * Find closest matching leaf node.
	 */
	t = head;
	do {
		i = t->p_b;
		t = bit(t->p_b, n->p_key) ? t->p_right : t->p_left;
	} while (i < t->p_b);

	/*
	 * If the keys are the same we need to check the masks.
	 */
	if (n->p_key == t->p_key) {
		/*
		 * If we have a duplicate mask, replace the entry
		 * with the new one.
		 */
		for (i=0; i < t->p_mlen; i++) {
			if (n->p_m->pm_mask == t->p_m[i].pm_mask) {
				t->p_m[i].pm_data = n->p_m->pm_data;
				free(n->p_m);
				free(n);
				n = 0;
				return t;
			}
		}
		
		/*
		 * Allocate space for a new set of masks.
		 */
		buf = (struct ptree_mask *)malloc(
		       sizeof(struct ptree_mask)*(t->p_mlen+1));

		/*
		 * Insert the new mask in the proper order from least
		 * to greatest mask.
		 */
		copied = 0;
		for (i=0, pm=buf; i < t->p_mlen; pm++) {
			if (n->p_m->pm_mask > t->p_m[i].pm_mask) {
                            memmove(pm, t->p_m + i, sizeof(struct ptree_mask));
				i++;
			}
			else {
                            memmove(pm, n->p_m, sizeof(struct ptree_mask));
				n->p_m->pm_mask = 0xffffffff;
				copied = 1;
			}
		}
		if (!copied) {
                    memmove(pm, n->p_m, sizeof(struct ptree_mask));
		}
		free(n->p_m);
		free(n);
		n = 0;
		t->p_mlen++;

		/*
		 * Free old masks and point to new ones.
		 */
		free(t->p_m);
		t->p_m = buf;
		
		return t;
	}

	/*
	 * Find the first bit that differs.
	 */
	for (i=1; i < 32 && bit(i, n->p_key) == bit(i, t->p_key); i++); 

	/*
	 * Recursive step.
	 */
	if (bit(head->p_b, n->p_key))
		head->p_right = insertR(head->p_right, n, i, head);
	else
		head->p_left = insertR(head->p_left, n, i, head);
	
	return n;
}


/*
 * Remove an entry given a key in a Patricia trie.
 */
int
pat_remove(struct ptree *n, struct ptree *head)
{
	struct ptree *p, *g, *pt, *pp, *t;
	struct ptree_mask *buf, *pm;
	int i;

	if (!n || !n->p_m || !t)
		return 0;

	/*
	 * Search for the target node, while keeping track of the
	 * parent and grandparent nodes.
	 */
	g = p = t = head;
	do {
		i = t->p_b;
		g = p;
		p = t;
		t = bit(t->p_b, n->p_key) ? t->p_right : t->p_left;
	} while (i < t->p_b);

	/*
	 * For removal, we need an exact match.
	 */
	if (t->p_key != n->p_key)
		return 0;

	/*
	 * If there is only 1 mask, we can remove the entire node.
	 */
	if (t->p_mlen == 1) {
		/*
		 * Don't allow removal of the default entry.
		 */
		if (t->p_b == 0)
			return 0;
		
		/*
		 * Must match on the mask.
		 */
		if (t->p_m->pm_mask != n->p_m->pm_mask)
			return 0;
		
		/*
		 * Search for the node that points to the parent, so
		 * we can make sure it doesn't get lost.
		 */
		pp = pt = p;
		do {
			i = pt->p_b;
			pp = pt;
			pt = bit(pt->p_b, p->p_key) ? pt->p_right : pt->p_left;
		} while (i < pt->p_b);

		if (bit(pp->p_b, p->p_key))
			pp->p_right = t;
		else
			pp->p_left = t;

		/*
		 * Point the grandparent to the proper node.
		 */
		if (bit(g->p_b, n->p_key))
			g->p_right = bit(p->p_b, n->p_key) ?
				p->p_left : p->p_right;
		else
			g->p_left = bit(p->p_b, n->p_key) ?
				p->p_left : p->p_right;
	
		/*
		 * Delete the target's data and copy in its parent's
		 * data, but not the bit value.
		 */
		if (t->p_m->pm_data)
			free(t->p_m->pm_data);
		free(t->p_m);
		if (t != p) {
			t->p_key = p->p_key;
			t->p_m = p->p_m;
			t->p_mlen = p->p_mlen;
		}
		free(p);

		return 1;
	}

	/*
	 * Multiple masks, so we need to find the one to remove.
	 * Return if we don't match on any of them.
	 */
	for (i=0; i < t->p_mlen; i++)
		if (n->p_m->pm_mask == t->p_m[i].pm_mask)
			break;
	if (i >= t->p_mlen)
		return 0;
	
	/*
	 * Allocate space for a new set of masks.
	 */
	buf = (struct ptree_mask *)malloc(
	       sizeof(struct ptree_mask)*(t->p_mlen-1));

	for (i=0, pm=buf; i < t->p_mlen; i++) {
		if (n->p_m->pm_mask != t->p_m[i].pm_mask) {
                    memmove(pm++, t->p_m + i, sizeof(struct ptree_mask));
		}
	}
		
	/*
	 * Free old masks and point to new ones.
	 */
	t->p_mlen--;
	free(t->p_m);
	t->p_m = buf;
	return 1;
}


/*
 * Find an entry given a key in a Patricia trie.
 */
struct ptree *
pat_search(unsigned long key, struct ptree *head)
{
	struct ptree *p = 0, *t = head;
	int i;
	
	if (!t)
		return 0;

	/*
	 * Find closest matching leaf node.
	 */
	do {
		/*
		 * Keep track of most complete match so far.
		 */
		if (t->p_key == (key & t->p_m->pm_mask)) {
			p = t;
		}
		
		i = t->p_b;
		t = bit(t->p_b, key) ? t->p_right : t->p_left;
	} while (i < t->p_b);

	/*
	 * Compare keys (and masks) to see if this
	 * is really the node we want.
	 */
	return (t->p_key == (key & t->p_m->pm_mask)) ? t : p;
}
