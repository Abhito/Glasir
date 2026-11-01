# SPDX-License-Identifier: MIT
# glasir_audio.dll: the card's sound in mpv's process (an mpv C plugin).

add_library(glasir_audio SHARED glasir_audio.cpp)
glasir_component(glasir_audio)
target_link_libraries(glasir_audio PRIVATE glasir_mpv_client ole32 avrt)
install(TARGETS glasir_audio RUNTIME DESTINATION engine/portable_config/scripts)
