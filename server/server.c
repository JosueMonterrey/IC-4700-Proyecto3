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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "catalog.h"
#include "channel.h"
#include "../common/exitcodes.h"
#include "../common/log.h"
#include "../common/protocol.h"
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
    int max_clientes;
    const char *canal[MAX_ELEMENTOS_CANAL];
    size_t n_canal;
    double duracion_canal; // segundos
    double separacion;     // segundos entre ciclos
};

static const char *nombre_modo(enum modo m)
{
    switch (m)
    {
    case FIFO:
        return "FIFO";
    case FORKED:
        return "FORKED";
    default:
        return "THREADED";
    }
}

static void ayuda(FILE *out, const char *prog)
{
    fprintf(out,
            "Uso: %s --modo MODO --puerto N --dir RUTA [opciones]\n"
            "\n"
            "Servidor de video. Atiende clientes por TCP.\n"
            "\n"
            "Obligatorios:\n"
            "  --modo MODO          Modo de concurrencia: FIFO | FORKED | THREADED\n"
            "                       (por ahora estan implementados FIFO y FORKED)\n"
            "  --puerto N           Puerto de escucha (1-65535)\n"
            "  --dir RUTA           Directorio con los videos del catalogo\n"
            "  --canal ARCHIVO      Contenido del canal en vivo. Se puede repetir para formar una\n"
            "                       lista ordenada (maximo %d). Se recomienda MPEG-TS (.ts)\n"
            "\n"
            "Opcionales:\n"
            "  --direccion IP       Direccion IPv4 donde escuchar (por omision: 0.0.0.0,\n"
            "                       todas las interfaces)\n"
            "  --inactividad S      Segundos de silencio antes de cerrar una sesion\n"
            "                       (por omision: 60)\n"
            "  --profundidad N      Conexiones pendientes admitidas en la cola\n"
            "                       (por omision: 16)\n"
            "  --max-clientes N     Maximo de clientes activos simultaneos en modo FORKED\n"
            "                       (por omision: 64). Los excedentes reciben error de capacidad\n"
            "  --duracion-canal S   Duracion total declarada del contenido del canal, en segundos.\n"
            "                       Si se omite, se consulta a ffprobe archivo por archivo\n"
            "  --separacion S       Segundos sin contenido entre el final de un ciclo y el inicio\n"
            "                       del siguiente (por omision: 0)\n"
            "  --ayuda, -h          Muestra esta ayuda\n"
            "\n"
            "Ejemplos:\n"
            "  %s --modo FIFO --puerto 8080 --dir videos/ --canal canal.ts\n"
            "  %s --modo FORKED --puerto 9000 --dir /srv/videos --canal a.ts --canal b.ts \\\n"
            "       --duracion-canal 600 --separacion 5 --direccion 127.0.0.1 --max-clientes 20\n"
            "\n"
            "Codigos de salida: 0 exito, 2 argumentos, 3 red, 5 capacidad, 6 interno.\n",
            prog, MAX_ELEMENTOS_CANAL, prog, prog);
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

static int decimal(const char *txt, double min, double max, double *out)
{
    char *fin;
    errno = 0;
    double v = strtod(txt, &fin);
    if (txt[0] == '\0' || *fin != '\0' || errno != 0 || !(v >= min && v <= max))
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
        {"max-clientes", required_argument, NULL, 'x'},
        {"canal", required_argument, NULL, 'c'},
        {"duracion-canal", required_argument, NULL, 'u'},
        {"separacion", required_argument, NULL, 's'},
        {"ayuda", no_argument, NULL, 'h'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    const char *modo_txt = NULL, *puerto_txt = NULL;
    long v;
    double dv;
    int c;

    cfg->dir = NULL;
    cfg->direccion = "0.0.0.0";
    cfg->inactividad = 60;
    cfg->profundidad = 16;
    cfg->max_clientes = 64;
    cfg->n_canal = 0;
    cfg->duracion_canal = 0;
    cfg->separacion = 0;

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
        case 'x':
            if (entero(optarg, 1, 4096, &v) != 0)
                return error_arg("--max-clientes debe ser un entero entre 1 y 4096 (recibido '%s')", optarg);
            cfg->max_clientes = (int)v;
            break;
        case 'c':
            if (cfg->n_canal >= MAX_ELEMENTOS_CANAL)
                return error_arg("--canal admite como maximo %d archivos", MAX_ELEMENTOS_CANAL);
            if (optarg[0] == '\0')
                return error_arg("--canal no puede estar vacio");
            cfg->canal[cfg->n_canal++] = optarg;
            break;
        case 'u':
            if (decimal(optarg, 1, 604800, &dv) != 0)
                return error_arg("--duracion-canal debe ser un numero de segundos entre 1 y 604800 (recibido '%s')", optarg);
            cfg->duracion_canal = dv;
            break;
        case 's':
            if (decimal(optarg, 0, 86400, &dv) != 0)
                return error_arg("--separacion debe ser un numero de segundos entre 0 y 86400 (recibido '%s')", optarg);
            cfg->separacion = dv;
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

    if (cfg->n_canal == 0)
        return error_arg("falta --canal (archivo del canal en vivo)");

    if (strcmp(modo_txt, "FIFO") == 0)
        cfg->modo = FIFO;
    else if (strcmp(modo_txt, "FORKED") == 0)
        cfg->modo = FORKED;
    else if (strcmp(modo_txt, "THREADED") == 0)
        cfg->modo = THREADED;
    else
        return error_arg("--modo '%s' no es valido. Permitido: FIFO, FORKED o THREADED", modo_txt);

    if (cfg->modo == THREADED)
        return error_arg("el modo THREADED todavia no esta implementado en esta version");

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

// MODO FORKED
struct hijos
{
    pid_t *pids;
    size_t cap;
    size_t activos;
};

static int hijos_iniciar(struct hijos *h, size_t cap)
{
    h->pids = calloc(cap, sizeof *h->pids);
    h->cap = h->pids ? cap : 0;
    h->activos = 0;
    return h->pids ? 0 : -1;
}

static int hijos_agregar(struct hijos *h, pid_t pid)
{
    for (size_t i = 0; i < h->cap; i++)
    {
        if (h->pids[i] == 0)
        {
            h->pids[i] = pid;
            h->activos++;
            return 0;
        }
    }
    return -1;
}

static void hijos_quitar(struct hijos *h, pid_t pid)
{
    for (size_t i = 0; i < h->cap; i++)
    {
        if (h->pids[i] == pid)
        {
            h->pids[i] = 0;
            h->activos--;
            return;
        }
    }
}

// quitar hijos que terminaron
static void hijos_recolectar(struct hijos *h)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
    {
        hijos_quitar(h, pid);
        if (WIFSIGNALED(st))
            log_error("proceso hijo %ld termino por la señal %d", (long)pid, WTERMSIG(st));
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            log_error("proceso hijo %ld termino con codigo %d", (long)pid, WEXITSTATUS(st));
    }
}

static void hijos_terminar(struct hijos *h)
{
    if (h->activos == 0)
        return;

    log_info("deteniendo %zu proceso(s) hijo...", h->activos);
    for (size_t i = 0; i < h->cap; i++)
        if (h->pids[i] > 0)
            kill(h->pids[i], SIGTERM);

    int64_t limite = canal_ahora_ns() + 5000000000LL; // +5s
    while (h->activos > 0 && canal_ahora_ns() < limite)
    {
        hijos_recolectar(h);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
        nanosleep(&ts, NULL);
    }

    if (h->activos > 0)
    {
        log_error("%zu hijo(s) no concluyeron a tiempo: se fuerza su cierre", h->activos);
        for (size_t i = 0; i < h->cap; i++)
        {
            if (h->pids[i] > 0)
            {
                kill(h->pids[i], SIGKILL);
                int st;
                while (waitpid(h->pids[i], &st, 0) < 0 && errno == EINTR)
                {
                }
                h->pids[i] = 0;
                h->activos--;
            }
        }
    }
}

static void rechazar_capacidad(int cs, const char *origen, int max)
{
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct encabezado enc;
    char buff[512];
    uint32_t id = 0;
    if (recibir_msg(cs, &enc, buff, sizeof buff) == PROTOCOL_OK)
        id = enc.id_solicitud;

    const char *txt = "el servidor alcanzo su limite de clientes; intente mas tarde";
    enviar_msg(cs, ERROR, CAPACIDAD, id, txt, (uint32_t)strlen(txt));
    log_error("[%s] rechazado: limite de %d clientes activos alcanzado", origen, max);
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
        fprintf(stderr, "Forma de uso: %s --modo FIFO|FORKED --puerto N --dir RUTA --canal ARCHIVO   (use --ayuda para mas detalles)\n", argv[0]);
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

    // 3. Canal en vivo
    struct canal canal;
    int hay_canal = (canal_construir(&canal, cfg.canal, cfg.n_canal, cfg.duracion_canal, cfg.separacion) == 0);
    if (!hay_canal)
        log_error("el canal en vivo no sera iniciado");

    // 4. Iniciar servidor
    instalar_senales();
    int socket = crear_escucha(cfg.direccion, cfg.puerto, cfg.profundidad);
    if (socket < 0)
    {
        if (errno == EADDRINUSE)
            log_error("el puerto %d ya esta ocupado", cfg.puerto);
        else
            log_error("no se pudo escuchar en %s:%d: %s", cfg.direccion, cfg.puerto, strerror(errno));
        catalogo_liberar(&cat);
        if (hay_canal)
            canal_liberar(&canal);
        return SALIDA_RED;
    }

    struct hijos hijos = {NULL, 0, 0};
    if (cfg.modo == FORKED && hijos_iniciar(&hijos, (size_t)cfg.max_clientes) != 0)
    {
        log_error("sin memoria para procesos hijo");
        close(socket);
        catalogo_liberar(&cat);
        if (hay_canal)
            canal_liberar(&canal);

        return SALIDA_INTERNO;
    }

    if (hay_canal)
        canal_iniciar(&canal); // t0

    log_info("Servidor iniciado. Modo=%s, escuchando en %s:%d", nombre_modo(cfg.modo), cfg.direccion, cfg.puerto);
    log_info("Catalogo: %zu entradas (%zu disponibles). Inactividad=%d s, cola=%d", cat.cantidad, disponibles, cfg.inactividad, cfg.profundidad);
    
    if (cfg.modo == FORKED)
        log_info("Maximo de clientes activos: %d", cfg.max_clientes);

    if (hay_canal)
        log_info("Canal en vivo: %zu elemento(s), contenido %.1f s, separacion %.1f s, ciclo %.1f s", canal.cantidad, (double)canal.contenido_ns / 1e9, (double)canal.separacion_ns / 1e9, (double)canal.ciclo_ns / 1e9);

    // 5. Bucle principal: admitir -> atender (segun el modo)
    unsigned long atendidas = 0, rechazadas = 0;

    while (!detencion_solicitada())
    {
        if (cfg.modo == FORKED)
            hijos_recolectar(&hijos);

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

        if (cfg.modo == FIFO)
        {
            log_info("[%s] conexion admitida (FIFO: se atiende hasta concluir)", origen);

            atender_sesion(client_socket, &cat, hay_canal ? &canal : NULL, origen, cfg.inactividad);
            close(client_socket);

            atendidas++;
            log_info("[%s] conexion cerrada; el servidor queda libre para la siguiente", origen);
            continue;
        }

        // ---- FORKED ----
        hijos_recolectar(&hijos);

        if (hijos.activos >= (size_t)cfg.max_clientes)
        {
            rechazar_capacidad(client_socket, origen, cfg.max_clientes);
            close(client_socket);
            rechazadas++;
            continue;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            log_error("[%s] fork() fallo: %s", origen, strerror(errno));
            const char *txt = "error interno del servidor";
            enviar_msg(client_socket, ERROR, INTERNO, 0, txt, (uint32_t)strlen(txt));
            close(client_socket);
            rechazadas++;
            continue;
        }

        if (pid == 0)
        {
            // hijo solo conserva el socket de su cliente
            close(socket);
            free(hijos.pids);

            atender_sesion(client_socket, &cat, hay_canal ? &canal : NULL, origen, cfg.inactividad);
            close(client_socket);
            log_info("[%s] conexion cerrada (proceso hijo %ld termina)", origen, (long)getpid());
            _exit(0);
        }

        // proceso padre
        close(client_socket);
        if (hijos_agregar(&hijos, pid) != 0)
            log_error("tabla de hijos inconsistente (pid %ld)", (long)pid);

        atendidas++;
        log_info("[%s] conexion admitida (FORKED: hijo %ld, %zu/%d activos)", origen, (long)pid, hijos.activos, cfg.max_clientes);
    }

    // 6. Cerrar servidor
    close(socket);
    
    if (cfg.modo == FORKED)
    {
        hijos_terminar(&hijos);
        free(hijos.pids);
    }

    catalogo_liberar(&cat);
    if (hay_canal)
        canal_liberar(&canal);
    log_info("Servidor detenido de forma ordenada. Sesiones atendidas: %lu, rechazadas: %lu", atendidas, rechazadas);

    return SALIDA_OK;
}