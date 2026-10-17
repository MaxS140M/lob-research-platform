#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace lob {

// Read-only memory mapping of a whole file (POSIX mmap).
// The OS pages the file in on demand, so multi-gigabyte files are read without copying
// them into a user-space buffer first.
class MappedFile {
public:
    // Throws std::system_error if the file cannot be opened or mapped.
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    void release() noexcept;

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace lob
