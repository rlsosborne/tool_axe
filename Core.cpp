// Copyright (c) 2011, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "Core.h"
#include "SystemState.h"
#include "Node.h"
#include "JIT.h"
#include "Timer.h"
#include "Synchroniser.h"
#include "Lock.h"
#include "Chanend.h"
#include "ClockBlock.h"
#include <iostream>
#include <iomanip>
#include <sstream>


Core::Core(uint32_t RamSize, uint32_t RamBase) :
  thread(new Thread[NUM_THREADS]),
  sync(new Synchroniser[NUM_SYNCS]),
  lock(new Lock[NUM_LOCKS]),
  chanend(new Chanend[NUM_CHANENDS]),
  timer(new Timer[NUM_TIMERS]),
  clkBlk(new ClockBlock[NUM_CLKBLKS]),
  port(new Port*[33]),
  portNum(new unsigned[33]),
  resource(new Resource**[LAST_STD_RES_TYPE + 1]),
  resourceNum(new unsigned[LAST_STD_RES_TYPE + 1]),
  memory(new uint32_t[RamSize >> 2]),
  coreNumber(0),
  parent(0),
  opcode(new OPCODE_TYPE[(RamSize >> 1) + ILLEGAL_PC_THREAD_ADDR_OFFSET]),
  operands(new Operands[RamSize >> 1]),
  invalidationInfo(new unsigned char[RamSize >> 1]),
  ram_size(RamSize),
  ram_base(RamBase),
  syscallAddress(~0),
  exceptionAddress(~0)
{
  resource[RES_TYPE_PORT] = 0;
  resourceNum[RES_TYPE_PORT] = 0;
  
  resource[RES_TYPE_TIMER] = new Resource*[NUM_TIMERS];
  for (unsigned i = 0; i < NUM_TIMERS; i++) {
    resource[RES_TYPE_TIMER][i] = &timer[i];
  }
  resourceNum[RES_TYPE_TIMER] = NUM_TIMERS;
  
  resource[RES_TYPE_CHANEND] = new Resource*[NUM_CHANENDS];
  for (unsigned i = 0; i < NUM_CHANENDS; i++) {
    resource[RES_TYPE_CHANEND][i] = &chanend[i];
  }
  resourceNum[RES_TYPE_CHANEND] = NUM_CHANENDS;
  
  resource[RES_TYPE_SYNC] = new Resource*[NUM_SYNCS];
  for (unsigned i = 0; i < NUM_SYNCS; i++) {
    resource[RES_TYPE_SYNC][i] = &sync[i];
  }
  resourceNum[RES_TYPE_SYNC] = NUM_SYNCS;
  
  resource[RES_TYPE_THREAD] = new Resource*[NUM_THREADS];
  for (unsigned i = 0; i < NUM_THREADS; i++) {
    thread[i].setParent(*this);
    resource[RES_TYPE_THREAD][i] = &thread[i];
  }
  resourceNum[RES_TYPE_THREAD] = NUM_THREADS;
  
  resource[RES_TYPE_LOCK] = new Resource*[NUM_LOCKS];
  for (unsigned i = 0; i < NUM_LOCKS; i++) {
    resource[RES_TYPE_LOCK][i] = &lock[i];
  }
  resourceNum[RES_TYPE_LOCK] = NUM_LOCKS;
  
  resource[RES_TYPE_CLKBLK] = new Resource*[NUM_CLKBLKS];
  for (unsigned i = 0; i < RES_TYPE_CLKBLK; i++) {
    resource[RES_TYPE_CLKBLK][i] = &clkBlk[i];
  }
  resourceNum[RES_TYPE_CLKBLK] = NUM_CLKBLKS;
  
  for (int i = RES_TYPE_TIMER; i <= LAST_STD_RES_TYPE; i++) {
    for (unsigned j = 0; j < resourceNum[i]; j++) {
      resource[i][j]->setNum(j);
    }
  }
  
  std::memset(port, 0, sizeof(port[0]) * 33);
  std::memset(portNum, 0, sizeof(portNum[0]) * 33);
  const unsigned portSpec[][2] = {
    {1, NUM_1BIT_PORTS},
    {4, NUM_4BIT_PORTS},
    {8, NUM_8BIT_PORTS},
    {16, NUM_16BIT_PORTS},
    {32, NUM_32BIT_PORTS},
  };
  for (unsigned i = 0; i < ARRAY_SIZE(portSpec); i++) {
    unsigned width = portSpec[i][0];
    unsigned num = portSpec[i][1];
    port[width] = new Port[num];
    for (unsigned j = 0; j < num; j++) {
      port[width][j].setNum(j);
      port[width][j].setWidth(width);
      port[width][j].setClkInitial(&clkBlk[0]);
    }
    portNum[width] = num;
  }
  thread[0].alloc(0);
  
  // Initialise instruction cache.
  for (unsigned i = 0; i < (RamSize >> 1) + ILLEGAL_PC_THREAD_ADDR_OFFSET;
       ++i) {
#ifdef DIRECT_THREADED
    opcode[i] = 0;
#else
    opcode[i] = INITIALIZE;
#endif
  }
  for (unsigned i = 0; i != (RamSize >> 1); ++i) {
    invalidationInfo[i] = INVALIDATE_NONE;
  }
}

Core::~Core() {
  delete[] opcode;
  delete[] operands;
  delete[] invalidationInfo;
  delete[] thread;
  delete[] sync;
  delete[] lock;
  delete[] chanend;
  delete[] timer;
  delete[] resource;
  delete[] resourceNum;
  delete[] memory;
}

bool Core::allocatable[LAST_STD_RES_TYPE + 1] = {
  false, // RES_TYPE_PORT
  true, // RES_TYPE_TIMER
  true, // RES_TYPE_CHANEND
  true, // RES_TYPE_SYNC
  true, // RES_TYPE_THREAD
  true, // RES_TYPE_LOCK
  false, // RES_TYPE_CLKBLK
};

void Core::dumpPaused() const
{
  for (unsigned i = 0; i < NUM_THREADS; i++) {
    Thread *t = static_cast<Thread*>(resource[RES_TYPE_THREAD][i]);
    if (!t->isInUse())
      continue;
    std::cout << "Thread " << std::dec << i;
    Thread &ts = *t;
    if (Resource *res = ts.pausedOn) {
      std::cout << " paused on ";
      std::cout << Resource::getResourceName(res->getType());
      std::cout << " 0x" << std::hex << res->getID();
    } else if (ts.eeble()) {
      std::cout << " waiting for events";
      if (ts.ieble())
        std::cout << " or interrupts";
    } else if (ts.ieble()) {
      std::cout << " waiting for interrupts";
    } else {
      std::cout << " paused on unknown";
    }
    std::cout << "\n";
  }
}

const Port *Core::getPortByID(ResourceID ID) const
{
  assert(ID.type() == RES_TYPE_PORT);
  unsigned width = ID.width();
  if (width > 32)
    return 0;
  unsigned num = ID.num();
  if (num >= portNum[width])
    return 0;
  return &port[width][num];
}

const Resource *Core::getResourceByID(ResourceID ID) const
{
  ResourceType type = ID.type();
  if (type > LAST_STD_RES_TYPE) {
    return 0;
  }
  if (type == RES_TYPE_PORT) {
    return getPortByID(ID);
  }
  unsigned num = ID.num();
  if (num >= resourceNum[type]) {
    return 0;
  }
  return resource[type][num];
}

Resource *Core::getResourceByID(ResourceID ID)
{
  return const_cast<Resource *>(
    static_cast<const Core *>(this)->getResourceByID(ID)
  );
}

bool Core::setSyscallAddress(uint32_t value)
{
  uint32_t addr = physicalAddress(value) >> 1;
  if (addr >= (ram_size << 1))
    return false;
  syscallAddress = addr;
  return true;
}

bool Core::setExceptionAddress(uint32_t value)
{
  uint32_t addr = physicalAddress(value) >> 1;
  if (addr >= (ram_size << 1))
    return false;
  exceptionAddress = addr;
  return true;
}

void Core::
initCache(OPCODE_TYPE decode, OPCODE_TYPE illegalPC,
          OPCODE_TYPE illegalPCThread, OPCODE_TYPE syscall,
          OPCODE_TYPE exception, OPCODE_TYPE jitFunction)
{
  const uint32_t ramSizeShorts = ram_size >> 1;
  // Initialise instruction cache.
  for (unsigned i = 0; i < ramSizeShorts; i++) {
    opcode[i] = decode;
  }
  opcode[ramSizeShorts] = illegalPC;
  opcode[getIllegalPCThreadAddr()] = illegalPCThread;
  if (syscallAddress < ramSizeShorts)
    opcode[syscallAddress] = syscall;
  if (exceptionAddress < ramSizeShorts)
    opcode[exceptionAddress] = exception;
  decodeOpcode = decode;
  jitFunctionOpcode = jitFunction;
}

bool Core::getLocalChanendDest(ResourceID ID, ChanEndpoint *&result)
{
  assert(ID.isChanendOrConfig());
  if (ID.isConfig()) {
    switch (ID.num()) {
      case RES_CONFIG_SSCTRL:
        if (parent->hasMatchingNodeID(ID)) {
          result = parent->getSSwitch();
          return true;
        }
        break;
      case RES_CONFIG_PSCTRL:
        // TODO.
        assert(0);
        result = 0;
        return true;
    }
  } else {
    if (ID.node() == getCoreID()) {
      Resource *res = getResourceByID(ID);
      if (res) {
        assert(res->getType() == RES_TYPE_CHANEND);
        result = static_cast<Chanend*>(res);
      } else {
        result = 0;
      }
      return true;
    }
  }
  return false;
}

ChanEndpoint *Core::getChanendDest(ResourceID ID)
{
  if (!ID.isChanendOrConfig())
    return 0;
  ChanEndpoint *result;
  // Try to lookup locally first.
  if (getLocalChanendDest(ID, result))
    return result;
  return parent->getParent()->getChanendDest(ID);
}

void Core::updateIDs()
{
  unsigned coreID = getCoreID();
  for (unsigned i = 0; i < NUM_CHANENDS; i++) {
    resource[RES_TYPE_CHANEND][i]->setNode(coreID);
  }    
}

uint32_t Core::getCoreID() const
{
  return getParent()->getCoreID(coreNumber);
}

std::string Core::getCoreName() const
{
  if (!codeReference.empty())
    return codeReference;
  std::ostringstream buf;
  buf << 'c' << getCoreID();
  return buf.str();
}

void Core::invalidateWordSlowPath(uint32_t shiftedAddress)
{
  if (invalidationInfo[shiftedAddress + 1] == INVALIDATE_NONE) {
    invalidateSlowPath(shiftedAddress);
    return;
  }
  invalidationInfo[shiftedAddress + 1] = INVALIDATE_CURRENT_AND_PREVIOUS;
  invalidateSlowPath(shiftedAddress + 1);
}

void Core::invalidateSlowPath(uint32_t shiftedAddress)
{
  unsigned char info;
  do {
    info = invalidationInfo[shiftedAddress];
    if (opcode[shiftedAddress] == jitFunctionOpcode) {
      JIT::markUnreachable(operands[shiftedAddress].func);
    }
    opcode[shiftedAddress] = decodeOpcode;
    invalidationInfo[shiftedAddress--] = INVALIDATE_NONE;
  } while (info == INVALIDATE_CURRENT_AND_PREVIOUS);
}
