/* Miscellaneous generic support functions for GNU Make.
Copyright (C) 1988-2022 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "makeint.h"
#include "filedef.h"
#include "dep.h"
#include "debug.h"

/* GNU make no longer supports pre-ANSI89 environments.  */

#include <stdarg.h>
#include <assert.h>

#ifdef WINDOWS32
# include <windows.h>
# include <io.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#else
# include <sys/file.h>
#endif

unsigned int
make_toui (const char *str, const char **error)
{
  char *end;
  unsigned long val = strtoul (str, &end, 10);

  if (error)
    {
      if (str[0] == '\0')
        *error = "Missing value";
      else if (*end != '\0')
        *error = "Invalid value";
      else
        *error = NULL;
    }

  return val;
}

/* Compare strings *S1 and *S2.
   Return negative if the first is less, positive if it is greater,
   zero if they are equal.  */

int
alpha_compare (const void *v1, const void *v2)
{
  const char *s1 = *((char **)v1);
  const char *s2 = *((char **)v2);

  if (*s1 != *s2)
    return *s1 - *s2;
  return strcmp (s1, s2);
}

/* Discard each backslash-newline combination from LINE.
   Backslash-backslash-newline combinations become backslash-newlines.
   This is done by copying the text at LINE into itself.  */

void
collapse_continuations (char *line)
{
  char *out = line;
  char *in = line;
  char *q;

  q = strchr(in, '\n');
  if (q == 0)
    return;

  do
    {
      char *p = q;
      int i;
      size_t out_line_length;

      if (q > line && q[-1] == '\\')
        {
          /* Search for more backslashes.  */
          i = -2;
          while (&p[i] >= line && p[i] == '\\')
            --i;
          ++i;
        }
      else
        i = 0;

      /* The number of backslashes is now -I, keep half of them.  */
      out_line_length = (p - in) + i - i/2;
      if (out != in)
        memmove (out, in, out_line_length);
      out += out_line_length;

      /* When advancing IN, skip the newline too.  */
      in = q + 1;

      if (i & 1)
        {
          /* Backslash/newline handling:
             In traditional GNU make all trailing whitespace, consecutive
             backslash/newlines, and any leading non-newline whitespace on the
             next line is reduced to a single space.
             In POSIX, each backslash/newline and is replaced by a space.  */
          while (ISBLANK (*in))
            ++in;
          if (! posix_pedantic)
            while (out > line && ISBLANK (out[-1]))
              --out;
          *out++ = ' ';
        }
      else
        {
          /* If the newline isn't quoted, put it in the output.  */
          *out++ = '\n';
        }

      q = strchr(in, '\n');
    }
  while (q);

  memmove(out, in, strlen(in) + 1);
}

/* Return 1 if this character is a file path separator.
 * Return 0 otherwise.  */
static int
pathsep (char c)
{
  if (c == '/')
    return 1;
#ifdef HAVE_DOS_PATHS
  if (c == '\\')
    return 1;
#endif
  return 0;
}

/* Normalize filepath by removing redundant ./ and collapsing successive
 * slashes. */

char *
normalize (char *s)
{
  size_t slen = strlen (s);

  while (slen > 2 && s[0] == '.' && pathsep (s[1]))
    {
      /* Skip "./" and all following slashes.  */
      s += 2;
      slen -= 2;
      for (; pathsep (*s); --slen)
        ++s;
    }

  /* Transform foo/.///.///bar/ foo/bar/.  */
  for (;;)
    {
      char *u = strstr (s, "/./");
      if (u)
        {
          /* n is the number of characters to remove.
           * 2 for ./ plus all successive slashes.  */
          size_t n = 2 + strspn (u + 3, "/");
          assert (slen > n);
          ++u;
          memmove (u, u + n, slen - (u - s) - n);
          s[slen - n] = '\0';
          slen -= n;
        }
      else
        break;
    }

    return s;
}

/* Print N spaces (used in debug for target-depth).  */

void
print_spaces (unsigned int n)
{
  while (n-- > 0)
    putchar (' ');
}


/* Return a string whose contents concatenate the NUM strings provided
   This string lives in static, re-used memory.  */

const char *
concat (unsigned int num, ...)
{
  static size_t rlen = 0;
  static char *result = NULL;
  size_t ri = 0;
  va_list args;

  va_start (args, num);

  while (num-- > 0)
    {
      const char *s = va_arg (args, const char *);
      size_t l = xstrlen (s);

      if (l == 0)
        continue;

      if (ri + l > rlen)
        {
          rlen = ((rlen ? rlen : 60) + l) * 2;
          result = xrealloc (result, rlen);
        }

      memcpy (result + ri, s, l);
      ri += l;
    }

  va_end (args);

  /* Get some more memory if we don't have enough space for the
     terminating '\0'.   */
  if (ri == rlen)
    {
      rlen = (rlen ? rlen : 60) * 2;
      result = xrealloc (result, rlen);
    }

  result[ri] = '\0';

  return result;
}


#ifndef HAVE_UNISTD_H
pid_t getpid ();
#endif

pid_t make_pid ()
{
  return getpid ();
}

/* Like malloc but get fatal error if memory is exhausted.  */
/* Don't bother if we're using dmalloc; it provides these for us.  */

#ifndef HAVE_DMALLOC_H

#undef xmalloc
#undef xcalloc
#undef xrealloc
#undef xstrdup

void *
xmalloc (size_t size)
{
  /* Make sure we don't allocate 0, for pre-ISO implementations.  */
  void *result = malloc (size ? size : 1);
  if (result == 0)
    out_of_memory ();
  return result;
}


void *
xcalloc (size_t size)
{
  /* Make sure we don't allocate 0, for pre-ISO implementations.  */
  void *result = calloc (size ? size : 1, 1);
  if (result == 0)
    out_of_memory ();
  return result;
}


void *
xrealloc (void *ptr, size_t size)
{
  void *result;

  /* Some older implementations of realloc() don't conform to ISO.  */
  if (! size)
    size = 1;
  result = ptr ? realloc (ptr, size) : malloc (size);
  if (result == 0)
    out_of_memory ();
  return result;
}


char *
xstrdup (const char *ptr)
{
  char *result;

#ifdef HAVE_STRDUP
  result = strdup (ptr);
#else
  result = malloc (strlen (ptr) + 1);
#endif

  if (result == 0)
    out_of_memory ();

#ifdef HAVE_STRDUP
  return result;
#else
  return strcpy (result, ptr);
#endif
}

#endif  /* HAVE_DMALLOC_H */

char *
xstrndup (const char *str, size_t length)
{
  char *result;

#ifdef HAVE_STRNDUP
  result = strndup (str, length);
  if (result == 0)
    out_of_memory ();
#else
  result = xmalloc (length + 1);
  if (length > 0)
    strncpy (result, str, length);
  result[length] = '\0';
#endif

  return result;
}

#ifndef HAVE_MEMRCHR
void *
memrchr(const void* str, int ch, size_t len)
{
  const char* sp = str;
  const char* cp = sp;

  if (len == 0)
    return NULL;

  cp += len - 1;

  while (cp[0] != ch)
    {
      if (cp == sp)
        return NULL;
      --cp;
    }

  return (void*)cp;
}
#endif



/* Limited INDEX:
   Search through the string STRING, which ends at LIMIT, for the character C.
   Returns a pointer to the first occurrence, or nil if none is found.
   Like INDEX except that the string searched ends where specified
   instead of at the first null.  */

char *
lindex (const char *s, const char *limit, int c)
{
  while (s < limit)
    if (*s++ == c)
      return (char *)(s - 1);

  return 0;
}

/* Return the address of the first whitespace or null in the string S.  */

char *
end_of_token (const char *s)
{
  while (! END_OF_TOKEN (*s))
    ++s;
  return (char *)s;
}

/* Return the address of the first nonwhitespace or null in the string S.  */

char *
next_token (const char *s)
{
  NEXT_TOKEN (s);
  return (char *)s;
}

/* Find the next token in PTR; return the address of it, and store the length
   of the token into *LENGTHPTR if LENGTHPTR is not nil.  Set *PTR to the end
   of the token, so this function can be called repeatedly in a loop.  */

char *
find_next_token (const char **ptr, size_t *lengthptr)
{
  const char *p = next_token (*ptr);

  if (*p == '\0')
    return 0;

  *ptr = end_of_token (p);
  if (lengthptr != 0)
    *lengthptr = *ptr - p;

  return (char *)p;
}

/* Write a BUFFER of size LEN to file descriptor FD.
   Retry short writes from EINTR.  Return LEN, or -1 on error.  */
ssize_t
writebuf (int fd, const void *buffer, size_t len)
{
  const char *msg = buffer;
  size_t l = len;
  while (l)
    {
      ssize_t r;

      EINTRLOOP (r, write (fd, msg, l));
      if (r < 0)
        return r;

      l -= r;
      msg += r;
    }

  return (ssize_t)len;
}

/* Read until we get LEN bytes from file descriptor FD, into BUFFER.
   Retry short reads on EINTR.  If we get an error, return it.
   Return 0 at EOF.  */
ssize_t
readbuf (int fd, void *buffer, size_t len)
{
  char *msg = buffer;
  while (len)
    {
      ssize_t r;

      EINTRLOOP (r, read (fd, msg, len));
      if (r < 0)
        return r;
      if (r == 0)
        break;

      len -= r;
      msg += r;
    }

  return (ssize_t)(msg - (char*)buffer);
}


/* Copy a chain of 'struct dep'.  For 2nd expansion deps, dup the name.  */

struct dep *
copy_dep_chain (const struct dep *d)
{
  struct dep *firstnew = 0;
  struct dep *lastnew = 0;

  while (d != 0)
    {
      struct dep *c = xmalloc (sizeof (struct dep));
      memcpy (c, d, sizeof (struct dep));

      if (c->need_2nd_expansion)
        c->name = xstrdup (c->name);

      c->next = 0;
      if (firstnew == 0)
        firstnew = lastnew = c;
      else
        lastnew = lastnew->next = c;

      d = d->next;
    }

  return firstnew;
}

/* Free a chain of struct nameseq.
   For struct dep chains use free_dep_chain.  */

void
free_ns_chain (struct nameseq *ns)
{
  while (ns != 0)
    {
      struct nameseq *t = ns;
      ns = ns->next;
      free_ns (t);
    }
}


#ifdef MAKE_MAINTAINER_MODE

void
spin (const char* type)
{
  char filenm[256];
  struct stat dummy;

  sprintf (filenm, ".make-spin-%s", type);

  if (stat (filenm, &dummy) == 0)
    {
      fprintf (stderr, "SPIN on %s\n", filenm);
      do
#ifdef WINDOWS32
        Sleep (1000);
#else
        sleep (1);
#endif
      while (stat (filenm, &dummy) == 0);
    }
}

#endif



/* Provide support for temporary files.  */

#ifndef HAVE_STDLIB_H
# ifdef HAVE_MKSTEMP
int mkstemp (char *template);
# else
char *mktemp (char *template);
# endif
#endif

#ifndef HAVE_UMASK
mode_t
umask (mode_t mask)
{
  return 0;
}
#endif

static char *
get_tmptemplate ()
{
  const char *tmpdir;
  char *template;
  size_t len;

#ifdef VMS
# define DEFAULT_TMPFILE     "sys$scratch:gnv$make_cmdXXXXXX.com"
#else
# define DEFAULT_TMPFILE     "GmXXXXXX"
#endif

#ifdef VMS
# define DEFAULT_TMPDIR     "/sys$scratch/"
#else
# ifdef P_tmpdir
#  define DEFAULT_TMPDIR    P_tmpdir
# else
#  define DEFAULT_TMPDIR    "/tmp"
# endif
#endif

  if (
#if defined (__MSDOS__) || defined (WINDOWS32) || defined (__EMX__)
      ((tmpdir = getenv ("TMP")) == NULL || *tmpdir == '\0') &&
      ((tmpdir = getenv ("TEMP")) == NULL || *tmpdir == '\0') &&
#endif
      ((tmpdir = getenv ("TMPDIR")) == NULL || *tmpdir == '\0'))
    tmpdir = DEFAULT_TMPDIR;

  len = strlen (tmpdir);
  template = xmalloc (len + CSTRLEN (DEFAULT_TMPFILE) + 2);
  strcpy (template, tmpdir);

#ifdef HAVE_DOS_PATHS
  if (template[len - 1] != '/' && template[len - 1] != '\\')
    strcat (template, "/");
#else
# ifndef VMS
  if (template[len - 1] != '/')
    strcat (template, "/");
# endif /* !VMS */
#endif /* !HAVE_DOS_PATHS */

  strcat (template, DEFAULT_TMPFILE);

  return template;
}

char *
get_tmppath ()
{
  char *path;

#ifdef HAVE_MKTEMP
  path = get_tmptemplate();
  if (*mktemp (path) == '\0')
    pfatal_with_name ("mktemp");
#else
  path = xmalloc (L_tmpnam + 1);
  if (tmpnam (path) == NULL)
    pfatal_with_name ("tmpnam");
#endif

  return path;
}

FILE *
get_tmpfile (char **name)
{
  FILE *file;
#ifdef HAVE_FDOPEN
  int fd;
#endif

  /* Preserve the current umask, and set a restrictive one for temp files.  */
  mode_t mask = umask (0077);

#if defined(HAVE_MKSTEMP) && defined(HAVE_FDOPEN)
  *name = get_tmptemplate ();

  /* It's safest to use mkstemp(), if we can.  */
  EINTRLOOP (fd, mkstemp (*name));
  if (fd == -1)
    file = NULL;
  else
    file = fdopen (fd, "w");
#else
  *name = get_tmppath ();

# ifdef HAVE_FDOPEN
  /* Can't use mkstemp(), but try to guard against a race condition.  */
  EINTRLOOP (fd, open (*name, O_CREAT|O_EXCL|O_WRONLY, 0600));
  if (fd == -1)
    return 0;
  file = fdopen (fd, "w");
# else
  /* Not secure, but what can we do?  */
  file = fopen (*name, "w");
# endif
#endif

  umask (mask);

  return file;
}


#if !HAVE_STRCASECMP && !HAVE_STRICMP && !HAVE_STRCMPI
/* If we don't have strcasecmp() (from POSIX), or anything that can substitute
   for it, define our own version.  */

int
strcasecmp (const char *s1, const char *s2)
{
  while (1)
    {
      int c1 = (unsigned char) *(s1++);
      int c2 = (unsigned char) *(s2++);

      if (isalpha (c1))
        c1 = tolower (c1);
      if (isalpha (c2))
        c2 = tolower (c2);

      if (c1 != '\0' && c1 == c2)
        continue;

      return (c1 - c2);
    }
}
#endif

#if !HAVE_STRNCASECMP && !HAVE_STRNICMP && !HAVE_STRNCMPI
/* If we don't have strncasecmp() (from POSIX), or anything that can
   substitute for it, define our own version.  */

int
strncasecmp (const char *s1, const char *s2, int n)
{
  while (n-- > 0)
    {
      int c1 = (unsigned char) *(s1++);
      int c2 = (unsigned char) *(s2++);

      if (isalpha (c1))
        c1 = tolower (c1);
      if (isalpha (c2))
        c2 = tolower (c2);

      if (c1 != '\0' && c1 == c2)
        continue;

      return (c1 - c2);
    }

  return 0;
}
#endif


#ifdef NEED_GET_PATH_MAX
unsigned int
get_path_max (void)
{
  static unsigned int value;

  if (value == 0)
    {
      long int x = pathconf ("/", _PC_PATH_MAX);
      if (x > 0)
        value = x;
      else
        return MAXPATHLEN;
    }

  return value;
}
#endif

#if !HAVE_MEMPCPY
void *
mempcpy (void *dest, const void *src, size_t n)
{
  return (char *) memcpy (dest, src, n) + n;
}
#endif

#if !HAVE_STPCPY
char *
stpcpy (char *dest, const char *src)
{
  char *d = dest;
  const char *s = src;

  do
    *d++ = *s;
  while (*s++ != '\0');

  return d - 1;
}
#endif

#if !HAVE_STRTOLL
# undef UNSIGNED
# undef USE_NUMBER_GROUPING
# undef USE_WIDE_CHAR
# define QUAD 1
# include <strtol.c>
#endif

#if !HAVE_STRERROR
char *
strerror (int errnum)
{
  static char msg[256];

#define SETMSG(_e, _m) case _e: strcpy(msg, _m); break

  switch (errnum)
    {
#ifdef EPERM
    SETMSG (EPERM  , "Operation not permitted");
#endif
#ifdef ENOENT
    SETMSG (ENOENT , "No such file or directory");
#endif
#ifdef ESRCH
    SETMSG (ESRCH  , "No such process");
#endif
#ifdef EINTR
    SETMSG (EINTR  , "Interrupted system call");
#endif
#ifdef EIO
    SETMSG (EIO    , "I/O error");
#endif
#ifdef ENXIO
    SETMSG (ENXIO  , "No such device or address");
#endif
#ifdef E2BIG
    SETMSG (E2BIG  , "Argument list too long");
#endif
#ifdef ENOEXEC
    SETMSG (ENOEXEC, "Exec format error");
#endif
#ifdef EBADF
    SETMSG (EBADF  , "Bad file number");
#endif
#ifdef ECHILD
    SETMSG (ECHILD , "No child processes");
#endif
#ifdef EAGAIN
    SETMSG (EAGAIN , "Try again");
#endif
#ifdef ENOMEM
    SETMSG (ENOMEM , "Out of memory");
#endif
#ifdef EACCES
    SETMSG (EACCES , "Permission denied");
#endif
#ifdef EFAULT
    SETMSG (EFAULT , "Bad address");
#endif
#ifdef ENOTBLK
    SETMSG (ENOTBLK, "Block device required");
#endif
#ifdef EBUSY
    SETMSG (EBUSY  , "Device or resource busy");
#endif
#ifdef EEXIST
    SETMSG (EEXIST , "File exists");
#endif
#ifdef EXDEV
    SETMSG (EXDEV  , "Cross-device link");
#endif
#ifdef ENODEV
    SETMSG (ENODEV , "No such device");
#endif
#ifdef ENOTDIR
    SETMSG (ENOTDIR, "Not a directory");
#endif
#ifdef EISDIR
    SETMSG (EISDIR , "Is a directory");
#endif
#ifdef EINVAL
    SETMSG (EINVAL , "Invalid argument");
#endif
#ifdef ENFILE
    SETMSG (ENFILE , "File table overflow");
#endif
#ifdef EMFILE
    SETMSG (EMFILE , "Too many open files");
#endif
#ifdef ENOTTY
    SETMSG (ENOTTY , "Not a typewriter");
#endif
#ifdef ETXTBSY
    SETMSG (ETXTBSY, "Text file busy");
#endif
#ifdef EFBIG
    SETMSG (EFBIG  , "File too large");
#endif
#ifdef ENOSPC
    SETMSG (ENOSPC , "No space left on device");
#endif
#ifdef ESPIPE
    SETMSG (ESPIPE , "Illegal seek");
#endif
#ifdef EROFS
    SETMSG (EROFS  , "Read-only file system");
#endif
#ifdef EMLINK
    SETMSG (EMLINK , "Too many links");
#endif
#ifdef EPIPE
    SETMSG (EPIPE  , "Broken pipe");
#endif
#ifdef EDOM
    SETMSG (EDOM   , "Math argument out of domain of func");
#endif
#ifdef ERANGE
    SETMSG (ERANGE , "Math result not representable");
#endif
    default: sprintf (msg, "Unknown error %d", errnum); break;
    }

  return msg;
}
#endif
