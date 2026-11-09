# Aligns functions at 64 bytes and loops at 32 bytes.
#
# Otherwise, where the compiler places a function or loop depends on all code before it, so that an unrelated
# change can move a hot loop across a boundary of the instruction fetch or the micro-op cache. That alone can
# change its speed by 10 % or more, which hides the effect of changes on performance. With fixed alignments the
# speed of the code no longer depends on such accidents, at the cost of about 3 % more code. Flags that the
# compiler does not support are left out (MSVC has neither).

include_guard(GLOBAL)

option(TERSE_ALIGN_CODE "Align functions and loops at fixed boundaries" ON)

if(TERSE_ALIGN_CODE)
  include(CheckCXXCompilerFlag)
  foreach(flag -falign-functions=64 -falign-loops=32)
    string(MAKE_C_IDENTIFIER "TERSE_COMPILER_SUPPORTS${flag}" supported)
    check_cxx_compiler_flag(${flag} ${supported})
    if(${supported})
      add_compile_options(${flag})
    endif()
  endforeach()
endif()
