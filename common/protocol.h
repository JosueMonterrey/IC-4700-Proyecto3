#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// Contantes
#define PROTO_VERSION      1
#define MAX_NOMBRE         255          // max nombre archivo largo máximo de un nombre de video
#define MAX_CARGA          (1024*1024)  // max carga útil aceptada = 1MB
#define TAM_BLOQUE         (64*1024)    // tam bloque de datos = 64KB

// Valores de respuesta de funciones
#define PROTOCOL_OK            0   // ok
#define PROTOCOL_ERR_IO       -1   // error de red o conexión cerrada
#define PROTOCOL_ERR_VERSION  -2   // diferente versión
#define PROTOCOL_ERR_LONGITUD -3   // longitud excedida


// Mensajes
enum tipo_msg {
    SALUDO      = 1,
    BIENVENIDA  = 2,
    LISTAR      = 3,
    CATALOGO    = 4,
    ABRIR       = 5,
    ACEPTADO    = 6,
    DATOS       = 7,
    FIN         = 8,
    ERROR       = 9,
    CERRAR      = 10,
    DESPEDIDA   = 11,
    UNIR        = 12,
    EMISION     = 13,
    SALIR       = 14
};

// Id Estados
enum estado {
    OK           = 0,
    VERSION      = 1,
    SOLICITUD    = 2,
    NO_HALLADO   = 3,
    NO_SERVIBLE  = 4,
    NO_PERMITIDO = 5,
    CAPACIDAD    = 6,
    INTERNO      = 7
};


// Encabezados 16 bytes
#define TAM_ENCABEZADO 16

struct encabezado {
    uint16_t version;
    uint16_t tipo;
    uint16_t estado;
    uint16_t reservado;
    uint32_t id_solicitud;
    uint32_t longitud;
};



int enviar(int socket, const void *buff, size_t n);
int recibir(int socket, void *buff, size_t n);

int enviar_msg( int socket, 
                uint16_t tipo,
                uint16_t estado,
                uint32_t id, 
                const void *carga, 
                uint32_t longitud
            );

int recibir_msg(int socket, 
                struct encabezado *enc, 
                void *carga, 
                uint32_t max_carga
            );

#endif