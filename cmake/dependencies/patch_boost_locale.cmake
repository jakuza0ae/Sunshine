# locale.hpp lives in include/boost, alongside the locale directory.
set(locale "${BOOST_SOURCE_DIR}/libs/locale/CMakeLists.txt")
file(READ "${locale}" contents)
string(REPLACE "/include/boost/locale\" PREFIX \"headers\"" "/include/boost\" PREFIX \"headers\"" fixed "${contents}")
if(NOT contents STREQUAL fixed)
    file(WRITE "${locale}" "${fixed}")
endif()
