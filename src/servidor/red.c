#include "servidor.h"
 
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
 
int red_crear_escucha(const config_servidor_t *config) {
    struct addrinfo hints, *resultado, *rp;
    char puerto_str[6]; /* "65535" + '\0' */
    int fd_escucha = -1;
    int rc;
 
    snprintf(puerto_str, sizeof(puerto_str), "%u", (unsigned)config->puerto);
 
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    const char *nodo = (config->direccion[0] != '\0') ? config->direccion : NULL;
 
    rc = getaddrinfo(nodo, puerto_str, &hints, &resultado);
    if (rc != 0) {
        fprintf(stderr, "Error: dirección/puerto inválidos (%s): %s\n",
                config->direccion[0] ? config->direccion : "(todas)",
                gai_strerror(rc));
        return -1;
    }
 
    for (rp = resultado; rp != NULL; rp = rp->ai_next) {
        int reusar = 1;
 
        fd_escucha = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd_escucha < 0) {
            continue;
        }

        if (setsockopt(fd_escucha, SOL_SOCKET, SO_REUSEADDR,
                        &reusar, sizeof(reusar)) < 0) {
            fprintf(stderr, "Aviso: no se pudo fijar SO_REUSEADDR: %s\n",
                    strerror(errno));
        }
 
        if (bind(fd_escucha, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd_escucha);
        fd_escucha = -1;
    }
 
    freeaddrinfo(resultado);
 
    if (fd_escucha < 0) {
        fprintf(stderr, "Error: no se pudo enlazar el socket en el puerto %u: %s\n",
                (unsigned)config->puerto, strerror(errno));
        return -1;
    }
 
    if (listen(fd_escucha, config->backlog) < 0) {
        fprintf(stderr, "Error: listen() falló: %s\n", strerror(errno));
        close(fd_escucha);
        return -1;
    }
 
    return fd_escucha;
}