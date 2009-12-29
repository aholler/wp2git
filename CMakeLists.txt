cmake_minimum_required( VERSION 2.4 FATAL_ERROR )
IF(COMMAND cmake_policy)
    cmake_policy(SET CMP0003 NEW)
    # as info: cmake --help-policy CMP0003
ENDIF(COMMAND cmake_policy)

project (WP2GIT)

# This generates version.h
# git-version-gen.sh was shameless stolen and modified from
# git itself.
add_custom_target(version.h
    ${CMAKE_CURRENT_SOURCE_DIR}/git-version-gen.sh
        ${CMAKE_CURRENT_BINARY_DIR}/version.h
        ${CMAKE_CURRENT_SOURCE_DIR}/version
        ${CMAKE_CURRENT_SOURCE_DIR})

# Boost
IF (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} GREATER 2.5)
    SET(Boost_USE_STATIC_LIBS OFF)
    SET(Boost_USE_MULTITHREAD OFF)
    SET(Boost_ADDITIONAL_VERSIONS "1.38" "1.38.0" "1.39" "1.39.0" "1.40" "1.40.0" "1.41" "1.41.0")
    find_package( Boost 1.35.0 COMPONENTS program_options date_time iostreams)
    # TODO: Message if boost was not found
    MESSAGE ("Use something like")
    MESSAGE ("  BOOST_ROOT=/opt/boost_1_40_0 cmake -DCMAKE_BUILD_TYPE=release")
    MESSAGE ("if you have installed boost somewhere where it was not found.")
    INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIRS})
ELSE (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} GREATER 2.5)
    # TODO: code for cmake 2.4
    INCLUDE_DIRECTORIES(/usr/include/boost)
ENDIF (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} GREATER 2.5)

add_executable (wp2git
    wp2git.cpp
    expat/xmlparse.c
    expat/xmlrole.c
    expat/xmltok.c
    expat/xmltok_impl.c
    expat/xmltok_ns.c
)

# Dependencies to the generated version.h
ADD_DEPENDENCIES(wp2git version.h)
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/version.h PROPERTIES GENERATED 1)
SET_SOURCE_FILES_PROPERTIES(wp2git.cpp PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/version.h)

SET_TARGET_PROPERTIES(wp2git PROPERTIES
    COMPILE_FLAGS "-std=gnu++0x -Wall -DHAVE_EXPAT_CONFIG_H -I${CMAKE_BINARY_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/expat"
    # To optimize some more, you can change the CFLAGS through the call to
    # cmake or by using options here like
    #COMPILE_FLAGS "-std=gnu++0x -Wall -DHAVE_EXPAT_CONFIG_H I${CMAKE_BINARY_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/expat -march=core2 -O3 -pipe -fomit-frame-pointer -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -fvisibility-inlines-hidden"
)

SET_TARGET_PROPERTIES(wp2git PROPERTIES
    LINK_FLAGS "-Wl,-O1 -Wl,--enable-new-dtags -Wl,--sort-common -Wl,--as-needed"
)

target_link_libraries (wp2git ${Boost_LIBRARIES})
