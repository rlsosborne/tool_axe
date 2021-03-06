// Copyright (c) 2012, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "PortHandleClockProxy.h"
#include "Signal.h"
#include "RunnableQueue.h"

using namespace axe;

PortHandleClockProxy::
PortHandleClockProxy(RunnableQueue &s, PortInterface &p) :
  PortHandleClockMixin<PortHandleClockProxy>(s),
  next(p)
{
}
