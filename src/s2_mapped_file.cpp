#include "../include/s2_mapped_file.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace s2 {

bool MappedFile::open(const std::string& path) {
    close();
    
#ifdef _WIN32
    // GGUF is mostly sequential
    HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(fh, &file_size)) { 
        CloseHandle(fh); 
        return false; 
    }
    size_ = static_cast<size_t>(file_size.QuadPart);
    
    if (size_ == 0) { 
        CloseHandle(fh); 
        return true;
    }
    
    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) { 
        CloseHandle(fh); 
        return false; 
    }
    
    void* addr = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, size_);
    if (!addr) { 
        CloseHandle(mh); 
        CloseHandle(fh); 
        return false; 
    }
    
    data_ = addr; 
    file_handle_ = static_cast<void*>(fh); 
    mapping_handle_ = static_cast<void*>(mh);
    
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) { 
        ::close(fd); 
        return false; 
    }
    size_ = static_cast<size_t>(st.st_size);
    
    if (size_ == 0) { 
        ::close(fd); 
        return true;
    }
    
    void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { 
        ::close(fd); 
        return false; 
    }
    
    // GGUF is mostly sequential
    ::madvise(addr, size_, MADV_SEQUENTIAL);

#ifdef __linux__
    // Huge pages reduce TLB pressure during multi-GB weight loads.
    ::madvise(addr, size_, MADV_HUGEPAGE);
    // Exclude from core dumps so we don't write entire model to disk in a crash.
    ::madvise(addr, size_, MADV_DONTDUMP);
#endif
    
    data_ = addr; 
    fd_ = fd;
#endif

    return true;
}

void MappedFile::close() {
#ifdef _WIN32
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (data_ && data_ != MAP_FAILED) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    size_ = 0;
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
#ifdef _WIN32
    data_ = other.data_; 
    other.data_ = nullptr; 
    size_ = other.size_; 
    other.size_ = 0;
    file_handle_ = other.file_handle_; 
    other.file_handle_ = nullptr;
    mapping_handle_ = other.mapping_handle_; 
    other.mapping_handle_ = nullptr;
#else
    data_ = other.data_; 
    other.data_ = nullptr; 
    size_ = other.size_; 
    other.size_ = 0; 
    fd_ = other.fd_; 
    other.fd_ = -1;
#endif
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) { 
        close();
#ifdef _WIN32
        data_ = other.data_; 
        other.data_ = nullptr; 
        size_ = other.size_; 
        other.size_ = 0;
        file_handle_ = other.file_handle_; 
        other.file_handle_ = nullptr;
        mapping_handle_ = other.mapping_handle_; 
        other.mapping_handle_ = nullptr;
#else
        data_ = other.data_; 
        other.data_ = nullptr; 
        size_ = other.size_; 
        other.size_ = 0; 
        fd_ = other.fd_; 
        other.fd_ = -1;
#endif
    }
    return *this;
}

void MappedFile::drop_page_cache() {
#if defined(__linux__) || defined(__APPLE__)
    if (data_ && data_ != MAP_FAILED && size_ > 0) {
        ::madvise(data_, size_, MADV_DONTNEED);
    }
    if (fd_ >= 0) {
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
    }
#elif defined(_WIN32)
    if (data_ && size_ > 0) {
        ::VirtualUnlock(data_, size_);
    }
#endif
}

}
