/* highlight.c: syntax highlighting for the ed line editor */
/* GNU ed - The GNU line editor.
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ed.h"


static bool highlight_on = false;
static const char * forced_lang = NULL;	/* language set via -S flag */

bool highlight_enabled( void ) { return highlight_on; }
void toggle_highlight( void ) { highlight_on = !highlight_on; }
void set_highlight( const bool enable ) { highlight_on = enable; }

bool set_syntax( const char * const lang )
  {
  static char * buf = 0;
  static int bufsz = 0;
  const int len = strlen( lang );

  if( strcmp( lang, "auto" ) == 0 ) { forced_lang = NULL; highlight_on = true; return true; }
  if( !resize_buffer( &buf, &bufsz, len + 1 ) ) return false;
  memcpy( buf, lang, len + 1 );
  forced_lang = buf;
  highlight_on = true;
  return true;
  }


enum Highlighter { HL_NONE = 0, HL_BAT, HL_PYGMENTIZE, HL_SOURCE_HIGHLIGHT,
                   HL_HIGHLIGHT };

static bool hl_avail[5] = { false, false, false, false, false };
static bool hl_probed = false;

static bool command_exists( const char * const name )
  {
  const char * const path = getenv( "PATH" );
  if( !path ) return false;
  const int namelen = strlen( name );
  const char * p = path;
  while( *p )
    {
    const char * const sep = strchr( p, ':' );
    const int dirlen = sep ? (int)( sep - p ) : (int)strlen( p );
    if( dirlen > 0 && dirlen + namelen + 2 < 4096 )
      {
      char buf[4096];
      memcpy( buf, p, dirlen );
      buf[dirlen] = '/';
      memcpy( buf + dirlen + 1, name, namelen + 1 );
      if( access( buf, X_OK ) == 0 ) return true;
      }
    if( !sep ) break;
    p = sep + 1;
    }
  return false;
  }


static void probe_highlighters( void )
  {
  if( hl_probed ) return;
  hl_avail[HL_BAT] = command_exists( "bat" );
  hl_avail[HL_PYGMENTIZE] = command_exists( "pygmentize" );
  hl_avail[HL_SOURCE_HIGHLIGHT] = command_exists( "source-highlight" );
  hl_avail[HL_HIGHLIGHT] = command_exists( "highlight" );
  hl_probed = true;
  }

/* Return the next available highlighter starting after 'after'.
   Pass HL_NONE to get the first available. */
static int next_highlighter( const int after )
  {
  int i;
  probe_highlighters();
  for( i = ( after < HL_BAT ? HL_BAT : after + 1 ); i <= HL_HIGHLIGHT; ++i )
    if( hl_avail[i] ) return i;
  return HL_NONE;
  }


static const char * detect_language( const char * const filename )
  {
  static const struct { const char * ext; const char * lang; } lang_map[] =
    {
    { "c", "c" }, { "h", "c" },
    { "cc", "cpp" }, { "cpp", "cpp" }, { "cxx", "cpp" },
    { "hh", "cpp" }, { "hpp", "cpp" },
    { "py", "python" }, { "pyw", "python" },
    { "js", "javascript" }, { "mjs", "javascript" },
    { "ts", "typescript" }, { "tsx", "tsx" }, { "jsx", "jsx" },
    { "rb", "ruby" },
    { "go", "go" },
    { "rs", "rust" },
    { "java", "java" },
    { "sh", "bash" }, { "bash", "bash" }, { "zsh", "zsh" },
    { "html", "html" }, { "htm", "html" },
    { "css", "css" }, { "scss", "scss" },
    { "json", "json" },
    { "yaml", "yaml" }, { "yml", "yaml" },
    { "xml", "xml" }, { "svg", "xml" },
    { "md", "markdown" }, { "markdown", "markdown" },
    { "sql", "sql" },
    { "lua", "lua" },
    { "pl", "perl" }, { "pm", "perl" },
    { "el", "elisp" }, { "lisp", "lisp" }, { "cl", "lisp" },
    { "hs", "haskell" },
    { "ml", "ocaml" }, { "mli", "ocaml" },
    { "tex", "latex" }, { "sty", "latex" },
    { "toml", "toml" },
    { "ini", "ini" }, { "conf", "ini" },
    { "swift", "swift" },
    { "kt", "kotlin" },
    { "scala", "scala" },
    { "r", "r" }, { "R", "r" },
    { "php", "php" },
    { "vim", "vim" },
    { "zig", "zig" },
    { "nim", "nim" },
    { "ex", "elixir" }, { "exs", "elixir" },
    { "erl", "erlang" },
    { "clj", "clojure" },
    { "dart", "dart" },
    { "cs", "csharp" },
    { "fs", "fsharp" },
    { "d", "d" },
    { "m", "objc" },
    { "mm", "objcpp" },
    { "fth", "forth" }, { "4th", "forth" }, { "forth", "forth" },
    { NULL, NULL }
    };

  if( !filename || !filename[0] ) return NULL;
  const char * const dot = strrchr( filename, '.' );
  if( !dot || dot == filename ) return NULL;
  const char * const ext = dot + 1;
  int i;
  for( i = 0; lang_map[i].ext; ++i )
    if( strcmp( ext, lang_map[i].ext ) == 0 ) return lang_map[i].lang;
  return NULL;
  }


/* Build argv for the highlighter process.
   Returns number of args, fills argv (must have room for at least 12 entries). */
static int build_argv( char * argv[], const int hl,
                       const char * const lang,
                       const char * const filename )
  {
  int argc = 0;
  switch( hl )
    {
    case HL_BAT:
      argv[argc++] = "bat";
      argv[argc++] = "--color=always";
      argv[argc++] = "--style=plain";
      argv[argc++] = "--paging=never";
      if( lang ) { argv[argc++] = "--language"; argv[argc++] = (char *)lang; }
      else if( filename && filename[0] )
        { argv[argc++] = "--file-name"; argv[argc++] = (char *)filename; }
      break;
    case HL_PYGMENTIZE:
      argv[argc++] = "pygmentize";
      argv[argc++] = "-f"; argv[argc++] = "terminal256";
      if( lang ) { argv[argc++] = "-l"; argv[argc++] = (char *)lang; }
      else { argv[argc++] = "-g"; }	/* guess */
      break;
    case HL_SOURCE_HIGHLIGHT:
      argv[argc++] = "source-highlight";
      argv[argc++] = "-f"; argv[argc++] = "esc";
      if( lang ) { argv[argc++] = "-s"; argv[argc++] = (char *)lang; }
      break;
    case HL_HIGHLIGHT:
      argv[argc++] = "highlight";
      argv[argc++] = "-O"; argv[argc++] = "ansi";
      if( lang ) { argv[argc++] = "-S"; argv[argc++] = (char *)lang; }
      break;
    default: return 0;
    }
  argv[argc] = NULL;
  return argc;
  }


/* Try a single highlighter; return true if it produced output. */
static bool try_highlighter( const int hl, const char * const lang,
                             const char * const filename,
                             const int from, const int to, const int pflags )
  {
  char * argv[12];
  if( build_argv( argv, hl, lang, filename ) == 0 ) return false;

  int stdin_pipe[2], stdout_pipe[2];
  if( pipe( stdin_pipe ) < 0 || pipe( stdout_pipe ) < 0 ) return false;

  disable_interrupts();
  const pid_t pid = fork();
  if( pid < 0 )
    {
    close( stdin_pipe[0] ); close( stdin_pipe[1] );
    close( stdout_pipe[0] ); close( stdout_pipe[1] );
    enable_interrupts();
    return false;
    }

  if( pid == 0 )
    {
    /* child */
    const int devnull = open( "/dev/null", O_WRONLY );
    if( devnull >= 0 ) { dup2( devnull, STDERR_FILENO ); close( devnull ); }
    close( stdin_pipe[1] ); close( stdout_pipe[0] );
    dup2( stdin_pipe[0], STDIN_FILENO );
    dup2( stdout_pipe[1], STDOUT_FILENO );
    close( stdin_pipe[0] ); close( stdout_pipe[1] );
    execvp( argv[0], argv );
    _exit( 127 );
    }

  /* parent */
  close( stdin_pipe[0] ); close( stdout_pipe[1] );

  /* write lines 1..to to the highlighter's stdin */
    {
    line_node * lp = search_line_node( 1 );
    int addr;
    for( addr = 1; addr <= to; ++addr )
      {
      char * const s = get_sbuf_line( lp );
      if( s )
        {
        write( stdin_pipe[1], s, lp->len );
        write( stdin_pipe[1], "\n", 1 );
        }
      lp = lp->q_forw;
      }
    }
  close( stdin_pipe[1] );

  /* read highlighted output, skip first (from-1) lines, print the rest */
  bool got_output = false;
    {
    FILE * const fp = fdopen( stdout_pipe[0], "r" );
    if( fp )
      {
      char * line = NULL;
      size_t linesz = 0;
      int linenum = 0;
      int print_addr = from;
      while( getline( &line, &linesz, fp ) >= 0 )
        {
        got_output = true;
        ++linenum;
        if( linenum >= from && linenum <= to )
          {
          set_current_addr( print_addr );
          if( pflags & pf_n ) printf( "%d\t", print_addr );
          fputs( line, stdout );
          /* getline includes newline; if missing, add one */
          if( line[0] && line[strlen( line ) - 1] != '\n' ) putchar( '\n' );
          ++print_addr;
          }
        if( linenum >= to ) break;
        }
      free( line );
      fclose( fp );		/* also closes stdout_pipe[0] */
      }
    else
      close( stdout_pipe[0] );
    }

  int status;
  waitpid( pid, &status, 0 );
  enable_interrupts();
  return got_output;
  }


bool print_lines_highlighted( int from, const int to, const int pflags )
  {
  if( !from ) { invalid_address(); return false; }

  const char * const filename = get_def_filename();
  const char * const lang = forced_lang ? forced_lang : detect_language( filename );

  int hl = HL_NONE;
  while( ( hl = next_highlighter( hl ) ) != HL_NONE )
    if( try_highlighter( hl, lang, filename, from, to, pflags ) ) return true;

  return print_lines( from, to, pflags );	/* all failed; plain fallback */
  }
