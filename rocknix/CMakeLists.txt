
# SDL2 frontend for embedded Linux handhelds (ROCKNIX). Needs the CLI lib for
# the discover/wakeup subcommands and the ffmpeg decoder for video.

if(NOT CHIAKI_ENABLE_CLI)
	message(FATAL_ERROR "CHIAKI_ENABLE_ROCKNIX requires CHIAKI_ENABLE_CLI=ON")
endif()
if(NOT CHIAKI_ENABLE_FFMPEG_DECODER)
	message(FATAL_ERROR "CHIAKI_ENABLE_ROCKNIX requires CHIAKI_ENABLE_FFMPEG_DECODER=ON")
endif()

find_package(SDL2 MODULE REQUIRED)

add_executable(chiaki-rocknix
	src/rocknix.h
	src/main.c
	src/config.c
	src/regist.c
	src/scan.c
	src/stream.c)

# explicit m: libopus may be linked statically and needs libm at final link
target_link_libraries(chiaki-rocknix chiaki-cli-lib chiaki-lib SDL2::SDL2 FFMPEG::avcodec FFMPEG::avutil m)

set_target_properties(chiaki-rocknix PROPERTIES
	C_STANDARD 11
	OUTPUT_NAME chiaki)

install(TARGETS chiaki-rocknix)
