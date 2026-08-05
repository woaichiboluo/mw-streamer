if(NOT LIBRARY OR NOT NM)
  message(FATAL_ERROR "shared library ABI test is missing a tool or library")
endif()

execute_process(
  COMMAND "${NM}" -D --defined-only "${LIBRARY}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed: ${nm_error}")
endif()

string(REPLACE "\n" ";" symbol_lines "${symbols}")
foreach(line IN LISTS symbol_lines)
  if(line AND
     NOT line MATCHES " MW_STREAMER_[0-9.]+$" AND
     NOT line MATCHES " mw_[a-z0-9_]+(@@MW_STREAMER_[0-9.]+)?$")
    message(FATAL_ERROR "unexpected exported symbol: ${line}")
  endif()
endforeach()

foreach(required_symbol IN ITEMS
        mw_init
        mw_shutdown
        mw_streaming_create
        mw_streaming_reload
        mw_remux_create
        mw_file_create
        mw_file_reload)
  if(NOT symbols MATCHES " ${required_symbol}(@@MW_STREAMER_[0-9.]+)?")
    message(FATAL_ERROR "missing exported symbol: ${required_symbol}")
  endif()
endforeach()
