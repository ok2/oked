/* io.c: i/o routines for the ed line editor */
/* GNU ed - The GNU line editor.
   Copyright (C) 1993, 1994 Andrew L. Moore, Talke Studio
   Copyright (C) 2006-2026 Antonio Diaz Diaz.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "ed.h"


static const line_node * unterminated_line = 0;	/* last line has no '\n' */
static unsigned linenum_ = 0;			/* script line number */

#ifdef HAVE_READLINE
static bool in_input_mode = false;
#endif

unsigned linenum( void ) { return linenum_; }

void reset_unterminated_line( void ) { unterminated_line = 0; }

void unmark_unterminated_line( const line_node * const lp )
  { if( unterminated_line == lp ) unterminated_line = 0; }

static bool unterminated_last_line( void )
  { return unterminated_line != 0 &&
           unterminated_line == search_line_node( last_addr() ); }


unsigned char escchar( const unsigned char ch )
  {
  const char * const escapes = "\a\b\f\n\r\t\v";
  const char * const escchars = "abfnrtv";
  const char * const p = strchr( escapes, ch );
  return ( ch && p ) ? escchars[p-escapes] : 0;
  }

/* print text to stdout */
static void print_line( const char * p, int len, const int pflags )
  {
  int col = 0;

  if( pflags & pf_n ) { printf( "%d\t", current_addr() ); col = 8; }
  while( --len >= 0 )
    {
    const unsigned char ch = *p++;
    if( !( pflags & pf_l ) ) putchar( ch );
    else
      {
      if( ++col > window_columns() ) { col = 1; fputs( "\\\n", stdout ); }
      if( ch >= 32 && ch <= 126 )
        { if( ch == '$' || ch == '\\' ) { ++col; putchar('\\'); }
          putchar( ch ); }
      else
        {
        ++col; putchar('\\');
        const char e = escchar( ch );
        if( e ) putchar( e );
        else
          {
          col += 2;
          putchar( ( ( ch >> 6 ) & 7 ) + '0' );
          putchar( ( ( ch >> 3 ) & 7 ) + '0' );
          putchar( ( ch & 7 ) + '0' );
          }
        }
      }
    }
  if( !traditional() && ( pflags & pf_l ) ) putchar('$');
  putchar('\n');
  }


/* print a range of lines to stdout */
bool print_lines( int from, const int to, const int pflags )
  {
  if( highlight_enabled() && !( pflags & pf_l ) && !scripted() && isatty( 1 ) )
    return print_lines_highlighted( from, to, pflags );

  line_node * const ep = search_line_node( inc_addr( to ) );
  line_node * bp = search_line_node( from );

  if( !from ) { invalid_address(); return false; }
  while( bp != ep )
    {
    const char * const s = get_sbuf_line( bp );
    if( !s ) return false;
    set_current_addr( from++ );
    print_line( s, bp->len, pflags );
    bp = bp->q_forw;
    }
  return true;
  }


/* return the parity of escapes at the end of a string */
static bool trailing_escape( const char * const s, int len )
  {
  bool odd_escape = false;
  while( --len >= 0 && s[len] == '\\' ) odd_escape = !odd_escape;
  return odd_escape;
  }


/* If *ibufpp contains an escaped newline, get an extended line (one
   with escaped newlines) from stdin.
   The backslashes escaping the newlines are stripped.
   Return line length in *lenp, including the trailing newline.
*/
bool get_extended_line( const char ** const ibufpp, int * const lenp,
                        const bool strip_escaped_newlines )
  {
  static char * buf = 0;
  static int bufsz = 0;
  int len;

  for( len = 0; (*ibufpp)[len++] != '\n'; ) ;
  if( len < 2 || !trailing_escape( *ibufpp, len - 1 ) )
    { if( lenp ) *lenp = len; return true; }
  if( !resize_buffer( &buf, &bufsz, len + 1 ) ) return false;
  memcpy( buf, *ibufpp, len );
  --len; buf[len-1] = '\n';			/* strip trailing esc */
  if( strip_escaped_newlines ) --len;		/* strip newline */
  while( true )
    {
    int len2;
    const char * const s = get_stdin_line( &len2 );
    if( !s ) return false;			/* error */
    if( len2 <= 0 ) return false;		/* EOF */
    if( !resize_buffer( &buf, &bufsz, len + len2 + 1 ) ) return false;
    memcpy( buf + len, s, len2 );
    len += len2;
    if( len2 < 2 || !trailing_escape( buf, len - 1 ) ) break;
    --len; buf[len-1] = '\n';			/* strip trailing esc */
    if( strip_escaped_newlines ) --len;		/* strip newline */
    }
  buf[len] = 0;
  *ibufpp = buf;
  if( lenp ) *lenp = len;
  return true;
  }


/* Read a line of text from stdin.
   Incomplete lines (lacking the trailing newline) are discarded.
   Return pointer to buffer and line size (including trailing newline),
   or 0 if error, or *sizep = 0 if EOF.
*/
const char * get_stdin_line( int * const sizep )
  {
  static char * buf = 0;
  static int bufsz = 0;
  int i = 0;

  while( true )
    {
    const int c = getchar();
    if( !resize_buffer( &buf, &bufsz, i + 2 ) ) { *sizep = 0; return 0; }
    if( c == EOF )
      {
      if( ferror( stdin ) )
        {
        show_strerror( "stdin", errno );
        set_error_msg( "Cannot read stdin" );
        clearerr( stdin );
        *sizep = 0; return 0;
        }
      if( feof( stdin ) )
        {
        set_error_msg( "Unexpected end-of-file" );
        clearerr( stdin );
        buf[0] = 0; *sizep = 0; if( i > 0 ) ++linenum_;	/* discard line */
        return buf;
        }
      }
    else
      {
      buf[i++] = c; if( c == 0 ) set_binary(); if( c != '\n' ) continue;
      ++linenum_; buf[i] = 0; *sizep = i;
      return buf;
      }
    }
  }


/* Read a line of text from the terminal using readline if available.
   Add non-empty command lines to history.
   Return pointer to buffer and line size (including trailing newline),
   or 0 if error, or *sizep = 0 if EOF.
*/
const char * get_tty_line( int * const sizep, const char * const prompt )
  {
#ifdef HAVE_READLINE
  if( isatty( 0 ) )
    {
    static char * buf = 0;
    static int bufsz = 0;
    char * line = readline( prompt );
    if( !line )				/* EOF */
      {
      if( !resize_buffer( &buf, &bufsz, 2 ) ) { *sizep = 0; return 0; }
      buf[0] = 0; *sizep = 0; return buf;
      }
    const int len = strlen( line );
    if( !resize_buffer( &buf, &bufsz, len + 2 ) )
      { free( line ); *sizep = 0; return 0; }
    memcpy( buf, line, len );
    buf[len] = '\n'; buf[len + 1] = 0;
    if( len > 0 ) add_history( line );
    free( line );
    ++linenum_;
    *sizep = len + 1;
    return buf;
    }
#endif
  /* fallback: print prompt and use getchar loop */
  if( prompt ) { fputs( prompt, stdout ); fflush( stdout ); }
  return get_stdin_line( sizep );
  }


static const char * history_path( void )
  {
  static char * path = 0;
  if( !path )
    {
    const char * const hd = home_directory();
    if( !hd || !hd[0] ) return 0;
    const int hdlen = strlen( hd );
    const char hist[] = "/.ed_history";
    path = (char *)malloc( hdlen + sizeof hist );
    if( !path ) return 0;
    memcpy( path, hd, hdlen );
    memcpy( path + hdlen, hist, sizeof hist );
    }
  return path;
  }


static const char * input_history_path( void )
  {
  static char * path = 0;
  if( !path )
    {
    const char * const hd = home_directory();
    if( !hd || !hd[0] ) return 0;
    const int hdlen = strlen( hd );
    const char hist[] = "/.ed_input_history";
    path = (char *)malloc( hdlen + sizeof hist );
    if( !path ) return 0;
    memcpy( path, hd, hdlen );
    memcpy( path + hdlen, hist, sizeof hist );
    }
  return path;
  }


#ifdef HAVE_READLINE

/* Skip the address portion of an ed command line.
   Return index of the command character (or len if only addresses). */
static int skip_address( const char * const buf, const int len )
  {
  int i = 0;
  while( i < len )
    {
    while( i < len && ( buf[i] == ' ' || buf[i] == '\t' ) ) ++i;
    if( i >= len ) break;
    const unsigned char c = buf[i];
    if( c >= '0' && c <= '9' )
      { while( i < len && buf[i] >= '0' && buf[i] <= '9' ) ++i; continue; }
    if( c == '.' || c == '$' || c == '+' || c == '-' ||
        c == ',' || c == ';' || c == '%' ) { ++i; continue; }
    if( c == '\'' && i + 1 < len ) { i += 2; continue; }
    if( c == '/' )
      { ++i; while( i < len && buf[i] != '/' )
        { if( buf[i] == '\\' && i + 1 < len ) ++i; ++i; }
      if( i < len ) ++i; continue; }
    if( c == '?' )
      { ++i; while( i < len && buf[i] != '?' )
        { if( buf[i] == '\\' && i + 1 < len ) ++i; ++i; }
      if( i < len ) ++i; continue; }
    break;
    }
  return i;
  }


/* Suffix completion state */
static char suffix_candidates[20];
static int suffix_gen_idx;

/* Generator: returns text + one suffix candidate per call */
static char * suffix_generator( const char * text, int state )
  {
  if( state == 0 ) suffix_gen_idx = 0;
  if( !suffix_candidates[suffix_gen_idx] ) return NULL;
  const int len = strlen( text );
  char * match = (char *)malloc( len + 2 );
  if( !match ) return NULL;
  memcpy( match, text, len );
  match[len] = suffix_candidates[suffix_gen_idx++];
  match[len + 1] = 0;
  return match;
  }


/* Fill suffix_candidates with available print suffixes (l, n, p) */
static void get_print_candidates( const char * const buf,
                                  const int from, const int to )
  {
  bool has_l = false, has_n = false, has_p = false;
  int i;
  for( i = from; i < to; ++i )
    {
    if( buf[i] == 'l' ) has_l = true;
    else if( buf[i] == 'n' ) has_n = true;
    else if( buf[i] == 'p' ) has_p = true;
    }
  int ci = 0;
  if( !has_l ) suffix_candidates[ci++] = 'l';
  if( !has_n ) suffix_candidates[ci++] = 'n';
  if( !has_p ) suffix_candidates[ci++] = 'p';
  suffix_candidates[ci] = 0;
  }


/* Fill suffix_candidates with available s-command flags.
   Return true if cursor is in a completable flag position. */
static bool get_s_candidates( const char * const buf,
                              const int cmd_idx, const int len )
  {
  int i = cmd_idx + 1;
  if( i >= len ) return false;		/* just 's', ambiguous */

  const unsigned char c = buf[i];

  /* repeat mode: first char after s is digit, g, p, or r */
  if( ( c >= '1' && c <= '9' ) || c == 'g' || c == 'p' || c == 'r' )
    {
    bool has_g = false, has_p = false, has_r = false;
    while( i < len )
      {
      const unsigned char ch = buf[i];
      if( ch >= '0' && ch <= '9' )
        { has_g = true;
          while( i < len && buf[i] >= '0' && buf[i] <= '9' ) ++i;
          continue; }
      if( ch == 'g' ) { has_g = true; ++i; continue; }
      if( ch == 'p' ) { has_p = true; ++i; continue; }
      if( ch == 'r' ) { has_r = true; ++i; continue; }
      break;
      }
    int ci = 0;
    if( !has_g )
      { int d; for( d = '1'; d <= '9'; ++d ) suffix_candidates[ci++] = d;
        suffix_candidates[ci++] = 'g'; }
    if( !has_p ) suffix_candidates[ci++] = 'p';
    if( !has_r ) suffix_candidates[ci++] = 'r';
    suffix_candidates[ci] = 0;
    return ci > 0;
    }

  /* new pattern mode: first char is delimiter */
  const unsigned char delim = c;
  ++i;
  /* skip pattern */
  while( i < len && buf[i] != delim )
    { if( buf[i] == '\\' && i + 1 < len ) ++i; ++i; }
  if( i >= len ) return false;		/* still in pattern */
  ++i;					/* skip 2nd delimiter */
  /* skip replacement */
  while( i < len && buf[i] != delim && buf[i] != '\n' )
    { if( buf[i] == '\\' && i + 1 < len ) ++i; ++i; }
  if( i >= len ) return false;		/* still in replacement */
  if( buf[i] == delim ) ++i;		/* skip 3rd delimiter */

  /* parse existing flags after 3rd delimiter */
  bool has_count = false, has_g = false, has_ic = false;
  bool has_l = false, has_n = false, has_p = false;
  while( i < len )
    {
    const unsigned char ch = buf[i];
    if( ch >= '1' && ch <= '9' )
      { has_count = true;
        while( i < len && buf[i] >= '0' && buf[i] <= '9' ) ++i;
        continue; }
    if( ch == 'g' ) { has_g = true; ++i; continue; }
    if( ch == 'i' || ch == 'I' ) { has_ic = true; ++i; continue; }
    if( ch == 'l' ) { has_l = true; ++i; continue; }
    if( ch == 'n' ) { has_n = true; ++i; continue; }
    if( ch == 'p' ) { has_p = true; ++i; continue; }
    break;
    }

  int ci = 0;
  if( !has_count && !has_g )
    { int d; for( d = '1'; d <= '9'; ++d ) suffix_candidates[ci++] = d;
      suffix_candidates[ci++] = 'g'; }
  if( !has_ic ) suffix_candidates[ci++] = 'i';
  if( !has_l ) suffix_candidates[ci++] = 'l';
  if( !has_n ) suffix_candidates[ci++] = 'n';
  if( !has_p ) suffix_candidates[ci++] = 'p';
  suffix_candidates[ci] = 0;
  return ci > 0;
  }


static char **ed_completion( const char * text, int start, int end )
  {
  (void)end;
  const int cmd_idx = skip_address( rl_line_buffer, rl_end );

  if( cmd_idx >= rl_end )
    { rl_attempted_completion_over = 1; return NULL; }

  const char cmd = rl_line_buffer[cmd_idx];

  /* cursor past command word: completing a space-separated argument */
  if( cmd_idx < start )
    {
    if( cmd == 'e' || cmd == 'E' || cmd == 'f' ||
        cmd == 'r' || cmd == 'w' || cmd == 'W' || cmd == '!' )
      return rl_completion_matches( text, rl_filename_completion_function );
    rl_attempted_completion_over = 1;
    return NULL;
    }

  /* cursor in same word as command: suffix/flag completion */
  rl_attempted_completion_over = 1;

  /* s command: special flag handling */
  if( cmd == 's' )
    {
    if( get_s_candidates( rl_line_buffer, cmd_idx, rl_end ) )
      return rl_completion_matches( text, suffix_generator );
    return NULL;
    }

  /* determine suffix area start for commands with print suffixes */
  int suf = cmd_idx + 1;
  bool has_suffix = false;

  switch( cmd )
    {
    /* simple commands: suffixes right after command letter */
    case 'a': case 'c': case 'd': case 'h': case 'H':
    case 'i': case 'j': case 'l': case 'n': case 'p':
    case 'P': case 'q': case 'Q': case 'S': case 'u':
    case 'x': case 'y': case '=':
      has_suffix = true;
      break;
    /* k: mark char then suffixes */
    case 'k':
      if( cmd_idx + 1 < rl_end ) { suf = cmd_idx + 2; has_suffix = true; }
      break;
    /* z: optional digits then suffixes */
    case 'z':
      while( suf < rl_end && rl_line_buffer[suf] >= '0' &&
             rl_line_buffer[suf] <= '9' ) ++suf;
      has_suffix = true;
      break;
    /* G/V: pattern delimiter then suffixes */
    case 'G': case 'V':
      if( suf < rl_end )
        {
        const unsigned char delim = rl_line_buffer[suf];
        ++suf;
        while( suf < rl_end && rl_line_buffer[suf] != delim )
          { if( rl_line_buffer[suf] == '\\' && suf + 1 < rl_end ) ++suf; ++suf; }
        if( suf < rl_end ) { ++suf; has_suffix = true; }
        }
      break;
    }

  if( has_suffix )
    {
    get_print_candidates( rl_line_buffer, suf, rl_end );
    if( suffix_candidates[0] )
      return rl_completion_matches( text, suffix_generator );
    }

  return NULL;
  }


static void switch_to_input_history( void )
  {
  if( in_input_mode ) return;
  const char * hp = history_path();
  if( hp ) write_history( hp );
  clear_history();
  stifle_history( 1000 );
  hp = input_history_path();
  if( hp ) read_history( hp );
  rl_bind_key( '\t', rl_insert );
  in_input_mode = true;
  }

static void switch_to_cmd_history( void )
  {
  if( !in_input_mode ) return;
  const char * hp = input_history_path();
  if( hp ) write_history( hp );
  clear_history();
  stifle_history( 1000 );
  hp = history_path();
  if( hp ) read_history( hp );
  rl_bind_key( '\t', rl_complete );
  in_input_mode = false;
  }
#endif


void end_input_mode( void )
  {
#ifdef HAVE_READLINE
  if( isatty( 0 ) ) switch_to_cmd_history();
#endif
  }


/* Read a line of text from stdin using readline in input mode.
   Add non-empty lines to input history.
   Return pointer to buffer and line size (including trailing newline),
   or 0 if error, or *sizep = 0 if EOF.
*/
const char * get_tty_input_line( int * const sizep )
  {
#ifdef HAVE_READLINE
  if( isatty( 0 ) )
    {
    static char * buf = 0;
    static int bufsz = 0;
    switch_to_input_history();
    char * line = readline( "" );
    if( !line )				/* EOF */
      {
      if( !resize_buffer( &buf, &bufsz, 2 ) ) { *sizep = 0; return 0; }
      buf[0] = 0; *sizep = 0; return buf;
      }
    const int len = strlen( line );
    if( !resize_buffer( &buf, &bufsz, len + 2 ) )
      { free( line ); *sizep = 0; return 0; }
    memcpy( buf, line, len );
    buf[len] = '\n'; buf[len + 1] = 0;
    if( len > 0 ) add_history( line );
    free( line );
    ++linenum_;
    *sizep = len + 1;
    return buf;
    }
#endif
  return get_stdin_line( sizep );
  }


void init_readline( void )
  {
#ifdef HAVE_READLINE
  rl_readline_name = "ed";
  rl_attempted_completion_function = ed_completion;
  using_history();
  stifle_history( 1000 );
  const char * const hp = history_path();
  if( hp ) read_history( hp );
  in_input_mode = false;
#endif
  }


void cleanup_readline( void )
  {
#ifdef HAVE_READLINE
  const char * hp;
  if( in_input_mode )
    hp = input_history_path();
  else
    hp = history_path();
  if( hp ) write_history( hp );
#endif
  }


/* Read a line of text from a stream.
   Return pointer to buffer and line size (including trailing newline
   if it exists and is not added now).
*/
static const char * read_stream_line( const char * const filename,
                                      FILE * const fp, int * const sizep,
                                      bool * const newline_addedp )
  {
  static char * buf = 0;
  static int bufsz = 0;
  int c, i = 0;

  while( true )
    {
    if( !resize_buffer( &buf, &bufsz, i + 2 ) ) return 0;
    c = getc( fp ); if( c == EOF ) break;
    buf[i++] = c;
    if( c == 0 ) set_binary();
    else if( c == '\n' )		/* remove CR only from CR/LF pairs */
      { if( strip_cr() && i > 1 && buf[i-2] == '\r' ) { buf[i-2] = '\n'; --i; }
        break; }
    }
  buf[i] = 0;
  if( c == EOF )
    {
    if( ferror( fp ) )
      {
      show_strerror( filename, errno );
      set_error_msg( "Cannot read input file" );
      return 0;
      }
    else if( i )
      {
      buf[i] = '\n'; buf[i+1] = 0; *newline_addedp = true;
      if( !isbinary() ) ++i;
      }
    }
  *sizep = i;
  return buf;
  }


/* Read a stream into the editor buffer.
   Return number of bytes read, or -1 if error.
*/
static long read_stream( const char * const filename, FILE * const fp,
                         const int addr )
  {
  line_node * lp = search_line_node( addr );
  undo_atom * up = 0;
  long total_size = 0;		/* number of bytes read */
  const bool o_isbinary = isbinary();
  const bool appended = ( addr == last_addr() );
  const bool o_unterminated_last_line = unterminated_last_line();
  bool newline_added = false;

  set_current_addr( addr );
  while( true )
    {
    int size = 0;
    const char * const s =
      read_stream_line( filename, fp, &size, &newline_added );
    if( !s ) return -1;
    if( size <= 0 ) break;
    total_size += size;
    disable_interrupts();
    if( !put_sbuf_line( s, size + newline_added ) )
      { enable_interrupts(); return -1; }
    lp = lp->q_forw;
    if( up ) up->tail = lp;
    else
      {
      up = push_undo_atom( UADD, current_addr(), current_addr() );
      if( !up ) { enable_interrupts(); return -1; }
      }
    enable_interrupts();
    }
  if( !scripted() )
    { if( addr && appended && total_size && o_unterminated_last_line )
        fputs( "Newline inserted\n", stdout );		/* before stream */
      else if( newline_added && ( !appended || !isbinary() ) )
        fputs( "Newline appended\n", stdout ); }	/* after stream */
  if( !appended && isbinary() && !o_isbinary && newline_added )
    ++total_size;
  if( appended && isbinary() && ( newline_added || total_size == 0 ) )
    unterminated_line = search_line_node( last_addr() );
  return total_size;
  }


/* Read a named file/pipe into the buffer.
   Return line count, -1 if file not found, -2 if fatal error.
*/
int read_file( const char * const filename, const int addr,
               bool * const read_onlyp )
  {
  FILE * fp;
  int ret;

  if( *filename == '!' ) fp = popen( filename + 1, "r" );
  else if( !( fp = fopen( filename, "r+" ) ) && errno != ENOENT &&
           ( fp = fopen( filename, "r" ) ) && read_onlyp && !modified() )
    *read_onlyp = true;
  if( !fp )
    { show_strerror( filename, errno );
      set_error_msg( "Cannot open input file" ); return -1; }
  const long size = read_stream( filename, fp, addr );	/* file size in bytes */
  if( *filename == '!' ) ret = pclose( fp ); else ret = fclose( fp );
  if( size < 0 ) return -2;
  if( ret < 0 )
    { show_strerror( filename, errno );
      set_error_msg( "Cannot close input file" ); return -2; }
  if( !scripted() ) printf( "%lu\n", size );
  return current_addr() - addr;	/* current_addr updated by add_line_node */
  }


/* Write a range of lines to a stream.
   Return number of bytes written, or -1 if error.
*/
static long write_stream( const char * const filename, FILE * const fp,
                          int from, const int to )
  {
  line_node * lp = search_line_node( from );
  long size = 0;		/* number of bytes written */

  while( from && from <= to )
    {
    int len;
    char * p = get_sbuf_line( lp );
    if( !p ) return -1;
    len = lp->len;
    if( from != last_addr() || !isbinary() || !unterminated_last_line() )
      p[len++] = '\n';
    size += len;
    while( --len >= 0 )
      if( fputc( *p++, fp ) == EOF )
        {
        show_strerror( filename, errno );
        set_error_msg( "Cannot write file" );
        return -1;
        }
    ++from; lp = lp->q_forw;
    }
  return size;
  }


/* Write a range of lines to a named file/pipe.
   Return line count, or -1 if error.
*/
int write_file( const char * const filename, const char * const mode,
                const int from, const int to )
  {
  FILE * fp;
  int ret;

  if( *filename == '!' ) fp = popen( filename + 1, "w" );
  else fp = fopen( filename, mode );
  if( !fp )
    { show_strerror( filename, errno );
      set_error_msg( "Cannot open output file" ); return -1; }
  const long size = write_stream( filename, fp, from, to );	/* bytes written */
  if( *filename == '!' ) ret = pclose( fp ); else ret = fclose( fp );
  if( size < 0 ) return -1;
  if( ret < 0 )
    { show_strerror( filename, errno );
      set_error_msg( "Cannot close output file" ); return -1; }
  if( !scripted() ) printf( "%lu\n", size );
  return ( from && from <= to ) ? to - from + 1 : 0;
  }
