#define _POSIX_C_SOURCE 200809L

#include "catalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void test_directorio_vacio(void) {
    char ruta[] = "/tmp/catalog_test_XXXXXX";

    // Crear un directorio temporal vacío.
    assert(mkdtemp(ruta) != NULL);

    catalog_t *catalog = NULL;

    // Inicializar el catálogo.
    catalog_result_t result = catalog_create(ruta, &catalog);

    // Verificar los resultados.
    assert(result == CATALOG_OK);
    assert(catalog != NULL);
    assert(catalog_count(catalog) == 0);

    // Liberar recursos.
    catalog_destroy(catalog);
    assert(rmdir(ruta) == 0);

    printf("[OK] Directorio vacío\n");
}

static void test_video_real(void) {
    catalog_t *catalog = NULL;

    // Crear catálogo con los archivos de videos/
    assert(catalog_create("videos", &catalog) == CATALOG_OK);

    printf("\nVideos encontrados: %zu\n", catalog_count(catalog));

    // Mostrar todas las entradas.
    for (size_t i = 0; i < catalog_count(catalog); i++) {
        const catalog_entry_t *entry = catalog_at(catalog, i);

        printf("Nombre: %s\n", entry->name);
        printf("Tamaño: %llu bytes\n",
               (unsigned long long)entry->size_bytes);
        printf("Disponible: %s\n\n",
               entry->available ? "Sí" : "No");
    }

    // Comprobar que encontró el video.
    const catalog_entry_t *video =
        catalog_find(catalog, "flower.mp4");

    assert(video != NULL);
    assert(video->available);
    assert(video->size_bytes > 0);

    // Verificar que el video puede abrirse.
    int fd = -1;
    uint64_t size = 0;

    assert(catalog_open(catalog, "flower.mp4", &fd, &size)
           == CATALOG_OK);

    assert(size == video->size_bytes);
    close(fd);

    // Comprobar que ignora el archivo de texto.
    assert(catalog_find(catalog, "notas.txt") == NULL);
    
    int fd_inexistente = -1;
    uint64_t size_inexistente = 0;

    assert(catalog_find(catalog, "noexiste.mp4") == NULL);

    assert(catalog_open(catalog, "noexiste.mp4",
                    &fd_inexistente,
                    &size_inexistente) == CATALOG_NOT_FOUND);

    assert(fd_inexistente == -1);

    catalog_destroy(catalog);

    printf("[OK] Video real reconocido correctamente\n");
}


static void test_archivo_eliminado(void) {
    char ruta[] = "/tmp/catalog_deleted_XXXXXX";
    assert(mkdtemp(ruta) != NULL);

    char archivo[512];
    snprintf(archivo, sizeof(archivo),
             "%s/prueba.mp4", ruta);

    // Crear un archivo de prueba.
    FILE *f = fopen(archivo, "wb");
    assert(f != NULL);
    assert(fputs("video de prueba", f) >= 0);
    assert(fclose(f) == 0);

    // Construir el catálogo.
    catalog_t *catalog = NULL;
    assert(catalog_create(ruta, &catalog) == CATALOG_OK);
    assert(catalog_count(catalog) == 1);

    // Eliminar el archivo después del escaneo.
    assert(unlink(archivo) == 0);

    int fd = -1;
    uint64_t size = 0;

    // Debe reconocer que ya no es servible.
    assert(catalog_open(catalog, "prueba.mp4",
                        &fd, &size) == CATALOG_NOT_SERVABLE);
    assert(fd == -1);

    catalog_destroy(catalog);
    assert(rmdir(ruta) == 0);

    printf("[OK] Archivo eliminado detectado\n");
}

static void test_enlace_simbolico(void) {
    char ruta[] = "/tmp/catalog_symlink_XXXXXX";
    assert(mkdtemp(ruta) != NULL);

    char enlace[512];
    snprintf(enlace, sizeof(enlace),
             "%s/externo.mp4", ruta);

    // Intentar presentar un archivo externo como video.
    assert(symlink("/etc/passwd", enlace) == 0);

    catalog_t *catalog = NULL;
    assert(catalog_create(ruta, &catalog) == CATALOG_OK);

    // No debe registrar el enlace.
    assert(catalog_count(catalog) == 0);
    assert(catalog_find(catalog, "externo.mp4") == NULL);

    catalog_destroy(catalog);

    assert(unlink(enlace) == 0);
    assert(rmdir(ruta) == 0);

    printf("[OK] Enlace simbólico rechazado\n");
}

int main(void) {
    test_directorio_vacio();
    test_video_real();
    test_archivo_eliminado();
    test_enlace_simbolico();

    printf("\nTodas las pruebas pasaron.\n");
    return 0;
}



