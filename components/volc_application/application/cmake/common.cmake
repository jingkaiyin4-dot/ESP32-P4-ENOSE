set(VOLC_FRAMEWORK_SRCS "${CMAKE_CURRENT_LIST_DIR}/../framework/src/aios.c"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 framework src file")

set(VOLC_FRAMEWORK_INCS "${CMAKE_CURRENT_LIST_DIR}/../framework/inc"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 framework include dir")

set(VOLC_PLATFORM_INCS "${CMAKE_CURRENT_LIST_DIR}/../platform/inc"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 platform include dir")

set(VOLC_SERVICE_INCS "${CMAKE_CURRENT_LIST_DIR}/../service/inc"
                       "${CMAKE_CURRENT_LIST_DIR}/../service/conv_service/inc"
                       "${CMAKE_CURRENT_LIST_DIR}/../service/function_call_service/inc"
                       "${CMAKE_CURRENT_LIST_DIR}/../service/local_logic_service/inc"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 service include dir")

set(VOLC_SERVICE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../service/src/volc_manager_service.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../service/conv_service/src/volc_conv_service.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../service/conv_service/src/volc_conv.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../service/function_call_service/src/volc_function_call_service.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../service/function_call_service/src/volc_local_function_list.c"
                     "${CMAKE_CURRENT_LIST_DIR}/../service/local_logic_service/src/local_logic_service.c"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 service src file")

set(VOLC_UTIL_INCS "${CMAKE_CURRENT_LIST_DIR}/../utils"
                CACHE INTERNAL "ConversationalAI-Embedded-Kit-2.0 util include dir")
