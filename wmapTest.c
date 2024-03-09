#include <assert.h>
#include "your_header_file.h"

void test_wmap() {
  // Initialize a process
  struct proc *p = initproc();

  // Test with valid parameters
  uint addr = 0;
  int length = 100;
  int flags = MAP_ANONYMOUS | MAP_PRIVATE;
  int fd = -1; // No file descriptor for anonymous mapping
  assert(wmap(addr, length, flags, fd) == addr);

  // Test with invalid parameters (length greater than process size)
  length = p->sz + 1;
  assert(wmap(addr, length, flags, fd) == -1);

  // Test with valid parameters and MAP_SHARED flag
  length = 100;
  flags = MAP_ANONYMOUS | MAP_SHARED;
  assert(wmap(addr, length, flags, fd) == addr);

  // Test with valid parameters and MAP_FIXED flag
  flags = MAP_ANONYMOUS | MAP_FIXED;
  assert(wmap(addr, length, flags, fd) == addr);

  // Clean up
  destroyproc(p);
}

int main() {
  test_wmap();
  printf("All tests passed!\n");
  return 0;
}