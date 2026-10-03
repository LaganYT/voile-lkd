#include <dlfcn.h>
#include <stdio.h>
int main(void){
  const char *paths[] = {
    "/System/Library/Frameworks/D3DMetal.framework/Versions/A/D3DMetal",
    "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
    "/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio",
    0 };
  for (int i=0; paths[i]; i++){
    void *h = dlopen(paths[i], RTLD_LAZY|RTLD_LOCAL);
    printf("%s -> %s\n", paths[i], h ? "OK" : dlerror());
  }
  return 0;
}
