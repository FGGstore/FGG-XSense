#include "lock.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

int lock_take(void)
{
    struct stat stv;
    int fd;

    if (stat(LOCK_PATH, &stv) == 0 &&
        time(NULL) - stv.st_mtime < LOCK_STALE_SECONDS)
        return 0;

    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void lock_refresh(void) { utimes(LOCK_PATH, NULL); }
void lock_release(void) { unlink(LOCK_PATH); }
