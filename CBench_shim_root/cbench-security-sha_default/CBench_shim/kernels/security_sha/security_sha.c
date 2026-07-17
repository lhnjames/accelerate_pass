
#include <stdio.h>
#include <unistd.h>

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

  char* fargv[] = { "kernel_security_sha", "/home/hanning/ctuning-datasets-min/dataset/txt-0001/data.txt" , NULL };
  int fargc = 2;

  kernel_security_sha(fargc, fargv, 1);

  return 0;
}
