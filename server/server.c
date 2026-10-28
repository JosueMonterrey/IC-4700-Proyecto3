#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "exitcodes.h"
#include "log.h"
#include "protocol.h"
#include "session.h"

enum modo
{
    FIFO,
    FORKED,
    THREADED
};

struct config
{
    enum modo modo;
    int puerto;
    const char *dir;
    const char *direccion;
    int inactividad;
    int profundidad;
};

static void ayuda(FILE *out, const char *prog)
{
    fprintf(out,
            "Uso: %s --modo MODO --puerto N --dir RUTA [opciones]\n"
            "\n"
            "Servidor de video. Atiende clientes por TCP.\n"
            "\n"
            "Obligatorios:\n"
            "  --modo MODO          Modo de concurrencia: FIFO | FORKED | THREADED\n"
            "                       (por ahora solo FIFO esta implementado)\n"
            "  --puerto N           Puerto de escucha (1-65535)\n"
            "  --dir RUTA           Directorio con los videos del catalogo\n"
            "\n"
            "Opcionales:\n"
            "  --direccion IP       Direccion IPv4 donde escuchar (por omision: 0.0.0.0,\n"
            "                       todas las interfaces)\n"
            "  --inactividad S      Segundos de silencio antes de cerrar una sesion\n"
            "                       (por omision: 60)\n"
            "  --profundidad N      Conexiones pendientes admitidas en la cola\n"
            "                       (por omision: 16)\n"
            "  --ayuda, -h          Muestra esta ayuda\n"
            "\n"
            "Ejemplos:\n"
            "  %s --modo FIFO --puerto 8080 --dir videos/\n"
            "  %s --modo FIFO --puerto 9000 --dir /srv/videos --direccion 127.0.0.1 --inactividad 30\n"
            "\n"
            "Codigos de salida: 0 exito, 2 argumentos, 3 red, 5 capacidad, 6 interno.\n",
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

static int error_arg(const char *formato, ...)
{
    va_list args;
    fprintf(stderr, "error: ");
    va_start(args, formato);
    vfprintf(stderr, formato, args);
    va_end(args);
    fprintf(stderr, "\n");
    return -1;
}

// config valido = 0, help = 1, error = -1
static int leer_config(int argc, char **argv, struct config *cfg)
{
    static const struct option args[] = {
        {"modo", required_argument, NULL, 'm'},
        {"puerto", required_argument, NULL, 'p'},
        {"dir", required_argument, NULL, 'd'},
        {"direccion", required_argument, NULL, 'a'},
        {"inactividad", required_argument, NULL, 'i'},
        {"profundidad", required_argument, NULL, 'q'},
        {"ayuda", no_argument, NULL, 'h'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    const char *modo_txt = NULL, *puerto_txt = NULL;
    long v;
    int c;

    cfg->dir = NULL;
    cfg->direccion = "0.0.0.0";
    cfg->inactividad = 60;
    cfg->profundidad = 16;

    opterr = 0;
    while ((c = getopt_long(argc, argv, "h", args, NULL)) != -1)
    {
        switch (c)
        {
        case 'm':
            modo_txt = optarg;
            break;
        case 'p':
            puerto_txt = optarg;
            break;
        case 'd':
            cfg->dir = optarg;
            break;
        case 'a':
            cfg->direccion = optarg;
            break;
        case 'i':
            if (entero(optarg, 1, 86400, &v) != 0)
                return error_arg("--inactividad debe ser un numero de segundos entre 1 y 86400 (recibido '%s')", optarg);
            cfg->inactividad = (int)v;
            break;
        case 'q':
            if (entero(optarg, 1, 1024, &v) != 0)
                return error_arg("--profundidad debe ser un entero entre 1 y 1024 (recibido '%s')", optarg);
            cfg->profundidad = (int)v;
            break;
        case 'h':
            return 1;
        default:
            return error_arg("argumento desconocido o sin valor: '%s'", argv[optind - 1]);
        }
    }

    if (optind < argc)
        return error_arg("argumento inesperado: '%s'", argv[optind]);

    if (modo_txt == NULL)
        return error_arg("falta --modo (FIFO, FORKED o THREADED)");

    if (puerto_txt == NULL)
        return error_arg("falta --puerto");

    if (cfg->dir == NULL)
        return error_arg("falta --dir (directorio de videos)");

    if (strcmp(modo_txt, "FIFO") == 0)
        cfg->modo = FIFO;
    else if (strcmp(modo_txt, "FORKED") == 0)
        cfg->modo = FORKED;
    else if (strcmp(modo_txt, "THREADED") == 0)
        cfg->modo = THREADED;
    else
        return error_arg("--modo '%s' no es valido. Permitido: FIFO, FORKED o THREADED", modo_txt);

    if (cfg->modo != FIFO)
        return error_arg("el modo %s todavia no esta implementado en esta version", modo_txt);

    if (entero(puerto_txt, 1, 65535, &v) != 0)
        return error_arg("--puerto debe ser un entero entre 1 y 65535 (recibido '%s')", puerto_txt);
    cfg->puerto = (int)v;

    struct in_addr tmp;
    if (inet_pton(AF_INET, cfg->direccion, &tmp) != 1)
        return error_arg("--direccion '%s' no es una direccion IPv4 valida", cfg->direccion);

    struct stat st;
    if (stat(cfg->dir, &st) != 0)
        return error_arg("--dir '%s' no se puede leer: %s", cfg->dir, strerror(errno));
        
    if (!S_ISDIR(st.st_mode))
        return error_arg("--dir '%s' no es un directorio", cfg->dir);

    return 0;
}

// crear socket para escuchar requests
static int crear_escucha(const char *direccion, int puerto, int profundidad)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;

    // para poder reiniciar el servidor sin esperar a que el puerto se libere
    int uno = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof saddr);
    saddr.sin_family = AF_INET;               // IPv4
    saddr.sin_port = htons((uint16_t)puerto); // 16 bit big endian
    inet_pton(AF_INET, direccion, &saddr.sin_addr);

    if (bind(s, (struct sockaddr *)&saddr, sizeof saddr) != 0 || listen(s, profundidad) != 0)
    {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }

    return s;
}

static void manejador_senal(int sig)
{
    (void)sig;
    detener_sesiones(); /* seguro dentro de un manejador */
}

static void instalar_senales(void)
{
    struct sigaction saddr;
    memset(&saddr, 0, sizeof saddr);
    saddr.sa_handler = manejador_senal;
    sigemptyset(&saddr.sa_mask);
    sigaction(SIGINT, &saddr, NULL);
    sigaction(SIGTERM, &saddr, NULL);
    signal(SIGPIPE, SIG_IGN);
}

// MAIN
int main(int argc, char **argv)
{
    struct config cfg;

    // 1. Leer argumentos y validar
    int r = leer_config(argc, argv, &cfg);
    if (r == 1)
    {
        ayuda(stdout, argv[0]);
        return SALIDA_OK;
    }
    if (r < 0)
    {
        fprintf(stderr, "Forma de uso: %s --modo FIFO --puerto N --dir RUTA   (use --ayuda para mas detalles)\n",
                argv[0]);
        return SALIDA_ARGS;
    }

    // 2. Generar catalogo
    struct catalogo cat;
    if (catalogo_construir(&cat, cfg.dir) != 0)
    {
        log_error("no se pudo leer el directorio '%s': %s", cfg.dir, strerror(errno));
        return SALIDA_ARGS;
    }
    size_t disponibles = catalogo_disponibles(&cat);
    if (disponibles == 0)
        log_error("el catalogo esta vacio: no hay contenido por el momento");

    // 3. Iniciar servidor
    instalar_senales();
    int socket = crear_escucha(cfg.direccion, cfg.puerto, cfg.profundidad);
    if (socket < 0)
    {
        if (errno == EADDRINUSE)
            log_error("el puerto %d ya esta ocupado", cfg.puerto);
        else
            log_error("no se pudo escuchar en %s:%d: %s", cfg.direccion, cfg.puerto, strerror(errno));
        catalogo_liberar(&cat);
        return SALIDA_RED;
    }

    log_info("Servidor iniciado. Modo=FIFO, escuchando en %s:%d", cfg.direccion, cfg.puerto);
    log_info("Catalogo: %zu entradas (%zu disponibles). Inactividad=%d s, cola=%d",
             cat.cantidad, disponibles, cfg.inactividad, cfg.profundidad);

    // 4. Bucle FIFO: admitir -> atender -> cerrar -> admitir
    unsigned long atendidas = 0;

    while (!detencion_solicitada())
    {
        /* Se espera con poll() para poder revisar la bandera de detención. */
        struct pollfd p = {.fd = socket, .events = POLLIN, .revents = 0};
        int n = poll(&p, 1, 500);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            log_error("error en poll(): %s", strerror(errno));
            break;
        }
        if (n == 0)
            continue;

        struct sockaddr_in client_addr;
        socklen_t len = sizeof client_addr;
        int client_socket = accept(socket, (struct sockaddr *)&client_addr, &len);
        if (client_socket < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN)
                continue;

            log_error("accept() fallo: %s", strerror(errno));
            sleep(1); // Delay
            continue;
        }

        char ip[INET_ADDRSTRLEN], origen[64];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof ip);   // client IPv4 legible

        snprintf(origen, sizeof origen, "%s:%u", ip, (unsigned)ntohs(client_addr.sin_port)); // log IPv4:port

        log_info("[%s] conexion admitida (FIFO: se atiende hasta concluir)", origen);

        atender_sesion(client_socket, &cat, origen, cfg.inactividad);
        close(client_socket);

        atendidas++;
        log_info("[%s] conexion cerrada; el servidor queda libre para la siguiente", origen);
    }

    // 5. Cerrar servidor
    close(socket);
    catalogo_liberar(&cat);
    log_info("Servidor detenido de forma ordenada. Sesiones atendidas: %lu", atendidas);
    return SALIDA_OK;
}