# Copyright (2025) Beijing Volcano Engine Technology Ltd.
# SPDX-License-Identifier: Apache License 2.0


file(GLOB SERVICE_SRCS_COMMON "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
file(GLOB SERVICE_SRCS_CONV "${CMAKE_CURRENT_LIST_DIR}/conv_service/src/*.c")
file(GLOB SERVICE_SRCS_FUNCTION_CALL "${CMAKE_CURRENT_LIST_DIR}/function_call_service/src/*.c")
set(SERVICE_SRCS ${SERVICE_SRCS_COMMON} ${SERVICE_SRCS_CONV} ${SERVICE_SRCS_FUNCTION_CALL})

set(SERVICE_INC "${CMAKE_CURRENT_LIST_DIR}/inc"
                "${CMAKE_CURRENT_LIST_DIR}/conv_service/inc"
                "${CMAKE_CURRENT_LIST_DIR}/function_call_service/inc")
