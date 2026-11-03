#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "exitcodes.h"
#include "log.h"
#include "protocol.h"
#include "reproductor.h"

#define SALIDA_INTERRUMPIDO 130 // Ctrl+c

static uint32_t g_id = 0;
static volatile sig_atomic_t g_interrumpido = 0;
static int g_flujo_sincronizado = 0; // 1 si tras una interrupcion el protocolo quedo en un punto limpio

static void manejador_senal(int sig)
{
    (void)sig;
    g_interrumpido = 1;
}

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
            "\n"
            "Requeridos para la accion 'descargar':\n"
            "  --archivo NOMBRE     Nombre del contenido tal como aparece en el catalogo\n"
            "  --destino RUTA       Directorio o archivo donde guardar lo recibido\n"
            "\n"
            "Opcionales:\n"
            "  --tiempo-limite S    Segundos maximos de espera de conexion o de respuesta\n"
            "                       (por omision: 10)\n"
            "  --tiempo-canal S     Duracion maxima de la conexion al canal (solo 'canal');\n"
            "                       si se omite, es indefinida (Ctrl+C para salir)\n"
            "  --reproductor PROG   Programa de video que recibe el contenido por stdin\n"
            "                       (se invoca como 'PROG -'). Por omision: mpv, ffplay, vlc o mplayer\n"
            "  --sin-reproductor    No lanzar reproductor (el canal se recibe y se descarta)\n"
            "  --ayuda, -h          Muestra esta ayuda\n"
            "\n"
            "Ejemplos:\n"
            "  %s --host 127.0.0.1 --puerto 8080 --accion canal\n"
            "  %s --host 127.0.0.1 --puerto 8080 --accion canal --tiempo-canal 120 --sin-reproductor\n"
            "  %s --host 127.0.0.1 --puerto 8080 --accion listar\n"
            "  %s --host 127.0.0.1 --puerto 8080 --accion descargar --archivo video.mp4 --destino ./descargas/\n"
            "  %s --host 192.168.1.20 --puerto 8080 --accion descargar --archivo video.mp4 --destino copia.mp4 --tiempo-limite 5\n"
            "\n"
            "Codigos de salida: 0 exito, 2 argumentos, 3 red, 4 contenido, 5 capacidad, 6 interno,\n"
            "                   130 interrumpido por el usuario.\n",
            prog, prog, prog, prog, prog, prog);
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
    const char *archivo;
    const char *destino;
    const char *reproductor;
    int sin_reproductor;
    int tiempo_canal;                // 0 = indefinido
    char ruta_final[PATH_MAX];       // ruta definitiva de la descarga
    char ruta_parcial[PATH_MAX + 8]; // ruta temporal ".part" mientras se recibe
};

// Calcula la ruta final y valida que se pueda escribir ahi (antes de usar la red)
static int resolver_destino(struct config *cfg)
{
    struct stat st;
    int n;

    if (stat(cfg->destino, &st) == 0 && S_ISDIR(st.st_mode))
    {
        if (access(cfg->destino, W_OK | X_OK) != 0)
            ERR_ARG("--destino '%s' es un directorio sin permiso de escritura", cfg->destino);

        n = snprintf(cfg->ruta_final, sizeof cfg->ruta_final, "%s/%s", cfg->destino, cfg->archivo);
    }
    else
    {
        size_t dlen = strlen(cfg->destino);
        if (dlen == 0 || cfg->destino[dlen - 1] == '/')
            ERR_ARG("--destino '%s' no es un directorio existente ni un nombre de archivo", cfg->destino);

        if (errno == 0 || stat(cfg->destino, &st) == 0)
        {
            if (stat(cfg->destino, &st) == 0 && !S_ISREG(st.st_mode))
                ERR_ARG("--destino '%s' existe y no es un archivo regular", cfg->destino);
        }

        n = snprintf(cfg->ruta_final, sizeof cfg->ruta_final, "%s", cfg->destino);
        if (n < 0 || (size_t)n >= sizeof cfg->ruta_final)
            ERR_ARG("--destino es demasiado largo");

        // el directorio padre debe existir y ser escribible
        char padre[PATH_MAX];
        snprintf(padre, sizeof padre, "%s", cfg->ruta_final);
        char *barra = strrchr(padre, '/');
        if (barra == NULL)
            snprintf(padre, sizeof padre, ".");
        else if (barra == padre)
            padre[1] = '\0';
        else
            *barra = '\0';

        if (stat(padre, &st) != 0 || !S_ISDIR(st.st_mode))
            ERR_ARG("el directorio '%s' del destino no existe", padre);
        if (access(padre, W_OK | X_OK) != 0)
            ERR_ARG("el directorio '%s' del destino no es escribible", padre);
    }

    if (n < 0 || (size_t)n >= sizeof cfg->ruta_final)
        ERR_ARG("la ruta de destino es demasiado larga");

    n = snprintf(cfg->ruta_parcial, sizeof cfg->ruta_parcial, "%s.part", cfg->ruta_final);
    if (n < 0 || (size_t)n >= sizeof cfg->ruta_parcial)
        ERR_ARG("la ruta de destino es demasiado larga");

    return 0;
}

/* 0 = válida, 1 = se pidió ayuda, -1 = error */
static int leer_config(int argc, char **argv, struct config *cfg)
{
    static const struct option largas[] = {
        {"host", required_argument, NULL, 'H'},
        {"puerto", required_argument, NULL, 'p'},
        {"accion", required_argument, NULL, 'a'},
        {"archivo", required_argument, NULL, 'f'},
        {"destino", required_argument, NULL, 'd'},
        {"tiempo-limite", required_argument, NULL, 't'},
        {"tiempo-canal", required_argument, NULL, 'c'},
        {"reproductor", required_argument, NULL, 'r'},
        {"sin-reproductor", no_argument, NULL, 'n'},
        {"ayuda", no_argument, NULL, 'h'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    const char *puerto_txt = NULL;
    long v;
    int c;

    cfg->host = NULL;
    cfg->accion = NULL;
    cfg->archivo = NULL;
    cfg->destino = NULL;
    cfg->tiempo_limite = 10;
    cfg->reproductor = NULL;
    cfg->sin_reproductor = 0;
    cfg->tiempo_canal = 0;

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
        case 'f':
            cfg->archivo = optarg;
            break;
        case 'd':
            cfg->destino = optarg;
            break;
        case 't':
            if (entero(optarg, 1, 3600, &v) != 0)
                ERR_ARG("--tiempo-limite debe ser un numero de segundos entre 1 y 3600 (recibido '%s')", optarg);
            cfg->tiempo_limite = (int)v;
            break;
        case 'c':
            if (entero(optarg, 1, 604800, &v) != 0)
                ERR_ARG("--tiempo-canal debe ser un numero de segundos entre 1 y 604800 (recibido '%s')", optarg);
            cfg->tiempo_canal = (int)v;
            break;
        case 'r':
            cfg->reproductor = optarg;
            break;
        case 'n':
            cfg->sin_reproductor = 1;
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

    if (cfg->reproductor != NULL && cfg->sin_reproductor)
        ERR_ARG("--reproductor y --sin-reproductor son contradictorios");

    if (cfg->reproductor != NULL && reproductor_buscar(cfg->reproductor) == NULL)
        ERR_ARG("--reproductor '%s' no se encontro", cfg->reproductor);

    if (strcmp(cfg->accion, "listar") == 0)
    {
        if (cfg->archivo != NULL || cfg->destino != NULL)
            ERR_ARG("--archivo y --destino solo se usan con la accion 'descargar'");
        if (cfg->tiempo_canal != 0)
            ERR_ARG("--tiempo-canal solo se usa con la accion 'canal'");
        if (cfg->reproductor != NULL || cfg->sin_reproductor)
            ERR_ARG("--reproductor y --sin-reproductor solo se usan con 'canal' o 'descargar'");
        return 0;
    }

    if (strcmp(cfg->accion, "descargar") == 0)
    {
        if (cfg->tiempo_canal != 0)
            ERR_ARG("--tiempo-canal solo se usa con la accion 'canal'");
        if (cfg->archivo == NULL)
            ERR_ARG("la accion 'descargar' requiere --archivo");
        if (cfg->destino == NULL)
            ERR_ARG("la accion 'descargar' requiere --destino");

        size_t alen = strlen(cfg->archivo);
        if (alen == 0 || alen > MAX_NOMBRE)
            ERR_ARG("--archivo debe tener entre 1 y %d caracteres", MAX_NOMBRE);
        if (cfg->archivo[0] == '.' || strchr(cfg->archivo, '/') != NULL || strchr(cfg->archivo, '\\') != NULL)
            ERR_ARG("--archivo '%s' no es un nombre de contenido valido (sin rutas ni nombres ocultos)", cfg->archivo);

        return resolver_destino(cfg);
    }

    if (strcmp(cfg->accion, "canal") == 0)
    {
        if (cfg->archivo != NULL || cfg->destino != NULL)
            ERR_ARG("--archivo y --destino solo se usan con la accion 'descargar'");
        return 0;
    }

    ERR_ARG("--accion '%s' no es valida; use canal, listar o descargar", cfg->accion);
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

static int fallo_recepcion(int r, const struct encabezado *enc)
{
    switch (r)
    {
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
}

static int mensaje_inesperado(const struct encabezado *enc, const char *buff)
{
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

static int recibir_esperado(int socket, uint16_t tipo_esperado, struct encabezado *enc, char *buff, uint32_t cap)
{
    int r = recibir_msg(socket, enc, buff, cap);
    if (r != PROTOCOL_OK)
        return fallo_recepcion(r, enc);

    if (enc->tipo == tipo_esperado)
        return 0;

    return mensaje_inesperado(enc, buff);
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

static int escribir_todo(int fd, const char *p, size_t n)
{
    while (n > 0)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void progreso(uint64_t recibidos, uint64_t total)
{
    if (total > 0)
        printf("\r  %llu / %llu bytes (%3u%%)", (unsigned long long)recibidos, (unsigned long long)total, (unsigned)(recibidos * 100 / total));
    else
        printf("\r  0 / 0 bytes");
    fflush(stdout);
}

static void iniciar_reproductor(struct reproductor *rep, const struct config *cfg)
{
    if (cfg->sin_reproductor)
        return;

    if (reproductor_iniciar(rep, cfg->reproductor) != 0)
        log_error("no se encontro un reproductor (mpv, ffplay, vlc, mplayer); se recibira sin reproducir");
    else
        log_info("reproductor '%s' iniciado", rep->nombre);
}

static int64_t ahora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int accion_descargar(int socket, const struct config *cfg, struct reproductor *rep)
{
    struct encabezado enc;
    int rc = SALIDA_INTERNO;
    int fd = -1;
    int creado = 0;
    char meta[64];
    char *bloque = malloc(TAM_BLOQUE);

    if (bloque == NULL)
    {
        log_error("sin memoria");
        return SALIDA_INTERNO;
    }

    size_t nlen = strlen(cfg->archivo);
    if (enviar_msg(socket, ABRIR, OK, ++g_id, cfg->archivo, (uint32_t)nlen) != PROTOCOL_OK)
    {
        log_error("no se pudo enviar la solicitud ABRIR");
        rc = SALIDA_RED;
        goto fin;
    }

    log_info("solicitud ABRIR '%s' enviada", cfg->archivo);

    rc = recibir_esperado(socket, ACEPTADO, &enc, meta, sizeof meta - 1);
    if (rc != 0)
        goto fin;

    meta[enc.longitud] = '\0';
    char *fin_num;
    errno = 0;
    unsigned long long total_ull = strtoull(meta, &fin_num, 10);
    if (meta[0] == '\0' || *fin_num != '\0' || errno != 0)
    {
        log_error("el servidor anuncio un tamaño invalido");
        rc = SALIDA_INTERNO;
        goto fin;
    }

    uint64_t total = (uint64_t)total_ull;
    log_info("el servidor acepto la solicitud: %llu bytes", total_ull);

    fd = open(cfg->ruta_parcial, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        log_error("no se pudo crear '%s': %s", cfg->ruta_parcial, strerror(errno));
        rc = SALIDA_INTERNO;
        goto fin;
    }
    creado = 1;

    iniciar_reproductor(rep, cfg);

    uint64_t recibidos = 0;
    progreso(recibidos, total);

    while (recibidos < total)
    {
        if (g_interrumpido)
        {
            printf("\n");
            log_error("descarga interrumpida por el usuario");
            rc = SALIDA_INTERRUMPIDO;
            goto fin;
        }

        int r = recibir_msg(socket, &enc, bloque, TAM_BLOQUE);
        if (r != PROTOCOL_OK)
        {
            printf("\n");
            rc = g_interrumpido ? SALIDA_INTERRUMPIDO : fallo_recepcion(r, &enc);
            goto fin;
        }

        if (enc.tipo != DATOS)
        {
            printf("\n");
            rc = mensaje_inesperado(&enc, bloque);
            goto fin;
        }

        if (recibidos + enc.longitud > total)
        {
            printf("\n");
            log_error("el servidor envio mas bytes de los anunciados");
            rc = SALIDA_INTERNO;
            goto fin;
        }

        if (escribir_todo(fd, bloque, enc.longitud) != 0)
        {
            printf("\n");
            log_error("error al escribir en '%s': %s", cfg->ruta_parcial, strerror(errno));
            rc = SALIDA_INTERNO;
            goto fin;
        }

        if (rep->fd >= 0 && reproductor_escribir(rep, bloque, enc.longitud) != 0)
            log_info("el reproductor se cerro; la descarga continua");

        recibidos += enc.longitud;
        progreso(recibidos, total);
    }

    printf("\n");

    rc = recibir_esperado(socket, FIN, &enc, bloque, TAM_BLOQUE);
    if (rc != 0)
        goto fin;

    if (recibidos != total)
    {
        log_error("discrepancia: se recibieron %llu bytes y se anunciaron %llu", (unsigned long long)recibidos, (unsigned long long)total);
        rc = SALIDA_INTERNO;
        goto fin;
    }

    if (close(fd) != 0)
    {
        fd = -1;
        log_error("error al cerrar '%s': %s", cfg->ruta_parcial, strerror(errno));
        rc = SALIDA_INTERNO;
        goto fin;
    }

    fd = -1;

    if (rename(cfg->ruta_parcial, cfg->ruta_final) != 0)
    {
        log_error("no se pudo mover a '%s': %s", cfg->ruta_final, strerror(errno));
        rc = SALIDA_INTERNO;
        goto fin;
    }

    creado = 0;
    log_info("descarga completa: %llu bytes guardados en '%s'", (unsigned long long)recibidos, cfg->ruta_final);
    rc = SALIDA_OK;

fin:
    if (fd >= 0)
        close(fd);
    if (creado)
        unlink(cfg->ruta_parcial); // no dejar parciales confundibles con una descarga completa
    free(bloque);
    return rc;
}

// Busca "clave = valor" en el string
static long long meta_valor(const char *txt, const char *clave)
{
    size_t kl = strlen(clave);
    const char *p = txt;
    while (*p != '\0')
    {
        if (strncmp(p, clave, kl) == 0 && p[kl] == '=')
        {
            char *fin;
            errno = 0;
            long long v = strtoll(p + kl + 1, &fin, 10);
            return (errno == 0 && fin != p + kl + 1) ? v : -1;
        }
        p = strchr(p, '\n');
        if (p == NULL)
            break;
        p++;
    }
    return -1;
}

enum motivo_salida
{
    MOTIVO_USUARIO,
    MOTIVO_TIEMPO,
    MOTIVO_REPRODUCTOR
};

static int accion_canal(int socket, const struct config *cfg, struct reproductor *rep)
{
    struct encabezado enc;
    int rc = SALIDA_INTERNO;
    char meta[512];
    char *bloque = malloc(TAM_BLOQUE);

    if (bloque == NULL)
    {
        log_error("sin memoria");
        return SALIDA_INTERNO;
    }

    int uno = 1;
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &uno, sizeof uno);

    if (enviar_msg(socket, UNIR, OK, ++g_id, NULL, 0) != PROTOCOL_OK)
    {
        log_error("no se pudo enviar la solicitud UNIR");
        rc = SALIDA_RED;
        goto fin;
    }
    
    log_info("solicitud UNIR enviada");

    rc = recibir_esperado(socket, EMISION, &enc, meta, sizeof meta - 1);
    if (rc != 0)
        goto fin;

    meta[enc.longitud] = '\0';

    long long pos_ms = meta_valor(meta, "posicion_ms");
    long long ciclo_ms = meta_valor(meta, "ciclo_ms");
    long long contenido_ms = meta_valor(meta, "contenido_ms");
    long long sep_ms = meta_valor(meta, "separacion_ms");
    long long elementos = meta_valor(meta, "elementos");
    if (pos_ms < 0 || ciclo_ms <= 0 || contenido_ms < 0 || sep_ms < 0)
    {
        log_error("el servidor envio metadatos de canal invalidos");
        rc = SALIDA_INTERNO;
        goto fin;
    }

    log_info("incorporado al canal en la posicion %lld.%03lld s (ciclo de %lld.%03lld s, %lld elemento(s), separacion %lld.%03lld s)",
             pos_ms / 1000, pos_ms % 1000, ciclo_ms / 1000, ciclo_ms % 1000, elementos, sep_ms / 1000, sep_ms % 1000);
    if (pos_ms >= contenido_ms)
        log_info("el canal esta en la separacion entre ciclos: la emision empieza en ~%lld s", (ciclo_ms - pos_ms + 999) / 1000);

    iniciar_reproductor(rep, cfg);
    int usa_rep = rep->pid > 0;

    int64_t limite = cfg->tiempo_canal > 0 ? ahora_ms() + (int64_t)cfg->tiempo_canal * 1000 : 0;
    int64_t proximo_informe = ahora_ms() + 2000;
    uint64_t recibidos = 0;
    enum motivo_salida motivo;

    while (1)
    {
        if (g_interrumpido)
        {
            motivo = MOTIVO_USUARIO;
            break;
        }
        if (limite != 0 && ahora_ms() >= limite)
        {
            motivo = MOTIVO_TIEMPO;
            break;
        }
        if (usa_rep && !reproductor_vivo(rep))
        {
            motivo = MOTIVO_REPRODUCTOR;
            break;
        }

        if (ahora_ms() >= proximo_informe)
        {
            printf("\r  canal: %.2f MB recibidos", (double)recibidos / (1024.0 * 1024.0));
            fflush(stdout);
            proximo_informe = ahora_ms() + 2000;
        }

        struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
        int pr = poll(&p, 1, 500);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;

            printf("\n");
            log_error("error esperando datos del canal: %s", strerror(errno));
            rc = SALIDA_RED;
            goto fin;
        }

        if (pr == 0)
            continue;

        int r = recibir_msg(socket, &enc, bloque, TAM_BLOQUE);
        if (r != PROTOCOL_OK)
        {
            printf("\n");
            rc = fallo_recepcion(r, &enc);
            goto fin;
        }

        switch (enc.tipo)
        {
        case DATOS:
            recibidos += enc.longitud;
            if (usa_rep)
                reproductor_escribir(rep, bloque, enc.longitud);
            break;

        case FIN:
            printf("\n");
            log_info("el servidor finalizo la transmision del canal");
            g_flujo_sincronizado = 1;
            rc = SALIDA_OK;
            goto fin;

        case DESPEDIDA:
            printf("\n");
            log_error("el servidor se esta deteniendo: %.*s", (int)enc.longitud, bloque);
            rc = SALIDA_RED;
            goto fin;

        default:
            printf("\n");
            rc = mensaje_inesperado(&enc, bloque);
            goto fin;
        }
    }

    printf("\n");
    switch (motivo)
    {
    case MOTIVO_USUARIO:
        log_info("saliendo del canal por orden del usuario");
        break;
    case MOTIVO_TIEMPO:
        log_info("se cumplio el tiempo de canal solicitado (%d s)", cfg->tiempo_canal);
        break;
    case MOTIVO_REPRODUCTOR:
        log_info("el reproductor se cerro; saliendo del canal");
        break;
    }

    if (enviar_msg(socket, SALIR, OK, ++g_id, NULL, 0) != PROTOCOL_OK)
    {
        rc = SALIDA_RED;
        goto fin;
    }

    // leer los datos que queden
    int64_t limite_fin = ahora_ms() + 3000;
    while (ahora_ms() < limite_fin)
    {
        struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
        int pr = poll(&p, 1, 200);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        if (recibir_msg(socket, &enc, bloque, TAM_BLOQUE) != PROTOCOL_OK)
            break;
        if (enc.tipo == FIN)
        {
            g_flujo_sincronizado = 1;
            break;
        }
        if (enc.tipo != DATOS)
            break;
    }

    rc = motivo == MOTIVO_USUARIO ? SALIDA_INTERRUMPIDO : SALIDA_OK;

fin:
    free(bloque);
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
        fprintf(stderr, "Forma de uso: %s --host DIRECCION --puerto N --accion canal|listar|descargar   (use --ayuda para mas detalles)\n", argv[0]);
        return SALIDA_ARGS;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = manejador_senal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

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

    struct reproductor rep = REPRODUCTOR_INICIAL;
    int es_descarga = strcmp(cfg.accion, "descargar") == 0;

    if (es_descarga)
        rc = accion_descargar(socket, &cfg, &rep);
    else if (strcmp(cfg.accion, "canal") == 0)
        rc = accion_canal(socket, &cfg, &rep);
    else
        rc = accion_listar(socket);

    if (rc != SALIDA_RED && rc != SALIDA_INTERNO && (rc != SALIDA_INTERRUMPIDO || g_flujo_sincronizado))
    {
        if (enviar_msg(socket, CERRAR, OK, ++g_id, NULL, 0) == PROTOCOL_OK)
        {
            char buff[128];
            recibir_msg(socket, &enc, buff, sizeof buff);
        }
    }
    close(socket);

    if (rc == SALIDA_OK)
        log_info("operacion completada");

    // descarga completa, dejar que el reproductor termine de mostrar lo recibido
    int esperar = es_descarga && rc == SALIDA_OK && rep.fd >= 0;
    if (esperar)
        log_info("esperando a que el reproductor termine");

    reproductor_cerrar(&rep, esperar);

    return rc;
}