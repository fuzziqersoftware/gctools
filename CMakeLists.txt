cmake_minimum_required(VERSION 3.27)



# Project setup

project(gctools)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -Wno-strict-aliasing)
endif()

find_path(SAMPLERATE_INCLUDE_DIR NAMES samplerate.h)
find_library(SAMPLERATE_LIBRARY NAMES samplerate)
find_package(phosg REQUIRED)
find_package(phosg-audio REQUIRED)



# Executable definitions

set(SimpleExecutableNames gcmasm gcmdump gvmdump rcfdump)
set(AllExecutableNames ${SimpleExecutableNames} smsdumpbanks smssynth modsynth)

foreach(ExecutableName IN ITEMS ${SimpleExecutableNames})
  add_executable(${ExecutableName} src/${ExecutableName}.cc)
  target_link_libraries(${ExecutableName} phosg::phosg)
endforeach()

add_executable(smsdumpbanks src/sms/smsdumpbanks.cc src/sms/aaf.cc src/sms/afc.cc src/sms/instrument.cc)
target_include_directories(smsdumpbanks PUBLIC ${OPENAL_INCLUDE_DIR})
target_link_libraries(smsdumpbanks phosg-audio::phosg-audio ${OPENAL_LIBRARY})

add_executable(smssynth src/sms/smssynth.cc src/sms/SampleCache.cc src/sms/aaf.cc src/sms/afc.cc src/sms/instrument.cc)
target_include_directories(smssynth PUBLIC ${OPENAL_INCLUDE_DIR} ${SAMPLERATE_INCLUDE_DIR})
target_link_libraries(smssynth phosg-audio::phosg-audio ${SAMPLERATE_LIBRARY} ${OPENAL_LIBRARY})

add_executable(modsynth src/sms/modsynth.cc src/sms/SampleCache.cc)
target_include_directories(modsynth PUBLIC ${OPENAL_INCLUDE_DIR} ${SAMPLERATE_INCLUDE_DIR})
target_link_libraries(modsynth phosg-audio::phosg-audio ${SAMPLERATE_LIBRARY} ${OPENAL_LIBRARY})



# Installation configuration

foreach(ExecutableName IN ITEMS ${AllExecutableNames})
  install(TARGETS ${ExecutableName} DESTINATION bin)
endforeach()
