vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libffi/libffi
    REF v3.5.2
    SHA512 4579932becbe33b2cb3c7a6327a9b47fee67f225ebb4677870ed4402bb7c186966a5b8645dc8a09128af51dcba27c23537e6a34567dbea4e3dc3728cfb51e038
    HEAD_REF master
)

# -------------------------------
# Windows build (clang-cl / MSVC ABI)
# -------------------------------
if(VCPKG_TARGET_IS_WINDOWS)

  message(STATUS "libffi: Windows clang-cl build (MSVC ABI)")

  set(OBJ_DIR ${CURRENT_BUILDTREES_DIR}/objs)
  file(MAKE_DIRECTORY ${OBJ_DIR})

  # Core sources (minimal but correct set for x64)
  set(LIBFFI_SOURCES
        ${SOURCE_PATH}/src/prep_cif.c
        ${SOURCE_PATH}/src/types.c
        ${SOURCE_PATH}/src/raw_api.c
        ${SOURCE_PATH}/src/java_raw_api.c
        ${SOURCE_PATH}/src/closures.c

        ${SOURCE_PATH}/src/x86/ffi.c
        ${SOURCE_PATH}/src/x86/ffiw64.c
    )

  set(OBJECTS)

  foreach(src IN LISTS LIBFFI_SOURCES)

    get_filename_component(fname ${src} NAME_WE)
    set(obj ${OBJ_DIR}/${fname}.obj)

    add_custom_command(
            OUTPUT ${obj}
            COMMAND ${VCPKG_CMAKE_C_COMPILER}
                /nologo
                /c ${src}
                /Fo${obj}
                /I${SOURCE_PATH}/include
                /I${SOURCE_PATH}/src
            DEPENDS ${src}
        )

    list(APPEND OBJECTS ${obj})

  endforeach()

  add_custom_target(libffi_objs ALL DEPENDS ${OBJECTS})

  # Create .lib
  set(LIB_DIR ${CURRENT_PACKAGES_DIR}/lib)
  file(MAKE_DIRECTORY ${LIB_DIR})

  set(LIB_PATH ${LIB_DIR}/libffi.lib)

  add_custom_command(
        TARGET libffi_objs POST_BUILD
        COMMAND lib /nologo /OUT:${LIB_PATH} ${OBJECTS}
    )

  # Install headers
  file(INSTALL
        ${SOURCE_PATH}/include/
        DESTINATION ${CURRENT_PACKAGES_DIR}/include
        FILES_MATCHING PATTERN "*.h"
    )

  # -------------------------------
  # Linux / POSIX build
  # -------------------------------
else()

  message(STATUS "libffi: POSIX autotools build")

  vcpkg_configure_make(
        SOURCE_PATH ${SOURCE_PATH}
        AUTOCONFIG
    )

  vcpkg_install_make()

endif()

# Fixups
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
vcpkg_fixup_cmake_targets()

# License
file(INSTALL ${SOURCE_PATH}/LICENSE
     DESTINATION ${CURRENT_PACKAGES_DIR}/share/libffi
     RENAME copyright)
