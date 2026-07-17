
#include <polybench.h>
#include <unistd.h>
#include <fcntl.h>

/* testc - Test adpcm coder */

#include "adpcm.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef XOPENME
#include <xopenme.h>
#endif

struct adpcm_state state;

#define NSAMPLES 1000

char	abuf[NSAMPLES/2];
short	sbuf[NSAMPLES];

int kernel_telecom_adpcm_c() {
    long ct_repeat=0;
    long ct_repeat_max=1;
    int ct_return=0;
    int n;

#ifdef XOPENME
  xopenme_init(1,0);
#endif

    if (getenv("CT_REPEAT_MAIN")!=NULL) ct_repeat_max=atol(getenv("CT_REPEAT_MAIN"));

#ifdef XOPENME
  xopenme_clock_start(0);
#endif

    while(1) {
        struct adpcm_state current_state = state;

	n = read(0, sbuf, NSAMPLES*2);
	if ( n < 0 ) {
	    perror("input file");
	    exit(1);
	}
	if ( n == 0 ) break;

        /* loop_wrap */
        for (ct_repeat=0; ct_repeat<ct_repeat_max; ct_repeat++)
        {
	  /* The call to adpcm_coder modifies the state. We need to make a
	     copy of the state and to restore it before each iteration of the
	     kernel to make sure we do not alter the output of the
	     application. */
          state = current_state;
  	  adpcm_coder(sbuf, abuf, n/2, &state);  /* modifies the state */
	}

	write(1, abuf, n/4);
    }

#ifdef XOPENME
  xopenme_clock_end(0);

  xopenme_dump_state();
  xopenme_finish();
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
  char* fargv[] = { "kernel_telecom_adpcm_c", "/home/hanning/ctuning-datasets-min/dataset/pcm-0001/data.pcm" , NULL };
  int fargc = 2;
  const char* out_path = "/home/hanning/comet/tmp/telecom_adpcm_c_out.tmp";
  if (!freopen("/home/hanning/ctuning-datasets-min/dataset/pcm-0001/data.pcm", "r", stdin)) return 1;

  /* Some cBench programs write their real output to stdout instead of (or
     as well as) an explicit output file argument. Redirect fd 1 to out_path
     for the duration of the call so that data doesn't collide with
     polybench_print_instruments' own stdout float print, then restore the
     original stdout to print the timing. */
  fflush(stdout);
  int _saved_stdout = dup(1);
  int _out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (_out_fd >= 0) { dup2(_out_fd, 1); close(_out_fd); }

  polybench_start_instruments;
  kernel_telecom_adpcm_c();
  fflush(stdout);
  polybench_stop_instruments;

  dup2(_saved_stdout, 1);
  close(_saved_stdout);
  polybench_print_instruments;

  double chk = _checksum_file(out_path);
  polybench_prevent_dce(_print_checksum(chk));
  return 0;
}
