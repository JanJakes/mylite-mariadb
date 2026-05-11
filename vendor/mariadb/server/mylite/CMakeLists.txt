# Copyright (c) 2026, MyLite contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_SOURCE_DIR}/libmysqld
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_BINARY_DIR}/sql
  ${PCRE_INCLUDE_DIRS}
)

ADD_DEFINITIONS(-DEMBEDDED_LIBRARY -UMYSQL_CLIENT)

MYSQL_ADD_EXECUTABLE(mylite-embedded-bootstrap-smoke
  bootstrap_smoke.cc
  COMPONENT Test
)
TARGET_LINK_LIBRARIES(mylite-embedded-bootstrap-smoke mysqlserver tpool)

IF(UNIX)
  SET_TARGET_PROPERTIES(mylite-embedded-bootstrap-smoke PROPERTIES
    ENABLE_EXPORTS TRUE)
ENDIF()
