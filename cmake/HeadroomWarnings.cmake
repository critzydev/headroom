# Warning flags for Headroom's own sources only. JUCE module sources are compiled into headroom_core
# too, so the flags are attached per source file rather than per target.
function(headroom_set_warnings)
    if(MSVC)
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "/W4")
    else()
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
    endif()
endfunction()
