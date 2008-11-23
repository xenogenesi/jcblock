/*
 *	Program name: jcblock
 *
 *      Copyright: 	Copyright 2008 Walter S. Heath
 *
 *      Copy permission:
 *      This program is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, either version 3 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *	Description:
 *	A program to block telemarketing (junk) calls.
 *	This program connects to a serial port modem and listens for
 *	the caller ID string that is sent between the first and second
 *	rings. It records the string in file callerID.dat. It then
 *	reads strings from file blacklist.dat and scans them against
 *	the caller ID string for a match. If it finds a match to a
 *	string in the blacklist, it sends an off-hook command (ATH1)
 *	to the modem, followed by an on-hook command (ATH0). This
 *	terminates the junk call. The program also updates the date
 *      field of the matching blacklist entry. The user may then identify
 *	entries that are old so that they can be removed.
 *
 *	The program requires a serial modem that can deliver caller
 *	ID. The modem used for testing was a Zoom model 3048. It will
 *	return caller ID if it is sent command: AT+VCID=1. Note that
 *	the modem is used to just get the caller ID; no attempt is
 *	made to make a connection to another modem at the caller's end.
 *	The program may be terminated by sending it a SIGINT (Ctrl-C)
 *	signal or a SIGKILL signal.
 *
 *	The program runs on a standard PC (it was written and tested
 *	on a Dell Dimension B110 running Ubuntu 7.10). It may be compiled
 *      with the following simple command: gcc -o jcblock jcblock.c. For
 *	continuous use, it should be run on a low-power single board
 *	computer so that it can be left on all the time.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>

#include <signal.h>

#define DEBUG

#define CALLERID_YES 1
#define CALLERID_NO 0

int fd;
FILE *fp;
FILE *fp2;
static struct termios options;

static void cleanup( int signo );

int send_modem_command(int fd, char *command );
static void check_blacklist( char *callstr );
static void open_port();
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
  // set Ctrl-C and kill terminator signal catchers
  signal( SIGINT, cleanup );
  signal( SIGKILL, cleanup );

  // Display copyright notice
  printf( copyright );

  // Open a file to write caller ID strings to
  if( (fp = fopen( "callerID.dat", "a+" ) ) == NULL )
  {
    printf("fopen() failed\n");
    return;
  }

  // Seek to end of existing strings
  if( fseek(fp, 0L, SEEK_END) < 0 )
  {
    printf("fseek() failed\n");
    return;
  }

  // Open the blacklist file (for reading & writing)
  if( (fp2 = fopen( "blacklist.dat", "r+" ) ) == NULL )
  {
    printf("fopen( blacklist.dat ) failed\n" );
    return;
  }

  // Disable buffering for blacklist.dat writes
  setbuf( fp2, NULL );

  // Open the serial port
  open_port();

  // Initialize the modem
  if( init_modem(fd, CALLERID_YES ) != 0 )
  {
    printf("init_modem() failed\n");
    close( fd );
    fclose(fp);
    fclose(fp2);
    return;
  }

  printf("Waiting for a call...\n");

  // Wait for calls to come in...
  wait_for_response(fd);

  close( fd );
  fclose(fp);
  fclose(fp2);
}

//
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

  if( doCallerID )
  {
    // tell modem to return caller ID
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

  // send an AT command followed by a CR
  if( write(fd, command, strlen(command) ) != strlen(command) )
  {
    printf("write() failed\n" );
  }

  for( tries = 0; tries < 20; tries++ )
  {
    // read characters into our string buffer until we get a CR or NL
    bufptr = buffer;
    while( (nbytes = read(fd, bufptr, buffer + sizeof(buffer) - bufptr - 1)) > 0 )
    {
      bufptr += nbytes;
      if( bufptr[-1] == '\n' || bufptr[-1] == '\r' )
        break;
    }

    // null terminate the string and see if we got an OK response
    *bufptr = '\0';

#if 0
for( i = 0; buffer[i] != '\0' ; i++ )
   if( buffer[i] == '\n' )
   {
     printf("buf[i] LF, i %d\n", i );
   }
   else if( buffer[i] == '\r' )
     printf("buf[i] CR, i %d\n", i );
   else
     printf("buf[i] %c, i %d\n", buffer[i], i );
#endif

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
  char buffer[255];     // Input buffer

  int nbytes;           // Number of bytes read
  int i;

  // Get a string of characters from the modem
  while(1)
  {
    // Block until at least one character is available.
    // After first character is received, continue reading
    // characters until inter-character timeout (VTIME)
    // occurs (or VMIN characters are received, which
    // shouldn't happen, since VMIN is set larger than
    // the longest string expected).

    nbytes = read( fd, buffer, 250 );

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

    // A string was received. If its a call
    // string, write string to callerID.dat file.
    // Ignore short strings (RING strings).
    if( nbytes > 40 )
    {
      if( fprintf( fp, buffer ) < 0 )
      {
        printf("fprintf() failed\n");
        return(-1);
      }

      // Flush the string to the file
      if( fflush(fp) == EOF )
      {
        printf("fflush(fp) failed\n");
        return(-1);
      }

      // Compare to blacklist; answer call if present
      check_blacklist( buffer );
    }
  }
}

//
// Compare strings in the 'blacklist.dat' file to fields in the
// received caller ID string. If a blacklist string is present,
// send off-hook (ATH1) and on-hook (ATH0) to the modem to
// terminate the call...
//
static void check_blacklist( char *callstr )
{
  char blackbuf[100];
  char blackbufsave[100];
  char *blackbufptr;
  char call_date[10];
  char *dateptr;
  int i;
  long file_pos_last, file_pos_next;

  // Close and re-open the blacklist.dat file. Note: this
  // seems to be necessary to be able to write records
  // back into the file. The write works the first time
  // after the file is opened but not subsequently! :-(
  //
  fclose( fp2 );

  // Re-open for reading and writing
  if( (fp2 = fopen( "blacklist.dat", "r+" ) ) == NULL )
  {
    printf("re-open fopen( blacklist) failed\n" );
    return;
  }

  // Disable buffering for blacklist.dat writes
  setbuf( fp2, NULL );

  // Seek to beginning of list
  fseek( fp2, 0, SEEK_SET );

  // Save the file's current access location
  if( file_pos_next = ftell( fp2 ) == -1L )
  {
    printf("ftell() failed\n");
    return;
  }

  while( fgets( blackbuf, sizeof( blackbuf ), fp2 ) != NULL )
  {
    // Save the start location of the string just read and get
    // the location of the start of the next string in the file.
    file_pos_last = file_pos_next;
    file_pos_next = ftell( fp2 );

    // Ignore lines that start with a '#' character (comment lines)
    if( blackbuf[0] == '#' )
      continue;

    // Ignore lines containing just a '\n'
    if( blackbuf[0] != '\n' )
    {
      // Save the string (for writing back to the file later)
      strcpy( blackbufsave, blackbuf );

      // Make sure a '?' char is present in the string
      if( strstr( blackbuf, "?" ) == NULL )
      {
        printf("ERROR: all blacklist.dat entry first fields *must be*\n");
        printf("       terminated with a \'?\' character!!\n");
        return;
      }

      // Get a pointer to the search token in the string
      if( ( blackbufptr = strtok( blackbuf, "?" ) ) == NULL )
      {
        printf("strtok() failed\n");
        return;
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

        usleep( 500000 );   // half second

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

        // Make sure the string is long enough to hold the date
        if( strlen( blackbufsave ) >= 24 )
        {
          // Make sure the 'DATE = ' field is present
          if( (dateptr = strstr( callstr, "DATE = " ) ) == NULL )
          {
            printf( "DATE field not found in caller ID!\n" );
            return;
          }

          // Get the current date from the caller ID string
          strncpy( call_date, &dateptr[7], 4 );

          // Terminate the string
          call_date[4] = 0;

          // Update the date in the blackbufsave record
          strncpy( &blackbufsave[19], call_date, 4 );

          // Write the record back to the blacklist.dat file
          fseek( fp2, file_pos_last, SEEK_SET );
          if( fputs( blackbufsave, fp2 ) == EOF )
          {
            printf("fputs() failed\n" );
            return;
          }

          // Flush the string to the file
          if( fflush(fp2) == EOF )
          {
            printf("fflush(fp2) failed\n");
            return;
          }

          // Force kernel file buffers to the disk
          // (probably not necessary)
          sync();
        }
        else
        {
          printf("Date not saved; blacklist.dat entry too short!\n" );
        }
        // A blacklist.dat entry matched, so return
        return;
      }
    }
  }
}

static void open_port()
{
  // Open modem device for reading and writing and not as controlling tty
  // because we don't want to get killed if linenoise sends CTRL-C.
  //
  if( ( fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY ) ) < 0 )
  {
    perror("/dev/ttyS0" );
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

  // Block read until a character is available or inter-character
  // time exceeds 1 unit (in 0.1sec units)
  options.c_cc[VMIN]    = 80;
  options.c_cc[VTIME]   = 1;

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

  usleep( 500000 );   // half second

  open_port();

  usleep( 500000 );   // half second
  init_modem(fd, doCallerID );
}

//
// SIGINT (Ctrl-C) and SIGKILL signal handler
//
static void cleanup( int signo )
{
  printf("\nin cleanup()...\n");

  // Reset the modem
  send_modem_command(fd, "ATZ\r");

  // Close everything
  close(fd);
  fclose(fp);
  fclose(fp2);
  _exit(0);
}

