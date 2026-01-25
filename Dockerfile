
# Build stage for the RISC-V GNU Toolchain
FROM ubuntu:22.04 AS builder

# Set noninteractive installation
ENV DEBIAN_FRONTEND=noninteractive

# Install packages needed for building the RISC-V toolchain
RUN apt-get update && \
    apt-get install -y \
    autoconf automake autotools-dev curl python3 python3-pip \
    libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex \
    texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev \
    ninja-build git cmake libglib2.0-dev libslirp-dev \
    && rm -rf /var/lib/apt/lists/*

# Build RISC-V GNU Toolchain with newlib
WORKDIR /tmp
RUN git clone --recursive https://github.com/riscv/riscv-gnu-toolchain && \
    cd riscv-gnu-toolchain && \
    mkdir build && cd build && \
    ../configure --prefix=/opt/riscv && \
    make -j$(nproc)

# Final stage
FROM ubuntu:24.04

# Set noninteractive installation
ENV DEBIAN_FRONTEND=noninteractive

# Install development packages
RUN apt-get update && \
    apt-get install -y \
    # Build essentials
    build-essential make cmake ninja-build \
    # Version control
    git \
    # Python and tools
    python3 python3-pip python3-venv \
    # QEMU for RISC-V
    qemu-system-misc \
    # Debugging
    gdb-multiarch \
    # U-Boot tools (for mkimage)
    u-boot-tools \
    # Deployment tools
    sshpass openssh-client \
    # Documentation
    doxygen graphviz \
    # Utilities
    curl wget file \
    # Editor (optional, lightweight)
    vim nano \
    # Terminal utilities
    tmux htop \
    # Network utilities
    netcat-openbsd iputils-ping \
    && rm -rf /var/lib/apt/lists/*

# Copy the built RISC-V toolchain from the builder stage
COPY --from=builder /opt/riscv /opt/riscv

# Add RISC-V tools to PATH
ENV PATH="/opt/riscv/bin:${PATH}"

# Create a directory for mounting the xv6 project
WORKDIR /xv6

# Set bash as the default shell
SHELL ["/bin/bash", "-c"]

# Create helper script for xv6
RUN echo '#!/bin/bash\n\
echo "xv6 RISC-V Development Environment"\n\
echo "--------------------------------"\n\
echo "Available commands:"\n\
echo "  mkdir build && cd build && cmake .. - Configure build"\n\
echo "  make -j$(nproc)                      - Build xv6"\n\
echo "  make qemu                            - Run xv6 in QEMU"\n\
echo "  make clean                           - Clean the build"\n\
echo ""\n\
echo "Environment variables:"\n\
echo "  LAB=$LAB (set with: export LAB=<lab>)"\n\
echo "  PLATFORM=qemu|orangepi"\n\
' > /usr/local/bin/xv6-help && \
chmod +x /usr/local/bin/xv6-help

# Create a non-root user
RUN useradd -m -s /bin/bash xv6user && \
    chown -R xv6user:xv6user /xv6

# Make the help script accessible to the non-root user
RUN chmod +x /usr/local/bin/xv6-help

# Switch to the non-root user
USER xv6user

# Default command when container starts
CMD ["/bin/bash", "-c", "echo 'xv6 RISC-V Development Environment. Type xv6-help for info.'; bash"]

# Note: To run this container, use:
# docker build -t xv6-riscv .
# docker run -it --rm -v $(pwd):/xv6 xv6-riscv