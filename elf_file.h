#ifndef ELF_FILE_H
#define ELF_FILE_H

#include <string>
#include <vector>
#include <span>

#include <elf.h>

class safe_map {
    int _fd = 0;
    void* _map = nullptr;
    uint64_t _size = 0;

    public:
    safe_map(const char* path);
    safe_map(void* map, uint64_t size);

    safe_map(const safe_map&) = delete;
    safe_map& operator=(const safe_map&) = delete;

    safe_map(safe_map&& other) noexcept;
    safe_map& operator=(safe_map&& other) noexcept;

    ~safe_map();

    void* map(uint64_t off = 0) const;
    int fd() const;

    private:
    void _unload();
};

class elf_file {
    safe_map _map;

    std::vector<safe_map> _programs;

    public:
    explicit elf_file(const std::string& path);

    std::span<const safe_map> programs() const;
    uintptr_t entry() const;

    private:
    void _load_programs();
};

#endif /* ELF_FILE_H */
