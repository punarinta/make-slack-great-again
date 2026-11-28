# text: one binary, one ctest test per case group.
add_executable(text_tests text_tests.cpp)
target_link_libraries(text_tests PRIVATE msga_text ${MSGA_HB_TARGET})
foreach(t wrap ellipsis emoji_caret emoji_color emoji_paint emoji_vertical rtl cjk_fallback hit_roundtrip inline_box style_boundary
          word_select malformed_utf8 glyph_cache glyph_cache_evict ink_bounds ink_lean perf recolor measure_cache build_move
          caret_long_line underline_link selection_order optical_size)
    add_test(NAME text_${t} COMMAND text_tests ${t})
endforeach()
# COLRv1 painting on a font of its own (assets/fonts): the index sees only
# that font and caches it in the build tree, not over the user's cache.
set(_colr_home ${CMAKE_CURRENT_BINARY_DIR}/colrv1-home)
add_test(NAME text_colrv1 COMMAND text_tests colrv1)
set_tests_properties(text_colrv1 PROPERTIES ENVIRONMENT
    "MSGA_NEXT_FONT_DIRS=${MSGA_TEST_ASSETS}/fonts;HOME=${_colr_home};XDG_CACHE_HOME=${_colr_home};LOCALAPPDATA=${_colr_home}")
