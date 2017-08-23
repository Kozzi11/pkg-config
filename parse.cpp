/* 
 * Copyright (C) 2006-2011 Tollef Fog Heen <tfheen@err.no>
 * Copyright (C) 2001, 2002, 2005-2006 Red Hat Inc.
 * Copyright (C) 2010 Dan Nicholson <dbn.lists@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "parse.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <sys/types.h>
#include<algorithm>
#include<glib.h>

bool parse_strict = true;
bool define_prefix = ENABLE_DEFINE_PREFIX;
const char *prefix_variable = "prefix";

#ifdef _WIN32
bool msvc_syntax = false;
#endif

static std::string get_basename(const std::string &s) {
    const char separator = '/';
    auto loc = s.rfind(separator);
    if(loc == std::string::npos) {
        return ".";
    }
    return s.substr(loc+1, std::string::npos);
}

static std::string get_dirname(const std::string &s) {
    const char separator = '/';
    auto loc = s.rfind(separator);
    if(loc == std::string::npos) {
        return ".";
    }
    return s.substr(0, loc);
}

bool string_starts_with(const std::string &s, const std::string prefix) {
    if(prefix.length() > s.length()) {
        return false;
    }
    for(std::string::size_type i=0; i<prefix.size(); i++) {
        if(s[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/**
 * Read an entire line from a file into a buffer. Lines may
 * be delimited with '\n', '\r', '\n\r', or '\r\n'. The delimiter
 * is not written into the buffer. Text after a '#' character is treated as
 * a comment and skipped. '\' can be used to escape a # character.
 * '\' proceding a line delimiter combines adjacent lines. A '\' proceding
 * any other character is ignored and written into the output buffer
 * unmodified.
 * 
 * Return value: %false if the stream was already at an EOF character.
 **/
static bool read_one_line(FILE *stream, std::string &str) {
    bool quoted = false;
    bool comment = false;
    int n_read = 0;

    str.clear();

    while(true) {
        int c;

        c = getc(stream);

        if(c == EOF) {
            if(quoted)
                str += '\\';

            goto done;
        } else
            n_read++;

        if(quoted) {
            quoted = false;

            switch (c){
            case '#':
                str += '#';
                break;
            case '\r':
            case '\n': {
                int next_c = getc(stream);

                if(!(c == EOF || (c == '\r' && next_c == '\n') || (c == '\n' && next_c == '\r')))
                    ungetc(next_c, stream);

                break;
            }
            default:
                str +=  '\\';
                str += c;
            }
        } else {
            switch (c){
            case '#':
                comment = true;
                break;
            case '\\':
                if(!comment)
                    quoted = true;
                break;
            case '\n': {
                int next_c = getc(stream);

                if(!(c == EOF || (c == '\r' && next_c == '\n') || (c == '\n' && next_c == '\r')))
                    ungetc(next_c, stream);

                goto done;
            }
            default:
                if(!comment)
                    str += c;
            }
        }
    }

    done:

    return n_read > 0;
}

static std::string
trim_string(const std::string &str) {
    std::string res(str);

    while(!res.empty() && isspace(res.front())) {
        res.erase(0, 1);
    }
    while(!res.empty() && isspace(res.back())) {
        res.erase(str.size()-1, 1);
    }
    return res;
}

static std::string
trim_and_sub(Package *pkg, const std::string &str, const std::string &path) {
    std::string subst;
    std::string::size_type p = 0;

    auto trimmed = trim_string(str);

    while(p<trimmed.size()) {
        if(str[p] == '$' && str[p+1] == '$') {
            /* escaped $ */
            subst += '$';
            p += 2;
        } else if(str[p] == '$' && str[p+1] == '{') {
            /* variable */
            std::string varval;

            auto var_start = p+2;

            /* Get up to close brace. */
            while(p<str.size() && str[p] != '}')
                ++p;

            auto varname = str.substr(var_start, p-var_start);

            ++p; /* past brace */

            varval = package_get_var(pkg, varname);

            if(varval.empty()) {
                verbose_error("Variable '%s' not defined in '%s'\n", varname, path);
                if(parse_strict)
                    exit(1);
            }

            subst += varval;
        } else {
            subst += str[p];

            ++p;
        }
    }

    return subst;
}

static void parse_name(Package *pkg, std::string &str, const std::string &path) {
    if(!pkg->name.empty()) {
        verbose_error("Name field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    pkg->name = trim_and_sub(pkg, str, path);
}

static void parse_version(Package *pkg, const std::string &str, const std::string &path) {
    if(!pkg->version.empty()) {
        verbose_error("Version field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    pkg->version = trim_and_sub(pkg, str, path);
}

static void parse_description(Package *pkg, const std::string &str, const std::string &path) {
    if(!pkg->description.empty()) {
        verbose_error("Description field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    pkg->description = trim_and_sub(pkg, str, path);
}

#define MODULE_SEPARATOR(c) ((c) == ',' || isspace ((guchar)(c)))
#define OPERATOR_CHAR(c) ((c) == '<' || (c) == '>' || (c) == '!' || (c) == '=')

/* A module list is a list of modules with optional version specification,
 * separated by commas and/or spaces. Commas are treated just like whitespace,
 * in order to allow stuff like: Requires: @FRIBIDI_PC@, glib, gmodule
 * where @FRIBIDI_PC@ gets substituted to nothing or to 'fribidi'
 */

typedef enum {
    /* put numbers to help interpret lame debug spew ;-) */
    OUTSIDE_MODULE = 0,
    IN_MODULE_NAME = 1,
    BEFORE_OPERATOR = 2,
    IN_OPERATOR = 3,
    AFTER_OPERATOR = 4,
    IN_MODULE_VERSION = 5
} ModuleSplitState;

#define PARSE_SPEW 0

static std::vector<std::string>
split_module_list(const std::string &str, const std::string &path) {
    std::vector<std::string> retval;
    std::string::size_type p = 0;
    std::string::size_type start = 0;
    ModuleSplitState state = OUTSIDE_MODULE;
    ModuleSplitState last_state = OUTSIDE_MODULE;

    /*   fprintf (stderr, "Parsing: '%s'\n", str); */

    while(p < str.size()) {
#if PARSE_SPEW
        fprintf (stderr, "p: %c state: %d last_state: %d\n", str.c_str() + (int)p, state, last_state);
#endif

        switch (state){
        case OUTSIDE_MODULE:
            if(!MODULE_SEPARATOR(str[p]))
                state = IN_MODULE_NAME;
            break;

        case IN_MODULE_NAME:
            if(isspace((guchar) str[p])) {
                /* Need to look ahead to determine next state */
                auto s = p;
                while(s < str.size() && isspace((guchar) str[s]))
                    ++s;

                if(s == str.size())
                    state = OUTSIDE_MODULE;
                else if(MODULE_SEPARATOR(str[s]))
                    state = OUTSIDE_MODULE;
                else if(OPERATOR_CHAR(str[s]))
                    state = BEFORE_OPERATOR;
                else
                    state = OUTSIDE_MODULE;
            } else if(MODULE_SEPARATOR(str[p]))
                state = OUTSIDE_MODULE; /* comma precludes any operators */
            break;

        case BEFORE_OPERATOR:
            /* We know an operator is coming up here due to lookahead from
             * IN_MODULE_NAME
             */
            if(isspace((guchar) str[p]))
                ; /* no change */
            else if(OPERATOR_CHAR(str[p]))
                state = IN_OPERATOR;
            else
                throw "Unreachable code";
            break;

        case IN_OPERATOR:
            if(!OPERATOR_CHAR(str[p]))
                state = AFTER_OPERATOR;
            break;

        case AFTER_OPERATOR:
            if(!isspace((guchar) str[p]))
                state = IN_MODULE_VERSION;
            break;

        case IN_MODULE_VERSION:
            if(MODULE_SEPARATOR(str[p]))
                state = OUTSIDE_MODULE;
            break;

        default:
            throw "Unreachable code.";
        }

        if(state == OUTSIDE_MODULE && last_state != OUTSIDE_MODULE) {
            /* We left a module */
            std::string module = str.substr(start, p - start);
            retval.push_back(module);

#if PARSE_SPEW
            fprintf (stderr, "found module: '%s'\n", module);
#endif

            /* reset start */
            start = p;
        }

        last_state = state;
        ++p;
    }

    if(p != start) {
        /* get the last module */
        std::string module = str.substr(start, p - start);
        retval.push_back(module);

#if PARSE_SPEW
        fprintf (stderr, "found module: '%s'\n", module);
#endif

    }

    return retval;
}

std::vector<RequiredVersion>
parse_module_list(Package *pkg, const std::string &str_, const std::string &path) {
    std::vector<RequiredVersion> retval;

    auto split = split_module_list(str_, path);

    for(auto &str : split) {
        std::string::size_type p = 0;
        std::string::size_type start = 0;

        RequiredVersion ver;
        ver.comparison = ALWAYS_MATCH;
        ver.owner = pkg;

        while(p<str.size() && MODULE_SEPARATOR(str[p]))
            ++p;

        start = p;

        while(p<str.size() && !isspace((guchar) str[p]))
            ++p;

        std::string package_name = str.substr(start, p-start);
        while(p<str.size() && MODULE_SEPARATOR(str[p])) {
            ++p;
        }

        if(package_name.empty()) {
            verbose_error("Empty package name in Requires or Conflicts in file '%s'\n", path);
            if(parse_strict)
                exit(1);
            else
                continue;
        }

        ver.name = package_name;

        start = p;

        while(p<str.size() && !isspace((guchar) str[p])) {
            ++p;
        }

        std::string comparison = str.substr(start, p-start);
        while(p<str.size() && isspace((guchar) str[p])) {
            ++p;
        }

        if(!comparison.empty()) {
            if(comparison == "=")
                ver.comparison = EQUAL;
            else if(comparison == ">=")
                ver.comparison = GREATER_THAN_EQUAL;
            else if(comparison == "<=")
                ver.comparison = LESS_THAN_EQUAL;
            else if(comparison == ">")
                ver.comparison = GREATER_THAN;
            else if(comparison == "<")
                ver.comparison = LESS_THAN;
            else if(comparison == "!=")
                ver.comparison = NOT_EQUAL;
            else {
                verbose_error("Unknown version comparison operator '%s' after "
                        "package name '%s' in file '%s'\n", comparison.c_str(), ver.name.c_str(), path);
                if(parse_strict)
                    exit(1);
                else
                    continue;
            }
        }

        start = p;

        while(p<str.size() && !MODULE_SEPARATOR(str[p]))
            ++p;

        std::string version = str.substr(start, p-start);
        while(p<str.size() && MODULE_SEPARATOR(str[p])) {
            ++p;
        }

        if(ver.comparison != ALWAYS_MATCH && version.empty()) {
            verbose_error("Comparison operator but no version after package "
                    "name '%s' in file '%s'\n", ver.name.c_str(), path);
            if(parse_strict)
                exit(1);
            else {
                ver.version = "0";
                continue;
            }
        }

        if(!version.empty()) {
            ver.version = version;
        }

        g_assert(!ver.name.empty());
        retval.push_back(ver);
    }

    return retval;
}

static std::vector<std::string>
split_module_list2(const std::string &str, const std::string &path) {
    std::vector<std::string> retval;
    std::string::size_type p = 0, start = 0;
    ModuleSplitState state = OUTSIDE_MODULE;
    ModuleSplitState last_state = OUTSIDE_MODULE;

    /*   fprintf (stderr, "Parsing: '%s'\n", str); */

    while(p<str.length()) {
#if PARSE_SPEW
        fprintf (stderr, "p: %c state: %d last_state: %d\n", *p, state, last_state);
#endif

        switch (state){
        case OUTSIDE_MODULE:
            if(!MODULE_SEPARATOR(str[p]))
                state = IN_MODULE_NAME;
            break;

        case IN_MODULE_NAME:
            if(isspace(str[p])) {
                /* Need to look ahead to determine next state */
                auto s = p;
                while(s<str.length() && isspace(str[s]))
                    ++s;

                if(s>=str.length())
                    state = OUTSIDE_MODULE;
                else if(MODULE_SEPARATOR(str[s]))
                    state = OUTSIDE_MODULE;
                else if(OPERATOR_CHAR(str[s]))
                    state = BEFORE_OPERATOR;
                else
                    state = OUTSIDE_MODULE;
            } else if(MODULE_SEPARATOR(str[p]))
                state = OUTSIDE_MODULE; /* comma precludes any operators */
            break;

        case BEFORE_OPERATOR:
            /* We know an operator is coming up here due to lookahead from
             * IN_MODULE_NAME
             */
            if(isspace(str[p]))
                ; /* no change */
            else if(OPERATOR_CHAR(str[p]))
                state = IN_OPERATOR;
            else
                throw "Unreachable code.";
            break;

        case IN_OPERATOR:
            if(!OPERATOR_CHAR(str[p]))
                state = AFTER_OPERATOR;
            break;

        case AFTER_OPERATOR:
            if(!isspace(str[p]))
                state = IN_MODULE_VERSION;
            break;

        case IN_MODULE_VERSION:
            if(MODULE_SEPARATOR(str[p]))
                state = OUTSIDE_MODULE;
            break;

        default:
            throw "Unreachable code";
        }

        if(state == OUTSIDE_MODULE && last_state != OUTSIDE_MODULE) {
            /* We left a module */
            auto module = str.substr(start, p - start);
            retval.push_back(module);

#if PARSE_SPEW
            fprintf (stderr, "found module: '%s'\n", module);
#endif

            /* reset start */
            start = p;
        }

        last_state = state;
        ++p;
    }

    if(p != start) {
        /* get the last module */
        auto module = str.substr(start, p - start);
        retval.push_back(module);

#if PARSE_SPEW
        fprintf (stderr, "found module: '%s'\n", module);
#endif

    }

    return retval;
}

std::vector<RequiredVersion>
parse_module_list2(Package *pkg, const std::string &str, const std::string &path) {
    std::vector<std::string> split;
    std::vector<RequiredVersion> retval;

    split = split_module_list2(str.c_str(), path);

    for(const auto &iter : split) {
        RequiredVersion ver;
        std::string::size_type p=0, start=0;
        std::string tmpstr{iter};

        ver.comparison = ALWAYS_MATCH;
        ver.owner = pkg;

        while(p<iter.size() && MODULE_SEPARATOR(iter[p]))
            ++p;

        start = p;

        while(p<iter.size() && !isspace(iter[p]))
            ++p;

        auto name = iter.substr(start, p-start);
        while(p<iter.length() && MODULE_SEPARATOR(iter[p])) {
            ++p;
        }

        if(name.empty()) {
            verbose_error("Empty package name in Requires or Conflicts in file '%s'\n", path);
            if(parse_strict)
                exit(1);
            else
                continue;
        }

        ver.name = name;

        start = p;

        while(p<str.size() && !isspace(str[p]))
            ++p;

        auto comparer = iter.substr(start, p-start);
        while(p<iter.size() && isspace(iter[p])) {
            ++p;
        }

        if(!comparer.empty()) {
            if(comparer == "=")
                ver.comparison = EQUAL;
            else if(comparer == ">=")
                ver.comparison = GREATER_THAN_EQUAL;
            else if(comparer == "<=")
                ver.comparison = LESS_THAN_EQUAL;
            else if(comparer == ">")
                ver.comparison = GREATER_THAN;
            else if(comparer == "<")
                ver.comparison = LESS_THAN;
            else if(comparer == "!=")
                ver.comparison = NOT_EQUAL;
            else {
                verbose_error("Unknown version comparison operator '%s' after "
                        "package name '%s' in file '%s'\n", comparer.c_str(), ver.name.c_str(), path.c_str());
                if(parse_strict)
                    exit(1);
                else
                    continue;
            }
        }

        start = p;

        while(p<iter.size() && !MODULE_SEPARATOR(iter[p]))
            ++p;

        auto number = iter.substr(start, p-start);
        while(p<iter.length() && MODULE_SEPARATOR(iter[p])) {
            ++p;
        }

        if(ver.comparison != ALWAYS_MATCH && number.empty()) {
            verbose_error("Comparison operator but no version after package "
                    "name '%s' in file '%s'\n", ver.name.c_str(), path.c_str());
            if(parse_strict)
                exit(1);
            else {
                ver.version = "0";
                continue;
            }
        }

        if(!number.empty()) {
            ver.version = number;
        }

        g_assert(!ver.name.empty());
        retval.push_back(ver);
    }

    return retval;
}

static void parse_requires(Package *pkg, const std::string &str, const std::string &path) {
    if(!pkg->requires.empty()) {
        verbose_error("Requires field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    auto trimmed = trim_and_sub(pkg, str, path);
    pkg->requires_entries = parse_module_list(pkg, trimmed, path);
}

static void parse_requires_private(Package *pkg, const std::string &str, const std::string &path) {
    std::string trimmed;

    if(!pkg->requires_private.empty()) {
        verbose_error("Requires.private field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    trimmed = trim_and_sub(pkg, str, path);
    pkg->requires_private_entries = parse_module_list(pkg, trimmed, path);
}

static void parse_conflicts(Package *pkg, const std::string &str, const std::string &path) {

    if(!pkg->conflicts.empty()) {
        verbose_error("Conflicts field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    auto trimmed = trim_and_sub(pkg, str, path);
    pkg->conflicts = parse_module_list2(pkg, trimmed, path);
}

static std::string strdup_escape_shell(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 10);
    for(char c : s) {
        if((c < '$') || (c > '$' && s[0] < '(') || (c > ')' && c < '+') || (c > ':' && c < '=')
                || (c > '=' && c < '@') || (c > 'Z' && c < '^') || (c == '`')
                || (c > 'z' && c < '~') || (c > '~')) {
            r.push_back('\\');
        }
        r.push_back(c);
    }
    return r;
}

static void _do_parse_libs(Package *pkg, const std::vector<std::string> &args) {
    std::string::size_type i = 0;
#ifdef G_OS_WIN32
    const char *L_flag = (msvc_syntax ? "/libpath:" : "-L");
    const std::string l_flag = (msvc_syntax ? "" : "-l");
    const char *lib_suffix = (msvc_syntax ? ".lib" : "");
#else
    const char *L_flag = "-L";
    const std::string l_flag = "-l";
    const char *lib_suffix = "";
#endif

    while(i < args.size()) {
        Flag flag;
        auto tmp = trim_string(args[i]);
        std::string arg = strdup_escape_shell(tmp);
        std::string::size_type p = 0;

        if(arg[p] == '-' && arg[p+1] == 'l' &&
        /* -lib: is used by the C# compiler for libs; it's not an -l
         flag. */
        !string_starts_with(arg.substr(p, std::string::npos), "-lib:")) {
            p += 2;
            while(p<arg.size() && isspace(arg[p]))
                ++p;

            flag.type = LIBS_l;
            flag.arg = l_flag + arg.substr(p, std::string::npos) + lib_suffix;
            pkg->libs.push_back(flag);
        } else if(arg[p] == '-' && arg[p+1] == 'L') {
            p += 2;
            while(p<arg.length() && isspace(arg[p]))
                ++p;

            flag.type = LIBS_L;
            flag.arg = L_flag + arg.substr(p, std::string::npos);
            pkg->libs.push_back(flag);
        } else if((arg.substr(p, std::string::npos) == "-framework" || arg.substr(p, std::string::npos) == "-Wl,-framework") && (i + 1 < args.size())) {
            /* Mac OS X has a -framework Foo which is really one option,
             * so we join those to avoid having -framework Foo
             * -framework Bar being changed into -framework Foo Bar
             * later
             */
            auto tmp = trim_string(args[i + 1]);

            auto framework = strdup_escape_shell(tmp);
            flag.type = LIBS_OTHER;
            flag.arg = arg;
            flag.arg += " ";
            flag.arg += framework;
            pkg->libs.push_back(flag);
            i++;
        } else if(!arg.empty()) {
            flag.type = LIBS_OTHER;
            flag.arg = arg;
            pkg->libs.push_back(flag);
        } else {
            /* flag wasn't used */
        }

        ++i;
    }

}

#if 0
/*
 * This is take from Glib internals.
 */
static inline void
ensure_token (GString **token)
{
  if (*token == NULL)
    *token = g_string_new (NULL);
}

static void
delimit_token (GString **token,
               GSList **retval)
{
  if (*token == NULL)
    return;

  *retval = g_slist_prepend (*retval, g_string_free (*token, FALSE));

  *token = NULL;
}

static GSList*
tokenize_command_line (const gchar *command_line,
                       GError **error)
{
  gchar current_quote;
  const gchar *p;
  GString *current_token = NULL;
  GSList *retval = NULL;
  gboolean quoted;

  current_quote = '\0';
  quoted = FALSE;
  p = command_line;

  while (*p)
    {
      if (current_quote == '\\')
        {
          if (*p == '\n')
            {
              /* we append nothing; backslash-newline become nothing */
            }
          else
            {
              /* we append the backslash and the current char,
               * to be interpreted later after tokenization
               */
              ensure_token (&current_token);
              g_string_append_c (current_token, '\\');
              g_string_append_c (current_token, *p);
            }

          current_quote = '\0';
        }
      else if (current_quote == '#')
        {
          /* Discard up to and including next newline */
          while (*p && *p != '\n')
            ++p;

          current_quote = '\0';

          if (*p == '\0')
            break;
        }
      else if (current_quote)
        {
          if (*p == current_quote &&
              /* check that it isn't an escaped double quote */
              !(current_quote == '"' && quoted))
            {
              /* close the quote */
              current_quote = '\0';
            }

          /* Everything inside quotes, and the close quote,
           * gets appended literally.
           */

          ensure_token (&current_token);
          g_string_append_c (current_token, *p);
        }
      else
        {
          switch (*p)
            {
            case '\n':
              delimit_token (&current_token, &retval);
              break;

            case ' ':
            case '\t':
              /* If the current token contains the previous char, delimit
               * the current token. A nonzero length
               * token should always contain the previous char.
               */
              if (current_token &&
                  current_token->len > 0)
                {
                  delimit_token (&current_token, &retval);
                }

              /* discard all unquoted blanks (don't add them to a token) */
              break;


              /* single/double quotes are appended to the token,
               * escapes are maybe appended next time through the loop,
               * comment chars are never appended.
               */

            case '\'':
            case '"':
              ensure_token (&current_token);
              g_string_append_c (current_token, *p);

              /* FALL THRU */
            case '\\':
              current_quote = *p;
              break;

            case '#':
              if (p == command_line)
            { /* '#' was the first char */
                  current_quote = *p;
                  break;
                }
              switch(*(p-1))
                {
                  case ' ':
                  case '\n':
                  case '\0':
                    current_quote = *p;
                    break;
                  default:
                    ensure_token (&current_token);
                    g_string_append_c (current_token, *p);
            break;
                }
              break;

            default:
              /* Combines rules 4) and 6) - if we have a token, append to it,
               * otherwise create a new token.
               */
              ensure_token (&current_token);
              g_string_append_c (current_token, *p);
              break;
            }
        }

      /* We need to count consecutive backslashes mod 2,
       * to detect escaped doublequotes.
       */
      if (*p != '\\')
    quoted = FALSE;
      else
    quoted = !quoted;

      ++p;
    }

  delimit_token (&current_token, &retval);

  if (current_quote)
    {
      if (current_quote == '\\')
        throw "Text ended just after a “\\” character. (The text was “%s”)";
//                     command_line);
      else
         throw "Text ended before matching quote was found for %c."
                       " (The text was “%s”)";
//                     current_quote, command_line);

      goto error;
    }

  if (retval == NULL)
    {
      throw "Text was empty (or contained only whitespace)";

      goto error;
    }

  /* we appended backward */
  retval = g_slist_reverse (retval);

  return retval;

 error:
  g_assert (error == NULL || *error != NULL);

  g_slist_free_full (retval, g_free);

  return NULL;
}

gboolean
g_shell_parse_argv2 (const gchar *command_line,
                    gint        *argcp,
                    gchar     ***argvp,
                    GError     **error)
{
  /* Code based on poptParseArgvString() from libpopt */
  gint argc = 0;
  gchar **argv = NULL;
  GSList *tokens = NULL;
  gint i;
  GSList *tmp_list;

  g_return_val_if_fail (command_line != NULL, FALSE);

  tokens = tokenize_command_line (command_line, error);
  if (tokens == NULL)
    return FALSE;

  /* Because we can't have introduced any new blank space into the
   * tokens (we didn't do any new expansions), we don't need to
   * perform field splitting. If we were going to honor IFS or do any
   * expansions, we would have to do field splitting on each word
   * here. Also, if we were going to do any expansion we would need to
   * remove any zero-length words that didn't contain quotes
   * originally; but since there's no expansion we know all words have
   * nonzero length, unless they contain quotes.
   *
   * So, we simply remove quotes, and don't do any field splitting or
   * empty word removal, since we know there was no way to introduce
   * such things.
   */

  argc = g_slist_length (tokens);
  argv = g_new0 (gchar*, argc + 1);
  i = 0;
  tmp_list = tokens;
  while (tmp_list)
    {
      argv[i] = g_shell_unquote (static_cast<const char*>(tmp_list->data), error);

      /* Since we already checked that quotes matched up in the
       * tokenizer, this shouldn't be possible to reach I guess.
       */
      if (argv[i] == NULL)
        goto failed;

      tmp_list = g_slist_next (tmp_list);
      ++i;
    }

  g_slist_free_full (tokens, g_free);

  if (argcp)
    *argcp = argc;

  if (argvp)
    *argvp = argv;
  else
    g_strfreev (argv);

  return TRUE;

 failed:

  g_assert (error == NULL || *error != NULL);
  g_strfreev (argv);
  g_slist_free_full (tokens, g_free);

  return FALSE;
}
#endif
static std::vector<std::string> c_arr_to_cpp(int argc, char **argv) {
    std::vector<std::string> res;
    for(int i=0; i<argc; ++i) {
        res.push_back(argv[i]);
    }
    return res;
}

#if 0
static std::vector<std::string> split_arg_string(const std::string &trimmed) {
    int argc;
    char **argv = nullptr;
    GError *error = nullptr;
    if(!g_shell_parse_argv(trimmed.c_str(), &argc, &argv, &error)) {
        throw "Something broke";
    }
    return c_arr_to_cpp(argc, argv);
}

enum QuoteState {
    PLAIN,
    SINGLE,
    DOUBLE,
};

static std::vector<std::string> split_arg_string(const std::string &trimmed) {
    std::vector<std::string> args;

    std::string current;
    QuoteState s = PLAIN;
    for(std::string::size_type i=0; i<trimmed.size(); ++i) {
        switch(s) {
        case PLAIN:
            switch(trimmed[i]) {
            case ' ' : if(!current.empty()) { args.push_back(current); current.clear(); } break;
            case '"' : s = DOUBLE; break;
            case '\'': s = SINGLE; break;
            case '\\' : if(i+1<trimmed.size()) {
                ++i;
                if(trimmed[i] == '\'') {
                    s = SINGLE;
                } else if(trimmed[i] == '"') {
                    s = DOUBLE;
                } else if(trimmed[i] == ' ') {
                    current.push_back(trimmed[i]);
                } else {
                    current.push_back('\'');
                    current.push_back(trimmed[i]);
                }
            } else {
                current.push_back('\'');
            } break;
            default : current.push_back(trimmed[i]); break;
            }
            break;
        case DOUBLE:
            switch(trimmed[i]) {
                default : break;
            }
            break;
        case SINGLE:
            switch(trimmed[i]) {
                default : break;
            }
            break;
        }
    }
//    args.push_back(trimmed.substr(start, end));
    return args;
}
#endif
static void parse_libs(Package *pkg, const std::string &str, const std::string &path) {
    /* Strip out -l and -L flags, put them in a separate list. */
    char **argv = NULL;
    int argc = 0;
    GError *error = NULL;

    if(pkg->libs_num > 0) {
        verbose_error("Libs field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    auto trimmed = trim_and_sub(pkg, str, path);
    if(!trimmed.empty() && !g_shell_parse_argv(trimmed.c_str(), &argc, &argv, &error)) {
        verbose_error("Couldn't parse Libs field into an argument vector: %s\n", error ? error->message : "unknown");        verbose_error("Couldn't parse Libs field into an argument vector\n");
        if(parse_strict)
            exit(1);
        else {
            return;
        }
    }

    auto args = c_arr_to_cpp(argc, argv);

    _do_parse_libs(pkg, args);

    g_strfreev(argv);
    pkg->libs_num++;
}

static void parse_libs_private(Package *pkg, const std::string &str, const std::string &path) {
    /*
     List of private libraries.  Private libraries are libraries which
     are needed in the case of static linking or on platforms not
     supporting inter-library dependencies.  They are not supposed to
     be used for libraries which are exposed through the library in
     question.  An example of an exposed library is GTK+ exposing Glib.
     A common example of a private library is libm.

     Generally, if include another library's headers in your own, it's
     a public dependency and not a private one.
     */
    char **argv = NULL;
    int argc = 0;
    GError *error = NULL;

    if(pkg->libs_private_num > 0) {
        verbose_error("Libs.private field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    auto trimmed = trim_and_sub(pkg, str, path);
    if(!trimmed.empty() && !g_shell_parse_argv(trimmed.c_str(), &argc, &argv, &error)) {
        verbose_error("Couldn't parse Libs.private field into an argument vector: %s\n",
                error ? error->message : "unknown");
        if(parse_strict)
            exit(1);
        else {
            return;
        }
    }

    auto args = c_arr_to_cpp(argc, argv);
    _do_parse_libs(pkg, args);

    g_strfreev(argv);
    pkg->libs_private_num++;
}

std::vector<std::string> parse_shell_string(std::string const& in)
{
    std::vector<std::string> out;
    std::string arg;

    char quoteChar = 0;

    for(auto ch : in) {

        if (quoteChar == '\\') {
            arg.push_back(ch);
            quoteChar = 0;
            continue;
        }

        if (quoteChar && ch != quoteChar) {
            arg.push_back(ch);
            continue;
        }

        switch (ch)
        {
        case '\'':
        case '\"':
        case '\\':
            quoteChar = quoteChar ? 0 : ch;
            break;

        case ' ':
        case '\t':
        case '\n':

            if (!arg.empty()) {
                out.push_back(arg);
                arg.clear();
            }
            break;

        default:
            arg.push_back(ch);
            break;
        }
    }

    if (!arg.empty()) {
        out.push_back(arg);
    }

    return out;
}

static void parse_cflags(Package *pkg, const std::string &str, const std::string &path) {
    /* Strip out -I flags, put them in a separate list. */

    if(!pkg->cflags.empty()) {
        verbose_error("Cflags field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    auto trimmed = trim_and_sub(pkg, str, path);

    auto argv = parse_shell_string(trimmed);
    if(!trimmed.empty() && FALSE) {
        verbose_error("Couldn't parse Cflags field into an argument vector: %s\n", "unknown");
        if(parse_strict)
            exit(1);
        else {
            return;
        }
    }

    for(int i=0; i<(int)argv.size(); ++i) {
        Flag flag;
        auto tmp = trim_string(argv[i].c_str());
        std::string arg = strdup_escape_shell(tmp);
        std::string::size_type p = 0;

        if(p < arg.size()-2 && arg[p] == '-' && arg[p+1] == 'I') {
            p += 2;
            while(p<arg.length() && isspace(arg[p]))
                ++p;

            flag.type = CFLAGS_I;
            flag.arg = std::string("-I") + arg.substr(p, std::string::npos);
            pkg->cflags.push_back(flag);
        } else if((("-idirafter" == arg) || ("-isystem" == arg)) && (i + 1 < (int)argv.size())) {
            auto tmp = trim_string(argv[i + 1].c_str());
            std::string option = strdup_escape_shell(tmp);

            /* These are -I flags since they control the search path */
            flag.type = CFLAGS_I;
            flag.arg = arg;
            flag.arg += " ";
            flag.arg += option;
            pkg->cflags.push_back(flag);
            i++;
        } else if(!arg.empty()) {
            flag.type = CFLAGS_OTHER;
            flag.arg = arg;
            pkg->cflags.push_back(flag);
        } else {
            /* flag wasn't used */
        }
    }

}

static void parse_url(Package *pkg, const std::string &str, const std::string &path) {
    if(!pkg->url.empty()) {
        verbose_error("URL field occurs twice in '%s'\n", path);
        if(parse_strict)
            exit(1);
        else
            return;
    }

    pkg->url = trim_and_sub(pkg, str, path);
}

static void parse_line(Package *pkg, const std::string &untrimmed, const std::string &path, bool ignore_requires,
        bool ignore_private_libs, bool ignore_requires_private) {
    std::string::size_type p;

    debug_spew("  line>%s\n", untrimmed);

    auto str = trim_string(untrimmed);

    if(str.empty()) /* empty line */
    {
        return;
    }

    p=0;

    /* Get first word */
    while((str[p] >= 'A' && str[p] <= 'Z') ||
            (str[p] >= 'a' && str[p] <= 'z') ||
            (str[p] >= '0' && str[p] <= '9') ||
            str[p] == '_' || str[p] == '.')
        p++;

    auto tag = str.substr(0, p);

    while(str.length() < p && isspace(str[p]))
        ++p;

    if(str[p] == ':') {
        /* keyword */
        ++p;
        while(p<str.length() && isspace(str[p]))
            ++p;

        auto remainder = str.substr(p, std::string::npos);
        if(tag == "Name")
            parse_name(pkg, remainder, path);
        else if(tag == "Description")
            parse_description(pkg, remainder, path);
        else if(tag == "Version")
            parse_version(pkg, remainder, path);
        else if(tag == "Requires.private") {
            if(!ignore_requires_private)
                parse_requires_private(pkg, remainder, path);
        } else if(tag == "Requires") {
            if(ignore_requires == false)
                parse_requires(pkg, remainder, path);
            else
                goto cleanup;
        } else if(tag == "Libs.private") {
            if(!ignore_private_libs)
                parse_libs_private(pkg, remainder, path);
        } else if(tag == "Libs")
            parse_libs(pkg, remainder, path);
        else if(tag == "Cflags" || tag == "CFlags")
            parse_cflags(pkg, remainder, path);
        else if(tag == "Conflicts")
            parse_conflicts(pkg, remainder, path);
        else if(tag == "URL")
            parse_url(pkg, remainder, path);
        else {
            /* we don't error out on unknown keywords because they may
             * represent additions to the .pc file format from future
             * versions of pkg-config.  We do make a note of them in the
             * debug spew though, in order to help catch mistakes in .pc
             * files. */
            debug_spew("Unknown keyword '%s' in '%s'\n", tag, path);
        }
    } else if(str[p] == '=') {
        /* variable */
        char *varname;

        ++p;
        while(p<str.length() && isspace(str[p]))
            ++p;

        if(define_prefix && tag == prefix_variable) {
            /* This is the prefix variable. Try to guesstimate a value for it
             * for this package from the location of the .pc file.
             */
            bool is_pkgconfigdir;

            auto base = get_basename(pkg->pcfiledir);
            std::transform(base.begin(), base.end(), base.begin(), ::tolower);
            is_pkgconfigdir = (base == "pkgconfig");
            if(is_pkgconfigdir) {
                /* It ends in pkgconfig. Good. */

                /* Keep track of the original prefix value. */
                pkg->orig_prefix = str.substr(p, std::string::npos);

                /* Get grandparent directory for new prefix. */
                auto prefix = get_dirname(get_dirname(pkg->pcfiledir));

                /* Turn backslashes into slashes or
                 * g_shell_parse_argv() will eat them when ${prefix}
                 * has been expanded in parse_libs().
                 */
                for(std::string::size_type i=0; i<prefix.size(); i++) {
                    if(prefix[i] == '\\')
                        prefix[i] = '/';
                }

                /* Now escape the special characters so that there's no danger
                 * of arguments that include the prefix getting split.
                 */
                std::string prefix_ = strdup_escape_shell(prefix);

                debug_spew(" Variable declaration, '%s' overridden with '%s'\n", tag.c_str(), prefix_.c_str());
                pkg->vars[tag] = prefix_;
                goto cleanup;
            }
        } else if(define_prefix && !pkg->orig_prefix.empty() &&
                strncmp(str.data()+p, pkg->orig_prefix.c_str(), pkg->orig_prefix.length()) == 0 &&
                G_IS_DIR_SEPARATOR (str[p+pkg->orig_prefix.length()])) {
            std::string oldstr = str;

            auto lookup = pkg->vars.find(prefix_variable);
            std::string tmp;
            if(lookup != pkg->vars.end()) {
                tmp = lookup->second;
            }
            str = tmp + str.substr(p + pkg->orig_prefix.length(), std::string::npos);
            p = 0;
        }

        if(pkg->vars.find(tag) != pkg->vars.end()) {
            verbose_error("Duplicate definition of variable '%s' in '%s'\n", tag, path);
            if(parse_strict)
                exit(1);
            else
                goto cleanup;
        }

        varname = g_strdup(tag.c_str());
        auto remainder = str.substr(p, std::string::npos);
        auto varval = trim_and_sub(pkg, remainder, path);

        debug_spew(" Variable declaration, '%s' has value '%s'\n", varname, varval);
        pkg->vars[varname] = varval;

    }

    cleanup:;
}

Package
parse_package_file(const std::string &key, const std::string &path, bool ignore_requires, bool ignore_private_libs,
        bool ignore_requires_private) {
    FILE *f;
    Package pkg;
    std::string str;
    bool one_line = false;

    f = fopen(path.c_str(), "r");

    if(f == NULL) {
        verbose_error("Failed to open '%s': %s\n", path, strerror(errno));

        return Package();
    }

    debug_spew("Parsing package file '%s'\n", path);

    pkg.key = key;

    if(!path.empty()) {
        pkg.pcfiledir = g_dirname(path.c_str());
    } else {
        debug_spew("No pcfiledir determined for package\n");
        pkg.pcfiledir = "???????";
    }

    /* Variable storing directory of pc file */
    pkg.vars["pcfiledir"] = pkg.pcfiledir;

    while(read_one_line(f, str)) {
        one_line = true;

        parse_line(&pkg, str, path.c_str(), ignore_requires, ignore_private_libs, ignore_requires_private);

        str.clear();
    }

    if(!one_line)
        verbose_error("Package file '%s' appears to be empty\n", path);
    fclose(f);

    //pkg->libs = g_list_reverse(pkg->libs);

    return pkg;
}

/* Parse a package variable. When the value appears to be quoted,
 * unquote it so it can be more easily used in a shell. Otherwise,
 * return the raw value.
 */
std::string
parse_package_variable(Package *pkg, const std::string &variable) {
    std::string value;
    std::string result;

    value = package_get_var(pkg, variable);
    if(value.empty())
        return value;

    if(value[0] != '"' && value[0] != '\'')
        /* Not quoted, return raw value */
        return value;

    // FIXME, this is wrong but the test suite does not
    // have any quotes-within-quotes tests so it passes. :)
    for(std::string::size_type i=1; i<value.size()-1; ++i) {
        result.push_back(value[i]);
    }
    return result;
}
