#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

extern "C" int test_fsync(int fd) {
  const char *marker = std::getenv("VELOCACHE_TEST_FSYNC_MARKER");
  if (marker && access(marker, F_OK) == 0) {
    const char *started = std::getenv("VELOCACHE_TEST_FSYNC_STARTED");
    if (started) {
      int signal = open(started, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (signal >= 0)
        close(signal);
    }
    usleep(800000);
    const char *failure = std::getenv("VELOCACHE_TEST_FSYNC_FAILURE");
    if (failure && access(failure, F_OK) == 0) {
      errno = EIO;
      return -1;
    }
  }
  return fsync(fd);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct {
  const void *replacement;
  const void *original;
} interposed[] = {{reinterpret_cast<const void *>(test_fsync),
                   reinterpret_cast<const void *>(fsync)}};
