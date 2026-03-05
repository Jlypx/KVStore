#include "kvstore/smoke.h"

int main() {
  return kvstore::SmokeCheck() ? 0 : 1;
}
