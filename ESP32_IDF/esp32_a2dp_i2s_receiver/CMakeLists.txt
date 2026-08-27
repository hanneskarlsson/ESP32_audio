cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_a2dp_i2s_receiver)

# ESP-IDF 5.5 changed the A2DP sink interface from the decoded-PCM callback
# used by this project to an undecoded audio-buffer interface. Stop with a
# clear message instead of producing confusing compiler errors.
if(IDF_VERSION_MAJOR LESS 5 OR
   IDF_VERSION_MAJOR GREATER 5 OR
   IDF_VERSION_MINOR GREATER_EQUAL 5)
    message(FATAL_ERROR
        "This decoded-PCM project requires ESP-IDF 5.0 through 5.4. "
        "ESP-IDF 5.4.x is recommended.")
endif()
