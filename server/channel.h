#ifndef CHANNEL_H
#define CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#define MAX_ELEMENTOS_CANAL 64

struct elemento_canal
{
    char *ruta;
    uint64_t tamano;     // en bytes
    int64_t duracion_ns; // duracion de reproduccion
    int64_t inicio_ns;   // posicion donde empieza
};

// Canal en vivo
struct canal
{
    struct elemento_canal *elementos;
    size_t cantidad;
    int64_t t0_ns;
    int64_t contenido_ns;  // suma de duraciones de los elementos
    int64_t separacion_ns; // pausa entre ciclos
    int64_t ciclo_ns;      // T = contenido + separacion
};

// (t - t0) mod T
struct posicion_canal
{
    uint64_t ciclo;    // numero de ciclo
    int64_t pos_ns;    // posicion dentro del ciclo
    int en_separacion; // 1 si cae en el intervalo sin contenido
    size_t indice;     // elemento actual
    int64_t offset_ns; // posicion dentro del elemento
    uint64_t byte;     // byte desde el cual se debe empezar a enviar
};

int64_t canal_ahora_ns(void);

int canal_construir(struct canal *c, const char *const *rutas, size_t n, double duracion_total_s, double separacion_s);

// Calcula inicios, duracion del contenido y del ciclo a partir de los elementos
int canal_preparar(struct canal *c);

// Fija t0 = ahora
void canal_iniciar(struct canal *c);

void canal_liberar(struct canal *c);

// depende solo de t0
void canal_posicion(const struct canal *c, int64_t ahora_ns, struct posicion_canal *p);

// byte del elemento que corresponde a un offset de tiempo
uint64_t canal_byte_en(const struct elemento_canal *el, int64_t offset_ns);
int64_t canal_instante_byte(const struct elemento_canal *el, uint64_t byte);

#endif