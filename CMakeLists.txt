cmake_minimum_required(VERSION 3.20)
project(txsv_poc C ASM)

# Selects: 0 none, 1 native (TraceX), 2 SystemView, 3 both
set(TXTRACE_BACKEND 2 CACHE STRING "ThreadX trace backend")
set_property(CACHE TXTRACE_BACKEND PROPERTY STRINGS 0 1 2 3)

set(THREADX_DIR  "" CACHE PATH "Unmodified upstream eclipse-threadx/threadx checkout")
set(SYSVIEW_DIR  "" CACHE PATH "SEGGERMicro/SystemView checkout")
set(RTT_DIR      "" CACHE PATH "SEGGERMicro/RTT checkout")

# ThreadX picks up our tx_user.h through its own TX_USER_FILE hook.
set(TX_USER_FILE ${CMAKE_CURRENT_LIST_DIR}/txsv/tx_user.h)
add_subdirectory(${THREADX_DIR} threadx)
target_compile_definitions(threadx PUBLIC TXTRACE_BACKEND=${TXTRACE_BACKEND})

if(TXTRACE_BACKEND GREATER_EQUAL 2)
  add_library(txsv STATIC
    txsv/txsv_adapter.c
    txsv/txsv_native_insert.c
    ${SYSVIEW_DIR}/SYSVIEW/SEGGER_SYSVIEW.c
    ${RTT_DIR}/RTT/SEGGER_RTT.c
    ${RTT_DIR}/RTT/SEGGER_RTT_ASM_ARMv7M.S)
  target_include_directories(txsv BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/txsv/svconf)
  target_include_directories(txsv PUBLIC
    ${SYSVIEW_DIR}/Config ${SYSVIEW_DIR}/SEGGER ${SYSVIEW_DIR}/SYSVIEW
    ${RTT_DIR}/Config ${RTT_DIR}/RTT)
  target_link_libraries(txsv PUBLIC threadx)
  # The adapter supplies symbols the kernel calls; link it into the kernel's users.
  target_link_libraries(threadx INTERFACE txsv)
endif()

set(TXPOC_TARGET qemu CACHE STRING "qemu (semihosting, post-mortem) or renode (live UART streaming)")
set_property(CACHE TXPOC_TARGET PROPERTY STRINGS qemu renode)
if(TXPOC_TARGET STREQUAL "qemu")
  target_compile_definitions(threadx PUBLIC TXPOC_QEMU)
else()
  target_compile_definitions(threadx PUBLIC TXPOC_RENODE)   # timestamps from DWT CYCCNT
endif()
if(TARGET txsv)
  option(TXSV_STATS "PoC: count adapter hook calls and dump SystemView buffer" OFF)
  if(TXSV_STATS)
    target_compile_definitions(txsv PUBLIC TXSV_STATS TXSV_AUTOSTART SEGGER_SYSVIEW_POST_MORTEM_MODE=1 SEGGER_SYSVIEW_RTT_BUFFER_SIZE=32768)
  endif()
  if(TXPOC_TARGET STREQUAL "renode")
    # rtt:  firmware only writes the RTT buffer, as with J-Link; Renode's
    #       renode/sv_rtt_probe.py reads it from memory like the probe does.
    # uart: target-side pump thread speaking SystemView's IP/UART Recorder protocols.
    set(TXSV_COMM rtt CACHE STRING "Renode SystemView transport: rtt (J-Link-like) or uart")
    set_property(CACHE TXSV_COMM PROPERTY STRINGS rtt uart)
    target_sources(txsv PRIVATE renode/board_pl011.c)
    target_compile_definitions(txsv PUBLIC SEGGER_SYSVIEW_RTT_BUFFER_SIZE=8192)
    if(TXSV_COMM STREQUAL "uart")
      target_sources(txsv PRIVATE txsv/txsv_comm_uart.c ${SYSVIEW_DIR}/Sample/COMM/SEGGER_SYSVIEW_REC.c)
      target_include_directories(txsv PUBLIC ${SYSVIEW_DIR}/Sample/COMM)
      target_compile_definitions(txsv PUBLIC TXSV_COMM_UART)
    endif()
  endif()
endif()

# Upstream example startup, vector table and linker script, unmodified.
set(EX ${THREADX_DIR}/ports/cortex_m4/gnu/example_build)
add_executable(demo app/demo.c ${EX}/cortexm4_vectors.S ${EX}/cortexm4_crt0.S ${EX}/tx_initialize_low_level.S)
target_link_libraries(demo PRIVATE threadx)
target_link_options(demo PRIVATE -T${EX}/sample_threadx.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=demo.map)

# Pitfall found in PoC: the example startup .S does not include tx_user.h, so the
# SysTick ISR hooks only compile in when the flag is passed on the command line.
if(TXTRACE_BACKEND GREATER_EQUAL 2)
  set_source_files_properties(${EX}/tx_initialize_low_level.S PROPERTIES COMPILE_DEFINITIONS TX_ENABLE_EXECUTION_CHANGE_NOTIFY)
endif()
