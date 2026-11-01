#ifndef CATALOG_H
#define CATALOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Representa un archivo dentro del catálogo.
typedef struct {
    const char *name;
    uint64_t size_bytes;
    bool available;
} catalog_entry_t;

// Declaración opaca del catálogo completo.
typedef struct catalog catalog_t;

// Resultados de las operaciones del catálogo.
typedef enum {
    CATALOG_OK = 0,
    CATALOG_INVALID,
    CATALOG_IO_ERROR,
    CATALOG_NO_MEMORY,
    CATALOG_NOT_FOUND,
    CATALOG_NOT_SERVABLE
} catalog_result_t;

// Crear el catálogo a partir de un directorio.
catalog_result_t catalog_create(
    const char *directory,
    catalog_t **out
);

// Consultar la cantidad de entradas.
size_t catalog_count(
    const catalog_t *catalog
);

// Obtener una entrada por su índice.
const catalog_entry_t *catalog_at(
    const catalog_t *catalog,
    size_t index
);

// Buscar una entrada por nombre.
const catalog_entry_t *catalog_find(
    const catalog_t *catalog,
    const char *name
);

// Abrir de forma segura un archivo catalogado.
catalog_result_t catalog_open(
    const catalog_t *catalog,
    const char *name,
    int *fd_out,
    uint64_t *size_out
);

// Liberar los recursos del catálogo.
void catalog_destroy(
    catalog_t *catalog
);

#endif // CATALOG_H

