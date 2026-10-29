#define _POSIX_C_SOURCE 200809L

#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"
#include "protocol.h"

#define TAM_SOLICITUD 512 // max tam solicitud

#define TXT_SERVICIOS "servicios: listar, descargar"
#define TXT_DETENCION "el servidor se esta deteniendo"

static volatile sig_atomic_t g_detener = 0;

void detener_sesiones(void) { g_detener = 1; }
int detencion_solicitada(void) { return g_detener; }

enum espera
{
    ESPERA_LISTO,
    ESPERA_TIMEOUT,
    ESPERA_DETENER,
    ESPERA_ERROR
};

static enum espera esperar_datos(int socket, int inactividad)
{
    int segundos = 0;

    while (1)
    {
        if (g_detener)
            return ESPERA_DETENER;

        struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
        int r = poll(&p, 1, 1000);

        if (r < 0)
        {
            if (errno == EINTR)
                continue;

            return ESPERA_ERROR;
        }
        if (r > 0)
            return ESPERA_LISTO;

        if (++segundos >= inactividad)
            return ESPERA_TIMEOUT;
    }
}

static int enviar_texto(int socket, uint16_t tipo, uint16_t estado, uint32_t id, const char *txt)
{
    return enviar_msg(socket, tipo, estado, id, txt, (uint32_t)strlen(txt));
}

static int leer_solicitud(int socket, const char *origen, int inactividad, struct encabezado *enc, char *buff, size_t cap)
{
    switch (esperar_datos(socket, inactividad))
    {
    case ESPERA_LISTO:
        break;
    case ESPERA_TIMEOUT:
        log_info("[%s] inactividad de %d s: se cierra la sesion", origen, inactividad);
        return -1;
    case ESPERA_DETENER:
        enviar_texto(socket, DESPEDIDA, OK, 0, TXT_DETENCION);
        log_info("[%s] servidor deteniendose: sesion cerrada", origen);
        return -1;
    case ESPERA_ERROR:
    default:
        log_error("[%s] error esperando datos del cliente", origen);
        return -1;
    }

    switch (recibir_msg(socket, enc, buff, (uint32_t)cap))
    {
    case PROTOCOL_OK:
        return 0;

    case PROTOCOL_ERR_VERSION:
        enviar_texto(socket, ERROR, VERSION, enc->id_solicitud, "version del protocolo no soportada; este servidor habla la version 1");
        log_error("[%s] rechazado: version de protocolo %u no soportada", origen, (unsigned)enc->version);
        return -1;

    case PROTOCOL_ERR_LONGITUD:
        enviar_texto(socket, ERROR, SOLICITUD, enc->id_solicitud, "mensaje malformado: longitud imposible");
        log_error("[%s] rechazado: longitud declarada imposible (%lu bytes)", origen, (unsigned long)enc->longitud);
        return -1;

    case PROTOCOL_ERR_IO:
    default:
        log_info("[%s] conexion cerrada por el cliente o mensaje incompleto", origen);
        return -1;
    }
}

// operacion: LISTAR
static int responder_listar(int socket, const char *origen, const struct catalogo *cat, const struct encabezado *enc)
{
    char *carga = malloc(MAX_CARGA);
    size_t largo = 0;

    if (carga == NULL || catalogo_armar_carga(cat, carga, MAX_CARGA, &largo) != 0)
    {
        free(carga);
        enviar_texto(socket, ERROR, INTERNO, enc->id_solicitud, "no se pudo preparar el catalogo");
        log_error("[%s] LISTAR: no se pudo armar el catalogo", origen);
        return 0;
    }

    int r = enviar_msg(socket, CATALOGO, OK, enc->id_solicitud, carga, (uint32_t)largo);
    free(carga);

    if (r != PROTOCOL_OK)
    {
        log_error("[%s] LISTAR: fallo al enviar la respuesta", origen);
        return -1;
    }

    log_info("[%s] LISTAR atendida (%zu bytes de catalogo)", origen, largo);
    return 0;
}

// operacion: ABRIR
static int responder_abrir(int socket, const char *origen, const struct catalogo *cat,
                           const struct encabezado *enc, const char *nombre)
{
    size_t len = enc->longitud;
    uint32_t id = enc->id_solicitud;

    if (!nombre_valido(nombre, len))
    {
        enviar_texto(socket, ERROR, SOLICITUD, id, "nombre de contenido malformado");
        log_error("[%s] ABRIR: nombre malformado (%zu bytes)", origen, len);
        return 0;
    }

    const struct entrada_catalogo *e = catalogo_buscar(cat, nombre, len);
    if (e == NULL)
    {
        enviar_texto(socket, ERROR, NO_HALLADO, id, "el contenido solicitado no existe en el catalogo");
        log_error("[%s] ABRIR '%.*s': no hallado", origen, (int)len, nombre);
        return 0;
    }

    if (!e->disponible)
    {
        enviar_texto(socket, ERROR, NO_SERVIBLE, id, "el contenido no esta disponible en este momento");
        log_error("[%s] ABRIR '%s': marcado como no servible", origen, e->nombre);
        return 0;
    }

    int fd = open(e->ruta, O_RDONLY);
    if (fd < 0)
    {
        enviar_texto(socket, ERROR, NO_SERVIBLE, id, "el contenido no esta disponible en este momento");
        log_error("[%s] ABRIR '%s': no se pudo abrir: %s", origen, e->nombre, strerror(errno));
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        enviar_texto(socket, ERROR, NO_SERVIBLE, id, "el contenido no esta disponible en este momento");
        log_error("[%s] ABRIR '%s': no es un archivo regular", origen, e->nombre);
        return 0;
    }

    uint64_t total = (uint64_t)st.st_size;

    char *bloque = malloc(TAM_BLOQUE);
    if (bloque == NULL)
    {
        close(fd);
        enviar_texto(socket, ERROR, INTERNO, id, "error interno del servidor");
        log_error("[%s] ABRIR '%s': sin memoria", origen, e->nombre);
        return 0;
    }

    // 0 = continuar | -1 = cerrar
    int resultado = 0;

    char meta[32];
    int mlen = snprintf(meta, sizeof meta, "%llu", (unsigned long long)total);

    if (enviar_msg(socket, ACEPTADO, OK, id, meta, (uint32_t)mlen) != PROTOCOL_OK)
    {
        log_error("[%s] ABRIR '%s': fallo al enviar ACEPTADO", origen, e->nombre);
        resultado = -1;
        goto fin;
    }

    log_info("[%s] ABRIR '%s': transfiriendo %llu bytes", origen, e->nombre, (unsigned long long)total);

    uint64_t enviados = 0;
    while (enviados < total)
    {
        if (g_detener)
        {
            enviar_texto(socket, DESPEDIDA, OK, id, TXT_DETENCION);
            log_info("[%s] servidor deteniendose: transferencia de '%s' abortada", origen, e->nombre);
            resultado = -1;
            goto fin;
        }

        uint64_t restante = total - enviados;
        size_t solicitar = restante < TAM_BLOQUE ? (size_t)restante : (size_t)TAM_BLOQUE;

        ssize_t n = read(fd, bloque, solicitar);
        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
        {
            enviar_texto(socket, ERROR, INTERNO, id, "error al leer el contenido. transferencia abortada");
            log_error("[%s] ABRIR '%s': lectura incompleta tras %llu de %llu bytes (%s)", origen, e->nombre, (unsigned long long)enviados, (unsigned long long)total, n < 0 ? strerror(errno) : "fin de archivo prematuro");
            goto fin;
        }

        if (enviar_msg(socket, DATOS, OK, id, bloque, (uint32_t)n) != PROTOCOL_OK)
        {
            log_error("[%s] ABRIR '%s': cliente perdido tras %llu de %llu bytes", origen, e->nombre, (unsigned long long)enviados, (unsigned long long)total);
            resultado = -1;
            goto fin;
        }

        enviados += (uint64_t)n;
    }

    if (enviar_msg(socket, FIN, OK, id, NULL, 0) != PROTOCOL_OK)
    {
        log_error("[%s] ABRIR '%s': fallo al enviar FIN", origen, e->nombre);
        resultado = -1;
        goto fin;
    }

    log_info("[%s] ABRIR '%s' atendida (%llu bytes)", origen, e->nombre, (unsigned long long)total);

fin:
    free(bloque);
    close(fd);
    return resultado;
}

int atender_sesion(int socket, const struct catalogo *cat, const char *origen, int inactividad)
{
    struct encabezado enc;
    char buff[TAM_SOLICITUD];

    struct timeval timeout = {.tv_sec = inactividad, .tv_usec = 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    if (leer_solicitud(socket, origen, inactividad, &enc, buff, sizeof buff) != 0)
        return -1;

    if (enc.tipo != SALUDO)
    {
        enviar_texto(socket, ERROR, NO_PERMITIDO, enc.id_solicitud, "se esperaba SALUDO para iniciar la sesion");
        log_error("[%s] rechazado: primer mensaje no fue SALUDO (tipo %u)", origen, (unsigned)enc.tipo);
        return -1;
    }

    if (enviar_texto(socket, BIENVENIDA, OK, enc.id_solicitud, TXT_SERVICIOS) != PROTOCOL_OK)
    {
        log_error("[%s] no se pudo enviar BIENVENIDA", origen);
        return -1;
    }

    log_info("[%s] sesion iniciada", origen);

    // saludo y bienvenida completados. atender solicitudes
    while (1)
    {
        if (leer_solicitud(socket, origen, inactividad, &enc, buff, sizeof buff) != 0)
            return -1;

        switch (enc.tipo)
        {
        case LISTAR:
            if (responder_listar(socket, origen, cat, &enc) != 0)
                return -1;
            break;

        case ABRIR:
            if (responder_abrir(socket, origen, cat, &enc, buff) != 0)
                return -1;
            break;

        case CERRAR:
            enviar_texto(socket, DESPEDIDA, OK, enc.id_solicitud, "hasta luego");
            log_info("[%s] sesion concluida", origen);
            return 0;

        default:
            enviar_texto(socket, ERROR, NO_PERMITIDO, enc.id_solicitud, "operacion no disponible");
            log_error("[%s] solicitud no permitida o no disponible (tipo %u)", origen, (unsigned)enc.tipo);
            break;
        }
    }
}