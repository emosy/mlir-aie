//===- AIENetlistAnalysis.h -------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2019 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_AIE_LOCKANALYSIS_H
#define MLIR_AIE_LOCKANALYSIS_H

#include "aie/Dialect/AIE/IR/AIEDialect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/TypeSupport.h"
#include "mlir/IR/Types.h"
#include "mlir/Interfaces/FunctionImplementation.h"
#include "llvm/ADT/StringSwitch.h"

#include <map>

using namespace mlir;

namespace xilinx::AIE {

class NetlistAnalysis {
  DeviceOp &device;
  DenseMap<TileID, Operation *> &tiles;
  DenseMap<Operation *, CoreOp> &cores;
  DenseMap<Operation *, MemOp> &mems;
  DenseMap<std::pair<Operation *, int>, LockOp> &locks;
  DenseMap<Operation *, SmallVector<BufferOp, 4>> &buffers;
  DenseMap<Operation *, SwitchboxOp> &switchboxes;
  DenseMap<Operation *, SmallVector<Operation *, 4>> bufferUsers;
  DenseMap<Operation *, SmallVector<Operation *, 4>> dma2BufMap;
  DenseMap<std::pair<Operation *, xilinx::AIE::DMAChannel>, Operation *> dmas;
  DenseMap<Operation *, SmallVector<Operation *, 4>> dmaConnections;
  DenseMap<Operation *, SmallVector<Operation *, 4>> dma2ConnectsMap;
  DenseMap<Operation *, Operation *> lockPairs;
  SmallVector<std::pair<Operation *, Operation *>, 4> lockChains;
  DenseMap<Operation *, SmallVector<Operation *, 4>> bufAcqLocks;

public:
  NetlistAnalysis(DeviceOp &d, DenseMap<TileID, Operation *> &tiles,
                  DenseMap<Operation *, CoreOp> &cores,
                  DenseMap<Operation *, MemOp> &mems,
                  DenseMap<std::pair<Operation *, int>, LockOp> &locks,
                  DenseMap<Operation *, SmallVector<BufferOp, 4>> &buffers,
                  DenseMap<Operation *, SwitchboxOp> &switchboxes)
      : device(d), tiles(tiles), cores(cores), mems(mems), locks(locks),
        buffers(buffers), switchboxes(switchboxes) {}

  void collectTiles(DenseMap<TileID, Operation *> &tiles);
  void collectCores(DenseMap<Operation *, CoreOp> &cores);
  void collectBuffers(DenseMap<Operation *, SmallVector<BufferOp, 4>> &buffers);

  auto getBufferUsers() const { return bufferUsers; }

  auto getDMA2BufMap() const { return dma2BufMap; }

  auto getDMAs() const { return dmas; }

  void collectDMAUsage();
  uint64_t getBufferBaseAddress(Operation *bufOp) const;

  SmallVector<Operation *, 4> getNextConnectOps(ConnectOp currentConnect) const;
  SmallVector<Operation *, 4> findDestConnectOps(ConnectOp source,
                                                 WireBundle destBundle) const;
  void dmaAnalysis();
};

} // namespace xilinx::AIE

#endif
