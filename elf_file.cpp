#include "elf_file.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <iostream>
#include <span>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <elf.h>

safe_map::safe_map(const char* path) {
    _fd = open(path, O_RDONLY);
    if (_fd < 0) {
        throw std::runtime_error("Could not open file");
    }

    struct stat st;
    if (fstat(_fd, &st) < 0) {
        close(_fd);
        throw std::runtime_error("Failed to stat file");
    }

    if (st.st_mode & S_IFDIR) {
        close(_fd);
        throw std::runtime_error("Attempting to open directory");
    }

    _size = st.st_size;
    _map = mmap(nullptr, _size, PROT_READ, MAP_PRIVATE, _fd, 0);
    if (_map == MAP_FAILED) {
        close(_fd);
        throw std::runtime_error("Failed to mmap");
    }
}

safe_map::safe_map(void* map, uint64_t size) : _map { map }, _size { size } {

}

safe_map::safe_map(safe_map&& other) noexcept : _map { other._map }, _size { other._size } {
    other._map = nullptr;
    other._size = 0;
}

safe_map& safe_map::operator=(safe_map&& other) noexcept {
    _unload();

    _map = std::exchange(other._map, nullptr);
    _size = std::exchange(other._size, 0);

    return *this;
}


safe_map::~safe_map() {
    _unload();
}

void* safe_map::map(uint64_t off) const {
    return static_cast<char*>(_map) + off;
}

int safe_map::fd() const {
    return _fd;
}

void safe_map::_unload() {
    if (_map) {
        munmap(_map, _size);
    }

    if (_fd) {
        close(_fd);
    }
}

elf_file::elf_file(const std::string& path) : _map { path.c_str() } {
    _load_programs();
}

std::span<const safe_map> elf_file::programs() const {
    return _programs;
}

uintptr_t elf_file::entry() const {
    const Elf64_Ehdr* elf = static_cast<Elf64_Ehdr*>(_map.map());

    return elf->e_entry;
}

void elf_file::_load_programs() {
    const Elf64_Ehdr* elf = static_cast<Elf64_Ehdr*>(_map.map());

    if (memcmp(elf->e_ident, ELFMAG, SELFMAG) != 0) {
        throw std::runtime_error("Invalid ELF identifier");
    }

    if (elf->e_ident[EI_VERSION] != EV_CURRENT) {
        throw std::runtime_error("Invalid ELF version");
    }

    if (elf->e_ident[EI_CLASS] != ELFCLASS64) {
        throw std::runtime_error("Unsupported ELF class");
    }

    if (elf->e_ident[EI_DATA] != ELFDATA2LSB) {
        throw std::runtime_error("Not an little-endian ELF");
    }

    if (elf->e_type != ET_EXEC) {
        throw std::runtime_error("Not an executable file");
    }

    if (elf->e_version != EV_CURRENT) {
        throw std::runtime_error("ELF version mismatch (weird)");
    }

    if (elf->e_machine != EM_RISCV) {
        throw std::runtime_error("Not a RISC-V ELF");
    }

    if (elf->e_phoff == 0) {
        throw std::runtime_error("No programs present");
    }

    std::span<const Elf64_Phdr> programs = std::span(
        static_cast<Elf64_Phdr*>(_map.map(elf->e_phoff)), elf->e_phnum);

    _programs.clear();

    for (const Elf64_Phdr& p : programs) {
        if (p.p_type == PT_LOAD) {
            int prot = 0;
            if (p.p_flags & PF_X) prot |= PROT_EXEC;
            if (p.p_flags & PF_W) prot |= PROT_WRITE;
            if (p.p_flags & PF_R) prot |= PROT_READ;

            uintptr_t target_addr = p.p_vaddr;

            /* Round down to page */
            uintptr_t page_size = sysconf(_SC_PAGESIZE);

            target_addr &= ~(page_size - 1);
            uintptr_t addr_offset = p.p_vaddr - target_addr;

            void* target_ptr = reinterpret_cast<void*>(target_addr);

            if (prot & PROT_WRITE) {
                /* MAP_ANONYMOUS and memcpy existing (possibly partial) data */
                void* map = mmap(target_ptr, p.p_memsz + addr_offset,
                                 prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                                 -1, 0);

                if (map != target_ptr) {
                    if (map != MAP_FAILED) {
                        munmap(map, p.p_filesz);
                    }
                    
                    throw std::runtime_error(std::string("Mapping failed: ")
                            + strerrorname_np(errno) + " - " + strerror(errno));
                }

                memcpy(reinterpret_cast<char*>(map) + addr_offset, _map.map(p.p_offset), p.p_filesz);
                _programs.emplace_back(map, p.p_memsz);
            } else {
                if (p.p_memsz != p.p_filesz) {
                    throw std::runtime_error("filesz != memsz on non-writable page");
                }

                if (addr_offset > 0) {
                    throw std::runtime_error("non-writable page must be page-aligned");
                }

                /* Map directly from the file */
                void* map = mmap(target_ptr, p.p_filesz,
                                 prot, MAP_PRIVATE | MAP_FIXED_NOREPLACE,
                                 _map.fd(), p.p_offset);

                if (map != target_ptr) {
                    if (map != MAP_FAILED) {
                        munmap(map, p.p_filesz);
                    }

                    throw std::runtime_error(std::string("Mapping failed: ")
                            + strerrorname_np(errno) + " - " + strerror(errno));
                }

                _programs.emplace_back(map, p.p_memsz);
            }
        }
    }
}
