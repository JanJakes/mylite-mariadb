# Copyright (c) 2006, 2014, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include 
${SSL_INCLUDE_DIRS})
ADD_DEFINITIONS(${SSL_DEFINES})

OPTION(MYLITE_WITH_VIO_TLS "Build VIO TLS transport" ON)
IF(MYLITE_WITH_VIO_TLS)
  ADD_DEFINITIONS(-DMYLITE_WITH_VIO_TLS=1)
  SET(MYLITE_VIO_TLS_SOURCES viossl.c viosslfactories.c)
  SET(MYLITE_VIO_TLS_LIBRARIES ${SSL_LIBRARIES})
ELSE()
  ADD_DEFINITIONS(-DMYLITE_WITH_VIO_TLS=0)
  SET(MYLITE_VIO_TLS_SOURCES mylite_viossl_disabled.c)
  SET(MYLITE_VIO_TLS_LIBRARIES)
ENDIF()

SET(VIO_SOURCES vio.c viosocket.c viopipe.c ${MYLITE_VIO_TLS_SOURCES})
ADD_CONVENIENCE_LIBRARY(vio ${VIO_SOURCES})
TARGET_LINK_LIBRARIES(vio ${LIBSOCKET})
TARGET_LINK_LIBRARIES(vio ${MYLITE_VIO_TLS_LIBRARIES})
