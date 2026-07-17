
#include <stdio.h>
#include <unistd.h>

/* testd - Test adpcm decoder */

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

int kernel_telecom_adpcm_d()
{
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

	n = read(0, abuf, NSAMPLES/2);
	if ( n < 0 ) {
	    perror("input file");
	    exit(1);
	}
	if ( n == 0 ) break;
  
        /* gfursin */
        for (ct_repeat=0; ct_repeat<ct_repeat_max; ct_repeat++)
        {
	  /* The call to adpcm_decoder modifies the state. We need to make a
	     copy of the state and to restore it before each iteration of the
	     kernel to make sure we do not alter the output of the
	     application. */
          state = current_state;
  	  adpcm_decoder(abuf, sbuf, n*2, &state);  /* modifies state */
	}

	write(1, sbuf, n*4);
    }

#ifdef XOPENME
  xopenme_clock_end(0);

  xopenme_dump_state();
  xopenme_finish();
#endif

    return 0;
}


static void _cat_file_to_stdout(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f) return;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
  fclose(f);
  remove(path);
}

int main(int argc, char** argv)
{

  char* fargv[] = { "kernel_telecom_adpcm_d", "/home/hanning/ctuning-datasets-min/dataset/adpcm-0001/data.adpcm" , NULL };
  int fargc = 2;
  if (!freopen("/home/hanning/ctuning-datasets-min/dataset/adpcm-0001/data.adpcm", "r", stdin)) return 1;

  kernel_telecom_adpcm_d();

  return 0;
}
