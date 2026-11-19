#include <l4/re/env>
#include <l4/turingos/device_supervisor.h>
#include <cstdio>
#include <cstring>
int main(int argc, char **argv)
{
  if (argc != 3 || strcmp(argv[1],"restart")) { puts("usage: svc restart DEVICE"); return 2; }
  auto svc = L4Re::Env::env()->get_cap<Device_supervisor>("supervisor");
  if (!svc.is_valid()) return 1;
  long rc = svc->restart(L4::Ipc::Array<char const>(strlen(argv[2]),argv[2]));
  if (rc) fprintf(stderr,"svc: restart failed (%ld)\n",rc);
  return rc != 0;
}
