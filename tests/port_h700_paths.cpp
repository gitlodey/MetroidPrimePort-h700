#include "port_paths.h"

#include <iostream>

// No SDL/window/GPU initialization. SDL supplies this physical executable's
// base path; every resolution below is the actual production path function.
int main() {
  std::cout << "user=" << PortPaths::UserFolder() << '\n';
  std::cout << "card=" << PortPaths::CardFolder() << '\n';
  std::cout << "portable=" << PortPaths::IsPortable() << '\n';
  std::cout << "migrations=" << PortPaths::MigrationLog().size() << '\n';
  return 0;
}
