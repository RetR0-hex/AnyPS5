if(MINGW)
    foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
        string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    endforeach()
endif()

function(add_ngs2_library target testPrefix)
    set(ngs2Dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    add_library(${target} SHARED EXCLUDE_FROM_ALL
            ${ngs2Dir}/src/Atrac9.cpp
            ${ngs2Dir}/src/Custom.cpp
            ${ngs2Dir}/src/Rack.cpp
            ${ngs2Dir}/src/Render.cpp
            ${ngs2Dir}/src/System.cpp
            ${ngs2Dir}/src/Unimplemented.cpp
            ${ngs2Dir}/src/Voice.cpp
    )
    target_include_directories(${target} PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/LibAtrac9/C/src)
    target_link_libraries(${target} PRIVATE atrac9)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${target} PRIVATE LINKER:--exclude-libs,ALL)
    endif()
    set_target_properties(${target} PROPERTIES
            CXX_EXTENSIONS OFF
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
    )
    configure_windows_unwind(${target})

    if(BUILD_TESTING)
        foreach(ngs2Test IN ITEMS Render Atrac9 Filter Custom)
            string(TOLOWER ${ngs2Test} testName)
            set(testName ${testPrefix}_${testName})
            add_test_executable(${testName}_tests ${ngs2Dir}/tests/Ngs2${ngs2Test}.cpp)
            target_include_directories(${testName}_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/LibAtrac9/C/src)
            target_link_libraries(${testName}_tests PRIVATE ${target} atrac9 libkernel libc)
            configure_windows_unwind(${testName}_tests)
            add_test(NAME ${testName} COMMAND ${testName}_tests)
            if(WIN32)
                set_tests_properties(${testName} PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target}>")
            endif()
        endforeach()
    endif()
endfunction()
