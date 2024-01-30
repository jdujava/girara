/* SPDX-License-Identifier: Zlib */

#include <ctype.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"
#include "datastructures.h"
#include "session.h"
#include "settings.h"
#include "internal.h"

char*
girara_fix_path(const char* path)
{
  if (path == NULL) {
    return NULL;
  }

  char* rpath = NULL;
  if (path[0] == '~') {
    const size_t len = strlen(path);
    char* user = NULL;
    size_t idx = 1;

    if (len > 1 && path[1] != '/') {
      while (path[idx] && path[idx] != '/') {
        ++idx;
      }

      user = g_strndup(path + 1, idx - 1);
    }

    char* home_path = girara_get_home_directory(user);
    g_free(user);

    if (home_path == NULL) {
      return g_strdup(path);
    }

    rpath = g_build_filename(home_path, path + idx, NULL);
    g_free(home_path);
  } else if (g_path_is_absolute(path) == TRUE) {
    rpath = g_strdup(path);
  } else {
    char* curdir = g_get_current_dir();
    rpath = g_build_filename(curdir, path, NULL);
    g_free(curdir);
  }

  return rpath;
}

bool
girara_xdg_open_with_working_directory(const char* uri, const char* working_directory)
{
  if (uri == NULL || strlen(uri) == 0) {
    return false;
  }

  /* g_spawn_async expects char** */
  static char xdg_open[] = "xdg-open";
  char* argv[] = { xdg_open, g_strdup(uri), NULL };

  GError* error = NULL;
  bool res = g_spawn_async(working_directory, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
      NULL, NULL, &error);
  if (error != NULL) {
    girara_warning("Failed to execute 'xdg-open %s': %s", uri, error->message);
    g_error_free(error);
    error = NULL;
  }

  if (res == false) {
    /* fall back to `gio open` */
    char* current_dir = working_directory != NULL ? g_get_current_dir() : NULL;
    if (working_directory != NULL) {
      g_chdir(working_directory);
    }

    res = g_app_info_launch_default_for_uri(uri, NULL, &error);
    if (error != NULL) {
      girara_warning("Failed to open '%s': %s", uri, error->message);
      g_error_free(error);
    }

    if (working_directory != NULL) {
      g_chdir(current_dir);
      g_free(current_dir);
    }
  }

  g_free(argv[1]);

  return res;
}

bool
girara_xdg_open(const char* uri)
{
  return girara_xdg_open_with_working_directory(uri, NULL);
}

#if defined(HAVE_GETPWNAM_R)
static char*
get_home_directory_getpwnam(const char* user)
{
#ifdef _SC_GETPW_R_SIZE_MAX
  int bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufsize < 0) {
    bufsize = 4096;
  }
#else
  const int bufsize = 4096;
#endif

  char* buffer = g_try_malloc0(sizeof(char) * bufsize);
  if (buffer == NULL) {
    return NULL;
  }

  struct passwd pwd;
  struct passwd* result = NULL;
  if (getpwnam_r(user, &pwd, buffer, bufsize, &result) != 0) {
    g_free(buffer);
    return NULL;
  }

  char* dir = g_strdup(pwd.pw_dir);
  g_free(buffer);
  return dir;
}
#else
static char*
get_home_directory_getpwnam(const char* user)
{
  const struct passwd* pwd = getpwnam(user);
  if (pwd != NULL) {
    return g_strdup(pwd->pw_dir);
  }

  return NULL;
}
#endif

char*
girara_get_home_directory(const char* user)
{
  if (user == NULL || g_strcmp0(user, g_get_user_name()) == 0) {
    return g_strdup(g_get_home_dir());
  }

  return get_home_directory_getpwnam(user);
}

char*
girara_get_xdg_path(girara_xdg_path_t path)
{
  static const char VARS[][16] = {
    [XDG_CONFIG_DIRS] = "XDG_CONFIG_DIRS",
    [XDG_DATA_DIRS] = "XDG_DATA_DIRS"
  };

  static const char DEFAULTS[][29] = {
    [XDG_CONFIG_DIRS] = "/etc/xdg",
    [XDG_DATA_DIRS] = "/usr/local/share/:/usr/share"
  };

  switch (path) {
    case XDG_DATA:
      return g_strdup(g_get_user_data_dir());
    case XDG_CONFIG:
      return g_strdup(g_get_user_config_dir());
    case XDG_CONFIG_DIRS:
    case XDG_DATA_DIRS:
    {
      const char* tmp = g_getenv(VARS[path]);
      if (tmp == NULL || !g_strcmp0(tmp, "")) {
        return g_strdup(DEFAULTS[path]);
      }
      return g_strdup(tmp);
    }
    case XDG_CACHE:
      return g_strdup(g_get_user_cache_dir());
  }

  return NULL;
}

girara_list_t* girara_split_path_array(const char* patharray) {
  if (patharray == NULL || !g_strcmp0(patharray, "")) {
    return NULL;
  }

  girara_list_t* res = girara_list_new_with_free(g_free);
  char** paths       = g_strsplit(patharray, ":", 0);
  for (size_t i = 0; paths[i] != NULL; ++i) {
    girara_list_append(res, g_strdup(paths[i]));
  }
  g_strfreev(paths);

  return res;
}

FILE*
girara_file_open(const char* path, const char* mode)
{
  if (path == NULL || mode == NULL) {
    return NULL;
  }

  char* fixed_path = girara_fix_path(path);
  if (fixed_path == NULL) {
    return NULL;
  }

  FILE* fp = fopen(fixed_path, mode);
  g_free(fixed_path);
  if (fp  == NULL) {
    return NULL;
  }

  return fp;
}

#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)
char*
girara_file_read_line(FILE* file)
{
  if (file == NULL) {
    return NULL;
  }

  size_t size = 0;
  char* line = fgetln(file, &size);
  if (line  == NULL) {
    return NULL;
  }

  char* copy = g_strndup(line, size);
  if (copy == NULL) {
    return NULL;
  }

  /* remove the trailing line deliminator */
  g_strdelimit(copy, "\n\r", '\0');

  return copy;
}
#else
char*
girara_file_read_line(FILE* file)
{
  if (file == NULL) {
    return NULL;
  }

  size_t size = 0;
  char* line = NULL;
  if (getline(&line, &size, file) == -1) {
    if (line != NULL) {
      free(line);
    }
    return NULL;
  }

  /* remove the trailing line deliminator */
  g_strdelimit(line, "\n\r", '\0');

  char* duplicate = g_strdup(line);
  free(line);
  return duplicate;
}
#endif

char*
girara_file_read(const char* path)
{
  if (path == NULL) {
    return NULL;
  }

  FILE* file = girara_file_open(path, "r");
  if (file == NULL) {
    return NULL;
  }

  char* content = girara_file_read2(file);
  fclose(file);
  return content;
}

char*
girara_file_read2(FILE* file)
{
  if (file == NULL) {
    return NULL;
  }

  const off_t curpos = ftello(file);
  if (curpos == -1) {
    return NULL;
  }

  fseeko(file, 0, SEEK_END);
  const off_t size = ftello(file) - curpos;
  fseeko(file, curpos, SEEK_SET);

  if (size == 0) {
    return g_try_malloc0(1);
  }

  /* this can happen on 32 bit systems */
  if ((uintmax_t)size >= (uintmax_t)SIZE_MAX) {
    girara_error("file is too large");
    return NULL;
  }

  char* buffer = g_try_malloc(size + 1);
  if (buffer == NULL) {
    return NULL;
  }

  size_t read = fread(buffer, size, 1, file);
  if (read != 1) {
    free(buffer);
    return NULL;
  }

  buffer[size] = '\0';
  return buffer;
}

void girara_clean_line(char* line) {
  if (line == NULL) {
    return;
  }

  size_t i     = 0;
  size_t j     = 0;
  bool ws_mode = true;

  for (i = 0; i < strlen(line); ++i) {
    if (isspace(line[i]) != 0) {
      if (ws_mode == true) {
        continue;
      }

      line[j++] = ' ';
      ws_mode   = true;
    } else {
      line[j++] = line[i];
      ws_mode   = false;
    }
  }

  line[j] = '\0';
}

char* girara_escape_string(const char* value) {
  if (value == NULL) {
    return NULL;
  }

  GString* str = g_string_new("");
  while (*value != '\0') {
    const char c = *value++;
    if (strchr("\\ \t\"\'#", c) != NULL) {
      g_string_append_c(str, '\\');
    }
    g_string_append_c(str, c);
  }

  return g_string_free(str, FALSE);
}

char*
girara_replace_substring(const char* string, const char* old, const char* new)
{
  if (string == NULL || old == NULL || new == NULL) {
    return NULL;
  }

  if (*string == '\0' || *old == '\0' || strstr(string, old) == NULL) {
    return g_strdup(string);
  }

  gchar** split = g_strsplit(string, old, -1);
  char* ret = g_strjoinv(new, split);
  g_strfreev(split);

  return ret;
}

bool
girara_exec_with_argument_list(girara_session_t* session, girara_list_t* argument_list)
{
  if (session == NULL || argument_list == NULL) {
    return false;
  }

  char* cmd = NULL;
  girara_setting_get(session, "exec-command", &cmd);
  if (cmd == NULL || strlen(cmd) == 0) {
    girara_debug("exec-command is empty, executing directly.");
    g_free(cmd);
    cmd = NULL;
  }

  bool dont_append_first_space = cmd == NULL;
  GString* command             = g_string_new(cmd ? cmd : "");
  g_free(cmd);

  for (size_t idx = 0; idx != girara_list_size(argument_list); ++idx) {
    if (dont_append_first_space == false) {
      g_string_append_c(command, ' ');
    }
    dont_append_first_space = false;
    char* tmp               = g_shell_quote(girara_list_nth(argument_list, idx));
    g_string_append(command, tmp);
    g_free(tmp);
  };

  GError* error = NULL;
  girara_info("executing: %s", command->str);
  gboolean ret = g_spawn_command_line_async(command->str, &error);
  if (error != NULL) {
    girara_warning("Failed to execute command: %s", error->message);
    girara_notify(session, GIRARA_ERROR, _("Failed to execute command: %s"), error->message);
    g_error_free(error);
  }

  g_string_free(command, TRUE);

  return ret;
}

void
widget_add_class(GtkWidget* widget, const char* styleclass)
{
  if (widget == NULL || styleclass == NULL) {
    return;
  }

  GtkStyleContext* context = gtk_widget_get_style_context(widget);
  if (gtk_style_context_has_class(context, styleclass) == FALSE) {
    gtk_style_context_add_class(context, styleclass);
  }
}

void
widget_remove_class(GtkWidget* widget, const char* styleclass)
{
  if (widget == NULL || styleclass == NULL) {
    return;
  }

  GtkStyleContext* context = gtk_widget_get_style_context(widget);
  if (gtk_style_context_has_class(context, styleclass) == TRUE) {
    gtk_style_context_remove_class(context, styleclass);
  }
}

const char*
girara_version(void)
{
  return GIRARA_VERSION;
}

int
list_strcmp(const void* data1, const void* data2)
{
  const char* str1 = data1;
  const char* str2 = data2;

  return g_strcmp0(str1, str2);
}
