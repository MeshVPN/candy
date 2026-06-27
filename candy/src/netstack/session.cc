// SPDX-License-Identifier: MIT
#include "netstack/session.h"

namespace candy {

Session::Session(NetStack *stack) : stack(stack), closing(false) {}

} // namespace candy
