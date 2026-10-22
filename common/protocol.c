#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Enviar n bytes
int enviar(int socket, const void *buff, size_t n)
{
    const uint8_t *p = buff;
    size_t total_enviados = 0;

    while (total_enviados < n)
    {
        // si el otro lado cerró send() devuelve MSG_NOSIGNAL
        ssize_t enviados = send(socket, p + total_enviados, n - total_enviados, MSG_NOSIGNAL);

        if (enviados < 0)
        {
            if (errno == EINTR)
                continue; // reintentar

            return PROTO_ERR_IO;
        }

        if (enviados == 0)
            return PROTO_ERR_IO;

        total_enviados += (size_t)enviados;
    }

    return PROTO_OK;
}

// Recibir n bytes
int recibir(int socket, void *buff, size_t n)
{
    uint8_t *p = buff;
    size_t total_recibidos = 0;

    while (total_recibidos < n)
    {
        ssize_t recibidos = recv(socket, p + total_recibidos, n - total_recibidos, 0);

        if (recibidos < 0)
        {
            if (errno == EINTR)
                continue; // reintentar

            return PROTO_ERR_IO;
        }

        if (recibidos == 0)
            return PROTO_ERR_IO; // conexion fue cerrada

        total_recibidos += (size_t)recibidos;
    }

    return PROTO_OK;
}

/* ------------------------------------------------------------------ */
/* Conversión del encabezado a/desde bytes de red                      */
/* ------------------------------------------------------------------ */
static void codificar_enc(uint8_t res[TAM_ENCABEZADO], const struct encabezado *enc)
{
    // convertir a big endian
    uint16_t version = htons(enc->version);
    uint16_t tipo = htons(enc->tipo);
    uint16_t estado = htons(enc->estado);
    uint16_t reservado = htons(enc->reservado);
    uint32_t id = htonl(enc->id_solicitud);
    uint32_t longitud = htonl(enc->longitud);

    // mover datos a buffer
    memcpy(res + 0, &version, 2);
    memcpy(res + 2, &tipo, 2);
    memcpy(res + 4, &estado, 2);
    memcpy(res + 6, &reservado, 2);
    memcpy(res + 8, &id, 4);
    memcpy(res + 12, &longitud, 4);
}

static void decodificar_enc(struct encabezado *e, const uint8_t in[TAM_ENCABEZADO])
{
    uint16_t version;
    uint16_t tipo;
    uint16_t estado;
    uint16_t reservado;
    uint32_t id;
    uint32_t longitud;

    // Leer cada campo sabiendo su posición en el buffer
    memcpy(&version, in + 0, 2);
    memcpy(&tipo, in + 2, 2);
    memcpy(&estado, in + 4, 2);
    memcpy(&reservado, in + 6, 2);
    memcpy(&id, in + 8, 4);
    memcpy(&longitud, in + 12, 4);

    // Convertir los valores de big endian
    e->version = ntohs(version);
    e->tipo = ntohs(tipo);
    e->estado = ntohs(estado);
    e->reservado = ntohs(reservado);
    e->id_solicitud = ntohl(id);
    e->longitud = ntohl(longitud);
}

// Enviar un mensaje
int enviar_msg(int socket, uint16_t tipo, uint16_t estado, uint32_t id, const void *carga, uint32_t longitud)
{
    struct encabezado enc;
    uint8_t bytes[TAM_ENCABEZADO];

    if (longitud > MAX_CARGA)
        return PROTO_ERR_LONGITUD;

    if (longitud > 0 && carga == NULL)
        return PROTO_ERR_LONGITUD;

    enc.version = PROTO_VERSION;
    enc.tipo = tipo;
    enc.estado = estado;
    enc.reservado = 0;
    enc.id_solicitud = id;
    enc.longitud = longitud;

    codificar_enc(bytes, &enc);

    if (enviar(socket, bytes, TAM_ENCABEZADO) != PROTO_OK)
        return PROTO_ERR_IO;

    if (longitud > 0 && enviar(socket, carga, longitud) != PROTO_OK)
        return PROTO_ERR_IO;

    return PROTO_OK;
}

// Recibir un mensaje
int recibir_msg(int socket, struct encabezado *enc, void *carga, uint32_t max_carga)
{
    uint8_t bytes[TAM_ENCABEZADO];

    if (recibir(socket, bytes, TAM_ENCABEZADO) != PROTO_OK)
        return PROTO_ERR_IO;

    decodificar_enc(enc, bytes);

    if (enc->version != PROTO_VERSION)
        return PROTO_ERR_VERSION;

    if (enc->longitud > MAX_CARGA || enc->longitud > max_carga)
        return PROTO_ERR_LONGITUD;

    if (enc->longitud > 0)
    {
        if (carga == NULL)
            return PROTO_ERR_LONGITUD;
            
        if (recibir(socket, carga, enc->longitud) != PROTO_OK)
            return PROTO_ERR_IO;
    }

    return PROTO_OK;
}