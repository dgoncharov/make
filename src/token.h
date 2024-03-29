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

#ifndef _TOKEN_H_
#define _TOKEN_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Find, validate and dequote the first token.

   Tokenizing rules.

   A not escaped and not quoted space, tab or newline character serves as a
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
   E.g. hello'world'of'many'tokens is treated as one token.

   Dequoting rules.

   Outside of quotes
     Remove each not escaped backslash.
     Remove each backslash that escapes a backslash.
     Replace one or multiple consecutive backslash-newline pairs with a single
     space.

   Within double quotes
     Remove opening and closing double quotes.
     Remove each backslash escaping a backslash or double quote.
     Replace one or multiple consecutive backslash-newline pairs with a single
     space.

   Within single quotes.
     Remove opening and closing single quotes.

   If no token is found,
     Keep '*status' intact.
     Point '*s' to the null terminator.
     Set '*tokenlen' to 0.
     Return 0.

   Otherwise, if a not escaped opening quote is present in the first token, but
   a corresponding closing quote is missing, then,
     Set '*status' to 1.
     Point '*s' to the null terminator.
     Set '*tokenlen' to the length of first token.
     Return the beginning of the first token.

   Otherwise,
     Keep '*status' intact.
     Point '*s' to the next token.
     Dequote S in place.
     Set '*tokenlen' to the length of the dequoted first token.
     Return the beginning of the first token.  */
char *next_dequoted_token (char **s, size_t *tokenlen, int *status);

#ifdef __cplusplus
}
#endif
#endif
