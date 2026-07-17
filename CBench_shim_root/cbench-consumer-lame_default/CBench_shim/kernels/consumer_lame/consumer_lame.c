
#include <stdio.h>
#include <unistd.h>

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
  snprintf(out_path, sizeof(out_path), "/home/hanning/comet/tmp/consumer_lame_out_%d.tmp", (int)getpid());

  char* fargv[] = { "kernel_consumer_lame", "/home/hanning/ctuning-datasets-min/dataset/audio-wav-0001/data.wav", out_path , NULL };
  int fargc = 3;

  kernel_consumer_lame(fargc, fargv);
  _cat_file_to_stdout(out_path);

  return 0;
}
