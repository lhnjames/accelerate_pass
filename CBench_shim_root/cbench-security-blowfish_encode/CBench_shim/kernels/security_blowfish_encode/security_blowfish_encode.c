/* Adapted from cBench's security_blowfish (src/bf.c), which is itself the
 * original CLI driver (Usage: blowfish {e|d} <input> <output> key) built on
 * OpenSSL's crypto/bf (BF_set_key + BF_cfb64_encrypt, CFB64 mode). The only
 * change from the original: dropped the "_finfo_dataset"-based loop_wrap
 * repeat-count file (an internal cBench convention for artificially
 * inflating tiny inputs' runtime, not used by any other port in this
 * corpus) -- every chunk is now encrypted exactly once, same as every other
 * cbench shim in this project. Renamed main -> kernel_security_blowfish_encode,
 * with a wrapper main() supplying a fixed dataset (matching the pattern used
 * by security_rijndael_encode.c): output written to a temp file then dumped
 * to stdout so the encrypted bytes themselves are what gets correctness-
 * checked, not just an exit code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "blowfish.h"

int kernel_security_blowfish_encode(int argc, char *argv[])
{
    BF_KEY key;
    unsigned char ukey[32];
    unsigned char indata[40], outdata[40], ivec[32] = {0};
    int num = 0;
    int by = 0, i = 0;
    int encordec = -1;
    char *cp, ch;
    FILE *fp, *fp2;

    if (argc < 5) {
        fprintf(stderr, "Usage: blowfish {e|d} <input> <output> key\n");
        exit(EXIT_FAILURE);
    }

    if (*argv[1] == 'e' || *argv[1] == 'E')
        encordec = 1;
    else if (*argv[1] == 'd' || *argv[1] == 'D')
        encordec = 0;
    else {
        fprintf(stderr, "Usage: blowfish {e|d} <input> <output> key\n");
        exit(EXIT_FAILURE);
    }

    cp = argv[4];
    while (i < 64 && *cp) {
        ch = toupper((unsigned char)*cp++);
        if (ch >= '0' && ch <= '9')
            by = (by << 4) + ch - '0';
        else if (ch >= 'A' && ch <= 'F')
            by = (by << 4) + ch - 'A' + 10;
        else {
            printf("key must be in hexadecimal notation\n");
            exit(EXIT_FAILURE);
        }
        if (i++ & 1)
            ukey[i / 2 - 1] = by & 0xff;
    }

    BF_set_key(&key, 8, ukey);

    if (*cp) {
        printf("Bad key value.\n");
        exit(EXIT_FAILURE);
    }

    if ((fp = fopen(argv[2], "r")) == 0) {
        fprintf(stderr, "could not open input file: %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }
    if ((fp2 = fopen(argv[3], "w")) == 0) {
        fprintf(stderr, "could not open output file: %s\n", argv[3]);
        exit(EXIT_FAILURE);
    }

    i = 0;
    while (!feof(fp)) {
        int j;
        while (!feof(fp) && i < 40)
            indata[i++] = getc(fp);

        BF_cfb64_encrypt(indata, outdata, i, &key, ivec, &num, encordec);

        for (j = 0; j < i; j++)
            fputc(outdata[j], fp2);
        i = 0;
    }

    fclose(fp);
    fclose(fp2);
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
  char out_path[512];
  snprintf(out_path, sizeof(out_path), "/tmp/security_blowfish_encode_out_%d.tmp", (int)getpid());

  char* fargv[] = { "kernel_security_blowfish_encode", "e",
                     "/home/hanning/ctuning-datasets-min/dataset/enc-0001/data.enc",
                     out_path,
                     "1234567890abcdeffedcba0987654321", NULL };
  int fargc = 5;

  kernel_security_blowfish_encode(fargc, fargv);
  _cat_file_to_stdout(out_path);

  return 0;
}
