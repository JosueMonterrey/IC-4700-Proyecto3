#define _POSIX_C_SOURCE 200809L

#include "channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

static uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c)
{
    return (uint64_t)((unsigned __int128)a * b / c);
}

int64_t canal_ahora_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int es_ts(const char *ruta)
{
    if (ruta == NULL)
        return 0;
    size_t n = strlen(ruta);
    return n > 3 && strcmp(ruta + n - 3, ".ts") == 0;
}


uint64_t canal_byte_en(const struct elemento_canal *el, int64_t offset_ns)
{
    if (offset_ns <= 0 || el->duracion_ns <= 0)
        return 0;
    if (offset_ns >= el->duracion_ns)
        return el->tamano ? el->tamano - 1 : 0;

    uint64_t b = mul_div(el->tamano, (uint64_t)offset_ns, (uint64_t)el->duracion_ns);
    if (es_ts(el->ruta))
        b -= b % 188;
    return b;
}

int64_t canal_instante_byte(const struct elemento_canal *el, uint64_t byte)
{
    if (el->tamano == 0)
        return el->inicio_ns;
    return el->inicio_ns + (int64_t)mul_div(byte, (uint64_t)el->duracion_ns, el->tamano);
}

int canal_preparar(struct canal *c)
{
    int64_t acum = 0;
    for (size_t i = 0; i < c->cantidad; i++)
    {
        c->elementos[i].inicio_ns = acum;
        acum += c->elementos[i].duracion_ns;
    }
    c->contenido_ns = acum;
    c->ciclo_ns = acum + c->separacion_ns;
    return c->ciclo_ns > 0 ? 0 : -1;
}

void canal_iniciar(struct canal *c)
{
    c->t0_ns = canal_ahora_ns();
}

void canal_liberar(struct canal *c)
{
    if (c == NULL)
        return;
    for (size_t i = 0; i < c->cantidad; i++)
        free(c->elementos[i].ruta);
    free(c->elementos);
    memset(c, 0, sizeof *c);
}

void canal_posicion(const struct canal *c, int64_t ahora_ns, struct posicion_canal *p)
{
    int64_t t = ahora_ns - c->t0_ns;
    if (t < 0)
        t = 0;

    memset(p, 0, sizeof *p);
    p->ciclo = (uint64_t)(t / c->ciclo_ns);
    p->pos_ns = t % c->ciclo_ns;

    if (p->pos_ns >= c->contenido_ns)
    {
        p->en_separacion = 1;
        return;
    }

    for (size_t i = 0; i < c->cantidad; i++)
    {
        const struct elemento_canal *el = &c->elementos[i];
        if (p->pos_ns < el->inicio_ns + el->duracion_ns)
        {
            p->indice = i;
            p->offset_ns = p->pos_ns - el->inicio_ns;
            p->byte = canal_byte_en(el, p->offset_ns);
            return;
        }
    }
}

static int64_t duracion_ffprobe(const char *ruta)
{
    int fd[2];
    if (pipe(fd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fd[0]);
        close(fd[1]);
        return -1;
    }

    if (pid == 0)
    {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
        {
            dup2(nul, STDERR_FILENO);
            close(nul);
        }
        execlp("ffprobe", "ffprobe", "-v", "error", "-show_entries", "format=duration",
               "-of", "default=noprint_wrappers=1:nokey=1", "-i", ruta, (char *)NULL);
        _exit(127);
    }

    close(fd[1]);
    char buf[64];
    size_t n = 0;
    while (n < sizeof buf - 1)
    {
        ssize_t r = read(fd[0], buf + n, sizeof buf - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd[0]);
    buf[n] = '\0';

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return -1;

    char *fin;
    double d = strtod(buf, &fin);
    if (fin == buf || !(d > 0.0) || d > 3.0e6)
        return -1;
    return (int64_t)(d * 1e9);
}

int canal_construir(struct canal *c, const char *const *rutas, size_t n,
                    double duracion_total_s, double separacion_s)
{
    memset(c, 0, sizeof *c);
    if (n == 0 || n > MAX_ELEMENTOS_CANAL)
        return -1;

    c->elementos = calloc(n, sizeof *c->elementos);
    if (c->elementos == NULL)
    {
        log_error("canal: sin memoria");
        return -1;
    }

    uint64_t bytes_total = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct stat st;
        if (stat(rutas[i], &st) != 0 || !S_ISREG(st.st_mode) || access(rutas[i], R_OK) != 0)
        {
            log_error("canal: '%s' no existe o no es un archivo legible", rutas[i]);
            goto fallo;
        }
        if (st.st_size == 0)
        {
            log_error("canal: '%s' esta vacio", rutas[i]);
            goto fallo;
        }

        char *copia = strdup(rutas[i]);
        if (copia == NULL)
        {
            log_error("canal: sin memoria");
            goto fallo;
        }
        c->elementos[i].ruta = copia;
        c->elementos[i].tamano = (uint64_t)st.st_size;
        c->cantidad = i + 1;
        bytes_total += (uint64_t)st.st_size;
    }

    if (duracion_total_s > 0.0)
    {
        // Duracion declarada: se reparte proporcional al tamano
        int64_t total_ns = (int64_t)(duracion_total_s * 1e9);
        int64_t asignado = 0;
        for (size_t i = 0; i < n; i++)
        {
            int64_t d = (i + 1 == n) ? total_ns - asignado
                                     : (int64_t)mul_div((uint64_t)total_ns, c->elementos[i].tamano, bytes_total);
            if (d <= 0)
            {
                log_error("canal: --duracion-canal es demasiado pequena para %zu elemento(s)", n);
                goto fallo;
            }
            c->elementos[i].duracion_ns = d;
            asignado += d;
        }
    }
    else
    {
        for (size_t i = 0; i < n; i++)
        {
            int64_t d = duracion_ffprobe(c->elementos[i].ruta);
            if (d <= 0)
            {
                log_error("canal: no se pudo determinar la duracion de '%s' (¿ffprobe instalado?). "
                          "Use --duracion-canal", c->elementos[i].ruta);
                goto fallo;
            }
            c->elementos[i].duracion_ns = d;
        }
    }

    c->separacion_ns = (int64_t)(separacion_s * 1e9);
    if (canal_preparar(c) != 0)
        goto fallo;

    c->t0_ns = canal_ahora_ns();
    return 0;

fallo:
    canal_liberar(c);
    return -1;
}