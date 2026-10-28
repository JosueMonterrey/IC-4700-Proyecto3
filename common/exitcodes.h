#ifndef EXITCODES_H

#define EXITCODES_H

// Códigos de terminación de ambos programas (RF-29, RF-68).
#define SALIDA_OK          0   // Ok
#define SALIDA_ARGS        2   // Argumentos ausentes o inválidos
#define SALIDA_RED         3   // Error de red: conexión, tiempo agotado, puerto ocupado
#define SALIDA_CONTENIDO   4   // El servidor rechazó el contenido (no hallado / no disponible)
#define SALIDA_CAPACIDAD   5   // El servidor alcanzó su límite de clientes
#define SALIDA_INTERNO     6   // Error interno o respuesta inesperada

#endif