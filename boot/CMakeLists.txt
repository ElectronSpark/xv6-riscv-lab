# ==============================================================================
# U-Boot script compilation
# ==============================================================================
# Compile .cmd scripts to .scr format using mkimage
# These are architecture-independent and can be compiled on any host

find_program(MKIMAGE mkimage)

if(MKIMAGE)
    message(STATUS "Found mkimage: ${MKIMAGE}")
    
    # List of U-Boot scripts to compile
    set(UBOOT_SCRIPTS
        ${CMAKE_CURRENT_SOURCE_DIR}/boot.cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/xv6.cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/xv6-tftp.cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/default.cmd
    )
    
    set(UBOOT_SCR_FILES)
    
    foreach(CMD_FILE ${UBOOT_SCRIPTS})
        if(EXISTS ${CMD_FILE})
            get_filename_component(SCRIPT_NAME ${CMD_FILE} NAME_WE)
            set(SCR_FILE ${CMAKE_CURRENT_BINARY_DIR}/${SCRIPT_NAME}.scr)
            
            add_custom_command(
                OUTPUT ${SCR_FILE}
                COMMAND ${MKIMAGE} -C none -A riscv -T script -d ${CMD_FILE} ${SCR_FILE}
                DEPENDS ${CMD_FILE}
                COMMENT "Compiling U-Boot script: ${SCRIPT_NAME}.cmd -> ${SCRIPT_NAME}.scr"
            )
            
            list(APPEND UBOOT_SCR_FILES ${SCR_FILE})
        endif()
    endforeach()
    
    # Target to compile all U-Boot scripts
    add_custom_target(uboot-scripts
        DEPENDS ${UBOOT_SCR_FILES}
        COMMENT "Compiling U-Boot scripts (.cmd -> .scr)"
    )
    
    # Export variables to parent scope
    set(UBOOT_SCR_FILES ${UBOOT_SCR_FILES} PARENT_SCOPE)
    set(MKIMAGE_FOUND TRUE PARENT_SCOPE)
else()
    message(STATUS "mkimage not found - U-Boot script compilation disabled")
    message(STATUS "  Install with: sudo apt-get install u-boot-tools")
    set(MKIMAGE_FOUND FALSE PARENT_SCOPE)
endif()

# ==============================================================================
# Deployment target (Orange Pi only)
# ==============================================================================
# Deploy to Orange Pi via scp
# Usage: cmake -DPLATFORM=orangepi -DORANGEPI_IP=192.168.0.201 .. && make deploy

if(PLATFORM STREQUAL "orangepi")
    set(ORANGEPI_IP "192.168.0.201" CACHE STRING "Orange Pi IP address for deployment")
    set(ORANGEPI_USER "root" CACHE STRING "Orange Pi SSH user for deployment")
    set(ORANGEPI_PASS "orangepi" CACHE STRING "Orange Pi SSH password for deployment")
    set(ORANGEPI_BOOT_DIR "/boot" CACHE STRING "Orange Pi boot directory")

    # Find sshpass for password-based authentication
    find_program(SSHPASS sshpass)
    if(SSHPASS)
        set(SCP_CMD ${SSHPASS} -p ${ORANGEPI_PASS} scp -o StrictHostKeyChecking=no)
        set(SSH_CMD ${SSHPASS} -p ${ORANGEPI_PASS} ssh -o StrictHostKeyChecking=no)
        message(STATUS "Found sshpass - deploy will use password authentication")
    else()
        set(SCP_CMD scp)
        set(SSH_CMD ssh)
        message(STATUS "sshpass not found - deploy will prompt for password (install with: sudo apt-get install sshpass)")
    endif()

    add_custom_target(deploy
        COMMAND ${SCP_CMD} ${CMAKE_BINARY_DIR}/kernel/${KERNEL_BIN} ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/
        COMMAND ${SCP_CMD} ${CMAKE_BINARY_DIR}/kernel/${KERNEL_SYM_DEPLOY} ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/
        COMMAND ${SCP_CMD} ${CMAKE_CURRENT_BINARY_DIR}/boot.scr ${CMAKE_CURRENT_BINARY_DIR}/xv6.scr ${CMAKE_CURRENT_BINARY_DIR}/xv6-tftp.scr ${CMAKE_CURRENT_BINARY_DIR}/default.scr ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/
        COMMAND ${SSH_CMD} ${ORANGEPI_USER}@${ORANGEPI_IP} sync
        COMMENT "Deploying to Orange Pi at ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/"
    )
    add_dependencies(deploy kernel_all fs_img)

    message(STATUS "Deploy target: scp to ${ORANGEPI_USER}@${ORANGEPI_IP}:${ORANGEPI_BOOT_DIR}/")
endif()
