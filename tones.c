/*
 *	Program name: jcblock
 *
 *	File name: tones.c
 *
 *	Copyright:      Copyright 2008 Walter S. Heath
 *
 *	Copy permission:
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You may view a copy of the GNU General Public License at:
 *	           <http://www.gnu.org/licenses/>.
 *
 *	Functions to input audio from a microphone and detect the presence
 *	of tones (941 Hz and 1209 Hz) produced by pressing the star (*) key
 *	on a touch tone telephone.
 *
 *	Audio recording based on:
 *	  "Introduction to Sound Programming with ALSA",
 * 	  by Jeff Tranter, Linux Journal, October 2004.
 *
 *	Tone detection based on:
 *	  "The Goretzel Algorithm", Kevin Banks,
 *	  Embedded Systems Programming, September 2002.
 *
 *	Choosing N, the block size (N = SAMPLING_RATE/BIN_WIDTH):
 *	  Objective is to choose N such that the tone frequency is in
 *	  the center of a bin.
 *	    For 941 Hz, try N = 259:
 *	     BIN_WIDTH = 8000.0/259 = 30.89 Hz; 941/30.89 = 30.46;
 *	     30*30.89 = 926.70; 31*30.89 = 957.59, so 941 is close to the
 *	     center.
 *	    For 1209 Hz, try N = 195:
 *	     BIN_WIDTH = 8000.0/195 = 41.03 Hz; 1209/41.03 = 29.47;
 *	     29*41.03 = 1189.87; 30*41.03 = 1230.90, so 1209 is close to
 *	     the center.
 *
 *	Choosing THRESHOLD:
 *	   Objective is to put THRESHOLD safely above the non-detection
 *	   amplitudes and below the detection amplitudes. From the
 *	   program's printf outputs, you can determine a safe threshold
 *	   that will work for both tones when the star (*) key is pressed.
 *	   The value depends on how close the microphone is to the speaker
 *	   and therefore will vary for different hardware systems.
 *	   You may have to adjust the value to get the program to work
 *	   with your computer. So far I haven't been able to figure out how
 *	   to control mic sample amplitude (volume) in ALSA (hint: have to
 *	   look at the amixer source code). This threshold value works in
 *	   most cases.
 */
#include <stdio.h>
#include <math.h>

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>

#include "common.h"

/* Goetzel globals */

#define FLOATING	float
#define SAMPLE		unsigned char

#define SAMPLING_RATE           8000.0		//8kHz

/* Low tone (941 Hz) parameters */
#define TARGET_FREQ_LO		941.0		//941 Hz
#define N_LO                    259             //941 Hz block size

/* High (1209 Hz) tone parameters */
#define TARGET_FREQ_HI         1209.0           //1209 Hz
#define N_HI                    195             //1209 Hz block size

#define THRESHOLD                10
#define BLK_CTR_MAX              5

#define PI			3.14159265

#define DEBUG 1


FLOATING magMax = 0;
int magNum = 0;
int blockCtr = 0;
FLOATING magnitudeSumLo = 0.0;
FLOATING magnitudeSumHi = 0.0;

FLOATING magSum = 0;

FLOATING coeff_lo, coeff_hi;
FLOATING Q1;
FLOATING Q2;
FLOATING sine_lo, sine_hi;
FLOATING cosine_lo, cosine_hi;

int N_max = N_LO;

/* Make the array large enough for largest block size */
SAMPLE testData[N_LO];

/* ALSA globals */
snd_pcm_t *handle;
char *buffer;
int rc;

/*
 * NOTE: Unfortunately, the value of the 'frames' parameter
 * is dependent upon the version of ALSA that is being used.
 * For older versions, a value of 32 works. For newer versions
 * the value must be 128 or more. For version 1.0.21 (included
 * with Ubuntu 10.04, Linux 2.6.32-25-generic) value 128 is
 * needed. For version 1.0.13 (Knoppix, Linux 2.6.19) value
 * 32 is needed.This value also works for the ALSA version
 * included with Ubuntu 8.04. The value is set to 128 here to
 * be compatible with newer versions of ALSA. Be aware that
 * you may need to set it to 32 for your version.
 */
snd_pcm_uframes_t frames = 128;

/* Prototypes */
void InitGoertzel(int N, int target_freq, FLOATING *sine, 
                       FLOATING *cosine, FLOATING *coeff);
void ProcessSample(FLOATING coeff, SAMPLE sample);
void GetRealImag(FLOATING *realPart, FLOATING *imagPart, 
                          FLOATING sine, FLOATING cosine);
bool ProcessToneSamples(int N, FLOATING sine, 
                        FLOATING cosine, FLOATING coeff );
void InitALSA();

/*
 * Goertzel algorithm functions:
 */

/* Call this routine before every "block" (size=N) of samples. */
void ResetGoertzel(void)
{
  Q2 = 0;
  Q1 = 0;
}

/* Call this once for each tone, to precompute the constants. */
void InitGoertzel(int N, int target_freq, FLOATING *sine,
                               FLOATING *cosine, FLOATING *coeff)
{
  int			k;
  FLOATING		floatN;
  FLOATING		omega;

  floatN = (FLOATING) N;
  k = (int) (0.5 + ((floatN * target_freq) / SAMPLING_RATE));
  omega = (2.0 * PI * k) / floatN;
  *sine = sin(omega);
  *cosine = cos(omega);
  *coeff = 2.0 * (*cosine);
}

/* Call this routine for every sample. */
void ProcessSample(FLOATING coeff, SAMPLE sample)
{
  FLOATING Q0;
  Q0 = coeff * Q1 - Q2 + (FLOATING) sample;
  Q2 = Q1;
  Q1 = Q0;
}

/* Basic Goertzel */
/* Call this routine after every block to get the complex result. */
void GetRealImag(FLOATING *realPart, FLOATING *imagPart,
                            FLOATING sine, FLOATING cosine)
{
  *realPart = (Q1 - Q2 * cosine);
  *imagPart = (Q2 * sine);
}

/* Process the tone samples */
bool ProcessToneSamples(int N, FLOATING sine,
                           FLOATING cosine, FLOATING coeff )
{
  FLOATING real, imag;
  FLOATING magnitude;
  FLOATING magnitudeSquared;
  FLOATING magAvg;
  int index;
  bool detection = FALSE;

  /* Process the samples */
  ResetGoertzel();
  for( index = 0; index < N; index++ )
  {
    ProcessSample(coeff, testData[index]);
  }

  /* Do the "standard Goertzel" processing */
  GetRealImag( &real, &imag, sine, cosine );

  magnitudeSquared = real*real + imag*imag;

  magnitude = sqrt( magnitudeSquared );

  if(N == N_LO)
  {
    magnitudeSumLo += magnitude;
  }
  else
  {
    magnitudeSumHi += magnitude;
  }

  /* "Debounce(average)" BLK_CTR_KAX results and compare to THRESHOLD */
  if(blockCtr == BLK_CTR_MAX )
  {
    if(N == N_LO)
    {
      magAvg = magnitudeSumLo/blockCtr;

#ifdef DEBUG
      printf("\nN_LO: ");
      printf("rel mag=%12.5f  ", magnitude);
      printf("mag avg5=%12.5f  ", magAvg );
#endif
      magnitudeSumLo = 0.0;
    }
    else
    {
      magAvg = magnitudeSumHi/blockCtr;

#ifdef DEBUG
      printf("N_HI: ");
      printf("rel mag=%12.5f  ", magnitude);
      printf("mag avg5=%12.5f  ", magAvg );
#endif
      magnitudeSumHi = 0.0;
    }

    if( magAvg > THRESHOLD )
    {
      detection = TRUE;
    }
    else
    {
      detection = FALSE;
    }

#ifdef DEBUG
    if(detection == TRUE )
    {
      printf("detection=TRUE");
    }
    else
    {
      printf("detection=FALSE");
    }
    printf("\n");
#endif
  }

  return detection;
}

/*
 * ALSA functions:
 */
void InitALSA(void)
{
  long loops;
  int size;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  int dir;

  /* Open PCM device for recording (capture). */
  rc = snd_pcm_open(&handle, "default",
                    SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr,
            "unable to open pcm device: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */

  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params,
                      SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 8-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S8);

  /* One channel (monoral) */
  snd_pcm_hw_params_set_channels(handle, params, 1);

  /* 8000 samples/second sampling rate (Telephone quality) */
  val = 8000;
  snd_pcm_hw_params_set_rate_near(handle, params,
                                  &val, &dir);

  /* Set period size to the value of 'frames'. See
   * NOTE: at the beginning of this file.
   */
  snd_pcm_hw_params_set_period_size_near(handle,
                              params, &frames, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr,
            "unable to set hw parameters: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Use a buffer large enough to hold one period */
  snd_pcm_hw_params_get_period_size(params,
                                      &frames, &dir);
  size = frames * 1; /* 1 byte/sample, 1 channel */
  buffer = (char *) malloc(size);
}


void tonesInit()
{
  /* Initialize the audio interface to the microphone */
  InitALSA();

  /* Initialize Goertzel parameters for the high and low frequencies */
  InitGoertzel( N_LO, TARGET_FREQ_LO, &sine_lo, &cosine_lo, &coeff_lo );
  InitGoertzel( N_HI, TARGET_FREQ_HI, &sine_hi, &cosine_hi, &coeff_hi );
}

bool tonesPoll()
{
  int index;
  int numSamples;
  int i;
  bool det_lo, det_hi;

  blockCtr++;

  /*
   * Read and condition 'frames' blocks of samples until N_max
   * samples have been read.
   */
  index = 0;
  for( numSamples = 0; numSamples < N_max; numSamples += frames )
  {
    /* Read a block of samples */
    rc = snd_pcm_readi(handle, buffer, frames);

    if (rc == -EPIPE)
    {
      /* EPIPE means overrun */
      fprintf(stderr, "overrun occurred\n");
      snd_pcm_prepare(handle);
      return FALSE;
    }
    else if (rc < 0)
    {
      fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
      return FALSE;
    }
    else if (rc != (int)frames)
    {
      fprintf(stderr, "short read, read %d frames\n", rc);
      return FALSE;
    }

    /* Condition the data for the Goertzel algorithm */
    for( i = 0; i < frames && index < N_max; i++ )
    {
      testData[index++] = (SAMPLE)( (buffer[i] * 100)/256 + 100 );
    }
  }

  /* Process the samples for each tone */
  det_lo = ProcessToneSamples( N_LO, sine_lo, cosine_lo, coeff_lo );
  det_hi = ProcessToneSamples( N_HI, sine_hi, cosine_hi, coeff_hi );


  if(blockCtr == BLK_CTR_MAX)
  {
    blockCtr = 0;
  }

  /* Require both tones to be detected to declare a final detection */
  if( det_lo && det_hi )
  {
    printf("KEY-* press detected\n");
    return TRUE;
  }

  return FALSE;
}

void tonesClose()
{
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  free(buffer);
}

#if 0
// This main() function may be activated to test tone detection separately.
// Compile it with:
//     gcc -o tones tones.c -lasound -ldl -lm
// It may then be tested by attaching a microphone to a telephone ear piece
// and pressing the star (*) key while the program is running. The output
// should indicate that both tones were detected (show TRUE).
int main()
{
  int blockNum = 0;

  tonesInit();

  while( blockNum < 200 )
  {
    tonesPoll();
    blockNum++;
  }

  tonesClose();

  return 0;
}
#endif

