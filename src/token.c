/* Utilities to tokenize a C string.
Copyright (C) 2024 Free Software Foundation, Inc.
Written by Dmitry Goncharov <dgoncharov@users.sf.net>.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include "token.h"
#include <string.h>
#include <stddef.h>
#include <assert.h>

static const char quotes_and_separators[] = "'\" \t\n";
static const char *separators = quotes_and_separators + 2;
static const char *next_token (const char **s, size_t *tokenlen, int *status);
static char *dequote (char *s, size_t *slen);
static char *quote_removal_in_double_quotes (char *s, size_t *slen);
static char *quote_removal_in_single_quotes (char *s, size_t *slen);
static int whitespace (char c);
static const char *skip_separators (const char *s);
static const char *skip_until_separator (const char *s);
static size_t strecspn (const char *s, const char *reject);
static size_t memecspn (const char *s, const char *reject, size_t slen);
static char *collapse_escaped_newlines (char *s, char *beg, const char **endp);


/* 's' is mutable, because next_dequoted_token dequotes the token in place.
  An alternative interface would be to have next_dequoted_token allocate the
  buffer on heap and return it.  That would require a malloc per token and
  cause the caller to free each token.
  Yet another interface would be to have next_dequoted_token take a buffer
  from the caller, copy a token to the provided buffer and modify the token in
  the buffer. Such interface requires next_dequoted_token to perform a memcpy
  per token.  Also, such interface requires that the caller allocates a buffer
  of sufficient size.  Since the caller does not know the length of a token,
  the caller would have to allocate a buffer sufficient to hold the whole
  input string. If a buffer sufficient to hold the whole string is allocated
  anyway the caller can as well copy the input string to the buffer to relieve
  next_dequoted_token from doing memcpy per token.
  The current interface, on the other hand, does not force either malloc/free
  or memcpy per token.
  Another nice property of this mutable 's' is that it serves as a storage for
  all dequoted tokens. This relieves the caller from keeping each token
  somewhere individually.
  For example, to populate an argv, the caller can copy the address of each
  token to the related argv element.  The tokens themself stay in 's'.

  while ((token = next_dequoted_token (&s, &len, &malformed)))
    argv[argc++] = token;
  argv[argc] = 0;  */
char *
next_dequoted_token (char **s, size_t *tokenlen, int *status)
{
  int st = 0;
  char *token = (char*) next_token ((const char **)s, tokenlen, &st);
  *status |= st;
  if (token && st == 0)
    dequote (token, tokenlen);
  return token;
}

/* A not escaped and not quoted space, tab or newline character serves as a
   token separator.
   A not escaped backslash serves as an escape character.

   In the absence of quotes, a token is delimited by token separators.
   Beginning of a quoted token is delimited by a pair of a token separator
   followed, immediately or not, by a not escaped single or double quote. The
   end of a quoted token is delimited by the same single or not escaped double
   quote followed, immediately or not, by a token separator.  A space, tab or
   newline inside the quoted token does not serve as a token separator.
   Multiple adjacent quoted tokens, in other words, quoted tokens without
   interleaving separators, are treated as one token.
   E.g. hello'world'of'many'tokens is treated as one token.  */

/* Return the beginning of the first token in the specified string 's'.
   Set '*tokenlen' to the length of the token.  The quotes that delimit a
   quoted token are themself a part of the token and included to '*tokenlen'.
   Set '*s' to the beginning of the next token.
   If an opening quote is present and the closing quote is missing, set
   '*status' to 1.  Otherwise, keep '*status' intact.
   If no token is found, return 0.

   'status' is a bitmask.
   The current implementation only sets bit 0. However, it is possible to
   modify next_token to detect $ or ` and set other bits in 'status'.  */
static const char *
next_token (const char **input, size_t *tokenlen, int *status)
{
  const char *token, *s;

  assert (*input);

  *tokenlen = 0;
  *input = skip_separators (*input);
  if (**input == '\0')
    /* Only separators in this input.  */
    return 0;

  /* Point 'token' at the beginning of the token.  */
  token = s = *input;

  /* An escaped newline is not treated like some other escaped space.
     An escaped newline is substituted with a space during quote removal phase,
     which serves as a token separator.  */

  for (;;)
    {
      /* Skip until a separator or quote.  */
      s = skip_until_separator (s);
      if (*s == '\0')
        /* Found the end of the token.  */
        break;

      if (*s == '\\' && s[1] == '\n')
        break;

      if (*s == '\'')
        {
          /* Advance past the opening quote.  */
          ++s;
          /* Skip until the next single quote, which is the closing quote.  */
          s += strcspn (s, "'");
          if (*s == '\0')
            {
              /* Closing quote is missing.  */
              *status |= 1;
              break;
            }
          assert (*s == '\'');
          /* Advance past the closing quote.  */
          ++s;
          if (whitespace (*s))
            /* Found the end of the token.  */
            break;
          continue;
        }

      if (*s == '"')
        {
          /* Advance past the opening quote.  */
          ++s;
          /* Skip until a not escaped double quote, which is the closing quote.
           */
          s += strecspn (s, "\"");
          if (*s == '\0')
            {
              /* Closing quote is missing.  */
              *status |= 1;
              break;
            }
          assert (*s == '"');
          /* Advance past the closing quote.  */
          ++s;
          if (whitespace (*s))
            /* Found the end of the token.  */
            break;
          continue;
        }

      assert (strchr (separators, *s));
      /* Found the end of the token.  */
      break;
    }

  /* 'token' points at the begininnig of the token. 's' points immediately
     after the end of the token.  */
  *tokenlen = s - token;

  /* Move 's' to the beginning of the next token.  */
  s = skip_separators (s);
  *input = s;

  return token;
}

/* Dequote the initial portion of '*slen' characters of string 's'.
   Store the lenght of the dequoted portion to '*slen'.
   Null terminate the dequoted portion.
   Return 's'.  */
static char *
dequote (char *s, size_t *slen)
{
  char *beg = s;
  const char *end = s + *slen;
  size_t len = *slen;

  s += memecspn (s, "\\'\"", *slen);
  for (s = beg; s < end; )
    {
      len = end - s;
      switch (*s)
        {
        case '\'':
          s = quote_removal_in_single_quotes (s, &len);
          end = s + len;
          break;
        case '"':
          s = quote_removal_in_double_quotes (s, &len);
          end = s + len;
          break;
        case '\\':
          if (s[1] == '\n')
            {
              /* This backslash escapes the newline in 's[1]'.
                 Remove the backslash and the newline.  */
              memmove (s, s + 2, len - 2);
              end -= 2;
              break;
            }
          if (strchr ("'\"\\ \t", s[1]))
            {
              /* This backslash escapes 's[1]'.
                 Remove the backslash.  */
              memmove (s, s + 1, len - 1);
              --end;

              /* Keep intact the escaped character.  */
              ++s;
              break;
            }

          /* This is a lone backslash that does not escape anything
             interesting. Remove the backslash.  */
          memmove (s, s + 1, len - 1);
          --end;
          break;
        default:
          /* Keep intact a regular character.  */
          ++s;
          break;
        }
    }

  assert (beg <= end);
  *slen = end - beg;
  beg[*slen] = '\0';

  return beg;
}

/* Within substring '[s, s + slen)' remove the opening and closing single
   quotes.

  Store the number of characters between the end of the
  token and the end of the substring, remaining after quote removal.
  Return the end of the token.  */
static char *
quote_removal_in_single_quotes (char *s, size_t *slen)
{
  const char *end = s + *slen;
  char *close;

  /* These assertions are corrects, because next_dequoted_token calls dequote
     only when the token is well formed.  */
  assert (*slen > 1);
  assert (*s == '\'');

  close = (char*)  memchr (s + 1, '\'', *slen - 1);
  assert (close);
  assert (close > s);

  /* Remove closing single quote.  */
  memmove (close, close + 1, end - close);
  --end;

  /* Remove opening single quote.  */
  memmove (s, s + 1, end - s);
  --end;
  --close;
  *slen = end - close;

  return close;
}

/* Within substring '[s, s + slen)'
   Remove the opening and closing double quotes.
   Remove each backslash which escapes a double quote or backslash.
   Replace each group of consecutive backslash-newline pairs along with
   surrounding space with a single space.

   Store the number of characters between the end of the token and the end of
   the substring, remaining after quote removal.
   Return the end of the token.

   Because '[s, s + slen)' is one token, there is no need to care about single
   quotes.  */
static char *
quote_removal_in_double_quotes (char *s, size_t *slen)
{
  const char qe[] = {'\\', '"', '\0'};
  char *beg = s;
  const char *end = s + *slen;

  /* These assertions are corrects, because next_dequoted_token calls dequote
     only when the token is well formed.  */
  assert (*slen > 1);
  assert (*s == '"');

  /* Remove the opening quote.  */
  memmove (s, s + 1, end - s);
  --end;

  for (;;)
    {
      s += memecspn (s, qe, end - s);
      if (s >= end)
        break;

      if (*s == '"')
        {
          /* Remove the closing quote.  */
          memmove (s, s + 1, end - s);
          --end;
          /* A not escaped double quote is the end of the double quoted
             substring.  */
          break;
        }

      assert (*s == '\\');

      if (s + 1 >= end)
        /* There is nothing to escape.  */
        break;

      if (s[1] == '\\' || s[1] == '"')
        {
          /* A backslash is escaping either a backslash or a quote.  */

          /* Remove the backslash.  */
          memmove (s, s + 1, end - s);
          --end;

          /* Keep intact the escaped character.  */
          ++s;

          continue;
        }

      if (s[1] == '\n')
        {
          s = collapse_escaped_newlines (s, beg, &end);
          continue;
        }

        /* Keep intact a lone backslash.  */
        ++s;
    }

  *slen = end - s;

  return s;
}

/* Return 1 if 'c' is a newline, space or tab.
   Various shells, posix and GNU Make do not honor form-feed, carriage return
   or vertical tab as token delimiters, even though isspace returns true for
   those characters.  */
static int
whitespace (char c)
{
    return c == '\n' || c == ' ' || c == '\t';
}

/* Skip separators and escaped newlines.  */
static const char *
skip_separators (const char *s)
{
  const char *beg;
  for (beg = 0; beg != s; )
    {
      beg = s;
      /* Skip separators.  */
      s += strspn (s, separators);

      /* Skip an escaped newline.  */
      if (*s == '\\' && s[1] == '\n')
        s += 2;
    }
  return s;
}

/* Skip until a separator or a newline or a not escaped newline.  */
static const char *
skip_until_separator (const char *s)
{
  const char escape = '\\';
  int n;

  for (n = 0; *s; ++s)
    {
      /* A backslash followed by newline is replaced with a space and thus
         serves as a separator. Therefore, any newline, escaped or not is a
         separator.  */
      if (*s == '\n')
        {
          s -= n % 2;
          break;
        }
      if (*s == escape)
        {
          ++n;
          continue;
        }
      if ((n % 2) == 0 && strchr (quotes_and_separators, (unsigned char) *s))
        break;
      n = 0;
    }

  return s;
}

/* Return the index of the first character in 's' that is present in 'reject'
   and not escaped with a backslash.
   If backslash itself is present in 'reject', then nothing can be escaped,
   because lookup returns the index of that very backslash and strecspn behaves
   as strcspn.  */
static size_t
strecspn (const char *s, const char *reject)
{
  size_t result;
  int n = 0;
  const char escape = '\\';
  const char *escape_rejected = strchr (reject, (unsigned char) escape);

  for (result = 0; *s; ++s, ++result)
    {
      if (*s == escape)
        {
          ++n;
          if (escape_rejected)
            break;
          else
            continue;
        }

      if ((n % 2) == 0 && strchr (reject, (unsigned char) *s))
        break;

      n = 0;
    }

  return result;
}

/* Same as strecspn within the first 'slen' characters of 's'.  */
static size_t
memecspn (const char *s, const char *reject, size_t slen)
{
  size_t result;
  int n = 0;
  const char escape = '\\';
  /* memchr along with 'rlen', rather than strchr, is used to ensure
     that a \0 in '[s, s+slen)' does not match the null terminator in 'reject'.
   */
  size_t rlen = strlen (reject);
  const char *escape_rejected =
                         (char*) memchr (reject, (unsigned char) escape, rlen);

  for (result = 0; slen && *s; ++s, ++result, --slen)
    {
      if (*s == escape)
        {
          ++n;
          if (escape_rejected)
            break;
          else
            continue;
        }

      if ((n % 2) == 0 && memchr (reject, (unsigned char) *s, rlen))
        break;

      n = 0;
    }

  return result;
}

/* Within substring '[beg, *endp)' replace all consecutive backslash-newline
   pairs along with surrounding space with a single space.
   Return the address immediately after the newly inserted space.
   Store the new end of the substring in '*endp'.  */
static char *
collapse_escaped_newlines (char *s, char *beg, const char **endp)
{
  char *p;
  const char *end = *endp;

  assert (beg < end);
  assert (beg <= s);
  assert (s < end);
  assert (*s == '\\');
  assert (s[1] == '\n');

  /* Convert all consecutive backslash-newline pairs along with
     surrounding space to a single space.  */

  /* Walk back optional leading space.  */
  for (p = s - 1; beg < p && (*p == '\t' || *p == ' '); --p)
    ;
  ++p;

  /* Walk forward optional trailing space along with more
     backslash-newline pairs.  */
  while (s < end && ((*s == '\t' || *s == ' ') || (*s == '\\' && s[1] == '\n')))
    {
      for (; s < end && (*s == '\t' || *s == ' '); ++s)
        ;
      for (; s < end && (*s == '\\' && s[1] == '\n'); s += 2)
        ;
    }

  /* 'p' points at the beginning of the leading space.
     's' points immediately after trailing space and all consecutive
     backslash-newline pairs.
     Replace '[p, s)' with a single space.  */
  *p = ' ';
  ++p;
  memmove (p, s, end - s);
  *endp -= s - p;

  return p;
}
