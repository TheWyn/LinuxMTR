find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

foreach(test tst_reportbuilder tst_hoptablemodel tst_core_parsers tst_hopgraphwidget tst_hoppropertiesdialog tst_mainwindow)
    qt_add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE linmtr_lib Qt6::Test)
    if(CMAKE_CROSSCOMPILING OR NOT test STREQUAL tst_core_parsers)
        add_test(NAME ${test} COMMAND ${test})
    endif()
endforeach()

# The build runs tst_core_parsers to list its functions, which a cross build cannot do.
if(NOT CMAKE_CROSSCOMPILING)
    set(core_parsers_tests "${CMAKE_CURRENT_BINARY_DIR}/tst_core_parsers_tests.cmake")
    set(core_parsers_include "${CMAKE_CURRENT_BINARY_DIR}/tst_core_parsers_include.cmake")
    add_custom_command(TARGET tst_core_parsers POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -D "TEST_EXE=$<TARGET_FILE:tst_core_parsers>" -D "OUT=${core_parsers_tests}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/add_test_functions.cmake"
        VERBATIM)
    # Without the list (no link yet), ctest shows a failed entry, not an include error.
    file(WRITE "${core_parsers_include}"
        "if(EXISTS \"${core_parsers_tests}\")\n"
        "    include(\"${core_parsers_tests}\")\n"
        "else()\n"
        "    add_test(tst_core_parsers_NOT_BUILT tst_core_parsers_NOT_BUILT)\n"
        "endif()\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${core_parsers_include}")
endif()
