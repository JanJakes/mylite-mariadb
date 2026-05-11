# Copyright (c) 2026, MyLite contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

MYSQL_ADD_PLUGIN(mylite ha_mylite.cc
  STORAGE_ENGINE
  MANDATORY
  RECOMPILE_FOR_EMBEDDED
)
