/*
 * Aegir Datatypes implementation.
 */

#include <aegir/datatypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace aegir::datatypes {

// FileStream
FileStream::FileStream(std::string_view path, bool write)
    : write_(write) {
    fd_ = open(std::string(path).c_str(), write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0644);
}

FileStream::~FileStream() {
    if (fd_ >= 0) close(fd_);
}

bool FileStream::read(void* buffer, size_t size, size_t& bytes_read) {
    if (fd_ < 0 || write_) { bytes_read = 0; return false; }
    ssize_t n = ::read(fd_, buffer, size);
    if (n < 0) { bytes_read = 0; return false; }
    bytes_read = static_cast<size_t>(n);
    return true;
}

bool FileStream::write(const void* buffer, size_t size, size_t& bytes_written) {
    if (fd_ < 0 || !write_) { bytes_written = 0; return false; }
    ssize_t n = ::write(fd_, buffer, size);
    if (n < 0) { bytes_written = 0; return false; }
    bytes_written = static_cast<size_t>(n);
    return true;
}

bool FileStream::seek(int64_t offset, int whence) {
    if (fd_ < 0) return false;
    off_t pos = lseek(fd_, offset, whence);
    return pos != -1;
}

int64_t FileStream::tell() {
    if (fd_ < 0) return -1;
    return lseek(fd_, 0, SEEK_CUR);
}

int64_t FileStream::size() {
    if (fd_ < 0) return -1;
    off_t cur = lseek(fd_, 0, SEEK_CUR);
    off_t end = lseek(fd_, 0, SEEK_END);
    lseek(fd_, cur, SEEK_SET);
    return end;
}

// MemoryStream
MemoryStream::MemoryStream(std::vector<uint8_t>&& data)
    : data_(std::move(data)) {}

bool MemoryStream::read(void* buffer, size_t size, size_t& bytes_read) {
    if (pos_ >= data_.size()) { bytes_read = 0; return false; }
    size_t available = data_.size() - pos_;
    size_t to_read = std::min(size, available);
    std::memcpy(buffer, data_.data() + pos_, to_read);
    pos_ += to_read;
    bytes_read = to_read;
    return true;
}

bool MemoryStream::write(const void* buffer, size_t size, size_t& bytes_written) {
    if (pos_ + size > data_.size()) {
        data_.resize(pos_ + size);
    }
    std::memcpy(data_.data() + pos_, buffer, size);
    pos_ += size;
    bytes_written = size;
    return true;
}

bool MemoryStream::seek(int64_t offset, int whence) {
    size_t new_pos;
    if (whence == SEEK_SET) {
        new_pos = static_cast<size_t>(offset);
    } else if (whence == SEEK_CUR) {
        new_pos = pos_ + static_cast<size_t>(offset);
    } else if (whence == SEEK_END) {
        new_pos = data_.size() + static_cast<size_t>(offset);
    } else {
        return false;
    }
    if (new_pos > data_.size()) return false;
    pos_ = new_pos;
    return true;
}

int64_t MemoryStream::tell() {
    return static_cast<int64_t>(pos_);
}

int64_t MemoryStream::size() {
    return static_cast<int64_t>(data_.size());
}

} // namespace aegir::datatypes