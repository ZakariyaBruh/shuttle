cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})
if (NOT ${ENVSDK} STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
		COMMAND head -n 1
		COMMAND cut -c9-
		OUTPUT_VARIABLE SDK
		OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK path not found; set PLAYDATE_SDK_PATH")
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(PLAYDATE_GAME_NAME airwave)
set(PLAYDATE_GAME_DEVICE airwave_DEVICE)
project(${PLAYDATE_GAME_NAME} C ASM)

set(SRC src/main.c src/ui.c src/net.c src/parse.c)
if (TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${SRC})
	target_link_options(${PLAYDATE_GAME_DEVICE} PRIVATE --specs=nosys.specs)   # stub newlib syscalls (_kill, _getpid, ...)
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${SRC})
endif()

include(${SDK}/C_API/buildsupport/playdate_game.cmake)
