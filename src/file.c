/* Target file management for GNU Make.
Copyright (C) 1988-2024 Free Software Foundation, Inc.
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

#include "makeint.h"

#include <assert.h>

#include "filedef.h"
#include "dep.h"
#include "job.h"
#include "commands.h"
#include "variable.h"
#include "debug.h"
#include "hash.h"
#include "shuffle.h"
#include "rule.h"

static struct dep **
second_expand_pattern_dep (struct file *f, struct dep **dp, const char *name);
static struct dep **
second_expand_dep (struct file *f, struct dep **dp, struct dep *d,
                   const char *name, int order_only, const char *dirname);
static const char *
substitute_stem (char *buf, const char *input, size_t len, const char *dirname);

/* Remember whether snap_deps has been invoked: we need this to be sure we
   don't add new rules (via $(eval ...)) afterwards.  In the future it would
   be nice to support this, but it means we'd need to re-run snap_deps() or
   at least its functionality... it might mean changing snap_deps() to be run
   per-file, so we can invoke it after the eval... or remembering which files
   in the hash have been snapped (a new boolean flag?) and having snap_deps()
   only work on files which have not yet been snapped. */
int snapped_deps = 0;

/* Hash table of files the makefile knows how to make.  */

static unsigned long
file_hash_1 (const void *key)
{
  return_ISTRING_HASH_1 (((struct file const *) key)->hname);
}

static unsigned long
file_hash_2 (const void *key)
{
  return_ISTRING_HASH_2 (((struct file const *) key)->hname);
}

static int
file_hash_cmp (const void *x, const void *y)
{
  return_ISTRING_COMPARE (((struct file const *) x)->hname,
                          ((struct file const *) y)->hname);
}

static struct hash_table files;

/* We can't free files we take out of the hash table, because they are still
   likely pointed to in various places.  The check_renamed() will be used if
   we come across these, to find the new correct file.  This is mainly to
   prevent leak checkers from complaining.  */
static struct file **rehashed_files = NULL;
static size_t rehashed_files_len = 0;
#define REHASHED_FILES_INCR 5

/* Whether or not .SECONDARY with no prerequisites was given.  */
static int all_secondary = 0;

/* Access the hash table of all file records.
   lookup_file  given a name, return the struct file * for that name,
                or nil if there is none.
*/

struct file *
lookup_file (const char *name)
{
  struct file *f;
  struct file file_key;
#if MK_OS_VMS
  int want_vmsify;
#ifndef WANT_CASE_SENSITIVE_TARGETS
  char *lname;
#endif
#endif

  assert (*name != '\0');

  /* This is also done in parse_file_seq, so this is redundant
     for names read from makefiles.  It is here for names passed
     on the command line.  */
#if MK_OS_VMS
   want_vmsify = (strpbrk (name, "]>:^") != NULL);
# ifndef WANT_CASE_SENSITIVE_TARGETS
  if (*name != '.')
    {
      const char *n;
      char *ln;
      lname = xstrdup (name);
      for (n = name, ln = lname; *n != '\0'; ++n, ++ln)
        *ln = isupper ((unsigned char)*n) ? tolower ((unsigned char)*n) : *n;
      *ln = '\0';
      name = lname;
    }
# endif

  while (name[0] == '[' && name[1] == ']' && name[2] != '\0')
      name += 2;
  while (name[0] == '<' && name[1] == '>' && name[2] != '\0')
      name += 2;
#endif
  while (name[0] == '.' && ISDIRSEP (name[1]) && name[2] != '\0')
    {
      name += 2;
      while (ISDIRSEP (*name))
        /* Skip following slashes: ".//foo" is "foo", not "/foo".  */
        ++name;
    }

  if (*name == '\0')
    {
      /* It was all slashes after a dot.  */
      name = "./";
#if MK_OS_VMS
      /* TODO - This section is probably not needed. */
      if (want_vmsify)
        name = "[]";
#endif
    }
  file_key.hname = name;
  f = hash_find_item (&files, &file_key);
#if MK_OS_VMS && !defined(WANT_CASE_SENSITIVE_TARGETS)
  if (*name != '.')
    free (lname);
#endif

  return f;
}

/* Look up a file record for file NAME and return it.
   Create a new record if one doesn't exist.  NAME will be stored in the
   new record so it should be constant or in the strcache etc.
 */

struct file *
enter_file (const char *name)
{
  struct file *f;
  struct file *new;
  struct file **file_slot;
  struct file file_key;

  assert (*name != '\0');
  assert (! verify_flag || strcache_iscached (name));

#if MK_OS_VMS && !defined(WANT_CASE_SENSITIVE_TARGETS)
  if (*name != '.')
    {
      const char *n;
      char *lname, *ln;
      lname = xstrdup (name);
      for (n = name, ln = lname; *n != '\0'; ++n, ++ln)
        if (isupper ((unsigned char)*n))
          *ln = tolower ((unsigned char)*n);
        else
          *ln = *n;

      *ln = '\0';
      name = strcache_add (lname);
      free (lname);
    }
#endif

  file_key.hname = name;
  file_slot = (struct file **) hash_find_slot (&files, &file_key);
  f = *file_slot;
  if (! HASH_VACANT (f) && !f->double_colon)
    {
      f->builtin = 0;
      return f;
    }

  new = xcalloc (sizeof (struct file));
  new->name = new->hname = name;
  new->update_status = us_none;

  if (HASH_VACANT (f))
    {
      new->last = new;
      hash_insert_at (&files, new, file_slot);
    }
  else
    {
      /* There is already a double-colon entry for this file.  */
      new->double_colon = f;
      f->last->prev = new;
      f->last = new;
    }

  return new;
}

/* Rehash FILE to NAME.  This is not as simple as resetting
   the 'hname' member, since it must be put in a new hash bucket,
   and possibly merged with an existing file called NAME.  */

void
rehash_file (struct file *from_file, const char *to_hname)
{
  struct file file_key;
  struct file **file_slot;
  struct file *to_file;
  struct file *deleted_file;
  struct file *f;

  /* If it's already that name, we're done.  */
  from_file->builtin = 0;
  file_key.hname = to_hname;
  if (! file_hash_cmp (from_file, &file_key))
    return;

  /* Find the end of the renamed list for the "from" file.  */
  file_key.hname = from_file->hname;
  check_renamed (from_file);
  if (file_hash_cmp (from_file, &file_key))
    /* hname changed unexpectedly!! */
    abort ();

  /* Remove the "from" file from the hash.  */
  deleted_file = hash_delete (&files, from_file);
  if (deleted_file != from_file)
    /* from_file isn't the one stored in files */
    abort ();

  /* Find where the newly renamed file will go in the hash.  */
  file_key.hname = to_hname;
  file_slot = (struct file **) hash_find_slot (&files, &file_key);
  to_file = *file_slot;

  /* Change the hash name for this file.  */
  from_file->hname = to_hname;
  for (f = from_file->double_colon; f != 0; f = f->prev)
    f->hname = to_hname;

  /* If the new name doesn't exist yet just set it to the renamed file.  */
  if (HASH_VACANT (to_file))
    {
      hash_insert_at (&files, from_file, file_slot);
      return;
    }

  /* TO_FILE already exists under TO_HNAME.
     We must retain TO_FILE and merge FROM_FILE into it.  */

  if (from_file->cmds != 0)
    {
      if (to_file->cmds == 0)
        to_file->cmds = from_file->cmds;
      else if (from_file->cmds != to_file->cmds)
        {
          size_t l = strlen (from_file->name);
          /* We have two sets of commands.  We will go with the
             one given in the rule found through directory search,
             but give a message to let the user know what's going on.  */
          if (to_file->cmds->fileinfo.filenm != 0)
            error (&from_file->cmds->fileinfo,
                   l + strlen (to_file->cmds->fileinfo.filenm) + INTSTR_LENGTH,
                   _("recipe was specified for file '%s' at %s:%lu,"),
                   from_file->name, from_file->cmds->fileinfo.filenm,
                   from_file->cmds->fileinfo.lineno);
          else
            error (&from_file->cmds->fileinfo, l,
                   _("recipe for file '%s' was found by implicit rule search,"),
                   from_file->name);
          l += strlen (to_hname);
          error (&from_file->cmds->fileinfo, l,
                 _("but '%s' is now considered the same file as '%s'"),
                 from_file->name, to_hname);
          error (&from_file->cmds->fileinfo, l,
                 _("recipe for '%s' will be ignored in favor of the one for '%s'"),
                 from_file->name, to_hname);
        }
    }

  /* Merge the dependencies of the two files.  */

  if (to_file->deps == 0)
    to_file->deps = from_file->deps;
  else
    {
      struct dep *deps = to_file->deps;
      while (deps->next != 0)
        deps = deps->next;
      deps->next = from_file->deps;
    }

  merge_variable_set_lists (&to_file->variables, from_file->variables);

  if (to_file->double_colon && from_file->is_target && !from_file->double_colon)
    OSS (fatal, NILF, _("can't rename single-colon '%s' to double-colon '%s'"),
         from_file->name, to_hname);
  if (!to_file->double_colon  && from_file->double_colon)
    {
      if (to_file->is_target)
        OSS (fatal, NILF,
             _("can't rename double-colon '%s' to single-colon '%s'"),
             from_file->name, to_hname);
      else
        to_file->double_colon = from_file->double_colon;
    }

  if (from_file->last_mtime > to_file->last_mtime)
    /* %%% Kludge so -W wins on a file that gets vpathized.  */
    to_file->last_mtime = from_file->last_mtime;

  to_file->mtime_before_update = from_file->mtime_before_update;

#define MERGE(field) to_file->field |= from_file->field
  MERGE (precious);
  MERGE (loaded);
  MERGE (tried_implicit);
  MERGE (updating);
  MERGE (updated);
  MERGE (is_target);
  MERGE (cmd_target);
  MERGE (phony);
  /* Don't merge intermediate because this file might be pre-existing */
  MERGE (is_explicit);
  MERGE (secondary);
  MERGE (notintermediate);
  MERGE (ignore_vpath);
  MERGE (snapped);
  MERGE (suffix);
#undef MERGE

  to_file->builtin = 0;
  from_file->renamed = to_file;

  if (rehashed_files_len % REHASHED_FILES_INCR == 0)
    rehashed_files = xrealloc (rehashed_files,
                               sizeof (struct file *) * (rehashed_files_len + REHASHED_FILES_INCR));

  rehashed_files[rehashed_files_len++] = from_file;
}

/* Rename FILE to NAME.  This is not as simple as resetting
   the 'name' member, since it must be put in a new hash bucket,
   and possibly merged with an existing file called NAME.  */

void
rename_file (struct file *from_file, const char *to_hname)
{
  rehash_file (from_file, to_hname);
  while (from_file)
    {
      from_file->name = from_file->hname;
      from_file = from_file->prev;
    }
}

/* Remove all nonprecious intermediate files.
   If SIG is nonzero, this was caused by a fatal signal,
   meaning that a different message will be printed, and
   the message will go to stderr rather than stdout.  */

void
remove_intermediates (int sig)
{
  struct file **file_slot;
  struct file **file_end;
  int doneany = 0;

  /* If there's no way we will ever remove anything anyway, punt early.  */
  if (question_flag || touch_flag || all_secondary || no_intermediates)
    return;

  if (sig && just_print_flag)
    return;

  file_slot = (struct file **) files.ht_vec;
  file_end = file_slot + files.ht_size;
  for ( ; file_slot < file_end; file_slot++)
    if (! HASH_VACANT (*file_slot))
      {
        struct file *f = *file_slot;
        /* Is this file eligible for automatic deletion?
           Yes, IFF: it's marked intermediate, it's not secondary, it wasn't
           given on the command line, and it's either a -include makefile or
           it's not precious.  */
        if (f->intermediate && (f->dontcare || !f->precious)
            && !f->secondary && !f->notintermediate && !f->cmd_target)
          {
            int status;
            if (f->update_status == us_none)
              /* If nothing would have created this file yet,
                 don't print an "rm" command for it.  */
              continue;
            if (just_print_flag)
              status = 0;
            else
              {
                status = unlink (f->name);
                if (status < 0 && errno == ENOENT)
                  continue;
              }
            if (!f->dontcare)
              {
                if (sig)
                  OS (error, NILF,
                      _("*** deleting intermediate file '%s'"), f->name);
                else
                  {
                    if (! doneany)
                      DB (DB_BASIC, (_("Removing intermediate files...\n")));
                    if (!run_silent)
                      {
                        if (! doneany)
                          {
                            fputs ("rm ", stdout);
                            doneany = 1;
                          }
                        else
                          putchar (' ');
                        fputs (f->name, stdout);
                        fflush (stdout);
                      }
                  }
                if (status < 0)
                  {
                    perror_with_name ("\nunlink: ", f->name);
                    /* Start printing over.  */
                    doneany = 0;
                  }
              }
          }
      }

  if (doneany && !sig)
    {
      putchar ('\n');
      fflush (stdout);
    }
}

/* Given a string containing prerequisites (fully expanded), break it up into
   a struct dep list.  Enter each of these prereqs into the file database.
 */
struct dep *
split_prereqs (char *p, const char *dirname)
{
  struct dep *new = PARSE_FILE_SEQ (&p, struct dep, MAP_PIPE, dirname,
                                    PARSEFS_WAIT);

  if (*p)
    {
      /* Files that follow '|' are "order-only" prerequisites that satisfy the
         dependency by existing: their modification times are irrelevant.  */
      struct dep *ood;

      ++p;
      ood = PARSE_FILE_SEQ (&p, struct dep, MAP_NUL, dirname, PARSEFS_WAIT);

      if (! new)
        new = ood;
      else
        {
          struct dep *dp;
          for (dp = new; dp->next != NULL; dp = dp->next)
            ;
          dp->next = ood;
        }

      for (; ood != NULL; ood = ood->next)
        ood->ignore_mtime = 1;
    }

  return new;
}

/* Given a list of prerequisites, enter them into the file database.
   If STEM is set then first expand patterns using STEM.  */
struct dep *
enter_prereqs (struct dep *deps, struct file *file)
{
  struct dep *d1;


//if (file)
//printf("entering file %s, f->stem = %s, file->deps = %p, deps = %p\n", file->name, file->stem, file->deps, deps);

  if (deps == 0)
    return 0;

  /* If we have a stem, expand the %'s.  We use patsubst_expand to translate
     the prerequisites' patterns into plain prerequisite names.  */
  if (deps && deps->stem)
    {
      const char *pattern = "%";
      struct dep *dp = deps, *dl = 0;

      while (dp != 0)
        {
          char *percent;
          size_t nl = strlen (dp->name) + 1;
          char *buf, *nm;
          size_t dlen;

//printf("old dep name %s, dp->stem = %p\n", dp->name, dp->stem);

          assert (dp->stem);
          assert (dp->stem_basename >= dp->stem);
          assert (dp->stem_basename <= dp->stem + strlen (dp->stem));
          dlen = dp->stem_basename - dp->stem;
          nm = buf = alloca (nl + dlen);

          buf = mempcpy (buf, dp->stem_dirname, dlen);
          memcpy (buf, dp->name, nl);
          percent = find_percent (nm);
//printf("nm = %s, dp->stem_basename = %s, percent = %s\n", nm, dp->stem_basename, percent);
          if (percent)
            {
              char *o;
              /* We have to handle empty stems specially, because that
                 would be equivalent to $(patsubst %,dp->name,) which
                 will always be empty.  */
              if (dp->stem_basename[0] == '\0')
                {
//printf("file %s, dep name with prepended dir %s, dep %s, stem_dirname %s, stem_basename %s\n", file->name, nm, dp->name, dp->stem_dirname, dp->stem_basename);
                  memmove (percent, percent+1, strlen (percent));
                  o = variable_buffer_output (variable_buffer, nm,
                                              strlen (nm) + 1);
                }
              else
                o = patsubst_expand_pat (variable_buffer, dp->stem_basename,
                                         pattern, nm, pattern+1, percent+1);

              /* If the name expanded to the empty string, ignore it.  */
              if (variable_buffer[0] == '\0')
                {
                  struct dep *df = dp;
                  if (dp == deps)
                    dp = deps = deps->next;
                  else
                    dp = dl->next = dp->next;
                  free_dep (df);
                  continue;
                }
//printf("adding new dep name %.*s\n", (int) (o - variable_buffer), variable_buffer);
              /* Save the name.  */
              dp->name = strcache_add_len (variable_buffer,
                                           o - variable_buffer);
//printf("new dep name %s, dp->stem = %s\n", dp->name, dp->stem);
            }
//          dp->stem = stem;
//printf("new dep stem %s\n", dp->stem);
          dp->staticpattern = 1;
          dl = dp;
          dp = dp->next;
        }
    }

  /* Enter them as files, unless they need a 2nd expansion.  */
  for (d1 = deps; d1 != 0; d1 = d1->next)
    {
      if (d1->need_2nd_expansion)
        continue;

      d1->file = lookup_file (d1->name);
      if (d1->file == 0)
        d1->file = enter_file (d1->name);
      d1->staticpattern = 0;
      d1->name = 0;
      if (!file || !file->stem)
        /* This file is explicitly mentioned as a prereq.  */
        d1->file->is_explicit = 1;
    }

  return deps;
}

/* Expand and parse each dependency line.
   For each dependency of the file, make the 'struct dep' point
   at the appropriate 'struct file' (which may have to be created).  */
void
expand_deps (struct file *f)
{
  struct dep *d, *next;
  struct dep **dp;
  int initialized = 0;
  int changed_dep = 0;

  if (f->snapped)
    return;
  f->snapped = 1;

//for (d = f->deps; d; d = d->next) {
//printf("initial d %s at %p\n", d->name, d);
//}

  /* Walk through the dependencies.  For any dependency that needs 2nd
     expansion, expand it then insert the result into the list.  */
  dp = &f->deps;
//printf("f->deps at %p, dp = %p\n", f->deps, dp);
  d = f->deps;
  while (d != 0)
    {
      if (! d->name || ! d->need_2nd_expansion)
        {
          /* This one is all set already.  */
          dp = &d->next;
          d = d->next;
          continue;
        }

      changed_dep = 1;

      /* We're going to do second expansion so initialize file variables for
         the file. Since the stem for static pattern rules comes from
         individual dep lines, we will temporarily set f->stem to d->stem.  */
      if (!initialized)
        {
          initialize_file_variables (f, 0);
          initialized = 1;
        }

      /* If it's from a static pattern rule, convert the initial pattern in
         each word to "$*" so they'll expand properly.  */
//printf("before dp at %p, *dp at %p\n", dp, *dp);
      next = d->next;
      if (d->staticpattern)
        dp = second_expand_pattern_dep (f, dp, d->name);
      else
        dp = second_expand_dep (f, dp, d, d->name, 0, NULL);

//printf("after dp == %p, *dp at %p, f->deps = %p\n", dp, *dp, f->deps);
      /* Free the un-expanded name.  */
      free ((char*) d->name);
      free_dep (d);

      d = *dp = next;
    }

//for (d = f->deps; d; d = d->next) {
//printf("final d %s at %p\n", dep_name (d), d);
//}

    /* Shuffle mode assumes '->next' and '->shuf' links both traverse the same
       dependencies (in different sequences).  Regenerate '->shuf' so we don't
       refer to stale data.  */
    if (changed_dep)
      shuffle_deps_recursive (f->deps);
}

static struct dep **
second_expand_pattern_dep (struct file *f, struct dep **dp, const char *name)
{
  const char *s;
  size_t nperc = 0;
  size_t len;
  int order_only;
  struct dep *d = *dp;

  /* Count the number of % in the string.  */
  for (s = name; (s = strchr (s, '%')); ++s)
    ++nperc;

  if (nperc == 0)
    return second_expand_dep (f, dp, d, name, 0, NULL);

  /* Break up dep name for words. Figure out order-only. For each word
     substitute the stem, second expand the word, prepend dirname after
     second expansion.
     Dep name needs to be broken up to words in order to correctly prepend
     dirname only to those words which carry a '%'. E.g. consider rule
     'lib/hello.o: %.o: $$(strip pre-%.c) global.h'.
     When dep name '$$(strip pre-%.c) global.h' is broken for words dir_name
     is set to 'lib/' for first word '$$(strip pre-%.c)', because
     '$$(strip pre-%.c)' carries a '%'.  After stem substitution and second
     expansion the prerequisite is 'lib/pre-hello.c'.
     Similarly, dir_name is set to NULL for second word 'global.h', because
     'global.h' carries no '%'. After stem substitution and second expansion
     the prerequisite stays 'global.h' and the rule correctly becomes
     'lib/hello.o: lib/pre-hello.c global.h'.
     On the other hand, doing stem substitution and second expansion for the
     whole dep name at once would result in dir_name set to 'lib/' for both
     '$$(strip pre-%.c)' and 'global.h' and would produce an incorrect rule
     'lib/hello.o: lib/pre-hello.c lib/global.h'.  */
  order_only = 0;
  for (s = name; (s = get_next_word (s, &len)); s += len)
    {
      const char *dir_name;
      char *depname;

      if (order_only == 0 && len == 1 && *s == '|')
        {
          order_only = 1;
          continue;
        }

      /* Allocate space to replace each % with ($(*F)) and append a null. */
      depname = alloca (len + 7*nperc + 1);
      dir_name = substitute_stem (depname, s, len, d->stem_dirname);
//printf("dep old name %.*s, new name %s, dir_name = %s\n", (int) len, s, depname, dir_name);
      dp = second_expand_dep (f, dp, d, depname, order_only, dir_name);
    }
  return dp;
}

/* Second expand the name.
   Split the result of second expansion to prerequisites, prepend dirname to
   each prerequisite, enter each prerequisite.
   Advance dp to insert each new prerequisite to the dep list.
   Return the new value of dp.
   The name can be from an explicit dep or the result of stem substitution in a
   static pattern dep.  */
static struct dep **
second_expand_dep (struct file *f, struct dep **dp, struct dep *d, const char *name,
                   int order_only, const char *dirname)
{
  struct dep *new;
  char *p;
  const char *dstem = d->stem;
//TODO: write a test which fails with
//const char *stem = dstem ? dstem : NULL;
  const char *stem = dstem ? dstem : f->stem;

//printf("file %s, f->stem = %s, dep %s, d->stem = %s\n", f->name, f->stem, dep_name (d), d->stem);
  set_file_variables (f, stem);

  /* Perform second expansion.  */
//printf("2nd expanding %s\n", name);
  p = expand_string_for_file (name, f);
//printf("2nd expanded list of prereqs = %s\n", p);

  /* Parse the prerequisites.  */
  new = split_prereqs (p, dirname);

  /* If there were no prereqs here (blank!) then throw this one out.  */
  if (new == 0)
    {
      *dp = d->next;
      return dp;
    }

  /* Enter newly parsed prerequisites into the file database and advance dp to
     insert each new prerequisite to the dep list.  */
//printf("dp == %p, *dp at %p, new at %p\n", dp, *dp, new);
  *dp = new;
  for (dp = &new, d = new; d != 0; dp = &d->next, d = d->next)
    {
      d->file = lookup_file (d->name);
      if (d->file == 0)
        /* enter_files transfers ownership of d->name to d->file->name.  */
        d->file = enter_file (d->name);
      d->name = NULL;
      d->stem = dstem;
      if (!dstem)
        /* This file is explicitly mentioned as a prereq.  */
        d->file->is_explicit = 1;
      if (order_only)
        d->ignore_mtime = 1;
    }
//printf("return dp == %p, *dp at %p, new at %p\n", dp, *dp, new);
  return dp;
}
//TODO: test substitute_stem in file.t.c.
/* Copy input of size len to buf with the first not-escaped % in each
   white-space-separated word substituted with $*, ($*), $(*F) or ($(*F)).
   If *dirname is set, then substitute the % with $(*F), otherwise with $*.
//TODO: should this embrace only take place when the number of dollars is odd?
//TODO: should only the first character be expanded?
//TODO: give this up completely?
//TODO: count the dollars and if odd, do $$*?
   If the % is immediately preceded by a $, then embrace $* or $(*F) with
   parenthesis.
   Null terminate buf.
   If input contains a %, then return dirname. Otherwise, return NULL.  */
static const char *
substitute_stem (char *buf, const char *input, size_t len, const char *dirname)
{
  char *s = buf;
  const char *end = buf + len;
  const char *dir_name = NULL;

  memcpy (s, input, len);
  s[len] = '\0';

  for (; (s = find_percent (s)); buf = s)
    {
      dir_name = dirname;
      if (s > buf && *(s - 1) == '$')
        {
          if (*dirname)
            {
              memmove (s + 7, s + 1, end - s);
              end += 7;
              s = mempcpy (s, "($(*F))", 7);
            }
          else
            {
              memmove (s + 4, s + 1, end - s);
              end += 4;
              s = mempcpy (s, "($*)", 4);
            }
        }
      else
        {
          if (*dirname)
            {
              memmove (s + 5, s + 1, end - s);
              end += 5;
              s = mempcpy (s, "$(*F)", 5);
            }
          else
            {
              memmove (s + 2, s + 1, end - s);
              end += 2;
              s = mempcpy (s, "$*", 2);
            }
        }
      /* Find the first % after the next whitespace.
         No need to worry about order-only, or nested
         functions: input went though get_next_word.  */
      while (s < end && ! END_OF_TOKEN (*s))
        ++s;
    }
  return dir_name;
}

/* Add extra prereqs to the file in question.  */

struct dep *
expand_extra_prereqs (const struct variable *extra)
{
  struct dep *d;
  struct dep *prereqs;

  prereqs = extra ? split_prereqs (expand_string (extra->value), NULL) : NULL;

  for (d = prereqs; d; d = d->next)
    {
      d->file = lookup_file (d->name);
      if (!d->file)
        d->file = enter_file (d->name);
      d->name = NULL;
      d->ignore_automatic_vars = 1;
    }

  return prereqs;
}

/* Perform per-file snap operations. */

static void
snap_file (const void *item, void *arg)
{
  struct file *f = (struct file*)item;
  struct dep *prereqs = NULL;
  struct dep *d;

  /* If we're not doing second expansion then reset updating.  */
  if (!second_expansion)
    f->updating = 0;

  /* More specific setting has priority.  */

  /* If .SECONDARY is set with no deps, mark all targets as intermediate,
     unless the target is a prereq of .NOTINTERMEDIATE.  */
  if (all_secondary && !f->notintermediate)
    f->intermediate = 1;

  /* If .NOTINTERMEDIATE is set with no deps, mark all targets as
     notintermediate, unless the target is a prereq of .INTERMEDIATE.  */
  if (no_intermediates && !f->intermediate && !f->secondary)
    f->notintermediate = 1;

  /* If .EXTRA_PREREQS is set, add them as ignored by automatic variables.  */
  if (f->variables)
    {
      prereqs = expand_extra_prereqs (lookup_variable_in_set (
                      STRING_SIZE_TUPLE(".EXTRA_PREREQS"), f->variables->set));
      if (second_expansion)
        for (d = prereqs; d; d = d->next)
          {
            if (!d->name)
              d->name = xstrdup (d->file->name);
            d->need_2nd_expansion = 1;
          }
    }
  else if (f->is_target)
    prereqs = copy_dep_chain (arg);

  if (prereqs)
    {
      for (d = prereqs; d; d = d->next)
        if (streq (f->name, dep_name (d)))
          /* Skip circular dependencies.  */
          break;

      if (d)
        /* We broke early: must have found a circular dependency.  */
        free_dep_chain (prereqs);
      else if (!f->deps)
        f->deps = prereqs;
      else
        {
          d = f->deps;
          while (d->next)
            d = d->next;
          d->next = prereqs;
        }
    }
}

/* Mark the files depended on by .PRECIOUS, .PHONY, .SILENT,
   and various other special targets.  */

void
snap_deps (void)
{
  struct file *f;
  struct file *f2;
  struct dep *d;

  /* Remember that we've done this.  Once we start snapping deps we can no
     longer define new targets.  */
  snapped_deps = 1;

  /* Now manage all the special targets.  */

  for (f = lookup_file (".PRECIOUS"); f != 0; f = f->prev)
    for (d = f->deps; d != 0; d = d->next)
      for (f2 = d->file; f2 != 0; f2 = f2->prev)
        f2->precious = 1;

  for (f = lookup_file (".LOW_RESOLUTION_TIME"); f != 0; f = f->prev)
    for (d = f->deps; d != 0; d = d->next)
      for (f2 = d->file; f2 != 0; f2 = f2->prev)
        f2->low_resolution_time = 1;

  for (f = lookup_file (".PHONY"); f != 0; f = f->prev)
    for (d = f->deps; d != 0; d = d->next)
      for (f2 = d->file; f2 != 0; f2 = f2->prev)
        {
          /* Mark this file as phony nonexistent target.  */
          f2->phony = 1;
          f2->is_target = 1;
          f2->last_mtime = NONEXISTENT_MTIME;
          f2->mtime_before_update = NONEXISTENT_MTIME;
        }

  for (f = lookup_file (".NOTINTERMEDIATE"); f != 0; f = f->prev)
    /* Mark .NOTINTERMEDIATE deps as notintermediate files.  */
    if (f->deps)
        for (d = f->deps; d != 0; d = d->next)
          for (f2 = d->file; f2 != 0; f2 = f2->prev)
            f2->notintermediate = 1;
    /* .NOTINTERMEDIATE with no deps marks all files as notintermediate.  */
    else
      no_intermediates = 1;

  /* The same file cannot be both .INTERMEDIATE and .NOTINTERMEDIATE.
     However, it is possible for a file to be .INTERMEDIATE and also match a
     .NOTINTERMEDIATE pattern.  In that case, the intermediate file has
     priority over the notintermediate pattern.  This priority is enforced by
     pattern_search.  */

  for (f = lookup_file (".INTERMEDIATE"); f != 0; f = f->prev)
    /* Mark .INTERMEDIATE deps as intermediate files.  */
    for (d = f->deps; d != 0; d = d->next)
      for (f2 = d->file; f2 != 0; f2 = f2->prev)
        if (f2->notintermediate)
          OS (fatal, NILF,
              _("%s cannot be both .NOTINTERMEDIATE and .INTERMEDIATE"),
              f2->name);
        else
          f2->intermediate = 1;
    /* .INTERMEDIATE with no deps does nothing.
       Marking all files as intermediates is useless since the goal targets
       would be deleted after they are built.  */

  for (f = lookup_file (".SECONDARY"); f != 0; f = f->prev)
    /* Mark .SECONDARY deps as both intermediate and secondary.  */
    if (f->deps)
      for (d = f->deps; d != 0; d = d->next)
        for (f2 = d->file; f2 != 0; f2 = f2->prev)
        if (f2->notintermediate)
          OS (fatal, NILF,
              _("%s cannot be both .NOTINTERMEDIATE and .SECONDARY"),
              f2->name);
        else
          f2->intermediate = f2->secondary = 1;
    /* .SECONDARY with no deps listed marks *all* files that way.  */
    else
      all_secondary = 1;

  if (no_intermediates && all_secondary)
    O (fatal, NILF,
       _(".NOTINTERMEDIATE and .SECONDARY are mutually exclusive"));

  f = lookup_file (".EXPORT_ALL_VARIABLES");
  if (f != 0 && f->is_target)
    export_all_variables = 1;

  f = lookup_file (".IGNORE");
  if (f != 0 && f->is_target)
    {
      if (f->deps == 0)
        ignore_errors_flag = 1;
      else
        for (d = f->deps; d != 0; d = d->next)
          for (f2 = d->file; f2 != 0; f2 = f2->prev)
            f2->command_flags |= COMMANDS_NOERROR;
    }

  f = lookup_file (".SILENT");
  if (f != 0 && f->is_target)
    {
      if (f->deps == 0)
        run_silent = 1;
      else
        for (d = f->deps; d != 0; d = d->next)
          for (f2 = d->file; f2 != 0; f2 = f2->prev)
            f2->command_flags |= COMMANDS_SILENT;
    }

  f = lookup_file (".NOTPARALLEL");
  if (f != 0 && f->is_target)
    {
      struct dep *d2;

      if (!f->deps)
        not_parallel = 1;
      else
        /* Set a wait point between every prerequisite of each target.  */
        for (d = f->deps; d != NULL; d = d->next)
          for (f2 = d->file; f2 != NULL; f2 = f2->prev)
            if (f2->deps)
              for (d2 = f2->deps->next; d2 != NULL; d2 = d2->next)
                d2->wait_here = 1;
    }

  {
    struct dep *prereqs = expand_extra_prereqs (lookup_variable (STRING_SIZE_TUPLE(".EXTRA_PREREQS")));

    /* Perform per-file snap operations.  */
    hash_map_arg(&files, snap_file, prereqs);

    free_dep_chain (prereqs);
  }

#ifndef NO_MINUS_C_MINUS_O
  /* If .POSIX was defined, remove OUTPUT_OPTION to comply.  */
  /* This needs more work: what if the user sets this in the makefile?
  if (posix_pedantic)
    define_variable_cname ("OUTPUT_OPTION", "", o_default, 1);
  */
#endif
}

/* Set the 'command_state' member of FILE and all its 'also_make's.
   Don't decrease the state of also_make's (e.g., don't downgrade a 'running'
   also_make to a 'deps_running' also_make).  */

void
set_command_state (struct file *file, enum cmd_state state)
{
  struct dep *d;

  file->command_state = state;

  for (d = file->also_make; d != 0; d = d->next)
    if (state > d->file->command_state)
      d->file->command_state = state;
}

/* Convert an external file timestamp to internal form.  */

FILE_TIMESTAMP
file_timestamp_cons (const char *fname, time_t stamp, long int ns)
{
  int offset = ORDINARY_MTIME_MIN + (FILE_TIMESTAMP_HI_RES ? ns : 0);
  FILE_TIMESTAMP s = stamp;
  FILE_TIMESTAMP product = (FILE_TIMESTAMP) s << FILE_TIMESTAMP_LO_BITS;
  FILE_TIMESTAMP ts = product + offset;

  if (! (s <= FILE_TIMESTAMP_S (ORDINARY_MTIME_MAX)
         && product <= ts && ts <= ORDINARY_MTIME_MAX))
    {
      char buf[FILE_TIMESTAMP_PRINT_LEN_BOUND + 1];
      const char *f = fname ? fname : _("Current time");
      ts = s <= OLD_MTIME ? ORDINARY_MTIME_MIN : ORDINARY_MTIME_MAX;
      file_timestamp_sprintf (buf, ts);
      OSS (error, NILF,
           _("%s: timestamp out of range: substituting %s"), f, buf);
    }

  return ts;
}

/* Return the current time as a file timestamp, setting *RESOLUTION to
   its resolution.  */
FILE_TIMESTAMP
file_timestamp_now (int *resolution)
{
  int r;
  time_t s;
  int ns;

  /* Don't bother with high-resolution clocks if file timestamps have
     only one-second resolution.  The code below should work, but it's
     not worth the hassle of debugging it on hosts where it fails.  */
#if FILE_TIMESTAMP_HI_RES
# if HAVE_CLOCK_GETTIME && defined CLOCK_REALTIME
  {
    struct timespec timespec;
    if (clock_gettime (CLOCK_REALTIME, &timespec) == 0)
      {
        r = 1;
        s = timespec.tv_sec;
        ns = timespec.tv_nsec;
        goto got_time;
      }
  }
# endif
# if HAVE_GETTIMEOFDAY
  {
    struct timeval timeval;
    if (gettimeofday (&timeval, 0) == 0)
      {
        r = 1000;
        s = timeval.tv_sec;
        ns = timeval.tv_usec * 1000;
        goto got_time;
      }
  }
# endif
#endif

  r = 1000000000;
  s = time ((time_t *) 0);
  ns = 0;

#if FILE_TIMESTAMP_HI_RES
 got_time:
#endif
  *resolution = r;
  return file_timestamp_cons (0, s, ns);
}

/* Place into the buffer P a printable representation of the file
   timestamp TS.  */
void
file_timestamp_sprintf (char *p, FILE_TIMESTAMP ts)
{
  time_t t = FILE_TIMESTAMP_S (ts);
  struct tm *tm = localtime (&t);

  if (tm)
    {
      intmax_t year = tm->tm_year;
      sprintf (p, "%04" PRIdMAX "-%02d-%02d %02d:%02d:%02d",
               year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
  else if (t < 0)
    sprintf (p, "%" PRIdMAX, (intmax_t) t);
  else
    sprintf (p, "%" PRIuMAX, (uintmax_t) t);
  p += strlen (p);

  /* Append nanoseconds as a fraction, but remove trailing zeros.  We don't
     know the actual timestamp resolution, since clock_getres applies only to
     local times, whereas this timestamp might come from a remote filesystem.
     So removing trailing zeros is the best guess that we can do.  */
  sprintf (p, ".%09d", FILE_TIMESTAMP_NS (ts));
  p += strlen (p) - 1;
  while (*p == '0')
    p--;
  p += *p != '.';

  *p = '\0';
}

/* Print the data base of files.  */

static void
print_prereqs (const struct dep *deps)
{
  const struct dep *ood = 0;

  /* Print all normal dependencies; note any order-only deps.  */
  for (; deps != 0; deps = deps->next)
    if (! deps->ignore_mtime)
      printf (" %s%s", deps->wait_here ? ".WAIT " : "", dep_name (deps));
    else if (! ood)
      ood = deps;

  /* Print order-only deps, if we have any.  */
  if (ood)
    {
      printf (" | %s%s", ood->wait_here ? ".WAIT " : "", dep_name (ood));
      for (ood = ood->next; ood != 0; ood = ood->next)
        if (ood->ignore_mtime)
          printf (" %s%s", ood->wait_here ? ".WAIT " : "", dep_name (ood));
    }

  putchar ('\n');
}

static void
print_file (const void *item)
{
  const struct file *f = item;

  /* If we're not using builtin targets, don't show them.

     Ideally we'd be able to delete them altogether but currently there's no
     facility to ever delete a file once it's been added.  */
  if (no_builtin_rules_flag && f->builtin)
    return;

  putchar ('\n');

  if (f->cmds && f->cmds->recipe_prefix != cmd_prefix)
    {
      fputs (".RECIPEPREFIX = ", stdout);
      cmd_prefix = f->cmds->recipe_prefix;
      if (cmd_prefix != RECIPEPREFIX_DEFAULT)
        putchar (cmd_prefix);
      putchar ('\n');
    }

  if (f->variables != 0)
    print_target_variables (f);

  if (!f->is_target)
    puts (_("# Not a target:"));
  printf ("%s:%s", f->name, f->double_colon ? ":" : "");
  print_prereqs (f->deps);

  if (f->precious)
    puts (_("#  Precious file (prerequisite of .PRECIOUS)."));
  if (f->phony)
    puts (_("#  Phony target (prerequisite of .PHONY)."));
  if (f->cmd_target)
    puts (_("#  Command line target."));
  if (f->dontcare)
    puts (_("#  A default, MAKEFILES, or -include/sinclude makefile."));
  if (f->builtin)
    puts (_("#  Builtin rule"));
  puts (f->tried_implicit
        ? _("#  Implicit rule search has been done.")
        : _("#  Implicit rule search has not been done."));
  if (f->stem != 0)
    printf (_("#  Implicit/static pattern stem: '%s'\n"), f->stem);
  if (f->intermediate)
    puts (_("#  File is an intermediate prerequisite."));
  if (f->notintermediate)
    puts (_("#  File is a prerequisite of .NOTINTERMEDIATE."));
  if (f->secondary)
    puts (_("#  File is secondary (prerequisite of .SECONDARY)."));
  if (f->also_make != 0)
    {
      const struct dep *d;
      fputs (_("#  Also makes:"), stdout);
      for (d = f->also_make; d != 0; d = d->next)
        printf (" %s", dep_name (d));
      putchar ('\n');
    }
  if (f->last_mtime == UNKNOWN_MTIME)
    puts (_("#  Modification time never checked."));
  else if (f->last_mtime == NONEXISTENT_MTIME)
    puts (_("#  File does not exist."));
  else if (f->last_mtime == OLD_MTIME)
    puts (_("#  File is very old."));
  else
    {
      char buf[FILE_TIMESTAMP_PRINT_LEN_BOUND + 1];
      file_timestamp_sprintf (buf, f->last_mtime);
      printf (_("#  Last modified %s\n"), buf);
    }
  puts (f->updated
        ? _("#  File has been updated.") : _("#  File has not been updated."));
  switch (f->command_state)
    {
    case cs_running:
      puts (_("#  Recipe currently running (THIS IS A BUG)."));
      break;
    case cs_deps_running:
      puts (_("#  Dependencies recipe running (THIS IS A BUG)."));
      break;
    case cs_not_started:
    case cs_finished:
      switch (f->update_status)
        {
        case us_none:
          break;
        case us_success:
          puts (_("#  Successfully updated."));
          break;
        case us_question:
          assert (question_flag);
          puts (_("#  Needs to be updated (-q is set)."));
          break;
        case us_failed:
          puts (_("#  Failed to be updated."));
          break;
        }
      break;
    default:
      puts (_("#  Invalid value in 'command_state' member!"));
      fflush (stdout);
      fflush (stderr);
      abort ();
    }

  if (f->variables != 0)
    print_file_variables (f);

  if (f->cmds != 0)
    print_commands (f->cmds);

  if (f->prev)
    print_file ((const void *) f->prev);
}

void
print_file_data_base (void)
{
  puts (_("\n# Files"));

  hash_map (&files, print_file);

  fputs (_("\n# files hash-table stats:\n# "), stdout);
  hash_print_stats (&files, stdout);
}

static void
print_target (const void *item)
{
  const struct file *f = item;

  if (!f->is_target || f->suffix)
    return;

  /* Ignore any special targets, as defined by POSIX. */
  if (f->name[0] == '.' && isupper ((unsigned char)f->name[1]))
    {
      const char *cp = f->name + 1;
      while (*(++cp) != '\0')
        if (!isupper ((unsigned char)*cp))
          break;
      if (*cp == '\0')
        return;
    }

  puts (f->name);
}

void
print_targets (void)
{
  hash_map (&files, print_target);
}

/* Verify the integrity of the data base of files.  */

#define VERIFY_CACHED(_p,_n) \
    do{                                                                       \
        if (_p->_n && _p->_n[0] && !strcache_iscached (_p->_n))               \
          error (NULL, strlen (_p->name) + CSTRLEN (# _n) + strlen (_p->_n),  \
                 _("%s: field '%s' not cached: %s"), _p->name, # _n, _p->_n); \
    }while(0)

static void
verify_file (const void *item)
{
  const struct file *f = item;
  const struct dep *d;

  VERIFY_CACHED (f, name);
  VERIFY_CACHED (f, hname);
  VERIFY_CACHED (f, vpath);
  VERIFY_CACHED (f, stem);

  /* Check the deps.  */
  for (d = f->deps; d != 0; d = d->next)
    {
      if (! d->need_2nd_expansion)
        VERIFY_CACHED (d, name);
      VERIFY_CACHED (d, stem);
      VERIFY_CACHED (d, stem_dirname);
    }
}

void
verify_file_data_base (void)
{
  hash_map (&files, verify_file);
}

#define EXPANSION_INCREMENT(_l)  ((((_l) / 500) + 1) * 500)

char *
build_target_list (char *value)
{
  static unsigned long last_targ_count = 0;

  if (files.ht_fill != last_targ_count)
    {
      size_t max = EXPANSION_INCREMENT (strlen (value));
      size_t len;
      char *p;
      struct file **fp = (struct file **) files.ht_vec;
      struct file **end = &fp[files.ht_size];

      /* Make sure we have at least MAX bytes in the allocated buffer.  */
      value = xrealloc (value, max);

      p = value;
      len = 0;
      for (; fp < end; ++fp)
        if (!HASH_VACANT (*fp) && (*fp)->is_target)
          {
            struct file *f = *fp;
            size_t l = strlen (f->name);

            len += l + 1;
            if (len > max)
              {
                size_t off = p - value;

                max += EXPANSION_INCREMENT (l + 1);
                value = xrealloc (value, max);
                p = &value[off];
              }

            p = mempcpy (p, f->name, l);
            *(p++) = ' ';
          }
      *(p-1) = '\0';

      last_targ_count = files.ht_fill;
    }

  return value;
}

void
init_hash_files (void)
{
  hash_init (&files, 1000, file_hash_1, file_hash_2, file_hash_cmp);
}

/* EOF */
