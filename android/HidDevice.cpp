#include "HidDevice.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Enumeration via sysfs
// ---------------------------------------------------------------------------

std::vector<std::string> HidDevice::Enumerate(uint16_t vid, uint16_t pid,
                                               uint16_t usagePage)
{
    std::vector<std::string> paths;
    DIR* dir = opendir("/sys/class/hidraw");
    if (!dir)
        return paths;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        // Read the uevent file from the parent hid device
        std::string ueventPath = std::string("/sys/class/hidraw/")
                                 + entry->d_name + "/device/uevent";
        FILE* f = fopen(ueventPath.c_str(), "r");
        if (!f)
            continue;

        uint16_t foundVid = 0;
        uint16_t foundPid = 0;
        uint16_t foundUsagePage = 0;
        char line[256];

        while (fgets(line, sizeof(line), f)) {
            unsigned v, p;
            if (sscanf(line, "HID_ID=%*X:%X:%X", &v, &p) == 2) {
                foundVid = static_cast<uint16_t>(v);
                foundPid = static_cast<uint16_t>(p);
            }
            unsigned up;
            if (sscanf(line, "HID_PAGE=%X", &up) == 1) {
                foundUsagePage = static_cast<uint16_t>(up);
            }
        }
        fclose(f);

        if (foundVid != vid || (pid != 0 && foundPid != pid))
            continue;
        if (usagePage != 0 && foundUsagePage != usagePage)
            continue;

        paths.push_back(std::string("/dev/") + entry->d_name);
    }
    closedir(dir);
    return paths;
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

bool HidDevice::Open(const std::string& path)
{
    Close();

    m_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (m_fd < 0) {
        perror("hidraw open");
        return false;
    }

    struct hidraw_devinfo info;
    if (ioctl(m_fd, HIDIOCGRAWINFO, &info) < 0) {
        perror("HIDIOCGRAWINFO");
        Close();
        return false;
    }

    printf("  opened %s  (bus=%04X  vid=%04X  pid=%04X)\n",
           path.c_str(), info.bustype, info.vendor, info.product);
    return true;
}

void HidDevice::Close()
{
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// Feature reports (control endpoint)
// ---------------------------------------------------------------------------

bool HidDevice::SendFeatureReport(const uint8_t* data, size_t size)
{
    if (m_fd < 0)
        return false;

    // HIDIOCSFEATURE(size) encodes the size in the ioctl command.
    // The buffer must have the report ID at index 0.
    int ret = ioctl(m_fd, HIDIOCSFEATURE(size), data);
    if (ret < 0) {
        fprintf(stderr, "HIDIOCSFEATURE(0x%02X) failed: ", data[0]);
        perror("");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input reports
// ---------------------------------------------------------------------------

size_t HidDevice::ReadInputReport(uint8_t* buffer, size_t size,
                                   uint32_t timeoutMs)
{
    if (m_fd < 0)
        return 0;

    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (ret <= 0)
        return 0;

    ssize_t n = read(m_fd, buffer, size);
    if (n < 0) {
        if (errno != EAGAIN) {
            // Fatal I/O error (EIO, ENODEV, etc.) — device disappeared
            Close();
        }
        return 0;
    }
    return static_cast<size_t>(n);
}
