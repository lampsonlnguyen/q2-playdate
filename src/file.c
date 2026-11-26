/* playdate.file for q2-playdate.
 *
 * Path model (see README):
 * - A game-relative path is normalized (leading "/" and "./" stripped,
 *   "." components dropped, ".." components rejected) and resolved
 *   against two roots: the per-game saves directory
 *   (CARD_ROOT/saves/<builtin|game>-<name>) and the bundle directory
 *   (the .pdx folder). Reads check the saves dir first, then the
 *   bundle, matching the SDK's kFileRead rule (Data folder first,
 *   then the pdx). A leading "/" maps to the saves root, so
 *   "/Shared/..." lands in CARD_ROOT/saves/Shared/...
 * - Writes (kFileWrite / kFileAppend), mkdir, delete and rename
 *   operate only in the saves dir; bundles are read-only on device.
 * - Host smoke tests can redirect the saves root with the
 *   Q2_CARD_ROOT environment variable (never set on the device).
 *
 * Limits: relative paths are capped at 240 bytes, each read/readline
 * call at 1 MiB, and 64 simultaneous open files (the SDK limit).
 * File handles are plain C stdio; a closed handle reports
 * nil + "file is closed" instead of crashing.
 */
#define _POSIX_C_SOURCE 200809L

#include "file.h"

#include "card.h"
#include "lauxlib.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_REL_PATH = 240,
    MAX_READ = 1024 * 1024,
    MAX_OPEN_FILES = 64
};

/* SDK FileOptions values (pd_api.h): kFileRead=1, kFileReadData=2,
 * kFileWrite=4, kFileAppend=8. Seek whence values match C SEEK_*:
 * kSeekSet=0, kSeekFromCurrent=1, kSeekFromEnd=2. */
enum { MODE_READ = 1, MODE_READ_DATA = 2, MODE_WRITE = 4, MODE_APPEND = 8 };

static char bundle_dir[1024] = "";
static char saves_dir[512] = "";
static int open_count;

void file_set_bundle(const char *bundle)
{
    const char *root = getenv("Q2_CARD_ROOT");
    if (!root || !*root)
        root = CARD_ROOT;
    snprintf(bundle_dir, sizeof bundle_dir, "%s", bundle ? bundle : "");
    const char *name = bundle ? strrchr(bundle, '/') : NULL;
    snprintf(saves_dir, sizeof saves_dir, "%s/saves/%s-%s", root,
             bundle && strcmp(bundle, CARD_ROOT "/demo.pdx") == 0 ? "builtin" : "game",
             name ? name + 1 : (bundle ? bundle : ""));
}

/* Normalize a game-relative path into `rel` (no leading slash, no
 * empty / "." / ".." components). Returns 0 on success, -1 when the
 * path is empty, too long, or escapes the roots. A lone "/" or "."
 * normalizes to the empty string (the root itself). */
static int normalize_path(const char *given, size_t len, char *rel, size_t size)
{
    if (len > MAX_REL_PATH)
        return -1;
    size_t i = 0, o = 0;
    while (i < len) {
        if (given[i] == '/') {
            i++;
            continue;
        }
        if (given[i] == '.' && i + 1 < len && given[i + 1] == '/') {
            i += 2;
            continue;
        }
        break;
    }
    while (i < len) {
        size_t j = i;
        while (j < len && given[j] != '/')
            j++;
        size_t clen = j - i;
        if (clen == 0) {
            i = j + 1;
            continue;
        }
        if (memchr(given + i, '\0', clen))
            return -1;
        if (clen == 1 && given[i] == '.') {
            i = j + 1;
            continue;
        }
        if (clen == 2 && given[i] == '.' && given[i + 1] == '.')
            return -1;
        if (o > 0) {
            if (o + 1 >= size)
                return -1;
            rel[o++] = '/';
        }
        if (o + clen >= size)
            return -1;
        memcpy(rel + o, given + i, clen);
        o += clen;
        i = j + 1;
    }
    rel[o] = '\0';
    return 0;
}

static int join_path(const char *base, const char *rel, char *out, size_t size)
{
    int n = rel[0] ? snprintf(out, size, "%s/%s", base, rel)
                   : snprintf(out, size, "%s", base);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/* mkdir -p: create every component of a directory path. */
static int mkdir_parents(const char *path)
{
    char work[1536];
    if (strlen(path) >= sizeof work)
        return -1;
    strcpy(work, path);
    for (char *slash = work + 1; *slash; slash++) {
        if (*slash == '/') {
            *slash = '\0';
            if (mkdir(work, 0755) != 0 && errno != EEXIST)
                return -1;
            *slash = '/';
        }
    }
    if (mkdir(work, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* stat a path, saves dir first then bundle. Returns 0 and fills `sb`
 * (and `from_saves`) when the path exists in either root. */
static int stat_lookup(const char *rel, struct stat *sb, int *from_saves)
{
    char path[1600];
    if (join_path(saves_dir, rel, path, sizeof path) == 0 &&
        stat(path, sb) == 0) {
        if (from_saves)
            *from_saves = 1;
        return 0;
    }
    if (join_path(bundle_dir, rel, path, sizeof path) == 0 &&
        stat(path, sb) == 0) {
        if (from_saves)
            *from_saves = 0;
        return 0;
    }
    return -1;
}

/* Resolve a relative path for reading. Returns 0 with the full path
 * in `out`; data_only restricts the search to the saves dir
 * (kFileReadData). */
static int resolve_read(const char *rel, int data_only, char *out, size_t size)
{
    struct stat sb;
    char path[1600];
    if (join_path(saves_dir, rel, path, sizeof path) == 0 &&
        stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
        return snprintf(out, size, "%s", path) < (int)size ? 0 : -1;
    if (data_only)
        return -1;
    if (join_path(bundle_dir, rel, path, sizeof path) == 0 &&
        stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
        return snprintf(out, size, "%s", path) < (int)size ? 0 : -1;
    return -1;
}

/* Resolve a relative path for writing (saves dir only), creating the
 * parent directories as needed. */
static int resolve_write(const char *rel, char *out, size_t size)
{
    if (mkdir_parents(saves_dir) != 0)
        return -1;
    if (join_path(saves_dir, rel, out, size) != 0)
        return -1;
    char *slash = strrchr(out, '/');
    if (slash) {
        *slash = '\0';
        int ok = mkdir_parents(out);
        *slash = '/';
        if (ok != 0)
            return -1;
    }
    struct stat sb;
    if (stat(out, &sb) == 0 && !S_ISREG(sb.st_mode))
        return -1;
    return 0;
}

static int push_path_or_error(lua_State *lua, const char *given, char *rel)
{
    size_t len;
    const char *path = luaL_checklstring(lua, 1, &len);
    if (normalize_path(path, len, rel, 256) != 0) {
        lua_pushboolean(lua, 0);
        return 0;
    }
    (void)given;
    return 1;
}

/* --- playdate.file.file userdata -------------------------------------- */

typedef struct {
    FILE *fp;
} PlayFile;

static PlayFile *check_file(lua_State *lua, int arg)
{
    return luaL_checkudata(lua, arg, "q2.playdate.file");
}

static int file_gc(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (file->fp) {
        fclose(file->fp);
        file->fp = NULL;
        open_count--;
    }
    return 0;
}

static int file_closed(lua_State *lua)
{
    lua_pushnil(lua);
    lua_pushstring(lua, "file is closed");
    return 2;
}

/* read([n]): up to n bytes (rest of file when omitted, capped at
 * 1 MiB per call), returning the string and the byte count. At EOF
 * (no bytes read) returns nil, matching Lua io.read behaviour so
 * read loops terminate. */
static int file_read(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (!file->fp)
        return file_closed(lua);
    lua_Integer wanted = luaL_optinteger(lua, 2, -1);
    if (wanted < -1)
        return luaL_argerror(lua, 2, "negative byte count");
    luaL_Buffer buffer;
    luaL_buffinit(lua, &buffer);
    size_t total = 0;
    char chunk[8192];
    while ((wanted < 0 || total < (size_t)wanted) && total < MAX_READ) {
        size_t want = sizeof chunk;
        if (wanted >= 0 && (size_t)wanted - total < want)
            want = (size_t)wanted - total;
        if (MAX_READ - total < want)
            want = MAX_READ - total;
        size_t got = fread(chunk, 1, want, file->fp);
        if (got == 0)
            break;
        luaL_addlstring(&buffer, chunk, got);
        total += got;
        if (got < want)
            break;
    }
    if (total == 0) {
        if (ferror(file->fp)) {
            lua_pushnil(lua);
            lua_pushstring(lua, strerror(errno));
            return 2;
        }
        lua_pushnil(lua);
        return 1;
    }
    luaL_pushresult(&buffer);
    lua_pushinteger(lua, (lua_Integer)total);
    return 2;
}

/* readline(): next line without its \n or \r\n terminator, nil at EOF. */
static int file_readline(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (!file->fp)
        return file_closed(lua);
    luaL_Buffer buffer;
    luaL_buffinit(lua, &buffer);
    size_t total = 0;
    int c;
    while (total < MAX_READ && (c = fgetc(file->fp)) != EOF) {
        if (c == '\n')
            break;
        if (c == '\r') {
            int next = fgetc(file->fp);
            if (next != '\n' && next != EOF)
                ungetc(next, file->fp);
            break;
        }
        char ch = (char)c;
        luaL_addlstring(&buffer, &ch, 1);
        total++;
    }
    if (total == 0 && c == EOF) {
        if (ferror(file->fp)) {
            lua_pushnil(lua);
            lua_pushstring(lua, strerror(errno));
            return 2;
        }
        lua_pushnil(lua);
        return 1;
    }
    luaL_pushresult(&buffer);
    return 1;
}

static int file_write(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (!file->fp)
        return file_closed(lua);
    size_t len;
    const char *data = luaL_checklstring(lua, 2, &len);
    if (len == 0) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    size_t written = fwrite(data, 1, len, file->fp);
    if (written < len) {
        lua_pushinteger(lua, 0);
        lua_pushstring(lua, strerror(errno));
        return 2;
    }
    lua_pushinteger(lua, (lua_Integer)written);
    return 1;
}

static int file_flush(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (file->fp)
        fflush(file->fp);
    return 0;
}

static int file_seek(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (!file->fp)
        return file_closed(lua);
    lua_Integer offset = luaL_checkinteger(lua, 2);
    int whence = (int)luaL_optinteger(lua, 3, 0);
    int how = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : -1;
    if (how < 0)
        return luaL_argerror(lua, 3, "invalid whence");
    if (fseek(file->fp, (long)offset, how) != 0) {
        lua_pushnil(lua);
        lua_pushstring(lua, strerror(errno));
        return 2;
    }
    return 0;
}

static int file_tell(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (!file->fp)
        return file_closed(lua);
    long pos = ftell(file->fp);
    if (pos < 0) {
        lua_pushnil(lua);
        lua_pushstring(lua, strerror(errno));
        return 2;
    }
    lua_pushinteger(lua, (lua_Integer)pos);
    return 1;
}

static int file_close(lua_State *lua)
{
    PlayFile *file = check_file(lua, 1);
    if (file->fp) {
        fclose(file->fp);
        file->fp = NULL;
        open_count--;
    }
    return 0;
}

static const luaL_Reg file_methods[] = {
    {"read", file_read}, {"readline", file_readline}, {"write", file_write},
    {"flush", file_flush}, {"seek", file_seek}, {"tell", file_tell},
    {"close", file_close}, {NULL, NULL}
};

/* --- playdate.file ----------------------------------------------------- */

static int lfile_open(lua_State *lua)
{
    size_t len;
    const char *given = luaL_checklstring(lua, 1, &len);
    int mode = (int)luaL_optinteger(lua, 2, MODE_READ);
    char rel[256];
    if (normalize_path(given, len, rel, sizeof rel) != 0 || rel[0] == '\0') {
        lua_pushnil(lua);
        lua_pushstring(lua, "invalid path");
        return 2;
    }
    if (open_count >= MAX_OPEN_FILES) {
        lua_pushnil(lua);
        lua_pushstring(lua, "too many open files");
        return 2;
    }
    FILE *fp = NULL;
    if (mode == MODE_WRITE || mode == MODE_APPEND) {
        char path[1600];
        if (resolve_write(rel, path, sizeof path) != 0) {
            lua_pushnil(lua);
            lua_pushstring(lua, strerror(errno));
            return 2;
        }
        fp = fopen(path, mode == MODE_WRITE ? "w" : "a");
    } else if (mode == MODE_READ || mode == MODE_READ_DATA ||
               mode == (MODE_READ | MODE_READ_DATA)) {
        char path[1600];
        if (resolve_read(rel, mode == MODE_READ_DATA, path, sizeof path) != 0) {
            lua_pushnil(lua);
            lua_pushstring(lua, "file does not exist");
            return 2;
        }
        fp = fopen(path, "r");
    } else {
        lua_pushnil(lua);
        lua_pushstring(lua, "invalid file mode");
        return 2;
    }
    if (!fp) {
        lua_pushnil(lua);
        lua_pushstring(lua, strerror(errno));
        return 2;
    }
    PlayFile *file = lua_newuserdatauv(lua, sizeof *file, 0);
    file->fp = fp;
    open_count++;
    luaL_getmetatable(lua, "q2.playdate.file");
    lua_setmetatable(lua, -2);
    return 1;
}

static int lfile_exists(lua_State *lua)
{
    char rel[256];
    if (!push_path_or_error(lua, NULL, rel))
        return 1;
    struct stat sb;
    lua_pushboolean(lua, stat_lookup(rel, &sb, NULL) == 0);
    return 1;
}

static int lfile_isdir(lua_State *lua)
{
    char rel[256];
    if (!push_path_or_error(lua, NULL, rel))
        return 1;
    struct stat sb;
    lua_pushboolean(lua, stat_lookup(rel, &sb, NULL) == 0 && S_ISDIR(sb.st_mode));
    return 1;
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* listFiles([path [, showhidden]]): merged bundle + saves listing,
 * directory names with a trailing "/", hidden files skipped unless
 * showhidden. A missing directory lists as an empty table. */
static int lfile_list_files(lua_State *lua)
{
    int show_hidden = 0;
    char rel[256] = "";
    if (!lua_isnoneornil(lua, 1)) {
        size_t len;
        const char *given = luaL_checklstring(lua, 1, &len);
        if (normalize_path(given, len, rel, sizeof rel) != 0) {
            lua_newtable(lua);
            return 1;
        }
    }
    show_hidden = lua_toboolean(lua, 2);
    char **names = NULL;
    size_t count = 0, capacity = 0;
    const char *roots[2] = {saves_dir, bundle_dir};
    for (int r = 0; r < 2; r++) {
        char path[1600];
        if (join_path(roots[r], rel, path, sizeof path) != 0)
            continue;
        DIR *dir = opendir(path);
        if (!dir)
            continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0')))
                continue;
            if (!show_hidden && name[0] == '.')
                continue;
            char full[1600];
            if (join_path(path, name, full, sizeof full) != 0)
                continue;
            struct stat sb;
            int is_dir = stat(full, &sb) == 0 && S_ISDIR(sb.st_mode);
            size_t need = strlen(name) + (size_t)is_dir + 1;
            char *copy = malloc(need);
            if (!copy)
                continue;
            snprintf(copy, need, "%s%s", name, is_dir ? "/" : "");
            int duplicate = 0;
            for (size_t i = 0; i < count; i++)
                if (strcmp(names[i], copy) == 0) {
                    duplicate = 1;
                    break;
                }
            if (duplicate) {
                free(copy);
                continue;
            }
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 16;
                char **grown = realloc(names, next * sizeof *grown);
                if (!grown) {
                    free(copy);
                    break;
                }
                names = grown;
                capacity = next;
            }
            names[count++] = copy;
        }
        closedir(dir);
    }
    if (count > 1)
        qsort(names, count, sizeof *names, compare_strings);
    lua_newtable(lua);
    for (size_t i = 0; i < count; i++) {
        lua_pushstring(lua, names[i]);
        lua_rawseti(lua, -2, (lua_Integer)i + 1);
        free(names[i]);
    }
    free(names);
    return 1;
}

static int lfile_mkdir(lua_State *lua)
{
    size_t len;
    const char *given = luaL_checklstring(lua, 1, &len);
    char rel[256];
    if (normalize_path(given, len, rel, sizeof rel) != 0 || rel[0] == '\0') {
        lua_pushboolean(lua, 0);
        return 1;
    }
    char path[1600];
    if (join_path(saves_dir, rel, path, sizeof path) != 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    struct stat sb;
    if (stat(path, &sb) == 0) {
        lua_pushboolean(lua, S_ISDIR(sb.st_mode));
        return 1;
    }
    lua_pushboolean(lua, mkdir_parents(path) == 0);
    return 1;
}

static int remove_recursive(const char *path)
{
    struct stat sb;
    if (lstat(path, &sb) != 0)
        return -1;
    if (!S_ISDIR(sb.st_mode))
        return unlink(path);
    DIR *dir = opendir(path);
    if (!dir)
        return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1600];
        if (join_path(path, entry->d_name, child, sizeof child) != 0) {
            result = -1;
            break;
        }
        if (remove_recursive(child) != 0) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    if (result == 0 && rmdir(path) != 0)
        result = -1;
    return result;
}

static int lfile_delete(lua_State *lua)
{
    size_t len;
    const char *given = luaL_checklstring(lua, 1, &len);
    int recursive = lua_toboolean(lua, 2);
    char rel[256];
    if (normalize_path(given, len, rel, sizeof rel) != 0 || rel[0] == '\0') {
        lua_pushboolean(lua, 0);
        return 1;
    }
    char path[1600];
    if (join_path(saves_dir, rel, path, sizeof path) != 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    struct stat sb;
    if (stat(path, &sb) != 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    int ok;
    if (S_ISDIR(sb.st_mode))
        ok = recursive ? remove_recursive(path) == 0 : rmdir(path) == 0;
    else
        ok = unlink(path) == 0;
    lua_pushboolean(lua, ok);
    return 1;
}

static int lfile_get_size(lua_State *lua)
{
    char rel[256];
    if (!push_path_or_error(lua, NULL, rel))
        return 1;
    struct stat sb;
    if (stat_lookup(rel, &sb, NULL) != 0 || !S_ISREG(sb.st_mode)) {
        lua_pushnil(lua);
        lua_pushstring(lua, "file does not exist");
        return 2;
    }
    lua_pushinteger(lua, (lua_Integer)sb.st_size);
    return 1;
}

static int lfile_get_type(lua_State *lua)
{
    char rel[256];
    if (!push_path_or_error(lua, NULL, rel))
        return 1;
    struct stat sb;
    if (stat_lookup(rel, &sb, NULL) != 0) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushstring(lua, S_ISDIR(sb.st_mode) ? "directory" : "file");
    return 1;
}

static int lfile_modtime(lua_State *lua)
{
    char rel[256];
    if (!push_path_or_error(lua, NULL, rel))
        return 1;
    struct stat sb;
    if (stat_lookup(rel, &sb, NULL) != 0) {
        lua_pushnil(lua);
        return 1;
    }
    struct tm broken;
    if (!localtime_r(&sb.st_mtime, &broken)) {
        lua_pushnil(lua);
        return 1;
    }
    lua_newtable(lua);
    lua_pushinteger(lua, broken.tm_year + 1900);
    lua_setfield(lua, -2, "year");
    lua_pushinteger(lua, broken.tm_mon + 1);
    lua_setfield(lua, -2, "month");
    lua_pushinteger(lua, broken.tm_mday);
    lua_setfield(lua, -2, "day");
    lua_pushinteger(lua, broken.tm_hour);
    lua_setfield(lua, -2, "hour");
    lua_pushinteger(lua, broken.tm_min);
    lua_setfield(lua, -2, "minute");
    lua_pushinteger(lua, broken.tm_sec);
    lua_setfield(lua, -2, "second");
    return 1;
}

static int lfile_rename(lua_State *lua)
{
    size_t old_len, new_len;
    const char *old_given = luaL_checklstring(lua, 1, &old_len);
    const char *new_given = luaL_checklstring(lua, 2, &new_len);
    char old_rel[256], new_rel[256];
    if (normalize_path(old_given, old_len, old_rel, sizeof old_rel) != 0 ||
        normalize_path(new_given, new_len, new_rel, sizeof new_rel) != 0 ||
        old_rel[0] == '\0' || new_rel[0] == '\0') {
        lua_pushboolean(lua, 0);
        return 1;
    }
    char old_path[1600], new_path[1600];
    if (join_path(saves_dir, old_rel, old_path, sizeof old_path) != 0 ||
        join_path(saves_dir, new_rel, new_path, sizeof new_path) != 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    /* SDK: rename does not create directories. */
    lua_pushboolean(lua, rename(old_path, new_path) == 0);
    return 1;
}

static const luaL_Reg file_functions[] = {
    {"open", lfile_open}, {"exists", lfile_exists}, {"isdir", lfile_isdir},
    {"listFiles", lfile_list_files}, {"mkdir", lfile_mkdir},
    {"delete", lfile_delete}, {"getSize", lfile_get_size},
    {"getType", lfile_get_type}, {"modtime", lfile_modtime},
    {"rename", lfile_rename}, {NULL, NULL}
};

/* --- playdate.metadata -------------------------------------------------- */

/* The bundle's pdxinfo file, parsed as key=value lines into a table.
 * Missing pdxinfo yields an empty table, never nil, so games can
 * index playdate.metadata.bundleID unconditionally. */
static void install_metadata(lua_State *lua)
{
    lua_newtable(lua);
    char path[1200];
    if (join_path(bundle_dir, "pdxinfo", path, sizeof path) == 0) {
        FILE *info = fopen(path, "r");
        if (info) {
            char line[512];
            while (fgets(line, sizeof line, info)) {
                char *equals = strchr(line, '=');
                if (!equals || equals == line)
                    continue;
                *equals = '\0';
                char *value = equals + 1;
                size_t length = strlen(value);
                while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r'))
                    value[--length] = '\0';
                lua_pushstring(lua, value);
                lua_setfield(lua, -2, line);
            }
            fclose(info);
        }
    }
    lua_setfield(lua, -2, "metadata");
}

void file_install(lua_State *lua)
{
    luaL_newmetatable(lua, "q2.playdate.file");
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    luaL_setfuncs(lua, file_methods, 0);
    lua_pushcfunction(lua, file_gc);
    lua_setfield(lua, -2, "__gc");
    lua_pop(lua, 1);

    luaL_newlib(lua, file_functions);
    lua_pushinteger(lua, MODE_READ);
    lua_setfield(lua, -2, "kFileRead");
    lua_pushinteger(lua, MODE_READ_DATA);
    lua_setfield(lua, -2, "kFileReadData");
    lua_pushinteger(lua, MODE_WRITE);
    lua_setfield(lua, -2, "kFileWrite");
    lua_pushinteger(lua, MODE_APPEND);
    lua_setfield(lua, -2, "kFileAppend");
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kSeekSet");
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "kSeekFromCurrent");
    lua_pushinteger(lua, 2);
    lua_setfield(lua, -2, "kSeekFromEnd");
    luaL_newlib(lua, file_methods);
    lua_setfield(lua, -2, "file");
    lua_setfield(lua, -2, "file");

    install_metadata(lua);
}
