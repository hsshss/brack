#include "util/main_thread.h"

namespace brack {

int runWithMainLoop(const std::function<int()>& body) { return body(); }

}  // namespace brack
