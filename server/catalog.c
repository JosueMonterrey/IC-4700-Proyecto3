#define _POSIX_C_SOURCE 200809L

#include "catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int nombre_valido(const char *nombre, size_t len)
{
    if (nombre == NULL || len == 0 || len > MAX_NOMBRE)
        return 0;

    // verificar que no hayan caracteres nulos en el nombre
    if (memchr(nombre, '\0', len) != NULL)
        return 0;
        
    if (nombre[0] == '.') // evitar archivos ocultos o salir el un directorio ".."
        return 0;
        
    for (size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)nombre[i];

        // caracteres invalidos
        if ((ch < 0x20 || ch == 0x7f) || (ch == '/' || ch == '\\'))
            return 0;
        
        // ".." en cualquier posición
        if (i + 1 < len && nombre[i] == '.' && nombre[i + 1] == '.')
            return 0;
    }

    return 1;
}


static int comparar(const void *a, const void *b)
{
    const struct entrada_catalogo *x = a, *y = b;
    return strcmp(x->nombre, y->nombre);
}

void catalogo_liberar(struct catalogo *cat)
{
    if (cat == NULL)
        return;

    for (size_t i = 0; i < cat->cantidad; i++)
        free(cat->entradas[i].ruta);

    free(cat->entradas);

    cat->entradas = NULL;
    cat->cantidad = 0;
}

int catalogo_construir(struct catalogo *cat, const char *dir)
{
    cat->entradas = NULL;
    cat->cantidad = 0;

    DIR *directory = opendir(dir);
    if (directory == NULL)
        return -1;

    size_t capacidad = 0;
    size_t dirlen = strlen(dir);
    struct dirent *el;

    while ((el = readdir(directory)) != NULL)
    {
        size_t namelen = strlen(el->d_name);
        if (!nombre_valido(el->d_name, namelen))
            continue;

        size_t rutalen = dirlen + 1 + namelen;
        char *ruta = malloc(rutalen + 1);
        if (ruta == NULL)
            goto fallo;

        // escribir la ruta completa en el buffer
        snprintf(ruta, rutalen + 1, "%s/%s", dir, el->d_name);

        // obtener informacion del archivo, fallar si no existe o es carpeta
        struct stat archivo;
        if (stat(ruta, &archivo) != 0 || !S_ISREG(archivo.st_mode))
        {
            free(ruta); /* subcarpeta, enlace roto, etc. */
            continue;
        }

        if (cat->cantidad == capacidad)
        {
            // duplicar capacidad
            size_t nueva = capacidad ? capacidad * 2 : 16;

            // actualizar tamaño del catalogo
            struct entrada_catalogo *nuevo_catalogo = realloc(cat->entradas, nueva * sizeof *nuevo_catalogo);
            if (nuevo_catalogo == NULL)
            {
                free(ruta);
                goto fallo;
            }

            cat->entradas = nuevo_catalogo;
            capacidad = nueva;
        }

        struct entrada_catalogo *n = &cat->entradas[cat->cantidad++];
        memcpy(n->nombre, el->d_name, namelen + 1);
        n->ruta = ruta;
        n->tamano = (uint64_t)archivo.st_size;
        n->disponible = (access(ruta, R_OK) == 0);
    }

    closedir(directory);
    if (cat->cantidad > 1)
        qsort(cat->entradas, cat->cantidad, sizeof *cat->entradas, comparar);

    return 0;

fallo:
    closedir(directory);
    catalogo_liberar(cat);
    return -1;
}


size_t catalogo_disponibles(const struct catalogo *cat)
{
    size_t n = 0;
    for (size_t i = 0; i < cat->cantidad; i++)
        if (cat->entradas[i].disponible)
            n++;

    return n;
}


const struct entrada_catalogo *catalogo_buscar( const struct catalogo *cat,
                                                const char *nombre,
                                                size_t len)
{
    if (!nombre_valido(nombre, len))
        return NULL;

    for (size_t i = 0; i < cat->cantidad; i++)
    {
        const struct entrada_catalogo *e = &cat->entradas[i];
        if (strlen(e->nombre) == len && memcmp(e->nombre, nombre, len) == 0)
            return e;
    }
    
    return NULL;
}


// MSG_CATALOGO
int catalogo_armar_carga(const struct catalogo *cat, char *out, size_t cap, size_t *len)
{
    size_t pos = 0;

    for (size_t i = 0; i < cat->cantidad; i++)
    {
        const struct entrada_catalogo *e = &cat->entradas[i];
        
        if (!e->disponible)
            continue;

        int n = snprintf(out + pos, cap - pos, "%s\t%llu\n", e->nombre, (unsigned long long)e->tamano);

        if (n < 0 || (size_t)n >= cap - pos)
            return -1; // sin espacio

        pos += (size_t)n;
    }

    *len = pos;
    return 0;
}