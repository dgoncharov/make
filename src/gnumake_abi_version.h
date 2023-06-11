/* Versioning of GNU Make abi.
Copyright (C) 2023 Free Software Foundation, Inc.
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

#ifdef GNUMAKE_ABI_VERSION_INCLUDED
#error gnumake_abi_version.h can only be included once.
#endif
#define GNUMAKE_ABI_VERSION_INCLUDED

#include "gnumake.h"

#define GMK_STR(s) #s
#define GMK_STRINGIFY(s) GMK_STR(s)
const char gmk_needed_abi_version__[] =
    GMK_STRINGIFY (GMK_ABI_VERSION) "." GMK_STRINGIFY (GMK_ABI_AGE);
#undef GMK_STRINGIFY
#undef GMK_STR
