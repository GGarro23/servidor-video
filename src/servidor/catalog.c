#define _POSIX_C_SOURCE 200809L

#include "catalog.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* El protocolo v1 admite identificadores de hasta 255 bytes. */
#define CATALOG_MAX_NAME_BYTES 255u

typedef struct {
    catalog_entry_t entry;
    struct stat original_stat;
} catalog_record_t;

struct catalog {
    int directory_fd;
    catalog_record_t *records;
    size_t count;
    size_t capacity;
};

/* Validación estricta de UTF-8 para los nombres que enviaremos al cliente. */
static bool valid_utf8(const unsigned char *text, size_t length) {
    for (size_t i = 0; i < length;) {
        unsigned char first = text[i];
        size_t extra;
        uint32_t codepoint;

        if (first <= 0x7F) {
            i++;
            continue;
        }
        if (first >= 0xC2 && first <= 0xDF) {
            extra = 1;
            codepoint = first & 0x1Fu;
        } else if (first >= 0xE0 && first <= 0xEF) {
            extra = 2;
            codepoint = first & 0x0Fu;
        } else if (first >= 0xF0 && first <= 0xF4) {
            extra = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }

        if (extra > length - i - 1) {
            return false;
        }
        for (size_t j = 1; j <= extra; j++) {
            unsigned char part = text[i + j];
            if ((part & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6) | (part & 0x3Fu);
        }

        if ((extra == 1 && codepoint < 0x80u) ||
            (extra == 2 && codepoint < 0x800u) ||
            (extra == 3 && codepoint < 0x10000u) ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint > 0x10FFFFu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

static bool is_video_extension(const char *name) {
    static const char *const extensions[] = {
        ".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".mpeg", ".mpg"
    };
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (strcasecmp(dot, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool valid_catalog_name(const char *name) {
    if (name == NULL || name[0] == '.') {
        return false;  /* También excluye . y .., y archivos ocultos. */
    }

    size_t length = strlen(name);
    if (length == 0 || length > CATALOG_MAX_NAME_BYTES ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strstr(name, "..") != NULL) {
        return false;
    }

    return valid_utf8((const unsigned char *)name, length) &&
           is_video_extension(name);
}

static bool same_file(const struct stat *before, const struct stat *now) {
    return S_ISREG(now->st_mode) && now->st_size >= 0 &&
           before->st_dev == now->st_dev &&
           before->st_ino == now->st_ino &&
           before->st_size == now->st_size &&
           before->st_mtim.tv_sec == now->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == now->st_mtim.tv_nsec;
}

static catalog_result_t append_record(catalog_t *catalog,
                                      const char *name,
                                      const struct stat *st,
                                      bool available) {
    if (catalog->count == catalog->capacity) {
        size_t new_capacity = catalog->capacity == 0 ? 8 : catalog->capacity * 2;
        if (new_capacity < catalog->capacity ||
            new_capacity > SIZE_MAX / sizeof(*catalog->records)) {
            return CATALOG_NO_MEMORY;
        }

        catalog_record_t *new_records = realloc(
            catalog->records, new_capacity * sizeof(*new_records));
        if (new_records == NULL) {
            return CATALOG_NO_MEMORY;
        }
        catalog->records = new_records;
        catalog->capacity = new_capacity;
    }

    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return CATALOG_NO_MEMORY;
    }
    memcpy(copy, name, length + 1);

    catalog_record_t *record = &catalog->records[catalog->count];
    record->entry.name = copy;
    record->entry.size_bytes = st != NULL && st->st_size >= 0
        ? (uint64_t)st->st_size : 0;
    record->entry.available = available;
    record->original_stat = st != NULL ? *st : (struct stat){0};
    catalog->count++;

    return CATALOG_OK;
}

static int compare_records(const void *left, const void *right) {
    const catalog_record_t *a = left;
    const catalog_record_t *b = right;
    return strcmp(a->entry.name, b->entry.name);
}

catalog_result_t catalog_create(const char *directory, catalog_t **out) {
    if (out == NULL) {
        return CATALOG_INVALID;
    }
    *out = NULL;
    if (directory == NULL || directory[0] == '\0') {
        return CATALOG_INVALID;
    }

    catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (catalog == NULL) {
        return CATALOG_NO_MEMORY;
    }
    catalog->directory_fd = -1;

    catalog->directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (catalog->directory_fd < 0) {
        catalog_destroy(catalog);
        return CATALOG_IO_ERROR;
    }

    int scan_fd = dup(catalog->directory_fd);
    if (scan_fd < 0) {
        catalog_destroy(catalog);
        return CATALOG_IO_ERROR;
    }

    DIR *dir = fdopendir(scan_fd);
    if (dir == NULL) {
        close(scan_fd);
        catalog_destroy(catalog);
        return CATALOG_IO_ERROR;
    }

    catalog_result_t result = CATALOG_OK;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(dir);
        if (item == NULL) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != 0) {
                result = CATALOG_IO_ERROR;
            }
            break;
        }

        if (!valid_catalog_name(item->d_name)) {
            continue;
        }

        struct stat st;
        if (fstatat(catalog->directory_fd, item->d_name,
                    &st, AT_SYMLINK_NOFOLLOW) < 0) {
            fprintf(stderr, "[catalogo] no se pudo consultar '%s'; no servible\n",
                    item->d_name);
            result = append_record(catalog, item->d_name, NULL, false);
        } else if (S_ISREG(st.st_mode)) {
            bool available = false;
            if (st.st_size >= 0) {
                /* Evita seguir enlaces y bloquearse si se reemplaza por un FIFO. */
                int fd = openat(catalog->directory_fd, item->d_name,
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                if (fd >= 0) {
                    struct stat current;
                    available = fstat(fd, &current) == 0 && same_file(&st, &current);
                    close(fd);
                }
            }

            if (!available) {
                fprintf(stderr, "[catalogo] '%s' no se puede servir\n",
                        item->d_name);
            }
            result = append_record(catalog, item->d_name, &st, available);
        } else {
            /* Excluye subdirectorios, enlaces simbólicos y archivos especiales. */
            continue;
        }

        if (result != CATALOG_OK) {
            break;
        }
    }

    if (closedir(dir) < 0 && result == CATALOG_OK) {
        result = CATALOG_IO_ERROR;
    }
    if (result != CATALOG_OK) {
        catalog_destroy(catalog);
        return result;
    }

    if (catalog->count > 1) {
        qsort(catalog->records, catalog->count, sizeof(*catalog->records),
              compare_records);
    }
    *out = catalog;
    return CATALOG_OK;
}

size_t catalog_count(const catalog_t *catalog) {
    return catalog == NULL ? 0 : catalog->count;
}

const catalog_entry_t *catalog_at(const catalog_t *catalog, size_t index) {
    if (catalog == NULL || index >= catalog->count) {
        return NULL;
    }
    return &catalog->records[index].entry;
}

static const catalog_record_t *find_record(const catalog_t *catalog,
                                           const char *name) {
    if (catalog == NULL || name == NULL) {
        return NULL;
    }

    /* Búsqueda binaria: catalog_create() ordena las entradas por nombre. */
    size_t low = 0;
    size_t high = catalog->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int comparison = strcmp(name, catalog->records[mid].entry.name);
        if (comparison == 0) {
            return &catalog->records[mid];
        }
        if (comparison < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return NULL;
}

const catalog_entry_t *catalog_find(const catalog_t *catalog,
                                    const char *name) {
    const catalog_record_t *record = find_record(catalog, name);
    return record == NULL ? NULL : &record->entry;
}

catalog_result_t catalog_open(const catalog_t *catalog, const char *name,
                              int *fd_out, uint64_t *size_out) {
    if (fd_out == NULL || size_out == NULL) {
        return CATALOG_INVALID;
    }
    *fd_out = -1;
    *size_out = 0;
    if (catalog == NULL || name == NULL) {
        return CATALOG_INVALID;
    }

    const catalog_record_t *record = find_record(catalog, name);
    if (record == NULL) {
        return CATALOG_NOT_FOUND;
    }
    if (!record->entry.available) {
        return CATALOG_NOT_SERVABLE;
    }

    int fd = openat(catalog->directory_fd, record->entry.name,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return CATALOG_NOT_SERVABLE;
    }

    struct stat current;
    if (fstat(fd, &current) < 0 ||
        !same_file(&record->original_stat, &current)) {
        close(fd);
        return CATALOG_NOT_SERVABLE;
    }

    *fd_out = fd;  /* El llamador adquiere la propiedad de este descriptor. */
    *size_out = (uint64_t)current.st_size;
    return CATALOG_OK;
}

void catalog_destroy(catalog_t *catalog) {
    if (catalog == NULL) {
        return;
    }
    for (size_t i = 0; i < catalog->count; i++) {
        free((void *)catalog->records[i].entry.name);
    }
    free(catalog->records);
    if (catalog->directory_fd >= 0) {
        close(catalog->directory_fd);
    }
    free(catalog);
}
