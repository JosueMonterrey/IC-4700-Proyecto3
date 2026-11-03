#ifndef REPRODUCTOR_H
#define REPRODUCTOR_H

#include <sys/types.h>

// Programa externo de video que recibe el contenido por stdin
struct reproductor {
    pid_t       pid; // -1 si no hay reproductor
    int         fd; 
    const char *nombre;
};

#define REPRODUCTOR_INICIAL {-1, -1, NULL}

// Busca en PATH un ejecutable
const char *reproductor_buscar(const char *nombre);

int reproductor_iniciar(struct reproductor *r, const char *nombre);
int reproductor_escribir(struct reproductor *r, const void *buf, size_t n);

// 1 si sigue ejecutandose
int reproductor_vivo(struct reproductor *r);

// esperar = 1: cierra la entrada y espera a que termine de reproducir
// esperar = 0: terminar
void reproductor_cerrar(struct reproductor *r, int esperar);

#endif