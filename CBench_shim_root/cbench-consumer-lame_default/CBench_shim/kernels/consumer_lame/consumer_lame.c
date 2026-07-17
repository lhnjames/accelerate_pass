
#include <polybench.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#ifdef _WIN32
/* needed to set stdout to binary */
#include <io.h>
#endif
#include "lame.h"


#ifdef HAVEGTK
#include "gtkanal.h"
#include <gtk/gtk.h>
#endif

#ifdef __riscos__
#include "asmstuff.h"
#endif




/************************************************************************
*
* main
*
* PURPOSE:  MPEG-1,2 Layer III encoder with GPSYCHO
* psychoacoustic model.
*
************************************************************************/


int kernel_consumer_lame(int argc, char **argv)
{

  char mp3buffer[LAME_MAXMP3BUFFER];
  short int Buffer[2][1152];
  int iread,imp3;
  lame_global_flags gf;
  FILE *outf = NULL;
#ifdef __riscos__
  int i;
#endif


  lame_init(&gf);                  /* initialize libmp3lame */
  if(argc==1) lame_usage(&gf,argv[0]);  /* no command-line args, print usage, exit  */

  /* parse the command line arguments, setting various flags in the
   * struct 'gf'.  If you want to parse your own arguments,
   * or call libmp3lame from a program which uses a GUI to set arguments,
   * skip this call and set the values of interest in the gf struct.
   * (see lame.h for documentation about these parameters)
   */
  lame_parse_args(&gf,argc, argv);

  if (!gf.gtkflag) {
    /* open the MP3 output file */
    if (!strcmp(gf.outPath, "-")) {
#ifdef __EMX__
      _fsetmode(stdout,"b");
#elif (defined  __BORLANDC__)
      setmode(_fileno(stdout), O_BINARY);
#elif (defined  __CYGWIN__)
      setmode(fileno(stdout), _O_BINARY);
#elif (defined _WIN32)
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      outf = stdout;
    } else {
      if ((outf = fopen(gf.outPath, "wb")) == NULL) {
	fprintf(stderr,"Could not create \"%s\".\n", gf.outPath);
	exit(1);
      }
    }
#ifdef __riscos__
    /* Assign correct file type */
    for (i = 0; gf.outPath[i]; i++)
      if (gf.outPath[i] == '.') gf.outPath[i] = '/';
    SetFiletype(gf.outPath, 0x1ad);
#endif
  }


  /* open the wav/aiff/raw pcm or mp3 input file.  This call will
   * open the file with name gf.inFile, try to parse the headers and
   * set gf.samplerate, gf.num_channels, gf.num_samples.
   * if you want to do your own file input, skip this call and set
   * these values yourself.
   */
  lame_init_infile(&gf);

  /* Now that all the options are set, lame needs to analyze them and
   * set some more options
   */
  lame_init_params(&gf);
  lame_print_config(&gf);   /* print usefull information about options being used */




#ifdef HAVEGTK
  if (gf.gtkflag) gtk_init (&argc, &argv);
  if (gf.gtkflag) gtkcontrol(&gf);
  else
#endif
    {

      /* encode until we hit eof */
      do {
	/* read in 'iread' samples */
	iread=lame_readframe(&gf,Buffer);


	/* encode */
	imp3=lame_encode_buffer(&gf,Buffer[0],Buffer[1],iread,
              mp3buffer,(int)sizeof(mp3buffer)); 

	/* was our output buffer big enough? */
	if (imp3==-1) {
	  fprintf(stderr,"mp3 buffer is not big enough... \n");
	  exit(1);
	}

	if (fwrite(mp3buffer,1,imp3,outf) != imp3) {
	  fprintf(stderr,"Error writing mp3 output");
	  exit(1);
	}
      } while (iread);
    }

  imp3=lame_encode_finish(&gf,mp3buffer,(int)sizeof(mp3buffer));   /* may return one more mp3 frame */
  fwrite(mp3buffer,1,imp3,outf);
  fclose(outf);
  lame_close_infile(&gf);            /* close the input file */
  lame_mp3_tags(&gf);                /* add id3 or VBR tags to mp3 file */
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
  char* fargv[] = { "kernel_consumer_lame", "/home/hanning/ctuning-datasets-min/dataset/audio-wav-0001/data.wav", "/home/hanning/comet/tmp/tmp-output.tmp" , NULL };
  int fargc = 3;
  const char* out_path = "/home/hanning/comet/tmp/tmp-output.tmp";

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
  kernel_consumer_lame(fargc, fargv);
  fflush(stdout);
  polybench_stop_instruments;

  dup2(_saved_stdout, 1);
  close(_saved_stdout);
  polybench_print_instruments;

  double chk = _checksum_file(out_path);
  polybench_prevent_dce(_print_checksum(chk));
  return 0;
}
