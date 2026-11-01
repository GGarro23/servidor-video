#define _POSIX_C_SOURCE 200809L
#include "vod_control.h"
#include "protocol.h"
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ID_VOD 73u
#define ID_CERRAR 99u
#define ID_EQUIVOCADO 74u

typedef enum { T_COMPLETO, T_CANCELAR, T_CERRAR, T_ID_MALO, T_FRAGMENTADO, T_CANCELAR_DURANTE } prueba_t;

static void enviar_control(int fd, uint8_t tipo, uint32_t id, bool fragmentar) {
    protocol_header_t h = { .version=PROTOCOL_VERSION, .type=tipo,
        .status=PROTOCOL_STATUS_EXITO, .request_id=id, .payload_length=0 };
    if (!fragmentar) {
        assert(protocol_send_message(fd, &h, NULL) == PROTOCOL_OK);
        return;
    }
    uint8_t cabecera[PROTOCOL_HEADER_SIZE];
    assert(protocol_encode_header(&h, cabecera) == PROTOCOL_OK);
    assert(send(fd, cabecera, 5, MSG_NOSIGNAL) == 5);
    struct timespec d = {.tv_sec=0, .tv_nsec=100000000};
    nanosleep(&d, NULL);
    assert(send(fd, cabecera+5, 7, MSG_NOSIGNAL) == 7);
}

static void probar(prueba_t tipo, size_t tam) {
    char ruta[] = "/tmp/vod-control-XXXXXX";
    int f = mkstemp(ruta);
    assert(f >= 0);
    uint8_t dato[4096];
    for (size_t i=0; i<sizeof dato; i++) dato[i]=(uint8_t)(i%241);
    for (size_t falta=tam; falta;) {
        size_t n=falta<sizeof dato? falta:sizeof dato;
        assert(write(f,dato,n)==(ssize_t)n);
        falta-=n;
    }
    close(f);

    int socks[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, socks) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(socks[0]);
        if (tipo==T_CANCELAR_DURANTE) {
            int poco=1024;
            assert(setsockopt(socks[1],SOL_SOCKET,SO_SNDBUF,&poco,sizeof(poco))==0);
        }
        int fd = open(ruta,O_RDONLY);
        assert(fd >= 0);
        vod_transfer_t t={.fd_archivo=fd, .tamano=tam, .request_id=ID_VOD};
        vod_control_result_t r=vod_control_servir(socks[1],&t);
        if (tipo==T_COMPLETO || tipo==T_ID_MALO) assert(r==VOD_CONTROL_COMPLETADO);
        if (tipo==T_CANCELAR || tipo==T_FRAGMENTADO || tipo==T_CANCELAR_DURANTE) assert(r==VOD_CONTROL_CANCELADO);
        if (tipo==T_CERRAR) assert(r==VOD_CONTROL_CERRAR);
        close(fd); close(socks[1]); _exit(0);
    }
    close(socks[1]);
    if (tipo==T_CANCELAR || tipo==T_FRAGMENTADO)
        enviar_control(socks[0],PROTOCOL_MSG_CANCELAR,ID_VOD,tipo==T_FRAGMENTADO);
    if (tipo==T_CERRAR)
        enviar_control(socks[0],PROTOCOL_MSG_CERRAR,ID_CERRAR,false);
    if (tipo==T_ID_MALO)
        enviar_control(socks[0],PROTOCOL_MSG_CANCELAR,ID_EQUIVOCADO,false);

    size_t total=0;
    bool visto_error=false, final=false, enviado_tardio=false;
    for (;;) {
        protocol_message_t m={0};
        protocol_result_t r=protocol_recv_message(socks[0],&m);
        if(r==PROTOCOL_EOF)break;
        assert(r==PROTOCOL_OK);
        if (m.header.type==PROTOCOL_MSG_DATOS) {
            assert(!final);
            assert(m.header.request_id==ID_VOD);
            total+=m.header.payload_length;
            assert(total<=tam);
            if (tipo==T_CANCELAR_DURANTE && !enviado_tardio) {
                enviado_tardio=true;
                enviar_control(socks[0],PROTOCOL_MSG_CANCELAR,ID_VOD,false);
            }
        } else if (m.header.type==PROTOCOL_MSG_FIN) {
            assert(tipo==T_COMPLETO || tipo==T_ID_MALO);
            assert(total==tam);
            assert(m.header.request_id==ID_VOD);
            final=true;
        } else if (m.header.type==PROTOCOL_MSG_CANCELADO) {
            assert(tipo==T_CANCELAR || tipo==T_FRAGMENTADO || tipo==T_CANCELAR_DURANTE);
            assert(m.header.request_id==ID_VOD);
            final=true;
        } else if (m.header.type==PROTOCOL_MSG_DESPEDIDA) {
            assert(tipo==T_CERRAR);
            assert(m.header.request_id==ID_CERRAR);
            final=true;
        } else if (m.header.type==PROTOCOL_MSG_ERROR) {
            assert(tipo==T_ID_MALO);
            assert(m.header.request_id==ID_EQUIVOCADO);
            assert(m.header.status==PROTOCOL_STATUS_NO_PERMITIDO);
            visto_error=true;
        } else assert(false);
        protocol_message_free(&m);
    }
    assert(final);
    if (tipo==T_ID_MALO) assert(visto_error);
    if (tipo==T_CANCELAR || tipo==T_CERRAR || tipo==T_FRAGMENTADO ||
        tipo==T_CANCELAR_DURANTE)
        assert(total < tam);
    if (tipo==T_CANCELAR_DURANTE) assert(enviado_tardio);
    int st=0;
    assert(waitpid(pid,&st,0)==pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st)==0);
    close(socks[0]);
    unlink(ruta);
    printf("OK: %d (%zu bytes transferidos de %zu)\n",tipo,total,tam);
}

int main(void) {
    probar(T_COMPLETO,0);
    probar(T_COMPLETO,1000);
    probar(T_COMPLETO,400000);
    probar(T_CANCELAR,5000000);
    probar(T_CERRAR,5000000);
    probar(T_ID_MALO,500000);
    probar(T_FRAGMENTADO,5000000);
    probar(T_CANCELAR_DURANTE,5000000);
    puts("Todas las pruebas de control VOD pasaron.");
}
