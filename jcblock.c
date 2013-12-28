/*
 *	Program name: jcblock
 *
 *	File name: jcblock.c
 *
 *	Copyright: 	Copyright 2008 Walter S. Heath
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
 *	Description:
 *	A program to block telemarketing (junk) calls.
 *	This program connects to a serial port modem and listens for
 *	the caller ID string that is sent between the first and second
 *	rings. It records the string in file callerID.dat. It then
 *	reads strings from file whitelist.dat and scans them against
 *	the caller ID string for a match. If it finds a match it accepts
 *	the call. If a match is not found, it reads strings from file
 *	blacklist.dat and scans them against the caller ID string for a
 *	match. If it finds a match to a string in the blacklist, it sends
 *	an off-hook command (ATH1) to the modem, followed by an on-hook
 *	command (ATH0). This terminates the junk call.
 *
 *	For more details, see the README and UPDATES files.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>

#include <signal.h>

#include "common.h"

#define DEBUG

// Comment out the following define if you don't have ALSA audio
// support. Then compile with:
//     gcc -o jcblock jcblock.c truncate.c -lm
// The program will then have all capabilities except the star (*) key
// feature.
#define DO_TONES

// Comment out the following define if you don't have an answering
// machine attached to the same telephone line.
#define ANS_MACHINE

// Comment out the following define if you don't want truncation of
// records older than nine months from files blacklist.dat and
// callerID.dat. Then remove truncate.c from the gcc compile command.
#define DO_TRUNCATE

#define CALLERID_YES 1
#define CALLERID_NO 0

#define OPEN_PORT_BLOCKED 1
#define OPEN_PORT_POLLED  0

// Default serial port specifier.
char *serialPort = "/dev/ttyS0";
int fd;                                  // the serial port

FILE *fpWh;                              // whitelist.dat file
static struct termios options;
static time_t pollTime, pollStartTime;
static bool modemInitialized = FALSE;
static bool inBlockedReadCall = FALSE;
static int numRings;

static void cleanup( int signo );

int send_modem_command(int fd, char *command );
static bool check_blacklist( char *callstr );
static bool write_blacklist( char *callstr );
static bool check_whitelist( char * callstr );
static void open_port( int mode );
static void close_open_port( int doCallerID );
int init_modem(int fd, int doCallerID );

static char *copyright = "\n"
	"jcblock Copyright (C) 2008 Walter S. Heath\n"
	"This program comes with absolutely no warranty.\n"
	"This is free software, distributed under terms\n"
	"of the GNU Public License described at:\n"
	"<http://www.gnu.org/licenses/>.\n\n";

// Main function
int main(int argc, char **argv)
{
  int optChar;

  // Set Ctrl-C and kill terminator signal catchers
  signal( SIGINT, cleanup );
  signal( SIGKILL, cleanup );

  // See if a serial port argument was specified
  if( argc > 1 )
  {
    while( ( optChar = getopt( argc, argv, "p:h" ) ) != EOF )
    {
      switch( optChar )
      {
        case 'p':
          serialPort = optarg;
          break;

        case 'h':
        default:
          fprintf( stderr, "Usage: jcblock [-p /dev/<portID>]\n" );
          fprintf( stderr, "Default serial port is: /dev/ttyS0.\n" );
          fprintf( stderr, "For another port, use the -p option.\n" );
          _exit(-1);
      }
    }
  }

  // Display copyright notice
  printf( "%s", copyright );

#ifdef DO_TONES
  // Initialize the the star (*) key tones operation
  tonesInit();
#endif
  // Open or create a file to append caller ID strings to
  if( (fpCa = fopen( "./callerID.dat", "a+" ) ) == NULL )
  {
    printf("fopen() of callerID.dat failed\n");
    return;
  }

  // Open the whitelist file (for reading & writing)
  if( (fpWh = fopen( "./whitelist.dat", "r+" ) ) == NULL )
  {
    printf("fopen() of whitelist.dat failed. A whitelist is not required.\n" );
  }

  // Open the blacklist file (for reading & writing)
  if( (fpBl = fopen( "./blacklist.dat", "r+" ) ) == NULL )
  {
    printf("fopen() of blacklist.dat failed. A blacklist must exist.\n" );
    return;
  }
  // Open the serial port
  open_port( OPEN_PORT_BLOCKED );

  // Initialize the modem
  if( init_modem(fd, CALLERID_YES ) != 0 )
  {
    printf("init_modem() failed\n");
    close(fd);
    fclose(fpCa);
    fclose(fpBl);
    fclose(fpWh);
#ifdef DO_TONES
    tonesClose();
#endif
    fflush(stdout);
    sync();
    return;
  }

modemInitialized = TRUE;

  printf("Waiting for a call...\n");

  // Wait for calls to come in...
  wait_for_response(fd);

  close( fd );
  fclose(fpCa);
  fclose(fpBl);
  fclose(fpWh);
#ifdef DO_TONES
  tonesClose();
#endif
  fflush(stdout);
  sync();
}

//
// Initialize the modem.
// The 'doCallerID' argument allows initialization with
// or without sending the caller ID command (AT+VCID=1).
//
int init_modem(int fd, int doCallerID )
{
  // Reset the modem
#ifdef DEBUG
  printf("sending ATZ command...\n");
#endif
  if( send_modem_command(fd, "ATZ\r") != 0 )
  {
    return(-1);
  }

  sleep(1);   // needed

  // If operating in a non-US telephone system region,
  // insert an appropriate "AT+GCI=XX\r" modem command here.
  // See the README file for details.

  if( doCallerID )
  {
    // Tell modem to return caller ID
#ifdef DEBUG
printf("sending AT+VCID=1 command...\n");
#endif
    if( send_modem_command(fd, "AT+VCID=1\r") != 0 )
    {
      return(-1);
    }
  }

  return(0);
}

//
// Send command string to the modem
//
int send_modem_command(int fd, char *command )
{
  char buffer[255];     // Input buffer
  char *bufptr;         // Current char in buffer
  int nbytes;           // Number of bytes read
  int tries;            // Number of tries so far
  int i;

  // Send an AT command followed by a CR
  if( write(fd, command, strlen(command) ) != strlen(command) )
  {
    printf("send_modem_command: write() failed\n" );
  }

  for( tries = 0; tries < 20; tries++ )
  {
    // Read characters into our string buffer until we get a CR or NL
    bufptr = buffer;
    inBlockedReadCall = TRUE;
    while( (nbytes = read(fd, bufptr, buffer + sizeof(buffer) - bufptr - 1)) > 0 )
    {
      bufptr += nbytes;
      if( bufptr[-1] == '\n' || bufptr[-1] == '\r' )
        break;
    }
    inBlockedReadCall = FALSE;

    // Null terminate the string and see if we got an OK response
    *bufptr = '\0';

    // Scan for string "OK"
    if( strstr( buffer, "OK" ) != NULL )
    {
#ifdef DEBUG
      printf("got command OK\n");
#endif
      return( 0 );
    }
  }
#ifdef DEBUG
    printf("did not get command OK\n");
#endif
  return( -1 );
}

//
// Wait (forever!) for calls...
//
int wait_for_response(fd)
{
  char buffer[255];     // Input buffers
  char buffer2[255];
  int nbytes2;          // bytes in buffer2
  char buffer3[255];
  char bufRing[10];     // RING input buffer
  int nbytes;           // Number of bytes read
  int i, j;
  struct tm *tmPtr;
  time_t currentTime;
  int currentYear;
  char curYear[4];

  // Get a string of characters from the modem
  while(1)
  {
#ifdef DEBUG
    // Flush anything in stdout (needed if stdout is redirected to
    // a disk file).
    fflush(stdout);     // flush C library buffers to kernel buffers
    sync();             // flush kernel buffers to disk
#endif

    // Block until at least one character is available.
    // After first character is received, continue reading
    // characters until inter-character timeout (VTIME)
    // occurs (or VMIN characters are received, which
    // shouldn't happen, since VMIN is set larger than
    // the longest string expected).

    inBlockedReadCall = TRUE;
    nbytes = read( fd, buffer, 250 );
    inBlockedReadCall = FALSE;

    // Replace '\n' and '\r' characters with '-' characters
    for( i = 0; i < nbytes; i++ )
    {
       if( ( buffer[i] == '\n' ) || ( buffer[i] == '\r' ) )
       {
         buffer[i] = '-';
       }
    }

    // Put a '\n' at its end and null-terminate it
    buffer[nbytes] = '\n';
    buffer[nbytes + 1] = 0;

#ifdef DEBUG
    printf("nbytes: %d, str: %s", nbytes, buffer );
#endif

    // A string was received. If its a 'RING' string, just ignore it.
    if( strstr( buffer, "RING" ) != NULL )
    {
      continue;
    }

    // Ignore a string "AT+VCID=1" returned from the modem.
    if( strncmp( buffer, "AT+VCID=1", 9 ) == 0 )
    {
      continue;
    }

    // Caller ID data was received after the first ring.
    numRings = 1;

    // A caller ID string was constructed.

    // If space(' ') characters are not present before and after all
    // equal('=') characters, insert them (some modems don't insert
    // them!).
    for( i = 0, j = 0; i < nbytes + 1; i++ )
    {
      if( buffer[i] == '=' )
      {
        if( buffer[i - 1] != ' ' )    // If space before is missing...
        {
          buffer2[j++] = ' ';
          buffer2[j++] = buffer[i];
          if( buffer[i + 1] != ' ' )  // If space after is missing...
          {
            buffer2[j++] = ' ';
          }
        }
        else                          // If space before is there...
        {
          buffer2[j++] = buffer[i];
        }
      }
      else                            // If this char is not a '='...
      {
        buffer2[j++] = buffer[i];
      }
    }
    nbytes2 = j;                      // number of bytes in buffer2

    // 
    // The DATE field does not contain the year. Compute the year
    // and insert it.
    if( time( &currentTime ) == -1 )
    {
      printf("time() failed\n" );
      return -1;
    }

    tmPtr = localtime( &currentTime );
    currentYear = tmPtr->tm_year -100;  // years since 2000

    if( sprintf( curYear, "%02d", currentYear ) != 2 )
    {
      printf( "sprintf() failed\n" );
      return -1;
    }

    // Zero a new buffer with room for the year.
    for( i = 0; i < 100; i++ )
    {
      buffer3[i] = 0;
    }

    // Fill it but leave room for the year
    for( i = 0; i < 13; i++ )
    {
      buffer3[i] = buffer2[i];
    }
    for( i = 13; i < nbytes2; i++ )
    {
      buffer3[i + 2] = buffer2[i];
    }

    // Insert the year characters.
    buffer3[13] = curYear[0];
    buffer3[14] = curYear[1];

    // Close and re-open file 'callerID.dat' (in case it was
    // edited while the program was running!).
    fclose(fpCa);
    if( (fpCa = fopen( "./callerID.dat", "a+" ) ) == NULL )
    {
      printf("re-fopen() of callerID.dat failed\n");
      return(-1);
    }

    // Write the record to the file
    if( fputs( (const char *)buffer3, fpCa ) == EOF )
    {
      printf("fputs( (const char *)buffer3, fpCa ) failed\n");
      return(-1);
    }

    // Flush the record to the file
    if( fflush(fpCa) == EOF )
    {
      printf("fflush(fpCa) failed\n");
      return(-1);
    }

    // If a whitelist.dat file was present, compare the
    // caller ID string to entries in the whitelist. If a match
    // is found, accept the call and bypass the blacklist check.
    if( fpWh != NULL )
    {
      if( check_whitelist( buffer3 ) == TRUE )
      {
        // Caller ID match was found (or an error occurred),
        // so accept the call
        continue;
      }
    }

    // Compare the caller ID string to entries in the blacklist. If
    // a match is found, answer (i.e., terminate) the call.
    if( check_blacklist( buffer3 ) == TRUE )
    {
      // Blacklist entry was found.
      //
#ifdef DO_TRUNCATE
      // The following function truncates (removes old) entries
      // in data files -- if thirty days have elapsed since the
      // last time it truncated. Entries in callerID.dat are removed
      // if they are older than nine months. Entries in blacklist.dat
      // are removed if they have not been used to terminate a call
      // within the last nine months.
      // Note: it is not necessary for this function to run for the
      // main program to operate normally. You may remove it if you
      // don't want automatic file truncation. All of its code is in
      // truncate.c.
      truncate_records();
#endif                            // end DO_TRUNCATE
      continue;
    }
#ifdef DO_TONES
    // At this point the phone will ring until the call has been
    // answered or the caller hangs up (RING strings stop arriving).
    // Listen for a star (*) key press by polling the microphone. If
    // a press is detected (within the timed window), build and add
    // an entry to the blacklist for this call.
    else
    {
      // Get current time (seconds since Unix Epoch)
      if( (pollStartTime = time( NULL ) ) == -1 )
      {
        printf("time() failed(1)\n");
        continue;
      }

      // Reinitialize the serial port for polling
      close(fd);
      open_port( OPEN_PORT_POLLED );

      // Now poll until 'RING' strings stop arriving.
      // Note: seven seconds is just longer than the
      // inter-ring time (six seconds).
      while( (pollTime = time( NULL )) < pollStartTime + 7 )
      {
        if( ( nbytes = read( fd, bufRing, 1 ) ) > 0 )
        {
          if(bufRing[0] == 'R')
          {
            pollStartTime  = time( NULL );
            numRings++;                   // count the ring
          }
        }
        usleep( 100000 );        // 100 msec
      }

      // Reinitialize the serial port for blocked operation
      close(fd);
      open_port( OPEN_PORT_BLOCKED );

#ifdef ANS_MACHINE
      // If the call is answered after three rings, poll for a
      // touchtone star (*) key press. Note that if an answering
      // machine is connected to the line, the star feature is only
      // available if the call is answered after the third ring.
      // This is necessary to avoid conflict with answering machines.
      // The answering machine *must be* set to answer on the fourth
      // or later ring. See the README and UPDATES files for further
      // details.
      if( numRings == 3 )
      {
#else
      // If no answering machine is connected to the same telephone
      // line, the star key feature is available for calls answered after
      // three or more rings.
      if( TRUE )
      {
#endif
        //
        // Send an off-hook modem command so the mic can pick up the tones
        // generated by the star (*) key press. When the command is sent
        // the listener hears a "click". That indicates the start of the
        // timed window when a star (*) key press will be accepted.
        send_modem_command(fd, "ATH1\r"); // off hook

        // Send on-hook and off-hook commands to produce two more clicks
        // to aid the listener in detecting the start of the window.
        // Note that, due to the hardware, the third click is delayed.
        // If you like, you can omit these two commands and just sound
        // one click to signal the start of the detection window.
        send_modem_command(fd, "ATH0\r"); // on hook
        send_modem_command(fd, "ATH1\r"); // off hook

        // Get current time (seconds since Unix Epoch)
        if( (pollStartTime = time( NULL ) ) == -1 )
        {
          printf("time() failed(2)\n");
          continue;
        }

        // Poll for star (*) key press within the timeout window
        // (ten seconds)
        while( (pollTime = time( NULL )) < pollStartTime + 10 )
        {
          if( tonesPoll() == TRUE )
          {
            // Write a caller ID entry to blacklist.dat.
            write_blacklist( buffer3 );
            break;
          }
        }

        // Re-initialize the modem to return caller ID.
        // This also produces two clicks to signal the
        // end of the tone detection window.
        send_modem_command(fd, "ATZ\r");
        send_modem_command(fd, "AT+VCID=1\r");
        continue;
      }
    }
#endif                          // end DO_TONES

  }         // end of while()
}

//
// Compare strings in the 'whitelist.dat' file to fields in the
// received caller ID string. If a whitelist string is present
// (or an error occurred), return TRUE; otherwise return FALSE.
//
static bool check_whitelist( char *callstr )
{
  char whitebuf[100];
  char whitebufsave[100];
  char *whitebufptr;
  char call_date[10];
  char *dateptr;
  char *strptr;
  int i;
  long file_pos_last, file_pos_next;

  // Close and re-open the whitelist.dat file. Note: this
  // seems to be necessary to be able to write records
  // back into the file. The write works the first time
  // after the file is opened but not subsequently! :-(
  // This also allows whitelist changes made while the
  // program is running to be recognized.
  //
  fclose( fpWh );
  // Re-open for reading and writing
  if( (fpWh = fopen( "./whitelist.dat", "r+" ) ) == NULL )
  {
    printf("Re-open of whitelist.dat file failed\n" );
    return(TRUE);           // accept the call
  }

  // Disable buffering for whitelist.dat writes
  setbuf( fpWh, NULL );

  // Seek to beginning of list
  fseek( fpWh, 0, SEEK_SET );

  // Save the file's current access location
  if( file_pos_next = ftell( fpWh ) == -1L )
  {
    printf("ftell(fpWh) failed\n");
    return(TRUE);           // accept the call
  }

  // Read and process records from the file
  while( fgets( whitebuf, sizeof( whitebuf ), fpWh ) != NULL )
  {
    // Save the start location of the string just read and get
    // the location of the start of the next string in the file.
    file_pos_last = file_pos_next;
    file_pos_next = ftell( fpWh );

    // Ignore lines that start with a '#' character (comment lines)
    if( whitebuf[0] == '#' )
      continue;

    // Ignore lines containing just a '\n'
    if( whitebuf[0] == '\n' )
    {
      continue;
    }

    // Ignore records that are too short (don't have room for the date)
    if( strlen( whitebuf ) < 26 )
    {
      printf("ERROR: whitelist.dat record is too short to hold date field.\n");
      printf("       record: %s", whitebuf);
      printf("       record is ignored (edit file and fix it).\n");
      continue;
    }

    // Save the string (for writing back to the file later)
    strcpy( whitebufsave, whitebuf );

    // Make sure a '?' char is present in the string
    if( ( strptr = strstr( whitebuf, "?" ) ) == NULL )
    {
      printf("ERROR: all whitelist.dat entry first fields *must be*\n");
      printf("       terminated with a \'?\' character!! Entry is:\n");
      printf("       %s", whitebuf);
      printf("       Entry was ignored!\n");
      continue;
    }

    // Make sure the '?' character is within the first twenty characters
    if( (int)( strptr - whitebuf ) > 18 )
    {
      printf("ERROR: terminator '?' is not within first 20 characters\n" );
      printf("       %s", whitebuf);
      printf("       Entry was ignored!\n");
      continue;
    }

    // Get a pointer to the search token in the string
    if( ( whitebufptr = strtok( whitebuf, "?" ) ) == NULL )
    {
      printf("whitebuf strtok() failed\n");
      return(TRUE);         // accept the call
    }

    // Scan the call string for the whitelist entry
    if( strstr( callstr, whitebufptr ) != NULL )
    {
#ifdef DEBUG
      printf("whitelist entry matches: %s\n", whitebuf );
#endif
      // Make sure the 'DATE = ' field is present
      if( (dateptr = strstr( callstr, "DATE = " ) ) == NULL )
      {
        printf( "DATE field not found in caller ID!\n" );
        return(TRUE);     // accept the call
      }

      // Get the current date from the caller ID string
      strncpy( call_date, &dateptr[7], 6 );

      // Terminate the string
      call_date[6] = 0;

      // Update the date in the whitebufsave record
      strncpy( &whitebufsave[19], call_date, 6 );

      // Write the record back to the whitelist.dat file
      fseek( fpWh, file_pos_last, SEEK_SET );
      if( fputs( whitebufsave, fpWh ) == EOF )
      {
        printf("fputs(whitebufsave, fpWh) failed\n" );
        return(TRUE);         // accept the call
      }

      // Flush the string to the file
      if( fflush(fpWh) == EOF )
      {
        printf("fflush(fpWh) failed\n");
        return(TRUE);         // accept the call
      }

      // Force kernel file buffers to the disk
      // (probably not necessary)
      sync();

      // A whitelist.dat entry matched, so return TRUE
      return(TRUE);             // accept the call
    }
  }                               // end of while()

  // No whitelist.dat entry matched, so return FALSE.
  return(FALSE);
}

//
// Compare strings in the 'blacklist.dat' file to fields in the
// received caller ID string. If a blacklist string is present,
// send off-hook (ATH1) and on-hook (ATH0) to the modem to
// terminate the call...
//
static bool check_blacklist( char *callstr )
{
  char blackbuf[100];
  char blackbufsave[100];
  char *blackbufptr;
  char call_date[10];
  char *dateptr;
  char *strptr;
  int i;
  long file_pos_last, file_pos_next;
  char yearStr[10];

  // Close and re-open the blacklist.dat file. Note: this
  // seems to be necessary to be able to write records
  // back into the file. The write works the first time
  // after the file is opened but not subsequently! :-(
  // This also allows blacklist changes made while the
  // program is running to be recognized.
  //
  fclose( fpBl );
  // Re-open for reading and writing
  if( (fpBl = fopen( "./blacklist.dat", "r+" ) ) == NULL )
  {
    printf("re-open fopen( blacklist) failed\n" );
    return(FALSE);
  }

  // Disable buffering for blacklist.dat writes
  setbuf( fpBl, NULL );

  // Seek to beginning of list
  fseek( fpBl, 0, SEEK_SET );

  // Save the file's current access location
  if( file_pos_next = ftell( fpBl ) == -1L )
  {
    printf("ftell(fpBl) failed\n");
    return(FALSE);
  }

  // Read and process records from the file
  while( fgets( blackbuf, sizeof( blackbuf ), fpBl ) != NULL )
  {
    // Save the start location of the string just read and get
    // the location of the start of the next string in the file.
    file_pos_last = file_pos_next;
    file_pos_next = ftell( fpBl );

    // Ignore lines that start with a '#' character (comment lines)
    if( blackbuf[0] == '#' )
      continue;

    // Ignore lines containing just a '\n'
    if( blackbuf[0] == '\n' )
    {
      continue;
    }

    // Ignore records that are too short (don't have room for the date)
    if( strlen( blackbuf ) < 26 )
    {
       printf("ERROR: blacklist.dat record is too short to hold date field.\n");
       printf("       record: %s", blackbuf );
       printf("       record is ignored (edit file and fix it).\n");
       continue;
    }

    // Save the string (for writing back to the file later)
    strcpy( blackbufsave, blackbuf );

    // Make sure a '?' char is present in the string
    if( ( strptr = strstr( blackbuf, "?" ) ) == NULL )
    {
      printf("ERROR: all blacklist.dat entry first fields *must be*\n");
      printf("       terminated with a \'?\' character!! Entry is:\n");
      printf("       %s", blackbuf);
      printf("       Entry was ignored!\n");
      continue;
    }

    // Make sure the '?' character is within the first twenty characters
    // (could not be if the previous record was only partially written).
    if( (int)( strptr - blackbuf ) > 18 )
    {
      printf("ERROR: terminator '?' is not within first 20 characters\n" );
      printf("       %s", blackbuf);
      printf("       Entry was ignored!\n");
      continue;
    }

    // Get a pointer to the search token in the string
    if( ( blackbufptr = strtok( blackbuf, "?" ) ) == NULL )
    {
      printf("blackbuf strtok() failed\n");
      return(FALSE);
    }

    // Scan the call string for the blacklist entry
    if( strstr( callstr, blackbufptr ) != NULL )
    {
#ifdef DEBUG
      printf("blacklist entry matches: %s\n", blackbuf );
#endif
      // At this point, the modem is in data mode. It must
      // be returned to command mode to send it the off-hook
      // and on-hook commands. For the modem used, command
      // 'AT+++' did not work. The only way I could find to
      // put it back in command mode was to close, open and
      // reinitialize the connection. This clears the DTR line
      // which resets the modem to command mode. To accomplish
      // this in time (before the next ring), the caller ID
      // command is not sent. Later, the modem is again
      // reinitialized with caller ID activated. This is all
      // kind of crude, but it works...
      close_open_port( CALLERID_NO );

      usleep( 250000 );   // quarter second

      // Send off hook command
#ifdef DEBUG
      printf("sending off hook\n");
#endif
      send_modem_command(fd, "ATH1\r");

      sleep(1);

      // Send on hook command
#ifdef DEBUG
      printf("sending on hook\n");
#endif
      send_modem_command(fd, "ATH0\r");

      sleep(1);

      // Now, to prepare for the next call, close and reopen
      // the port with caller ID activated.
      close_open_port( CALLERID_YES );

      // Make sure the 'DATE = ' field is present
      if( (dateptr = strstr( callstr, "DATE = " ) ) == NULL )
      {
        printf( "DATE field not found in caller ID!\n" );
        return(FALSE);
      }

      // Get the current date from the caller ID string
      strncpy( call_date, &dateptr[7], 6 );

      // Terminate the string
      call_date[6] = 0;

      // Update the date in the blackbufsave record
      strncpy( &blackbufsave[19], call_date, 6 );

      // Write the record back to the blacklist.dat file
      fseek( fpBl, file_pos_last, SEEK_SET );
      if( fputs( blackbufsave, fpBl ) == EOF )
      {
        printf("fputs(blackbufsave, fpBl) failed\n" );
        return(FALSE);
      }

      // Flush the string to the file
      if( fflush(fpBl) == EOF )
      {
        printf("fflush(fpBl) failed\n");
        return(FALSE);
      }

      // Force kernel file buffers to the disk
      // (probably not necessary)
      sync();

      // A blacklist.dat entry matched, so return TRUE
      return(TRUE);
    }
  }                                         // end of while()

  /* A blacklist.dat entry was not matched, so return FALSE */
  return(FALSE);
}

//
// Add a record to the blacklist.dat file.
// Extract the NAME or NMBR field from the callerID record and use it to
// construct a blacklist.dat entry. Then append it to the blacklist.dat file.
// Return TRUE if an entry was made; FALSE on an error.
// Note:
// If you enter blacklist.dat records manually with some editors (e.g., vi
// or gedit), the editor adds a '\n' character at the end of the file when
// the file is closed if you didn't! Some editors don't do this (e.g., emacs).
// The '\n' character can be in the last or second to last location since the
// stored length is always even.
// This function starts the record it constructs with a '\n'. It checks to see
// if your editor added a '\n' at the end of the file. If one is present, it
// writes the first character of the new record over it. If not, it appends
// the new record to the end of the file.
//
bool write_blacklist( char *callstr )
{
  char blackbuf[100];
  char blacklistEntry[80];
  char readbuf[10];
  char *srcDesc = "*-KEY ENTRY";
  char *nameStr, *nmbrStr, *nmbrStrEnd;
  int nameStrLength, nmbrStrLength;
  int i;
  char yearStr[10];

  // Close and re-open the blacklist.dat file. Note: this
  // seems to be necessary to be able to write records
  // back into the file. The write works the first time
  // after the file is opened but not subsequently! :-(
  // This also allows blacklist changes made while the
  // program is running to be recognized.
  //
  fclose( fpBl );

  // Re-open for reading and writing
  if( (fpBl = fopen( "./blacklist.dat", "r+" ) ) == NULL )
  {
    printf("write_blacklist: re-open fopen() failed\n" );
    return(FALSE);
  }

  // Disable buffering for blacklist.dat writes
  setbuf( fpBl, NULL );

  // Build a blacklist entry from the caller ID string.
  // First fill the build array with ' ' chars.
  for(i = 0; i < 80; i++)
  {
    blacklistEntry[i] = ' ';
  }

  // If the NAME field does not contain "Cell Phone", use it as the
  // match string. If it does, use the call's number instead.
  // Note: Cell phone calls generally contain a "generic" NAME
  // field: "Cell Phone   XX", where XX is the state ID (e.g., MI for
  // Michigan). If that field was used in the blacklist record, all
  // cell phone calls from that state would be blocked! So we use the
  // call's number instead in those cases.
  //
  // For some caller ID strings, the NMBR and NAME fields are not the
  // standard lengths (10 and 15, respectively). So we need to calculate
  // their positions and lengths.
  //
  // Find the start of the "NAME = " string.
  if( ( nameStr = strstr( callstr, "NAME = " ) ) == NULL )
  {
    printf( "write_blacklist: strstr(..., \"NAME = \" ) failed\n" );
    return FALSE;
  }

  // Find the start of the "NMBR = " string.
  if( ( nmbrStr = strstr( callstr, "NMBR = " ) ) == NULL )
  {
    printf( "write_blacklist: strstr(..., \"NMBR = \" ) failed\n" );
    return FALSE;
  }

  // While here, find a pointer to the character after the NMBR
  // string field (subtract  2 from nameStr for the "--" separater)
  nmbrStrEnd = nameStr - 2;

  // Find the start of the NAME string field.
  nameStr += strlen( "NAME = " );

  // Find the start of the NMBR string field.
  nmbrStr += strlen( "NMBR = " );

  // Find the length of the NAME string field (subtract 3 for the
  // "--\n" at  its end).
  nameStrLength = strlen( nameStr ) - 3;

  // Find the length of the NMBR string field.
  nmbrStrLength = (int)(nmbrStrEnd - nmbrStr);

  // Now build the new blacklist entry.
  // Put a '\n' at the start of the string.
  blacklistEntry[0] = '\n';

  // See if the NAME field starts with "Cell Phone".
  if( strstr( nameStr, "Cell Phone" ) != NULL )
  {
    // If it does, use the NMBR field instead.
    strncpy( &blacklistEntry[1], &callstr[37], nmbrStrLength );
    blacklistEntry[ nmbrStrLength + 1] = '?'; // Add the terminator
  }
  else
  {
    // Get the call NAME field from the caller ID.
    strncpy( &blacklistEntry[1], nameStr, nameStrLength );
    blacklistEntry[nameStrLength + 1] = '?'; // Add the terminator
  }

  // Get the date field from the caller ID.
  strncpy( &blacklistEntry[20], &callstr[9], 6 );  

  // Add the source descriptor string ("KEY-* ENTRY").
  strncpy( &blacklistEntry[34], srcDesc, strlen(srcDesc) + 1 );

  // Read the last two characters in the file. If either is a '\n',
  // seek to its position so the following write will overwrite it.
  // If a '\n' is not found, seek to the end of the file.
  fseek( fpBl, -2, SEEK_END );

  if( fread( readbuf, 1, 2, fpBl ) != 2 )
  {
    printf("write_blacklist: fread() failed\n");
    return FALSE;
  }

  if( readbuf[0] == '\n' )
  {
    fseek( fpBl, -2, SEEK_END );
  }
  else if( readbuf[1] == '\n' )
  {
    fseek( fpBl, -1, SEEK_END );
  }
  else
  {
    fseek( fpBl, 0, SEEK_END );
  }

  // Write the new record to the file.
  if( fwrite( blacklistEntry, 1, strlen(blacklistEntry), fpBl ) !=
                                              strlen( blacklistEntry ) )
  {
    printf("write_blacklist: fwrite() failed\n");
    return FALSE;
  }
  return TRUE;
}

//
// Open the serial port.
//
static void open_port(int mode )
{
  // Open modem device for reading and writing and not as the controlling
  // tty (so the program does not get terminated if line noise sends CTRL-C).
  //
  if( ( fd = open( serialPort, O_RDWR | O_NOCTTY ) ) < 0 )
  {
    perror( serialPort );
    _exit(-1);
  }
  fcntl(fd, F_SETFL, 0);

  // Get the current options
  tcgetattr(fd, &options);

  // Set eight bits, no parity, one stop bit
  options.c_cflag       &= ~PARENB;
  options.c_cflag       &= ~CSTOPB;
  options.c_cflag       &= ~CSIZE;
  options.c_cflag       |= CS8;

  // Set hardware flow control
  options.c_cflag       |= CRTSCTS;

  // Set raw input
  options.c_cflag       |= (CLOCAL | CREAD);

  options.c_lflag       &= ~(ICANON | ECHO |ECHOE | ISIG);
  options.c_oflag       &=~OPOST;

  if( mode == OPEN_PORT_BLOCKED )
  {
    // Block read until a character is available or inter-character
    // time exceeds 1 unit (in 0.1sec units)
    options.c_cc[VMIN]    = 80;
    options.c_cc[VTIME]   = 1;
  }
  else                   // (mode == OPEN_PORT_POLLED)
  {
    // A read returns immediately with up to the number of bytes
    // requested. It returns the number read; zero if none available
    options.c_cc[VMIN]    = 0;
    options.c_cc[VTIME]   = 0;
  }

  // Set the baud rate (caller ID is sent at 1200 baud)
  cfsetispeed( &options, B1200 );
  cfsetospeed( &options, B1200 );

  // Set options
  tcsetattr(fd, TCSANOW, &options);
}


//
// Function to close and open the serial port to disable the DTR
// line. Needed to switch the modem from data mode back into
// command mode.
//
static void close_open_port( int doCallerID )
{
  // Close the port
  close(fd);

  usleep( 250000 );   // quarter second

  open_port( OPEN_PORT_BLOCKED );

  usleep( 250000 );   // quarter second
  init_modem(fd, doCallerID );
}

//
// SIGINT (Ctrl-C) and SIGKILL signal handler
//
static void cleanup( int signo )
{
  printf("\nin cleanup()...wait for kill...\n");

  if( modemInitialized )
  {
    // Reset the modem
#ifdef DEBUG
  printf("sending ATZ command...\n");
#endif
    send_modem_command(fd, "ATZ\r");
  }

  // Close everything
  close(fd);
  fclose(fpCa);
  fclose(fpBl);
  fclose(fpWh);
#ifdef DO_TONES
  tonesClose();
#endif
  fflush(stdout);     // flush C library buffers to kernel buffers
  sync();             // flush kernel buffers to disk

  // If program is in a blocked read(...) call, use kill() to
  // terminate program (happens when modem is not connected!).
  if( inBlockedReadCall )
  {
    kill( 0, SIGKILL );
  }

  // Otherwise terminate normally
  _exit(0);
}

