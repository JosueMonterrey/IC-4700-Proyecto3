#ifndef SESSION_H
#define SESSION_H

#include "catalog.h"
#include "channel.h"

int atender_sesion(int socket, const struct catalogo *cat, const struct canal *canal, const char *origen, int inactividad_s);

void detener_sesiones();
int detencion_solicitada();

#endif