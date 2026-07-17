
#include <polybench.h>
#include <unistd.h>
#include <fcntl.h>

/* NIST Secure Hash Algorithm */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sha.h"

int kernel_security_sha(int argc, char **argv, int print)
{
    FILE *fin;
    SHA_INFO sha_info;

    if (argc < 2) {
	fin = stdin;
        sha_stream(&sha_info, fin);
        if (print)
            sha_print(&sha_info);
    } else {
	while (--argc) {
	    fin = fopen(*(++argv), "rb");
	    if (fin == NULL) {
		fprintf(stderr, "error opening %s for reading\n", *argv);
                exit(EXIT_FAILURE);
	    } else {
                sha_stream(&sha_info, fin);
		if (print)
                    sha_print(&sha_info);
		fclose(fin);
	    }
	}
    }
    return(0);
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
  char* fargv[] = { "kernel_security_sha", "/home/hanning/ctuning-datasets-min/dataset/txt-0001/data.txt" , NULL };
  int fargc = 2;
  const char* out_path = "/home/hanning/comet/tmp/security_sha_out.tmp";

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
  kernel_security_sha(fargc, fargv, 1);
  fflush(stdout);
  polybench_stop_instruments;

  dup2(_saved_stdout, 1);
  close(_saved_stdout);
  polybench_print_instruments;

  double chk = _checksum_file(out_path);
  polybench_prevent_dce(_print_checksum(chk));
  return 0;
}
