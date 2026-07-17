#ifndef SPEC
#define SPEC 1
#endif
#ifndef NDEBUG
#define NDEBUG 1
#endif

#include <polybench.h>
#include <unistd.h>
#include <fcntl.h>

/**************************************************************************
MCF.H of ZIB optimizer MCF, SPEC version

Dres. Loebel, Borndoerfer & Weider GbR (LBW)
Churer Zeile 15, 12205 Berlin

Konrad-Zuse-Zentrum fuer Informationstechnik Berlin (ZIB)
Scientific Computing - Optimization
Takustr. 7, 14195 Berlin

This software was developed at ZIB Berlin. Maintenance and revisions 
solely on responsibility of LBW

Copyright (c) 1998-2000 ZIB.           
Copyright (c) 2000-2002 ZIB & Loebel.  
Copyright (c) 2003-2005 Loebel.
Copyright (c) 2006-2010 LBW.
**************************************************************************/
/*  LAST EDIT: Wed May 26 00:06:03 2010 by Loebel (opt0.zib.de)  */
/*  $Id: mcf.c,v 1.17 2010/05/26 08:26:29 bzfloebe Exp $  */



#include "mcf.h"
#include "time.h"

#define REPORT

extern LONG min_impl_duration;
network_t net;





#ifdef _PROTO_
LONG global_opt( void )
#else
LONG global_opt( )
#endif
{
  LONG new_arcs;
  LONG residual_nb_it = 1;
  
  
  new_arcs = -1;

  while( new_arcs )
  {
#ifdef REPORT
    printf( "active arcs                : %" PRId64 "\n", net.m );
#endif
    
    primal_net_simplex( &net );
    
#ifdef REPORT
    printf( "simplex iterations         : %" PRId64 "\n", net.iterations );
    printf( "objective value            : %0.0f\n", flow_cost(&net) );
#endif
    
    
#if defined AT_HOME
    printf( "iterations                 : %ld\n", residual_nb_it );
#endif
    
    
    if( net.m_impl )
    {
      new_arcs = suspend_impl( &net, (cost_t)-1, 0 );
      
      if ( new_arcs < 0)
      {
#ifdef REPORT
        printf( "not enough memory, exit(-1)\n" );
#endif
        
        exit(-1);
      }
#ifdef REPORT
      if( new_arcs )
        printf( "erased arcs                : %" PRId64 "\n", new_arcs );
#endif
    }
    else 
    {
      refreshPositions(&net, &getOriginalArcPosition,net.m);
    }
    
    
    new_arcs = price_out_impl( &net );
    
#ifdef REPORT
    if( new_arcs )
      printf( "new implicit arcs          : %" PRId64 "\n", new_arcs );
#endif
    
    if( new_arcs < 0 )
    {
#ifdef REPORT
      printf( "not enough memory, exit(-1)\n" );
#endif
      
      exit(-1);
    }
    
#ifndef REPORT
    printf( "\n" );
#endif
    
    residual_nb_it++;
  }
  
  net.optcost = flow_cost(&net);
  printf( "checksum                   : %0.0f\n", net.optcost );
  
  return 0;
}






#ifdef _PROTO_
int kernel_mcf_r( int argc, char *argv[] )
#else
int kernel_mcf_r( argc, argv )
    int argc;
    char *argv[];
#endif
{
  int outnum; 
  char outfile[80];
  if( argc < 2 )
    return -1;
  
#ifndef SPEC
  time_t startTime, endTime;
  time(&startTime);
#endif
  
  printf( "\nMCF SPEC CPU version 1.11\n" );
  printf( "Copyright (c) 1998-2000 Zuse Institut Berlin (ZIB)\n" );
  printf( "Copyright (c) 2000-2002 Andreas Loebel & ZIB\n" );
  printf( "Copyright (c) 2003-2005 Andreas Loebel\n" );
  printf( "Copyright (c) 2006-2010 Dres. Loebel, Borndoerfer & Weider "\
          "GbR (LBW)\n" );
  printf( "\n" );
  
  //omp_set_num_threads(1);
  
  memset( (void *)(&net), 0, (size_t)sizeof(network_t) );
  net.bigM = (LONG)BIGM;
  
  
  strcpy( net.inputfile, argv[1] );
  if (argc == 3) {
     outnum = atoi(argv[2]);
     sprintf(outfile,"mcf.%d.out",outnum);
  } else {
     strcpy(outfile,"mcf.out"); 
  }  
 
  if( read_min( &net ) )
  {
    printf( "read error, exit\n" );
    getfree( &net );
    return -1;
  }
  
#ifndef SPEC
#ifdef _OPENMP
  printf( "number of threads          : %d\n", omp_get_max_threads() );
#else
  printf( "single threaded\n" );
#endif
#endif
  
#if defined(REPORT) || defined(SPEC)
  printf( "nodes                      : %" PRId64 "\n", net.n_trips );
#endif
  
  
  primal_start_artificial( &net );
  global_opt( );
  
  
  
  if( write_objective_value( outfile, &net ) )
  {
    getfree( &net );
    return -1;    
  }
  
  
  getfree( &net );
#ifndef SPEC
  time(&endTime);
  printf("runtime = %ld seconds\n",endTime - startTime);
#endif
  
  
#ifdef REPORT
  printf( "done\n" );
#endif
  
  
  return 0;
}


static double _checksum_file(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f) return -1.0;
  double sum = 0.0;
  long i = 0;
  int c;
  while ((c = fgetc(f)) != EOF) { sum += (double)c * (1.0 + (double)(i % 97)); i++; }
  fclose(f);
  return sum;
}

static void _print_checksum(double chk)
{
  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("checksum");
  fprintf(POLYBENCH_DUMP_TARGET, "%.6f", chk);
  POLYBENCH_DUMP_END("checksum");
  POLYBENCH_DUMP_FINISH;
}

int main(int argc, char** argv)
{
  (void)argc; (void)argv;
  char* fargv[] = { "kernel_mcf_r", "/home/hanning/spec2017/benchspec/CPU/505.mcf_r/data/test/input/inp.in", "" , NULL };
  int fargc = 3;
  const char* out_path = "/home/hanning/comet/tmp/mcf_r_stdout.tmp";

  fflush(stdout);
  int _saved_stdout = dup(1);
  int _out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (_out_fd >= 0) { dup2(_out_fd, 1); close(_out_fd); }

  polybench_start_instruments;
  kernel_mcf_r(fargc, fargv);
  fflush(stdout);
  polybench_stop_instruments;

  dup2(_saved_stdout, 1);
  close(_saved_stdout);
  polybench_print_instruments;

  double chk = _checksum_file(out_path);
  polybench_prevent_dce(_print_checksum(chk));
  return 0;
}
