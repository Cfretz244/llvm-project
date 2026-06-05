#include <sanitizer/ubsan_interface.h>

__attribute__((weak, constructor)) void install_trap_loop_detection() {
  __ubsan_install_trap_loop_detection();
}
