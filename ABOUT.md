# About This Enhanced xv6 Project

## Relationship to Original xv6

This operating system is a **derivative work** based on the original xv6 operating system created by Frans Kaashoek, Robert Morris, and Russ Cox at MIT PDOS (Parallel and Distributed Operating Systems group).

### What is xv6?

xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix Version 6 (v6). The original xv6 was created as a teaching operating system for MIT's 6.1810 (formerly 6.828) Operating System Engineering course. It loosely follows the structure and style of Unix v6, but is implemented for modern RISC-V multiprocessors using ANSI C.

### What is This Enhanced Version?

This repository contains an **extensively modified and enhanced version** of xv6. While it maintains the xv6 foundation, it includes substantial new features and architectural changes:

- **Virtual File System (VFS)** layer replacing the original monolithic file system
- **Multiple file system support** (xv6fs, tmpfs)
- **Advanced memory management** (slab allocator, enhanced page cache, dynamic page arrays)
- **Modern synchronization primitives** (rwlocks, completions, two-level VM locking)
- **CMake build system** replacing the original Makefile
- **Device driver framework** for modular device support
- **Extended networking** capabilities
- **Hierarchical mounting** and advanced file system operations
- **Multi-platform support**: QEMU virt machine and Orange Pi RV2 (Allwinner D1)
- **Runtime device discovery**: Dynamic MMIO addresses instead of compile-time constants
- **SBI console**: Early boot output via OpenSBI before UART initialization

**These enhancements were developed independently and are not part of the original xv6 project.**

## Important Distinctions

### Authorship and Credit

- **Original xv6**: Created by Frans Kaashoek, Robert Morris, and Russ Cox
- **Original xv6 Contributors**: See [XV6_ORIGINAL_README](XV6_ORIGINAL_README) for complete list
- **Enhancements**: Developed independently as a derivative work

### Purpose and Scope

- **Original xv6**: Teaching operating system focusing on simplicity and clarity
- **Enhanced Version**: Extended for more advanced educational use and experimental features

### Maintenance and Support

- **Original xv6**: Maintained by MIT PDOS, focused on teaching goals
- **Enhanced Version**: Independent project, separate maintenance and development

## Using This Documentation

### For Original xv6 Information

If you're looking for information about the **original xv6** project:

1. Read [XV6_ORIGINAL_README](XV6_ORIGINAL_README) for original project details
2. Visit the MIT 6.1810 course website: https://pdos.csail.mit.edu/6.1810/
3. Read the xv6 book: https://pdos.csail.mit.edu/6.828/2023/xv6/book-riscv-rev3.pdf
4. Contact the original authors: Frans Kaashoek and Robert Morris (kaashoek, rtm@mit.edu)

### For Enhanced Version Information

If you're interested in the **enhancements and modifications**:

1. Read [README.md](README.md) for overview of enhanced features
2. See [kernel/vfs/VFS_DESIGN.md](kernel/vfs/VFS_DESIGN.md) for VFS architecture
3. Browse the Doxygen-generated documentation (run `doxygen` to generate)
4. Examine individual source files for implementation details

**Important**: Questions about enhanced features should NOT be directed to the original xv6 authors. They are not responsible for modifications in this derivative work.

## License and Copyright

### Original xv6

```
The code in the files that constitute xv6 is
Copyright 2006-2024 Frans Kaashoek, Robert Morris, and Russ Cox.
```

The original xv6 license permits modification and redistribution. See the [LICENSE](LICENSE) file for complete terms.

### Enhanced Version

Enhancements and modifications in this repository are provided under the same license terms as the original xv6. By using this enhanced version, you agree to:

1. Respect the original xv6 copyright and license
2. Acknowledge xv6 as the foundation of this work
3. Not imply endorsement by the original xv6 authors
4. Understand that enhancements are provided as-is without warranty

## Educational Use

### For Students

If you're using this for educational purposes:

- **For MIT 6.1810**: Use the **original xv6**, not this enhanced version
- **For other OS courses**: Consult with your instructor about using this enhanced version
- **For self-study**: This enhanced version provides more advanced OS concepts beyond basic xv6

### For Instructors

If you're considering this for teaching:

- This enhanced version is more complex than original xv6
- It demonstrates more production-like OS architecture
- The VFS layer and advanced features require more background knowledge
- Multi-platform support (QEMU and Orange Pi RV2) shows real hardware porting techniques
- Original xv6 may be more appropriate for introductory OS courses

## Contributing

### To Original xv6

To contribute to the **original xv6** project, visit:
- https://github.com/mit-pdos/xv6-riscv
- Contact MIT PDOS per their guidelines

### To This Enhanced Version

Contributions to this enhanced version should:
1. Maintain separation from original xv6 project
2. Follow existing architectural patterns
3. Include appropriate documentation
4. Respect the original xv6 license and attribution

## Summary

- ✅ **This IS**: A derivative work based on xv6 with extensive enhancements
- ✅ **This IS**: Licensed under the same terms as original xv6  
- ✅ **This IS**: Suitable for advanced OS education and experimentation
- ❌ **This IS NOT**: The official xv6 project
- ❌ **This IS NOT**: Endorsed by the original xv6 authors
- ❌ **This IS NOT**: Supported by MIT PDOS

## Acknowledgment

We gratefully acknowledge the original xv6 project and its creators. xv6 has been an invaluable educational resource and provided an excellent foundation for this enhanced operating system. The clarity and simplicity of the original xv6 design made these extensions possible.

**Original xv6 Website**: https://pdos.csail.mit.edu/6.1810/  
**Original xv6 Repository**: https://github.com/mit-pdos/xv6-riscv

---

*This document clarifies the relationship between this enhanced version and the original xv6 project to avoid confusion and ensure proper attribution.*
