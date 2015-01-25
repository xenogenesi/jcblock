/*
 *	Program name: jcblock
 *
 *	File name: tonesRPi.c
 *	A version of tones.c for the Raspberry Pi Model B+ processor with a
 *	Cirrus Logic Audio Card (a.k.a., Wolfson Audio Card+) microphone
 *	adapter.
 *
 *	Copyright:      Copyright 2015 Walter S. Heath
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
 *	  The objective is to choose N such that the tone frequency is
 *	  close to an integer multiple of the bin width.
 *
 *	    For 941 Hz, try N = 528:
 *	     BIN_WIDTH = 8000.0/528 = 15.15 Hz. Then: 941/15.15 = 62.11
 *	     which is close to integer 62, so we choose it.
 *
 *	    For 1209 Hz, try N = 410:
 *	     BIN_WIDTH = 8000.0/410 = 19.51 Hz. Then 1209/19.51 = 61.97
 *	     which is close to integer 62, so we choose it.
 *
 *	  Note that, subject to the above, the larger N is the better the
 *	  algorithm works -- but the longer the *-key tones must be
 *	  present. The chosen values are a compromise.
 *
 *	Choosing THRESHOLD:
 *	   Objective is to put THRESHOLD safely above the non-detection
 *	   amplitudes and below the detection amplitudes. From the
 *	   program's printf outputs, you can determine a safe threshold
 *	   that will work for both tones when the star (*) key is pressed.
 *	   The value depends on how close the microphone is to the modem
 *	   speaker and therefore will vary for different hardware systems.
 *	   You may have to adjust the value to get the program to work
 *	   with your system.
 */
#include <stdio.h>
#include <math.h>

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>

#include "common.h"

// For phones that send a time-limited "beep" when the *-key is
// pressed (e.g., wireless and some wired phones), this option allows
// the operator to press the *-key twice to indicate that a blacklist
// entry should be added for the call. Note that there is some risk
// that a "false positive" result may occur. That is, the algorithm
// may interpret "audio noise" as a beep and create an unintended
// blacklist entry for the call. The noise can come from: 1) caller
// audio, 2) audio in the room where the phone is located or 3) audio
// from the room where the modem is located. Requiring two beeps helps
// to mitigate the risk. A way to avoid the risk is to put important
// calls on the whitelist. This option is activated by default.
// Note that the original detection method (for phones with non-time-
// limited tone generation when the *-key is pressed) is still present
// whether DO_BEEPS is active or not. To deactivate "beep" processing,
// comment out '#define DO_BEEPS' below.
#define DO_BEEPS

// Each data input frame contains 16-bit "Left" and "Right" samples
// (same hardware for record (microphone) and playback (speaker)).
// For a microphone the channels contain the same data.
struct bufFrame {
        short lSample;
        short rSample;
};

// Input data buffer union
static union {
         char *buffer;
         struct bufFrame *fPtr;
}unIn;

/* Goetzel globals */

#define FLOATING	float

#define NUM_FRAMES		128  // Number frames in a sample
				     // period (in a readi())

#define SAMPLING_RATE           8000.0		//8kHz

/* Low tone (941 Hz) parameters */
#define TARGET_FREQ_LO		941.0		//941 Hz
#define N_LO                    528            //941 Hz block size

/* High (1209 Hz) tone parameters */
#define TARGET_FREQ_HI         1209.0           //1209 Hz
#define N_HI                    410             //1209 Hz block size

#define THRESHOLD               0.5

#define DET_MIN                  10


#define PI			3.14159265

#define DEBUG 1

int numDetLo = 0;
int numDetLoWas = 0;
int numDetHi = 0;
int numDetHiWas = 0;
int numBeeps = 0;

FLOATING coeff_lo, coeff_hi;
FLOATING Q1;
FLOATING Q2;
FLOATING sine_lo, sine_hi;
FLOATING cosine_lo, cosine_hi;

int N_max = N_LO;

/* Make the array large enough for largest block size */
FLOATING testData[N_LO];

/* ALSA globals */
snd_pcm_t *handle;
int rc;

/* Set the number of frames in a sample period */
snd_pcm_uframes_t frames = NUM_FRAMES;

/* Prototypes */
void InitGoertzel(int N, int target_freq, FLOATING *sine, 
                       FLOATING *cosine, FLOATING *coeff);
void ProcessSample(FLOATING coeff, FLOATING sample);
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
void ProcessSample(FLOATING coeff, FLOATING sample)
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
#ifdef DEBUG
    printf("\nN_LO: ");
    printf("rel mag=%12.5f  ", magnitude);
#endif
  }
  else
  {
#ifdef DEBUG
    printf("N_HI: ");
    printf("rel mag=%12.5f  ", magnitude);
#endif
  }

  if( magnitude > THRESHOLD )
  {
#ifdef DEBUG
    printf("detection=TRUE\n");
#endif
    return TRUE;
  }
  else
  {
#ifdef DEBUG
    printf("detection=FALSE\n");
#endif
    return FALSE;
  }
}

/*
 * ALSA functions:
 */
void InitALSA(void)
{
  int bytes_per_frame;
  int bufferSize;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  int dir;

  /*
   * Open the PCM device for recording (capture).
   * Note: the device ID is: "hw:sndrpiwsp".
   * It is also the "default" device if no other
   * audio device is present.
   */
    rc = snd_pcm_open(&handle, "hw:sndrpiwsp",
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

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S16);

  /* Two channels (a hardware requirement) */
  snd_pcm_hw_params_set_channels(handle, params, 2);

  /* 8000 samples/second rate (Telephone quality) */
  val = 8000;
  dir = 0;                  /* set rate exactly */
  snd_pcm_hw_params_set_rate_near(handle, params,
                                  &val, &dir);

  /* Set period size to the value of 'frames'. */
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

  /*
     Use a read buffer large enough to hold one period.
     Get the actual period size in frames.
   */
  snd_pcm_hw_params_get_period_size(params,
                                &frames, &dir);

  /* Compute buffer size */
  /* bytes/frame = 2 bytes/sample * 2 channels = 4 */
  bytes_per_frame = 4;
  bufferSize = frames * bytes_per_frame;

  // Allocate the buffer
  unIn.buffer = (char*)malloc(bufferSize);
}

void tonesInit()
{
  /* Initialize the audio interface to the microphone */
  InitALSA();

  /* Initialize Goertzel parameters for the high and low frequencies */
  InitGoertzel( N_LO, TARGET_FREQ_LO, &sine_lo, &cosine_lo, &coeff_lo );
  InitGoertzel( N_HI, TARGET_FREQ_HI, &sine_hi, &cosine_hi, &coeff_hi );
}

/*
 * Remove any samples left in the audio buffer from a
 * previous call. Also, zero numBeeps (not related to buffer
 * clearing, but done here for convenience).
 */
void tonesClearBuffer()
{
  // Clear the buffer
  if( snd_pcm_drop(handle) < 0 ) {
    fprintf(stderr, "snd_pcm_drop() call failed\n");
    exit(1);
  }

  // Re-prepare the channel for use
  if( snd_pcm_prepare(handle) < 0 ) {
    fprintf(stderr, "snd_pcm_prepare() call failed\n");
    exit(1);
  }

  numBeeps = 0;	// in case it was still set from the previous call
}

bool tonesPoll()
{
  int index;
  int numSamples;
  int i;

  /*
   * Read and condition 'frames' blocks of samples until N_max
   * samples have been read.
   */
  index = 0;
  for( numSamples = 0; numSamples < N_max; numSamples += frames )
  {
    /* Read 'frames' interleaved frames */
    rc = snd_pcm_readi(handle, unIn.buffer, frames);

    if (rc == -EPIPE)
    {
      /* EPIPE means overrun */
      fprintf(stderr, "overrun occurred (not serious)\n");
      snd_pcm_prepare(handle);
      numBeeps = 0;
      numDetLoWas = numDetHiWas = 0;
      numDetLo = numDetHi = 0;
      return FALSE;
    }
    else if (rc < 0)
    {
      fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
      numBeeps = 0;
      numDetLoWas = numDetHiWas = 0;
      numDetLo = numDetHi = 0;
      return FALSE;
    }
    else if (rc != (int)frames)
    {
      fprintf(stderr, "short read, read %d frames\n", rc);
      numBeeps = 0;
      numDetLoWas = numDetHiWas = 0;
      numDetLo = numDetHi = 0;
      return FALSE;
    }

    /*
     * Capture the samples from the "left" channel only.
     * Scale them.
     */
    for( i = 0; i < frames && index < N_max; i++ )
    {
      testData[index++] = unIn.fPtr[i].lSample/32768.0;
    }
  }

  /* Process the samples for each tone */
  if( ProcessToneSamples( N_LO, sine_lo, cosine_lo, coeff_lo ) == TRUE )
  {
    numDetLo++;
  }
  else
  {
    numDetLoWas = numDetLo;
    numDetLo = 0;
  }
  if( ProcessToneSamples( N_HI, sine_hi, cosine_hi, coeff_hi ) == TRUE )
  {
    numDetHi++;
  }
  else
  {
    numDetHiWas = numDetHi;
    numDetHi = 0;
  }

  /*
   * For phones that send the tones continuously as long as the
   * *-key is pressed...
   * Require at least DET_MIN consecutive detections of both tones
   * to declare a *-KEY press detection.
   */
  if( numDetLo >= DET_MIN && numDetHi >= DET_MIN )
  {
    printf("*-KEY press detected\n");
    numDetLo = numDetHi = 0;
    numDetLoWas = numDetHiWas = 0;
    return TRUE;
  }
#ifdef DO_BEEPS
  /*
   * For phones that send a time-limited "beep" when the *-key
   * is pressed...
   * Require two consecutive detections of both tones twice (two
   * *-key press detections) to declare an operator auto-blacklist
   * entry request. Note: the number of detections may have to be
   * adjusted depending on the speed of your processor and the
   * duration of your phone's beep tone.
   */
  else if( ( numDetLoWas == 2 ) && ( numDetHiWas == 2 ) )
  {
    if(numBeeps == 0)     // If first *-key press detection
    {
      numBeeps = 1;
      numDetLoWas = numDetHiWas = 0;
    }
    else                  // If second *-key press detection
    {
      printf("Two *-key presses detected\n");
      numBeeps = 0;
      numDetLoWas = numDetHiWas = 0;
      numDetLo = numDetHi = 0;
      return TRUE;
    }
  }
#endif                               // end of DO_BEEPS
  return FALSE;
}

void tonesClose()
{
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  free(unIn.buffer);
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

  while( blockNum < 50 )
  {
    tonesPoll();
    blockNum++;
  }

  tonesClose();

  return 0;
}
#endif

