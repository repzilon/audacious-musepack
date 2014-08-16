cmake_minimum_required(VERSION 2.4)

project(audacious_musepack C)

SET(CMAKE_VERBOSE_MAKEFILE TRUE)
SET(CMAKE_C_FLAGS "-Wall")
IF(NOT CMAKE_BUILD_TYPE)
  SET(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
ENDIF(NOT CMAKE_BUILD_TYPE)

FIND_PACKAGE(GTK2 REQUIRED gtk)

FIND_PATH(MPCDEC_INCLUDE_DIR mpc/mpc_types.h
	${audacious_musepack_SOURCE_DIR}/../libmpc/include
	/usr/local/include
	/usr/include
)

FIND_PATH(AUD_PLUGIN_DIR Input
	/usr/local/lib/audacious
	/usr/lib/audacious
)

FIND_LIBRARY(MPCDEC_LIBRARY NAMES mpcdec PATHS
	/usr/local/lib
	/usr/lib
)

include_directories(${GTK2_INCLUDE_DIRS} ${MPCDEC_INCLUDE_DIR} ${MOWGLI_INCLUDE_DIR})
link_directories(${GTK_LIBRARIES} ${MPCDEC_LIBRARY})

add_library(musepack MODULE libmpc)
target_link_libraries(musepack pthread mpcdec glib-2.0 gtk-x11-2.0)
SET_TARGET_PROPERTIES(musepack PROPERTIES PREFIX "")

install(TARGETS musepack LIBRARY DESTINATION "${AUD_PLUGIN_DIR}/Input/")
