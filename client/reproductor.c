#define _POSIX_C_SOURCE 200809L

#include "reproductor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const CONOCIDOS[] = {"mpv", "ffplay", "vlc", "mplayer", NULL};

static const char *buscar_uno(const char *nombre, char *out, size_t cap)
{
    if (strchr(nombre, '/') != NULL)
    {
        if (access(nombre, X_OK) != 0)
            return NULL;
        snprintf(out, cap, "%s", nombre);
        return out;
    }

    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";

    while (*path != '\0')
    {
        size_t n = strcspn(path, ":");
        if (n > 0 && n < PATH_MAX - 1)
        {
            snprintf(out, cap, "%.*s/%s", (int)n, path, nombre);
            if (access(out, X_OK) == 0)
                return out;
        }
        path += n;
        if (*path == ':')
            path++;
    }
    return NULL;
}

const char *reproductor_buscar(const char *nombre)
{
    char tmp[PATH_MAX + 64];
    if (nombre != NULL)
        return buscar_uno(nombre, tmp, sizeof tmp) ? nombre : NULL;

    for (int i = 0; CONOCIDOS[i] != NULL; i++)
        if (buscar_uno(CONOCIDOS[i], tmp, sizeof tmp))
            return CONOCIDOS[i];
    return NULL;
}

int reproductor_iniciar(struct reproductor *r, const char *nombre)
{
    r->pid = -1;
    r->fd = -1;
    r->nombre = reproductor_buscar(nombre);
    if (r->nombre == NULL)
        return -1;

    int p[2];
    if (pipe(p) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(p[0]);
        close(p[1]);
        return -1;
    }

    if (pid == 0)
    {
        signal(SIGPIPE, SIG_DFL);
        dup2(p[0], STDIN_FILENO);

        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
        {
            dup2(nul, STDOUT_FILENO);
            dup2(nul, STDERR_FILENO);
        }

        // no heredar el socket ni archivos del cliente
        for (int fd = 3; fd < 1024; fd++)
            close(fd);

        // execlp(r->nombre, r->nombre, "-", (char *)NULL);
        const char *base = strrchr(r->nombre, '/');
        base = base ? base + 1 : r->nombre;

        if (strcmp(base, "mpv") == 0)
            execlp(r->nombre, r->nombre,
                   "--demuxer-lavf-probesize=32768",
                   "--demuxer-lavf-analyzeduration=0.5",
                   "-", (char *)NULL);
        else if (strcmp(base, "ffplay") == 0)
            execlp(r->nombre, r->nombre,
                   "-probesize", "32768",
                   "-analyzeduration", "500000",
                   "-fflags", "nobuffer",
                   "-i", "-", (char *)NULL);

        execlp(r->nombre, r->nombre, "-", (char *)NULL);
        _exit(127);
    }

    close(p[0]);
    r->pid = pid;
    r->fd = p[1];
    return 0;
}

int reproductor_escribir(struct reproductor *r, const void *buf, size_t n)
{
    if (r->fd < 0)
        return -1;

    const char *p = buf;
    while (n > 0)
    {
        ssize_t w = write(r->fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            close(r->fd); // el reproductor termino
            r->fd = -1;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int reproductor_vivo(struct reproductor *r)
{
    if (r->pid <= 0)
        return 0;
    if (r->fd < 0)
        return 0;

    int st;
    pid_t w = waitpid(r->pid, &st, WNOHANG);
    if (w == r->pid || (w < 0 && errno == ECHILD))
    {
        r->pid = -1;
        close(r->fd);
        r->fd = -1;
        return 0;
    }
    return 1;
}

void reproductor_cerrar(struct reproductor *r, int esperar)
{
    if (r->fd >= 0)
    {
        close(r->fd);
        r->fd = -1;
    }
    if (r->pid > 0)
    {
        if (!esperar)
            kill(r->pid, SIGTERM);

        int st;
        while (waitpid(r->pid, &st, 0) < 0 && errno == EINTR)
            ;
        r->pid = -1;
    }
}