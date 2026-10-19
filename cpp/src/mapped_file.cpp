// Maps a whole file into memory with mmap, so the parser can read it like an array.

#include "lob/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace lob {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw_errno("open " + path.string());

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        throw_errno("fstat " + path.string());
    }

    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ > 0) {
        void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            throw_errno("mmap " + path.string());
        }
        // The parser reads front to back: ask the kernel for aggressive read-ahead.
        ::madvise(addr, size_, MADV_SEQUENTIAL);
        data_ = static_cast<const std::uint8_t*>(addr);
    }
    // The mapping stays valid after the descriptor is closed.
    ::close(fd);
}

MappedFile::~MappedFile() { release(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void MappedFile::release() noexcept {
    if (data_ != nullptr) {
        ::munmap(const_cast<std::uint8_t*>(data_), size_);
        data_ = nullptr;
        size_ = 0;
    }
}

}  // namespace lob
