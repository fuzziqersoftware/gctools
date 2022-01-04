cmake_minimum_required(VERSION 3.10)



# Project setup

project(gctools)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -Wno-strict-aliasing)
endif()

include_directories("/usr/local/include")
link_directories("/usr/local/lib")



# Library definitions

add_library(prs src/prs/PRSDataLog.cc src/prs/PRS.cc src/prs/Yay0.cc src/prs/Yaz0.cc)



# Executable definitions

find_package(OpenAL REQUIRED)

set(SimpleExecutableNames afsdump gcmdump gsldump gvmdump rcfdump)
set(AllExecutableNames ${SimpleExecutableNames} pae2gvm prsd smsdumpbanks smssynth modsynth)

foreach(ExecutableName IN ITEMS ${SimpleExecutableNames})
  add_executable(${ExecutableName} src/${ExecutableName}.cc)
  target_link_libraries(${ExecutableName} phosg)
endforeach()

add_executable(pae2gvm src/pae2gvm.cc)
target_link_libraries(pae2gvm phosg prs)

add_executable(prsd src/prs/main.cc)
target_link_libraries(prsd phosg prs)

add_executable(smsdumpbanks src/sms/smsdumpbanks.cc src/sms/aaf.cc src/sms/afc.cc src/sms/instrument.cc)
target_include_directories(smsdumpbanks PUBLIC ${OPENAL_INCLUDE_DIR})
target_link_libraries(smsdumpbanks phosg phosg-audio ${OPENAL_LIBRARY})

add_executable(smssynth src/sms/smssynth.cc src/sms/SampleCache.cc src/sms/aaf.cc src/sms/afc.cc src/sms/instrument.cc)
target_include_directories(smssynth PUBLIC ${OPENAL_INCLUDE_DIR})
target_link_libraries(smssynth phosg phosg-audio samplerate ${OPENAL_LIBRARY})

add_executable(modsynth src/sms/modsynth.cc src/sms/SampleCache.cc)
target_include_directories(modsynth PUBLIC ${OPENAL_INCLUDE_DIR})
target_link_libraries(modsynth phosg phosg-audio samplerate ${OPENAL_LIBRARY})



# Installation configuration

foreach(ExecutableName IN ITEMS ${AllExecutableNames})
  install(TARGETS ${ExecutableName} DESTINATION bin)
endforeach()

file(GLOB PRSHeaders ${CMAKE_SOURCE_DIR}/src/prs/*.hh)
install(TARGETS prs DESTINATION lib)
install(FILES ${PRSHeaders} DESTINATION include/prs)
