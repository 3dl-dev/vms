/* input_file.c - Deal with Input Files -
   Copyright (C) 1987-2024 Free Software Foundation, Inc.

   This file is part of GAS, the GNU Assembler.

   GAS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GAS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GAS; see the file COPYING.  If not, write to the Free
   Software Foundation, 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Confines all details of reading source bytes to this module.
   All O/S specific crocks should live here.
   What we lose in "efficiency" we gain in modularity.
   Note we don't need to #include the "as.h" file. No common coupling!  */

#include "as.h"
#include "input-file.h"
#include "safe-ctype.h"

#ifdef OVMX_RMS_IO
/* OVMX (vms-0b6b): route the PRIMARY .s source read through OVMX's RMS
 * system services instead of raw fopen()/fread()/fclose() — see
 * third-party/binutils/ovmx/ovmx_rms_io.h for scope/rationale. */
#include "ovmx_rms_io.h"
#endif

/* This variable is non-zero if the file currently being read should be
   preprocessed by app.  It is zero if the file can be read straight in.  */
int preprocess = 0;

/* This code opens a file, then delivers BUFFER_SIZE character
   chunks of the file on demand.
   BUFFER_SIZE is supposed to be a number chosen for speed.
   The caller only asks once what BUFFER_SIZE is, and asks before
   the nature of the input files (if any) is known.  */

#define BUFFER_SIZE (32 * 1024)

/* We use static data: the data area is not sharable.  */

static FILE *f_in;
static const char *file_name;
#ifdef OVMX_RMS_IO
/* OVMX (vms-0b6b): >= 0 while the primary .s source is open via RMS
 * (ovmx_rms_open_read); -1 otherwise (stdin, or the stock fopen() path
 * never taken when this file is compiled with OVMX_RMS_IO). f_in is set to
 * a non-NULL sentinel (never dereferenced as a real FILE*) whenever this is
 * >= 0, so the existing "is a file open" checks below keep working. */
static int ovmx_rms_fd = -1;
#endif

/* Struct for saving the state of this module for file includes.  */
struct saved_file
  {
    FILE * f_in;
    const char * file_name;
    int    preprocess;
    char * app_save;
  };

/* These hooks accommodate most operating systems.  */

void
input_file_begin (void)
{
  f_in = (FILE *) 0;
}

void
input_file_end (void)
{
}

/* Return BUFFER_SIZE.  */
size_t
input_file_buffer_size (void)
{
  return (BUFFER_SIZE);
}

/* Push the state of our input, returning a pointer to saved info that
   can be restored with input_file_pop ().  */

char *
input_file_push (void)
{
  struct saved_file *saved;

  saved = XNEW (struct saved_file);

  saved->f_in = f_in;
  saved->file_name = file_name;
  saved->preprocess = preprocess;
  if (preprocess)
    saved->app_save = app_push ();

  /* Initialize for new file.  */
  input_file_begin ();

  return (char *) saved;
}

void
input_file_pop (char *arg)
{
  struct saved_file *saved = (struct saved_file *) arg;

  input_file_end ();		/* Close out old file.  */

  f_in = saved->f_in;
  file_name = saved->file_name;
  preprocess = saved->preprocess;
  if (preprocess)
    app_pop (saved->app_save);

  free (arg);
}

/* Open the specified file, "" means stdin.  Filename must not be null.  */

void
input_file_open (const char *filename,
		 int pre)
{
  int c;
  char buf[80];

  preprocess = pre;

  gas_assert (filename != 0);	/* Filename may not be NULL.  */
#ifdef OVMX_RMS_IO
  if (filename[0])
    {
      /* OVMX (vms-0b6b): the primary .s source, opened via RMS instead of
       * fopen(). f_in gets a non-NULL sentinel so the "is a file open"
       * checks in input_file_give_next_buffer/input_file_close still work;
       * it is never dereferenced as a real FILE* on this path. */
      ovmx_rms_fd = ovmx_rms_open_read (filename);
      f_in = (ovmx_rms_fd >= 0) ? (FILE *) (intptr_t) 1 : NULL;
      file_name = filename;
    }
  else
    {
      /* Use stdin for the input file — OVMX RMS routing applies only to
       * the named primary source (see ovmx_rms_io.h SCOPE); stdin stays on
       * the stock path. */
      ovmx_rms_fd = -1;
      f_in = stdin;
      file_name = _("{standard input}");
    }

  if (f_in == NULL)
    {
      as_bad (_("can't open %s for reading: %s"),
	      file_name, xstrerror (errno));
      return;
    }

  /* OVMX (vms-0b6b): the stock #NO_APP/#APP leading-comment sniff below
   * (a single-character stdio getc()/ungetc() peek) is deliberately SKIPPED
   * for an RMS-opened file — see ovmx_rms_io.h SCOPE for why. `preprocess`
   * keeps whatever the caller passed via `pre`. This only affects the
   * heuristic that recognizes cpp/gcc-emitted "#NO_APP"/"#APP" markers at
   * the very start of a .s file; hand-written test assembly (no leading
   * '#' line) is unaffected. */
  if (ovmx_rms_fd >= 0)
    return;
  c = getc (f_in);
#else
  if (filename[0])
    {
      f_in = fopen (filename, FOPEN_RT);
      file_name = filename;
    }
  else
    {
      /* Use stdin for the input file.  */
      f_in = stdin;
      /* For error messages.  */
      file_name = _("{standard input}");
    }

  if (f_in == NULL)
    {
      as_bad (_("can't open %s for reading: %s"),
	      file_name, xstrerror (errno));
      return;
    }

  c = getc (f_in);
#endif

  if (ferror (f_in))
    {
      as_bad (_("can't read from %s: %s"),
	      file_name, xstrerror (errno));

      fclose (f_in);
      f_in = NULL;
      return;
    }

  /* Check for an empty input file.  */
  if (feof (f_in))
    {
      fclose (f_in);
      f_in = NULL;
      return;
    }
  gas_assert (c != EOF);

  if (c == '#')
    {
      /* Begins with comment, may not want to preprocess.  */
      c = getc (f_in);
      if (c == 'N')
	{
	  char *p = fgets (buf, sizeof (buf), f_in);
	  if (p && startswith (p, "O_APP") && ISSPACE (p[5]))
	    preprocess = 0;
	  if (!p || !strchr (p, '\n'))
	    ungetc ('#', f_in);
	  else
	    ungetc ('\n', f_in);
	}
      else if (c == 'A')
	{
	  char *p = fgets (buf, sizeof (buf), f_in);
	  if (p && startswith (p, "PP") && ISSPACE (p[2]))
	    preprocess = 1;
	  if (!p || !strchr (p, '\n'))
	    ungetc ('#', f_in);
	  else
	    ungetc ('\n', f_in);
	}
      else if (c == '\n')
	ungetc ('\n', f_in);
      else
	ungetc ('#', f_in);
    }
  else
    ungetc (c, f_in);
}

/* Close input file.  */

void
input_file_close (void)
{
#ifdef OVMX_RMS_IO
  /* OVMX (vms-0b6b): close the RMS-backed primary source, if that's what's
   * open; the stdin fallback stays on the stock fclose() path. */
  if (ovmx_rms_fd >= 0)
    {
      ovmx_rms_close_read (ovmx_rms_fd);
      ovmx_rms_fd = -1;
      f_in = 0;
      return;
    }
#endif
  /* Don't close a null file pointer.  */
  if (f_in != NULL)
    fclose (f_in);

  f_in = 0;
}

/* This function is passed to do_scrub_chars.  */

static size_t
input_file_get (char *buf, size_t buflen)
{
  size_t size;

#ifdef OVMX_RMS_IO
  /* OVMX (vms-0b6b): the RMS-backed primary source read — ovmx_rms_read
   * mirrors fread()'s contract (bytes copied, 0 at EOF, <0 on error). */
  if (ovmx_rms_fd >= 0)
    {
      int r = ovmx_rms_read (ovmx_rms_fd, buf, (int) buflen);
      if (r < 0)
	{
	  as_bad (_("can't read from %s (OVMX RMS)"), file_name);
	  return 0;
	}
      return (size_t) r;
    }
#endif

  if (feof (f_in))
    return 0;

  size = fread (buf, sizeof (char), buflen, f_in);
  if (ferror (f_in))
    as_bad (_("can't read from %s: %s"), file_name, xstrerror (errno));
  return size;
}

/* Read a buffer from the input file.  */

char *
input_file_give_next_buffer (char *where /* Where to place 1st character of new buffer.  */)
{
  char *return_value;		/* -> Last char of what we read, + 1.  */
  size_t size;

  if (f_in == (FILE *) 0)
    return 0;
  /* fflush (stdin); could be done here if you want to synchronise
     stdin and stdout, for the case where our input file is stdin.
     Since the assembler shouldn't do any output to stdout, we
     don't bother to synch output and input.  */
  if (preprocess)
    size = do_scrub_chars (input_file_get, where, BUFFER_SIZE,
                           multibyte_handling == multibyte_warn);
  else
    {
      size = input_file_get (where, BUFFER_SIZE);

      if (multibyte_handling == multibyte_warn)
	{
	  const unsigned char *start = (const unsigned char *) where;

	  (void) scan_for_multibyte_characters (start, start + size,
						true /* Generate warnings */);
	}
    }

  if (size)
    return_value = where + size;
  else
    {
#ifdef OVMX_RMS_IO
      /* OVMX (vms-0b6b): auto-close-on-EOF for the RMS-backed primary
       * source; the stdin fallback keeps the stock fclose(). */
      if (ovmx_rms_fd >= 0)
	{
	  if (ovmx_rms_close_read (ovmx_rms_fd))
	    as_warn (_("can't close %s (OVMX RMS)"), file_name);
	  ovmx_rms_fd = -1;
	  f_in = (FILE *) 0;
	  return 0;
	}
#endif
      if (fclose (f_in))
	as_warn (_("can't close %s: %s"), file_name, xstrerror (errno));

      f_in = (FILE *) 0;
      return_value = 0;
    }

  return return_value;
}
