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

SET(VIO_SOURCES vio.c viosocket.c viopipe.c)
IF(NOT MYLITE_DISABLE_VIO_SSL)
  LIST(APPEND VIO_SOURCES viossl.c viosslfactories.c)
ELSE()
  LIST(APPEND VIO_SOURCES mylite_vio_ssl_stub.c)
ENDIF()
ADD_CONVENIENCE_LIBRARY(vio ${VIO_SOURCES})
TARGET_LINK_LIBRARIES(vio ${LIBSOCKET})
IF(NOT MYLITE_DISABLE_VIO_SSL)
  TARGET_LINK_LIBRARIES(vio ${SSL_LIBRARIES})
ENDIF()
