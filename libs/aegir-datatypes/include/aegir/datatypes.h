/*
 * Aegir Datatypes - MIME-based datatype system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Inspired by Amiga Datatypes: load/save data by MIME type.
 */

#ifndef AEGIR_DATATYPES_H
#define AEGIR_DATATYPES_H

#include <aegir/trinket/unicode.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aegir::datatypes {

class Stream;

// Base class for datatype loaders
class Datatype {
public:
    virtual ~Datatype() = default;

    // MIME type this datatype handles (e.g., "image/png", "text/plain")
    virtual std::string mime_type() const = 0;

    // Human-readable name
    virtual std::string name() const = 0;

    // Load object from stream
    // Returns true on success, false on failure
    virtual bool load(Stream& stream, void*& object_out) = 0;

    // Save object to stream
    virtual bool save(Stream& stream, void* object) = 0;

    // Create new empty object of this type
    virtual void* create_object() = 0;

    // Destroy object
    virtual void destroy_object(void* object) = 0;
};

// Stream abstraction for loading/saving
class Stream {
public:
    virtual ~Stream() = default;

    // Read bytes
    virtual bool read(void* buffer, size_t size, size_t& bytes_read) = 0;

    // Write bytes
    virtual bool write(const void* buffer, size_t size, size_t& bytes_written) = 0;

    // Seek
    virtual bool seek(int64_t offset, int whence) = 0;

    // Tell position
    virtual int64_t tell() = 0;

    // Get size
    virtual int64_t size() = 0;
};

// File-based stream
class FileStream : public Stream {
public:
    explicit FileStream(std::string_view path, bool write = false);
    ~FileStream() override;

    bool read(void* buffer, size_t size, size_t& bytes_read) override;
    bool write(const void* buffer, size_t size, size_t& bytes_written) override;
    bool seek(int64_t offset, int whence) override;
    int64_t tell() override;
    int64_t size() override;

private:
    int fd_ = -1;
    bool write_;
};

// Memory stream
class MemoryStream : public Stream {
public:
    MemoryStream() = default;
    explicit MemoryStream(std::vector<uint8_t>&& data);
    ~MemoryStream() override;

    bool read(void* buffer, size_t size, size_t& bytes_read) override;
    bool write(const void* buffer, size_t size, size_t& bytes_written) override;
    bool seek(int64_t offset, int whence) override;
    int64_t tell() override;
    int64_t size() override;

    std::vector<uint8_t>& data() { return data_; }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPES_H