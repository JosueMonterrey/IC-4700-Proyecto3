#ifndef SESSION_H
#define SESSION_H

#include "catalog.h"

int atender_sesion(int socket, const struct catalogo *cat, const char *origen, int inactividad_s);

void detener_sesiones();
int detencion_solicitada();

#endif