#include "../RuntimeCyclePolicy.h"
#include <cassert>
int main() {
  const char good[] = "benesse,a04br3\0mediatek,mt8167";
  const char bad[] = "benesse,a04br4\0mediatek,mt8167";
  assert(armcpu_runtime_is_a04br3(good,sizeof(good)));
  assert(!armcpu_runtime_is_a04br3(bad,sizeof(bad)));
  assert(!armcpu_runtime_is_a04br3(nullptr,0));
  assert(!armcpu_runtime_is_a04br3(good,4));
}
