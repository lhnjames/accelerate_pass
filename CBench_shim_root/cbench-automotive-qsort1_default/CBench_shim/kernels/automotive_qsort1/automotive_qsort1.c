
#include <polybench.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define UNLIMIT
#define MAXARRAY 60000 /* this number, if too large, will cause a seg. fault!! */

void qsortx(void *base, unsigned num, unsigned width,
            int (*comp)(const void *, const void *));

struct my3DVertexStruct {
  int x, y, z;
  double distance;
};

int compare(const void *elem1, const void *elem2)
{
  /* D = [(x1 - x2)^2 + (y1 - y2)^2 + (z1 - z2)^2]^(1/2) */
  /* sort based on distances from the origin... */

  double distance1, distance2;

  distance1 = (*((struct my3DVertexStruct *)elem1)).distance;
  distance2 = (*((struct my3DVertexStruct *)elem2)).distance;

  return (distance1 > distance2) ? 1 : ((distance1 == distance2) ? 0 : -1);
}

struct my3DVertexStruct array[MAXARRAY];

int
kernel_automotive_qsort1(int argc, char *argv[], int print) {
  FILE* fmisc=NULL;
  FILE *fp;
  int i,count=0;
  long x=0;
  long y=0;
  long z=0;

  if (argc<2) {
    fprintf(stderr,"Usage: qsort_large <file>\n");
    exit(EXIT_FAILURE);
  }
  else {
    fp = fopen(argv[1],"r");

    while((fscanf(fp, "%d", &x) == 1) && (fscanf(fp, "%d", &y) == 1) && (fscanf(fp, "%d", &z) == 1) &&  (count < MAXARRAY)) {
	 array[count].x = x;
	 array[count].y = y;
	 array[count].z = z;
	 array[count].distance = sqrt(pow(x, 2) + pow(y, 2) + pow(z, 2));
	 count++;
    }

    fclose(fp);
  }
  if (print==1) {
      printf("\nSorting %d vectors based on distance from the origin.\n\n",
             count);
  }
  qsortx(array,count,sizeof(struct my3DVertexStruct),compare);

  if (print==1) {
      if ((fmisc=fopen("tmp-output.tmp","wt"))==NULL)
          {
              fprintf(stderr,"\nError: Can't open output file\n");
              exit(EXIT_FAILURE);
          }
   for(i=0;i<count;i+=count/100)
     fprintf(fmisc, "%d %d %d\n", array[i].x, array[i].y, array[i].z);
  
   fclose(fmisc);
  }
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
  char* fargv[] = { "kernel_automotive_qsort1", "/home/hanning/ctuning-datasets-min/dataset/cdataset-qsort-0001/data.txt" , NULL };
  int fargc = 2;
  const char* out_path = "/home/hanning/comet/tmp/automotive_qsort1_out.tmp";

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
  kernel_automotive_qsort1(fargc, fargv, 1);
  fflush(stdout);
  polybench_stop_instruments;

  dup2(_saved_stdout, 1);
  close(_saved_stdout);
  polybench_print_instruments;

  double chk = _checksum_file(out_path);
  polybench_prevent_dce(_print_checksum(chk));
  return 0;
}
