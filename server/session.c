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

#include "../common/log.h"
#include "../common/protocol.h"

#define TAM_SOLICITUD 512 // max tam solicitud

#define TXT_SERVICIOS_VOD "servicios: listar, descargar"
#define TXT_SERVICIOS_CANAL "servicios: listar, descargar, canal"
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
static int responder_abrir(int socket, const char *origen, const struct catalogo *cat, const struct encabezado *enc, const char *nombre)
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

// CANAL EN VIVO
#define ADELANTO_NS 1000000000LL // cuanto se puede enviar por delante de la linea de tiempo
#define PASO_ESPERA_MS 200       // cada cuanto se revisa detencion / mensajes del cliente

enum fin_canal
{
    CANAL_SIGUE,
    CANAL_SALIO,     // el cliente envio SALIR
    CANAL_PERDIDO,   // el cliente desaparecio o envio basura
    CANAL_DETENCION, // el servidor se esta deteniendo
    CANAL_ERROR      // fallo al leer el contenido
};

// Durante una transferencia solo se admite SALIR. Cualquier otra cosa se rechaza.
static enum fin_canal revisar_cliente(int socket, const char *origen)
{
    struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
    int r = poll(&p, 1, 0);
    if (r < 0)
        return errno == EINTR ? CANAL_SIGUE : CANAL_PERDIDO;
    if (r == 0)
        return CANAL_SIGUE;

    struct encabezado enc;
    char buff[TAM_SOLICITUD];
    int rr = recibir_msg(socket, &enc, buff, sizeof buff);
    if (rr != PROTOCOL_OK)
    {
        if (rr == PROTOCOL_ERR_LONGITUD || rr == PROTOCOL_ERR_VERSION)
        {
            enviar_texto(socket, ERROR, rr == PROTOCOL_ERR_VERSION ? VERSION : SOLICITUD,
                         enc.id_solicitud, "mensaje invalido durante la transmision");
            log_error("[%s] canal: mensaje invalido del cliente", origen);
        }
        else
            log_info("[%s] canal: el cliente se desconecto", origen);
        return CANAL_PERDIDO;
    }

    if (enc.tipo == SALIR)
        return CANAL_SALIO;

    enviar_texto(socket, ERROR, NO_PERMITIDO, enc.id_solicitud,
                 "hay una transmision en curso: envie SALIR antes de otra solicitud");
    log_error("[%s] canal: solicitud tipo %u rechazada (transmision en curso)", origen, (unsigned)enc.tipo);
    return CANAL_SIGUE;
}

// Espera sin enviar nada hasta limite_ns
static enum fin_canal esperar_hasta(int socket, const char *origen, int64_t limite_ns)
{
    while (1)
    {
        if (g_detener)
            return CANAL_DETENCION;

        int64_t resto = limite_ns - canal_ahora_ns();
        if (resto <= 0)
            return CANAL_SIGUE;

        int ms = resto > (int64_t)PASO_ESPERA_MS * 1000000LL ? PASO_ESPERA_MS : (int)((resto + 999999) / 1000000);
        struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
        int r = poll(&p, 1, ms);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return CANAL_PERDIDO;
        }
        if (r > 0)
        {
            enum fin_canal f = revisar_cliente(socket, origen);
            if (f != CANAL_SIGUE)
                return f;
        }
    }
}

// operacion: UNIR
static int responder_unir(int socket, const char *origen, const struct canal *canal, const struct encabezado *enc)
{
    uint32_t id = enc->id_solicitud;

    if (canal == NULL)
    {
        enviar_texto(socket, ERROR, NO_SERVIBLE, id, "el canal en vivo no esta disponible en este momento");
        log_error("[%s] UNIR: el canal no esta disponible", origen);
        return 0;
    }

    if (enc->longitud != 0)
    {
        enviar_texto(socket, ERROR, NO_PERMITIDO, id, "el canal no permite elegir una posicion de reproduccion");
        log_error("[%s] UNIR: se intento indicar una posicion (rechazado)", origen);
        return 0;
    }

    struct posicion_canal pos;
    canal_posicion(canal, canal_ahora_ns(), &pos);

    char meta[320];
    int mlen = snprintf(meta, sizeof meta,
                        "posicion_ms=%lld\n"
                        "ciclo_ms=%lld\n"
                        "contenido_ms=%lld\n"
                        "separacion_ms=%lld\n"
                        "elementos=%zu\n"
                        "elemento_actual=%zu\n"
                        "numero_ciclo=%llu\n"
                        "en_separacion=%d\n",
                        (long long)(pos.pos_ns / 1000000),
                        (long long)(canal->ciclo_ns / 1000000),
                        (long long)(canal->contenido_ns / 1000000),
                        (long long)(canal->separacion_ns / 1000000),
                        canal->cantidad,
                        pos.indice + 1,
                        (unsigned long long)pos.ciclo, pos.en_separacion);

    char *bloque = malloc(TAM_BLOQUE);
    if (bloque == NULL)
    {
        enviar_texto(socket, ERROR, INTERNO, id, "error interno del servidor");
        log_error("[%s] UNIR: sin memoria", origen);
        return 0;
    }

    if (enviar_msg(socket, EMISION, OK, id, meta, (uint32_t)mlen) != PROTOCOL_OK)
    {
        free(bloque);
        log_error("[%s] UNIR: fallo al enviar EMISION", origen);
        return -1;
    }

    log_info("[%s] UNIR: incorporado en %lld.%03lld s (ciclo %llu, elemento %zu/%zu%s)", origen,
             (long long)(pos.pos_ns / 1000000000LL), (long long)((pos.pos_ns / 1000000) % 1000),
             (unsigned long long)pos.ciclo, pos.indice + 1, canal->cantidad,
             pos.en_separacion ? ", en separacion entre ciclos" : "");

    uint64_t ciclo = pos.ciclo;
    size_t i = pos.indice;
    uint64_t byte = pos.byte;
    int en_separacion = pos.en_separacion;
    uint64_t enviados = 0;
    int fd = -1;
    enum fin_canal fin = CANAL_SIGUE;

    while (fin == CANAL_SIGUE)
    {
        int64_t base = canal->t0_ns + (int64_t)ciclo * canal->ciclo_ns; // inicio del ciclo actual

        if (en_separacion)
        {
            fin = esperar_hasta(socket, origen, base + canal->ciclo_ns);
            ciclo++;
            i = 0;
            byte = 0;
            en_separacion = 0;
            continue;
        }

        const struct elemento_canal *el = &canal->elementos[i];
        fd = open(el->ruta, O_RDONLY);
        if (fd < 0 || lseek(fd, (off_t)byte, SEEK_SET) < 0)
        {
            log_error("[%s] canal: no se pudo abrir '%s': %s", origen, el->ruta, strerror(errno));
            fin = CANAL_ERROR;
            break;
        }

        while (byte < el->tamano && fin == CANAL_SIGUE)
        {
            // el bloque se emite cuando su instante en la linea de tiempo esta a menos de ADELANTO_NS
            int64_t objetivo = base + canal_instante_byte(el, byte);
            fin = esperar_hasta(socket, origen, objetivo - ADELANTO_NS);
            if (fin != CANAL_SIGUE)
                break;

            fin = revisar_cliente(socket, origen);
            if (fin != CANAL_SIGUE)
                break;

            uint64_t restante = el->tamano - byte;
            size_t solicitar = restante < TAM_BLOQUE ? (size_t)restante : (size_t)TAM_BLOQUE;
            ssize_t n = read(fd, bloque, solicitar);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
            {
                log_error("[%s] canal: lectura fallida de '%s' (%s)", origen, el->ruta,
                          n < 0 ? strerror(errno) : "fin de archivo prematuro");
                fin = CANAL_ERROR;
                break;
            }

            if (enviar_msg(socket, DATOS, OK, id, bloque, (uint32_t)n) != PROTOCOL_OK)
            {
                log_info("[%s] canal: cliente perdido tras %llu bytes", origen, (unsigned long long)enviados);
                fin = CANAL_PERDIDO;
                break;
            }

            byte += (uint64_t)n;
            enviados += (uint64_t)n;
        }

        close(fd);
        fd = -1;
        if (fin != CANAL_SIGUE)
            break;

        // siguiente elemento, empieza otro ciclo
        byte = 0;
        if (++i == canal->cantidad)
        {
            i = 0;
            if (canal->separacion_ns > 0)
                en_separacion = 1;
            else
                ciclo++;
        }
    }

    free(bloque);
    if (fd >= 0)
        close(fd);

    switch (fin)
    {
    case CANAL_SALIO:
        log_info("[%s] canal: el cliente abandono la transmision (%llu bytes enviados)", origen, (unsigned long long)enviados);
        if (enviar_msg(socket, FIN, OK, id, NULL, 0) != PROTOCOL_OK)
            return -1;
        return 0;

    case CANAL_ERROR:
        enviar_texto(socket, ERROR, INTERNO, id, "error al leer el contenido del canal");
        return 0;

    case CANAL_DETENCION:
        enviar_texto(socket, DESPEDIDA, OK, id, TXT_DETENCION);
        log_info("[%s] servidor deteniendose: canal cerrado", origen);
        return -1;

    case CANAL_PERDIDO:
    default:
        return -1;
    }
}

int atender_sesion(int socket, const struct catalogo *cat, const struct canal *canal, const char *origen, int inactividad)
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

    if (enviar_texto(socket, BIENVENIDA, OK, enc.id_solicitud, canal ? TXT_SERVICIOS_CANAL : TXT_SERVICIOS_VOD) != PROTOCOL_OK)
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

        case UNIR:
            if (responder_unir(socket, origen, canal, &enc) != 0)
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