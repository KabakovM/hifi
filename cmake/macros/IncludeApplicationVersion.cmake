# 
#  IncludeApplicationVersion.cmake
#  cmake/macros
#
#  Created by Leonardo Murillo on 07/14/2015.
#  Copyright 2015 High Fidelity, Inc.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
# 

macro(INCLUDE_APPLICATION_VERSION)
  #
  # We are relying on Jenkins defined environment variables to determine the origin of this build
  # and will only package if this is a PR or Release build
  if (DEFINED ENV{JOB_ID})
    set (DEPLOY_PACKAGE 1)
    set (BUILD_SEQ $ENV{JOB_ID})
  elseif (DEFINED ENV{ghprbPullId})
    set (DEPLOY_PACKAGE 1)
    set (BUILD_SEQ "PR-$ENV{ghprbPullId}")
  else ()
    set(BUILD_SEQ "dev")
  endif ()
  configure_file("${MACRO_DIR}/ApplicationVersion.h.in" "${PROJECT_BINARY_DIR}/includes/ApplicationVersion.h")
  include_directories("${PROJECT_BINARY_DIR}/includes")
endmacro(INCLUDE_APPLICATION_VERSION)
