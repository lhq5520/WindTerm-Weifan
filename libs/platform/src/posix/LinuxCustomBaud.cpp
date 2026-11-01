// Arbitrary baud rates on Linux (termios2 with BOTHER). Lives in its own file because
// <asm/termbits.h> redefines struct termios from <termios.h>.

#if defined(__linux__)

#include <asm/termbits.h>
#include <sys/ioctl.h>

#include <cerrno>

namespace wt::platform {

int setLinuxCustomBaudRate(int fd, int rate) {
    struct termios2 tio {};
    if (::ioctl(fd, TCGETS2, &tio) != 0) {
        return errno;
    }
    tio.c_cflag &= ~static_cast<tcflag_t>(CBAUD | (CBAUD << IBSHIFT));
    tio.c_cflag |= BOTHER | (BOTHER << IBSHIFT);
    tio.c_ispeed = static_cast<speed_t>(rate);
    tio.c_ospeed = static_cast<speed_t>(rate);
    if (::ioctl(fd, TCSETS2, &tio) != 0) {
        return errno;
    }
    return 0;
}

} // namespace wt::platform

#endif
