find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

foreach(test tst_reportbuilder tst_hoptablemodel tst_core_parsers tst_hopgraphwidget tst_hoppropertiesdialog)
    qt_add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE linmtr_lib Qt6::Test)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
