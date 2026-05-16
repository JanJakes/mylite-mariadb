# Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
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

SET(CSV_SOURCES  ha_tina.cc ha_tina.h transparent_file.cc transparent_file.h)
IF(NOT DEFINED MYLITE_WITH_CSV_STORAGE_ENGINE)
  SET(MYLITE_WITH_CSV_STORAGE_ENGINE ON CACHE BOOL "Build CSV storage engine")
ENDIF()

IF(MYLITE_WITH_CSV_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(csv ${CSV_SOURCES} STORAGE_ENGINE MANDATORY)
ELSE()
  MYSQL_ADD_PLUGIN(csv ${CSV_SOURCES} STORAGE_ENGINE DISABLED)
ENDIF()
