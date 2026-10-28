#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "exitcodes.h"
#include "log.h"
#include "protocol.h"

static uint32_t g_id = 0;

static void ayuda(FILE *out, const char *prog)
{
    fprintf(out,
            "Uso: %s --host DIRECCION --puerto N --accion ACCION [opciones]\n"
            "\n"
            "Cliente del servidor de video.\n"
            "\n"
            "Obligatorios:\n"
            "  --host DIRECCION     Direccion IP o nombre del servidor\n"
            "  --puerto N           Puerto del servidor (1-65535)\n"
            "  --accion ACCION      canal | listar | descargar\n"
            "                       (por ahora solo 'listar' esta implementada)\n"
            "\n"
            "Opcionales:\n"
            "  --tiempo-limite S    Segundos maximos de espera de conexion o de respuesta\n"
            "                       (por omision: 10)\n"
            "  --ayuda, -h          Muestra esta ayuda\n"
            "\n"
            "Ejemplos:\n"
            "  %s --host 127.0.0.1 --puerto 8080 --accion listar\n"
            "  %s --host 192.168.1.20 --puerto 8080 --accion listar --tiempo-limite 5\n"
            "\n"
            "Codigos de salida: 0 exito, 2 argumentos, 3 red, 4 contenido, 5 capacidad, 6 interno.\n",
            prog, prog, prog);
}

static int entero(const char *txt, long min, long max, long *out)
{
    char *fin;
    errno = 0;
    long v = strtol(txt, &fin, 10);
    if (txt[0] == '\0' || *fin != '\0' || errno != 0 || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

#define ERR_ARG(...)                            \
    do                                          \
    {                                           \
        fprintf(stderr, "error: " __VA_ARGS__); \
        fprintf(stderr, "\n");                  \
        return -1;                              \
    } while (0)

struct config
{
    const char *host;
    int puerto;
    const char *accion;
    int tiempo_limite;
};

/* 0 = válida, 1 = se pidió ayuda, -1 = error */
static int leer_config(int argc, char **argv, struct config *cfg)
{
    static const struct option largas[] = {
        {"host", required_argument, NULL, 'H'},
        {"puerto", required_argument, NULL, 'p'},
        {"accion", required_argument, NULL, 'a'},
        {"tiempo-limite", required_argument, NULL, 't'},
        {"ayuda", no_argument, NULL, 'h'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    const char *puerto_txt = NULL;
    long v;
    int c;

    cfg->host = NULL;
    cfg->accion = NULL;
    cfg->tiempo_limite = 10;

    opterr = 0;
    while ((c = getopt_long(argc, argv, "h", largas, NULL)) != -1)
    {
        switch (c)
        {
        case 'H':
            cfg->host = optarg;
            break;
        case 'p':
            puerto_txt = optarg;
            break;
        case 'a':
            cfg->accion = optarg;
            break;
        case 't':
            if (entero(optarg, 1, 3600, &v) != 0)
                ERR_ARG("--tiempo-limite debe ser un numero de segundos entre 1 y 3600 (recibido '%s')", optarg);
            cfg->tiempo_limite = (int)v;
            break;
        case 'h':
            return 1;
        default:
            ERR_ARG("argumento desconocido o sin valor: '%s'", argv[optind - 1]);
        }
    }
    if (optind < argc)
        ERR_ARG("argumento inesperado: '%s'", argv[optind]);

    if (cfg->host == NULL)
        ERR_ARG("falta --host");
    if (puerto_txt == NULL)
        ERR_ARG("falta --puerto");
    if (cfg->accion == NULL)
        ERR_ARG("falta --accion (canal, listar o descargar)");

    if (entero(puerto_txt, 1, 65535, &v) != 0)
        ERR_ARG("--puerto debe ser un entero entre 1 y 65535 (recibido '%s')", puerto_txt);
    cfg->puerto = (int)v;

    if (strcmp(cfg->accion, "listar") != 0)
    {
        if (strcmp(cfg->accion, "canal") == 0 || strcmp(cfg->accion, "descargar") == 0)
            ERR_ARG("la accion '%s' todavia no esta implementada en esta version", cfg->accion);
        ERR_ARG("--accion '%s' no es valida; use canal, listar o descargar", cfg->accion);
    }
    return 0;
}

static int conectar(const struct config *cfg)
{
    struct addrinfo hints, *res, *ai;
    char puerto[16];
    int s = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(puerto, sizeof puerto, "%d", cfg->puerto);

    int addr = getaddrinfo(cfg->host, puerto, &hints, &res);
    if (addr != 0)
    {
        log_error("no se pudo resolver '%s': %s", cfg->host, gai_strerror(addr));
        return -1;
    }

    struct timeval timeframe = {.tv_sec = cfg->tiempo_limite, .tv_usec = 0};
    for (ai = res; ai != NULL; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
            continue;

        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeframe, sizeof timeframe);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeframe, sizeof timeframe);

        // intentar conectar
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        // seguir buscando
        addr = errno;
        close(s);
        s = -1;
        errno = addr;
    }

    freeaddrinfo(res);

    if (s < 0)
        log_error("no se pudo conectar a %s:%d", cfg->host, cfg->puerto);

    return s;
}

static int codigo_por_estado(uint16_t st)
{
    switch (st)
    {
    case NO_HALLADO:
    case NO_SERVIBLE:
        return SALIDA_CONTENIDO;
    case CAPACIDAD:
        return SALIDA_CAPACIDAD;
    case VERSION:
        return SALIDA_RED;
    default:
        return SALIDA_INTERNO;
    }
}

static int recibir_esperado(int socket, uint16_t tipo_esperado, struct encabezado *enc, char *buff, uint32_t cap)
{
    switch (recibir_msg(socket, enc, buff, cap))
    {
    case PROTOCOL_OK:
        break;

    case PROTOCOL_ERR_VERSION:
        log_error("el servidor responde con otra version del protocolo (%u)", (unsigned)enc->version);
        return SALIDA_RED;

    case PROTOCOL_ERR_LONGITUD:
        log_error("respuesta invalida del servidor (longitud imposible)");
        return SALIDA_RED;

    case PROTOCOL_ERR_IO:
    default:
        log_error("sin respuesta del servidor (tiempo agotado o conexion cerrada)");
        return SALIDA_RED;
    }

    if (enc->tipo == tipo_esperado)
        return 0;

    if (enc->tipo == ERROR)
    {
        log_error("el servidor rechazo la solicitud (codigo %u): %.*s", (unsigned)enc->estado, (int)enc->longitud, buff);
        return codigo_por_estado(enc->estado);
    }
    if (enc->tipo == DESPEDIDA)
    {
        log_error("el servidor cerro la sesion de forma ordenada: %.*s", (int)enc->longitud, buff);
        return SALIDA_RED;
    }

    log_error("respuesta inesperada del servidor (tipo %u)", (unsigned)enc->tipo);
    return SALIDA_INTERNO;
}

static int accion_listar(int socket)
{
    struct encabezado enc;
    int rc = SALIDA_INTERNO;
    char *buff = malloc((size_t)MAX_CARGA + 1);

    if (buff == NULL)
    {
        log_error("sin memoria");
        return SALIDA_INTERNO;
    }

    if (enviar_msg(socket, LISTAR, OK, ++g_id, NULL, 0) != PROTOCOL_OK)
    {
        log_error("no se pudo enviar la solicitud LISTAR");
        rc = SALIDA_RED;
        goto fin;
    }

    log_info("solicitud LISTAR enviada");

    rc = recibir_esperado(socket, CATALOGO, &enc, buff, MAX_CARGA);
    if (rc != 0)
        goto fin;

    buff[enc.longitud] = '\0'; // fin del string

    int n = 0;
    printf("\nCatalogo del servidor:\n");
    char *linea = buff;
    while (*linea != '\0')
    {
        char *fin_linea = strchr(linea, '\n');
        char *siguiente;
        if (fin_linea != NULL)
        {
            *fin_linea = '\0';
            siguiente = fin_linea + 1;
        }
        else
        {
            siguiente = linea + strlen(linea);
        }

        char *tab = strchr(linea, '\t');
        if (tab != NULL)
        {
            *tab = '\0';
            printf("  %-40s %12s bytes\n", linea, tab + 1);
            n++;
        }
        linea = siguiente;
    }

    if (n == 0)
        printf("  (catalogo vacio)\n");

    printf("%d contenido(s) disponible(s).\n\n", n);
    rc = SALIDA_OK;

fin:
    free(buff);
    return rc;
}

// MAIN
int main(int argc, char **argv)
{
    struct config cfg;

    int r = leer_config(argc, argv, &cfg);
    if (r == 1)
    {
        ayuda(stdout, argv[0]);
        return SALIDA_OK;
    }
    if (r < 0)
    {
        fprintf(stderr, "Forma de uso: %s --host DIRECCION --puerto N --accion listar   (use --ayuda para mas detalles)\n", argv[0]);
        return SALIDA_ARGS;
    }

    signal(SIGPIPE, SIG_IGN);

    int socket = conectar(&cfg);
    if (socket < 0)
        return SALIDA_RED;

    log_info("conectado a %s:%d", cfg.host, cfg.puerto);

    // SALUDO
    struct encabezado enc;
    char saludo[256];
    int rc;

    if (enviar_msg(socket, SALUDO, OK, ++g_id, NULL, 0) != PROTOCOL_OK)
    {
        log_error("no se pudo enviar SALUDO");
        close(socket);
        return SALIDA_RED;
    }

    rc = recibir_esperado(socket, BIENVENIDA, &enc, saludo, sizeof saludo - 1);
    if (rc != 0)
    {
        close(socket);
        return rc;
    }

    saludo[enc.longitud] = '\0'; // arreglar string
    log_info("sesion establecida (servidor: %s)", saludo);

    rc = accion_listar(socket);

    if (enviar_msg(socket, CERRAR, OK, ++g_id, NULL, 0) == PROTOCOL_OK)
    {
        char buff[128];
        recibir_msg(socket, &enc, buff, sizeof buff);
    }
    close(socket);

    if (rc == SALIDA_OK)
        log_info("operacion completada");
        
    return rc;
}