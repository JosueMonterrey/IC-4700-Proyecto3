#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>
#include <stdint.h>
#include "../common/protocol.h"

// Struc para elemento del catalogo
struct entrada_catalogo {
    char     nombre[MAX_NOMBRE + 1];
    char    *ruta;
    uint64_t tamano;
    int      disponible; // bool
};

// Struc de catalogo
struct catalogo {
    struct entrada_catalogo *entradas;
    size_t                   cantidad;
};


int catalogo_construir(struct catalogo *cat, const char *dir);

// liberar memoria
void catalogo_liberar(struct catalogo *cat);

size_t catalogo_disponibles(const struct catalogo *cat);

int nombre_valido(const char *nombre, size_t len);

const struct entrada_catalogo *catalogo_buscar( const struct catalogo *cat, 
                                                const char *nombre, 
                                                size_t len);

int catalogo_armar_carga(const struct catalogo *cat, char *out, size_t cap, size_t *len);

#endif